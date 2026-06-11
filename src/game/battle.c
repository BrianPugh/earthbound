/*
 * Battle system core — utility functions for combat.
 *
 * Ports of assembly routines from asm/battle/:
 *   set_hp.asm, set_pp.asm, reduce_hp.asm, reduce_pp.asm,
 *   recover_hp.asm, recover_pp.asm, inflict_status.asm,
 *   determine_dodge.asm, success_255.asm, success_500.asm,
 *   success_speed.asm, count_chars.asm, ko_target.asm,
 *   revive_target.asm, increase_offense_16th.asm,
 *   decrease_offense_16th.asm, increase_defense_16th.asm,
 *   decrease_defense_16th.asm, psi_shield_nullify.asm,
 *   25_percent_variance.asm, 50_percent_variance.asm,
 *   calc_psi_damage_modifiers.asm, calc_psi_resistance_modifiers.asm
 */
#include "game/battle.h"
#include "game/battle_internal.h"
#include "game/game_state.h"
#include "game/display_text.h"
#include "game/inventory.h"
#include "game/audio.h"
#include "game/overworld.h"
#include "game/map_loader.h"
#include "game/window.h"
#include "game/text.h"
#include "game/fade.h"
#include "game/oval_window.h"
#include "game/battle_bg.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "data/assets.h"
#include "core/math.h"
#include "core/memory.h"
#include "core/mode_stack.h"
#include "include/binary.h"
#include "include/pad.h"
#include "snes/ppu.h"
#include "core/decomp.h"
#include "platform/platform.h"
#include "data/text_refs.h"
#include "game/display_text_internal.h"  /* dt_make_child_init (battle text pushes) */
#include "core/log.h"
#include <string.h>

/* EarthBound character encoding constants (from include/macros.asm EBTEXT) */
/* EB_CHAR_SPACE, EB_CHAR_A_MINUS_1 moved to battle_internal.h */

/* TARGETTED_* defines moved to battle_internal.h */

/* Window IDs used by battle item/targeting/PSI system */
#define WINDOW_INVENTORY          0x02
#define WINDOW_TEXT_STANDARD      0x01
/* Other window IDs now in window.h (PSI_TARGET_COST, TEXT_BATTLE, PSI_CATEGORY,
 * BATTLE_ACTION_NAME, TARGETING_PROMPT) */

/* PSI field offsets, entry size, ability IDs now in battle.h */

/* PSI defines moved to battle_psi.c */

/* ---- Consolidated battle state ---- */
BattleState bt = {
    .current_flashing_enemy = -1,
    .current_flashing_row = -1,
    .last_selected_psi_description = 0x00FF,
};

/* ROM data pointers (not in BattleState — these are const ROM data, not saveable state) */
const BattleAction *battle_action_table;
const EnemyData *enemy_config_table;
const uint8_t *battle_sprites_pointers_data;

/* Scratch arrays for instant-win sorted matching (file-static) */
static uint16_t instant_win_sorted_offense[4];
static uint16_t instant_win_sorted_hp[4];
static uint16_t instant_win_sorted_defense[4];

/* BACKGROUND_COLOUR_BACKUP — saves palettes[0] before screen flash */
/* bt.background_colour_backup and battle_menu_selection now in BattleState bt. */

/* Assembly globals referenced by battle_routine and final-attack dispatch */
#include "game_main.h"

/* ---- Forward declarations ---- */
void display_text_with_prompt(const uint8_t *text, size_t size);
void display_text_with_prompt_addr(uint32_t addr);

/* Asset table references loaded from ROM (defined near battle_routine) */
extern const uint8_t *btl_entry_ptr_table;
extern const uint8_t *btl_entry_bg_table;
extern const uint8_t *consolation_item_table;
/* npc_ai_table declared in battle_internal.h */

/* clear_hppp_window_header() is implemented in window.c */

/*
 * DISPLAY_IN_BATTLE_TEXT (asm/text/display_in_battle_text.asm)
 *
 * Displays battle text without CNUM parameter.
 * Same auto-fight cancel and blinking prompt logic as display_text_wait.
 */
void display_in_battle_text(const uint8_t *text, size_t size) {
    /* Auto-fight cancel: if enabled and B pressed, disable auto-fight */
    if (game_state.auto_fight_enable && (core.pad1_held & PAD_B)) {
        game_state.auto_fight_enable = 0;
        clear_hppp_window_header();
    }

    if (bt.battle_mode_flag) {
        dt.blinking_triangle_flag = 2;
    }

    display_text(text, size);
    dt.blinking_triangle_flag = 0;
}

/* SNES address variants — resolve text address then display with battle wrappers. */
void display_text_wait_addr(uint32_t addr, uint32_t param) {
    if (game_state.auto_fight_enable && (core.pad1_held & PAD_B)) {
        game_state.auto_fight_enable = 0;
        clear_hppp_window_header();
    }
    set_cnum(param);
    if (bt.battle_mode_flag) {
        dt.blinking_triangle_flag = 2;
    }
    display_text_from_addr(addr);
    dt.blinking_triangle_flag = 0;
}

void display_in_battle_text_addr(uint32_t addr) {
    if (game_state.auto_fight_enable && (core.pad1_held & PAD_B)) {
        game_state.auto_fight_enable = 0;
        clear_hppp_window_header();
    }
    if (bt.battle_mode_flag) {
        dt.blinking_triangle_flag = 2;
    }
    display_text_from_addr(addr);
    dt.blinking_triangle_flag = 0;
}

/*
 * FIND_NEXT_ENEMY_LETTER (asm/battle/find_next_enemy_letter.asm)
 *
 * For a given enemy_id, scans all battlers to find which "letters" (A, B, C...)
 * are already assigned to enemies of the same type, then returns the next
 * available letter (1='A', 2='B', ..., 26='Z'). Returns 0 if all 26 are used.
 */
uint8_t find_next_enemy_letter(uint16_t enemy_id) {
    /* Clear the used-letters tracking array */
    memset(bt.used_enemy_letters, 0, 26);

    /* Scan all battlers for conscious enemies with the same base ID */
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0)
            continue;
        if (b->ally_or_enemy != 1)
            continue;
        if (b->enemy_type_id != enemy_id)
            continue;
        /* Mark this letter as used (the_flag is 1-based: 1='A', 2='B', etc.) */
        if (b->the_flag >= 1 && b->the_flag <= 26)
            bt.used_enemy_letters[b->the_flag - 1] = 1;
    }

    /* Find the first unused letter */
    for (int x = 0; x < 26; x++) {
        if (bt.used_enemy_letters[x] == 0)
            return (uint8_t)(x + 1);
    }
    return 0;
}

/*
 * BATTLE_INIT_ENEMY_STATS (asm/battle/init_enemy_stats.asm)
 *
 * Initialize a battler struct from the enemy configuration table.
 * Parameters: battler = target battler, enemy_id = index into enemy_config_table.
 */
void battle_init_enemy_stats(Battler *battler, uint16_t enemy_id) {
    const EnemyData *edata = &enemy_config_table[enemy_id];

    /* Zero-fill the entire battler struct */
    memset(battler, 0, sizeof(Battler));

    /* Track highest enemy level for this battle */
    uint8_t level = edata->level;
    if ((uint16_t)level > bt.highest_enemy_level_in_battle)
        bt.highest_enemy_level_in_battle = level;

    /* Basic identity */
    battler->id = enemy_id;
    battler->enemy_type_id = (uint8_t)enemy_id;
    battler->sprite = (uint8_t)edata->battle_sprite;

    /* Assign next available letter for this enemy type */
    battler->the_flag = find_next_enemy_letter(enemy_id);

    /* Consciousness and side */
    battler->consciousness = 1;
    battler->ally_or_enemy = 1;  /* enemy */
    battler->npc_id = 0;
    battler->row = edata->row;

    /* HP and PP */
    battler->hp_max = edata->hp;
    battler->hp_target = edata->hp;
    battler->hp = edata->hp;
    battler->pp_max = edata->pp;
    battler->pp_target = edata->pp;
    battler->pp = edata->pp;

    /* Stats: base stats are 8-bit, active stats are 16-bit zero-extended */
    battler->base_offense = (uint8_t)edata->offense;
    battler->offense = (uint16_t)(uint8_t)edata->offense;
    battler->base_defense = (uint8_t)edata->defense;
    battler->defense = (uint16_t)(uint8_t)edata->defense;
    battler->base_speed = edata->speed;
    battler->speed = (uint16_t)edata->speed;
    battler->base_guts = edata->guts;
    battler->guts = (uint16_t)edata->guts;
    battler->base_luck = edata->luck;
    battler->luck = (uint16_t)edata->luck;

    /* Vitality is zeroed for enemies, IQ comes from config */
    battler->vitality = 0;
    battler->iq = edata->iq;

    /* PSI resistance modifiers */
    battler->fire_resist = battle_calc_psi_dmg_modifier(edata->fire_vulnerability);
    battler->freeze_resist = battle_calc_psi_dmg_modifier(edata->freeze_vulnerability);
    battler->flash_resist = battle_calc_psi_res_modifier(edata->flash_vulnerability);
    battler->paralysis_resist = battle_calc_psi_res_modifier(edata->paralysis_vulnerability);

    /* Hypnosis and brainshock share a single byte:
     * hypnosis_resist = modifier(value), brainshock_resist = modifier(3 - value) */
    uint8_t hyp_bs = edata->hypnosis_brainshock_vulnerability;
    battler->hypnosis_resist = battle_calc_psi_res_modifier(hyp_bs);
    battler->brainshock_resist = battle_calc_psi_res_modifier(3 - hyp_bs);

    /* Money and experience */
    battler->money = edata->money;
    battler->exp = edata->exp;

    /* Apply initial status effect */
    switch (edata->initial_status) {
    case INITIAL_STATUS_PSI_SHIELD:
        battle_shields_common(battler, STATUS_6_PSI_SHIELD);
        break;
    case INITIAL_STATUS_PSI_SHIELD_POWER:
        battle_shields_common(battler, STATUS_6_PSI_SHIELD_POWER);
        break;
    case INITIAL_STATUS_SHIELD:
        battle_shields_common(battler, STATUS_6_SHIELD);
        break;
    case INITIAL_STATUS_SHIELD_POWER:
        battle_shields_common(battler, STATUS_6_SHIELD_POWER);
        break;
    case INITIAL_STATUS_ASLEEP:
        battler->afflictions[STATUS_GROUP_TEMPORARY] = STATUS_2_ASLEEP;
        break;
    case INITIAL_STATUS_CANT_CONCENTRATE:
        battler->afflictions[STATUS_GROUP_CONCENTRATION] = STATUS_4_CANT_CONCENTRATE4;
        break;
    case INITIAL_STATUS_STRANGE:
        battler->afflictions[STATUS_GROUP_STRANGENESS] = STATUS_3_STRANGE;
        break;
    default:
        break;
    }
}

/*
 * BATTLE_INIT_PLAYER_STATS (asm/battle/init_player_stats.asm)
 *
 * Initialize a battler from a party character's stats.
 * character is 1-based (1=Ness, 2=Paula, 3=Jeff, 4=Poo).
 */
void battle_init_player_stats(uint16_t character, Battler *target) {
    CharStruct *cs = &party_characters[character - 1];

    /* Zero-fill the battler */
    memset(target, 0, sizeof(Battler));

    /* Identity */
    target->id = character;
    target->sprite = 0;
    target->consciousness = 1;
    target->ally_or_enemy = 0;  /* party */
    target->npc_id = 0;

    /* HP and PP from character */
    target->hp = cs->current_hp;
    target->hp_target = cs->current_hp_target;
    target->hp_max = cs->max_hp;
    target->pp = cs->current_pp;
    target->pp_target = cs->current_pp_target;
    target->pp_max = cs->max_pp;

    /* Copy afflictions */
    memcpy(target->afflictions, cs->afflictions, AFFLICTION_GROUP_COUNT);

    /* Stats: base stats are 8-bit, active stats are 16-bit zero-extended */
    target->base_offense = cs->offense;
    target->offense = (uint16_t)cs->offense;
    target->base_defense = cs->defense;
    target->defense = (uint16_t)cs->defense;
    target->base_speed = cs->speed;
    target->speed = (uint16_t)cs->speed;
    target->base_guts = cs->guts;
    target->guts = (uint16_t)cs->guts;
    target->base_luck = cs->luck;
    target->luck = (uint16_t)cs->luck;

    /* Vitality and IQ */
    target->vitality = cs->vitality;
    target->iq = cs->iq;

    /* PSI resistance modifiers */
    target->fire_resist = battle_calc_psi_dmg_modifier(cs->fire_resist);
    target->freeze_resist = battle_calc_psi_dmg_modifier(cs->freeze_resist);
    target->flash_resist = battle_calc_psi_res_modifier(cs->flash_resist);
    target->paralysis_resist = battle_calc_psi_res_modifier(cs->paralysis_resist);

    /* Hypnosis and brainshock share a single byte in char_struct */
    uint8_t hyp_bs = cs->hypnosis_brainshock_resist;
    target->hypnosis_resist = battle_calc_psi_res_modifier(hyp_bs);
    target->brainshock_resist = battle_calc_psi_res_modifier(3 - hyp_bs);

    /* Row is 0-based character index */
    target->row = (uint8_t)(character - 1);
}

void swap_attacker_with_target(void) {
    uint16_t tmp = bt.current_attacker;
    bt.current_attacker = bt.current_target;
    bt.current_target = tmp;
    fix_attacker_name(0);
    fix_target_name();
}

void set_current_item(uint8_t item) {
    dt.citem = item;
}

/*
 * DISPLAY_MENU_HEADER_TEXT (asm/text/menu/display_menu_header_text.asm)
 *
 * Creates WINDOW::TARGETING_PROMPT and prints a targeting prompt text.
 * text_index: 0="Who?", 1="Which?", 2="Where?", 3="Whom?", 4="Where?"
 *
 * Now shared via window.h — display_menu_header_text() and close_menu_header_window().
 */

/* ---------------------------------------------------------------------------
 * GAME_MODE_CHAR_SELECT — battle-style HP/PP character column selection.
 *
 * Run-to-completion port of char_select_prompt()'s battle path (mode 0/2). The
 * two-level render/input loop becomes a three-phase machine (CharSelectPhase),
 * mirroring GAME_MODE_NUMBER_SELECT: CSP_PRIME reproduces the original's second
 * post-render yield so input timing is frame-identical. The on_change/check_valid
 * function-pointer callbacks are stored as IDs (see cs_invoke_* in text.c) so the
 * state stays serializable. See docs/plans/savestate-unified-loop.md.
 * ------------------------------------------------------------------------- */

/* CSP_RENDER body: highlight the current character (mode 0), run one non-yielding
 * window frame, draw pagination arrows, reset the input poll counter. */
static void cs_render(CharSelectState *st) {
    if (st->mode == 0)
        select_battle_menu_character(st->current_index);
    clear_instant_printing();
    window_tick_work();
    render_pagination_arrows();
    st->counter = 0;
}

StepResult mode_step_char_select(ModeState *ms) {
    CharSelectState *st = &ms->char_select;

    /* Outlives the dispatch turn (pump_mode copies the init immediately): holds the
     * DISPLAY_TEXT child init for an on_change callback that requests a STEP_PUSH. */
    static ModeState cs_onchange_init;

    /* Post-child resume: an on_change callback (CS_ONCHANGE_PARTY_SELECT_SCRIPT)
     * STEP_PUSHed a DISPLAY_TEXT child; run its deferred render tail on the frame the
     * child pops back, before re-entering the input loop. */
    if (st->resume == CS_RESUME_INIT) {
        st->resume = CS_RESUME_NONE;
        dt.pagination_animation_frame = 0;
        cs_render(st);
        st->phase = CSP_PRIME;
        return STEP_RESULT_CONTINUE();
    } else if (st->resume == CS_RESUME_NAV) {
        st->resume = CS_RESUME_NONE;
        st->delay = 4;   /* shorter poll window right after navigating */
        cs_render(st);
        st->phase = CSP_PRIME;
        return STEP_RESULT_CONTINUE();
    }

    switch ((CharSelectPhase)st->phase) {
    case CSP_INIT: {
        /* Pre-loop one-shot (former char_select_prompt lines 556-561 /
         * party_character_selector lines 89-102): initial on_change, THEN reset the
         * pagination animation, THEN the first render — preserving the blocking
         * original's order (on_change completes before pagination_animation_frame=0).
         * The no-push path falls through to the first render with no extra yield. */
        uint8_t mid = game_state.party_members[st->current_index] & 0xFF;
        memset(&cs_onchange_init, 0, sizeof(cs_onchange_init));
        if (cs_invoke_on_change(st->on_change_id, mid, &cs_onchange_init)) {
            /* on_change pushed a DISPLAY_TEXT child; the pagination reset + first
             * render run in CS_RESUME_INIT after it pops. */
            st->resume = CS_RESUME_INIT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &cs_onchange_init);
        }
        dt.pagination_animation_frame = 0;
        cs_render(st);
        st->phase = CSP_PRIME;
        return STEP_RESULT_CONTINUE();
    }

    case CSP_RENDER:
        cs_render(st);
        st->phase = CSP_PRIME;
        return STEP_RESULT_CONTINUE();

    case CSP_PRIME:
        /* One HP/PP-meter frame before the first input read, matching the
         * original inner loop's first update_hppp_meter_and_render() yield. */
        update_hppp_meter_work();
        st->phase = CSP_INPUT;
        return STEP_RESULT_CONTINUE();

    case CSP_INPUT:
    default: {
        int16_t  direction_step = 0;
        uint16_t nav_sfx = 0;

        if (core.pad1_pressed & PAD_LEFT) {
            direction_step = -1;
            nav_sfx = (st->mode == 0) ? 27 : 2;   /* MENU_OPEN_CLOSE : CURSOR2 */
            dt.pagination_animation_frame = 2;
        } else if (core.pad1_pressed & PAD_RIGHT) {
            direction_step = 1;
            nav_sfx = (st->mode == 0) ? 27 : 2;
            dt.pagination_animation_frame = 3;
        } else if (core.pad1_pressed & PAD_CONFIRM) {
            uint16_t result = game_state.party_members[st->current_index] & 0xFF;
            play_sfx(1);  /* SFX::CURSOR1 */
            dt.pagination_animation_frame = -1;
            set_argument_memory(st->saved_argument_memory);
            return STEP_RESULT_POP(result);
        } else if ((core.pad1_pressed & PAD_CANCEL) && st->allow_cancel == 1) {
            play_sfx((st->mode == 0) ? 27 : 2);
            clear_battle_menu_character_indicator();
            dt.pagination_animation_frame = -1;
            set_argument_memory(st->saved_argument_memory);
            return STEP_RESULT_POP(0);
        } else {
            /* No actionable input this frame (includes a disallowed cancel). */
            st->counter++;
            if (st->counter >= st->delay) {
                /* Poll window expired: toggle pagination arrow animation, reset
                 * the longer delay, and re-render for the next outer iteration. */
                dt.pagination_animation_frame =
                    (dt.pagination_animation_frame != 0) ? 0 : 1;
                st->delay = 10;
                cs_render(st);
                st->phase = CSP_PRIME;
                return STEP_RESULT_CONTINUE();
            }
            update_hppp_meter_work();
            return STEP_RESULT_CONTINUE();
        }

        /* Direction pressed: find the next valid character in that direction with
         * wrap-around; play SFX + on_change only if the selection actually moved. */
        int16_t new_index = (int16_t)st->current_index + direction_step;
        uint8_t party_count = game_state.player_controlled_party_count & 0xFF;
        for (;;) {
            if (new_index >= (int16_t)party_count)
                new_index = 0;
            else if (new_index < 0)
                new_index = (int16_t)(party_count - 1);

            if (st->check_valid_id != CS_CHECKVALID_NONE) {
                uint8_t mid = game_state.party_members[new_index];
                if (cs_invoke_check_valid(st->check_valid_id, mid & 0xFF) == 0) {
                    new_index += direction_step;
                    continue;
                }
            }
            break;
        }

        if ((uint16_t)new_index != st->current_index) {
            play_sfx(nav_sfx);
            st->current_index = (uint16_t)new_index;
            uint8_t mid = game_state.party_members[st->current_index] & 0xFF;
            memset(&cs_onchange_init, 0, sizeof(cs_onchange_init));
            if (cs_invoke_on_change(st->on_change_id, mid, &cs_onchange_init)) {
                /* on_change pushed a DISPLAY_TEXT child; the delay reset + re-render
                 * tail runs in the CS_RESUME_NAV handler when it pops. */
                st->resume = CS_RESUME_NAV;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &cs_onchange_init);
            }
        }

        st->delay = 4;   /* shorter poll window right after navigating */
        cs_render(st);
        st->phase = CSP_PRIME;
        return STEP_RESULT_CONTINUE();
    }
    }
}

/*
 * CHAR_SELECT_PROMPT — Port of asm/text/character_select_prompt.asm (~418 lines).
 *
 * Party member selection with two modes:
 *   mode == 1: Overworld-style — creates a text window with party member names,
 *              runs SELECTION_MENU, uses on_change as cursor_move_callback.
 *   mode == 0: Battle-style — HPPP column selection with LEFT/RIGHT navigation,
 *              highlights current character via SELECT_BATTLE_MENU_CHARACTER.
 *   mode == 2: Like mode 0 but always starts at index 0 (ignores stored character).
 *
 * on_change: called with party member ID (1-based) when selection moves. NULL = none.
 * check_valid: called with party member ID; returns non-zero if valid. NULL = all valid.
 *              Used to skip characters (e.g., Jeff in PSI menu).
 *
 * Returns: 1-based character ID (PARTY_MEMBER enum), or 0 if cancelled.
 */
uint16_t char_select_prompt(uint16_t mode, uint16_t allow_cancel,
                            void (*on_change)(uint16_t),
                            uint16_t (*check_valid)(uint16_t)) {
    /* Save active window's argument_memory (assembly lines 31-37). */
    uint32_t saved_argument_memory = get_argument_memory();
    uint16_t result = 0;

    if (mode == 1) {
        /* --- Overworld path (assembly lines 41-126) ---
         * Creates a window with party member names and runs SELECTION_MENU. */
        uint16_t window_id = char_select_overworld_prepare(on_change);

        result = selection_menu(allow_cancel);

        char_select_overworld_finish(window_id, on_change != NULL);
        set_argument_memory(saved_argument_memory);
        return result;
    }

    /* --- Battle-style path (assembly lines 127-405): HP/PP column selection,
     * now run as GAME_MODE_CHAR_SELECT (run-to-completion). The input loop lives
     * in mode_step_char_select(). --- */
    ModeState init;
    char_select_make_init(&init, mode, allow_cancel,
                          cs_onchange_id(on_change), cs_checkvalid_id(check_valid));

    /* The step performs @CLEANUP_AND_RETURN (restore argument_memory,
     * pagination_animation_frame = -1) before it pops. */
    return (uint16_t)pump_mode(GAME_MODE_CHAR_SELECT, &init);
}

/* char_select_prompt mode-1 (overworld path) prologue, factored out so an
 * already-converted parent (GAME_MODE_DETERMINE_TARGETING's ally pick) can
 * build the name window and STEP_PUSH SELECTION_MENU itself: save the window
 * text attributes, create the party-sized select window, add one name item
 * per member (assembly lines 41-108: text_x = i * 6, text_y = 0), print, and
 * set the cursor-move callback. Returns the window id to close on finish.
 * NOTE: does not save argument_memory — the caller brackets that. */
uint16_t char_select_overworld_prepare(void (*on_change)(uint16_t)) {
    save_window_text_attributes();

    uint16_t party_count = game_state.player_controlled_party_count & 0xFF;
    uint16_t window_id;
    if (party_count == 1) {
        window_id = WINDOW_SINGLE_CHARACTER_SELECT;
    } else {
        window_id = WINDOW_TARGETING_PROMPT + party_count - 1;
    }
    create_window(window_id);

    for (uint16_t i = 0; i < party_count; i++) {
        uint8_t member_id = game_state.party_members[i];
        char name_buf[8];
        if (member_id >= 1 && member_id <= 4) {
            for (int j = 0; j < 5; j++)
                name_buf[j] = eb_char_to_ascii(party_characters[member_id - 1].name[j]);
            name_buf[5] = '\0';
        } else {
            name_buf[0] = '\0';
        }
        add_menu_item(name_buf, member_id, (uint16_t)(i * 6), 0);
    }

    print_menu_items();

    /* Set cursor move callback from on_change parameter (assembly lines 112-116). */
    if (on_change) {
        set_cursor_move_callback(on_change);
    }
    return window_id;
}

/* char_select_prompt mode-1 epilogue (assembly lines 117-126 + the
 * @CLEANUP_AND_RETURN pagination reset): clear the cursor callback, close the
 * select window, restore text attributes. The argument_memory restore stays
 * with the caller (it captured the value). */
void char_select_overworld_finish(uint16_t window_id, bool had_on_change) {
    if (had_on_change) {
        clear_cursor_move_callback();
    }
    close_window(window_id);
    restore_window_text_attributes();

    /* Cleanup (@CLEANUP_AND_RETURN, lines 406-417). */
    dt.pagination_animation_frame = -1;
}

/* Build a GAME_MODE_CHAR_SELECT init for the battle-style path (mode 0/2),
 * replicating char_select_prompt()'s prologue: capture the focus window's
 * argument_memory (restored by the step before it pops) and the initial
 * character index (assembly lines 128-139: 0 for mode 2 or when no character was
 * previously selected, else the stored one). The initial on_change + pagination
 * reset run in the mode's CSP_INIT phase. Lets an already-converted parent
 * STEP_PUSH the char select directly instead of calling the blocking wrapper. */
void char_select_make_init(ModeState *init, uint16_t mode, uint16_t allow_cancel,
                           uint8_t on_change_id, uint8_t check_valid_id) {
    uint16_t current_index;
    if (win.battle_menu_current_character_id == -1 || mode == 2)
        current_index = 0;
    else
        current_index = (uint16_t)win.battle_menu_current_character_id;

    memset(init, 0, sizeof(*init));
    init->char_select.phase                 = CSP_INIT;
    init->char_select.mode                  = (uint8_t)mode;
    init->char_select.allow_cancel          = (uint8_t)allow_cancel;
    init->char_select.on_change_id          = on_change_id;
    init->char_select.check_valid_id        = check_valid_id;
    init->char_select.current_index         = current_index;
    init->char_select.delay                 = 10;
    init->char_select.counter               = 0;
    init->char_select.saved_argument_memory = get_argument_memory();
}

/*
 * INVENTORY_GET_ITEM_NAME (asm/misc/inventory_get_item_name.asm)
 *
 * Populates a window with the character's inventory items as menu options.
 * Creates the given window, sets character name as title, adds each non-empty
 * inventory slot as a menu item with an equipped marker prefix for equipped items,
 * then renders the menu in 2-column layout.
 *
 * char_id: 1-indexed character ID.
 * window_type: window ID to create (e.g., WINDOW_INVENTORY = 0x02).
 */
void inventory_get_item_name(uint16_t char_id, uint16_t window_type) {
    create_window(window_type);

    uint16_t char_idx = char_id - 1;

    /* Assembly lines 25-31: set PAGINATION_WINDOW if multi-party
     * (enables pagination arrow indicators on the window). */
    if ((game_state.player_controlled_party_count & 0xFF) > 1) {
        dt.pagination_window = window_type;
    }

    /* Assembly lines 32-43: SET_WINDOW_TITLE with character name (X=5 max chars). */
    {
        char name_buf[6];
        eb_to_ascii_buf(party_characters[char_idx].name, 5, name_buf);
        set_window_title(window_type, name_buf, 5);
    }

    /* Assembly lines 44-148: loop over ITEM_INVENTORY_SIZE (14) slots.
     * For each non-empty slot, check if equipped (US), copy item name from
     * ITEM_CONFIGURATION_TABLE, add as menu option. */
    for (uint16_t i = 0; i < ITEM_INVENTORY_SIZE; i++) {
        uint8_t item_id = party_characters[char_idx].items[i];
        if (item_id == 0) continue;

        const ItemConfig *item_entry = get_item_entry(item_id);
        if (!item_entry) continue;

        char label[MENU_LABEL_SIZE];
        int offset = 0;

        /* Check if this slot is equipped (US only).
         * Assembly uses CHAR::EQUIPPED ($22) as prefix character. */
        if (check_item_equipped(char_id, i + 1)) {
            label[0] = EB_CHAR_EQUIPPED;
            offset = 1;
        }

        /* Copy item name from config table (EB encoding → ASCII) */
        int j;
        for (j = 0; j < ITEM_NAME_LEN && (offset + j) < MENU_LABEL_SIZE - 1; j++) {
            if (item_entry->name[j] == 0x00) break;
            label[offset + j] = eb_char_to_ascii(item_entry->name[j]);
        }
        label[offset + j] = '\0';

        /* Assembly uses ADD_MENU_OPTION (no position);
         * OPEN_WINDOW_AND_PRINT_MENU calculates positions. */
        add_menu_item_no_position(label, i + 1);
    }

    /* Assembly lines 149-155: WINDOW_TICK_WITHOUT_INSTANT_PRINTING (US only),
     * then OPEN_WINDOW_AND_PRINT_MENU(columns=2, start_index=0). */
    window_tick_without_instant_printing();
    open_window_and_print_menu(2, 0);
}

/* ---- PSI ability table accessor (loaded from ROM binary asset) ---- */

const PsiAbility *battle_psi_table = NULL;
/* PSI table data and text arrays moved to battle_psi.c */

/*
 * CHECK_ALL_HPPP_METERS_STABLE (asm/battle/check_all_hppp_meters_stable.asm)
 *
 * Checks all player-controlled party members to see if their HP/PP rolling
 * meters have reached their target values. Returns 1 if all stable, 0 if
 * any member still has a rolling fraction or mismatched HP/PP.
 */
