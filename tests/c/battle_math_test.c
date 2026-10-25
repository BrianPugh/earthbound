/*
 * Battle-math unit test: replays cases recorded from the ORIGINAL ROM.
 *
 * tests/c/battle_oracle.py runs each asm routine (SUCCESS_500, DETERMINE_DODGE,
 * the damage variances, INFLICT_STATUS_BATTLE, LEVEL_UP_CHAR, ...) in the
 * playtest emulator with chosen inputs and records the return value, the RNG
 * state and the battler / party-character bytes after the call
 * (tests/c/battle_cases.txt). This program loads the same inputs into the
 * port's globals, calls the C port of the routine and compares every output.
 * struct battler and char_struct have the asm layouts (ASSERT_STRUCT_SIZE),
 * so the recorded blobs are copied in and out unchanged. Scene cases also
 * carry named globals ("var", see var_defs), the game state and event flags,
 * and run the KO / apply / revive / battle-scene children they push.
 *
 * Usage: battle_math_test CASES_FILE [-v]
 * Exit status: 0 when every case matches, 1 otherwise.
 */
#include "core/math.h"
#include "data/assets.h"
#include "core/mode_stack.h"
#include "game/battle.h"
#include "game/battle_internal.h"
#include "game/game_state.h"
#include "game/inventory.h"
#include "game/overworld_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BLOBS 32
#define MAX_VARS 32
#define LINE_MAX_LEN 4096
#define VAR_MAX_LEN (EVENT_FLAG_COUNT / 8)

typedef struct {
    uint16_t index;
    uint8_t bytes[sizeof(CharStruct) > sizeof(Battler) ? sizeof(CharStruct) : sizeof(Battler)];
} Blob;

/* A named global (a "var NAME HEX" line): the asm WRAM symbol and the port's
 * variable holding the same value, compared byte for byte (little-endian). */
typedef struct {
    const char *name;
    void *ptr;
    size_t size;
} VarDef;

static const VarDef var_defs[] = {
    {"BATTLER_TARGET_FLAGS", &bt.battler_target_flags, 4},
    {"GIYGAS_PHASE", &bt.giygas_phase, 2},
    {"CURRENT_BATTLE_GROUP", &bt.current_battle_group, 2},
    {"MIRROR_ENEMY", &bt.mirror_enemy, 2},
    {"MIRROR_TURN_TIMER", &bt.mirror_turn_timer, 2},
    {"SPECIAL_DEFEAT", &bt.special_defeat, 2},
    {"ENEMY_PERFORMING_FINAL_ATTACK", &bt.enemy_performing_final_attack, 2},
    {"SKIP_DEATH_TEXT_AND_CLEANUP", &bt.skip_death_text_and_cleanup, 2},
    {"SHIELD_HAS_NULLIFIED_DAMAGE", &bt.shield_has_nullified_damage, 2},
    {"DAMAGE_IS_REFLECTED", &bt.damage_is_reflected, 2},
    {"IS_SMAAAAASH_ATTACK", &bt.is_smaaaash_attack, 2},
    {"ITEM_DROPPED", &bt.item_dropped, 2},
    {"BATTLE_MODE_FLAG", &bt.battle_mode_flag, 2},
    {"BATTLE_EXP_SCRATCH", &bt.battle_exp_scratch, 4},
    {"BATTLE_MONEY_SCRATCH", &bt.battle_money_scratch, 2},
    {"FRONT_ROW_BATTLERS", bt.front_row_battlers, 8},
    {"BACK_ROW_BATTLERS", bt.back_row_battlers, 8},
    {"NUM_BATTLERS_IN_FRONT_ROW", &bt.num_battlers_in_front_row, 2},
    {"NUM_BATTLERS_IN_BACK_ROW", &bt.num_battlers_in_back_row, 2},
    {"ENEMIES_IN_BATTLE", &bt.enemies_in_battle, 2},
    {"EVENT_FLAGS", event_flags, EVENT_FLAG_COUNT / 8},
};

