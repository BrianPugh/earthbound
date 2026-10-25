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
 * so the recorded blobs are copied in and out unchanged.
 *
 * Usage: battle_math_test CASES_FILE [-v]
 * Exit status: 0 when every case matches, 1 otherwise.
 */
#include "core/math.h"
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

#define MAX_BLOBS 8
#define LINE_MAX_LEN 2048

typedef struct {
    uint16_t index;
    uint8_t bytes[sizeof(CharStruct) > sizeof(Battler) ? sizeof(CharStruct) : sizeof(Battler)];
} Blob;

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
    if (*n >= MAX_BLOBS || sscanf(rest, "%x %511s", &idx, hex) != 2)
        return 0;
    arr[*n].index = (uint16_t)idx;
    if (!parse_hex_blob(hex, arr[*n].bytes, len))
        return 0;
    (*n)++;
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

static int32_t run_mode(GameMode mode, const ModeState *init) {
    uint8_t base = g_mode_stack.depth;
    int32_t result = 0;
    mode_push(mode, init);
    for (int guard = 0; g_mode_stack.depth > base && guard < 10000; guard++) {
        uint8_t top = (uint8_t)(g_mode_stack.depth - 1);
        GameMode m = (GameMode)g_mode_stack.mode[top];
        if (m != GAME_MODE_BATTLE_CALC && m != GAME_MODE_BATTLE_ACTION) {
            if (m != GAME_MODE_DISPLAY_TEXT && m != GAME_MODE_BATTLE_WAIT)
                unexpected_child = (int)m;
            result = mode_pop(0);
            continue;
        }
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
    else if (!strncmp(n, "BTLACT_", 7)) {
        ModeState init;
        if (!c->func || !battle_action_dispatch(c->func, &init))
            return 0;
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
    if (c->atk >= 0)
        bt.current_attacker = (uint16_t)(c->atk * sizeof(Battler));
    if (c->tgt >= 0)
        bt.current_target = (uint16_t)(c->tgt * sizeof(Battler));
    rng_state.a = c->rng_a;
    rng_state.b = c->rng_b;

    uint16_t ret;
    unexpected_child = 0;
    if (!run_port(c, &ret)) {
        printf("case %d %s: no port mapping\n", case_no, c->name);
        return 0;
    }

    int ok = 1;
    if (c->has_ret && ret != c->ret)
        ok = 0;
    if (rng_state.a != c->rng_out_a || rng_state.b != c->rng_out_b)
        ok = 0;
    for (int i = 0; i < c->n_battlers_out; i++)
        if (memcmp(&bt.battlers_table[c->battlers_out[i].index], c->battlers_out[i].bytes, sizeof(Battler)))
            ok = 0;
    for (int i = 0; i < c->n_chars_out; i++) {
        uint8_t *port = (uint8_t *)&party_characters[c->chars_out[i].index];
        mask_rolling_meters(c, port, c->chars_out[i].bytes);
        if (memcmp(port, c->chars_out[i].bytes, sizeof(CharStruct)))
            ok = 0;
    }

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

    static Case c;
    char line[LINE_MAX_LEN];
    int lineno = 0, cases = 0, passed = 0, in_case = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (line[0] == '#' || line[0] == '\n')
            continue;
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
