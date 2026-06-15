/*
 * Battle targeting functions.
 *
 * Extracted from battle.c — target selection, row-based targeting,
 * target dispatch, and mask-based targeting operations.
 */
#include "game/battle.h"
#include "game/battle_internal.h"
#include "game/game_state.h"
#include "game/audio.h"
#include "game/display_text.h"
#include "game/inventory.h"
#include "game/text.h"
#include "game/window.h"

#include "data/assets.h"
#include "data/text_refs.h"

#define BATTLE_ROW_TEXT_LENGTH 13
#include "game/overworld.h"
#include "game/battle_bg.h"
#include "game/fade.h"
#include "entity/entity.h"
#include "include/pad.h"
#include "snes/ppu.h"
#include "platform/platform.h"
#include "core/mode_stack.h"
#include "game_main.h"

/*
 * CHECK_BATTLE_ACTION_TARGETABLE (asm/battle/ui/check_battle_action_targetable.asm)
 *
 * Checks whether a battler in the given row is targetable for the given action.
 * For front-row battlers (row 0), always returns 1.
 * For back-row battlers (row 1), checks if the action type is PSI (1); if so,
 * additionally validates the row via IS_ROW_VALID.
 *
 * row: 0 = front, 1 = back.
 * index: position within the row.
 * action_param: index into battle_action_table.
 */
static uint16_t check_battle_action_targetable(uint16_t row, uint16_t index,
                                               uint16_t action_param) {
    if (row != 1)
        return 1;  /* front row: always targetable */

    /* Assembly reads offset +2 (type field) via INX;INX from direction base */
    if (battle_action_table) {
        uint8_t action_type = battle_action_table[action_param].type;
        if (action_type == 1) {  /* ACTION_TYPE_PSI */
            if (!is_row_valid())
                return 0;
        }
    }
    return 1;
}


/*
 * FIND_NEXT_BATTLER_IN_ROW (asm/battle/ui/find_next_battler_in_row.asm)
 *
 * Starting from x_position, searches forward (left to right) through the row
 * for the next targetable battler whose x position is strictly greater.
 * Returns the index within the row, or -1 (0xFFFF) if none found.
 *
 * row: 0 = front, 1 = back.
 * x_position: current x position to search from.
 * action_param: passed to CHECK_BATTLE_ACTION_TARGETABLE.
 */
static int16_t find_next_battler_in_row(uint16_t row, uint16_t x_position,
                                        uint16_t action_param) {
    uint16_t count;
    const uint8_t *positions;

    if (row == 0) {
        count = bt.num_battlers_in_front_row;
        positions = bt.battler_front_row_x_positions;
    } else {
        count = bt.num_battlers_in_back_row;
        positions = bt.battler_back_row_x_positions;
    }

    for (uint16_t i = 0; i < count; i++) {
        uint16_t pos = positions[i] & 0xFF;
        if (pos <= x_position)
            continue;  /* assembly: BLTEQ (<=) skips */
        if (check_battle_action_targetable(row, i, action_param))
            return (int16_t)i;
    }
    return -1;
}


/*
 * FIND_PREV_BATTLER_IN_ROW (asm/battle/ui/find_prev_battler_in_row.asm)
 *
 * Starting from x_position, searches backward (right to left) through the row
 * for the previous targetable battler whose x position is strictly less.
 * Returns the index within the row, or -1 (0xFFFF) if none found.
 *
 * row: 0 = front, 1 = back.
 * x_position: current x position to search from.
 * action_param: passed to CHECK_BATTLE_ACTION_TARGETABLE.
 */
static int16_t find_prev_battler_in_row(uint16_t row, uint16_t x_position,
                                        uint16_t action_param) {
    uint16_t count;
    const uint8_t *positions;

    if (row == 0) {
        count = bt.num_battlers_in_front_row;
        positions = bt.battler_front_row_x_positions;
    } else {
        count = bt.num_battlers_in_back_row;
        positions = bt.battler_back_row_x_positions;
    }

    /* Search backward from last entry */
    for (int16_t i = (int16_t)count - 1; i >= 0; i--) {
        uint16_t pos = positions[i] & 0xFF;
        if (pos >= x_position)
            continue;  /* assembly: BCS (>=) skips */
        if (check_battle_action_targetable(row, (uint16_t)i, action_param))
            return i;
    }
    return -1;
}


/*
 * DISPLAY_BATTLE_TARGET_TEXT (asm/battle/ui/display_battle_target_text.asm)
 *
 * Displays "To <enemy name>" or "To the Front/Back Row" in WINDOW 0x31.
 * When enemy_index == -1, shows row name instead of individual target name.
 *
 * row: 0 = front, 1 = back.
 * enemy_index: index within the row, or -1 for row-level targeting.
 */
static void display_battle_target_text(uint16_t row, int16_t enemy_index) {
    /* Create the target text window (WINDOW::BATTLE_TARGET_TEXT = 0x31) */
    create_window(WINDOW_BATTLE_TARGET_TEXT);

    /* Print "To " prefix */
    print_string("To ");

    if (enemy_index != -1) {
        /* Assembly calls SET_TARGET_NAME_WITH_ARTICLE which copies the enemy name
         * to the target name ert.buffer, then prints it via RETURN_BATTLE_ATTACKER_ADDRESS.
         * We approximate by using the battler's name directly. */
        uint16_t battler_idx;
        if (row != 0)
            battler_idx = bt.back_row_battlers[enemy_index] & 0xFF;
        else
            battler_idx = bt.front_row_battlers[enemy_index] & 0xFF;

        /* Copy enemy name to target name ert.buffer */
        Battler *b = &bt.battlers_table[battler_idx];
        if (b->ally_or_enemy == 1) {
            const EnemyData *edata = &enemy_config_table[b->id];
            uint8_t scratch[ENEMY_NAME_SIZE + 2];
            memset(scratch, 0, sizeof(scratch));
            uint16_t pos = copy_enemy_name(edata->name, scratch, ENEMY_NAME_SIZE, sizeof(scratch));

            /* Append letter suffix if needed (same logic as fix_target_name) */
            bool add_letter = true;
            if (b->the_flag == 1) {
                uint8_t next = find_next_enemy_letter(b->enemy_type_id);
                if (next == 2)
                    add_letter = false;
            }
            if (add_letter && b->the_flag != 0) {
                scratch[pos++] = EB_CHAR_SPACE;
                scratch[pos] = b->the_flag + EB_CHAR_A_MINUS_1;
            }
            set_battle_attacker_name((const char *)scratch, ENEMY_NAME_SIZE + 2);
        }

        /* Print the attacker name (assembly: JSR RETURN_BATTLE_ATTACKER_ADDRESS) */
        char *attacker_name = return_battle_attacker_address();
        print_eb_string((const uint8_t *)attacker_name, BATTLE_NAME_ATTACKER_SIZE);

        /* Assembly lines 65-96 (US non-prototype): display affliction indicator tile.
         * Looks up the battler's afflictions via GET_EQUIP_WINDOW_TEXT(mode=0),
         * then renders the tile at cursor position (17, 0). */
        set_focus_text_cursor(17, 0);
        uint16_t equip_tile = get_equip_window_text(b->afflictions, 0);
        print_char_with_sound(equip_tile);
    } else {
        /* Row targeting: print "the Front Row" or "the Back Row" from ROM data */
        if (row == 0)
            print_eb_string(ASSET_DATA(ASSET_US_DATA_BATTLE_FRONT_ROW_TEXT_BIN), BATTLE_ROW_TEXT_LENGTH);
        else
            print_eb_string(ASSET_DATA(ASSET_US_DATA_BATTLE_BACK_ROW_TEXT_BIN), BATTLE_ROW_TEXT_LENGTH);
    }
}