typedef struct {
    const VarDef *def;
    uint8_t bytes[VAR_MAX_LEN];
} Var;

typedef struct {
    char name[64];
    uint16_t a, x, y;
    uint32_t func;
    uint16_t rng_a, rng_b;
    int atk, tgt;
    Blob battlers[MAX_BLOBS], chars[MAX_BLOBS];
    int n_battlers, n_chars;
    uint16_t flags[MAX_BLOBS][2];
    int n_flags;
    Var vars[MAX_VARS], vars_out[MAX_VARS];
    int n_vars, n_vars_out;
    int has_gs;
    uint8_t gs[sizeof(GameState)], gs_out[sizeof(GameState)];
    uint16_t ret;
    int has_ret; /* the asm routine declares a return value */
    uint16_t frames;
    uint16_t rng_out_a, rng_out_b;
    Blob battlers_out[MAX_BLOBS], chars_out[MAX_BLOBS];
    int n_battlers_out, n_chars_out;
} Case;

static uint16_t battlers_table_addr; /* "battlers_table" header: WRAM address of BATTLERS_TABLE */
static int verbose;

static int parse_hex_blob(const char *hex, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1)
            return 0;
        out[i] = (uint8_t)v;
    }
    return hex[len * 2] == '\0' || hex[len * 2] == '\n';
}

/* "I OFF=VAL ...": the input blob I with the listed bytes changed. */
static int add_changes(Blob *arr, int *n, const Blob *inputs, int n_inputs, const char *rest, size_t len) {
    unsigned idx;
    int used = 0;
    if (*n >= MAX_BLOBS || sscanf(rest, "%x%n", &idx, &used) != 1)
        return 0;
    const Blob *in = NULL;
    for (int i = 0; i < n_inputs; i++)
        if (inputs[i].index == idx)
            in = &inputs[i];
    if (!in)
        return 0;
    Blob *out = &arr[*n];
    *out = *in;
    rest += used;
    unsigned off, val;
    while (sscanf(rest, " %x=%x%n", &off, &val, &used) == 2) {
        if (off >= len)
            return 0;
        out->bytes[off] = (uint8_t)val;
        rest += used;
    }
    (*n)++;
    return 1;
}

static int add_blob(Blob *arr, int *n, const char *rest, size_t len) {
    unsigned idx;
    char hex[LINE_MAX_LEN];
    if (*n >= MAX_BLOBS || sscanf(rest, "%x %4000s", &idx, hex) != 2)
        return 0;
    arr[*n].index = (uint16_t)idx;
    if (!parse_hex_blob(hex, arr[*n].bytes, len))
        return 0;
    (*n)++;
    return 1;
}

/* "NAME HEX": a named global's bytes. */
static int add_var(Var *arr, int *n, const char *rest) {
    char name[64], hex[2 * VAR_MAX_LEN + 1];
    if (*n >= MAX_VARS || sscanf(rest, "%63s %256s", name, hex) != 2)
        return 0;
    for (size_t i = 0; i < sizeof(var_defs) / sizeof(var_defs[0]); i++) {
        if (strcmp(var_defs[i].name, name))
            continue;
        arr[*n].def = &var_defs[i];
        if (strlen(hex) != var_defs[i].size * 2 || !parse_hex_blob(hex, arr[*n].bytes, var_defs[i].size))
            return 0;
        (*n)++;
        return 1;
    }
    fprintf(stderr, "unknown var %s\n", name);
    return 0;
}

/* "OFF=VAL ..." changes applied to a copy of `base`. */
static int apply_changes(uint8_t *out, const uint8_t *base, size_t len, const char *rest) {
    memcpy(out, base, len);
    unsigned off, val;
    int used;
    while (sscanf(rest, " %x=%x%n", &off, &val, &used) == 2) {
        if (off >= len)
            return 0;
        out[off] = (uint8_t)val;
        rest += used;
    }
    return 1;
}