uint16_t check_all_hppp_meters_stable(void) {
    uint16_t count = game_state.player_controlled_party_count & 0xFF;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t member = game_state.party_members[i];
        CharStruct *ch = &party_characters[member - 1];
        if (ch->current_hp_fraction != 0) return 0;
        if (ch->current_pp_fraction != 0) return 0;
        if (ch->current_hp != ch->current_hp_target) return 0;
        if (ch->current_pp != ch->current_pp_target) return 0;
    }
    return 1;
}

/*
 * GET_EFFECTIVE_HPPP_METER_SPEED (asm/battle/effects/get_effective_hppp_meter_speed.asm)
 *
 * Returns bt.hp_meter_speed, halved (arithmetic shift right by 1) if
 * bt.half_hppp_meter_speed is nonzero.
 */
int32_t get_effective_hppp_meter_speed(void) {
    if (bt.half_hppp_meter_speed != 0)
        return bt.hp_meter_speed >> 1;
    return bt.hp_meter_speed;
}

/*
 * RESET_HPPP_METER_SPEED_IF_STABLE (asm/battle/reset_hppp_meter_speed_if_stable.asm)
 *
 * If all HP/PP meters are stable, resets fastest_hppp_meter_speed to 0.
 */
void reset_hppp_meter_speed_if_stable(void) {
    if (check_all_hppp_meters_stable())
        fastest_hppp_meter_speed = 0;
}

/*
 * HP_PP_ROLLER (asm/misc/hp_pp_roller.asm)
 *
 * Called each frame. Animates one party member's HP/PP toward target values
 * using 16.16 fixed-point arithmetic. The current_hp and current_hp_fraction
 * fields form a 32-bit fixed-point value: (current_hp << 16) | fraction.
 * Bit 0 of fraction is the "rolling active" flag.
 *
 * Speeds: normal HP uses get_effective_hppp_meter_speed(), normal PP uses
 * 0x00019000. Flipout/fastest modes use 0x00064000. In flipout mode,
 * HP oscillates between 999 and 1, PP between 999 and 0.
 */
void hp_pp_roller(void) {
    if (bt.disable_hppp_rolling & 0xFF)
        return;

    /* Process one party member per frame, cycling 0-3 */
    uint16_t slot = core.frame_counter & 3;
    uint8_t member = game_state.party_members[slot];
    if (member == 0)
        return;
    if (member >= 5)
        return;  /* skip NPCs */

    CharStruct *ch = &party_characters[member - 1];

    /* ---- HP section ---- */
    if (bt.hppp_meter_flipout_mode == 0) {
        /* Check if rolling is active (bit 0 of fraction) */
        if ((ch->current_hp_fraction & 1) == 0) {
            /* Not rolling — start if current != target */
            if (ch->current_hp != ch->current_hp_target)
                ch->current_hp_fraction = 1;
            goto pp_section;
        }
    }

    /* HP rolling is active */
    {
        uint16_t hp = ch->current_hp;
        uint16_t target = ch->current_hp_target;

        if (hp < target) {
            /* HP increasing */
            int32_t speed;
            if ((fastest_hppp_meter_speed & 0xFF) || bt.hppp_meter_flipout_mode)
                speed = 0x00064000;
            else
                speed = get_effective_hppp_meter_speed();

            uint32_t hp_fixed = ((uint32_t)ch->current_hp << 16) | ch->current_hp_fraction;
            hp_fixed += (uint32_t)speed;
            ch->current_hp_fraction = (uint16_t)(hp_fixed & 0xFFFF);
            ch->current_hp = (uint16_t)(hp_fixed >> 16);

            if (ch->current_hp >= target) {
                ch->current_hp = target;
                ch->current_hp_fraction = 1;  /* flag: rolling done */
            }
        } else if (hp == target && ch->current_hp_fraction == 1) {
            /* Reached target — clear rolling flag */
            ch->current_hp_fraction = 0;
        } else {
            /* HP decreasing */
            int32_t speed;
            if (bt.hppp_meter_flipout_mode)
                speed = 0x00064000;
            else
                speed = get_effective_hppp_meter_speed();

            uint32_t hp_fixed = ((uint32_t)ch->current_hp << 16) | ch->current_hp_fraction;
            hp_fixed -= (uint32_t)speed;
            ch->current_hp_fraction = (uint16_t)(hp_fixed & 0xFFFF);
            ch->current_hp = (uint16_t)(hp_fixed >> 16);

            /* Check if overshot or underflowed (hp > 1000 means wrap-around).
             * Assembly uses BLTEQ (unsigned <=) to skip clamp when hp <= 1000,
             * so clamp fires only when hp > 1000 (not >= 1000). */
            if (ch->current_hp < target || ch->current_hp > 1000) {
                ch->current_hp = target;
                ch->current_hp_fraction = 1;
            }
        }
    }

pp_section:
    /* ---- PP section ---- */
    if (bt.hppp_meter_flipout_mode == 0) {
        if ((ch->current_pp_fraction & 1) == 0) {
            if (ch->current_pp != ch->current_pp_target)
                ch->current_pp_fraction = 1;
            goto check_flipout;
        }
    }

    /* PP rolling is active */
    {
        uint16_t pp = ch->current_pp;
        uint16_t target = ch->current_pp_target;

        if (pp < target) {
            /* PP increasing */
            int32_t speed;
            if (bt.hppp_meter_flipout_mode)
                speed = 0x00064000;
            else
                speed = 0x00019000;  /* PP normal increase speed */

            uint32_t pp_fixed = ((uint32_t)ch->current_pp << 16) | ch->current_pp_fraction;
            pp_fixed += (uint32_t)speed;
            ch->current_pp_fraction = (uint16_t)(pp_fixed & 0xFFFF);
            ch->current_pp = (uint16_t)(pp_fixed >> 16);

            if (ch->current_pp >= target) {
                ch->current_pp = target;
                ch->current_pp_fraction = 1;
            }
        } else if (pp == target && ch->current_pp_fraction == 1) {
            ch->current_pp_fraction = 0;
        } else {
            /* PP decreasing */
            int32_t speed;
            if (bt.hppp_meter_flipout_mode)
                speed = 0x00064000;
            else
                speed = 0x00019000;  /* PP normal decrease speed */

            uint32_t pp_fixed = ((uint32_t)ch->current_pp << 16) | ch->current_pp_fraction;
            pp_fixed -= (uint32_t)speed;
            ch->current_pp_fraction = (uint16_t)(pp_fixed & 0xFFFF);
            ch->current_pp = (uint16_t)(pp_fixed >> 16);

            /* Same as HP: assembly uses BLTEQ so clamp fires only when pp > 1000. */
            if (ch->current_pp < target || ch->current_pp > 1000) {
                ch->current_pp = target;
                ch->current_pp_fraction = 1;
            }
        }
    }

check_flipout:
    if (bt.hppp_meter_flipout_mode == 0)
        return;

    /* Flipout mode: oscillate HP between 999 and 1 */
    if (ch->current_hp == 999)
        ch->current_hp_target = 1;
    else if (ch->current_hp == 1)
        ch->current_hp_target = 999;

    /* Flipout mode: oscillate PP between 999 and 0 */
    if (ch->current_pp == 999)
        ch->current_pp_target = 0;
    else if (ch->current_pp == 0)
        ch->current_pp_target = 999;
}

/*
 * WAIT_FOR_FADE_WITH_TICK (asm/battle/wait_for_fade_with_tick.asm)
 *
 * Loops until the current fade completes, calling window_tick()
 * each frame to keep windows/sprites updating.
 */
void wait_for_fade_with_tick(void) {
    /* Run-to-completion form: the former loop body (window_tick) is now the
     * GAME_MODE_FADE_WAIT step's FADE_TICK_WINDOW tick (window_tick_work, no
     * internal yield); pump_mode owns the single yield. See
     * docs/plans/savestate-unified-loop.md. */
    ModeState init = { .fade_wait = { .tick_kind = FADE_TICK_WINDOW } };
    pump_mode(GAME_MODE_FADE_WAIT, &init);
}
/*
 * FIND_NEXT_ENEMY_LETTER (asm/battle/find_next_enemy_letter.asm)
 *
 * Given an enemy group ID (battler::enemy_type_id), scans all conscious enemies
 * with matching group, marks their the_flag letters as used, then returns
 * the index+1 of the first unused letter (1-26), or 0 if all 26 are used.
 *
 * Used by FIX_ATTACKER_NAME/FIX_TARGET_NAME to determine if letter suffix
 * is needed: if result == 2, only one enemy of this type exists (letter A),
 * so the suffix is skipped.
 * (Uses find_next_enemy_letter() defined above.)
 */

/*
 * COPY_ENEMY_NAME (asm/text/copy_enemy_name.asm)
 *
 * Copies enemy name from source (enemy_data::name in ROM asset) to dest ert.buffer.
 * Handles NESS_PLACEHOLDER (0xAC) by substituting Ness's name from party_characters[0].
 * Stops at null byte or after 'length' characters.
 * Returns the position after the last character written.
 */
uint16_t copy_enemy_name(const uint8_t *src, uint8_t *dest, uint16_t length, uint16_t dest_size) {
    uint16_t pos = 0;
    for (uint16_t i = 0; i < length; i++) {
        uint8_t ch = src[i];
        if (ch == 0) break;
        if (ch == 0xAC) { /* CHAR::NESS_PLACEHOLDER (US) */
            for (uint16_t j = 0; j < sizeof(party_characters[0].name); j++) {
                uint8_t nc = party_characters[0].name[j];
                if (nc == 0) break;
                if (pos >= dest_size - 1) break;
                dest[pos++] = nc;
            }
        } else {
            if (pos >= dest_size - 1) break;
            dest[pos++] = ch;
        }
    }
    dest[pos] = 0;
    return pos;
}

#define ENEMY_MY_PET    160

/*
 * FIX_ATTACKER_NAME (asm/text/fix_attacker_name.asm)
 *
 * Sets up the attacker display name for battle messages.
 * If attacker is an enemy/NPC: copies enemy name from config table,
 * appends letter suffix (A, B, etc.) if needed for disambiguation.
 * If attacker is a party member: uses character name.
 * param: 0 = full name, nonzero = abbreviated (skip letter if only 1 enemy in battle).
 */
void fix_attacker_name(uint16_t param) {
    uint8_t scratch[ENEMY_NAME_SIZE + 2];
    memset(scratch, 0, sizeof(scratch));
    set_print_attacker_article(0);

    Battler *atk = battler_from_offset(bt.current_attacker);

    if (atk->ally_or_enemy == 1 || atk->npc_id != 0) {
        /* Enemy or NPC path */
        const EnemyData *edata = &enemy_config_table[atk->id];
        uint16_t pos = copy_enemy_name(edata->name, scratch, ENEMY_NAME_SIZE, sizeof(scratch));

        if (atk->ally_or_enemy == 1 && param == 0) {
            /* Full mode: check if letter suffix is needed */
            bool add_letter = true;
            if (atk->the_flag == 1) {
                uint8_t next_letter = find_next_enemy_letter(atk->enemy_type_id);
                if (next_letter == 2)
                    add_letter = false; /* Only one enemy of this type */
            }
            if (add_letter && atk->the_flag != 0) {
                scratch[pos++] = EB_CHAR_SPACE;
                set_print_attacker_article(1);
                scratch[pos] = atk->the_flag + EB_CHAR_A_MINUS_1;
            }
        }

        /* Special case: "My Pet" uses the player's pet name */
        if (atk->id == ENEMY_MY_PET) {
            memcpy(scratch, game_state.pet_name, sizeof(game_state.pet_name));
            scratch[sizeof(game_state.pet_name)] = 0;
        }

        set_battle_attacker_name((const char *)scratch,
                                 ENEMY_NAME_SIZE + sizeof(((CharStruct *)0)->name) - 3);
        set_attacker_enemy_id((int16_t)atk->id);
    } else {
        /* Party member path */
        if (atk->id <= 4) {
            CharStruct *ch = &party_characters[atk->row];
            set_battle_attacker_name((const char *)ch->name, sizeof(ch->name));
        }
    }
}

/*
 * FIX_TARGET_NAME (asm/text/fix_target_name.asm)
 *
 * Sets up the target display name for battle messages.
 * Same logic as FIX_ATTACKER_NAME but for the current target,
 * always uses full mode (no abbreviated param).
 */
void fix_target_name(void) {
    uint8_t scratch[ENEMY_NAME_SIZE + 2];
    memset(scratch, 0, sizeof(scratch));
    set_print_target_article(0);

    Battler *tgt = battler_from_offset(bt.current_target);

    if (tgt->ally_or_enemy == 1 || tgt->npc_id != 0) {
        /* Enemy or NPC path */
        const EnemyData *edata = &enemy_config_table[tgt->id];
        uint16_t pos = copy_enemy_name(edata->name, scratch, ENEMY_NAME_SIZE, sizeof(scratch));

        if (tgt->ally_or_enemy == 1) {
            /* Check if letter suffix is needed */
            bool add_letter = true;
            if (tgt->the_flag == 1) {
                uint8_t next_letter = find_next_enemy_letter(tgt->enemy_type_id);
                if (next_letter == 2)
                    add_letter = false;
            }
            if (add_letter && tgt->the_flag != 0) {
                scratch[pos++] = EB_CHAR_SPACE;
                set_print_target_article(1);
                scratch[pos] = tgt->the_flag + EB_CHAR_A_MINUS_1;
            }
        }

        /* Special case: "My Pet" */
        if (tgt->id == ENEMY_MY_PET) {
            memcpy(scratch, game_state.pet_name, sizeof(game_state.pet_name));
            scratch[sizeof(game_state.pet_name)] = 0;
        }

        set_battle_target_name((const char *)scratch, ENEMY_NAME_SIZE + 2);
        set_target_enemy_id((int16_t)tgt->id);
    } else {
        /* Party member path */
        if (tgt->id <= 4) {
            CharStruct *ch = &party_characters[tgt->row];
            set_battle_target_name((const char *)ch->name, sizeof(ch->name));
        }
    }
}
/*
 * APPLY_ACTION_TO_TARGETS (asm/battle/apply_action_to_targets.asm)
 *
 * Waits for PSI animation to finish, then iterates all targeted battlers
 * (enemies first indices 8+, then party 0-7), calling the action function
 * on each. If action is NULL, just iterates without calling.
 */
void apply_action_to_targets(battle_action_fn action) {
    /* Wait for PSI animation to finish */
    {
        ModeState init = { .battle_wait = { .kind = BW_PSI_ANIM } };
        pump_mode(GAME_MODE_BATTLE_WAIT, &init);
    }

    /* Process enemies first (indices 8 to BATTLER_COUNT-1) */
    bt.current_target = 8 * sizeof(Battler);
    for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
        if (battle_is_char_targeted(i)) {
            fix_target_name();
            if (action != NULL)
                action();
        }
        bt.current_target += sizeof(Battler);
    }

    /* Then process party members (indices 0 to 7) */
    bt.current_target = 0;
    for (uint16_t i = 0; i < 8; i++) {
        if (battle_is_char_targeted(i)) {
            fix_target_name();
            if (action != NULL)
                action();
        }
        bt.current_target += sizeof(Battler);
    }
}

/* ---- Helpers ---- */

/* rand_byte() and rand_limit() now in battle_internal.h */

/*
 * WAIT (port of asm/system/wait.asm)
 *
 * Loops for the given number of frames, calling window_tick() each
 * iteration to keep the UI responsive (text rendering, palette animation,
 * entity updates, etc.).  The assembly calls WINDOW_TICK.
 */
void battle_wait(uint16_t frames) {
    /* Run-to-completion form: window_tick() each frame is the GAME_MODE_BATTLE_
     * WAIT step's BW_FRAMES body (window_tick_work, no internal yield); pump_mode
     * owns the single yield. See docs/plans/savestate-unified-loop.md. */
    ModeState init = { .battle_wait = { .kind = BW_FRAMES, .remaining = frames } };
    pump_mode(GAME_MODE_BATTLE_WAIT, &init);
}

/* GAME_MODE_BATTLE_WAIT — run-to-completion port of the battle "advance one frame
 * until <condition>" loops. The single yield belongs to the pump; this step only
 * does one frame's work and reports CONTINUE/POP. See the header for the per-kind
 * body/condition table and the `primed` ordering notes. */
StepResult mode_step_battle_wait(ModeState *ms) {
    BattleWaitState *s = &ms->battle_wait;

    switch ((BattleWaitKind)s->kind) {
    case BW_FRAMES:
        /* for (i = frames; i != 0; i--) window_tick(); */
        if (s->remaining == 0)
            return STEP_RESULT_POP(0);
        window_tick_work();
        s->remaining--;
        return STEP_RESULT_CONTINUE();

    case BW_PSI_ANIM:
        /* while (is_psi_animation_active()) window_tick(); */
        if (!is_psi_animation_active())
            return STEP_RESULT_POP(0);
        window_tick_work();
        return STEP_RESULT_CONTINUE();

    case BW_SCREEN_EFFECT:
        /* while (bt.screen_effect_minimum_wait_frames) window_tick(); */
        if (!bt.screen_effect_minimum_wait_frames)
            return STEP_RESULT_POP(0);
        window_tick_work();
        return STEP_RESULT_CONTINUE();

    case BW_HPPP_STABLE:
        /* while (1) { window_tick(); reset_hppp_meter_speed_if_stable();
         *             if (check_all_hppp_meters_stable()) break; }
         * The blocking loop checks the exit AFTER the per-frame work and yield,
         * so the exit test sits at the top guarded by `primed` (skipped on the
         * first step, which has not yet yielded). */
        if (s->primed && check_all_hppp_meters_stable())
            return STEP_RESULT_POP(0);
        window_tick_work();
        reset_hppp_meter_speed_if_stable();
        s->primed = 1;
        return STEP_RESULT_CONTINUE();

    case BW_SWIRL_WINDOW:
        /* while (is_battle_swirl_active()) window_tick(); */
        if (!is_battle_swirl_active())
            return STEP_RESULT_POP(0);
        window_tick_work();
        return STEP_RESULT_CONTINUE();

    case BW_SWIRL_UPDATE:
        /* while (is_battle_swirl_active()) { wait_for_vblank(); update_swirl_effect(); }
         * The blocking loop yields BEFORE update_swirl_effect(), so the update is
         * deferred to the step that follows the prior yield (`primed`), preserving
         * the original yield/update interleave exactly. */
        if (s->primed)
            update_swirl_effect();
        if (!is_battle_swirl_active())
            return STEP_RESULT_POP(0);
        s->primed = 1;
        return STEP_RESULT_CONTINUE();
    }

    return STEP_RESULT_POP(0);
}

/* ======================================================================
 * HP / PP management
 * ====================================================================== */

/*
 * SET_HP (asm/battle/set_hp.asm)
 *
 * Parameters: A = target offset, X = new HP value
 * Clamps new_hp to [0, hp_max].
 * For player chars (ally_or_enemy==0, npc_id==0): updates hp_target and
 *   syncs to party_characters[row].current_hp_target.
 * For NPC allies (ally_or_enemy==0, npc_id!=0): sets both hp and hp_target,
 *   updates game_state.party_npc_X_hp.
 * For enemies (ally_or_enemy!=0): sets both hp and hp_target directly.
 */
void battle_set_hp(Battler *target, uint16_t new_hp) {
    /* Clamp to max */
    if (new_hp > target->hp_max)
        new_hp = target->hp_max;

    if (target->ally_or_enemy == 0) {
        if (target->npc_id == 0) {
            /* Player character — update hp_target; rolling meter handles display */
            target->hp_target = new_hp;
            /* Sync to char_struct */
            uint16_t char_idx = target->row;
            party_characters[char_idx].current_hp_target = new_hp;
        } else {
            /* NPC ally — set both hp and hp_target immediately */
            target->hp = new_hp;
            target->hp_target = new_hp;
            /* Update game_state NPC HP (row 0 = npc_1_hp, row 1 = npc_2_hp) */
            if (target->row == 0)
                game_state.party_npc_1_hp = new_hp;
            else
                game_state.party_npc_2_hp = new_hp;
        }
    } else {
        /* Enemy — set both directly */
        target->hp = new_hp;
        target->hp_target = new_hp;
    }
}

/*
 * SET_PP (asm/battle/set_pp.asm)
 *
 * Same structure as SET_HP but for PP (mana/PSI points).
 */
void battle_set_pp(Battler *target, uint16_t new_pp) {
    if (new_pp > target->pp_max)
        new_pp = target->pp_max;

    if (target->ally_or_enemy == 0) {
        if (target->npc_id == 0) {
            /* Player character */
            target->pp_target = new_pp;
            uint16_t char_idx = target->row;
            party_characters[char_idx].current_pp_target = new_pp;
        } else {
            /* NPC ally — NPCs have no separate PP tracking in game_state */
            target->pp = new_pp;
            target->pp_target = new_pp;
        }
    } else {
        /* Enemy */
        target->pp = new_pp;
        target->pp_target = new_pp;
    }
}

/*
 * REDUCE_HP (asm/battle/reduce_hp.asm)
 *
 * Parameters: A = target offset, X = damage amount
 * Subtracts damage from hp_target, floors at 0, applies via SET_HP.
 */
void battle_reduce_hp(Battler *target, uint16_t damage) {
    uint16_t new_hp;
    if (damage >= target->hp_target)
        new_hp = 0;
    else
        new_hp = target->hp_target - damage;
    battle_set_hp(target, new_hp);
}

/*
 * REDUCE_PP (asm/battle/reduce_pp.asm)
 *
 * Same as REDUCE_HP but for PP.
 */
void battle_reduce_pp(Battler *target, uint16_t cost) {
    uint16_t new_pp;
    if (cost >= target->pp_target)
        new_pp = 0;
    else
        new_pp = target->pp_target - cost;
    battle_set_pp(target, new_pp);
}

/*
 * RECOVER_HP (asm/battle/recover_hp.asm)
 *
 * Parameters: A = target offset, X = heal amount
 * Only works on conscious battlers (consciousness == 1).
 * Blocked if afflictions[0] == 1 (UNCONSCIOUS — heal blocked).
 * Displays battle text for full HP or partial recovery.
 */
void battle_recover_hp(Battler *target, uint16_t heal_amount) {
    /* Must be conscious */
    if (target->consciousness != 1)
        return;

    /* Heal blocked if afflictions[0] == UNCONSCIOUS */
    if (target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS) {
        /* Display "couldn't be healed" message */
        display_in_battle_text_addr(MSG_BTL4_RESULT_HEAL_NO_EFFECT);
        return;
    }

    uint16_t new_hp = target->hp_target + heal_amount;
    battle_set_hp(target, new_hp);

    if (new_hp >= target->hp_max) {
        /* Display "HP maxed out" message */
        display_in_battle_text_addr(MSG_BTL5_HP_MAXED_OUT);
    } else {
        /* Display "recovered X HP" message with heal_amount as parameter */
        display_text_wait_addr(MSG_BTL5_HP_RECOVERED, heal_amount);
    }
}

/*
 * RECOVER_PP (asm/battle/recover_pp.asm)
 *
 * Parameters: A = target offset, X = amount
 * Only works on conscious battlers.
 * Blocked if afflictions[0] == UNCONSCIOUS.
 * Caps recovery at pp_max, stores actual amount recovered for display.
 */
void battle_recover_pp(Battler *target, uint16_t amount) {
    if (target->consciousness != 1)
        return;

    if (target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS)
        return;

    /* Calculate actual recovery (capped at max) */
    uint16_t pp_cur = target->pp_target;
    uint16_t pp_max = target->pp_max;
    uint16_t actual_recovery;
    if (pp_cur + amount >= pp_max)
        actual_recovery = pp_max - pp_cur;
    else
        actual_recovery = amount;

    uint16_t new_pp = pp_cur + amount;
    battle_set_pp(target, new_pp);

    /* Display "recovered X PP" message */
    display_text_wait_addr(MSG_BTL5_PP_RECOVERED, actual_recovery);
}

/* ======================================================================
 * Status effects
 * ====================================================================== */

/*
 * INFLICT_STATUS_BATTLE (asm/battle/inflict_status.asm)
 *
 * Parameters: A = target offset, X = status_group, Y = status_value
 * Returns 1 if status was applied, 0 if blocked.
 * Blocked if: target is NPC (npc_id != 0), or status already active with
 * equal/higher value.
 */
uint16_t battle_inflict_status(Battler *target, uint16_t status_group,
                                uint16_t status_value) {
    /* NPCs can't be afflicted */
    if (target->npc_id != 0)
        return 0;

    uint8_t current = target->afflictions[status_group];
    if (current != 0) {
        /* Status already active — don't overwrite worse (lower value) with milder.
         * Assembly (BLTEQ): skip when current <= status_value (i.e. keep worse). */
        if (current <= (uint8_t)status_value)
            return 0;
    }

    /* Apply the status */
    target->afflictions[status_group] = (uint8_t)status_value;
    return 1;
}

/*
 * COUNT_CHARS (asm/battle/count_chars.asm)
 *
 * Counts alive, non-NPC battlers on the given side that are not
 * unconscious or diamondized.
 * side: 0 = party (ally_or_enemy == 0), 1 = enemies (ally_or_enemy == 1).
 */
uint16_t battle_count_chars(uint16_t side) {
    uint16_t count = 0;
    for (int i = 0; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0)
            continue;
        if (b->ally_or_enemy != (uint8_t)side)
            continue;
        if (b->npc_id != 0)
            continue;
        /* Exclude unconscious (1) and diamondized (2) */
        uint8_t aff = b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
        if (aff == STATUS_0_UNCONSCIOUS || aff == STATUS_0_DIAMONDIZED)
            continue;
        count++;
    }
    return count;
}

/* ======================================================================
 * KO / Revive
 * ====================================================================== */

/*
 * KO_TARGET (asm/battle/ko_target.asm)
 *
 * Handles the death of a battler. This is one of the most complex battle
 * functions with many branches for different battler types.
 *
 * For player characters:
 *   - Sets all afflictions, zero HP target
 *   - Syncs to char_struct
 *   - Displays KO text
 *
 * For enemies:
 *   - Skips Giygas phases (they can't be KO'd normally)
 *   - Checks if last enemy alive → ensures all player chars have >= 1 HP
 *   - Accumulates EXP and money rewards
 *   - Executes final attack if defined
 *   - Death animation (fade white → fade black)
 *   - death_type processing (group death for boss-type enemies)
 *   - Ghost/possession mechanics
 */