/*
 * CLEAR_BATTLER_FLASHING (asm/battle/clear_battler_flashing.asm)
 *
 * Clears the flashing state for all battlers in the currently flashing row.
 * Sets is_flash_target = 0 for each battler in the row.
 */
static void clear_battler_flashing(void) {
    if (bt.current_flashing_row == -1)
        return;

    uint16_t count;
    if (bt.current_flashing_row != 0)
        count = bt.num_battlers_in_back_row;
    else
        count = bt.num_battlers_in_front_row;

    for (uint16_t i = 0; i < count; i++) {
        uint16_t battler_idx;
        if (bt.current_flashing_row != 0)
            battler_idx = bt.back_row_battlers[i] & 0xFF;
        else
            battler_idx = bt.front_row_battlers[i] & 0xFF;
        bt.battlers_table[battler_idx].is_flash_target = 0;
    }

    bt.enemy_targetting_flashing = 0;
    bt.current_flashing_row = -1;
    ow.redraw_all_windows = 1;
}


/*
 * SET_BATTLER_FLASHING (asm/battle/set_battler_flashing.asm)
 *
 * Sets all battlers in the specified row to flash (for row-targeting mode).
 * Clears any previously flashing row first.
 *
 * row: 0 = front, 1 = back.
 */
static void set_battler_flashing(uint16_t row) {
    if (bt.current_flashing_row != -1)
        clear_battler_flashing();

    bt.current_flashing_row = (int16_t)row;

    uint16_t count;
    if (row != 0)
        count = bt.num_battlers_in_back_row;
    else
        count = bt.num_battlers_in_front_row;

    for (uint16_t i = 0; i < count; i++) {
        uint16_t battler_idx;
        if (row != 0)
            battler_idx = bt.back_row_battlers[i] & 0xFF;
        else
            battler_idx = bt.front_row_battlers[i] & 0xFF;
        bt.battlers_table[battler_idx].is_flash_target = 1;
    }

    bt.enemy_targetting_flashing = 1;
    ow.redraw_all_windows = 1;
}


/*
 * SELECT_BATTLE_TARGET (asm/battle/ui/select_battle_target.asm)
 *
 * Interactive single-target selection UI.
 * Player uses LEFT/RIGHT to cycle through enemies, UP/DOWN to switch rows,
 * A/L to confirm, B/SELECT to cancel.
 *
 * Returns 1-based target index on confirm, 0 on cancel.
 *
 * allow_cancel: 1 = B/SELECT cancels, 0 = can't cancel.
 * action_param: index into battle_action_table for targetability check.
 */
/* The "WINDOW_TICK equivalent" battle render the targeting UIs run each frame,
 * minus the trailing wait_for_vblank() — the pump owns the single yield. Shared
 * by both targeting modes below.
 *
 * Park-propagating split (savestate D4b): a parked actionscript callroutine
 * becomes a STEP_PUSH instead of a nested pump_mode. _step() returns true iff a
 * frame parked — the caller must STEP_PUSH GAME_MODE_ACTIONSCRIPT_FRAME
 * (actionscript_frame_take_push) and call _flush() at its resume; on no park the
 * render finishes inline and _step() returns false. */
static bool targeting_render_work_step(void) {
    render_all_windows();
    upload_battle_screen_to_vram();
    if (run_actionscript_frame_step())
        return true;
    sync_palettes_to_cgram();
    battle_bg_update();
    return false;
}

static void targeting_render_work_flush(void) {
    sync_palettes_to_cgram();
    battle_bg_update();
}

/* select_battle_target's `update_target_display` work: recompute x_pos, flash the
 * current battler, show the target text the first time, then render one frame.
 * Returns true if the render parked (caller STEP_PUSHes + resumes at flush 1). */
static bool et_display_work(BattleEnemySelectState *s) {
    s->x_pos = get_battler_row_x_position(s->current_row, s->current_enemy);
    enemy_flashing_on(s->current_row, s->current_enemy);
    if (!s->target_shown)
        display_battle_target_text(s->current_row, (int16_t)s->current_enemy);
    s->target_shown++;
    return targeting_render_work_step();
}

/* GAME_MODE_BATTLE_ENEMY_SELECT — run-to-completion port of select_battle_target.
 * See mode_stack.h for the phase mapping of the blocking goto-machine. */