/* Battler pointer from a WRAM address in a register parameter. */
static Battler *battler_at(uint16_t addr) {
    return battler_from_offset((uint16_t)(addr - battlers_table_addr));
}

/* Run a battle-calc / battle-action mode to completion on the real mode stack,
 * as the root loop would, with every other child (DISPLAY_TEXT, BATTLE_WAIT,
 * BATTLE_KO, ...) popped at once with 0: only the arithmetic, the RNG calls and
 * the battler writes are under test. Returns the mode's pop result. */
static int unexpected_child;
/* A final action showed a cutscene text (DISPLAY_BATTLE_CUTSCENE_TEXT): the ROM
 * reloaded the battle scene and rendered the battle sprites for some frames,
 * which counts the damage-blink timers down; the C test pops that child. */
static int scene_reloaded;

static int32_t run_mode(GameMode mode, const ModeState *init) {
    uint8_t base = g_mode_stack.depth;
    int32_t result = 0;
    mode_push(mode, init);
    for (int guard = 0; g_mode_stack.depth > base && guard < 10000; guard++) {
        uint8_t top = (uint8_t)(g_mode_stack.depth - 1);
        GameMode m = (GameMode)g_mode_stack.mode[top];
        if (m != GAME_MODE_BATTLE_CALC && m != GAME_MODE_BATTLE_ACTION && m != GAME_MODE_BATTLE_KO &&
            m != GAME_MODE_BATTLE_APPLY && m != GAME_MODE_BATTLE_REVIVE && m != GAME_MODE_LOAD_BATTLE_SCENE) {
            if (m != GAME_MODE_DISPLAY_TEXT && m != GAME_MODE_BATTLE_WAIT && m != GAME_MODE_FADE_WAIT)
                unexpected_child = (int)m;
            result = mode_pop(0);
            continue;
        }
        if (m == GAME_MODE_LOAD_BATTLE_SCENE)
            scene_reloaded = 1;
        StepResult r = mode_dispatch_step(m, &g_mode_stack.state[top]);
        if (r.kind == STEP_PUSH)
            mode_push(r.push_mode, r.push_init);
        else if (r.kind == STEP_POP)
            result = mode_pop(r.pop_result);
    }
    return result;
}

static uint16_t run_calc(uint8_t kind, uint16_t arg0, uint16_t arg1) {
    ModeState init;
    battle_calc_make_init(&init, kind, arg0, arg1);
    return (uint16_t)run_mode(GAME_MODE_BATTLE_CALC, &init);
}