void battle_ko_target(Battler *target) {
    uint16_t target_offset = battler_to_offset(target);
    bt.skip_death_text_and_cleanup = 0;

    if (target->ally_or_enemy != 0) {
        /* ---- ENEMY KO ---- */

        /* Skip for certain Giygas phases — they can't be killed normally */
        uint16_t enemy_id = target->id;
        if (enemy_id == ENEMY_GIYGAS_2 || enemy_id == ENEMY_GIYGAS_3 ||
            enemy_id == ENEMY_GIYGAS_5 || enemy_id == ENEMY_GIYGAS_6)
            return;

        /* Check if this is the last enemy — if so, ensure players have >= 1 HP */
        if (battle_count_chars(1) == 1) {
            reset_hppp_rolling();
            for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
                Battler *b = &bt.battlers_table[i];
                if (b->consciousness == 0)
                    continue;
                if (b->ally_or_enemy != 0)
                    continue;
                uint8_t easyheal = b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
                if (easyheal == STATUS_0_UNCONSCIOUS)
                    continue;
                if (b->npc_id != 0)
                    continue;
                /* Check char_struct current_hp — if 0, set to 1 */
                uint8_t char_row = b->row;
                if (party_characters[char_row].current_hp == 0) {
                    b->hp_target = 1;
                    party_characters[char_row].current_hp_target = 1;
                }
            }
        }

        /* Accumulate EXP and money rewards */
        bt.battle_exp_scratch += target->exp;
        bt.battle_money_scratch += target->money;

        /* Check for final attack */
        if (enemy_config_table != NULL) {
            const EnemyData *edata = &enemy_config_table[target->id];
            if (edata->final_action != 0) {
                bt.enemy_performing_final_attack = 1;

                /* Save current attacker/target state */
                uint16_t saved_attacker = bt.current_attacker;
                uint16_t saved_target = bt.current_target;
                uint32_t saved_target_flags = bt.battler_target_flags;

                /* Set dying enemy as attacker for final attack */
                bt.current_attacker = target_offset;
                target->current_action = edata->final_action;
                target->current_action_argument = edata->final_action_arg;

                /* Choose target and execute final attack */
                choose_target(target_offset);
                set_battler_targets_by_action(target_offset);
                fix_attacker_name(0);
                set_target_if_targeted();

                /* Display final attack description text */
                if (battle_action_table) {
                    uint32_t desc_addr = battle_action_table[edata->final_action].description_text_pointer;
                    if (desc_addr != 0) {
                        display_text_with_prompt_addr(desc_addr);
                    }
                }

                /* Execute final attack function via dispatch table.
                 * Assembly: passes function pointer to APPLY_ACTION_TO_TARGETS,
                 * which calls it once per targeted battler. */
                {
                    battle_action_fn final_fn = NULL;
                    if (battle_action_table && edata->final_action != 0) {
                        uint32_t func_ptr = battle_action_table[edata->final_action].battle_function_pointer;
                        if (func_ptr != 0) {
                            bt.temp_function_pointer = func_ptr;
                            final_fn = jump_temp_function_pointer;
                        }
                    }
                    apply_action_to_targets(final_fn);
                }
                bt.enemy_performing_final_attack = 0;

                /* Restore attacker/target state */
                bt.current_attacker = saved_attacker;
                bt.current_target = saved_target;
                bt.battler_target_flags = saved_target_flags;
                fix_attacker_name(0);
                fix_target_name();

                if (bt.special_defeat != 0)
                    return;
            }
        }

        /* Show death text (if not skipped) */
        if (bt.skip_death_text_and_cleanup == 0) {
            /* Display enemy death text from enemy_config_table */
            if (enemy_config_table != NULL) {
                uint32_t death_addr = enemy_config_table[target->id].death_text_ptr;
                if (death_addr != 0) {
                    display_in_battle_text_addr(death_addr);
                }
            }
        }

        /* Clear alt spritemap for all battlers, then set it for dying enemy */
        for (int i = 0; i < BATTLER_COUNT; i++)
            bt.battlers_table[i].use_alt_spritemap = 0;
        target->use_alt_spritemap = 1;

        /* Death animation: fade to white */
        set_battle_sprite_palette_effect_speed(10);
        for (int pal = 1; pal < 16; pal++) {
            uint16_t pal_offset = (uint16_t)target->vram_sprite_index * 16 + pal;
            setup_battle_sprite_palette_effect(pal_offset, 31, 31, 31);
        }
        battle_wait(SIXTH_OF_A_SECOND);

        /* Fade to black */
        set_battle_sprite_palette_effect_speed(20);
        for (int pal = 1; pal < 16; pal++) {
            uint16_t pal_offset = (uint16_t)target->vram_sprite_index * 16 + pal;
            setup_battle_sprite_palette_effect(pal_offset, 0, 0, 0);
        }
        battle_wait(THIRD_OF_A_SECOND);

        /* Apply KO status */
        target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = STATUS_0_UNCONSCIOUS;
        target->afflictions[STATUS_GROUP_SHIELD] = 0;
        target->afflictions[STATUS_GROUP_HOMESICKNESS] = 0;
        target->afflictions[STATUS_GROUP_CONCENTRATION] = 0;
        target->afflictions[STATUS_GROUP_STRANGENESS] = 0;
        target->afflictions[STATUS_GROUP_TEMPORARY] = 0;
        target->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] = 0;
        target->hp_target = 0;

        /* Check death_type for group death */
        if (enemy_config_table != NULL) {
            const EnemyData *edata = &enemy_config_table[target->id];
            if (edata->death_type != 0) {
                /* Group death — set all remaining enemies to alt spritemap */
                for (int i = FIRST_ENEMY_INDEX; i < BATTLER_COUNT; i++) {
                    if (bt.battlers_table[i].consciousness != 0)
                        bt.battlers_table[i].use_alt_spritemap = 1;
                }

                /* Play death SFX (assembly: ko_target.asm line 710) */
                play_sfx(33);  /* SFX::ENEMY_DEFEATED */

                /* Group flash white animation */
                set_battle_sprite_palette_effect_speed(10);
                for (int pal = 1; pal < 64; pal++) {
                    if ((pal & 15) == 0) continue; /* Skip every 16th */
                    setup_battle_sprite_palette_effect(pal, 31, 31, 31);
                }
                battle_wait(SIXTH_OF_A_SECOND);

                /* Group fade to black */
                set_battle_sprite_palette_effect_speed(20);
                for (int pal = 1; pal < 64; pal++) {
                    if ((pal & 15) == 0) continue;
                    setup_battle_sprite_palette_effect(pal, 0, 0, 0);
                }
                battle_wait(20);

                /* KO all remaining enemies */
                for (int i = FIRST_ENEMY_INDEX; i < BATTLER_COUNT; i++) {
                    if (bt.battlers_table[i].consciousness != 0)
                        bt.battlers_table[i].afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = STATUS_0_UNCONSCIOUS;
                }
                render_all_battle_sprites();
                bt.special_defeat = 2;
            }
        }

        /* Ghost/possession: if the dead enemy is TINY_LIL_GHOST,
         * cure possession from party members and potentially respawn ghost */
        if (target->npc_id == ENEMY_TINY_LIL_GHOST) {
            /* Cure possession from any party member */
            for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
                Battler *b = &bt.battlers_table[i];
                if (b->consciousness == 0) continue;
                if (b->npc_id != 0) continue;
                if (b->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] == STATUS_1_POSSESSED) {
                    b->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] = 0;
                    break; /* Only cure one */
                }
            }

            /* Check if any party member is still possessed — respawn ghost */
            for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
                Battler *b = &bt.battlers_table[i];
                if (b->consciousness == 0) continue;
                if (b->npc_id != 0) continue;
                if (b->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] == STATUS_1_POSSESSED) {
                    /* Respawn ghost in slot 6 (first NPC enemy slot) */
                    battle_init_enemy_stats(&bt.battlers_table[TOTAL_PARTY_COUNT],
                                            ENEMY_TINY_LIL_GHOST);
                    bt.battlers_table[TOTAL_PARTY_COUNT].npc_id = ENEMY_TINY_LIL_GHOST;
                    bt.battlers_table[TOTAL_PARTY_COUNT].has_taken_turn = 1;
                    break;
                }
            }
        }

    } else {
        /* ---- PLAYER / NPC KO ---- */

        /* Check for possession — possessed chars that die have special handling.
         * Assembly (ko_target.asm:27-62): when dying target IS possessed,
         * kill the ghost and respawn it for any OTHER possessed member. */
        if (target->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] == STATUS_1_POSSESSED) {
            /* Kill the ghost enemy if it's TINY_LIL_GHOST */
            if (bt.battlers_table[TOTAL_PARTY_COUNT].npc_id == ENEMY_TINY_LIL_GHOST) {
                bt.battlers_table[TOTAL_PARTY_COUNT].consciousness = 0;

                /* Respawn ghost for another possessed party member (start from dying target's slot) */
                uint16_t target_slot = (uint16_t)(target - bt.battlers_table);
                for (int j = 0; j < TOTAL_PARTY_COUNT; j++) {
                    uint16_t idx = (target_slot + j) % TOTAL_PARTY_COUNT;
                    Battler *b2 = &bt.battlers_table[idx];
                    if (b2 == target) continue;
                    if (b2->consciousness == 0) continue;
                    if (b2->npc_id != 0) continue;
                    if (b2->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] == STATUS_1_POSSESSED) {
                        battle_init_enemy_stats(&bt.battlers_table[TOTAL_PARTY_COUNT],
                                                ENEMY_TINY_LIL_GHOST);
                        bt.battlers_table[TOTAL_PARTY_COUNT].npc_id = ENEMY_TINY_LIL_GHOST;
                        bt.battlers_table[TOTAL_PARTY_COUNT].consciousness = 1;
                        break;
                    }
                }
            }
        }

        /* Set UNCONSCIOUS status and clear all other afflictions */
        target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = STATUS_0_UNCONSCIOUS;
        target->afflictions[STATUS_GROUP_SHIELD] = 0;
        target->afflictions[STATUS_GROUP_HOMESICKNESS] = 0;
        target->afflictions[STATUS_GROUP_CONCENTRATION] = 0;
        target->afflictions[STATUS_GROUP_STRANGENESS] = 0;
        target->afflictions[STATUS_GROUP_TEMPORARY] = 0;
        target->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] = 0;

        if (target->npc_id != 0) {
            /* NPC enemy ally death — look up death text from enemy_config_table */
            if (enemy_config_table != NULL) {
                uint32_t death_addr = enemy_config_table[target->id].death_text_ptr;
                if (death_addr != 0) {
                    display_in_battle_text_addr(death_addr);
                }
            }
            target->consciousness = 0;

            /* Check for teddy bear replacement */
            if (target->npc_id == PARTY_MEMBER_TEDDY_BEAR ||
                target->npc_id == PARTY_MEMBER_PLUSH_TEDDY_BEAR) {
                /* Check if a replacement NPC is available */
                uint8_t npc_slot = target->row;
                uint8_t replacement_npc;
                if (npc_slot == 0)
                    replacement_npc = game_state.party_npc_1;
                else
                    replacement_npc = game_state.party_npc_2;

                if (replacement_npc != 0) {
                    /* Revive with replacement NPC */
                    target->consciousness = 1;
                    target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;

                    /* Load replacement NPC HP */
                    uint16_t npc_hp;
                    if (npc_slot == 0)
                        npc_hp = game_state.party_npc_1_hp;
                    else
                        npc_hp = game_state.party_npc_2_hp;
                    target->hp_target = npc_hp;
                    target->hp = npc_hp;
                    target->npc_id = replacement_npc;

                    /* Look up AI type from NPC_AI_TABLE (assembly lines 222-232) */
                    if (npc_ai_table) {
                        target->id = npc_ai_table[replacement_npc * 2 + 1];
                    }
                }
            } else {
                /* Non-teddy NPC death — reassign front row if needed */
                if (game_state.party_npc_1 != 0) {
                    for (int i = 0; i < BATTLER_COUNT; i++) {
                        Battler *b = &bt.battlers_table[i];
                        if (b->consciousness == 0) continue;
                        if (b->ally_or_enemy != 0) continue;
                        if (b->npc_id == game_state.party_npc_1) {
                            b->row = 0;
                            break;
                        }
                    }
                }
            }
        } else {
            /* Player character KO */
            target->hp_target = 0;
            uint8_t char_row = target->row;
            party_characters[char_row].current_hp_target = 0;
            party_characters[char_row].current_hp = 1;

            /* Display KO text */
            display_in_battle_text_addr(MSG_BTL5_ALLY_COLLAPSED);
        }
    }
}

/*
 * REVIVE_TARGET (asm/battle/revive_target.asm)
 *
 * Revives a KO'd battler with specified HP amount.
 * Clears all afflictions, resets action, displays revive message.
 * For enemies: palette fade animation (zero → white → restore).
 */
void battle_revive_target(Battler *target, uint16_t hp) {
    /* Display revive text */
    display_in_battle_text_addr(MSG_BTL5_REVIVED);

    /* Clear all afflictions */
    target->afflictions[STATUS_GROUP_SHIELD] = 0;
    target->afflictions[STATUS_GROUP_HOMESICKNESS] = 0;
    target->afflictions[STATUS_GROUP_CONCENTRATION] = 0;
    target->afflictions[STATUS_GROUP_STRANGENESS] = 0;
    target->afflictions[STATUS_GROUP_TEMPORARY] = 0;
    target->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] = 0;
    target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;

    /* Reset action and mark as having taken turn */
    target->current_action = 0;
    target->has_taken_turn = 1;

    /* Set HP */
    battle_set_hp(target, hp);

    /* For player characters: sync HP to char_struct */
    if (target->ally_or_enemy == 0 && target->npc_id == 0) {
        uint8_t char_row = target->row;
        party_characters[char_row].current_hp_target = hp;
        party_characters[char_row].current_hp = 1;
    }

    /* For enemies: palette animation (appears only for enemy revives) */
    if (target->ally_or_enemy != 0 && target->npc_id == 0) {
        /* Clear all battler alt spritemaps, set it for this one */
        for (int i = 0; i < BATTLER_COUNT; i++)
            bt.battlers_table[i].use_alt_spritemap = 0;
        target->use_alt_spritemap = 1;

        /* Zero palette bank 12 entries 1-15 for this sprite (assembly lines 119-140) */
        for (int pal = 1; pal < 16; pal++) {
            uint16_t pal_word_index = (uint16_t)target->vram_sprite_index * 16 + pal;
            ert.palettes[12 * 16 + pal_word_index] = 0;
        }

        /* Fade to white (assembly lines 142-170) */
        set_battle_sprite_palette_effect_speed(10);
        for (int pal = 1; pal < 16; pal++) {
            uint16_t pal_offset = (uint16_t)target->vram_sprite_index * 16 + pal;
            setup_battle_sprite_palette_effect(pal_offset, 31, 31, 31);
        }
        battle_wait(SIXTH_OF_A_SECOND);

        /* Restore original palette from palette bank 8 (assembly lines 171-223) */
        set_battle_sprite_palette_effect_speed(20);
        for (int pal = 1; pal < 16; pal++) {
            uint16_t pal_word_index = (uint16_t)target->vram_sprite_index * 16 + pal;
            uint16_t color = ert.palettes[8 * 16 + pal_word_index];
            uint16_t red = color & 0x1F;
            uint16_t green = (color >> 5) & 0x1F;
            uint16_t blue = (color >> 10) & 0x1F;
            setup_battle_sprite_palette_effect(pal_word_index, red, green, blue);
        }
        battle_wait(THIRD_OF_A_SECOND * 2);
    }
}

/*
 * DEAD_TARGETTABLE_ACTIONS table (asm/data/battle/dead_targettable_actions.asm)
 *
 * Actions that can target dead/unconscious battlers (healing, revival, etc.)
 * Zero-terminated list of action IDs.
 */
const uint16_t dead_targettable_actions[] = {
    BATTLE_ACTION_PSI_LIFEUP_ALPHA,
    BATTLE_ACTION_PSI_LIFEUP_BETA,
    BATTLE_ACTION_PSI_LIFEUP_GAMMA,
    BATTLE_ACTION_PSI_LIFEUP_OMEGA,
    BATTLE_ACTION_PSI_HEALING_ALPHA,
    BATTLE_ACTION_PSI_HEALING_BETA,
    BATTLE_ACTION_PSI_HEALING_GAMMA,
    BATTLE_ACTION_PSI_HEALING_OMEGA,
    BATTLE_ACTION_ACTION_135,
    BATTLE_ACTION_ACTION_136,
    BATTLE_ACTION_ACTION_137,
    BATTLE_ACTION_ACTION_138,
    BATTLE_ACTION_ACTION_139,
    BATTLE_ACTION_HAND_AID,
    BATTLE_ACTION_ACTION_141,
    BATTLE_ACTION_ACTION_142,
    BATTLE_ACTION_ACTION_143,
    BATTLE_ACTION_ACTION_144,
    BATTLE_ACTION_ACTION_145,
    BATTLE_ACTION_ACTION_146,
    BATTLE_ACTION_ACTION_147,
    BATTLE_ACTION_ACTION_148,
    BATTLE_ACTION_WET_TOWEL,
    BATTLE_ACTION_REFRESHING_HERB,
    BATTLE_ACTION_SECRET_HERB,
    BATTLE_ACTION_FULL_STATUS_HEAL,
    BATTLE_ACTION_ACTION_153,
    BATTLE_ACTION_ACTION_154,
    BATTLE_ACTION_ACTION_155,
    BATTLE_ACTION_ACTION_156,
    BATTLE_ACTION_ACTION_157,
    BATTLE_ACTION_ACTION_158,
    0  /* terminator */
};

/* ======================================================================
 * Enemy lookup and misc battle utilities
 * ====================================================================== */

/*
 * GET_ENEMY_TYPE (asm/battle/get_enemy_type.asm)
 *
 * Returns the type field from the enemy configuration table for a given enemy ID.
 */
uint16_t battle_get_enemy_type(uint16_t enemy_id) {
    return (uint16_t)enemy_config_table[enemy_id].type;
}

/* ======================================================================
 * Post-battle stat reset
 * ====================================================================== */

/*
 * RESET_POST_BATTLE_STATS (asm/battle/reset_post_battle_stats.asm)
 *
 * After battle ends, clears temporary afflictions (groups 2-4, 6) from
 * party characters. Does not clear persistent afflictions (groups 0-1)
 * or homesickness (group 5).
 */
void battle_reset_post_battle_stats(void) {
    for (uint16_t i = 0; i < TOTAL_PARTY_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0)
            continue;
        if (b->ally_or_enemy != 0)
            continue;
        if (b->npc_id != 0)
            continue;

        /* b->row is the 0-based character index */
        uint8_t char_idx = b->row;
        CharStruct *cs = &party_characters[char_idx];
        cs->afflictions[STATUS_GROUP_SHIELD] = 0;
        cs->afflictions[STATUS_GROUP_CONCENTRATION] = 0;
        cs->afflictions[STATUS_GROUP_STRANGENESS] = 0;
        cs->afflictions[STATUS_GROUP_TEMPORARY] = 0;
    }
}

/*
 * BOSS_BATTLE_CHECK (asm/battle/boss_battle_check.asm)
 *
 * Scans all conscious enemies; if any has the "boss" flag set in
 * enemy_config_table, returns 0 (is a boss battle).
 * Returns 1 if no bosses found (can run away).
 */
uint16_t battle_boss_battle_check(void) {
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0)
            continue;
        if (b->ally_or_enemy != 1)
            continue;
        if (enemy_config_table[b->id].boss != 0)
            return 0;  /* boss found — can't run */
    }
    return 1;  /* no bosses — can run */
}

/*
 * FIND_FIRST_UNCONSCIOUS_PARTY_SLOT (asm/battle/find_first_unconscious_party_slot.asm)
 *
 * Searches party_order for the first member whose affliction is
 * exactly 1 (unconscious). Returns the index into party_order (0-based).
 * If no unconscious member found, returns player_controlled_party_count.
 */
uint16_t find_first_unconscious_party_slot(void) {
    uint16_t i = 0;
    while (i < (game_state.player_controlled_party_count & 0xFF)) {
        uint8_t member = game_state.party_order[i];
        uint16_t char_idx = (member & 0xFF) - 1;
        uint16_t affliction = party_characters[char_idx].afflictions[0] & 0xFF;
        if (affliction == 1) /* unconscious */
            break;
        i++;
    }
    return i;
}

/*
 * FIND_FIRST_ALIVE_PARTY_MEMBER (asm/battle/find_first_alive_party_member.asm)
 *
 * Searches party_order for the first member whose affliction is NOT
 * unconscious (1) or diamondized (2). Returns the 1-based character ID.
 * Returns 0 if all party members are dead/diamondized.
 */
uint16_t find_first_alive_party_member(void) {
    uint16_t count = game_state.player_controlled_party_count & 0xFF;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t member_id = game_state.party_order[i] & 0xFF;
        uint16_t char_idx = member_id - 1;
        uint8_t affliction = party_characters[char_idx].afflictions[0] & 0xFF;
        if (affliction != 1 && affliction != 2) {
            return member_id;
        }
    }
    return 0;
}

/*
 * REPLACE_BOSS_BATTLER (asm/battle/replace_boss_battler.asm)
 *
 * Replaces the boss battler (slot 8, first enemy) with a new enemy type.
 * Preserves sprite_x and sprite_y position, reinitializes all stats.
 * Sets has_taken_turn to 1 so the replacement doesn't act immediately.
 */
void replace_boss_battler(uint16_t new_enemy_id) {
    Battler *boss = &bt.battlers_table[FIRST_ENEMY_INDEX]; /* slot 8 */
    uint8_t saved_x = boss->sprite_x;
    uint8_t saved_y = boss->sprite_y;

    battle_init_enemy_stats(boss, new_enemy_id);

    boss->sprite_x = saved_x;
    boss->sprite_y = saved_y;
    boss->has_taken_turn = 1;
}

/* ======================================================================
 * Mirror (Metamorphose) system
 * ====================================================================== */

/*
 * COPY_MIRROR_DATA (asm/battle/copy_mirror_data.asm)
 *
 * Copies battler data from source to dest, preserving certain fields from dest:
 * hp, pp, hp_target, pp_target, hp_max, pp_max, ally_or_enemy, row, id, has_taken_turn.
 * This lets an enemy copy the target's stats/abilities while keeping its own
 * identity and HP/PP.
 */
void battle_copy_mirror_data(Battler *dest, const Battler *source) {
    /* Save dest fields that must be preserved */
    uint16_t saved_hp = dest->hp;
    uint16_t saved_pp = dest->pp;
    uint16_t saved_hp_target = dest->hp_target;
    uint16_t saved_pp_target = dest->pp_target;
    uint16_t saved_hp_max = dest->hp_max;
    uint16_t saved_pp_max = dest->pp_max;
    uint8_t  saved_ally_or_enemy = dest->ally_or_enemy;
    uint8_t  saved_row = dest->row;
    uint16_t saved_id = dest->id;
    uint8_t  saved_has_taken_turn = dest->has_taken_turn;

    /* Copy entire battler struct from source */
    memcpy(dest, source, sizeof(Battler));

    /* Restore preserved fields */
    dest->hp = saved_hp;
    dest->pp = saved_pp;
    dest->hp_target = saved_hp_target;
    dest->pp_target = saved_pp_target;
    dest->hp_max = saved_hp_max;
    dest->pp_max = saved_pp_max;
    dest->ally_or_enemy = saved_ally_or_enemy;
    dest->row = saved_row;
    dest->id = saved_id;
    dest->has_taken_turn = saved_has_taken_turn;
}

/*
 * APPLY_NEUTRALIZE_TO_ALL (asm/battle/apply_neutralize_to_all.asm)
 *
 * If mirror (metamorphose) is active, finds the mirrored Poo battler,
 * restores original stats from bt.mirror_battler_backup, clears mirror state.
 * Then targets all conscious battlers and applies btlact_neutralize to each.
 */
void apply_neutralize_to_all(void) {
    /* If mirror is active, reverse metamorphosis first */
    if (bt.mirror_enemy != 0) {
        for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
            Battler *b = &bt.battlers_table[i];
            if (b->consciousness == 0) continue;
            if (b->ally_or_enemy != 0) continue;
            if (b->id != PARTY_MEMBER_POO) continue;

            bt.mirror_enemy = 0;
            battle_copy_mirror_data(b, &bt.mirror_battler_backup);
            b->current_action = 0;
            display_in_battle_text_addr(MSG_BTL5_MORPH_NEUTRALIZED);
            break;
        }
    }

    battle_target_all();
    battle_remove_dead_targeting();
    apply_action_to_targets(btlact_neutralize);
    bt.battler_target_flags = 0;
}

/*
 * FIND_STEALABLE_ITEMS (asm/battle/find_stealable_items.asm)
 *
 * Scans all party member inventories for items that can be stolen by enemies.
 * An item is stealable if:
 *   - Character ID is 1-4 (playable character, not NPC)
 *   - Item is not the one being used this turn (action_item_slot)
 *   - Item is not empty (ID != 0)
 *   - Item cost is > 0 and < 290
 *   - Item type bits 4-5 == 0x20 (equipment category)
 *   - Item is not currently equipped
 * Results are stored in stealable_item_candidates[]. Returns the count.
 */
#define MAX_STEALABLE_ITEMS (14 * 4)  /* 14 slots per char * 4 PCs */
uint8_t stealable_item_candidates[MAX_STEALABLE_ITEMS];

/* ======================================================================
 * Return battle name addresses
 * ====================================================================== */

/*
 * RETURN_BATTLE_ATTACKER_ADDRESS (asm/battle/return_battle_attacker_address.asm)
 *
 * Returns pointer to the BATTLE_ATTACKER_NAME ert.buffer.
 */
char *return_battle_attacker_address(void) {
    return dt.battle_attacker_name;
}

/*
 * RETURN_BATTLE_TARGET_ADDRESS (asm/battle/return_battle_target_address.asm)
 *
 * Returns pointer to the BATTLE_TARGET_NAME ert.buffer.
 */
char *return_battle_target_address(void) {
    return dt.battle_target_name;
}

/*
 * GIYGAS_HURT_PRAYER (asm/battle/giygas_hurt_prayer.asm)
 *
 * Applies prayer damage to Giygas (always battler slot 8, the first enemy).
 * Waits 1 second, sets target, applies 25% variance, green flash, SMAAAASH
 * flag, calc resist damage, then waits another second.
 */
void giygas_hurt_prayer(uint16_t base_damage) {
    battle_wait(FRAMES_PER_SECOND);

    /* Target Giygas (slot 8 = first enemy) */
    bt.current_target = FIRST_ENEMY_INDEX * sizeof(Battler);
    fix_target_name();

    bt.green_flash_duration = FRAMES_PER_SECOND;
    bt.is_smaaaash_attack = 1;

    uint16_t damage = battle_25pct_variance(base_damage);
    battle_calc_resist_damage(damage, 0xFF);

    battle_wait(FRAMES_PER_SECOND);
}

/* ======================================================================
 * PSI Flash sub-effects
 * ====================================================================== */

/*
 * FLASH_INFLICT_CRYING (asm/battle/actions/psi_flash_crying.asm)
 *
 * Direct crying infliction from PSI Flash (no NPC or resist check).
 */
void flash_inflict_crying(void) {
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_2_CRYING, STATUS_2_CRYING);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_CRYING);
    } else {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
    }
}

/*
 * FLASH_INFLICT_PARALYSIS (asm/battle/actions/psi_flash_paralysis.asm)
 *
 * Direct paralysis infliction from PSI Flash (no NPC or resist check).
 */
void flash_inflict_paralysis(void) {
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_PARALYZED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_NUMB);
    } else {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
    }
}

/*
 * FLASH_INFLICT_FEELING_STRANGE (asm/battle/actions/psi_flash_feeling_strange.asm)
 *
 * Direct "feeling strange" infliction from PSI Flash (no NPC or resist check).
 */
void flash_inflict_feeling_strange(void) {
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_STRANGENESS, STATUS_3_STRANGE);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_STRANGE);
    } else {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
    }
}

/* ======================================================================
 * Battle entry points
 * ====================================================================== */

/* ---- Battle action ID constants (from include/constants/actions.asm) ---- */
#define BACT_NO_EFFECT         0
#define BACT_USE_NO_EFFECT     1
#define BACT_BASH              4
#define BACT_SHOOT             5
#define BACT_SPY               6
#define BACT_PRAY              7
#define BACT_GUARD             8
#define BACT_PSI_LIFEUP_ALPHA  32
#define BACT_PSI_LIFEUP_BETA   33
#define BACT_PSI_LIFEUP_GAMMA  34
#define BACT_PSI_LIFEUP_OMEGA  35
#define BACT_PSI_HEALING_ALPHA 36
#define BACT_PSI_HEALING_BETA  37
#define BACT_PSI_HEALING_GAMMA 38
#define BACT_PSI_HEALING_OMEGA 39
#define BACT_STEAL             66
#define BACT_ON_GUARD          103
#define BACT_ENEMY_EXTENDER    245
#define BACT_RUN_AWAY          279
#define BACT_MIRROR            280
#define BACT_ACTION_251        251
#define BACT_ACTION_252        252
#define BACT_ACTION_253        253
#define BACT_ACTION_254        254
#define BACT_ACTION_255        255
#define BACT_ACTION_256        256
#define BACT_FINAL_PRAYER_1    291
#define BACT_FINAL_PRAYER_2    292
#define BACT_FINAL_PRAYER_3    293
#define BACT_FINAL_PRAYER_4    294
#define BACT_FINAL_PRAYER_5    295
#define BACT_FINAL_PRAYER_6    296
#define BACT_FINAL_PRAYER_7    297
#define BACT_FINAL_PRAYER_8    298
#define BACT_FINAL_PRAYER_9    299

/* ---- Combat constants ---- */
#define MUSHROOMIZED_TARGET_CHANGE_CHANCE 25
#define CHANCE_OF_BODY_MOVING_AGAIN       15

/*
 * Color math register configuration table (hardware register presets).
 * Source: asm/data/unknown/C0AFF1.asm (COLOR_MATH_REGISTER_TABLE)
 * Laid out as 4 consecutive arrays indexed by layer config mode:
 *   [0..10]  = TM  (main screen designation)
 *   [11..20] = TD  (sub screen designation)
 *   [21..30] = CGWSEL (color math control)
 *   [31..40] = CGADSUB (color math designation)
 */
/* color_math_register_table moved to battle_ui.c */

/*
 * CHECK_DEAD_PLAYERS (asm/battle/check_dead_players.asm)
 *
 * Syncs HP/PP from party char_structs to battlers. If a player battler's
 * HP has dropped to 0 (from rolling HP), marks them unconscious, clears
 * all afflictions, displays KO text, and syncs afflictions back to char_struct.
 * Called each turn to detect deaths from rolling HP damage.
 */
void check_dead_players(void) {
    for (uint16_t i = 0; i < TOTAL_PARTY_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];

        /* Skip unconscious, enemy, or NPC battlers */
        if (b->consciousness == 0)
            continue;
        if (b->ally_or_enemy != 0)
            continue;
        if (b->npc_id != 0)
            continue;

        /* Sync HP and PP from char_struct to battler */
        uint16_t char_row = b->row;
        b->hp = party_characters[char_row].current_hp;
        b->pp = party_characters[char_row].current_pp;

        /* Check if HP has reached 0 */
        if (b->hp == 0) {
            /* Skip if already marked unconscious */
            if (b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS)
                goto sync_afflictions;

            /* Mark as unconscious and clear all afflictions */
            bt.current_target = (uint16_t)((uint8_t *)b - (uint8_t *)bt.battlers_table);
            b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = STATUS_0_UNCONSCIOUS;
            b->afflictions[STATUS_GROUP_SHIELD] = 0;
            b->afflictions[STATUS_GROUP_HOMESICKNESS] = 0;
            b->afflictions[STATUS_GROUP_CONCENTRATION] = 0;
            b->afflictions[STATUS_GROUP_STRANGENESS] = 0;
            b->afflictions[STATUS_GROUP_TEMPORARY] = 0;
            b->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] = 0;

            fix_target_name();

            /* Check if battle text window was already open */
            WindowInfo *existing = get_window(0x0E);
            create_window(0x0E);  /* WINDOW::TEXT_BATTLE */
            display_in_battle_text_addr(MSG_BTL5_ALLY_COLLAPSED);
            if (!existing) {
                redirect_close_focus_window();
            }
        }

sync_afflictions:
        /* Copy afflictions from battler back to char_struct */
        for (uint16_t j = 0; j < AFFLICTION_GROUP_COUNT; j++) {
            party_characters[char_row].afflictions[j] = b->afflictions[j];
        }

        /* Clamp concentration (afflictions[4]) to 0 or 1 */
        if (party_characters[char_row].afflictions[STATUS_GROUP_CONCENTRATION] != 0) {
            party_characters[char_row].afflictions[STATUS_GROUP_CONCENTRATION] = 1;
        }

        update_party();
    }
}