StepResult mode_step_battle_enemy_select(ModeState *ms) {
    BattleEnemySelectState *s = &ms->battle_enemy_select;

    /* Resume after a parked actionscript frame (D4b): the child completed the
     * render's actionscript frame; finish the render then advance per the site. */
    if (s->flush == 1) {        /* ET_DISPLAY / refresh render parked */
        s->flush = 0;
        targeting_render_work_flush();
        s->phase = ET_PRIME;
        return STEP_RESULT_CONTINUE();
    }
    if (s->flush == 2) {        /* ET_PRIME / idle hppp frame parked */
        s->flush = 0;
        update_hppp_meter_work_flush();
        s->phase = ET_INPUT;
        return STEP_RESULT_CONTINUE();
    }
    if (s->flush == 3) {        /* ET_INPUT change render parked */
        s->flush = 0;
        targeting_render_work_flush();
        s->phase = ET_DISPLAY;
        return STEP_RESULT_CONTINUE();
    }

    switch ((BattleEnemySelectPhase)s->phase) {
    case ET_DISPLAY:
        /* The blocking code's play_sfx ran right after the apply-common yield,
         * i.e. in this (post-yield) frame, before update_target_display renders. */
        if (s->pending_sfx) {
            play_sfx(s->pending_sfx);
            s->pending_sfx = 0;
        }
        if (et_display_work(s)) {
            s->flush = 1;
            return actionscript_frame_take_push();
        }
        s->phase = ET_PRIME;
        return STEP_RESULT_CONTINUE();

    case ET_PRIME:
        if (update_hppp_meter_work_step()) {
            s->flush = 2;
            return actionscript_frame_take_push();
        }
        s->phase = ET_INPUT;
        return STEP_RESULT_CONTINUE();

    case ET_INPUT:
    default: {
        uint16_t pressed = platform_input_get_pad_new();
        uint16_t sfx = 2;  /* SFX::CURSOR2 default (LEFT/RIGHT same-row move) */

        /* A/L: confirm selection */
        if (pressed & PAD_CONFIRM) {
            enemy_flashing_off();
            uint16_t result = s->current_row * bt.num_battlers_in_front_row +
                              s->current_enemy + 1;
            play_sfx(1);  /* SFX::CURSOR1 */
            close_focus_window();
            return STEP_RESULT_POP(result);
        }

        /* B/SELECT: cancel */
        if ((pressed & PAD_CANCEL) && s->allow_cancel == 1) {
            enemy_flashing_off();
            play_sfx(2);  /* SFX::CURSOR2 */
            close_focus_window();
            return STEP_RESULT_POP(0);
        }

        /* Navigation — decide the outcome, mirroring the blocking gotos:
         *   change  -> apply_selection[_common] (recreate window + two renders)
         *   refresh -> update_target_display    (one render, no change/sfx)
         *   neither -> idle update_hppp_meter_work frame, stay in ET_INPUT.
         * The blocking UP/DOWN guards are sequential `if`s that fall through to
         * LEFT/RIGHT when their row condition fails; the else-if chain below is
         * equivalent because the directions are exclusive and a failed UP/DOWN
         * guard leaves its branch untaken. */
        bool change = false;
        bool refresh = false;

        if ((pressed & PAD_UP) && s->current_row == 0 &&
            bt.num_battlers_in_back_row != 0) {
            /* switch_row to the back row */
            sfx = 3;  /* SFX::CURSOR3 */
            uint16_t new_row = s->current_row ^ 1;
            int16_t found = find_next_battler_in_row(new_row, s->x_pos - 1, s->action_param);
            if (found == -1)
                found = find_prev_battler_in_row(new_row, s->x_pos + 1, s->action_param);
            if (found == -1) refresh = true;
            else { s->current_enemy = (uint16_t)found; s->current_row = new_row; change = true; }
        } else if ((pressed & PAD_DOWN) && s->current_row == 1 &&
                   bt.num_battlers_in_front_row != 0) {
            /* switch_row to the front row */
            sfx = 3;
            uint16_t new_row = s->current_row ^ 1;
            int16_t found = find_next_battler_in_row(new_row, s->x_pos - 1, s->action_param);
            if (found == -1)
                found = find_prev_battler_in_row(new_row, s->x_pos + 1, s->action_param);
            if (found == -1) refresh = true;
            else { s->current_enemy = (uint16_t)found; s->current_row = new_row; change = true; }
        } else if (pressed & PAD_LEFT) {
            int16_t found = find_prev_battler_in_row(s->current_row, s->x_pos, s->action_param);
            if (found != -1) { s->current_enemy = (uint16_t)found; change = true; }
            else {
                uint16_t other = s->current_row ^ 1;
                found = find_prev_battler_in_row(other, s->x_pos, s->action_param);
                if (found == -1) refresh = true;
                else { s->current_enemy = (uint16_t)found; s->current_row = other; change = true; }
            }
        } else if (pressed & PAD_RIGHT) {
            int16_t found = find_next_battler_in_row(s->current_row, s->x_pos, s->action_param);
            if (found != -1) { s->current_enemy = (uint16_t)found; change = true; }
            else {
                uint16_t other = s->current_row ^ 1;
                found = find_next_battler_in_row(other, s->x_pos, s->action_param);
                if (found == -1) refresh = true;
                else { s->current_enemy = (uint16_t)found; s->current_row = other; change = true; }
            }
        }

        if (change) {
            /* apply_selection_common: recreate the target window + render frame
             * (yield C); ET_DISPLAY then does update_target_display's render
             * (yield A) and plays the queued sfx in that frame. x_pos/pending_sfx
             * do not depend on the render, so set them before the park check. */
            s->target_shown = 0;
            create_window(WINDOW_BATTLE_TARGET_TEXT);
            bool parked = targeting_render_work_step();
            s->x_pos = get_battler_row_x_position(s->current_row, s->current_enemy);
            s->pending_sfx = (uint8_t)sfx;
            if (parked) {
                s->flush = 3;
                return actionscript_frame_take_push();
            }
            s->phase = ET_DISPLAY;
            return STEP_RESULT_CONTINUE();
        }
        if (refresh) {
            /* goto update_target_display: one render frame, no change, no sfx. */
            if (et_display_work(s)) {
                s->flush = 1;
                return actionscript_frame_take_push();
            }
            s->phase = ET_PRIME;
            return STEP_RESULT_CONTINUE();
        }

        /* Idle: the blocking loop's per-iteration update_hppp_meter_and_render. */
        if (update_hppp_meter_work_step()) {
            s->flush = 2;
            return actionscript_frame_take_push();
        }
        return STEP_RESULT_CONTINUE();
    }
    }
}

/* Build a GAME_MODE_BATTLE_ENEMY_SELECT init (select_battle_target's
 * prologue) for the DETERMINE_TARGETING / BATTLE_MENU pushes (declared in
 * battle_internal.h). */
void enemy_select_make_init(ModeState *init, uint16_t allow_cancel,
                            uint16_t action_param) {
    uint16_t current_row;
    /* Start on front row if it has battlers, otherwise back row */
    if (bt.num_battlers_in_front_row != 0)
        current_row = 0;
    else
        current_row = 1;
    /* During Giygas battle, force back row */
    if (bt.giygas_phase != 0)
        current_row = 1;

    *init = (ModeState){0};
    init->battle_enemy_select.phase         = ET_DISPLAY;
    init->battle_enemy_select.allow_cancel  = (uint8_t)allow_cancel;
    init->battle_enemy_select.action_param  = action_param;
    init->battle_enemy_select.current_enemy = 0;
    init->battle_enemy_select.current_row   = current_row;
    init->battle_enemy_select.target_shown  = 0;
}

