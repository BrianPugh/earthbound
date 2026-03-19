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
#include "data/battle_text_data.h"

#define BATTLE_ROW_TEXT_LENGTH 13
#include "game/overworld.h"
#include "game/battle_bg.h"
#include "game/fade.h"
#include "entity/entity.h"
#include "include/pad.h"
#include "snes/ppu.h"
#include "platform/platform.h"
#include "game_main.h"

/* ITEM_USABLE_FLAGS table (from asm/data/item_usable_flags.asm)
 * Indexed by char_id - 1 (0=Ness, 1=Paula, 2=Jeff, 3=Poo) */
static const uint8_t item_usable_flags[4] = { 0x01, 0x02, 0x04, 0x08 };

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
static uint16_t select_battle_target(uint16_t allow_cancel, uint16_t action_param) {
    uint16_t current_enemy = 0;     /* @LOCAL04: index within current row */
    uint16_t target_shown = 0;      /* @LOCAL05: set after first display */
    uint16_t current_row;           /* @VIRTUAL04: 0=front, 1=back */

    /* Start on front row if it has battlers, otherwise back row */
    if (bt.num_battlers_in_front_row != 0)
        current_row = 0;
    else
        current_row = 1;

    /* During Giygas battle, force back row */
    if (bt.giygas_phase != 0)
        current_row = 1;

update_target_display:
    {
        uint16_t x_pos = get_battler_row_x_position(current_row, current_enemy);
        enemy_flashing_on(current_row, current_enemy);

        if (!target_shown) {
            display_battle_target_text(current_row, (int16_t)current_enemy);
        }
        target_shown++;

        /* WINDOW_TICK equivalent — render one frame */
        render_all_windows();
        upload_battle_screen_to_vram();
        run_actionscript_frame();
        sync_palettes_to_cgram();
        battle_bg_update();
        wait_for_vblank();

        /* Input loop */
        while (!platform_input_quit_requested()) {
            update_hppp_meter_and_render();

            uint16_t pressed = platform_input_get_pad_new();
            uint16_t sfx = 2;  /* SFX::CURSOR2 default */

            /* UP: switch to back row if on front row */
            if (pressed & PAD_UP) {
                if (current_row == 0 && bt.num_battlers_in_back_row != 0)
                    goto switch_row;
            }

            /* DOWN: switch to front row if on back row */
            if (pressed & PAD_DOWN) {
                if (current_row == 1 && bt.num_battlers_in_front_row != 0)
                    goto switch_row;
            }

            /* LEFT: find previous battler (assembly @CHECK_LEFT) */
            if (pressed & PAD_LEFT) {
                int16_t found = find_prev_battler_in_row(current_row, x_pos,
                                                         action_param);
                if (found != -1) {
                    current_enemy = (uint16_t)found;
                    goto apply_selection;
                }
                /* Try other row */
                uint16_t other_row = current_row ^ 1;
                found = find_prev_battler_in_row(other_row, x_pos,
                                                 action_param);
                if (found == -1)
                    goto update_target_display;
                current_enemy = (uint16_t)found;
                current_row = other_row;
                goto apply_selection_common;
            }

            /* RIGHT: find next battler (assembly @CHECK_RIGHT) */
            if (pressed & PAD_RIGHT) {
                int16_t found = find_next_battler_in_row(current_row, x_pos,
                                                         action_param);
                if (found != -1) {
                    current_enemy = (uint16_t)found;
                    goto apply_selection;
                }
                /* Try other row */
                uint16_t other_row = current_row ^ 1;
                found = find_next_battler_in_row(other_row, x_pos,
                                                 action_param);
                if (found == -1)
                    goto update_target_display;
                current_enemy = (uint16_t)found;
                current_row = other_row;
                goto apply_selection_common;
            }

            /* A/L: confirm selection */
            if (pressed & PAD_CONFIRM) {
                enemy_flashing_off();
                uint16_t result = current_row * bt.num_battlers_in_front_row +
                                  current_enemy + 1;
                play_sfx(1);  /* SFX::CURSOR1 */
                close_focus_window();
                return result;
            }

            /* B/SELECT: cancel */
            if (pressed & PAD_CANCEL) {
                if (allow_cancel == 1) {
                    enemy_flashing_off();
                    play_sfx(2);  /* SFX::CURSOR2 */
                    close_focus_window();
                    return 0;
                }
            }

            continue;

        switch_row:
            sfx = 3;  /* SFX::CURSOR3 */
            {
                uint16_t new_row = current_row ^ 1;
                /* Find nearest battler in other row starting from current x - 1 */
                int16_t found = find_next_battler_in_row(new_row, x_pos - 1,
                                                         action_param);
                if (found != -1) {
                    current_enemy = (uint16_t)found;
                    current_row = new_row;
                    goto apply_selection_common;
                }
                /* Try from x + 1 going backward */
                found = find_prev_battler_in_row(new_row, x_pos + 1,
                                                 action_param);
                if (found == -1)
                    goto update_target_display;
                current_enemy = (uint16_t)found;
                current_row = new_row;
                goto apply_selection_common;
            }

        apply_selection:
            /* Keep current_row, just update enemy */
            goto apply_selection_common;

        apply_selection_common:
            target_shown = 0;  /* reset to show new target text */
            /* Recreate target text window */
            create_window(WINDOW_BATTLE_TARGET_TEXT);
            render_all_windows();
            upload_battle_screen_to_vram();
            run_actionscript_frame();
            sync_palettes_to_cgram();
            battle_bg_update();
            wait_for_vblank();

            x_pos = get_battler_row_x_position(current_row, current_enemy);
            play_sfx(sfx);
            goto update_target_display;
        }
        /* Quit requested */
        close_focus_window();
        return 0;
    }
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
static uint16_t select_battle_row(uint16_t allow_cancel) {
    uint16_t current_row;  /* @LOCAL02: 0=front, 1=back */

    /* Start on front row if it has battlers, otherwise back row */
    if (bt.num_battlers_in_front_row != 0)
        current_row = 0;
    else
        current_row = 1;

display_row:
    set_battler_flashing(current_row);
    display_battle_target_text(current_row, -1);  /* -1 = row text */

    /* WINDOW_TICK equivalent */
    render_all_windows();
    upload_battle_screen_to_vram();
    run_actionscript_frame();
    sync_palettes_to_cgram();
    battle_bg_update();
    wait_for_vblank();

    /* Input loop */
    while (!platform_input_quit_requested()) {
        update_hppp_meter_and_render();
        uint16_t pressed = platform_input_get_pad_new();

        uint16_t target_row = 0xFFFF;
        uint16_t sfx = 3;  /* SFX::CURSOR3 */

        if (pressed & PAD_UP) {
            target_row = 1;  /* want back row */
        }
        if (pressed & PAD_DOWN) {
            target_row = 0;  /* want front row */
        }

        /* A/L: confirm */
        if (pressed & PAD_CONFIRM) {
            clear_battler_flashing();
            uint16_t result = current_row + 1;  /* 1=front, 2=back */
            play_sfx(1);  /* SFX::CURSOR1 */
            close_focus_window();
            return result;
        }

        /* B/SELECT: cancel */
        if (pressed & PAD_CANCEL) {
            if (allow_cancel == 1) {
                clear_battler_flashing();
                play_sfx(2);  /* SFX::CURSOR2 */
                close_focus_window();
                return 0;
            }
        }

        /* Validate and apply row change */
        if (target_row != 0xFFFF) {
            bool valid = false;
            if (target_row == 0 && bt.num_battlers_in_front_row != 0)
                valid = true;
            if (target_row == 1 && bt.num_battlers_in_back_row != 0)
                valid = true;

            if (valid) {
                play_sfx(sfx);
                current_row = target_row;
                goto display_row;
            }
        }
    }
    close_focus_window();
    return 0;
}


/*
 * SELECT_BATTLE_TARGET_DISPATCH (asm/battle/ui/select_battle_target_dispatch.asm)
 *
 * Dispatches to SELECT_BATTLE_TARGET (single target) or SELECT_BATTLE_ROW
 * based on the mode parameter.
 *
 * mode: 0 = single target, nonzero = row selection.
 * allow_cancel: 1 = can cancel with B/SELECT.
 * action_param: battle action index for targetability check.
 *
 * Returns: 1-based target (single) or row+1 (row), or 0 on cancel.
 */
uint16_t select_battle_target_dispatch(uint16_t mode, uint16_t allow_cancel,
                                              uint16_t action_param) {
    if (mode == 0)
        return select_battle_target(allow_cancel, action_param);
    else
        return select_battle_row(allow_cancel);
}


/*
 * DETERMINE_TARGETTING (asm/battle/determine_targetting.asm)
 *
 * Core targeting logic for battle actions (items, PSI, etc.).
 * Looks up the action's direction and target type from battle_action_table,
 * then dispatches to the appropriate targeting UI or auto-selection.
 *
 * action_id: index into battle_action_table.
 * char_id: 1-indexed character using the action.
 *
 * Returns packed value: (targeting_mode << 8) | target_index.
 *   targeting_mode: TARGETTED_* flags (ENEMIES|SINGLE, ALLIES|ALL, etc.)
 *   target_index: selected target (0xFF=auto, 1+=enemy/ally index, 0=cancelled)
 *   Returns 0 if cancelled.
 */
uint16_t determine_targetting(uint16_t action_id, uint16_t char_id) {
    if (!battle_action_table) return 0;

    uint8_t direction = battle_action_table[action_id].direction;
    uint8_t target_type = battle_action_table[action_id].target;
    uint8_t targeting_mode;
    uint8_t target_index = 0xFF;

    if (direction == ACTION_DIRECTION_PARTY) {
        /* Enemy-targeting action */
        targeting_mode = TARGETTED_ENEMIES;

        switch (target_type) {
        case ACTION_TARGET_NONE:
            /* No specific target selection — default to single with auto-target.
             * Assembly: stores char_id as VIRTUAL01, returns immediately. */
            targeting_mode = TARGETTED_ENEMIES | TARGETTED_SINGLE;
            target_index = (uint8_t)char_id;
            break;

        case ACTION_TARGET_ONE: {
            /* Single enemy selection via target picker */
            targeting_mode = TARGETTED_ENEMIES | TARGETTED_SINGLE;
            uint16_t result = select_battle_target_dispatch(0, 1, action_id);
            if (result == 0) return 0;  /* cancelled */
            target_index = (uint8_t)result;
            break;
        }

        case ACTION_TARGET_RANDOM: {
            /* Random enemy — pick from alive enemies */
            targeting_mode = TARGETTED_ENEMIES | TARGETTED_SINGLE;
            uint16_t count = battle_count_chars(1);
            if (count > 0) {
                target_index = (uint8_t)(rand_byte() % count + 1);
            }
            break;
        }

        case ACTION_TARGET_ROW: {
            /* Row selection via target picker */
            targeting_mode = TARGETTED_ENEMIES | TARGETTED_ROW;
            uint16_t result = select_battle_target_dispatch(1, 1, action_id);
            if (result == 0) return 0;  /* cancelled */
            target_index = (uint8_t)result;
            break;
        }

        case ACTION_TARGET_ALL:
        default:
            /* All enemies */
            targeting_mode |= TARGETTED_ALL;
            break;
        }
    } else {
        /* Ally-targeting action */
        targeting_mode = TARGETTED_ALLIES;

        switch (target_type) {
        case ACTION_TARGET_NONE:
            /* No specific target — default to self */
            targeting_mode = TARGETTED_SINGLE | TARGETTED_ALLIES;
            target_index = (uint8_t)char_id;
            break;

        case ACTION_TARGET_ONE: {
            /* Single ally selection */
            targeting_mode = TARGETTED_SINGLE | TARGETTED_ALLIES;
            uint16_t party_count = game_state.player_controlled_party_count & 0xFF;
            if (party_count <= 1) {
                /* Only one party member — auto-select */
                target_index = (uint8_t)char_id;
            } else {
                /* Show "Whom?" prompt and character selection */
                display_menu_header_text(3);
                uint16_t result = char_select_prompt(1, 1, NULL, NULL);
                close_menu_header_window();
                if (result == 0) return 0;  /* cancelled */
                target_index = (uint8_t)result;
            }
            break;
        }

        case ACTION_TARGET_RANDOM: {
            /* Random ally */
            targeting_mode = TARGETTED_ALLIES | TARGETTED_SINGLE;
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
            targeting_mode |= TARGETTED_ALL;
            break;
        }
    }

    /* Pack result: (targeting_mode << 8) | target_index
     * Assembly: ASL16_ENTRY2 with Y=8, then ORA with target_index */
    return ((uint16_t)targeting_mode << 8) | (uint16_t)target_index;
}


/*
 * DETERMINE_BATTLE_ITEM_TARGET (asm/battle/ui/determine_battle_item_target.asm)
 *
 * Determines targeting for the selected battle item.
 * Reads char_id and item slot from battle_menu_selection,
 * looks up the item's type and effect, then calls determine_targetting().
 * Stores results (selected_action, targetting, selected_target) in
 * battle_menu_selection fields.
 *
 * Returns 0 if cancelled, nonzero on success.
 */
uint16_t determine_battle_item_target(void) {
    uint16_t result = 0xFF;  /* default: success */

    uint8_t item_slot = bt.battle_menu_param1;
    uint8_t user_id = bt.battle_menu_user;
    uint16_t item_id = get_character_item(user_id, item_slot);

    const ItemConfig *item_entry = get_item_entry(item_id);
    if (!item_entry) return result;

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
        uint16_t effect = item_entry->effect_id;

        result = determine_targetting(effect, user_id);
        if ((result & 0xFF) == 0) return 0;  /* cancelled */

        /* Store effect as selected_action */
        bt.battle_menu_selected_action = effect;
        /* Unpack targeting: high byte = mode, low byte = target */
        bt.battle_menu_targetting = (uint8_t)(result >> 8);
        bt.battle_menu_selected_target = (uint8_t)(result & 0xFF);
    } else if (type_category == 0x30) {
        /* Equipment item — check if usable in battle */
        uint8_t equip_subtype = item_type & 0x0C;
        if (equip_subtype != 0 && equip_subtype != 4) {
            /* Not a battle-usable equipment subtype — return with defaults.
             * Assembly: jumps to SET_DEFAULT_ACTION without modifying selected_action. */
            goto done;
        }

        /* Check if this character can use this equipment */
        if (user_id >= 1 && user_id <= 4) {
            uint8_t item_flags = item_entry->flags;
            if (!(item_flags & item_usable_flags[user_id - 1])) {
                /* Character can't use this item */
                bt.battle_menu_selected_action = 3;
                goto done;
            }
        }

        /* Equipment is usable — determine targeting from its effect */
        uint16_t effect = item_entry->effect_id;
        result = determine_targetting(effect, user_id);
        if ((result & 0xFF) == 0) return 0;  /* cancelled */

        bt.battle_menu_selected_action = effect;
        bt.battle_menu_targetting = (uint8_t)(result >> 8);
        bt.battle_menu_selected_target = (uint8_t)(result & 0xFF);
    } else {
        /* Other item type — no targeting needed, use defaults */
        /* selected_action stays as 2, returns 0xFF */
    }

done:
    return result;
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