/*
 * BATTLE_SELECTION_MENU — Port of asm/battle/menu_handler.asm (994 lines).
 *
 * Handles the entire player battle menu: auto-fight AI (tries PSI Lifeup/Healing
 * before falling back to bash/shoot), manual menu (Bash/Shoot, Goods, Auto Fight,
 * PSI/Spy, Defend, Run Away, Pray/Mirror), target selection, and debug controls.
 *
 * Parameters:
 *   char_id      — 1-based party member ID (NESS=1, PAULA=2, JEFF=3, POO=4)
 *   num_selected — how many characters have already selected actions (0 = first)
 *
 * Returns: selected battle action ID (BACT_*), or 0 for cancel, 0xFFFF for debug exit.
 */
/* Window IDs from BATTLE_WINDOW_SIZES (asm/data/battle/battle_window_sizes.asm):
 * [0] = WINDOW::BATTLE_MENU_JEFF = 0x12
 * [1] = WINDOW::BATTLE_MENU      = 0x0F
 * [2] = WINDOW::BATTLE_MENU_FULL  = 0x30  */
static const uint8_t battle_window_sizes[] = { 0x12, 0x0F, 0x30 };

/* ITEM_USABLE_FLAGS table (from asm/data/item_usable_flags.asm)
 * Indexed by char_id - 1 (0=Ness, 1=Paula, 2=Jeff, 3=Poo) */
static const uint8_t item_usable_flags[4] = { 0x01, 0x02, 0x04, 0x08 };

/* The @CLOSE_MENU tail shared by every confirmed selection: refocus the
 * battle menu window, clear the meter-speed flags (resume music), and pop
 * the selected action. */
static StepResult bm_close_menu(BattleMenuState *st) {
    win.current_focus_window = battle_window_sizes[st->window_index];
    bt.half_hppp_meter_speed = 0;
    bt.disable_hppp_rolling = 0;
    return STEP_RESULT_POP(st->selected_action);
}

/* The Goods success tail (battle_item_menu's targeting-window close + the
 * Goods case's writeback in the blocking original): record the used item and
 * take the action the item classification chose. */
static StepResult bm_item_done(BattleMenuState *st) {
    close_window(WINDOW_BATTLE_ACTION_NAME);
    bt.battle_item_used = get_character_item(st->char_id, bt.battle_menu_param1);
    st->selected_action = bt.battle_menu_selected_action;
    return bm_close_menu(st);
}

/*
 * BATTLE_SELECTION_MENU (asm/battle/menu_handler.asm)
 *
 * The per-character battle command menu: Bash/Shoot (or Do Nothing), Goods,
 * Auto Fight, PSI/Spy, Defend, Run Away, Pray/Mirror. Also runs the
 * auto-fight AI, which picks an action without opening the menu.
 *
 * Run-to-completion: GAME_MODE_BATTLE_MENU. The former goto machine's labels
 * map onto the BM_* phases (see BattleMenuState, mode_stack.h); the command/
 * inventory selection menus, the enemy target picker (Bash/Shoot/Spy/Mirror),
 * the PSI cascade, and item targeting are STEP_PUSHed children. The former
 * battle_item_menu() (asm/battle/ui/battle_item_menu.asm) and
 * determine_battle_item_target() (asm/battle/ui/determine_battle_item_target
 * .asm) drivers are folded into the BM_ITEM* phases. Pushed by
 * GAME_MODE_BATTLE's player-menu phase (BTL_MENU below).
 */
StepResult mode_step_battle_menu(ModeState *ms) {
    BattleMenuState *st = &ms->battle_menu;
    static ModeState child_init;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch ((BattleMenuPhase)st->phase) {

        case BM_ENTER: {
            load_attack_palette(0);

            /* Compute pointer to this character's char_struct */
            CharStruct *ch = &party_characters[st->char_id - 1];

            /* Determine weapon type: 0=bash, 1=shoot (gun), 2=paralyzed/immobilized */
            st->weapon_type = 0;  /* @LOCAL06 */
            if (ch->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_PARALYZED ||
                ch->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_IMMOBILIZED) {
                st->weapon_type = 2;  /* Can only do nothing */
            } else {
                /* Check if character has an equipped weapon of gun type */
                uint8_t weapon_slot = ch->equipment[EQUIP_WEAPON];
                uint8_t item_id = 0;
                if (weapon_slot != 0) {
                    item_id = ch->items[weapon_slot - 1];
                }
                if (item_id != 0) {
                    const ItemConfig *item_entry = get_item_entry(item_id);
                    if (item_entry && (item_entry->type & 0x03) == 1) {
                        st->weapon_type = 1;  /* Shoot */
                    }
                }
            }

            /* ---- Auto-fight AI (fully synchronous: resolves + pops inline) ---- */
            if (game_state.auto_fight_enable & 0xFF) {
                uint16_t char_id = st->char_id;
        /* Check for status conditions that prevent PSI decision-making:
         * afflictions[4] (concentration) nonzero, or
         * afflictions[3] == STRANGE, or
         * afflictions[1] == MUSHROOMIZED → skip PSI, just attack */
        if ((ch->afflictions[STATUS_GROUP_CONCENTRATION] & 0xFF) != 0)
            goto auto_fight_attack;
        if ((ch->afflictions[STATUS_GROUP_STRANGENESS] & 0xFF) == STATUS_3_STRANGE)
            goto auto_fight_attack;
        if ((ch->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] & 0xFF) == STATUS_1_MUSHROOMIZED)
            goto auto_fight_attack;

        /* Only Ness and Poo try auto PSI healing */
        if (char_id != PARTY_MEMBER_NESS && char_id != PARTY_MEMBER_POO)
            goto auto_fight_attack;

        /* Try PSI Lifeup Omega (ability 26, action 35) */
        bt.battle_menu_targetting = 1;
        bt.battle_menu_param1 = 26;
        bt.battle_menu_selected_action = BACT_PSI_LIFEUP_OMEGA;
        if (check_if_psi_known(char_id, 26) &&
            (battle_action_table[BACT_PSI_LIFEUP_OMEGA].pp_cost & 0xFF) <= ch->current_pp_target) {
            /* Check if 2+ party members alive and ALL have HP < max_hp/4.
             * Assembly logic: loops through all PCs; if any member has
             * max_hp/4 <= current_hp_target (i.e. they're NOT critically low),
             * it falls through to try Lifeup Gamma instead.  Only if ALL members
             * are critically low (HP < 25%) does it use Omega on the whole party. */
            if (battle_count_chars(0) >= 2) {
                bool all_critical = true;
                for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
                    uint8_t member = game_state.party_members[i];
                    if (member < PARTY_MEMBER_NESS || member > PARTY_MEMBER_POO) continue;
                    CharStruct *pch = &party_characters[member - 1];
                    if ((pch->max_hp >> 2) <= pch->current_hp_target) {
                        all_critical = false;  /* This member has enough HP */
                        break;
                    }
                }
                if (all_critical) {
                    /* Everyone is below 25% HP — use Omega (target all) */
                    bt.battle_menu_targetting = 4;
                    goto auto_fight_return;
                }
            }
        }

        /* Try PSI Lifeup Gamma (ability 25, action 34) */
        bt.battle_menu_param1 = 25;
        bt.battle_menu_selected_action = BACT_PSI_LIFEUP_GAMMA;
        if (check_if_psi_known(char_id, 25) &&
            (battle_action_table[BACT_PSI_LIFEUP_GAMMA].pp_cost & 0xFF) <= ch->current_pp_target) {
            uint8_t target = autolifeup();
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
        }

        /* Try PSI Lifeup Beta (ability 24, action 33) */
        bt.battle_menu_param1 = 24;
        bt.battle_menu_selected_action = BACT_PSI_LIFEUP_BETA;
        if (check_if_psi_known(char_id, 24) &&
            (battle_action_table[BACT_PSI_LIFEUP_BETA].pp_cost & 0xFF) <= ch->current_pp_target) {
            uint8_t target = autolifeup();
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
        }

        /* Try PSI Lifeup Alpha (ability 23, action 32) */
        bt.battle_menu_param1 = 23;
        bt.battle_menu_selected_action = BACT_PSI_LIFEUP_ALPHA;
        if (check_if_psi_known(char_id, 23) &&
            (battle_action_table[BACT_PSI_LIFEUP_ALPHA].pp_cost & 0xFF) <= ch->current_pp_target) {
            uint8_t target = autolifeup();
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
        }

        /* Try PSI Healing Omega (ability 30, action 39)
         * Assembly: LDX #1; LDA #0 → autohealing(group=0, id=1) = UNCONSCIOUS */
        bt.battle_menu_param1 = 30;
        bt.battle_menu_selected_action = BACT_PSI_HEALING_OMEGA;
        if (check_if_psi_known(char_id, 30) &&
            (battle_action_table[BACT_PSI_HEALING_OMEGA].pp_cost & 0xFF) <= ch->current_pp_target) {
            uint8_t target = autohealing(0, 1);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
        }

        /* Try PSI Healing Gamma (ability 29, action 38)
         * Checks: paralyzed(0,3), diamondized(0,2), unconscious(0,1) */
        bt.battle_menu_param1 = 29;
        bt.battle_menu_selected_action = BACT_PSI_HEALING_GAMMA;
        if (check_if_psi_known(char_id, 29) &&
            (battle_action_table[BACT_PSI_HEALING_GAMMA].pp_cost & 0xFF) <= ch->current_pp_target) {
            uint8_t target = autohealing(0, 3);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
            target = autohealing(0, 2);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
            target = autohealing(0, 1);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
        }

        /* Try PSI Healing Beta (ability 28, action 37)
         * Checks: poisoned(0,5), nauseous(0,4), crying(2,2), strange(3,1) */
        bt.battle_menu_param1 = 28;
        bt.battle_menu_selected_action = BACT_PSI_HEALING_BETA;
        if (check_if_psi_known(char_id, 28) &&
            (battle_action_table[BACT_PSI_HEALING_BETA].pp_cost & 0xFF) <= ch->current_pp_target) {
            uint8_t target = autohealing(0, 5);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
            target = autohealing(0, 4);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
            target = autohealing(2, 2);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
            target = autohealing(3, 1);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
        }

        /* Try PSI Healing Alpha (ability 27, action 36)
         * Checks: cold(0,7), sunstroke(0,6), asleep(2,1) */
        bt.battle_menu_param1 = 27;
        bt.battle_menu_selected_action = BACT_PSI_HEALING_ALPHA;
        if (check_if_psi_known(char_id, 27) &&
            (battle_action_table[BACT_PSI_HEALING_ALPHA].pp_cost & 0xFF) <= ch->current_pp_target) {
            uint8_t target = autohealing(0, 7);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
            target = autohealing(0, 6);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
            target = autohealing(2, 1);
            bt.battle_menu_selected_target = target;
            if (target != 0) goto auto_fight_return;
        }

    auto_fight_attack:
                /* Fall through to basic attack */
                if (st->weapon_type == 0) {
                    st->selected_action = BACT_BASH;
                } else if (st->weapon_type == 1) {
                    st->selected_action = BACT_SHOOT;
                } else {  /* weapon_type == 2: paralyzed/immobilized */
                    return STEP_RESULT_POP(BACT_USE_NO_EFFECT);
                }
                /* Select random enemy target */
                bt.battle_menu_user = (uint8_t)char_id;
                bt.battle_menu_param1 = 0;
                bt.battle_menu_selected_action = st->selected_action;
                bt.battle_menu_targetting = 17;
                {
                    uint16_t total_enemies = bt.num_battlers_in_front_row + bt.num_battlers_in_back_row;
                    uint16_t target_idx = rand_limit(total_enemies);
                    bt.battle_menu_selected_target = (uint8_t)(target_idx + 1);
                }
                return STEP_RESULT_POP(st->selected_action);

    auto_fight_return:
                bt.battle_menu_user = (uint8_t)char_id;
                return STEP_RESULT_POP(bt.battle_menu_selected_action);
            }

            /* ---- Manual menu setup ---- */
            bt.half_hppp_meter_speed = 1;

            /* Determine window type index:
             * Paula/Poo have PSI → index starts at 1
             * Jeff/Ness without PSI → index starts at 0
             * If num_selected == 0 (first character), increment by 1 */
            st->window_index = 0;  /* @LOCAL03 */
            if (st->char_id == PARTY_MEMBER_PAULA || st->char_id == PARTY_MEMBER_POO) {
                st->window_index = 1;
            }
            if (st->num_selected == 0) {
                st->window_index++;
            }

            /* Create the battle menu window */
            uint8_t window_id = battle_window_sizes[st->window_index];
            create_window(window_id);

            /* Set window title to character name (assembly: SET_WINDOW_TITLE with X=5) */
            {
                char name_buf[8];
                int j;
                for (j = 0; j < 5 && ch->name[j]; j++)
                    name_buf[j] = eb_char_to_ascii(ch->name[j]);
                name_buf[j] = '\0';
                set_window_title(window_id, name_buf, 5);
            }

            /* Add attack option (Bash/Shoot/Do Nothing) */
            if (st->weapon_type == 0) {
                add_menu_item("Bash", 1, 0, 0);
            } else if (st->weapon_type == 1) {
                add_menu_item("Shoot", 1, 0, 0);
            } else {
                add_menu_item("Do Nothing", 1, 0, 0);
            }

            /* Add Goods and Defend options (unless paralyzed/immobilized)
             * Assembly: BATTLE_MENU_TEXT+16="Goods" userdata=2, +64="Defend" userdata=5 */
            if (st->weapon_type != 2) {
                add_menu_item("Goods", 2, 6, 0);
                add_menu_item("Defend", 5, 6, 1);
            }

            /* Add Defend and Run Away (only for first character selecting) */
            if (st->num_selected == 0) {
                uint16_t x_offset;
                if (st->window_index == 2) {
                    x_offset = 16;
                } else {
                    x_offset = 11;
                }
                if (st->char_id == PARTY_MEMBER_PAULA || st->char_id == PARTY_MEMBER_POO) {
                    x_offset += 2;
                }
                add_menu_item("Auto Fight", 3, x_offset, 0);
                add_menu_item("Run Away", 6, x_offset, 1);
            }

            /* Add Jeff's Spy or PSI option */
            if (st->char_id == PARTY_MEMBER_JEFF) {
                add_menu_item("Spy", 4, 0, 1);
            } else if ((ch->afflictions[STATUS_GROUP_CONCENTRATION] & 0xFF) == 0) {
                /* Can use PSI (not concentration-blocked) */
                add_menu_item("PSI", 4, 0, 1);
            }

            /* Add Paula's Pray */
            if (st->char_id == PARTY_MEMBER_PAULA) {
                add_menu_item("Pray", 7, 11, 0);
            }

            /* Add Poo's Mirror */
            if (st->char_id == PARTY_MEMBER_POO) {
                add_menu_item("Mirror", 7, 13, 0);
            }

            st->phase = BM_MAIN;
            break;
        }

        case BM_MAIN:
            /* @MENU_SELECTION_LOOP: set focus to the battle menu window */
            win.current_focus_window = battle_window_sizes[st->window_index];

            /* Print menu items on first entry only (assembly: menu_handler.asm:646-650) */
            if (!st->menu_printed) {
                print_menu_items();
                st->menu_printed = 1;
            }

            /* Run selection menu (allow cancel = 1) */
            child_init = (ModeState){0};
            child_init.selection_menu.phase        = SM_SETUP;
            child_init.selection_menu.allow_cancel = 1;
            st->phase = BM_MAIN_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_SELECTION_MENU, &child_init);

        case BM_MAIN_RESULT: {
            uint16_t result = (uint16_t)mode_child_result();

            if (result == 0) {
                /* Menu cancelled (B button) */

                /* Debug controls (only when ow.debug_flag is set) */
                if (ow.debug_flag) {
                    /* SELECT+START = instant win */
                    if ((core.pad1_held & (PAD_SELECT | PAD_START)) ==
                        (PAD_SELECT | PAD_START)) {
                        /* resume_music: clear meter speed flags */
                        bt.half_hppp_meter_speed = 0;
                        bt.disable_hppp_rolling = 0;
                        return STEP_RESULT_POP(0xFFFF);
                    }
                    /* R button = cycle through battle targets (debug) */
                    if (core.pad1_held & PAD_R) {
                        /* cycle_battle_target — debug-only, skip for now */
                        st->phase = BM_MAIN;
                        break;
                    }
                }

                /* "Can't cancel" menus (ow.battle_mode != 0 means can go back)
                 * skip the second debug block and return 0 directly. */
                if (ow.battle_mode == 0 && ow.debug_flag) {
                    if (core.pad1_held & PAD_L) {
                        /* debug_set_char_level — re-init all battler stats */
                        for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
                            uint8_t member = game_state.party_members[i];
                            if (member < PARTY_MEMBER_NESS || member > PARTY_MEMBER_POO) continue;
                            battle_init_player_stats(member, &bt.battlers_table[i]);
                        }
                        st->phase = BM_MAIN;
                        break;
                    }
                    if (core.pad1_held & PAD_SELECT) {
                        /* debug_y_button_goods — debug-only */
                        st->phase = BM_MAIN;
                        break;
                    }
                }

                /* @CANCEL_MENU: resume music (clear meter speed flags) */
                bt.half_hppp_meter_speed = 0;
                bt.disable_hppp_rolling = 0;
                return STEP_RESULT_POP(0);
            }

            /* @PROCESS_SELECTION */
            bt.battle_item_used = 0;

            /* Get the selected menu item's userdata */
            WindowInfo *w = get_window(battle_window_sizes[st->window_index]);
            if (!w || w->selected_option >= w->menu_count)
                return bm_close_menu(st);
            uint16_t selection = w->menu_items[w->selected_option].userdata;

            switch (selection) {
            case 1:  /* Bash/Shoot/Do Nothing */
                if (st->weapon_type == 0) {
                    st->selected_action = BACT_BASH;
                } else if (st->weapon_type == 1) {
                    st->selected_action = BACT_SHOOT;
                } else {
                    st->selected_action = BACT_USE_NO_EFFECT;
                }
                bt.battle_menu_selected_action = st->selected_action;
                bt.battle_menu_targetting = 17;
                if (st->weapon_type == 2) {
                    return bm_close_menu(st);  /* "Do Nothing" needs no target */
                }
                /* Assembly: JSL SELECT_BATTLE_TARGET_DISPATCH_FAR
                 * A=0 (single target), X=1 (allow cancel), Y=selected_action */
                enemy_select_make_init(&child_init, 1, st->selected_action);
                st->phase = BM_TARGET_RESULT;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ENEMY_SELECT, &child_init);

            case 2:  /* Goods (assembly: JSL BATTLE_ITEM_MENU_FAR) */
                bt.battle_menu_user = (uint8_t)st->char_id;
                st->phase = BM_ITEM;
                break;

            case 3:  /* Auto Fight (assembly: @ACTION_AUTO_FIGHT, menu_handler.asm:722) */
                game_state.auto_fight_enable = 1;
                render_hppp_window_header();
                st->selected_action = BACT_NO_EFFECT;
                return bm_close_menu(st);

            case 4:  /* PSI or Spy */
                if (st->char_id == PARTY_MEMBER_JEFF) {
                    /* Spy */
                    st->selected_action = BACT_SPY;
                    bt.battle_menu_selected_action = st->selected_action;
                    bt.battle_menu_targetting = 17;
                    /* Assembly: JSL SELECT_BATTLE_TARGET_DISPATCH_FAR
                     * A=0 (single target), X=1 (allow cancel), Y=BACT_SPY */
                    enemy_select_make_init(&child_init, 1, st->selected_action);
                    st->phase = BM_TARGET_RESULT;
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ENEMY_SELECT, &child_init);
                } else {
                    /* PSI menu */
                    bt.battle_menu_user = (uint8_t)st->char_id;
                    child_init = (ModeState){0};
                    child_init.battle_psi_menu.phase   = BP_OPEN;
                    child_init.battle_psi_menu.char_id = bt.battle_menu_user;
                    st->phase = BM_PSI_RESULT;
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_PSI_MENU, &child_init);
                }

            case 5:  /* Defend/Guard (assembly: @ACTION_GUARD, menu_handler.asm:726) */
                st->selected_action = BACT_GUARD;
                bt.battle_menu_selected_action = st->selected_action;
                bt.battle_menu_targetting = 0;
                return bm_close_menu(st);

            case 6:  /* Run Away */
                bt.battle_menu_targetting = 1;
                bt.battle_menu_selected_target = (uint8_t)st->char_id;
                st->selected_action = BACT_RUN_AWAY;
                bt.battle_menu_selected_action = st->selected_action;
                return bm_close_menu(st);

            case 7:  /* Pray or Mirror */
                bt.battle_menu_targetting = 1;
                bt.battle_menu_selected_target = (uint8_t)st->char_id;

                if (st->char_id == PARTY_MEMBER_PAULA) {
                    /* Pray — check Giygas phase for final prayers */
                    switch (bt.giygas_phase) {
                    case GIYGAS_START_PRAYING:   st->selected_action = BACT_FINAL_PRAYER_1; break;
                    case GIYGAS_PRAYER_1_USED:   st->selected_action = BACT_FINAL_PRAYER_2; break;
                    case GIYGAS_PRAYER_2_USED:   st->selected_action = BACT_FINAL_PRAYER_3; break;
                    case GIYGAS_PRAYER_3_USED:   st->selected_action = BACT_FINAL_PRAYER_4; break;
                    case GIYGAS_PRAYER_4_USED:   st->selected_action = BACT_FINAL_PRAYER_5; break;
                    case GIYGAS_PRAYER_5_USED:   st->selected_action = BACT_FINAL_PRAYER_6; break;
                    case GIYGAS_PRAYER_6_USED:   st->selected_action = BACT_FINAL_PRAYER_7; break;
                    case GIYGAS_PRAYER_7_USED:   st->selected_action = BACT_FINAL_PRAYER_8; break;
                    case GIYGAS_PRAYER_8_USED:   st->selected_action = BACT_FINAL_PRAYER_9; break;
                    default:                     st->selected_action = BACT_PRAY;            break;
                    }
                    bt.battle_menu_selected_action = st->selected_action;
                    return bm_close_menu(st);
                } else if (st->char_id == PARTY_MEMBER_POO) {
                    /* Mirror — needs target selection */
                    st->selected_action = BACT_MIRROR;
                    bt.battle_menu_selected_action = st->selected_action;
                    bt.battle_menu_targetting = 17;
                    /* Assembly: JSL SELECT_BATTLE_TARGET_DISPATCH_FAR
                     * A=0 (single target), X=1 (allow cancel), Y=BACT_MIRROR */
                    enemy_select_make_init(&child_init, 1, st->selected_action);
                    st->phase = BM_TARGET_RESULT;
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ENEMY_SELECT, &child_init);
                }
                return bm_close_menu(st);

            default:
                return bm_close_menu(st);
            }
            break;
        }

        case BM_TARGET_RESULT: {
            /* The enemy target picker popped (Bash/Shoot/Spy/Mirror). */
            uint16_t target = (uint16_t)mode_child_result();
            if (target == 0) {
                /* Cancelled: back to the command menu */
                st->phase = BM_MAIN;
                break;
            }
            bt.battle_menu_selected_target = (uint8_t)target;
            return bm_close_menu(st);
        }

        case BM_PSI_RESULT:
            /* The BATTLE_PSI_MENU cascade popped: 0 = cancelled. */
            if ((uint16_t)mode_child_result() == 0) {
                st->phase = BM_MAIN;
                break;
            }
            st->selected_action = bt.battle_menu_selected_action;
            return bm_close_menu(st);

        case BM_ITEM:
            /* battle_item_menu entry: no items → back to the command menu */
            if (party_characters[st->char_id - 1].items[0] == 0) {
                st->phase = BM_MAIN;
                break;
            }
            /* @SELECT_ITEM: create the inventory window and populate it.
             * inventory_get_item_name's embedded window_tick yield is the
             * accepted precedent (the pause menu's Goods path calls it the
             * same way from a step). */
            inventory_get_item_name(st->char_id, WINDOW_INVENTORY);
            child_init = (ModeState){0};
            child_init.selection_menu.phase        = SM_SETUP;
            child_init.selection_menu.allow_cancel = 1;
            st->phase = BM_ITEM_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_SELECTION_MENU, &child_init);

        case BM_ITEM_RESULT: {
            uint16_t selection = (uint16_t)mode_child_result();

            /* Close the inventory window */
            close_window(WINDOW_INVENTORY);

            if (selection == 0) {
                /* Cancelled: back to the command menu */
                st->phase = BM_MAIN;
                break;
            }

            /* Store selected item slot in bt.battle_menu_param1 */
            bt.battle_menu_param1 = (uint8_t)selection;

            /* DETERMINE_BATTLE_ITEM_TARGET, folded inline: the defaults and
             * the item classification are synchronous; only the targeting UI
             * is a child push. */
            uint8_t user_id = bt.battle_menu_user;
            uint16_t item_id = get_character_item(user_id, bt.battle_menu_param1);

            const ItemConfig *item_entry = get_item_entry(item_id);
            if (!item_entry) {
                /* Assembly: returns success with the defaults untouched. */
                return bm_item_done(st);
            }

            /* Set defaults in battle_menu_selection:
             * selected_action = 2 (placeholder), targetting = 1, selected_target = user */
            bt.battle_menu_selected_action = 2;
            bt.battle_menu_targetting = 1;
            bt.battle_menu_selected_target = user_id;

            /* Read item type byte (offset 25) */
            uint8_t item_type = item_entry->type;
            uint8_t type_category = item_type & 0x30;

            if (type_category == 0x10 || type_category == 0x20) {
                /* Offensive (0x10) or Support (0x20) item — has effect field */
                st->item_effect = item_entry->effect_id;
            } else if (type_category == 0x30) {
                /* Equipment item — check if usable in battle */
                uint8_t equip_subtype = item_type & 0x0C;
                if (equip_subtype != 0 && equip_subtype != 4) {
                    /* Not a battle-usable equipment subtype — finish with the
                     * defaults. Assembly: jumps to SET_DEFAULT_ACTION without
                     * modifying selected_action. */
                    return bm_item_done(st);
                }

                /* Check if this character can use this equipment */
                if (user_id >= 1 && user_id <= 4 &&
                    !(item_entry->flags & item_usable_flags[user_id - 1])) {
                    /* Character can't use this item */
                    bt.battle_menu_selected_action = 3;
                    return bm_item_done(st);
                }

                /* Equipment is usable — determine targeting from its effect */
                st->item_effect = item_entry->effect_id;
            } else {
                /* Other item type — no targeting needed, use the defaults */
                return bm_item_done(st);
            }

            child_init = (ModeState){0};
            child_init.targeting.phase     = TGT_ENTER;
            child_init.targeting.action_id = st->item_effect;
            child_init.targeting.char_id   = user_id;
            st->phase = BM_ITEM_TGT_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DETERMINE_TARGETING, &child_init);
        }

        case BM_ITEM_TGT_RESULT:
        default: {
            uint16_t result = (uint16_t)mode_child_result();

            if ((result & 0xFF) == 0) {
                /* Targeting cancelled: close the targeting sub-window and go
                 * back to item selection (battle_item_menu's select_item). */
                close_window(WINDOW_BATTLE_ACTION_NAME);
                st->phase = BM_ITEM;
                break;
            }

            /* Store effect as selected_action; unpack targeting:
             * high byte = mode, low byte = target */
            bt.battle_menu_selected_action = st->item_effect;
            bt.battle_menu_targetting = (uint8_t)(result >> 8);
            bt.battle_menu_selected_target = (uint8_t)(result & 0xFF);
            return bm_item_done(st);
        }
        }
    }
}

/*
 * CONSUME_USED_BATTLE_ITEM (asm/battle/consume_used_battle_item.asm)
 *
 * After a battle item is used, consumes it from the character's inventory
 * if the item has the CONSUMED_ON_USE flag set. Only applies to player
 * characters (not enemies or NPCs). Verifies the item in the inventory
 * slot still matches before consuming.
 */
void consume_used_battle_item(void) {
    Battler *attacker = battler_from_offset(bt.current_attacker);

    /* Only player characters consume items */
    if (attacker->ally_or_enemy != 0)
        return;
    if (attacker->npc_id != 0)
        return;

    /* action_item_slot: 1-based inventory slot; 0 = no item */
    uint16_t item_slot = attacker->action_item_slot;
    if (item_slot == 0)
        return;

    uint16_t char_id = attacker->id;  /* 1-based: 1=Ness, 2=Paula, etc. */
    uint16_t item_id = attacker->current_action_argument;
    if (item_id == 0)
        return;

    /* Verify the item at that slot still matches */
    uint16_t char_idx = char_id - 1;
    uint8_t slot_item = party_characters[char_idx].items[item_slot - 1];
    if (slot_item != item_id)
        return;

    /* Check CONSUMED_ON_USE flag in item config table */
    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry)
        return;
    if (!(entry->flags & ITEM_FLAG_CONSUMED))
        return;

    /* Check if item is usable by this character */
    static const uint8_t usable_flags[4] = { 0x01, 0x02, 0x04, 0x08 };
    if (char_id < 1 || char_id > 4)
        return;
    if (!(entry->flags & usable_flags[char_id - 1]))
        return;

    /* Remove the item from inventory */
    remove_item_from_inventory(char_id, item_slot);
}

/*
 * DISPLAY_TEXT_WITH_PROMPT (asm/text/display_text_with_prompt.asm)
 *
 * Enables the blinking triangle prompt, displays text via DISPLAY_TEXT,
 * then clears the prompt. The triangle signals the player to press a button
 * to advance the text.
 */