/*
 * SELECT_BATTLE_ROW (asm/battle/ui/select_battle_row.asm)
 *
 * Interactive row selection UI.
 * Player uses UP/DOWN to switch between front and back rows,
 * A/L to confirm, B/SELECT to cancel.
 *
 * Returns 1 (front) or 2 (back) on confirm, 0 on cancel.
 *
 * allow_cancel: 1 = B/SELECT cancels, 0 = can't cancel.
 */
/* select_battle_row's `display_row` work: flash the row, show its target text,
 * then render one frame. No window recreation (unlike enemy-select), so a row
 * change is a single render frame. */
/* Returns true if the render parked (caller STEP_PUSHes + resumes at flush 1). */
static bool br_render_work(uint16_t current_row) {
    set_battler_flashing(current_row);
    display_battle_target_text(current_row, -1);  /* -1 = row text */
    return targeting_render_work_step();
}

/* GAME_MODE_BATTLE_ROW_SELECT — run-to-completion port of select_battle_row.
 * Three-phase machine (BattleRowSelectPhase) in the verified char-select idiom. */
StepResult mode_step_battle_row_select(ModeState *ms) {
    BattleRowSelectState *s = &ms->battle_row_select;

    /* Resume after a parked actionscript frame (D4b): finish the render, advance. */
    if (s->flush == 1) {        /* BR_RENDER / change render parked */
        s->flush = 0;
        targeting_render_work_flush();
        s->phase = BR_PRIME;
        return STEP_RESULT_CONTINUE();
    }
    if (s->flush == 2) {        /* BR_PRIME / idle hppp frame parked */
        s->flush = 0;
        update_hppp_meter_work_flush();
        s->phase = BR_INPUT;
        return STEP_RESULT_CONTINUE();
    }

    switch ((BattleRowSelectPhase)s->phase) {
    case BR_RENDER:
        if (br_render_work(s->current_row)) {
            s->flush = 1;
            return actionscript_frame_take_push();
        }
        s->phase = BR_PRIME;
        return STEP_RESULT_CONTINUE();

    case BR_PRIME:
        if (update_hppp_meter_work_step()) {
            s->flush = 2;
            return actionscript_frame_take_push();
        }
        s->phase = BR_INPUT;
        return STEP_RESULT_CONTINUE();

    case BR_INPUT:
    default: {
        uint16_t pressed = platform_input_get_pad_new();
        uint16_t target_row = 0xFFFF;
        uint16_t sfx = 3;  /* SFX::CURSOR3 */

        if (pressed & PAD_UP)
            target_row = 1;  /* want back row */
        if (pressed & PAD_DOWN)
            target_row = 0;  /* want front row */

        /* A/L: confirm */
        if (pressed & PAD_CONFIRM) {
            clear_battler_flashing();
            uint16_t result = s->current_row + 1;  /* 1=front, 2=back */
            play_sfx(1);  /* SFX::CURSOR1 */
            close_focus_window();
            return STEP_RESULT_POP(result);
        }

        /* B/SELECT: cancel */
        if ((pressed & PAD_CANCEL) && s->allow_cancel == 1) {
            clear_battler_flashing();
            play_sfx(2);  /* SFX::CURSOR2 */
            close_focus_window();
            return STEP_RESULT_POP(0);
        }

        /* Validate and apply row change */
        if (target_row != 0xFFFF) {
            bool valid = false;
            if (target_row == 0 && bt.num_battlers_in_front_row != 0)
                valid = true;
            if (target_row == 1 && bt.num_battlers_in_back_row != 0)
                valid = true;

            if (valid) {
                /* goto display_row: play_sfx then re-render in this frame (yield A),
                 * then BR_PRIME's update_hppp (yield B) before the next input. */
                play_sfx(sfx);
                s->current_row = target_row;
                if (br_render_work(s->current_row)) {
                    s->flush = 1;
                    return actionscript_frame_take_push();
                }
                s->phase = BR_PRIME;
                return STEP_RESULT_CONTINUE();
            }
        }

        /* Idle: the blocking loop's per-iteration update_hppp_meter_and_render. */
        if (update_hppp_meter_work_step()) {
            s->flush = 2;
            return actionscript_frame_take_push();
        }
        return STEP_RESULT_CONTINUE();
    }
    }
}

/* Build a GAME_MODE_BATTLE_ROW_SELECT init (select_battle_row's prologue)
 * for DETERMINE_TARGETING's push. */
static void row_select_make_init(ModeState *init, uint16_t allow_cancel) {
    uint16_t current_row;
    /* Start on front row if it has battlers, otherwise back row */
    if (bt.num_battlers_in_front_row != 0)
        current_row = 0;
    else
        current_row = 1;

    *init = (ModeState){0};
    init->battle_row_select.phase        = BR_RENDER;
    init->battle_row_select.allow_cancel = (uint8_t)allow_cancel;
    init->battle_row_select.current_row  = current_row;
}

/* SELECT_BATTLE_TARGET_DISPATCH (asm/battle/ui/select_battle_target_dispatch
 * .asm) — the blocking mode-0/mode-1 dispatcher — was deleted when its last
 * caller (battle_routine's command menu) became GAME_MODE_BATTLE_MENU; its
 * callers now push GAME_MODE_BATTLE_ENEMY_SELECT / GAME_MODE_BATTLE_ROW_SELECT
 * directly via the *_make_init builders above. */


/*
 * GAME_MODE_DETERMINE_TARGETING step — run-to-completion port of
 * determine_targetting() (asm/battle/determine_targetting.asm). See
 * TargetingState in mode_stack.h.
 *
 * Core targeting logic for battle actions (items, PSI, etc.).
 * Looks up the action's direction and target type from battle_action_table,
 * then dispatches to the appropriate targeting UI (a STEP_PUSHed child mode)
 * or auto-selection (resolved inline, popping without a yield).
 *
 * Pops packed value: (targeting_mode << 8) | target_index.
 *   targeting_mode: TARGETTED_* flags (ENEMIES|SINGLE, ALLIES|ALL, etc.)
 *   target_index: selected target (0xFF=auto, 1+=enemy/ally index)
 *   Pops 0 if a targeting UI was cancelled.
 */