/* Call the port's version of c->name. Returns 0 for an unknown name. */
static int run_port(const Case *c, uint16_t *ret) {
    const char *n = c->name;
    *ret = 0;
    if (!strcmp(n, "SUCCESS_255")) *ret = battle_success_255(c->a);
    else if (!strcmp(n, "SUCCESS_500")) *ret = battle_success_500(c->a);
    else if (!strcmp(n, "SUCCESS_SPEED")) *ret = battle_success_speed(c->a);
    else if (!strcmp(n, "SUCCESS_LUCK40")) *ret = battle_success_luck40();
    else if (!strcmp(n, "SUCCESS_LUCK80")) *ret = battle_success_luck80();
    else if (!strcmp(n, "DETERMINE_DODGE")) *ret = battle_determine_dodge();
    else if (!strcmp(n, "TWENTY_FIVE_PERCENT_VARIANCE")) *ret = battle_25pct_variance(c->a);
    else if (!strcmp(n, "FIFTY_PERCENT_VARIANCE")) *ret = battle_50pct_variance(c->a);
    else if (!strcmp(n, "CALC_PSI_DMG_MODIFIERS")) *ret = battle_calc_psi_dmg_modifier((uint8_t)c->a);
    else if (!strcmp(n, "CALC_PSI_RES_MODIFIERS")) *ret = battle_calc_psi_res_modifier((uint8_t)c->a);
    else if (!strcmp(n, "INCREASE_OFFENSE_16TH")) battle_increase_offense(battler_at(c->a));
    else if (!strcmp(n, "HEXADECIMATE_OFFENSE")) battle_decrease_offense(battler_at(c->a));
    else if (!strcmp(n, "INCREASE_DEFENSE_16TH")) battle_increase_defense(battler_at(c->a));
    else if (!strcmp(n, "HEXADECIMATE_DEFENSE")) battle_decrease_defense(battler_at(c->a));
    else if (!strcmp(n, "INFLICT_STATUS_BATTLE")) *ret = battle_inflict_status(battler_at(c->a), c->x, c->y);
    else if (!strcmp(n, "LEVEL_UP_CHAR")) host_test_level_up_char_silent(c->a);
    else if (!strcmp(n, "RESET_CHAR_LEVEL_ONE")) reset_char_level_one(c->a, c->x, c->y);
    else if (!strcmp(n, "MISS_CALC")) *ret = run_calc(BC_MISS_CALC, c->a, 0);
    else if (!strcmp(n, "SMAAAASH")) *ret = run_calc(BC_SMAAAASH, 0, 0);
    else if (!strcmp(n, "CALC_RESIST_DAMAGE")) *ret = run_calc(BC_RESIST_DAMAGE, c->a, c->x);
    else if (!strcmp(n, "PSI_SHIELD_NULLIFY")) *ret = run_calc(BC_PSI_SHIELD_NULLIFY, 0, 0);
    else if (!strcmp(n, "WEAKEN_SHIELD")) *ret = run_calc(BC_WEAKEN_SHIELD, 0, 0);
    else if (!strcmp(n, "HEAL_STRANGENESS")) *ret = run_calc(BC_HEAL_STRANGENESS, 0, 0);
    else if (!strcmp(n, "FAIL_ATTACK_ON_NPCS")) *ret = run_calc(BC_FAIL_ON_NPCS, 0, 0);
    else if (!strcmp(n, "CALC_DAMAGE"))
        *ret = run_calc(BC_CALC_DAMAGE, (uint16_t)(c->a - battlers_table_addr), c->x);
    else if (!strcmp(n, "BATTLE_INIT_PLAYER_STATS")) battle_init_player_stats(c->a, battler_at(c->x));
    else if (!strcmp(n, "BATTLE_INIT_ENEMY_STATS")) battle_init_enemy_stats(battler_at(c->x), c->a);
    else if (!strcmp(n, "CHOOSE_TARGET")) choose_target((uint16_t)(c->a - battlers_table_addr));
    else if (!strncmp(n, "BTLACT_", 7)) {
        ModeState init;
        if (!c->func)
            return 0;
        /* An action without a stepper runs inline inside the dispatch */
        if (battle_action_dispatch(c->func, &init))
            run_mode(GAME_MODE_BATTLE_ACTION, &init);
    } else return 0;
    return 1;
}

static void report_bytes(const char *what, uint16_t idx, const uint8_t *want, const uint8_t *got, size_t len) {
    for (size_t k = 0; k < len; k++) {
        if (want[k] != got[k])
            printf("    %s %u +%zu: rom %02X port %02X\n", what, idx, k, want[k], got[k]);
    }
}

/* A routine that displayed text ran frames in the oracle, and every text frame
 * rolls the HP/PP meters (UPDATE_HPPP_METER_AND_RENDER): take the ROM's rolled
 * current_hp/current_pp (and their fractions) as given, they are not battle math. */