void display_text_with_prompt(const uint8_t *text, size_t size) {
    dt.blinking_triangle_flag = 1;
    display_in_battle_text(text, size);
    dt.blinking_triangle_flag = 0;
}

void display_text_with_prompt_addr(uint32_t addr) {
    dt.blinking_triangle_flag = 1;
    display_in_battle_text_addr(addr);
    dt.blinking_triangle_flag = 0;
}

/*
 * SET_BATTLER_PP_FROM_TARGET (asm/battle/set_battler_pp_from_target.asm)
 *
 * Deducts pp_cost from the battler's pp_target, clamping at 0.
 * A = battler offset, X = pp cost.
 * Reads pp_target, subtracts cost (or clamps to 0), calls SET_PP.
 */
void set_battler_pp_from_target(uint16_t attacker_offset, uint16_t pp_cost) {
    Battler *b = battler_from_offset(attacker_offset);
    uint16_t new_pp;
    if (pp_cost > b->pp_target) {
        new_pp = 0;
    } else {
        new_pp = b->pp_target - pp_cost;
    }
    battle_set_pp(b, new_pp);
}

/*
 * SET_BATTLER_TARGET (asm/battle/set_battler_target.asm)
 *
 * Finds the battler at target_index in the front/back row arrays
 * and sets the attacker's bt.current_target field to a 1-based position
 * (front row positions first, then back row).
 *
 * A = attacker battler offset, X = target battler index.
 */
void set_battler_target(uint16_t attacker_offset, uint16_t target_index) {
    Battler *attacker = battler_from_offset(attacker_offset);

    /* Search front row */
    for (uint16_t i = 0; i < bt.num_battlers_in_front_row; i++) {
        if ((bt.front_row_battlers[i] & 0xFF) == target_index) {
            attacker->current_target = (uint8_t)(i + 1);
            return;
        }
    }

    /* Search back row */
    for (uint16_t i = 0; i < bt.num_battlers_in_back_row; i++) {
        if ((bt.back_row_battlers[i] & 0xFF) == target_index) {
            attacker->current_target = (uint8_t)(i + bt.num_battlers_in_front_row + 1);
            return;
        }
    }
}

/*
 * CLOSE_ALL_WINDOWS_AND_HIDE_HPPP (asm/text/hp_pp_window/close_all_windows_and_hide_hppp.asm)
 *
 * Closes all windows, ticks rendering, hides HPPP windows, ticks again.
 */
void close_all_windows_and_hide_hppp(void) {
    close_all_windows();
    window_tick();
    hide_hppp_windows();
    window_tick();
}

/*
 * ENEMY_SELECT_MODE — Port of asm/battle/enemy_select_mode.asm (332 lines).
 *
 * Debug-only interactive battle group selector.
 * Opens a text window with a 4-digit number (the battle group ID).
 * LEFT/RIGHT moves cursor between digits, UP/DOWN changes selected digit
 * (with wrapping). On confirm (A/L), loads the enemy group and sets up
 * battle sprites. On cancel (B/SELECT), returns the original group.
 *
 * After confirming a group, the enemies are parsed, battler slots 8+
 * are cleared and re-initialized, sprites are set up, and the display
 * is refreshed before returning to the digit editor.
 */
uint16_t enemy_select_mode(uint16_t current_group) {
    uint16_t value = current_group;          /* @LOCAL0A: editable value */
    uint16_t original = current_group;       /* @LOCAL09: return on cancel */
    uint16_t return_value;

    /* Open text window and get cursor position (lines 23-34) */
    set_instant_printing();
    create_window(WINDOW_TEXT_BATTLE);
    WindowInfo *w = get_window(WINDOW_TEXT_BATTLE);
    uint16_t start_x = w ? w->text_x : 0;   /* @LOCAL08 */
    uint16_t start_y = w ? w->text_y : 0;    /* @LOCAL07 */
    uint16_t cursor_pos = 1;                 /* @LOCAL06: digit pos from right, 1-based */
    uint16_t place_value = 1;                /* @LOCAL05: 10^(cursor_pos-1) */

    #define CHAR_ZERO_US 0x60  /* CHAR::ZERO for US (include/macros.asm:256) */
    #define MAX_ENEMY_GROUP 482  /* ENEMY_GROUP::UNKNOWN_482 */

redraw_number:
    /* Redraw the 4-digit number at the window's text position (lines 38-116) */
    set_instant_printing();
    set_focus_text_cursor(start_x, start_y);

    /* Convert value to 4 digits (NUMBER_TO_TEXT_BUFFER equivalent) */
    {
        uint8_t digits[4];
        uint16_t tmp = value;
        for (int d = 3; d >= 0; d--) {
            digits[d] = (uint8_t)(tmp % 10);
            tmp /= 10;
        }
        /* Print each digit. In US retail, all digits use CHAR::ZERO base (0x60).
         * (In CLEAN_ROM/JP, selected digit uses JZERO_UNDERLINED (0x10),
         *  others use JZERO (0x30), but US uses ZERO for both.) */
        for (int i = 0; i < 4; i++) {
            uint16_t tile = CHAR_ZERO_US + digits[i];
            print_char_with_sound(tile);
        }
    }
    clear_instant_printing();
    window_tick();

    /* Input loop (lines 117-217) */
    for (;;) {
        window_tick();

        /* LEFT: move cursor to higher digit (lines 120-129) */
        if (core.pad1_pressed & PAD_LEFT) {
            if (cursor_pos < 3) {  /* max 3 = thousands digit */
                cursor_pos++;
                place_value *= 10;
            }
            goto redraw_number;
        }
        /* RIGHT: move cursor to lower digit (lines 130-143) */
        if (core.pad1_pressed & PAD_RIGHT) {
            if (cursor_pos > 1) {
                cursor_pos--;
                place_value /= 10;
            }
            goto redraw_number;
        }
        /* UP: increment current digit with wrapping (lines 144-173) */
        if (core.pad1_autorepeat & PAD_UP) {
            uint16_t current_digit = (value / place_value) % 10;
            if (current_digit == 9) {
                /* Wrap 9→0: subtract 9 * place_value */
                value -= place_value * 9;
            } else {
                value += place_value;
            }
            goto validate_number;
        }
        /* DOWN: decrement current digit with wrapping (lines 174-203) */
        if (core.pad1_autorepeat & PAD_DOWN) {
            uint16_t current_digit = (value / place_value) % 10;
            if (current_digit == 0) {
                /* Wrap 0→9: add 9 * place_value */
                value += place_value * 9;
            } else {
                value -= place_value;
            }
            goto validate_number;
        }
        /* A/L: confirm selection (lines 204-210) */
        if (core.pad1_pressed & PAD_CONFIRM) {
            return_value = value;
            goto close_and_return;
        }
        /* B/SELECT: cancel, return original (lines 211-217) */
        if (core.pad1_pressed & PAD_CANCEL) {
            return_value = original;
            goto close_and_return;
        }
        continue;

    validate_number:
        /* Clamp value to valid range (lines 218-226) */
        if (value == 0)
            goto redraw_number;
        if (value > MAX_ENEMY_GROUP)
            value = MAX_ENEMY_GROUP;

        /* Load and preview the enemy group (lines 227-324) */
        bt.current_battle_group = value;

        /* Parse BTL_ENTRY_PTR_TABLE to populate bt.enemies_in_battle_ids[]
         * (same logic as init_battle_scripted, assembly lines 228-270) */
        {
            static const uint8_t *ptr_table = NULL;
            static const uint8_t *groups_table = NULL;
            if (!ptr_table)
                ptr_table = ASSET_DATA(ASSET_DATA_BTL_ENTRY_PTR_TABLE_BIN);
            if (!groups_table)
                groups_table = ASSET_DATA(ASSET_DATA_ENEMY_BATTLE_GROUPS_TABLE_BIN);
            if (ptr_table && groups_table) {
                uint32_t entry_off = (uint32_t)value * 8;
                uint32_t rom_ptr = (uint32_t)ptr_table[entry_off]
                                 | ((uint32_t)ptr_table[entry_off + 1] << 8)
                                 | ((uint32_t)ptr_table[entry_off + 2] << 16);
                uint32_t groups_offset = rom_ptr - 0xD0D52D;
                const uint8_t *ptr = groups_table + groups_offset;

                bt.enemies_in_battle = 0;
                while (1) {
                    uint8_t count = *ptr;
                    if (count == 0xFF)
                        break;
                    uint16_t enemy_id = read_u16_le(&ptr[1]);
                    for (uint8_t j = 0; j < count; j++) {
                        if (bt.enemies_in_battle < MAX_ENEMY_BATTLER_SLOTS)
                            bt.enemies_in_battle_ids[bt.enemies_in_battle++] = enemy_id;
                    }
                    ptr += 3;
                }
            }
        }

        /* Set up enemy sprites (lines 271-272) */
        force_blank_and_wait_vblank();
        setup_battle_enemy_sprites();

        /* Clear battler slots 8 through BATTLER_COUNT-1 (lines 273-292).
         * Assembly zeroes each battler struct via MEMSET16. */
        for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
            memset(&bt.battlers_table[i], 0, sizeof(Battler));
        }

        /* Initialize enemy battlers from the group (lines 293-316) */
        for (uint16_t i = 0; i < bt.enemies_in_battle; i++) {
            battle_init_enemy_stats(&bt.battlers_table[8 + i], bt.enemies_in_battle_ids[i]);
        }

        /* Layout positions and refresh display (lines 317-324) */
        layout_enemy_battle_positions();
        set_palette_upload_mode(24);
        blank_screen_and_wait_vblank();
        fade_in(1, 1);
        goto redraw_number;
    }

close_and_return:
    /* Close window and return (lines 325-331) */
    set_window_focus(WINDOW_TEXT_BATTLE);
    close_focus_window();
    return return_value;

    #undef CHAR_ZERO_US
    #undef MAX_ENEMY_GROUP
}

/*
 * SHOW_PSI_ANIMATION — Port of asm/battle/show_psi_animation.asm (518 lines).
 *
 * Initializes a PSI visual effect:
 *   1. Loads and decompresses PSI graphics (2bpp → 4bpp conversion if needed)
 *   2. Uploads tile data to VRAM
 *   3. Loads PSI palette into animation state and displayed palette group
 *   4. Loads and decompresses PSI arrangement (tilemap frames) into ert.buffer
 *   5. Populates psi_animation_state with timing and palette cycling params
 *   6. Darkens background ert.palettes and backs up sprite ert.palettes
 *   7. Marks targeted enemy sprites based on target type (SINGLE/ROW/ALL/RANDOM)
 *   8. Sets BG scroll offsets to center animation on target
 *
 * anim_id: index into PSI_ANIM_CFG table (0-33).
 */
/* PSI animation defines moved to battle_psi.c */

/* ======================================================================
 * DISPLAY_BATTLE_CUTSCENE_TEXT (asm/battle/display_battle_cutscene_text.asm)
 *
 * Fades out, shows cutscene text (hiding battle UI), then reloads battle
 * scene and restores UI. Used during Giygas prayer sequences.
 *
 * Parameters:
 *   group: enemy group to reload battle scene with
 *   music: music track to play after reload
 *   text_addr: SNES address of battle text to display during cutscene
 * ====================================================================== */
void display_battle_cutscene_text(uint16_t group, uint16_t music,
                                         uint32_t text_addr) {
    fade_out(1, 4);
    wait_for_fade_with_tick();
    bt.battle_mode_flag = 0;
    ml.current_map_music_track = 0;
    close_all_windows_and_hide_hppp();
    display_in_battle_text_addr(text_addr);
    fade_out(1, 2);
    wait_for_fade_with_tick();
    load_battle_scene(group, music);
    bt.battle_mode_flag = 1;
    redirect_show_hppp_windows();
    create_window(0x0E);  /* WINDOW::TEXT_BATTLE */
    battle_wait(FRAMES_PER_SECOND);  /* 1 second */
}

/* ======================================================================
 * PLAY_GIYGAS_WEAKENED_SEQUENCE (asm/battle/play_giygas_weakened_sequence.asm)
 *
 * Fades to black, shows Giygas weakened text on BG3-only screen,
 * then fades back to battle. Used during prayers 8 and 9.
 * ====================================================================== */
void play_giygas_weakened_sequence(uint16_t music,
                                          uint32_t text_addr) {
    fade_out(1, 1);
    write_apu_port1(2);
    wait_for_fade_with_tick();
    bt.battle_mode_flag = 0;
    close_all_windows_and_hide_hppp();
    ppu.tm = 0x04;  /* BG3 only */
    change_music(191);  /* MUSIC::GIYGAS_WEAKENED */
    fade_in(1, 1);
    wait_for_fade_with_tick();
    battle_wait(20);  /* 2 * SIXTHS_OF_A_SECOND = 20 frames */
    display_in_battle_text_addr(text_addr);
    bt.battle_mode_flag = 1;
    battle_wait(20);  /* 2 * SIXTHS_OF_A_SECOND */
    write_apu_port1(2);
    fade_out(1, 1);
    wait_for_fade_with_tick();
    redirect_show_hppp_windows();
    create_window(0x0E);  /* WINDOW::TEXT_BATTLE */
    ppu.tm = 0x17;  /* BG1 + BG2 + BG3 + OBJ */
    change_music(music);
    fade_in(1, 1);
    wait_for_fade_with_tick();
}

/*
 * JUMP_TEMP_FUNCTION_POINTER — Port of asm/overworld/jump_temp_function_pointer.asm.
 * Assembly: JML (TEMP_FUNCTION_POINTER) — indirect long jump through a
 * 24-bit ROM address stored in bt.temp_function_pointer.
 *
 * In the C port, we dispatch via a lookup table mapping ROM addresses to
 * C function pointers. ROM addresses come from the battle_action_table
 * (loaded from the donor ROM).
 */
/* ======================================================================
 * BATTLE_ROUTINE (asm/battle/main_battle_routine.asm)
 *
 * The main battle loop. Initializes battlers, runs the player menu →
 * enemy AI → turn execution → win/loss check cycle.
 *
 * Run-to-completion: GAME_MODE_BATTLE. The former goto machine's labels map
 * onto the BTL_* phases (see BattleRoutineState, mode_stack.h). The player
 * command menus, every battle text, the battle waits, the ending fade and
 * the end-of-round level-ups are STEP_PUSHed children; the big synchronous
 * chunks (scene reinit, enemy AI, the debug encounter-setup loop, turn
 * setup) live in btl_* helpers below, verbatim from the blocking original.
 * Pushed by GAME_MODE_BATTLE_ENTRY / GAME_MODE_BATTLE_SCRIPTED. Pops 0 =
 * victory, 1 = party defeated, 2 = special defeat code.
 * ====================================================================== */

/* Asset table references loaded from ROM */
const uint8_t *btl_entry_ptr_table;
const uint8_t *btl_entry_bg_table;
const uint8_t *npc_ai_table;
const uint8_t *consolation_item_table;

/* The display_in_battle_text_addr / display_text_wait_addr /
 * display_text_with_prompt_addr prologue (see the top of this file) + a
 * DISPLAY_TEXT child init for `addr`. `prompt` is the with-prompt variant's
 * blinking_triangle_flag = 1; `has_cnum` is the wait_addr variant's
 * set_cnum(cnum). Returns false (warn, like display_text_from_addr) when the
 * address is unresolvable — the caller falls through to its resume phase
 * inline, which runs the epilogue (clearing the prompt flag). Shared with the
 * battle_actions.c action steppers (battle_internal.h). */
bool battle_push_text_ex(ModeState *child, uint32_t addr, bool prompt,
                         bool has_cnum, uint32_t cnum) {
    if (prompt)
        dt.blinking_triangle_flag = 1;
    if (game_state.auto_fight_enable && (core.pad1_held & PAD_B)) {
        game_state.auto_fight_enable = 0;
        clear_hppp_window_header();
    }
    if (has_cnum)
        set_cnum(cnum);
    if (bt.battle_mode_flag)
        dt.blinking_triangle_flag = 2;
    if (dt_make_child_init(child, addr))
        return true;
    LOG_WARN("WARNING: resolve_text_addr(0x%06X) returned NULL\n", addr);
    return false;
}

bool battle_push_text(ModeState *child, uint32_t addr) {
    return battle_push_text_ex(child, addr, false, false, 0);
}

/* One-time entry setup (assembly lines 26-133): normal-battle-mode party
 * defaults, the Giygas phase, and the BG/palette/letterbox lookups. */
static void btl_begin(BattleRoutineState *st) {
    st->debug_party_flags = 1;
    st->debug_enemy_level = 1;

    /* Load asset tables if not yet loaded */
    if (!btl_entry_ptr_table)
        btl_entry_ptr_table = ASSET_DATA(ASSET_DATA_BTL_ENTRY_PTR_TABLE_BIN);
    if (!btl_entry_bg_table)
        btl_entry_bg_table = ASSET_DATA(ASSET_DATA_BTL_ENTRY_BG_TABLE_BIN);
    if (!npc_ai_table)
        npc_ai_table = ASSET_DATA(ASSET_DATA_NPC_AI_TABLE_BIN);
    if (!consolation_item_table)
        consolation_item_table = ASSET_DATA(ASSET_DATA_CONSOLATION_ITEM_TABLE_BIN);

    /* ================================================================
     * Phase 1: Initial setup (lines 26-133)
     * ================================================================ */
    if (ow.battle_mode == 0) {
        /* Normal battle mode: set up single-character party with 1 enemy */
        st->debug_enemy_level = 1;
        st->debug_party_flags = 1;
        game_state.player_controlled_party_count = 1;

        /* Clear party_members and party_order */
        memset(game_state.party_members, 0, 6);
        memset(game_state.party_order, 0, 6);

        /* Set first member and first order slot to 1 */
        game_state.party_members[0] = 1;
        game_state.party_order[0] = 1;

        bt.enemies_in_battle = 1;
        bt.current_battle_group = 1;

        /* Read enemy ID from btl_entry_ptr_table[1] */
        /* battle_entry_ptr_entry is 8 bytes; pointer at offset 2; enemy ID at offset 3 */
        if (btl_entry_ptr_table) {
            uint16_t enemy_id = read_u16_le(&btl_entry_ptr_table[8 + 3]);
            bt.enemies_in_battle_ids[0] = enemy_id;
        }
    }

    /* Init Giygas phase */
    bt.giygas_phase = 0;
    if (bt.current_battle_group == ENEMY_GROUP_BOSS_GIYGAS_PHASE_1_ENTRY) {  /* ENEMY_GROUP::UNKNOWN_475 */
        bt.giygas_phase = GIYGAS_BATTLE_STARTED;
    }

    /* Load BG table entries for this battle group */
    if (btl_entry_bg_table) {
        uint16_t offset = bt.current_battle_group * 4;
        st->bg_id = read_u16_le(&btl_entry_bg_table[offset]);
        st->palette_id = read_u16_le(&btl_entry_bg_table[offset + 2]);
    } else {
        st->bg_id = 0;
        st->palette_id = 0;
    }

    /* Get letterbox style from btl_entry_ptr_table */
    if (btl_entry_ptr_table) {
        uint16_t entry_offset = bt.current_battle_group * 8 + 7; /* battle_entry_ptr_entry::letterbox_style */
        st->letterbox_style = btl_entry_ptr_table[entry_offset];
    } else {
        st->letterbox_style = 0;
    }
}

/* reinit_battle scene setup (assembly lines 134-480, through the fade-in):
 * sprite/BG loads, battler-table init, battle music. The force_blank/
 * blank_screen one-shot vblank yields stay inline (documented deferral,
 * matching door.c's sequential-frame helpers). */
static void btl_reinit_scene(BattleRoutineState *st) {
    bt.mirror_enemy = 0;
    st->run_attempt = 0;
    bt.battle_item_used = 0;
    st->turn_counter = 0;
    bt.battle_money_scratch = 0;
    bt.battle_exp_scratch = 0;

    force_blank_and_wait_vblank();
    clear_battle_visual_effects();
    load_enemy_battle_sprites();
    text_load_window_gfx();
    upload_text_tiles_to_vram(1);
    load_battle_bg(st->bg_id, st->palette_id, st->letterbox_style);
    setup_battle_enemy_sprites();

    /* Clear all battler slots */
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        memset(&bt.battlers_table[i], 0, sizeof(Battler));
    }

    bt.highest_enemy_level_in_battle = 0;

    /* Initialize party members (slots 0-5) */
    uint16_t npc_index = 0;
    for (uint16_t i = 0; i < 6; i++) {
        uint8_t member_id = game_state.party_members[i];
        if (member_id == 0) continue;

        if (member_id >= 1 && member_id <= 4) {
            /* Player character */
            battle_init_player_stats(member_id, &bt.battlers_table[i]);
        } else if (member_id >= 5) {
            /* NPC party member — look up their enemy config from NPC_AI_TABLE */
            Battler *b = &bt.battlers_table[i];
            if (npc_ai_table) {
                uint8_t enemy_cfg = npc_ai_table[member_id * 2 + 1];
                battle_init_enemy_stats(b, enemy_cfg);
            }
            b->ally_or_enemy = 0;
            b->npc_id = member_id;
            b->row = (uint8_t)npc_index;

            /* Set HP from party NPC state */
            uint16_t npc_hp = (npc_index == 0) ?
                game_state.party_npc_1_hp : game_state.party_npc_2_hp;
            b->hp_target = npc_hp;
            b->hp = npc_hp;
            npc_index++;
            b->pp_target = 0;
            b->pp = 0;
        }
    }

    clamp_enemies_to_screen_width();

    /* Initialize enemy battlers (slots 8+) */
    for (uint16_t i = 0; i < bt.enemies_in_battle; i++) {
        battle_init_enemy_stats(&bt.battlers_table[FIRST_ENEMY_INDEX + i],
                                bt.enemies_in_battle_ids[i]);
    }

    layout_enemy_battle_positions();
    render_all_battle_sprites();
    load_character_window_palette();
    set_palette_upload_mode(24);
    bt.battle_mode_flag = 1;

    /* Play battle music based on first enemy */
    if (enemy_config_table) {
        uint8_t music_id = enemy_config_table[bt.enemies_in_battle_ids[0]].music;
        change_music(music_id);
    }

    blank_screen_and_wait_vblank();
    fade_in(1, 0);
}

/* The normal-mode (ow.battle_mode == 0) reinit tail (assembly lines
 * 410-480): battle party display + party re-init from current game state. */
static void btl_reinit_party(BattleRoutineState *st) {
    /* Initialize battle party display */
    initialize_battle_party(st->debug_enemy_level);

    /* Re-init party members from current game state */
    for (uint16_t slot = 0; slot < 6; slot++) {
        uint8_t member_id = game_state.party_members[slot];
        if (member_id == 0) continue;

        if (member_id >= 1 && member_id <= 4) {
            battle_init_player_stats(member_id, &bt.battlers_table[slot]);
        } else if (member_id >= 5) {
            /* NPC reinit — check AI table flag */
            if (npc_ai_table) {
                uint8_t ai_byte = npc_ai_table[member_id * 2];
                if ((ai_byte & 1) == 0) continue;

                uint8_t enemy_cfg = npc_ai_table[member_id * 2 + 1];
                Battler *b = &bt.battlers_table[slot];
                battle_init_enemy_stats(b, enemy_cfg);
                b->row = 0; /* assembly stores @VIRTUAL02 which is 0 at this point */

                /* Load NPC HP from game state */
                uint16_t npc_hp = game_state.party_npc_1_hp;
                b->hp_target = npc_hp;
                b->hp = npc_hp;
                b->pp_target = 0;
                b->pp = 0;
                b->ally_or_enemy = 0;
                b->npc_id = member_id;
            }
        }
    }

    redirect_show_hppp_windows();
}

/* Debug battle loop (assembly lines 482-740).
 * When ow.debug_flag is set, this loop lets the developer interactively
 * change enemy groups (SELECT), party composition (LEFT/RIGHT),
 * enemy level (UP/DOWN/X), preview PSI animations (A), and
 * swirl effects (B). Press START to proceed to actual battle.
 * Kept inline-blocking (debug-only; documented deferral). Returns true on
 * apply_debug_party (the caller re-runs the whole battle reinit), false on
 * START (proceed to battle). */
static bool btl_debug_loop(BattleRoutineState *st) {
    for (;;) {
        wait_for_vblank();
        update_battle_screen_effects();

        /* START: exit debug loop, start battle (line 486-488) */
        if (core.pad1_pressed & PAD_START)
            return false;

        /* SELECT: open enemy_select_mode to change battle group (lines 489-533) */
        if (core.pad1_pressed & PAD_SELECT) {
            uint16_t new_group = enemy_select_mode(bt.current_battle_group);
            bt.current_battle_group = new_group;
            /* Reload BG/palette/letterbox from tables for the new group */
            if (btl_entry_bg_table) {
                uint16_t offset = new_group * 4;
                st->bg_id = read_u16_le(&btl_entry_bg_table[offset]);
                st->palette_id = read_u16_le(&btl_entry_bg_table[offset + 2]);
            }
            if (btl_entry_ptr_table) {
                st->letterbox_style = btl_entry_ptr_table[new_group * 8 + 7];
            }
            goto apply_debug_party;
        }

        /* RIGHT: add next party member flag (lines 534-542) */
        if (core.pad1_autorepeat & PAD_RIGHT) {
            if (st->debug_party_flags < 15) {
                st->debug_party_flags++;
                goto apply_debug_party;
            }
        }
        /* LEFT: remove last party member flag (lines 543-551) */
        else if (core.pad1_autorepeat & PAD_LEFT) {
            if (st->debug_party_flags > 1) {
                st->debug_party_flags--;
                goto apply_debug_party;
            }
        }

        /* DOWN: decrease enemy level (lines 552-560) */
        if (core.pad1_autorepeat & PAD_DOWN) {
            if (st->debug_enemy_level > 1) {
                st->debug_enemy_level--;
                goto apply_debug_party;
            }
        }
        /* UP: increase enemy level (lines 561-569) */
        else if (core.pad1_autorepeat & PAD_UP) {
            if (st->debug_enemy_level < 99) {  /* MAX_LEVEL */
                st->debug_enemy_level++;
                goto apply_debug_party;
            }
        }

        /* X: set level to highest enemy level (lines 570-576) */
        if (core.pad1_pressed & PAD_X) {
            st->debug_enemy_level = bt.highest_enemy_level_in_battle;
            goto apply_debug_party;
        }

        /* A: cycle PSI animations (lines 577-588) */
        if (core.pad1_pressed & PAD_A) {
            show_psi_animation(bt.debugging_current_psi_animation);
            bt.debugging_current_psi_animation++;
            if (bt.debugging_current_psi_animation >= 34)
                bt.debugging_current_psi_animation = 0;
        }

        /* B: cycle swirl effects (lines 589-606) */
        if (core.pad1_pressed & PAD_B) {
            init_swirl_effect(bt.debugging_current_swirl, bt.debugging_current_swirl_flags);
            bt.debugging_current_swirl++;
            if (bt.debugging_current_swirl >= 8) {
                bt.debugging_current_swirl = 0;
                bt.debugging_current_swirl_flags = (bt.debugging_current_swirl_flags + 1) & 3;
            }
        }

        continue;

    apply_debug_party:
        /* Rebuild party_members[] from debug_party_flags bitmask (lines 607-740).
         * Bits 0-3 select Ness/Paula/Jeff/Poo respectively. */
        {
            uint8_t slot = 0;
            if (st->debug_party_flags & 1) game_state.party_members[slot++] = 1; /* Ness */
            if (st->debug_party_flags & 2) game_state.party_members[slot++] = 2; /* Paula */
            if (st->debug_party_flags & 4) game_state.party_members[slot++] = 3; /* Jeff */
            if (st->debug_party_flags & 8) game_state.party_members[slot++] = 4; /* Poo */
            game_state.player_controlled_party_count = slot;
            while (slot < 6) game_state.party_members[slot++] = 0;
        }
        return true;  /* Re-setup the entire battle scene */
    }
}

/* check_buzz_buzz through the initiative setup (assembly lines 741-1109,
 * minus the encounter texts): Buzz Buzz / possessed-party battlers, the
 * item-drop roll, initiative, and the battle text window. Returns the first
 * enemy's encounter text address (0 = none). */
static uint32_t btl_prep(BattleRoutineState *st) {
    /* Check if Buzz Buzz is in party (event flag) */
    if (event_flag_get(EVENT_FLAG_BUNBUN)) {
        Battler *buzz = &bt.battlers_table[TOTAL_PARTY_COUNT]; /* slot 6 */
        battle_init_enemy_stats(buzz, ENEMY_BUZZ_BUZZ);
        buzz->row = 1;
        buzz->ally_or_enemy = 0;
        buzz->npc_id = ENEMY_BUZZ_BUZZ;
    }

    /* Check if any party member is possessed → spawn Tiny Lil Ghost */
    for (uint16_t i = 0; i < TOTAL_PARTY_COUNT; i++) {
        uint8_t member_id = game_state.party_members[i];
        if (member_id == 0 || member_id > 4) continue;

        /* Check affliction group 1 (PERSISTENT_HARDHEAL) for POSSESSED */
        uint8_t *afflictions = party_characters[member_id - 1].afflictions;
        if (afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] == STATUS_1_POSSESSED) {
            Battler *ghost = &bt.battlers_table[TOTAL_PARTY_COUNT];
            battle_init_enemy_stats(ghost, ENEMY_TINY_LIL_GHOST);
            ghost->npc_id = ENEMY_TINY_LIL_GHOST;
            goto setup_item_drop;
        }
    }