StepResult mode_step_determine_targeting(ModeState *ms) {
    TargetingState *st = &ms->targeting;
    /* Child init for a push this dispatch turn (the pump copies it). */
    static ModeState child_init;

    switch ((TargetingPhase)st->phase) {

    case TGT_ENTER: {
        if (!battle_action_table) return STEP_RESULT_POP(0);

        uint8_t direction = battle_action_table[st->action_id].direction;
        uint8_t target_type = battle_action_table[st->action_id].target;
        uint8_t target_index = 0xFF;

        if (direction == ACTION_DIRECTION_PARTY) {
            /* Enemy-targeting action */
            st->targeting_mode = TARGETTED_ENEMIES;

            switch (target_type) {
            case ACTION_TARGET_NONE:
                /* No specific target selection — default to single with
                 * auto-target. Assembly: stores char_id as VIRTUAL01,
                 * returns immediately. */
                st->targeting_mode = TARGETTED_ENEMIES | TARGETTED_SINGLE;
                target_index = (uint8_t)st->char_id;
                break;

            case ACTION_TARGET_ONE:
                /* Single enemy selection via target picker
                 * (select_battle_target_dispatch mode 0, allow_cancel=1) */
                st->targeting_mode = TARGETTED_ENEMIES | TARGETTED_SINGLE;
                enemy_select_make_init(&child_init, 1, st->action_id);
                st->phase = TGT_PICK_RESULT;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ENEMY_SELECT,
                                             &child_init);

            case ACTION_TARGET_RANDOM: {
                /* Random enemy — pick from alive enemies */
                st->targeting_mode = TARGETTED_ENEMIES | TARGETTED_SINGLE;
                uint16_t count = battle_count_chars(1);
                if (count > 0) {
                    target_index = (uint8_t)(rand_byte() % count + 1);
                }
                break;
            }

            case ACTION_TARGET_ROW:
                /* Row selection via target picker
                 * (select_battle_target_dispatch mode 1, allow_cancel=1) */
                st->targeting_mode = TARGETTED_ENEMIES | TARGETTED_ROW;
                row_select_make_init(&child_init, 1);
                st->phase = TGT_PICK_RESULT;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ROW_SELECT,
                                             &child_init);

            case ACTION_TARGET_ALL:
            default:
                /* All enemies */
                st->targeting_mode |= TARGETTED_ALL;
                break;
            }
        } else {
            /* Ally-targeting action */
            st->targeting_mode = TARGETTED_ALLIES;

            switch (target_type) {
            case ACTION_TARGET_NONE:
                /* No specific target — default to self */
                st->targeting_mode = TARGETTED_SINGLE | TARGETTED_ALLIES;
                target_index = (uint8_t)st->char_id;
                break;

            case ACTION_TARGET_ONE: {
                /* Single ally selection */
                st->targeting_mode = TARGETTED_SINGLE | TARGETTED_ALLIES;
                uint16_t party_count = game_state.player_controlled_party_count & 0xFF;
                if (party_count <= 1) {
                    /* Only one party member — auto-select */
                    target_index = (uint8_t)st->char_id;
                    break;
                }
                /* "Whom?" prompt + char_select_prompt(1, 1, NULL, NULL)'s
                 * name window, with its SELECTION_MENU STEP_PUSHed; the
                 * mode-1 epilogue runs in TGT_ALLY_RESULT after it pops. */
                st->saved_argument_memory = get_argument_memory();
                display_menu_header_text(3);
                st->ally_window_id = char_select_overworld_prepare(NULL);
                child_init = (ModeState){0};
                child_init.selection_menu.phase        = SM_SETUP;
                child_init.selection_menu.allow_cancel = 1;
                st->phase = TGT_ALLY_RESULT;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_SELECTION_MENU,
                                             &child_init);
            }

            case ACTION_TARGET_RANDOM: {
                /* Random ally */
                st->targeting_mode = TARGETTED_ALLIES | TARGETTED_SINGLE;
                uint16_t count = battle_count_chars(0);
                if (count > 0) {
                    uint16_t idx = rand_byte() % count;
                    target_index = game_state.party_order[idx];
                }
                break;
            }

            case ACTION_TARGET_ROW:
            case ACTION_TARGET_ALL:
            default:
                /* All allies */
                st->targeting_mode |= TARGETTED_ALL;
                break;
            }
        }

        /* Pack result: (targeting_mode << 8) | target_index
         * Assembly: ASL16_ENTRY2 with Y=8, then ORA with target_index */
        return STEP_RESULT_POP(((uint16_t)st->targeting_mode << 8) |
                               (uint16_t)target_index);
    }

    case TGT_PICK_RESULT: {
        /* The enemy/row picker popped: 0 = cancelled. */
        uint16_t result = (uint16_t)mode_child_result();
        if (result == 0) return STEP_RESULT_POP(0);
        return STEP_RESULT_POP(((uint16_t)st->targeting_mode << 8) |
                               (uint16_t)(uint8_t)result);
    }

    case TGT_ALLY_RESULT:
    default: {
        /* The ally SELECTION_MENU popped: run char_select_prompt's mode-1
         * epilogue (window close, attrs, pagination, argument_memory), then
         * determine_targetting's own header close. */
        uint16_t result = (uint16_t)mode_child_result();
        char_select_overworld_finish(st->ally_window_id, false);
        set_argument_memory(st->saved_argument_memory);
        close_menu_header_window();
        if (result == 0) return STEP_RESULT_POP(0);  /* cancelled */
        return STEP_RESULT_POP(((uint16_t)st->targeting_mode << 8) |
                               (uint16_t)(uint8_t)result);
    }
    }
}

/*
 * FIND_TARGETTABLE_NPC (asm/battle/find_targettable_npc.asm)
 *
 * 25% chance of returning 0 immediately (no NPC target).
 * Otherwise, scans party_members for NPCs (member_id >= POKEY=5),
 * checks NPC_AI_TABLE for UNTARGETTABLE flag (bit 1),
 * and returns the 1-based target index if a matching conscious battler is found.
 * Returns 0 if no valid NPC target.
 */
static uint16_t find_targettable_npc(void) {
    /* 25% chance to skip NPC targeting entirely */
    if ((rand_byte() & 0x03) == 0)
        return 0;

    for (uint16_t pm = 0; pm < TOTAL_PARTY_COUNT; pm++) {
        uint8_t member_id = game_state.party_members[pm];
        if (member_id < PARTY_MEMBER_POKEY)
            continue;

        /* Check NPC_AI_TABLE for untargettable flag (bit 1) */
        if (npc_ai_table) {
            uint8_t flags = npc_ai_table[member_id * 2];
            if ((flags & 0x02) == 0)  /* NPC_FLAGS::UNTARGETTABLE = 2 */
                continue;
        } else {
            continue;
        }

        /* Search battlers for one with matching npc_id.
         * Assembly uses shared counter LOCAL01 for both loops — when inner
         * loop exhausts without match, LOCAL01 = TOTAL_PARTY_COUNT, incremented
         * to 7, which exits the outer loop immediately. */
        for (uint16_t bi = 0; bi < TOTAL_PARTY_COUNT; bi++) {
            Battler *b = &bt.battlers_table[bi];
            if (b->consciousness == 0)
                continue;
            if ((b->npc_id & 0xFF) == member_id) {
                return bi + 1;  /* 1-based */
            }
        }
        break;  /* No matching battler found — exit outer loop */
    }
    return 0;
}