static void mask_rolling_meters(const Case *c, uint8_t *port_char, const uint8_t *rom_char) {
    if (c->frames <= 1)
        return;
    static const size_t fields[] = {
        offsetof(CharStruct, current_hp_fraction), offsetof(CharStruct, current_hp),
        offsetof(CharStruct, current_pp_fraction), offsetof(CharStruct, current_pp),
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        memcpy(port_char + fields[i], rom_char + fields[i], sizeof(uint16_t));
    /* Once no enemy is left standing the meters stop where they are: the
     * targets snap to the rolled values (the "survives a mortal hit when the
     * battle ends first" rule), so they are frame-count dependent too. */
    if (battle_count_chars(1) == 0) {
        static const size_t targets[] = {
            offsetof(CharStruct, current_hp_target), offsetof(CharStruct, current_pp_target),
        };
        for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
            memcpy(port_char + targets[i], rom_char + targets[i], sizeof(uint16_t));
    }
}

/* "let ID HEX" lines name a blob that later lines use as "@ID" (the oracle
 * interns the save's party, game state and flags, which repeat in every case). */
#define MAX_LETS 8192
static char *lets[MAX_LETS];

static int define_let(const char *rest) {
    unsigned id;
    int used = 0;
    if (sscanf(rest, "%x %n", &id, &used) != 1 || id >= MAX_LETS || lets[id])
        return 0;
    size_t len = strcspn(rest + used, " \n");
    lets[id] = malloc(len + 1);
    memcpy(lets[id], rest + used, len);
    lets[id][len] = '\0';
    return 1;
}

/* Copy `in` to `out` with every "@ID" token replaced by its blob. */
static int expand_lets(const char *in, char *out, size_t cap) {
    size_t n = 0;
    while (*in) {
        const char *src = in;
        size_t len = 1;
        if (*in == '@') {
            char *end;
            unsigned long id = strtoul(in + 1, &end, 16);
            if (end == in + 1 || id >= MAX_LETS || !lets[id])
                return 0;
            src = lets[id];
            len = strlen(src);
            in = end;
        } else {
            in++;
        }
        if (n + len + 1 > cap)
            return 0;
        memcpy(out + n, src, len);
        n += len;
    }
    out[n] = '\0';
    return 1;
}

static int run_case(const Case *c, int case_no) {
    /* The oracle starts every case from the same freshly booted overworld,
     * where no battle has run yet: start from a zeroed battle state too. */
    memset(&bt, 0, sizeof(bt));
    for (int i = 0; i < c->n_battlers; i++)
        memcpy(&bt.battlers_table[c->battlers[i].index], c->battlers[i].bytes, sizeof(Battler));
    for (int i = 0; i < c->n_chars; i++)
        memcpy(&party_characters[c->chars[i].index], c->chars[i].bytes, sizeof(CharStruct));
    for (int i = 0; i < c->n_flags; i++) {
        if (c->flags[i][1])
            event_flag_set(c->flags[i][0]);
        else
            event_flag_clear(c->flags[i][0]);
    }
    for (int i = 0; i < c->n_vars; i++)
        memcpy(c->vars[i].def->ptr, c->vars[i].bytes, c->vars[i].def->size);
    if (c->has_gs)
        memcpy(&game_state, c->gs, sizeof(GameState));
    if (c->atk >= 0)
        bt.current_attacker = (uint16_t)(c->atk * sizeof(Battler));
    if (c->tgt >= 0)
        bt.current_target = (uint16_t)(c->tgt * sizeof(Battler));
    rng_state.a = c->rng_a;
    rng_state.b = c->rng_b;

    uint16_t ret;
    unexpected_child = 0;
    scene_reloaded = 0;
    if (!run_port(c, &ret)) {
        printf("case %d %s: no port mapping\n", case_no, c->name);
        return 0;
    }

    int ok = 1;
    if (c->has_ret && ret != c->ret)
        ok = 0;
    if (rng_state.a != c->rng_out_a || rng_state.b != c->rng_out_b)
        ok = 0;
    for (int i = 0; i < c->n_battlers_out; i++) {
        Battler *b = &bt.battlers_table[c->battlers_out[i].index];
        if (scene_reloaded)
            b->blink_timer = c->battlers_out[i].bytes[offsetof(Battler, blink_timer)];
        if (memcmp(b, c->battlers_out[i].bytes, sizeof(Battler)))
            ok = 0;
    }
    for (int i = 0; i < c->n_chars_out; i++) {
        uint8_t *port = (uint8_t *)&party_characters[c->chars_out[i].index];
        mask_rolling_meters(c, port, c->chars_out[i].bytes);
        if (memcmp(port, c->chars_out[i].bytes, sizeof(CharStruct)))
            ok = 0;
    }

    for (int i = 0; i < c->n_vars_out; i++)
        if (memcmp(c->vars_out[i].def->ptr, c->vars_out[i].bytes, c->vars_out[i].def->size))
            ok = 0;
    if (c->has_gs && memcmp(&game_state, c->gs_out, sizeof(GameState)))
        ok = 0;

    if (unexpected_child) {
        printf("case %d %s: pushed mode %d (the recorded inputs should not reach it)\n", case_no, c->name,
               unexpected_child);
        ok = 0;
    }
    if (!ok || verbose) {
        printf("%s case %d %s regs %X %X %X rng %04X %04X\n", ok ? "ok  " : "FAIL", case_no, c->name,
               c->a, c->x, c->y, c->rng_a, c->rng_b);
    }
    if (!ok) {
        if (c->has_ret && ret != c->ret)
            printf("    ret: rom %X port %X\n", c->ret, ret);
        if (rng_state.a != c->rng_out_a || rng_state.b != c->rng_out_b)
            printf("    rng: rom %04X %04X port %04X %04X\n", c->rng_out_a, c->rng_out_b, rng_state.a, rng_state.b);
        for (int i = 0; i < c->n_battlers_out; i++)
            report_bytes("battler", c->battlers_out[i].index, c->battlers_out[i].bytes,
                         (const uint8_t *)&bt.battlers_table[c->battlers_out[i].index], sizeof(Battler));
        for (int i = 0; i < c->n_vars_out; i++)
            report_bytes(c->vars_out[i].def->name, 0, c->vars_out[i].bytes, c->vars_out[i].def->ptr,
                         c->vars_out[i].def->size);
        if (c->has_gs)
            report_bytes("game_state", 0, c->gs_out, (const uint8_t *)&game_state, sizeof(GameState));
        for (int i = 0; i < c->n_chars_out; i++)
            report_bytes("char", c->chars_out[i].index, c->chars_out[i].bytes,
                         (const uint8_t *)&party_characters[c->chars_out[i].index], sizeof(CharStruct));
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s CASES_FILE [-v]\n", argv[0]);
        return 2;
    }
    verbose = argc > 2 && !strcmp(argv[2], "-v");
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }

    load_enemy_spawn_data(); /* enemy_config_table + battle_action_table */
    /* Battle start loads the sprite size table (SETUP_BATTLE_ENEMY_SPRITES);
     * the bombs' splash range reads it through GET_BATTLE_SPRITE_WIDTH. */
    battle_sprites_pointers_data = ASSET_DATA(ASSET_DATA_BATTLE_SPRITES_POINTERS_BIN);

    static Case c;
    static char line[LINE_MAX_LEN];
    int lineno = 0, cases = 0, passed = 0, in_case = 0;
    static char raw[LINE_MAX_LEN];
    while (fgets(raw, sizeof(raw), f)) {
        lineno++;
        if (raw[0] == '#' || raw[0] == '\n')
            continue;
        if (!strncmp(raw, "let ", 4)) {
            if (!define_let(raw + 4)) {
                fprintf(stderr, "%s:%d: bad let\n", argv[1], lineno);
                fclose(f);
                return 2;
            }
            continue;
        }
        if (!expand_lets(raw, line, sizeof(line))) {
            fprintf(stderr, "%s:%d: bad @ reference\n", argv[1], lineno);
            fclose(f);
            return 2;
        }
        char key[32];
        int used = 0;
        if (sscanf(line, "%31s %n", key, &used) != 1)
            continue;
        const char *rest = line + used;
        unsigned v0, v1, v2;
        int good = 1;
        if (!strcmp(key, "battlers_table")) {
            good = sscanf(rest, "%x", &v0) == 1;
            battlers_table_addr = (uint16_t)v0;
        } else if (!strcmp(key, "case")) {
            memset(&c, 0, sizeof(c));
            c.atk = c.tgt = -1;
            good = sscanf(rest, "%63s", c.name) == 1;
            in_case = 1;
        } else if (!in_case) {
            good = 0;
        } else if (!strcmp(key, "regs")) {
            good = sscanf(rest, "%x %x %x", &v0, &v1, &v2) == 3;
            c.a = (uint16_t)v0, c.x = (uint16_t)v1, c.y = (uint16_t)v2;
        } else if (!strcmp(key, "func")) {
            good = sscanf(rest, "%x", &v0) == 1;
            c.func = v0;
        } else if (!strcmp(key, "rng")) {
            good = sscanf(rest, "%x %x", &v0, &v1) == 2;
            c.rng_a = (uint16_t)v0, c.rng_b = (uint16_t)v1;
        } else if (!strcmp(key, "rng_out")) {
            good = sscanf(rest, "%x %x", &v0, &v1) == 2;
            c.rng_out_a = (uint16_t)v0, c.rng_out_b = (uint16_t)v1;
        } else if (!strcmp(key, "atk")) {
            good = sscanf(rest, "%x", &v0) == 1;
            c.atk = (int)v0;
        } else if (!strcmp(key, "tgt")) {
            good = sscanf(rest, "%x", &v0) == 1;
            c.tgt = (int)v0;
        } else if (!strcmp(key, "frames")) {
            good = sscanf(rest, "%x", &v0) == 1;
            c.frames = (uint16_t)v0;
        } else if (!strcmp(key, "ret")) {
            good = sscanf(rest, "%x", &v0) == 1;
            c.ret = (uint16_t)v0;
            c.has_ret = 1;
        } else if (!strcmp(key, "flag")) {
            good = c.n_flags < MAX_BLOBS && sscanf(rest, "%x %x", &v0, &v1) == 2;
            if (good) {
                c.flags[c.n_flags][0] = (uint16_t)v0;
                c.flags[c.n_flags][1] = (uint16_t)v1;
                c.n_flags++;
            }
        } else if (!strcmp(key, "var")) {
            good = add_var(c.vars, &c.n_vars, rest);
        } else if (!strcmp(key, "var_out")) {
            good = add_var(c.vars_out, &c.n_vars_out, rest);
        } else if (!strcmp(key, "game_state")) {
            static char hex[LINE_MAX_LEN];
            good = sscanf(rest, "%4000s", hex) == 1 && parse_hex_blob(hex, c.gs, sizeof(GameState));
            c.has_gs = good;
        } else if (!strcmp(key, "game_state_out")) {
            good = c.has_gs && apply_changes(c.gs_out, c.gs, sizeof(GameState), rest);
        } else if (!strcmp(key, "battler")) {
            good = add_blob(c.battlers, &c.n_battlers, rest, sizeof(Battler));
        } else if (!strcmp(key, "char")) {
            good = add_blob(c.chars, &c.n_chars, rest, sizeof(CharStruct));
        } else if (!strcmp(key, "battler_out")) {
            good = add_changes(c.battlers_out, &c.n_battlers_out, c.battlers, c.n_battlers, rest, sizeof(Battler));
        } else if (!strcmp(key, "char_out")) {
            good = add_changes(c.chars_out, &c.n_chars_out, c.chars, c.n_chars, rest, sizeof(CharStruct));
        } else if (!strcmp(key, "end")) {
            passed += run_case(&c, cases);
            cases++;
            in_case = 0;
        } else {
            good = 0;
        }
        if (!good) {
            fprintf(stderr, "%s:%d: bad line: %s", argv[1], lineno, line);
            fclose(f);
            return 2;
        }
    }
    fclose(f);
    if (!battlers_table_addr && cases) {
        fprintf(stderr, "%s: no battlers_table header\n", argv[1]);
        return 2;
    }
    printf("%d/%d cases match the ROM\n", passed, cases);
    return (cases > 0 && passed == cases) ? 0 : 1;
}