setup_item_drop:
    redirect_show_hppp_windows();

    /* Select a random enemy for item drop */
    {
        uint16_t drop_enemy_slot = rand_limit(bt.enemies_in_battle);
        uint16_t drop_enemy_id = bt.enemies_in_battle_ids[drop_enemy_slot];

        if (enemy_config_table) {
            const EnemyData *edata = &enemy_config_table[drop_enemy_id];
            bt.item_dropped = edata->item_dropped;

            /* Apply rarity check */
            uint8_t rarity = edata->item_drop_rate;
            static const uint16_t rarity_masks[] = {
                0x7F, 0x3F, 0x1F, 0x0F, 0x07, 0x03, 0x01
            };
            if (rarity <= 6) {
                uint8_t r = rand_byte();
                if ((r & rarity_masks[rarity]) != 0) {
                    bt.item_dropped = 0;
                }
            }
            /* rarity >= 7: guaranteed drop (or no item set) */
        }
    }

    /* Consolation item check: if no item dropped, scan consolation table */
    if (bt.item_dropped == 0 && consolation_item_table) {
        for (uint16_t table_entry = 0; table_entry < 2; table_entry++) {
            for (uint16_t enemy_slot = 8; enemy_slot < BATTLER_COUNT; enemy_slot++) {
                Battler *b = &bt.battlers_table[enemy_slot];
                if (b->consciousness == 0) continue;

                /* Each consolation entry is 9 bytes: enemy_id (1) + 8 item slots */
                uint16_t entry_offset = table_entry * 9;
                if (consolation_item_table[entry_offset] == b->id) {
                    /* Pick random item from the 8 consolation slots + 1 offset */
                    uint16_t item_index = rand_limit(7);
                    bt.item_dropped = consolation_item_table[entry_offset + 1 + item_index];
                }
            }
        }
    }

    /* ================================================================
     * Phase 3: Initiative setup (lines 1008-1109)
     * ================================================================ */
    st->initiative_mode = 0;
    if (bt.battle_initiative == 1) {
        st->initiative_mode = INITIATIVE_PARTY_FIRST;
    } else if (bt.battle_initiative == 2) {
        st->initiative_mode = INITIATIVE_ENEMIES_FIRST;
    }
    bt.battle_initiative = 0;

    /* Open battle text window and announce encounter */
    create_window(0x0E);  /* WINDOW::TEXT_BATTLE */

    bt.current_attacker = FIRST_ENEMY_INDEX * sizeof(Battler);
    fix_attacker_name(1);

    /* Encounter text from the first enemy's config (the BTL_PREP push) */
    if (enemy_config_table) {
        return enemy_config_table[bt.enemies_in_battle_ids[0]].encounter_text_ptr;
    }
    return 0;
}

/* ================================================================
 * Phase 4: Start turn (lines 1110-1176)
 * ================================================================ */
static void btl_start_turn(BattleRoutineState *st) {
    st->turn_counter++;
    sort_battlers_into_rows();

    /* Calculate initiative for all battlers */
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        b->has_taken_turn = 0;

        if (b->consciousness == 0) continue;

        uint16_t init_val = battle_50pct_variance(b->speed);
        if (init_val == 0) init_val = 1;
        b->initiative = (uint8_t)init_val;
    }

    /* Clear char_struct::unknown94 for all 4 player characters */
    for (uint16_t i = 0; i < 4; i++) {
        party_characters[i].unknown94 = 0;
    }

    /* Reset the player-menu loop (the blocking original's block locals) */
    st->num_selected = 0;
    st->party_slot = 0;
}

/* The player-menu writeback (assembly lines 1404-1496): find the battler
 * slot for this party member and store the selected action/item/targeting.
 * Shared by the skip-menu and menu paths, like the blocking original. */
static void btl_store_action(uint16_t member_id, uint16_t selected_action) {
    /* Find the battler slot for this party member */
    for (uint16_t slot = 0; slot < BATTLER_COUNT; slot++) {
        Battler *b = &bt.battlers_table[slot];
        if (b->consciousness == 0) continue;
        if (b->ally_or_enemy != 0) continue;
        if (b->id != member_id) continue;

        /* Store action */
        b->current_action = selected_action;

        if (bt.battle_item_used != 0) {
            b->action_item_slot = bt.battle_menu_param1;
            b->current_action_argument = bt.battle_item_used;
        } else {
            b->action_item_slot = 0;
            b->current_action_argument = bt.battle_menu_param1;
        }

        /* Store targeting info */
        b->action_targetting = bt.battle_menu_targetting;
        b->current_target = bt.battle_menu_selected_target;

        /* If targeting type is 1 (single target by ID), find the slot index */
        if ((bt.battle_menu_targetting & 0xFF) == 1) {
            for (uint16_t search = 0; search < 6; search++) {
                Battler *tb = &bt.battlers_table[search];
                if (tb->consciousness == 0) continue;
                if (tb->npc_id != 0) continue;
                if (tb->id == bt.battle_menu_selected_target) {
                    b->current_target = (uint8_t)(search + 1);
                    break;
                }
            }
        }

        /* Set guarding flag */
        if (b->current_action == BACT_GUARD) {
            b->guarding = 1;
        } else {
            b->guarding = 0;
        }
        break;
    }
}

/* ================================================================
 * Phase 6: Enemy AI — select actions (lines 1497-1921)
 * ================================================================ */
static void btl_enemy_ai(BattleRoutineState *st) {
    {
        for (uint16_t bi = 0; bi < BATTLER_COUNT; bi++) {
            Battler *b = &bt.battlers_table[bi];

            /* Skip dead battlers that aren't Poo-with-mirror */
            if (b->consciousness == 0) {
                /* NPC party members (Pokey/King) proceed to action selection */
                if (b->npc_id == 0) {
                    if (b->id != PARTY_MEMBER_POO) goto next_battler_action;
                    if (bt.mirror_enemy == 0) goto next_battler_action;
                }
            } else if (b->ally_or_enemy != 1) {
                /* NPC party members (Pokey/King) proceed to action selection */
                if (b->npc_id == 0) {
                    if (b->id != PARTY_MEMBER_POO) goto next_battler_action;
                    if (bt.mirror_enemy == 0) goto next_battler_action;
                }
            }

            /* Check initiative skip */
            if (st->initiative_mode == 1 || st->initiative_mode == 4) {
                if (b->ally_or_enemy == 1) {
                    b->current_action = 0;
                    goto next_battler_action;
                }
            }
            if (st->initiative_mode == 2) {
                if (b->ally_or_enemy == 0) {
                    b->current_action = 0;
                    goto next_battler_action;
                }
            }

            /* Determine which enemy config to use */
            const EnemyData *econfig;
            if (b->ally_or_enemy == 0 && b->id == PARTY_MEMBER_POO && bt.mirror_enemy != 0) {
                econfig = &enemy_config_table[bt.mirror_enemy];
            } else {
                econfig = &enemy_config_table[b->id];
            }

            /* Select action based on action_order pattern */
            uint8_t action_index = 0;
            switch (econfig->action_order) {
            case 0: /* Random uniform: pick 0-3 */
                action_index = rand_byte() & 3;
                break;

            case 1: { /* Weighted: 0=50%, 1=25%, 2=12.5%, 3=12.5% */
                uint8_t r = rand_byte() & 7;
                if (r == 0)      action_index = 3;
                else if (r == 1) action_index = 2;
                else if (r <= 3) action_index = 1;
                else             action_index = 0;
                break;
            }

            case 2: { /* Cyclic: 0,1,2,3,0,1,2,3,... */
                action_index = b->action_order_var;
                b->action_order_var = (b->action_order_var + 1) & 3;
                break;
            }

            case 3: { /* Alternating pairs: (0,1), (2,3), (0,1), ... with random within pair */
                uint8_t pair = b->action_order_var * 2;
                action_index = pair + (rand_byte() & 1);
                b->action_order_var = (b->action_order_var + 1) & 1;
                break;
            }

            default:
                action_index = 0;
                break;
            }

            /* Apply selected action to battler */
            b->current_action = econfig->actions[action_index];
            b->current_action_argument = econfig->action_args[action_index];

            /* Handle ENEMY_EXTENDER action */
            if (b->current_action == BACT_ENEMY_EXTENDER) {
                if (b->ally_or_enemy == 0 && b->id == PARTY_MEMBER_POO) {
                    /* Poo mirror: extender arg is new mirror enemy */
                    bt.mirror_enemy = b->current_action_argument;
                    /* Re-select action with new mirror enemy config */
                    bi--;  /* Retry this battler */
                    continue;
                } else {
                    /* Regular enemy: extender changes enemy ID */
                    b->id = b->current_action_argument;
                    bi--;  /* Retry with new ID */
                    continue;
                }
            }

            /* Handle STEAL action */
            if (b->current_action == BACT_STEAL) {
                uint16_t stolen = select_stealable_item();
                b->current_action_argument = (uint8_t)stolen;
                b->initiative = 0;  /* Steal always goes last */
            }

            /* Set guard flag */
            if (b->current_action == BACT_ON_GUARD) {
                b->guarding = 1;
            } else {
                b->guarding = 0;
            }

            /* Choose target for this battler */
            choose_target(bi * sizeof(Battler));

next_battler_action:
            (void)0;
        }
    }
}

/* The Phase 8 run-away computation (assembly lines 1932-2078, minus the
 * result texts): speed comparison + the escape roll. */
static bool btl_run_success(BattleRoutineState *st) {
    uint16_t max_enemy_speed = 0;
    uint16_t max_player_speed = 0;
    bool boss_present = false;

    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0) continue;
        if (b->npc_id != 0) continue;

        if (b->ally_or_enemy == 1) {
            /* Enemy: check if boss */
            if (enemy_config_table && enemy_config_table[b->id].boss != 0) {
                boss_present = true;
                break;
            }
            /* Skip incapacitated enemies */
            uint8_t s0 = b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
            if (s0 == STATUS_0_UNCONSCIOUS || s0 == STATUS_0_DIAMONDIZED ||
                s0 == STATUS_0_PARALYZED) continue;
            uint8_t s2 = b->afflictions[STATUS_GROUP_TEMPORARY];
            if (s2 == STATUS_2_ASLEEP || s2 == STATUS_2_IMMOBILIZED ||
                s2 == STATUS_2_SOLIDIFIED) continue;

            if (b->speed > max_enemy_speed) {
                max_enemy_speed = b->speed;
            }
        } else {
            /* Player */
            if (b->speed > max_player_speed) {
                max_player_speed = b->speed;
            }
        }
    }

    bool run_success = false;
    if (boss_present) {
        /* Can't run from bosses */
        run_success = false;
    } else if (max_enemy_speed == 0) {
        run_success = true;
    } else if (st->initiative_mode == 4) {
        /* Party first + run = guaranteed */
        run_success = true;
    } else {
        /* Speed-based run check */
        uint16_t run_threshold = st->turn_counter * 10 + max_player_speed;
        if (run_threshold >= max_enemy_speed) {
            uint16_t roll = rand_limit(100);
            uint16_t diff = run_threshold - max_enemy_speed;
            if (roll < diff) {
                run_success = true;
            }
        }
    }
    return run_success;
}

/* execute_turns: find the battler with the highest initiative who hasn't
 * acted yet (assembly lines 2090-2114). Returns -1 when everyone acted. */
static int16_t btl_find_attacker(void) {
    int16_t best_battler = -1;
    uint16_t best_initiative = 0;

    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0) continue;
        if (b->has_taken_turn != 0) continue;
        if (b->initiative >= best_initiative) {
            best_initiative = b->initiative;
            best_battler = (int16_t)i;
        }
    }
    return best_battler;
}

/* execute_turns status overrides (assembly lines 2128-2280): paralyzed/
 * immobilized, asleep, solidified, can't-concentrate, homesick, and the
 * self-target check. All synchronous. */
static void btl_status_overrides(Battler *attacker) {
    uint8_t status0 = attacker->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];

    /* ---- Status override: Paralyzed/Immobilized ---- */
    if (status0 == STATUS_0_PARALYZED ||
        attacker->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_IMMOBILIZED) {

        /* Check if action is PSI (can still use PSI while paralyzed) */
        uint16_t action_id = attacker->current_action;
        bool exempt = false;
        if (battle_action_table &&
            battle_action_table[action_id].type == ACTION_TYPE_PSI) {
            exempt = true;
        }

        /* Check exempt actions (pray, final prayers, spy, mirror, no_effect) */
        if (action_id == BACT_PRAY ||
            (action_id >= BACT_FINAL_PRAYER_1 && action_id <= BACT_FINAL_PRAYER_9) ||
            action_id == BACT_SPY ||
            action_id == BACT_MIRROR ||
            action_id == BACT_NO_EFFECT) {
            exempt = true;
        }

        if (!exempt) {
            /* Override action: paralyzed → action 252, immobilized → action 254 */
            if (status0 == STATUS_0_PARALYZED) {
                attacker->current_action = BACT_ACTION_252;
            } else {
                attacker->current_action = BACT_ACTION_254;
            }
            attacker->action_item_slot = 0;
        }
    }

    /* ---- Status override: Asleep ---- */
    if (attacker->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_ASLEEP) {
        if (attacker->current_action != 0) {
            attacker->current_action = BACT_ACTION_253;
            attacker->action_item_slot = 0;
        }
    }

    /* ---- Status override: Solidified ---- */
    if (attacker->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_SOLIDIFIED) {
        if (attacker->current_action != 0) {
            attacker->current_action = BACT_ACTION_255;
            attacker->afflictions[STATUS_GROUP_TEMPORARY] = 0;
            attacker->action_item_slot = 0;
        }
    }

    /* ---- Status override: Can't concentrate (blocks PSI) ---- */
    if (attacker->afflictions[STATUS_GROUP_CONCENTRATION] != 0) {
        uint16_t action_id = attacker->current_action;
        if (battle_action_table && action_id != 0) {
            uint8_t action_type = battle_action_table[action_id].type;
            if (action_type == ACTION_TYPE_PSI) {
                attacker->current_action = BACT_ACTION_256;
            }
        }
    }

    /* ---- Status override: Homesick (1/8 chance of wasting turn) ---- */
    if (attacker->afflictions[STATUS_GROUP_HOMESICKNESS] == STATUS_5_HOMESICK) {
        if (attacker->current_action != 0) {
            if ((rand_byte() & 7) == 0) {
                attacker->current_action = BACT_ACTION_251;
                attacker->action_item_slot = 0;
            }
        }
    }

    /* Self-target check: if action direction is PARTY (0) and target is NONE (0) */
    if (battle_action_table) {
        uint16_t action_id = attacker->current_action;
        uint8_t direction = battle_action_table[action_id].direction;
        uint8_t target_type = battle_action_table[action_id].target;

        if (direction == ACTION_DIRECTION_PARTY && target_type == ACTION_TARGET_NONE) {
            if (attacker->ally_or_enemy == 0) {
                /* Ally self-targeting */
                attacker->action_targetting = 1;
                uint16_t self_index = (bt.current_attacker / sizeof(Battler)) + 1;
                attacker->current_target = (uint8_t)self_index;
            } else {
                /* Enemy self-targeting */
                attacker->action_targetting = 17;
                uint16_t self_index = bt.current_attacker / sizeof(Battler);
                set_battler_target(bt.current_attacker, self_index);
            }
        }
    }
}

/* execute_turns targeting setup (assembly lines 2398-2520): enemy target
 * re-pick, set targets by action, untargettable removal, the mushroomized/
 * strange retarget rolls, steal validation, names and the party highlight.
 * Updates st->retargeted. All synchronous. */
static void btl_exec_setup_targets(BattleRoutineState *st, Battler *attacker) {
    /* Re-choose target for enemies */
    if (attacker->ally_or_enemy == 1) {
        choose_target(bt.current_attacker);

        /* Re-select stealable item for steal actions */
        if (attacker->current_action == BACT_STEAL) {
            uint16_t stolen = select_stealable_item();
            attacker->current_action_argument = (uint8_t)stolen;
        }
    }

    /* Set targets by action */
    set_battler_targets_by_action(bt.current_attacker);

    /* For party members with target-none actions: remove untargettable then re-pick if empty */
    if (attacker->ally_or_enemy == 0 && battle_action_table) {
        uint8_t direction = battle_action_table[attacker->current_action].direction;
        if (direction == ACTION_DIRECTION_PARTY) {
            battle_remove_status_untargettable_targets();
            if (bt.battler_target_flags == 0) {
                choose_target(bt.current_attacker);
                set_battler_targets_by_action(bt.current_attacker);
                battle_remove_status_untargettable_targets();
            }
        }
    }

    /* Mushroomized/strange retargeting */
    if (attacker->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] == STATUS_1_MUSHROOMIZED) {
        if (rand_limit(100) < MUSHROOMIZED_TARGET_CHANGE_CHANCE) {
            st->retargeted = 1;
        }
    }
    if (attacker->afflictions[STATUS_GROUP_STRANGENESS] == STATUS_3_STRANGE) {
        st->retargeted = 1;
    }
    if (st->retargeted && battle_action_table) {
        uint8_t target_type = battle_action_table[attacker->current_action].target;
        if (target_type != ACTION_TARGET_NONE) {
            do {
                battle_feeling_strange_retargeting();
                battle_remove_status_untargettable_targets();
            } while (bt.battler_target_flags == 0);
        } else {
            st->retargeted = 0;
        }
    }

    /* Validate steal target */
    if (attacker->current_action == BACT_STEAL) {
        if (!is_item_stealable(attacker->current_action_argument)) {
            attacker->current_action_argument = 0;
        }
    }

    /* ---- Prepare and execute the action ---- */
    fix_attacker_name(0);
    set_current_item_far(attacker->current_action_argument);
    set_target_if_targeted();

    /* Highlight party member during their turn */
    if (attacker->ally_or_enemy == 0 && attacker->id <= PLAYER_CHAR_COUNT) {
        for (uint16_t slot = 0; slot < 6; slot++) {
            if (game_state.party_members[slot] == attacker->id) {
                select_battle_menu_character_far(slot);
                break;
            }
        }
    }
}

/* enemies_are_dead bookkeeping (assembly lines 3060-3105): money deposit +
 * the per-member EXP split. */
static void btl_victory_money_exp(void) {
    /* Deposit battle money */
    uint32_t deposited = deposit_into_atm(bt.battle_money_scratch);

    /* Add money to game total (assembly adds deposited amount, not original) */
    {
        uint32_t money = read_u32_le(game_state.unknownC4);
        money += deposited;
        game_state.unknownC4[0] = (uint8_t)money;
        game_state.unknownC4[1] = (uint8_t)(money >> 8);
        game_state.unknownC4[2] = (uint8_t)(money >> 16);
        game_state.unknownC4[3] = (uint8_t)(money >> 24);
    }

    /* Calculate EXP per surviving party member */
    {
        uint16_t alive_count = battle_count_chars(0);
        if (alive_count > 0) {
            /* Add (alive_count - 1) bonus EXP to base, then divide */
            bt.battle_exp_scratch += (alive_count - 1);
            bt.battle_exp_scratch /= alive_count;
        }
    }
}

/* battle_ending mirror restore + cleanup (assembly lines 3190-3240). */
static void btl_ending_cleanup(void) {
    /* Restore mirror if still active */
    if (bt.mirror_enemy != 0) {
        for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
            Battler *b = &bt.battlers_table[i];
            if (b->consciousness == 0) continue;
            if (b->ally_or_enemy != 0) continue;
            if (b->id != PARTY_MEMBER_POO) continue;

            /* Save current status before restore */
            uint8_t saved_status0 = b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
            bt.mirror_enemy = 0;
            battle_copy_mirror_data(b, &bt.mirror_battler_backup);
            b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = saved_status0;
            check_dead_players();
            break;
        }
    }

    /* Cleanup */
    battle_reset_post_battle_stats();
    game_state.auto_fight_enable = 0;
    bt.battle_mode_flag = 0;
}

/* GAME_MODE_BATTLE step — run-to-completion port of battle_routine(). The
 * former goto labels map onto the BTL_* phases (see BattleRoutineState,
 * mode_stack.h); the synchronous chunks live in the btl_* helpers above. */