/*
 * CHOOSE_TARGET (asm/battle/choose_target.asm)
 *
 * Target selection for a battler's current action.
 * 1. Scans front/back rows for valid targets; re-sorts if none found.
 * 2. Reads action direction from battle_action_table to determine default
 *    targeting side (own team vs opposing team).
 * 3. Reads target type (self/single/row/all) and dispatches:
 *    - SELF:   target self
 *    - SINGLE: pick random target (enemies pick from players, players from enemies)
 *              special case: enemies may target NPCs via find_targettable_npc
 *    - ROW:    random front or back row
 *    - ALL:    target everything
 */
void choose_target(uint16_t attacker_offset) {
    Battler *atk = battler_from_offset(attacker_offset);

    /* Phase 1: Check if there's at least one valid target in front/back rows */
    bool found_valid = false;
    for (uint16_t i = 0; i < bt.num_battlers_in_front_row && !found_valid; i++) {
        if (battle_check_if_valid_target(bt.front_row_battlers[i] & 0xFF))
            found_valid = true;
    }
    for (uint16_t i = 0; i < bt.num_battlers_in_back_row && !found_valid; i++) {
        if (battle_check_if_valid_target(bt.back_row_battlers[i] & 0xFF))
            found_valid = true;
    }
    if (!found_valid) {
        sort_battlers_into_rows();
    }

    /* Phase 2: Set default action_targetting based on action direction and side.
     * direction byte (byte 0 of battle_action_table entry):
     *   0 = targets same side (enemy→enemy, player→player)
     *   non-zero = targets opposite side (enemy→player, player→enemy) */
    uint8_t direction = 0;
    if (battle_action_table) {
        direction = battle_action_table[atk->current_action].direction;
    }

    if (direction == 0) {
        /* Same-side targeting */
        if ((atk->ally_or_enemy & 0xFF) == 1) {
            /* Enemy targets own side (enemies) */
            atk->action_targetting = 0;
        } else {
            /* Player targets opposite side (enemies) */
            atk->action_targetting = 0x10;
        }
    } else {
        /* Opposite-side targeting */
        if ((atk->ally_or_enemy & 0xFF) == 1) {
            /* Enemy targets players */
            atk->action_targetting = 0x10;
        } else {
            /* Player targets own side (players/allies) */
            atk->action_targetting = 0;
        }
    }

    /* Phase 3: Read target type and dispatch */
    uint8_t target_type = 0;
    if (battle_action_table) {
        target_type = battle_action_table[atk->current_action].target;
    }

    switch (target_type) {
    case 0: {
        /* TARGET_TYPE_SELF: target the attacker itself */
        atk->action_targetting |= 0x01;
        uint16_t slot = battler_to_offset(atk) / sizeof(Battler);
        if ((atk->ally_or_enemy & 0xFF) == 1) {
            /* Enemy self-target: use set_battler_target */
            set_battler_target(attacker_offset, slot);
        } else {
            /* Player self-target: store slot+1 directly */
            atk->current_target = (uint8_t)(slot + 1);
        }
        break;
    }
    case 1:
    case 2: {
        /* TARGET_TYPE_SINGLE: pick a single target */
        atk->action_targetting |= 0x01;

        /* Read the direction byte again to determine targeting logic */
        if ((atk->ally_or_enemy & 0xFF) == 1) {
            /* Enemy attacker */
            if (direction == 0) {
                /* Targets own side (enemies): try NPC first, then random enemy */
                uint16_t npc_target = find_targettable_npc();
                if (npc_target != 0) {
                    atk->current_target = (uint8_t)npc_target;
                    break;
                }
                /* Pick random player target */
                for (;;) {
                    uint8_t roll = rand_byte() & 0x07;
                    roll++;
                    atk->current_target = roll;
                    if (battle_check_if_valid_target(roll - 1))
                        break;
                }
            } else {
                /* Targets opposite side (players): random player */
                for (;;) {
                    uint16_t slot = pick_random_enemy_target(attacker_offset);
                    if (battle_check_if_valid_target(slot))
                        break;
                }
            }
        } else {
            /* Player attacker */
            if (direction == 0) {
                /* Targets same side (players/allies): random enemy */
                for (;;) {
                    uint16_t slot = pick_random_enemy_target(attacker_offset);
                    if (battle_check_if_valid_target(slot))
                        break;
                }
            } else {
                /* Targets opposite side (enemies): random player */
                for (;;) {
                    uint8_t roll = rand_byte() & 0x07;
                    roll++;
                    atk->current_target = roll;
                    if (battle_check_if_valid_target(roll - 1))
                        break;
                }
            }
        }
        break;
    }
    case 3: {
        /* TARGET_TYPE_ROW: target a row */
        atk->action_targetting |= 0x02;
        if ((atk->ally_or_enemy & 0xFF) == 1) {
            /* Enemy: target front row (1) */
            atk->current_target = 1;
        } else if (bt.num_battlers_in_front_row == 0) {
            /* No front row: target back row (2) */
            atk->current_target = 2;
        } else if (bt.num_battlers_in_back_row == 0) {
            /* No back row: target front row (1) */
            atk->current_target = 1;
        } else {
            /* Both rows exist: random row (1 or 2) */
            atk->current_target = (rand_byte() & 1) + 1;
        }
        break;
    }
    case 4: {
        /* TARGET_TYPE_ALL: target everything */
        atk->action_targetting |= 0x04;
        atk->current_target = 1;
        break;
    }
    default:
        break;
    }
}

/*
 * SET_TARGET_IF_TARGETED (asm/battle/set_target_if_targeted.asm)
 *
 * If bt.battler_target_flags is non-zero, finds the first targeted battler
 * and sets it as bt.current_target, then updates the target display name.
 */
void set_target_if_targeted(void) {
    if (bt.battler_target_flags == 0)
        return;

    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        if (battle_is_char_targeted(i)) {
            bt.current_target = i * sizeof(Battler);
            fix_target_name();
            return;
        }
    }
}


/* ======================================================================
 * Targeting functions
 * ====================================================================== */

/*
 * TARGET_BATTLER (asm/battle/target_battler.asm)
 *
 * Sets the bit for battler_index in the 32-bit bt.battler_target_flags.
 * Assembly uses POWERS_OF_TWO_32BIT lookup; C uses direct bit shift.
 */
void battle_target_battler(uint16_t battler_index) {
    bt.battler_target_flags |= (1U << battler_index);
}


/*
 * REMOVE_TARGET (asm/battle/remove_target.asm)
 *
 * Clears the bit for battler_index in bt.battler_target_flags.
 */
void battle_remove_target(uint16_t battler_index) {
    bt.battler_target_flags &= ~(1U << battler_index);
}


/*
 * IS_CHAR_TARGETTED (asm/battle/is_char_targetted.asm)
 *
 * Returns 1 if battler_index's bit is set in bt.battler_target_flags, else 0.
 */
uint16_t battle_is_char_targeted(uint16_t battler_index) {
    return (bt.battler_target_flags & (1U << battler_index)) ? 1 : 0;
}


/*
 * TARGET_ALL (asm/battle/target_all.asm)
 *
 * Sets target bits for all conscious battlers.
 */
void battle_target_all(void) {
    bt.battler_target_flags = 0;
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        if (bt.battlers_table[i].consciousness != 0)
            bt.battler_target_flags |= (1U << i);
    }
}


/*
 * TARGET_ALL_ENEMIES (asm/battle/target_all_enemies.asm)
 *
 * Sets target bits for all conscious enemies (ally_or_enemy == 1).
 */
void battle_target_all_enemies(void) {
    bt.battler_target_flags = 0;
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        if (bt.battlers_table[i].consciousness != 0 &&
            bt.battlers_table[i].ally_or_enemy == 1)
            bt.battler_target_flags |= (1U << i);
    }
}


/*
 * RANDOM_TARGETTING (asm/battle/random_targetting.asm)
 *
 * Given a target bitmask, randomly selects one set bit and returns
 * a mask with only that bit set. Generates a random count [1..BATTLER_COUNT],
 * then cyclically scans set bits starting from position 0, returning the Nth
 * set bit found. Returns 0 if input mask is 0.
 */
uint32_t battle_random_targeting(uint32_t target_mask) {
    if (target_mask == 0)
        return 0;

    uint32_t mask_copy = target_mask;
    uint16_t position = 0;
    uint16_t count = (rand_byte() & 0x1F) + 1;  /* [1..32] random skip count */

    /* Skip 'count' set bits cyclically */
    while (count > 0) {
        /* Advance to next position (wrapping at BATTLER_COUNT) */
        position++;
        if (position >= BATTLER_COUNT)
            position = 0;
        /* Check if bit is set */
        if (mask_copy & (1U << position)) {
            count--;
        }
    }

    return (1U << position);
}


/*
 * TARGET_ROW (asm/battle/target_row.asm)
 *
 * Sets bt.battler_target_flags based on parameter:
 *   0 = target all conscious allies (ally_or_enemy == 0)
 *   1 = target conscious enemies in row 0 (front)
 *   2 = target conscious enemies in row 1 (back)
 */
void battle_target_row(uint16_t param) {
    bt.battler_target_flags = 0;
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0)
            continue;
        if (param == 0) {
            /* Target allies */
            if (b->ally_or_enemy == 0)
                bt.battler_target_flags |= (1U << i);
        } else if (param == 1 || param == 2) {
            /* Target enemies in specific row */
            if (b->ally_or_enemy != 1)
                continue;
            if (b->row == (uint8_t)(param - 1))
                bt.battler_target_flags |= (1U << i);
        }
    }
}


/*
 * REMOVE_DEAD_TARGETTING (asm/battle/remove_dead_targetting.asm)
 *
 * Iterates all battlers; if targeted and unconscious, removes from targeting.
 */
void battle_remove_dead_targeting(void) {
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        if (!battle_is_char_targeted(i))
            continue;
        if (bt.battlers_table[i].afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS)
            battle_remove_target(i);
    }
}


/*
 * CHECK_IF_VALID_TARGET (asm/battle/check_if_valid_target.asm)
 *
 * Returns 1 if battler at given index is:
 *   - conscious
 *   - not an NPC
 *   - not unconscious or diamondized
 */
uint16_t battle_check_if_valid_target(uint16_t battler_index) {
    Battler *b = &bt.battlers_table[battler_index];
    if (b->consciousness == 0)
        return 0;
    if (b->npc_id != 0)
        return 0;
    uint8_t status0 = b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
    if (status0 == STATUS_0_UNCONSCIOUS || status0 == STATUS_0_DIAMONDIZED)
        return 0;
    return 1;
}


/*
 * REMOVE_STATUS_UNTARGETTABLE_TARGETS (asm/battle/remove_status_untargettable_targets.asm)
 *
 * If current attacker's action is NOT in the dead-targettable list,
 * removes any unconscious or diamondized battlers from the target flags.
 */
void battle_remove_status_untargettable_targets(void) {
    /* Check if current action is dead-targettable */
    Battler *atk = battler_from_offset(bt.current_attacker);
    for (uint16_t i = 0; dead_targettable_actions[i] != 0; i++) {
        if (dead_targettable_actions[i] == atk->current_action)
            return;  /* Action can target dead - skip removal */
    }

    /* Remove unconscious/diamondized targets */
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        if (!battle_is_char_targeted(i))
            continue;
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0 ||
            b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS ||
            b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_DIAMONDIZED) {
            battle_remove_target(i);
        }
    }
}


/*
 * SET_BATTLER_TARGETS_BY_ACTION (asm/battle/set_battler_targets_by_action.asm)
 *
 * Sets bt.battler_target_flags based on the attacker's action_targetting type:
 *   1 (ONE)    → target single battler by bt.current_target index
 *   2,4 (RANDOM, ALL) → target all allies, check shield, remove NPC/dead
 *   17         → target by row (front/back), special PSI_HEALING_OMEGA logic
 *   18         → target row, remove NPC/dead
 *   20         → target all enemies, remove NPC/dead
 */