StepResult mode_step_battle(ModeState *ms) {
    BattleRoutineState *st = &ms->battle;
    static ModeState child_init;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch ((BattleRoutinePhase)st->phase) {

        case BTL_BEGIN:
            /* Phase 1: initial setup (lines 26-133) */
            btl_begin(st);
            st->phase = BTL_REINIT;
            break;

        case BTL_REINIT:
            /* reinit_battle (lines 134-480) */
            btl_reinit_scene(st);
            if (ow.battle_mode != 0) {
                st->phase = BTL_PREP;  /* goto check_buzz_buzz */
                break;
            }
            btl_reinit_party(st);
            /* The blocking original's window_tick(): same work, the pump
             * owns the yield. */
            window_tick_work();
            st->phase = BTL_DEBUG;
            return STEP_RESULT_CONTINUE();

        case BTL_DEBUG:
            if (ow.debug_flag && btl_debug_loop(st)) {
                st->phase = BTL_REINIT;  /* apply_debug_party: re-setup the scene */
                break;
            }
            /* start_battle_music: play music for current enemy group */
            if (enemy_config_table) {
                uint8_t music_id = enemy_config_table[bt.enemies_in_battle_ids[0]].music;
                change_music(music_id);
            }
            st->phase = BTL_PREP;
            break;

        case BTL_PREP: {
            /* check_buzz_buzz .. initiative setup; push the encounter text */
            uint32_t encounter_addr = btl_prep(st);
            st->announce_i = 0;
            st->announce_stage = 0;
            st->phase = BTL_AURA;
            if (encounter_addr != 0 &&
                battle_push_text(&child_init, encounter_addr))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            break;
        }

        case BTL_AURA:
            dt.blinking_triangle_flag = 0;
            /* Announce "Green/blue/red aura!" for party-first initiative */
            st->phase = BTL_ANNOUNCE;
            if (st->initiative_mode == INITIATIVE_PARTY_FIRST &&
                battle_push_text(&child_init, MSG_BTL8_SURPRISE_ATTACK_PLAYER))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            break;

        case BTL_ANNOUNCE:
            /* Announce initial enemy statuses (asleep, sealed, strange) —
             * resumable per enemy (announce_i) and per text (announce_stage). */
            dt.blinking_triangle_flag = 0;
            while (st->announce_i < bt.enemies_in_battle) {
                Battler *enemy = &bt.battlers_table[FIRST_ENEMY_INDEX + st->announce_i];
                if (st->announce_stage == 0) {
                    bt.current_target =
                        (uint16_t)((FIRST_ENEMY_INDEX + st->announce_i) * sizeof(Battler));
                    fix_target_name();
                    st->announce_stage = 1;
                    if (enemy->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_ASLEEP &&
                        battle_push_text(&child_init, MSG_BTL0_STATUS_ASLEEP_AT_START))
                        return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
                }
                if (st->announce_stage == 1) {
                    st->announce_stage = 2;
                    if (enemy->afflictions[STATUS_GROUP_CONCENTRATION] != 0 &&
                        battle_push_text(&child_init, MSG_BTL0_STATUS_PSI_BLOCKED_AT_START))
                        return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
                }
                st->announce_stage = 0;
                st->announce_i++;
                if (enemy->afflictions[STATUS_GROUP_STRANGENESS] == STATUS_3_STRANGE &&
                    battle_push_text(&child_init, MSG_BTL0_STATUS_STRANGE_AT_START))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            }
            redirect_close_focus_window();
            st->post_battle_exit = 0;
            bt.special_defeat = 0;
            st->phase = BTL_TURN;  /* goto check_new_turn (post_battle_exit == 0) */
            break;

        case BTL_TURN:
            /* start_turn (lines 1110-1176) */
            btl_start_turn(st);
            st->phase = BTL_MENU;
            break;

        case BTL_MENU:
            /* Phase 5: player menu loop (lines 1177-1496) */
            while (st->party_slot < 6) {
                check_dead_players();
                if (battle_count_chars(0) == 0) {
                    create_window(0x0E);
                    st->phase = BTL_END_CHECK;
                    goto menu_done;
                }

                uint8_t member_id = game_state.party_members[st->party_slot];
                if (member_id == 0 || member_id > 4) {
                    st->party_slot++;
                    continue;
                }

                /* Check if this character should skip the menu */
                bool skip_menu = false;

                if (st->initiative_mode == 2 || st->initiative_mode == 3 ||
                    st->initiative_mode == 4) {
                    skip_menu = true;
                }

                /* Poo with mirror active skips menu */
                if (!skip_menu && member_id == PARTY_MEMBER_POO && bt.mirror_enemy != 0) {
                    skip_menu = true;
                }

                /* Check for incapacitation */
                if (!skip_menu) {
                    uint8_t *aff = party_characters[member_id - 1].afflictions;
                    if (aff[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS ||
                        aff[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_DIAMONDIZED) {
                        skip_menu = true;
                    }
                    uint8_t temp_status = aff[STATUS_GROUP_TEMPORARY];
                    if (temp_status == STATUS_2_ASLEEP || temp_status == STATUS_2_SOLIDIFIED) {
                        skip_menu = true;
                    }
                }

                if (skip_menu) {
                    bt.battle_item_used = 0;
                    btl_store_action(member_id, BACT_NO_EFFECT);
                    st->party_slot++;
                    continue;
                }

                /* Show battle menu for this character */
                select_battle_menu_character_far(st->party_slot);
                child_init = (ModeState){0};
                child_init.battle_menu.phase        = BM_ENTER;
                child_init.battle_menu.char_id      = member_id;
                child_init.battle_menu.num_selected = st->num_selected;
                st->phase = BTL_MENU_RESULT;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_MENU, &child_init);
            }
            st->phase = BTL_ENEMY_AI;
        menu_done:
            break;

        case BTL_MENU_RESULT: {
            uint16_t selected_action = (uint16_t)mode_child_result();
            uint8_t member_id = game_state.party_members[st->party_slot];

            clear_battle_menu_character_indicator_far();
            redirect_close_focus_window();

            /* Check for Giygas battle abort */
            if (ow.battle_mode != 0 && selected_action == 0xFFFF) {
                st->battle_result = 0;
                st->phase = BTL_ENDING;
                break;
            }

            /* Handle run away */
            if (selected_action == BACT_RUN_AWAY) {
                selected_action = BACT_USE_NO_EFFECT;
                if (st->initiative_mode == 1) {
                    st->initiative_mode = 4;
                } else {
                    st->initiative_mode = 3;
                }
                st->run_attempt = 1;
            }

            /* Handle "back" (action == 0 from the battle menu) */
            if (selected_action == 0) {
                if (st->num_selected == 0) {
                    /* The original's party_slot--/continue: retry this slot */
                    st->phase = BTL_MENU;
                    break;
                }
                st->num_selected--;
                st->party_slot = bt.party_members_with_selected_actions[st->num_selected];
                st->phase = BTL_MENU;
                break;
            }

            /* Reinit battle if -1 returned (debug instant win, battle_mode 0) */
            if (selected_action == 0xFFFF) {
                st->phase = BTL_REINIT;
                break;
            }

            /* Store selected action index */
            bt.party_members_with_selected_actions[st->num_selected] = st->party_slot;
            st->num_selected++;

            /* Map action 1 to 0 */
            if (selected_action == 1) selected_action = 0;

            btl_store_action(member_id, selected_action);
            st->party_slot++;
            st->phase = BTL_MENU;
            break;
        }

        case BTL_ENEMY_AI:
            /* Phase 6: enemy AI (lines 1497-1921) */
            btl_enemy_ai(st);

            /* Phase 7: announce enemy-first initiative (lines 1922-1931) */
            create_window(0x0E);
            st->phase = BTL_RUN_CHECK;
            if (st->initiative_mode == INITIATIVE_ENEMIES_FIRST &&
                battle_push_text(&child_init, MSG_BTL8_SURPRISE_ATTACK_ENEMY))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            break;

        case BTL_RUN_CHECK:
            /* Phase 8: run away check (lines 1932-2078) */
            dt.blinking_triangle_flag = 0;
            if (st->run_attempt) {
                if (btl_run_success(st)) {
                    st->phase = BTL_RUN_SUCCESS;
                    if (battle_push_text(&child_init, MSG_BTL0_ESCAPE_SUCCESS))
                        return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
                    break;
                }
                st->run_attempt = 0;
                st->phase = BTL_RUN_FAIL;
                if (battle_push_text(&child_init, MSG_BTL0_ESCAPE_FAILED))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
                break;
            }
            /* Clear initiative mode for next turn */
            st->initiative_mode = 0;
            st->phase = BTL_TURN_CONT;
            break;

        case BTL_RUN_SUCCESS:
            dt.blinking_triangle_flag = 0;
            st->battle_result = 0;
            st->phase = BTL_ENDING;
            break;

        case BTL_RUN_FAIL:
            dt.blinking_triangle_flag = 0;
            /* Clear initiative mode for next turn */
            st->initiative_mode = 0;
            st->phase = BTL_TURN_CONT;
            break;

        case BTL_EXEC: {
            /* Phase 9: execute_turns (lines 2085-2926) */
            check_dead_players();
            if (battle_count_chars(0) == 0) { st->phase = BTL_END_CHECK; break; }
            if (battle_count_chars(1) == 0) { st->phase = BTL_END_CHECK; break; }

            /* Find battler with highest initiative who hasn't acted yet */
            int16_t best_battler = btl_find_attacker();
            if (best_battler < 0) {
                st->phase = BTL_CLOSE_WINDOW;
                break;
            }

            clear_focus_window_content_far();
            bt.current_attacker = best_battler * sizeof(Battler);
            st->attacker = best_battler;
            Battler *attacker = &bt.battlers_table[best_battler];
            attacker->has_taken_turn = 1;

            /* Check if attacker is incapacitated */
            uint8_t status0 = attacker->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
            if (status0 == STATUS_0_UNCONSCIOUS || status0 == STATUS_0_DIAMONDIZED) {
                st->phase = BTL_TURN_CONT;
                break;
            }

            /* Status overrides + the self-target check */
            btl_status_overrides(attacker);

            /* Set up names and status damage */
            st->retargeted = 0;
            bt.current_target = bt.current_attacker;
            fix_attacker_name(0);
            fix_target_name();

            /* Status damage (nauseous, poisoned, sunstroke, cold) */
            st->status_damage = 0;
            uint32_t dmg_text = 0;
            status0 = attacker->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];

            if (status0 == STATUS_0_NAUSEOUS) {
                st->status_damage = battle_25pct_variance(20);
                dmg_text = MSG_BTL4_STATUS_SICK_HP_DAMAGE;
            } else if (status0 == STATUS_0_POISONED) {
                st->status_damage = battle_25pct_variance(20);
                dmg_text = MSG_BTL4_STATUS_POISON_HP_DAMAGE;
            } else if (status0 == STATUS_0_SUNSTROKE) {
                st->status_damage = battle_25pct_variance(4);
                dmg_text = MSG_BTL4_STATUS_DIZZY_HP_DAMAGE;
            } else if (status0 == STATUS_0_COLD) {
                st->status_damage = battle_25pct_variance(4);
                dmg_text = MSG_BTL4_STATUS_SNEEZE_HP_DAMAGE;
            }
            st->phase = BTL_EXEC_DMG;
            if (dmg_text != 0 &&
                battle_push_text_ex(&child_init, dmg_text, false,
                                    true, st->status_damage))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            break;
        }

        case BTL_EXEC_DMG: {
            dt.blinking_triangle_flag = 0;
            Battler *attacker = &bt.battlers_table[st->attacker];

            /* Apply status damage */
            if (st->status_damage > 0) {
                battle_lose_hp_status(attacker, st->status_damage);

                if (attacker->hp == 0) {
                    battle_ko_target(attacker);
                    if (battle_count_chars(0) == 0) { st->phase = BTL_END_CHECK; break; }
                    if (battle_count_chars(1) == 0) { st->phase = BTL_END_CHECK; break; }
                    st->phase = BTL_TURN_CONT;
                    break;
                }
            }
            st->phase = BTL_EXEC_SETUP;
            break;
        }

        case BTL_EXEC_SETUP: {
            Battler *attacker = &bt.battlers_table[st->attacker];

            /* Targeting setup, retarget rolls, steal validation, highlight */
            btl_exec_setup_targets(st, attacker);

            /* Check PP cost */
            if (battle_action_table) {
                uint8_t pp_cost = battle_action_table[attacker->current_action].pp_cost;
                if (pp_cost > 0) {
                    if (pp_cost > attacker->pp_target) {
                        st->phase = BTL_AFTER_ACTION;  /* goto after_action */
                        if (battle_push_text(&child_init, MSG_BTL6_NOT_ENOUGH_PP_BATTLE))
                            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
                        break;
                    }
                    set_battler_pp_from_target(bt.current_attacker, pp_cost);
                }
            }

            /* Enemy attack palette effect */
            if (attacker->ally_or_enemy == 1 && attacker->current_action != 0 &&
                battle_action_table) {
                uint8_t action_type = battle_action_table[attacker->current_action].type;
                if (action_type == ACTION_TYPE_PHYSICAL ||
                    action_type == ACTION_TYPE_PIERCING_PHYSICAL) {
                    load_attack_palette(1);
                } else if (action_type == ACTION_TYPE_PSI) {
                    load_attack_palette(2);
                } else if (action_type == ACTION_TYPE_OTHER) {
                    load_attack_palette(3);
                }
            }

            /* Animate attacker (12-frame bob) */
            attacker->shake_timer = 12;
            child_init = (ModeState){0};
            child_init.battle_wait.kind = BW_FRAMES;
            child_init.battle_wait.remaining = 12;
            st->phase = BTL_EXEC_RETARGET1;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_WAIT, &child_init);
        }

        case BTL_EXEC_RETARGET1: {
            /* Display status text for confused/mushroomized */
            Battler *attacker = &bt.battlers_table[st->attacker];
            st->phase = BTL_EXEC_RETARGET2;
            if (st->retargeted &&
                attacker->afflictions[STATUS_GROUP_STRANGENESS] == STATUS_3_STRANGE &&
                battle_push_text(&child_init, MSG_BTL0_ACTING_UNUSUAL))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            break;
        }

        case BTL_EXEC_RETARGET2: {
            dt.blinking_triangle_flag = 0;
            Battler *attacker = &bt.battlers_table[st->attacker];
            st->phase = BTL_EXEC_DESC;
            if (st->retargeted &&
                attacker->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] == STATUS_1_MUSHROOMIZED &&
                battle_push_text(&child_init, MSG_BTL0_ACTING_FUNKY))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            break;
        }

        case BTL_EXEC_DESC: {
            dt.blinking_triangle_flag = 0;
            Battler *attacker = &bt.battlers_table[st->attacker];
            st->phase = BTL_EXEC_ACT;
            /* Display action description text (display_text_with_prompt_addr) */
            if (battle_action_table) {
                uint32_t desc_addr =
                    battle_action_table[attacker->current_action].description_text_pointer;
                if (desc_addr != 0 &&
                    battle_push_text_ex(&child_init, desc_addr, true, false, 0))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            }
            break;
        }

        case BTL_EXEC_ACT: {
            dt.blinking_triangle_flag = 0;
            Battler *attacker = &bt.battlers_table[st->attacker];

            /* Skip if action is 0 (no effect) */
            if (attacker->current_action == 0) {
                st->phase = BTL_AFTER_ACTION;
                break;
            }

            /* Wait for PSI animation to complete */
            st->target_i = 0;
            child_init = (ModeState){0};
            child_init.battle_wait.kind = BW_PSI_ANIM;
            st->phase = BTL_TARGET;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_WAIT, &child_init);
        }

        case BTL_TARGET: {
            /* ---- Target loop head: apply action to each targeted battler.
             * One iteration per entry; re-entering via `break` (no yield)
             * replaces the former inner while loop so the action execution
             * can yield mid-loop. ---- */
            dt.blinking_triangle_flag = 0;
            Battler *attacker = &bt.battlers_table[st->attacker];

            if (st->target_i >= BATTLER_COUNT) {
                st->phase = BTL_AFTER_ACTION;
                break;
            }

            uint16_t target_i = st->target_i;
            if (!battle_is_char_targeted(target_i)) {
                st->target_i++;
                break;  /* next target, no yield */
            }

            bt.current_target = target_i * sizeof(Battler);
            Battler *target = &bt.battlers_table[target_i];
            fix_target_name();

            /* Check if target is dead — only some actions can target dead battlers */
            if (target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS) {
                bool can_target_dead = false;
                for (uint16_t di = 0; dead_targettable_actions[di] != 0; di++) {
                    if (dead_targettable_actions[di] == attacker->current_action) {
                        can_target_dead = true;
                        break;
                    }
                }
                if (!can_target_dead) {
                    st->target_i++;  /* the resume re-enters the loop head */
                    if (battle_push_text(&child_init, MSG_BTL4_RESULT_TARGET_ALREADY_GONE))
                        return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
                    dt.blinking_triangle_flag = 0;  /* unresolvable: epilogue inline */
                    break;
                }
            }

            /* Execute the action callback: converted actions are a
             * GAME_MODE_BATTLE_ACTION STEP_PUSH (resume in BTL_TARGET_POST);
             * unconverted ones run inline-blocking through the same dispatch
             * (the battle_actions.c long tail, convertible incrementally). */
            st->phase = BTL_TARGET_POST;
            if (battle_action_table) {
                uint32_t func_ptr =
                    battle_action_table[attacker->current_action].battle_function_pointer;
                if (func_ptr != 0 && battle_action_dispatch(func_ptr, &child_init))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ACTION, &child_init);
            }
            break;  /* ran inline (or no function): post checks, no yield */
        }

        case BTL_TARGET_POST: {
            /* ---- Post-action checks for the current target ---- */
            check_dead_players();
            ow.redraw_all_windows = 1;

            if (battle_count_chars(0) == 0 || battle_count_chars(1) == 0) {
                consume_used_battle_item();
                st->phase = BTL_END_CHECK;
                break;
            }

            /* Handle special defeat codes */
            if (bt.special_defeat == 3) {
                st->battle_result = 0;
                st->phase = BTL_ENDING;
                break;
            }
            if (bt.special_defeat == 2) {
                consume_used_battle_item();
                st->phase = BTL_VICTORY;  /* goto enemies_are_dead */
                break;
            }
            if (bt.special_defeat == 1) {
                st->battle_result = 2;
                st->phase = BTL_ENDING;
                break;
            }

            /* Wait for screen effects, then the next target */
            st->target_i++;
            child_init = (ModeState){0};
            child_init.battle_wait.kind = BW_SCREEN_EFFECT;
            st->phase = BTL_TARGET;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_WAIT, &child_init);
        }

        case BTL_AFTER_ACTION: {
            /* ---- Post-action processing (lines 2927-3035) ---- */
            dt.blinking_triangle_flag = 0;
            Battler *attacker = &bt.battlers_table[st->attacker];
            st->phase = BTL_AFTER_STATUS;

            /* Consume item if attacker is a party member */
            if (attacker->ally_or_enemy == 0) {
                consume_used_battle_item();

                /* Mirror turn timer countdown */
                if (bt.mirror_enemy != 0 && attacker->id == PARTY_MEMBER_POO) {
                    bt.mirror_turn_timer--;
                    if (bt.mirror_turn_timer == 0) {
                        bt.mirror_enemy = 0;
                        battle_copy_mirror_data(attacker, &bt.mirror_battler_backup);
                        if (battle_push_text(&child_init, MSG_BTL5_MORPH_NEUTRALIZED))
                            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
                    }
                }
            }
            break;
        }

        case BTL_AFTER_STATUS: {
            dt.blinking_triangle_flag = 0;
            Battler *attacker = &bt.battlers_table[st->attacker];

            /* Clear menu indicator */
            clear_battle_menu_character_indicator_far();

            /* Post-action status recovery */
            check_dead_players();
            bt.current_target = bt.current_attacker;
            fix_target_name();

            /* Check for status recovery (the affliction clear runs after the
             * text, like the assembly — see BTL_AFTER_CURED) */
            uint8_t temp_status = attacker->afflictions[STATUS_GROUP_TEMPORARY];
            uint32_t cured_text = 0;
            if (temp_status == STATUS_2_ASLEEP) {
                /* 1/4 chance of waking up */
                if ((rand_byte() & 3) == 0) {
                    cured_text = MSG_BTL5_CURED_ASLEEP;
                }
            } else if (temp_status == STATUS_2_IMMOBILIZED) {
                /* CHANCE_OF_BODY_MOVING_AGAIN% chance of recovery */
                if (rand_limit(100) < (100 - CHANCE_OF_BODY_MOVING_AGAIN)) {
                    cured_text = MSG_BTL5_CURED_IMMOBILIZED;
                }
            } else if (temp_status == STATUS_2_SOLIDIFIED) {
                /* Always recover from solidified after acting */
                cured_text = MSG_BTL5_CURED_SOLIDIFIED;
            }
            if (cured_text != 0) {
                st->phase = BTL_AFTER_CURED;
                if (battle_push_text(&child_init, cured_text))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
                break;
            }
            st->phase = BTL_AFTER_CONC;
            break;
        }

        case BTL_AFTER_CURED: {
            dt.blinking_triangle_flag = 0;
            Battler *attacker = &bt.battlers_table[st->attacker];
            attacker->afflictions[STATUS_GROUP_TEMPORARY] = 0;
            st->phase = BTL_AFTER_CONC;
            break;
        }

        case BTL_AFTER_CONC: {
            /* Concentration timer countdown */
            Battler *attacker = &bt.battlers_table[st->attacker];
            st->phase = BTL_AFTER_TAIL;
            uint8_t conc = attacker->afflictions[STATUS_GROUP_CONCENTRATION];
            if (conc > 0) {
                conc--;
                attacker->afflictions[STATUS_GROUP_CONCENTRATION] = conc;
                if (conc == 0 &&
                    battle_push_text(&child_init, MSG_BTL5_CURED_PSI_BLOCKED))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            }
            break;
        }

        case BTL_AFTER_TAIL:
            dt.blinking_triangle_flag = 0;
            /* Clear alt spritemaps for all battlers */
            for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
                bt.battlers_table[i].use_alt_spritemap = 0;
            }
            check_dead_players();
            redirect_show_hppp_windows();
            st->phase = BTL_END_CHECK;
            break;

        case BTL_END_CHECK:
            /* Phase 10: battle end checks (lines 3036-3159) */
            if (battle_count_chars(0) == 0) {
                /* Party defeated */
                st->battle_result = 1;
                reset_hppp_rolling();
                st->phase = BTL_DEFEAT_DONE;
                if (battle_push_text(&child_init, MSG_BTL8_ENEMY_VICTORY))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
                break;
            }
            st->phase = BTL_END_CHECK2;
            break;

        case BTL_DEFEAT_DONE:
            dt.blinking_triangle_flag = 0;
            st->post_battle_exit = 1;
            st->phase = BTL_END_CHECK2;
            break;

        case BTL_END_CHECK2:
            if (battle_count_chars(1) == 0) {
                st->phase = BTL_VICTORY;  /* goto enemies_are_dead */
                break;
            }
            st->phase = BTL_TURN_CONT;
            break;

        case BTL_VICTORY: {
            /* enemies_are_dead (lines 3050-3158) */
            st->battle_result = 0;
            reset_hppp_rolling();
            bt.letterbox_effect_ending = 1;
            bt.enable_background_darkening = 1;

            btl_victory_money_exp();

            /* Display victory message with EXP amount */
            uint32_t victory_addr = (bt.current_battle_group >= 0x01C0)  /* ENEMY_GROUP_BOSS_START */
                ? MSG_BTL8_PLAYER_VICTORY_BOSS
                : MSG_BTL8_PLAYER_VICTORY;
            st->phase = BTL_VICTORY_DROP;
            if (battle_push_text_ex(&child_init, victory_addr, false,
                                    true, bt.battle_exp_scratch))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            break;
        }

        case BTL_VICTORY_DROP:
            dt.blinking_triangle_flag = 0;
            st->exp_i = 0;
            st->phase = BTL_VICTORY_EXP;
            /* Announce item drop */
            if (bt.item_dropped != 0) {
                set_current_item_far((uint8_t)bt.item_dropped);
                if (battle_push_text(&child_init, MSG_BTL8_ENEMY_PRESENT_DROPPED))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            }
            break;

        case BTL_VICTORY_EXP:
            /* Distribute EXP to all conscious, non-NPC, non-dead party
             * members. gain_exp(1, ...) ran here in the blocking original;
             * the prepare half runs inline and any pending level-up runs as
             * a pushed GAME_MODE_LEVEL_UP child (was the pump bridge). */
            dt.blinking_triangle_flag = 0;
            while (st->exp_i < BATTLER_COUNT) {
                Battler *b = &bt.battlers_table[st->exp_i++];
                if (b->consciousness == 0) continue;
                if (b->ally_or_enemy != 0) continue;
                if (b->npc_id != 0) continue;
                if (b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS) continue;
                if (b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_DIAMONDIZED) continue;

                if (gain_exp_prepare(b->id, bt.battle_exp_scratch)) {
                    level_up_make_init(&child_init, b->id);
                    /* phase stays BTL_VICTORY_EXP — the resume re-enters the loop */
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_LEVEL_UP, &child_init);
                }
            }
            st->post_battle_exit = 1;
            st->phase = BTL_TURN_CONT;
            break;

        case BTL_TURN_CONT:
            /* check_turn_continue */
            if (st->post_battle_exit == 0) {
                st->phase = BTL_EXEC;  /* goto execute_turns */
                break;
            }
            st->phase = BTL_CLOSE_WINDOW;
            break;

        case BTL_CLOSE_WINDOW:
            /* close_battle_window + check_new_turn */
            redirect_close_focus_window();
            if (st->post_battle_exit == 0) {
                st->phase = BTL_TURN;  /* goto start_turn */
                break;
            }
            st->phase = BTL_ENDING;
            break;

        case BTL_ENDING:
            /* Phase 11: battle ending (lines 3176-3306) */
            reset_hppp_rolling();

            /* Wait for HP/PP meters to stabilize */
            child_init = (ModeState){0};
            child_init.battle_wait.kind = BW_HPPP_STABLE;
            st->phase = BTL_ENDING_MIRROR;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_WAIT, &child_init);

        case BTL_ENDING_MIRROR:
            /* Mirror restore + post-battle cleanup */
            btl_ending_cleanup();

            if (ow.battle_mode == 0) {
                /* Debug mode: loop back to reinit */
                st->phase = BTL_REINIT;
                break;
            }

            /* Fade out; the former loop body (update_battle_screen_effects)
             * is the GAME_MODE_FADE_WAIT step's FADE_TICK_BATTLE_EFFECTS tick. */
            fade_out(1, 0);
            child_init = (ModeState){0};
            child_init.fade_wait.tick_kind = FADE_TICK_BATTLE_EFFECTS;
            st->phase = BTL_EXIT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_FADE_WAIT, &child_init);

        case BTL_EXIT:
        default:
            clear_hppp_window_header();
            force_blank_and_wait_vblank();
            close_all_windows_and_hide_hppp();
            clear_battle_visual_effects();
            return STEP_RESULT_POP(st->battle_result);
        }
    }
}

/*
 * UPDATE_NPC_PARTY_LINEUP (asm/overworld/party/update_npc_party_lineup.asm)
 *
 * After update_party sorts the party, this syncs the game_state NPC tracking
 * fields (party_npc_1/2 and their HP) with the sorted party_members array.
 * Counts player-controlled members (IDs 1-4), then compares existing NPC
 * slots against the new ordering, swapping or reassigning as needed.
 */
static uint16_t get_npc_max_hp(uint8_t member_id) {
    if (!npc_ai_table || !enemy_config_table) return 0;
    uint8_t enemy_idx = npc_ai_table[member_id * 2 + 1];
    return enemy_config_table[enemy_idx].hp;
}

void update_npc_party_lineup(void) {
    /* Count player-controlled members (IDs 1-4; 0 = end of list, >= 5 = NPC) */
    uint16_t player_count = 0;
    while (player_count < 6) {
        uint8_t member = game_state.party_members[player_count];
        if (member == 0 || member > 4) break;
        player_count++;
    }
    game_state.player_controlled_party_count = (uint8_t)player_count;

    uint8_t saved_npc_1 = game_state.party_npc_1;
    uint8_t first_npc_slot = game_state.party_members[player_count];

    if (game_state.party_npc_1 == first_npc_slot) {
        /* NPC 1 is already in the correct slot; check NPC 2 */
        uint8_t second_npc_slot = game_state.party_members[player_count + 1];
        if (game_state.party_npc_2 != second_npc_slot) {
            game_state.party_npc_2 = second_npc_slot;
            game_state.party_npc_2_hp = get_npc_max_hp(second_npc_slot);
        }
        return;
    }

    /* NPC 1 doesn't match first slot; check if NPC 2 is there (swap case) */
    if (game_state.party_npc_2 == first_npc_slot) {
        /* NPC 2 should become NPC 1 */
        game_state.party_npc_1 = game_state.party_npc_2;
        game_state.party_npc_1_hp = game_state.party_npc_2_hp;
        /* Assign new NPC 2 from second NPC slot */
        uint8_t second_npc_slot = game_state.party_members[player_count + 1];
        game_state.party_npc_2 = second_npc_slot;
        game_state.party_npc_2_hp = get_npc_max_hp(second_npc_slot);
        return;
    }

    /* Neither NPC matches first slot; check if original NPC 1 matches second slot */
    uint8_t second_npc_slot = game_state.party_members[player_count + 1];
    if (saved_npc_1 == second_npc_slot) {
        /* Original NPC 1 belongs in position 2 */
        game_state.party_npc_2 = saved_npc_1;
        game_state.party_npc_2_hp = game_state.party_npc_1_hp;
        /* Assign new NPC 1 from first NPC slot */
        game_state.party_npc_1 = first_npc_slot;
        game_state.party_npc_1_hp = get_npc_max_hp(first_npc_slot);
        return;
    }

    /* Neither NPC matches either slot; reassign both */
    game_state.party_npc_1 = first_npc_slot;
    game_state.party_npc_1_hp = get_npc_max_hp(first_npc_slot);
    if (game_state.party_npc_2 != second_npc_slot) {
        game_state.party_npc_2 = second_npc_slot;
        game_state.party_npc_2_hp = get_npc_max_hp(second_npc_slot);
    }
}

/*
 * UPDATE_MUSHROOM_STATUS (asm/overworld/update_mushroom_status.asm)
 *
 * Checks if the leader (first player-controlled party member) is mushroomized.
 * If so, sets the mushroomized walking flag and initializes the timer if needed.
 * Also forces bicycle dismount while mushroomized.
 */
static void update_mushroom_status(void) {
    uint8_t leader_id = game_state.player_controlled_party_members[0];
    uint8_t mushroom_affliction = party_characters[leader_id].afflictions[1];

    if (mushroom_affliction == 1) {
        ow.mushroomized_walking_flag = 1;
        if (ow.mushroomization_timer == 0) {
            ow.mushroomization_timer = 0x0708;
            ow.mushroomization_modifier = 0;
        }
        if (game_state.walking_style == WALKING_STYLE_BICYCLE) {
            dismount_bicycle();
        }
    } else {
        ow.mushroomized_walking_flag = 0;
    }
}

/*
 * UPDATE_PARTY (asm/overworld/update_party.asm)
 *
 * Sorts the active party ordering based on member status:
 *   - Normal (alive) members come first (sort key = char_id, 0-4)
 *   - Incapacitated (unconscious=1 or diamondized=2) get +0x100
 *   - NPCs (member_type >= 5) get +0x300
 * After sorting, writes back to game_state.party_order,
 * party_entity_slots, player_controlled_party_members, and updates
 * each entity's var5 with its new party position.
 * Finally calls UPDATE_NPC_PARTY_LINEUP, UPDATE_MUSHROOM_STATUS,
 * and LOAD_CHARACTER_WINDOW_PALETTE.
 */
void update_party(void) {
    uint16_t party_count = game_state.party_count;
    if (party_count == 0) return;

    /* Helper to read/write party_entity_slots as 16-bit words */
    #define READ_PARTY_SLOT(i) \
        read_u16_le(&game_state.party_entity_slots[(i)*2])
    #define WRITE_PARTY_SLOT(i, val) do { \
        game_state.party_entity_slots[(i)*2] = (uint8_t)(val); \
        game_state.party_entity_slots[(i)*2+1] = (uint8_t)((val) >> 8); \
    } while(0)

    uint16_t positions[6];   /* saved position_index per member */
    uint16_t sort_keys[6];   /* sort priority key */
    uint16_t eslots[6];      /* entity slot values */
    uint16_t members[6];     /* player_controlled_party_members values */

    /* Phase 1: Save each player-controlled member's position_index */
    for (uint16_t i = 0; i < party_count; i++) {
        uint8_t char_id = game_state.player_controlled_party_members[i];
        positions[i] = party_characters[char_id].position_index;
    }

    /* Phase 2: Build parallel arrays for sorting */
    for (uint16_t y = 0; y < party_count; y++) {
        uint16_t member_type = game_state.party_order[y];
        uint16_t key = member_type;

        if (member_type >= 5) {
            /* NPC: high sort priority (pushed to end) */
            key += 0x0300;
        } else {
            /* Player character: check if incapacitated */
            uint16_t entity_slot = READ_PARTY_SLOT(y);
            uint16_t entity_offset = entity_slot;
            uint16_t char_idx = (uint16_t)entities.var[1][entity_offset];
            uint8_t affliction = party_characters[char_idx].afflictions[0];
            if (affliction == 1 || affliction == 2) {
                /* Unconscious or diamondized: medium sort priority */
                key += 0x0100;
            }
        }

        sort_keys[y] = key;
        eslots[y] = READ_PARTY_SLOT(y);
        members[y] = game_state.player_controlled_party_members[y];
    }

    /* Phase 3: Bubble sort all three arrays by sort_keys (ascending) */
    for (uint16_t outer = 0; outer + 1 < party_count; outer++) {
        for (uint16_t inner = 0; inner + 1 < party_count; inner++) {
            if (sort_keys[inner] > sort_keys[inner + 1]) {
                /* Swap sort_keys */
                uint16_t tmp = sort_keys[inner];
                sort_keys[inner] = sort_keys[inner + 1];
                sort_keys[inner + 1] = tmp;
                /* Swap eslots */
                tmp = eslots[inner];
                eslots[inner] = eslots[inner + 1];
                eslots[inner + 1] = tmp;
                /* Swap members */
                tmp = members[inner];
                members[inner] = members[inner + 1];
                members[inner + 1] = tmp;
            }
        }
    }

    /* Phase 4: Write sorted results back to game_state */
    for (uint16_t i = 0; i < party_count; i++) {
        /* party_order gets the low byte of the sort key (original member_type) */
        game_state.party_order[i] = (uint8_t)(sort_keys[i] & 0xFF);

        /* Write entity slot back */
        WRITE_PARTY_SLOT(i, eslots[i]);

        /* Write player_controlled member back */
        game_state.player_controlled_party_members[i] = (uint8_t)(members[i] & 0xFF);

        /* Restore this member's position_index */
        uint8_t char_id = (uint8_t)(members[i] & 0xFF);
        party_characters[char_id].position_index = positions[i];

        /* Update entity var5 with new party position (word offset) */
        uint16_t entity_offset = eslots[i];
        entities.var[5][entity_offset] = (int16_t)(i * 2);
    }

    /* Set current_party_members to the first entity slot */
    game_state.current_party_members = READ_PARTY_SLOT(0);

    update_npc_party_lineup();
    update_mushroom_status();

    load_character_window_palette();

    #undef READ_PARTY_SLOT
    #undef WRITE_PARTY_SLOT
}

/*
 * RELOAD_MAP (asm/overworld/reload_map.asm)
 *
 * Restores the overworld map after battle/menu screens that overwrite VRAM.
 * Resets palette/tileset caches, resolves map music, reloads the map sector
 * and tile data at the leader's position, then re-enables display.
 */
void reload_map(void) {
    /* Assembly (reload_map.asm lines 8-10): invalidate palette and tileset caches.
     * All three caches must be invalidated so load_map_at_sector reloads
     * tileset GFX, palette, and tile arrangement data. */
    ow.loaded_map_palette = -1;
    ow.loaded_map_tile_combo = -1;
    invalidate_loaded_tileset_combo();

    force_blank_and_wait_vblank();

    /* Reset music tracking so music change is forced */
    ml.current_map_music_track = (uint16_t)-1;

    uint16_t leader_x = game_state.leader_x_coord;
    uint16_t leader_y = game_state.leader_y_coord;

    resolve_map_sector_music(leader_x, leader_y);

    /* Assembly lines 32-47: set up VRAM display mode.
     * reload_map_at_position does NOT do this (unlike load_map_at_position),
     * so we must call it here, matching the assembly's inline calls. */
    overworld_setup_vram();

    /* Assembly line 55: reload map tiles/collision/VRAM without clearing entities.
     * Uses reload_map_at_position (NOT load_map_at_position) so that roaming
     * enemies spawned by the enemy spawner are preserved after battle. */
    reload_map_at_position(leader_x, leader_y);

    /* Bicycle overrides map music with its own track */
    if (game_state.walking_style == WALKING_STYLE_BICYCLE) {
        change_music(82); /* MUSIC::BICYCLE */
    } else {
        apply_next_map_music();
    }

    /* Enable all layers: BG1+BG2+BG3+OBJ on main screen */
    ppu.tm = 0x17;

    blank_screen_and_wait_vblank();
}