void set_battler_targets_by_action(uint16_t attacker_offset) {
    Battler *atk = battler_from_offset(attacker_offset);
    bt.battler_target_flags = 0;

    uint8_t targeting = atk->action_targetting & 0xFF;

    switch (targeting) {
    case 1: {  /* ONE: single target */
        uint16_t target_idx = (atk->current_target & 0xFF) - 1;
        battle_target_battler(target_idx);
        break;
    }
    case 2:  /* RANDOM */
    case 4: {  /* ALL → target allies */
        battle_target_allies();
        uint16_t shield = battle_get_shield_targeting(atk->current_action);
        if (shield == 0 && atk->ally_or_enemy == 0)
            battle_remove_npc_targeting();
        battle_remove_status_untargettable_targets();
        break;
    }
    case 17: {  /* Single target by row position */
        uint16_t target_pos = atk->current_target & 0xFF;
        uint16_t battler_idx;
        if (target_pos > bt.num_battlers_in_front_row) {
            /* Back row */
            uint16_t back_idx = target_pos - bt.num_battlers_in_front_row - 1;
            battler_idx = bt.back_row_battlers[back_idx] & 0xFF;
        } else {
            /* Front row */
            battler_idx = bt.front_row_battlers[target_pos - 1] & 0xFF;
        }
        battle_target_battler(battler_idx);

        /* Special case: PSI_HEALING_OMEGA targets first unconscious battler instead */
        if (atk->current_action == BATTLE_ACTION_PSI_HEALING_OMEGA) {
            for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
                Battler *b = &bt.battlers_table[i];
                if (b->consciousness == 0)
                    continue;
                if (b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS) {
                    bt.battler_target_flags = 0;
                    battle_target_battler(i);
                    break;
                }
            }
        }
        break;
    }
    case 18: {  /* Target row */
        uint16_t row_param = atk->current_target & 0xFF;
        battle_target_row(row_param);
        battle_remove_npc_targeting();
        battle_remove_status_untargettable_targets();
        break;
    }
    case 20: {  /* Target all enemies */
        battle_target_all_enemies();
        if (atk->ally_or_enemy == 0)
            battle_remove_npc_targeting();
        battle_remove_status_untargettable_targets();
        break;
    }
    default:
        break;
    }
}


/* ======================================================================
 * Additional targeting functions
 * ====================================================================== */

/*
 * TARGET_ALLIES (asm/battle/target_allies.asm)
 *
 * Targets all conscious battlers who are party members (ally_or_enemy == 0)
 * or NPC helpers (ally_or_enemy != 0 but npc_id != 0).
 */
void battle_target_allies(void) {
    bt.battler_target_flags = 0;
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0)
            continue;
        if (b->ally_or_enemy == 0) {
            /* Party member */
            bt.battler_target_flags |= (1U << i);
        } else if (b->npc_id != 0) {
            /* Enemy-side NPC helper */
            bt.battler_target_flags |= (1U << i);
        }
    }
}


/*
 * REMOVE_NPC_TARGETTING (asm/battle/remove_npc_targetting.asm)
 *
 * Clears target bits for any battler with npc_id != 0.
 */
void battle_remove_npc_targeting(void) {
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0)
            continue;
        if (b->npc_id != 0)
            bt.battler_target_flags &= ~(1U << i);
    }
}


/*
 * FEELING_STRANGE_RETARGETTING (asm/battle/feeling_strange_retargetting.asm)
 *
 * Re-rolls targeting for a confused battler based on their action's target type:
 *   ONE    → pick one random target from all conscious battlers
 *   RANDOM → target a random row (rand() % 3: 0=allies, 1=row0, 2=row1)
 *   ALL    → 50/50 target all allies or all enemies; check shield targeting
 */
void battle_feeling_strange_retargeting(void) {
    bt.battler_target_flags = 0;
    Battler *atk = battler_from_offset(bt.current_attacker);
    uint8_t targeting = atk->action_targetting & 0x07;

    if (targeting == ACTION_TARGET_ONE) {
        /* Target one random battler from all conscious */
        battle_target_all();
        uint32_t all_mask = bt.battler_target_flags;
        bt.battler_target_flags = battle_random_targeting(all_mask);
    } else if (targeting == ACTION_TARGET_RANDOM) {
        /* Target a random row */
        uint16_t row = rand_byte() % 3;
        battle_target_row(row);
    } else if (targeting == ACTION_TARGET_ALL) {
        /* 50/50 allies or enemies */
        if (rand_byte() & 1)
            battle_target_allies();
        else
            battle_target_all_enemies();

        /* Check if shield action → skip NPC removal */
        uint16_t shield = battle_get_shield_targeting(atk->current_action);
        if (shield != 0)
            return;

        /* If attacker is an ally, remove NPCs from targeting */
        if (atk->ally_or_enemy == 0)
            battle_remove_npc_targeting();
    }
}


/*
 * IS_ROW_VALID (asm/battle/is_row_valid.asm)
 *
 * Always returns 1 — matches assembly (LDA #1; END_C_FUNCTION).
 */
uint16_t is_row_valid(void) {
    return 1;
}


/*
 * PICK_RANDOM_ENEMY_TARGET (asm/battle/pick_random_enemy_target.asm)
 *
 * Randomly picks an enemy from front+back rows for the given attacker.
 * Stores the 1-based target index into battler::bt.current_target, and
 * returns the selected battler slot index.
 *
 * A = attacker offset into bt.battlers_table
 */
uint16_t pick_random_enemy_target(uint16_t attacker_offset) {
    uint16_t total = bt.num_battlers_in_front_row + bt.num_battlers_in_back_row;
    uint16_t roll = rand_limit(total);
    roll++; /* 1-based */

    Battler *attacker = battler_from_offset(attacker_offset);
    attacker->current_target = (uint8_t)roll;

    uint16_t slot;
    if (roll > bt.num_battlers_in_front_row) {
        /* Back row */
        uint16_t back_idx = roll - bt.num_battlers_in_front_row - 1;
        slot = bt.back_row_battlers[back_idx] & 0xFF;
    } else {
        /* Front row */
        slot = bt.front_row_battlers[roll - 1] & 0xFF;
    }
    return slot;
}


/*
 * CHECK_BATTLE_TARGET_TYPE — Port of asm/battle/check_battle_target_type.asm (36 lines).
 *
 * Checks bt.current_target's type and applies the appropriate PSI battle effect:
 *   - If target is TINY_LIL_GHOST → return true (no effect applied)
 *   - If target is ally (ally_or_enemy == 0) → apply ally_effect, return false
 *   - If target is enemy → apply enemy_effect, return true
 */
bool check_battle_target_type(uint16_t ally_effect, uint16_t enemy_effect) {
    Battler *target = battler_from_offset(bt.current_target);

    /* Ghost target: no animation */
    if (target->npc_id == ENEMY_TINY_LIL_GHOST)
        return true;

    if (target->ally_or_enemy == 0) {
        /* Ally target */
        apply_psi_battle_effect(ally_effect);
        return false;
    } else {
        /* Enemy target */
        apply_psi_battle_effect(enemy_effect);
        return true;
    }
}