void render_and_disable_entities(void) {
    update_party();
    refresh_party_entities();
    render_frame_tick();
    disable_all_entities();
    if (ow.entity_fade_entity != -1) {
        entities.tick_callback_hi[ow.entity_fade_entity] &=
            (uint16_t)~(uint16_t)(OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
    }
}

/*
 * BATTLE_SWIRL_SEQUENCE (asm/overworld/battle_swirl_sequence.asm)
 *
 * Configures and starts the battle swirl screen transition effect.
 * Sets swirl type, color math, and music based on battle initiative
 * and whether this is a boss fight.
 */
void battle_swirl_sequence(void) {
    uint16_t swirl_type = 1;
    uint16_t swirl_red = 4;
    uint16_t swirl_green = 4;
    uint16_t swirl_blue = 0;
    uint16_t swirl_music;
    uint16_t options;

    switch (bt.battle_initiative) {
    case INITIATIVE_NORMAL:
        swirl_music = 176;  /* MUSIC::BATTLE_SWIRL4 */
        options = 0x0E;
        break;
    case INITIATIVE_PARTY_FIRST:
        swirl_music = 176;  /* MUSIC::BATTLE_SWIRL4 */
        swirl_red = 0x1C;
        swirl_green = 5;
        swirl_blue = 0x0C;
        options = 0x06;
        break;
    case INITIATIVE_ENEMIES_FIRST:
        swirl_music = 9;    /* MUSIC::BATTLE_SWIRL2 */
        swirl_red = 0;
        swirl_green = 0x1F;
        swirl_blue = 0x1F;
        options = 0x06;
        break;
    default:
        swirl_music = 0;
        options = 0;
        break;
    }

    /* Boss override: all bosses (group >= 448) get type 3 swirl */
    if (bt.current_battle_group >= ENEMY_GROUP_BOSS_START) {
        swirl_type = 3;
        options = 0x0E;
        swirl_music = 8;    /* MUSIC::BATTLE_SWIRL1 */
    }

    change_music(swirl_music);
    restore_bg_palette_and_enable_display();

    if (options & 0x04) {
        set_coldata((uint8_t)swirl_red, (uint8_t)swirl_green, (uint8_t)swirl_blue);
        if (options & 0x08) {
            /* Color math: subtract with half, all layers */
            set_colour_addsub_mode(0x10, 0xFF);
        } else {
            /* Color math: subtract without half, all layers */
            set_colour_addsub_mode(0x10, 0xBF);
        }
    }

    start_battle_swirl(swirl_type, options, 30);

    /* Override swirl mask settings after start_battle_swirl */
    if (options & 0x04) {
        set_swirl_mask_settings(0x20);
    } else {
        set_swirl_mask_settings(0x0F);
    }
    set_swirl_auto_restore(0);
}

/*
 * INSTANT_WIN_CHECK (asm/battle/instant_win_check.asm)
 *
 * Determines if the party is strong enough for an automatic victory.
 * Scans party members for min offense/speed, counts active (non-incapacitated)
 * members, then compares against enemy stats.
 *
 * For INITIATIVE_NORMAL: requires party min_speed >= max enemy speed AND
 *   party min_offense*2 >= every enemy's (HP + defense).
 * For INITIATIVE_PARTY_FIRST: uses sorted matching — strongest party members
 *   are paired against weakest enemies to simulate sequential attacks.
 *
 * Returns 1 if instant win, 0 otherwise.
 */
static uint16_t instant_win_check(void) {
    /* Enemies going first can never be instant win */
    if (bt.battle_initiative == INITIATIVE_ENEMIES_FIRST)
        return 0;

    uint16_t max_enemy_speed = 0;
    uint16_t active_party_count = 0;
    uint16_t min_offense = 0xFFFF;
    uint16_t min_speed = 0xFFFF;

    /* Scan all 6 party slots */
    for (uint16_t i = 0; i < TOTAL_PARTY_COUNT; i++) {
        uint8_t member_id = game_state.party_members[i];
        if (member_id < 1 || member_id > 4)
            continue;

        CharStruct *cs = &party_characters[member_id - 1];

        /* Track min speed */
        uint16_t spd = (uint16_t)cs->speed;
        if (spd < min_speed)
            min_speed = spd;

        /* Track min offense */
        uint16_t off = (uint16_t)cs->offense;
        if (off < min_offense)
            min_offense = off;

        /* Check if this member is incapacitated */
        uint8_t status0 = cs->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
        if (status0 == STATUS_0_UNCONSCIOUS || status0 == STATUS_0_DIAMONDIZED ||
            status0 == STATUS_0_PARALYZED || status0 == STATUS_0_NAUSEOUS ||
            status0 == STATUS_0_POISONED || status0 == STATUS_0_SUNSTROKE ||
            status0 == STATUS_0_COLD)
            continue;

        uint8_t status1 = cs->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL];
        if (status1 == STATUS_1_MUSHROOMIZED || status1 == STATUS_1_POSSESSED)
            continue;

        /* Active member — store offense in sorted array */
        if (active_party_count < 4)
            instant_win_sorted_offense[active_party_count] = off;
        active_party_count++;
    }

    /* Need at least as many active party members as enemies */
    if (bt.enemies_in_battle > active_party_count)
        return 0;

    if (bt.battle_initiative == INITIATIVE_NORMAL) {
        /* --- Normal initiative path --- */

        /* Find max enemy speed */
        for (uint16_t i = 0; i < bt.enemies_in_battle; i++) {
            uint16_t eid = bt.enemies_in_battle_ids[i];
            uint16_t espd = (uint16_t)enemy_config_table[eid].speed;
            if (espd > max_enemy_speed)
                max_enemy_speed = espd;
        }

        /* Party must be faster than all enemies */
        if (min_speed < max_enemy_speed)
            return 0;

        /* Each enemy must be beatable by min_offense * 2 >= HP + defense */
        for (uint16_t i = 0; i < bt.enemies_in_battle; i++) {
            uint16_t eid = bt.enemies_in_battle_ids[i];
            const EnemyData *ed = &enemy_config_table[eid];
            uint16_t toughness = ed->hp + ed->defense;
            if (min_offense * 2 < toughness)
                return 0;
        }

        return 1;
    }

    /* --- Party-first initiative path (green swirl) --- */

    /* Collect enemy HP and defense into sorted arrays */
    for (uint16_t i = 0; i < bt.enemies_in_battle && i < 4; i++) {
        uint16_t eid = bt.enemies_in_battle_ids[i];
        const EnemyData *ed = &enemy_config_table[eid];
        instant_win_sorted_hp[i] = ed->hp;
        instant_win_sorted_defense[i] = ed->defense;
    }

    /* Bubble sort offense array — descending (strongest first) */
    for (;;) {
        int sorted = 1;
        for (uint16_t outer = 0; outer + 1 < active_party_count && outer < 4; outer++) {
            for (uint16_t inner = outer + 1; inner < active_party_count && inner < 4; inner++) {
                if (instant_win_sorted_offense[inner] > instant_win_sorted_offense[outer]) {
                    /* Swap */
                    sorted = 0;
                    uint16_t tmp = instant_win_sorted_offense[outer];
                    instant_win_sorted_offense[outer] = instant_win_sorted_offense[inner];
                    instant_win_sorted_offense[inner] = tmp;
                }
            }
        }
        if (sorted)
            break;
    }

    /* Bubble sort enemy arrays — descending by HP (strongest first) */
    for (;;) {
        int sorted = 1;
        for (uint16_t outer = 0; outer + 1 < bt.enemies_in_battle && outer < 4; outer++) {
            for (uint16_t inner = outer + 1; inner < bt.enemies_in_battle && inner < 4; inner++) {
                /* Assembly: CMP inner vs outer, BLTEQ skip → swap when inner > outer */
                if (instant_win_sorted_hp[inner] <= instant_win_sorted_hp[outer])
                    continue;
                /* Swap HP */
                sorted = 0;
                uint16_t tmp_hp = instant_win_sorted_hp[outer];
                instant_win_sorted_hp[outer] = instant_win_sorted_hp[inner];
                instant_win_sorted_hp[inner] = tmp_hp;
                /* Swap defense in parallel */
                uint16_t tmp_def = instant_win_sorted_defense[outer];
                instant_win_sorted_defense[outer] = instant_win_sorted_defense[inner];
                instant_win_sorted_defense[inner] = tmp_def;
            }
        }
        if (sorted)
            break;
    }

    /* Simulate combat: strongest party vs weakest enemies */
    uint16_t enemy_idx = 0;
    for (uint16_t party_idx = 0; party_idx < active_party_count && party_idx < 4; party_idx++) {
        uint16_t attack_power = instant_win_sorted_offense[party_idx] * 2;
        uint16_t enemy_hp = instant_win_sorted_hp[enemy_idx];
        uint16_t enemy_def = instant_win_sorted_defense[enemy_idx];
        uint16_t toughness = enemy_hp + enemy_def;

        if (attack_power >= toughness) {
            /* Enemy killed — advance to next enemy */
            enemy_idx++;
            if (enemy_idx >= bt.enemies_in_battle)
                return 1;  /* All enemies defeated */
        } else {
            /* Enemy survives — reduce its HP by (attack_power - defense) */
            uint16_t damage = attack_power - enemy_def;
            instant_win_sorted_hp[enemy_idx] = enemy_hp - damage;
        }
    }

    return 0;  /* Ran out of party members */
}

/*
 * FILL_PALETTES_WITH_COLOR (port of C26189)
 *
 * Fills all 256 palette entries with a single color and triggers a full
 * palette upload. The blocking original's trailing one-frame wait is the
 * IW_FLASH phase's yield (the pump owns it).
 */
static void iw_fill_palettes(uint16_t color) {
    for (int i = 0; i < 256; i++) {
        ert.palettes[i] = color;
    }
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
}

/* The instant-win screen flash: green, red, blue twice, then black — each
 * entry one fill_palettes_with_color() frame in the blocking original. */
static const uint16_t iw_flash_colors[7] = {
    0x03E0, 0x001F, 0x7C00, 0x03E0, 0x001F, 0x7C00, 0x0000
};

/* The synchronous victory setup between the fade and the "YOU WON" text
 * (verbatim from the blocking instant_win_handler): entity disable, the
 * battle text window, the money deposit, battler re-init, and the EXP split. */
static void iw_victory_setup(void) {
    /* Disable all entities during the victory sequence */
    disable_all_entities();

    /* Open the battle text window (WINDOW::TEXT_BATTLE = 0x0E) */
    create_window(0x0E);

    /* --- Sum enemy money --- */
    bt.battle_money_scratch = 0;
    for (uint16_t i = 0; i < bt.enemies_in_battle; i++) {
        uint16_t enemy_id = bt.enemies_in_battle_ids[i];
        const EnemyData *edata = &enemy_config_table[enemy_id];
        bt.battle_money_scratch += edata->money;
    }

    /* Deposit money into ATM and add to running total (game_state.unknownC4) */
    uint32_t money32 = (uint32_t)bt.battle_money_scratch;
    uint32_t deposited = deposit_into_atm(money32);

    /* Add deposited amount to game_state.unknownC4 (32-bit running battle money) */
    uint32_t total_money;
    memcpy(&total_money, game_state.unknownC4, 4);
    total_money += deposited;
    memcpy(game_state.unknownC4, &total_money, 4);

    /* --- Clear and re-init battlers --- */
    for (int i = 0; i < BATTLER_COUNT; i++) {
        memset(&bt.battlers_table[i], 0, sizeof(Battler));
    }

    /* Initialize player battlers from party_members */
    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        uint8_t party_member = game_state.party_members[i];
        if (party_member == 0) continue;
        if (party_member > 4) continue;
        battle_init_player_stats((uint16_t)party_member, &bt.battlers_table[i]);
    }

    /* --- Sum enemy EXP --- */
    bt.battle_exp_scratch = 0;
    for (uint16_t i = 0; i < bt.enemies_in_battle; i++) {
        uint16_t enemy_id = bt.enemies_in_battle_ids[i];
        const EnemyData *edata = &enemy_config_table[enemy_id];
        bt.battle_exp_scratch += edata->exp;
    }

    /* Divide EXP evenly among alive party members (ceiling division) */
    uint16_t alive_count = battle_count_chars(0);
    if (alive_count > 0) {
        bt.battle_exp_scratch = (bt.battle_exp_scratch + (uint32_t)(alive_count - 1)) / (uint32_t)alive_count;
    }
}

/* The random item-drop roll (verbatim from the blocking instant_win_handler).
 * Leaves the result in bt.item_dropped (0 = nothing dropped). */
static void iw_roll_drop(void) {
    /* --- Random item drop --- */
    uint16_t rand_enemy_idx = rand_limit(bt.enemies_in_battle);
    uint16_t selected_enemy_id = bt.enemies_in_battle_ids[rand_enemy_idx];
    const EnemyData *drop_enemy = &enemy_config_table[selected_enemy_id];

    bt.item_dropped = drop_enemy->item_dropped;
    uint8_t drop_rate = drop_enemy->item_drop_rate;

    /* Item rarity masks (from include/config.asm) */
    static const uint8_t rarity_masks[7] = {
        0x7F,  /* rate 0: 1/128 chance */
        0x3F,  /* rate 1: 1/64 chance */
        0x1F,  /* rate 2: 1/32 chance */
        0x0F,  /* rate 3: 1/16 chance */
        0x07,  /* rate 4: 1/8 chance */
        0x03,  /* rate 5: 1/4 chance */
        0x01,  /* rate 6: 1/2 chance */
    };

    if (drop_rate < 7) {
        /* AND with rarity mask; if nonzero, item is NOT dropped */
        if (rand_byte() & rarity_masks[drop_rate]) {
            bt.item_dropped = 0;
        }
    }
    /* drop_rate >= 7: always drop (no rarity check) */
}

/*
 * INSTANT_WIN_HANDLER (port of asm/battle/instant_win_handler.asm)
 *
 * Handles the "instant win" sequence when the party is much stronger than the
 * enemies.  Plays the sudden victory music, flashes the screen, fades in,
 * awards money and EXP, handles item drops, and restores overworld music.
 *
 * Run-to-completion: GAME_MODE_INSTANT_WIN. The former wait_for_vblank loops
 * are the IW_FLASH / IW_FADE per-frame phases; the texts are DISPLAY_TEXT
 * pushes via battle_push_text_ex (the resume phase clears the prompt flag,
 * i.e. the blocking epilogue, and falls through inline when the address is
 * unresolvable); the per-battler gain_exp(1, ...) calls are
 * gain_exp_prepare() inline + a GAME_MODE_LEVEL_UP push (the BTL_VICTORY_EXP
 * idiom). Pushed by GAME_MODE_BATTLE_ENTRY's instant-win branch.
 */
StepResult mode_step_instant_win(ModeState *state) {
    InstantWinState *st = &state->instant_win;
    static ModeState child_init;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->phase) {
        case IW_BEGIN:
            /* Reset initiative */
            bt.battle_initiative = 0;

            /* Play sudden victory music */
            change_music(183);  /* MUSIC::SUDDEN_VICTORY */

            /* Stop the battle swirl effect */
            stop_battle_swirl();

            st->phase = IW_FLASH;
            st->flash_i = 0;
            break;

        case IW_FLASH:
            /* Flash screen green/red/blue twice, then fill with black — one
             * palette fill per frame, exactly the blocking fill_palettes_
             * with_color() cadence (work, then the pump's yield). */
            if (st->flash_i < 7) {
                iw_fill_palettes(iw_flash_colors[st->flash_i++]);
                return STEP_RESULT_CONTINUE();
            }
            st->phase = IW_FADE;
            st->fade_i = 0;
            break;

        case IW_FADE:
            if (st->fade_i == 0) {
                /* Restore saved palettes as fade target */
                PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
                memcpy(fade->target, ert.buffer + BUF_BATTLE_PALETTE_SAVE,
                       sizeof(fade->target));

                /* Prepare palette fade from black to original over 6 frames */
                prepare_palette_fade_slopes(6, 0xFFFF);
            }
            /* Run 6 frames of palette fade */
            if (st->fade_i < 6) {
                st->fade_i++;
                update_map_palette_animation();
                return STEP_RESULT_CONTINUE();
            }
            /* Finalize the fade (after the 6th frame's yield, like the
             * blocking loop's trailing wait_for_vblank). */
            finalize_palette_fade();
            st->phase = IW_VICTORY;
            break;

        case IW_VICTORY:
            /* Entity disable, battle text window, money, battler re-init,
             * EXP split — then the "YOU WON" message with the EXP amount. */
            iw_victory_setup();
            st->phase = IW_EXP;
            st->battler_i = 0;
            if (battle_push_text_ex(&child_init, MSG_BTL8_PLAYER_VICTORY_FORCED,
                                    false, true, bt.battle_exp_scratch))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            break;

        case IW_EXP:
            /* display_text_wait_addr's epilogue (idempotent on re-entry —
             * LEVEL_UP clears it itself before popping). */
            dt.blinking_triangle_flag = 0;

            /* --- Award EXP to eligible battlers --- gain_exp(1, ...) ran
             * here in the blocking original; the prepare half runs inline
             * and any pending level-up runs as a pushed GAME_MODE_LEVEL_UP
             * child (the BTL_VICTORY_EXP idiom). */
            while (st->battler_i < BATTLER_COUNT) {
                Battler *b = &bt.battlers_table[st->battler_i++];

                /* Must be a conscious, non-NPC ally, not unconscious or
                 * diamondized */
                if (b->consciousness == 0) continue;
                if (b->ally_or_enemy != 0) continue;
                if (b->npc_id != 0) continue;
                uint8_t status0 = b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
                if (status0 == STATUS_0_UNCONSCIOUS) continue;
                if (status0 == STATUS_0_DIAMONDIZED) continue;

                if (gain_exp_prepare(b->id, bt.battle_exp_scratch)) {
                    level_up_make_init(&child_init, b->id);
                    /* phase stays IW_EXP — the resume re-enters the loop */
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_LEVEL_UP, &child_init);
                }
            }
            st->phase = IW_DROP;
            break;

        case IW_DROP:
            iw_roll_drop();
            st->phase = IW_FINISH;
            if (bt.item_dropped != 0) {
                set_current_item((uint8_t)bt.item_dropped);
                if (battle_push_text(&child_init, MSG_BTL8_ENEMY_PRESENT_DROPPED))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child_init);
            }
            break;

        case IW_FINISH:
        default:
            /* display_in_battle_text_addr's epilogue */
            dt.blinking_triangle_flag = 0;

            /* --- Restore music and re-enable entities --- */
            close_all_windows();
            hide_hppp_windows();

            if (game_state.walking_style == WALKING_STYLE_BICYCLE) {
                change_music(82);  /* MUSIC::BICYCLE */
            } else {
                update_map_music_at_leader();
            }

            enable_all_entities();
            return STEP_RESULT_POP(0);
        }
    }
}

/*
 * INSTANT_WIN_PP_RECOVERY — Port of asm/battle/instant_win_pp_recovery.asm (114 lines).
 *
 * Called from event scripts after an instant-win battle.
 * Flashes the screen purple twice (palette fade effect), then recovers
 * 20 PP for each party member that is NESS (1), PAULA (2), or POO (4).
 */
void instant_win_pp_recovery(void) {
    play_sfx(SFX_RECOVER_HP);

    /* Flash screen purple twice */
    for (int flash = 0; flash < 2; flash++) {
        /* Save current ert.palettes to ert.buffer */
        memcpy(ert.buffer, ert.palettes, 512);

        /* Fill all 256 palette entries with purple (SNES RGB $5D70) */
        for (int i = 0; i < 256; i++) {
            ert.palettes[i] = 0x5D70;
        }

        /* Fade from purple back to saved colors over 12 frames */
        prepare_palette_fade_slopes(12, 0xFFFF);
        for (int f = 0; f < 12; f++) {
            update_map_palette_animation();
            wait_for_vblank();
        }
        finalize_palette_fade();
    }

    /* Recover 20 PP for NESS, PAULA, and POO */
    for (int slot = 0; slot < 6; slot++) {
        uint8_t member = game_state.party_members[slot];
        /* Only recover for NESS (1), PAULA (2), or POO (4) */
        if (member != 1 && member != 2 && member != 4)
            continue;

        int char_idx = member - 1;  /* 0-based character index */
        CharStruct *ch = &party_characters[char_idx];
        uint16_t new_pp = ch->current_pp_target + 20;
        if (new_pp > ch->max_pp) {
            new_pp = ch->max_pp;
        }
        ch->current_pp_target = new_pp;
    }
}

/*
 * INIT_BATTLE_OVERWORLD (asm/battle/init_overworld.asm)
 *
 * Entry point for random/overworld encounters.
 * Handles instant win (auto-win), runs the battle (with init_battle_common's
 * fade-out + post-battle party update inlined around the GAME_MODE_BATTLE
 * push), post-battle map reload, entity state reset, and intangibility
 * frames.
 *
 * Run-to-completion: GAME_MODE_BATTLE_ENTRY. The debug exit-button busy-wait
 * is BE_DEBUG_WAIT; an instant win pushes GAME_MODE_INSTANT_WIN; the battle
 * is a GAME_MODE_BATTLE push. Kept inline-blocking (documented):
 * teleport_mainloop() (the PSI-teleport driver, its own large blocking
 * machine) and reload_map()'s force_blank/blank_screen one-shot vblank
 * helpers. init_battle_overworld() below is the pump bridge for the
 * still-blocking overworld root loop (game_main.c — Phase D).
 */
StepResult mode_step_battle_entry(ModeState *state) {
    BattleEntryState *st = &state->battle_entry;
    static ModeState child_init;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->phase) {
        case BE_ENTER:
            if (ow.battle_mode == 0)
                return STEP_RESULT_POP(0);

            /* CHECK_DEBUG_EXIT_BUTTON (asm/system/debug/check_debug_exit_button.asm).
             * If ow.debug_mode_number==2: wait for B button press, then skip battle.
             * Otherwise: if Y is held, skip battle immediately. */
            if (ow.debug_flag) {
                if (ow.debug_mode_number == 2) {
                    /* The blocking original busy-waited on WAIT_UNTIL_NEXT_FRAME;
                     * BE_DEBUG_WAIT checks before the first yield, like the
                     * original's pre-loop condition test. */
                    st->phase = BE_DEBUG_WAIT;
                    break;
                }
                if (core.pad1_held & PAD_Y) {
                    st->phase = BE_RESET;
                    break;
                }
            }

            /* Check for instant win (high level vs weak enemies) */
            if (instant_win_check()) {
                child_init = (ModeState){0};
                child_init.instant_win.phase = IW_BEGIN;
                st->phase = BE_IW_DONE;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_INSTANT_WIN, &child_init);
            }

            /* Run the battle — init_battle_common's front half:
             * FADE_OUT_WITH_MOSAIC(A=step, X=delay, Y=mosaic_enable) → (1, 1, 0) */
            fade_out(1, 1);
            child_init = (ModeState){0};
            child_init.battle.phase = BTL_BEGIN;
            st->phase = BE_BATTLE_DONE;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE, &child_init);

        case BE_DEBUG_WAIT:
            if (core.pad1_held & PAD_B) {
                ow.battle_mode = 0;
                st->phase = BE_RESET;
                break;
            }
            return STEP_RESULT_CONTINUE();

        case BE_IW_DONE:
            /* After the instant-win sequence pops */
            ow.battle_mode = 0;
            st->phase = BE_RESET;
            break;

        case BE_BATTLE_DONE: {
            /* init_battle_common's tail */
            uint16_t result = (uint16_t)mode_child_result();
            update_party();
            bt.party_members_alive_overworld = 1;
            ow.battle_mode = 0;

            refresh_party_entities();
            ow.overworld_status_suppression = 0;

            if (ow.psi_teleport_destination != 0) {
                /* PSI teleport triggered during battle (inline-blocking —
                 * its own large driver, deferred) */
                teleport_mainloop();
            } else if (result == 0) {
                /* Normal victory — reload the map and fade in */
                /* Debug: CHECK_VIEW_CHARACTER_MODE — skip in C port */
                reload_map();
                fade_in(1, 1);
            } else {
                /* Party defeated or special result — caller handles */
                return STEP_RESULT_POP(0);
            }
            st->phase = BE_RESET;
            break;
        }

        case BE_RESET:
        default:
            /* Reset entity collision/pathfinding state for the first 23 entities */
            for (uint16_t i = 0; i < MAX_ENEMY_ENCOUNTER_SLOTS; i++) {
                entities.collided_objects[i] = -1; /* ENTITY_COLLISION_NO_OBJECT */
                entities.pathfinding_states[i] = 0;
                entities.spritemap_ptr_hi[i] &= 0x7FFF; /* clear hide bit (bit 15) */
            }

            ow.overworld_status_suppression = 0;
            enable_all_entities();

            ow.player_intangibility_frames = 120;
            bt.touched_enemy = (int16_t)0xFFFF;
            return STEP_RESULT_POP(0);
        }
    }
}

/* Pump bridge for the still-blocking overworld root loop caller
 * (game_main.c overworld_step — converts at the Phase D flip). */
void init_battle_overworld(void) {
    if (ow.battle_mode == 0)
        return;
    ModeState init = {0};
    init.battle_entry.phase = BE_ENTER;
    pump_mode(GAME_MODE_BATTLE_ENTRY, &init);
}

/*
 * INIT_BATTLE_SCRIPTED (asm/battle/init_scripted.asm)
 *
 * Entry point for scripted/event-triggered battles.
 * Parses the enemy group from BTL_ENTRY_PTR_TABLE, plays battle swirl,
 * runs the battle, handles post-battle (reload map or teleport).
 *
 * The BTL_ENTRY_PTR_TABLE has 8-byte entries (battle_entry_ptr_entry):
 *   4 bytes: pointer to enemy group data
 *   2 bytes: run_away_flag
 *   1 byte:  run_away_flag_state
 *   1 byte:  letterbox_style
 *
 * The enemy group data is a sequence of 3-byte entries:
 *   1 byte: count (how many of this enemy, or 0xFF = terminator)
 *   2 bytes: enemy_id
 *
 * Run-to-completion: GAME_MODE_BATTLE_SCRIPTED. The swirl wait is a
 * BW_SWIRL_UPDATE push, the battle a GAME_MODE_BATTLE push (with
 * init_battle_common's fade-out/party-update halves inlined around it, as
 * in GAME_MODE_BATTLE_ENTRY), and render_and_disable_entities() is split
 * at its render_frame_tick yield (BS_CLEANUP work / BS_FINISH disable).
 * Kept inline-blocking (documented): teleport_mainloop() and reload_map()'s
 * one-shot vblank helpers. Pushed by CC_1F_23 TRIGGER_BATTLE (cc_1f_dispatch
 * push-signal; the result is stored to working memory in the
 * DT_RESUME_CC1F_BATTLE handler).
 *
 * Pops: 0 = normal victory/post-battle, 1 = party defeated.
 */
StepResult mode_step_battle_scripted(ModeState *state) {
    BattleScriptedState *st = &state->battle_scripted;
    static ModeState child_init;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->phase) {
        case BS_ENTER:
            bt.current_battle_group = st->battle_group;

            /* Parse BTL_ENTRY_PTR_TABLE + ENEMY_BATTLE_GROUPS_TABLE to populate
             * bt.enemies_in_battle_ids[] (port of init_scripted.asm lines 12-48).
             *
             * BTL_ENTRY_PTR_TABLE: 8 bytes per entry, first 3 bytes = 24-bit ROM
             * pointer into ENEMY_BATTLE_GROUPS_TABLE.
             * Groups table format: count(1), enemy_id(2), repeated, terminated by 0xFF. */
            bt.enemies_in_battle = 0;
            {
                static const uint8_t *ptr_table = NULL;
                static const uint8_t *groups_table = NULL;
                if (!ptr_table)
                    ptr_table = ASSET_DATA(ASSET_DATA_BTL_ENTRY_PTR_TABLE_BIN);
                if (!groups_table)
                    groups_table = ASSET_DATA(ASSET_DATA_ENEMY_BATTLE_GROUPS_TABLE_BIN);
                if (ptr_table && groups_table) {
                    /* Read 24-bit pointer from table entry (little-endian) */
                    uint32_t entry_off = (uint32_t)st->battle_group * 8;
                    uint32_t rom_ptr = (uint32_t)ptr_table[entry_off]
                                     | ((uint32_t)ptr_table[entry_off + 1] << 8)
                                     | ((uint32_t)ptr_table[entry_off + 2] << 16);
                    /* Convert ROM address to offset within groups table.
                     * ROM address of ENEMY_BATTLE_GROUPS_TABLE = $D0D52D. */
                    uint32_t groups_offset = rom_ptr - 0xD0D52D;
                    const uint8_t *ptr = groups_table + groups_offset;

                    while (1) {
                        uint8_t count = *ptr;
                        if (count == 0xFF)
                            break;
                        uint16_t enemy_id = (uint16_t)ptr[1] | ((uint16_t)ptr[2] << 8);
                        for (uint8_t i = 0; i < count; i++) {
                            if (bt.enemies_in_battle < MAX_ENEMY_BATTLER_SLOTS)
                                bt.enemies_in_battle_ids[bt.enemies_in_battle++] = enemy_id;
                        }
                        ptr += 3;
                    }
                }
            }

            /* Set battle mode to active */
            ow.battle_mode = 0xFFFF;

            /* Play battle swirl and wait for completion.
             * Assembly: JSL WAIT_UNTIL_NEXT_FRAME; JSL UPDATE_SWIRL_EFFECT
             * (yield-before-update; BW_SWIRL_UPDATE preserves that interleave). */
            battle_swirl_sequence();
            child_init = (ModeState){0};
            child_init.battle_wait.kind = BW_SWIRL_UPDATE;
            st->phase = BS_SWIRL_DONE;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_WAIT, &child_init);

        case BS_SWIRL_DONE:
            /* Run the actual battle — init_battle_common's front half:
             * FADE_OUT_WITH_MOSAIC(A=step, X=delay, Y=mosaic_enable) → (1, 1, 0) */
            fade_out(1, 1);
            child_init = (ModeState){0};
            child_init.battle.phase = BTL_BEGIN;
            st->phase = BS_BATTLE_DONE;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE, &child_init);

        case BS_BATTLE_DONE: {
            /* init_battle_common's tail */
            uint16_t result = (uint16_t)mode_child_result();
            update_party();
            bt.party_members_alive_overworld = 1;
            ow.battle_mode = 0;

            /* Post-battle handling */
            if (ow.psi_teleport_destination != 0) {
                /* PSI teleport triggered during battle (inline-blocking —
                 * its own large driver, deferred) */
                teleport_mainloop();
                if (result != 0)
                    return STEP_RESULT_POP(1);  /* party defeated */
            } else if (result == 0) {
                /* Normal victory — reload map and fade in */
                reload_map();
                fade_in(1, 1);
            } else {
                /* Party defeated — return immediately */
                return STEP_RESULT_POP(1);
            }
            st->phase = BS_CLEANUP;
            break;
        }

        case BS_CLEANUP:
            /* Post-battle cleanup — render_and_disable_entities() split at
             * its render_frame_tick yield: the work half here, the entity
             * disable after the yield in BS_FINISH (work-then-yield matches
             * the blocking helper exactly). */
            update_party();
            refresh_party_entities();
            render_frame_tick_work();
            st->phase = BS_FINISH;
            return STEP_RESULT_CONTINUE();

        case BS_FINISH:
        default:
            disable_all_entities();
            if (ow.entity_fade_entity != -1) {
                entities.tick_callback_hi[ow.entity_fade_entity] &=
                    (uint16_t)~(uint16_t)(OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
            }

            /* Non-boss fights grant intangibility frames */
            if (bt.current_battle_group < ENEMY_GROUP_BOSS_START) {
                ow.player_intangibility_frames = 120;
            }

            return STEP_RESULT_POP(0);
        }
    }
}
