/* door.c — Door system for the C port.
 *
 * Ports the following assembly routines:
 *   FIND_DOOR_AT_POSITION     (asm/overworld/door/find_door_at_position.asm)
 *   PROCESS_DOOR_AT_TILE      (asm/overworld/door/process_door_at_tile.asm)
 *   CHECK_DOOR_EVENT_FLAG     (asm/overworld/door/check_door_event_flag.asm)
 *   TRY_ACTIVATE_DOOR         (asm/overworld/door/try_activate_door.asm)
 *   SET_WALKING_STYLE_STAIRS  (asm/overworld/door/handle_stairs_movement.asm)
 *   CHECK_DOOR_IN_DIRECTION   (asm/overworld/door/check_door_in_direction.asm)
 *   CHECK_DOOR_NEAR_LEADER    (asm/misc/check_door_near_leader.asm)
 *   DOOR_TRANSITION           (asm/overworld/door_transition.asm)
 *   PROCESS_DOOR_INTERACTIONS (asm/overworld/door/process_door_interactions.asm)
 *   GET_SCREEN_TRANSITION_SOUND_EFFECT (asm/overworld/get_screen_transition_sound_effect.asm)
 *   SCREEN_TRANSITION              (asm/overworld/screen_transition.asm)
 *   INIT_TRANSITION_SCROLL_VELOCITY (asm/misc/init_transition_scroll_velocity.asm)
 *   UPDATE_TRANSITION_SCROLL       (asm/misc/update_transition_scroll.asm)
 */

#include "game/door.h"

#include <math.h>
#include "include/binary.h"
#include <string.h>
#include <stdio.h>

#include "data/assets.h"
#include "entity/entity.h"
#include "game/audio.h"
#include "game/battle.h"
#include "game/display_text.h"
#include "game/fade.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "game/oval_window.h"
#include "game/overworld.h"
#include "game/position_buffer.h"
#include "snes/ppu.h"

/* ---- Data buffers — compile-time linked ---- */
#define door_pointer_table       ASSET_DATA(ASSET_MAPS_DOOR_POINTER_TABLE_BIN)
#define door_pointer_table_size  ASSET_SIZE(ASSET_MAPS_DOOR_POINTER_TABLE_BIN)
#define door_config_data         ASSET_DATA(ASSET_MAPS_DOOR_CONFIG_TABLE_BIN)
#define door_config_data_size    ASSET_SIZE(ASSET_MAPS_DOOR_CONFIG_TABLE_BIN)
#define door_data_buf            ASSET_DATA(ASSET_MAPS_DOOR_DATA_BIN)
#define door_data_buf_size       ASSET_SIZE(ASSET_MAPS_DOOR_DATA_BIN)
#define screen_transition_config ((const ScreenTransitionConfig *)ASSET_DATA(ASSET_MAPS_SCREEN_TRANSITION_CONFIG_BIN))
#define screen_transition_config_size ASSET_SIZE(ASSET_MAPS_SCREEN_TRANSITION_CONFIG_BIN)

/* ROM address of DOOR_CONFIG_ENTRY_0 — needed to convert ROM pointers
 * in door_pointer_table to offsets into door_config_data. */
#define DOOR_CONFIG_BASE_ADDR  0xCF264Fu

/* DOOR_DATA starts at the beginning of bank 0x0F ($CF0000).
 * dr.door_found values are .LOWORD() of door entry ROM addresses,
 * i.e. offsets from $CF0000. */
#define DOOR_DATA_BANK_BASE    0xCF0000u

/* (SCREEN_TRANSITION_ENTRY_SIZE removed — ScreenTransitionConfig struct used instead) */

/* ---- Door direction offset tables ----
 * Port of DIRECTION_X_OFFSET_TABLE / DIRECTION_Y_OFFSET_TABLE
 * (asm/data/unknown/C3E230.asm, C3E240.asm).
 * Indexed by direction (0-7), give tile offsets for door search. */
static const int16_t door_dir_x_offsets[8] = {
    0, 1, 1, 1, 0, -1, -1, -1
};
static const int16_t door_dir_y_offsets[8] = {
    -1, -1, 0, 1, 1, 1, 0, -1
};

/* DOOR_ENTRY_DIRECTION_TABLE (asm/data/unknown/C3E1D8.asm):
 * Indexed by (bits 14-15 of unknown6), gives direction enum value
 * used by SET_LEADER_POSITION_AND_LOAD_PARTY. */
static const uint16_t door_dest_direction_table[4] = { 4, 0, 2, 6 };

#include "game_main.h"

/* ---- Globals ---- */
DoorState dr;

/* Transition scroll state and door interactions now in DoorState dr (door.h). */

/* ---- Data loading ---- */


const uint8_t *get_door_data_entry(uint16_t door_found_offset) {
    /* door_found_offset is .LOWORD of the ROM address within bank CF.
     * DOOR_DATA starts at offset 0 within the bank, so the offset
     * directly indexes into our ert.buffer. */
    if (door_found_offset >= door_data_buf_size) return NULL;
    return door_data_buf + door_found_offset;
}

/* ---- FIND_DOOR_AT_POSITION (C07477) ---- */

uint8_t find_door_at_position(uint16_t x_tile, uint16_t y_tile) {
    /* Assembly lines 14-29: calculate sector index and look up pointer.
     * sector_x = x_tile >> 5
     * sector_row = y_tile & 0xFFE0 = (y_tile / 32) * 32
     * index = (sector_row + sector_x) * 4 */
    uint16_t sector_x = x_tile >> 5;
    uint16_t sector_row = y_tile & 0xFFE0;
    uint32_t ptr_index = (uint32_t)(sector_row + sector_x) * 4;

    if (ptr_index + 4 > door_pointer_table_size) return 0xFF;

    /* Read 32-bit ROM pointer from table */
    uint32_t rom_ptr = read_u32_le(door_pointer_table + ptr_index);

    /* Convert ROM pointer to offset into door_config_data */
    if (rom_ptr < DOOR_CONFIG_BASE_ADDR) return 0xFF;
    uint32_t config_offset = rom_ptr - DOOR_CONFIG_BASE_ADDR;
    if (config_offset + 2 > door_config_data_size) return 0xFF;

    /* Read count of doors in this sector */
    const uint8_t *entry = door_config_data + config_offset;
    uint16_t count = read_u16_le(entry);
    entry += 2;

    if (count == 0) return 0xFF;

    /* Assembly lines 41-47: compute position within sector */
    uint8_t x_within = x_tile & 0x1F;
    uint8_t y_within = y_tile & 0x1F;

    /* Assembly lines 49-84: search through door records (5 bytes each) */
    for (uint16_t i = 0; i < count; i++) {
        uint8_t entry_y = entry[0];
        uint8_t entry_x = entry[1];
        uint8_t entry_type = entry[2];
        uint16_t entry_door_offset = read_u16_le(entry + 3);

        if (entry_x == x_within && entry_y == y_within) {
            dr.door_found = entry_door_offset;
            dr.door_found_type = entry_type;
            return entry_type;
        }
        entry += 5;
    }

    return 0xFF;
}

/* ---- CHECK_DOOR_EVENT_FLAG (C06A1B) ---- */

static void check_door_event_flag(uint16_t door_offset) {
    const uint8_t *door = get_door_data_entry(door_offset & 0x7FFF);
    if (!door) return;

    /* Assembly lines 20-22: read event_flag (offset 0) and check it */
    uint16_t event_flag_raw = read_u16_le(door);  /* door_data::event_flag at offset 0 */
    uint16_t flag_id = event_flag_raw & 0x7FFF;
    bool flag_value = event_flag_get(flag_id);

    /* Assembly lines 24-36: determine expected state.
     * If event_flag > 0x8000 (BLTEQ: unsigned <=), expected = 1; else expected = 0. */
    bool expected = (event_flag_raw > 0x8000);

    /* If flag matches expected, queue the door's text interaction */
    if (flag_value == expected) {
        /* Assembly lines 37-52: read text pointer (offset 2, dword) and queue */
        uint32_t text_ptr = read_u32_le(door + 2);  /* door_data::text at offset 2 */
        queue_interaction(0, text_ptr);
        ow.ladder_stairs_tile_y = 0;
        ow.ladder_stairs_tile_x = 0;
    }
}

/* ---- TRY_ACTIVATE_DOOR (C06ACA) ---- */

static void try_activate_door(uint16_t door_offset) {
    /* Assembly lines 10-19: guard conditions */
    if (!ow.player_has_done_something_this_frame) return;
    if (game_state.camera_mode == 2) return;
    if (ow.pending_interactions) return;
    if (ow.enemy_has_been_touched || ow.battle_swirl_countdown) return;

    /* Assembly lines 20-35: set up door interaction */
    door_offset &= 0x7FFF;
    dr.using_door = 1;

    /* Build full ROM pointer: bank base + offset */
    uint32_t data_ptr = DOOR_DATA_BANK_BASE + door_offset;

    queue_interaction(2, data_ptr);
    clear_party_sprite_hide_flags();
}

/* ---- SET_WALKING_STYLE_STAIRS (C070CB) ---- */

static void set_walking_style_stairs(uint16_t door_offset) {
    /* Assembly lines 6-10: if already on stairs (7 or 8), skip */
    if (game_state.walking_style == 7 || game_state.walking_style == 8) {
        return;
    }

    /* Assembly lines 11-18: set walking style based on door_offset.
     * door_offset == 0 → style 7 (stairs up)
     * door_offset != 0 → style 8 (stairs down) */
    if (door_offset == 0) {
        game_state.walking_style = 7;
    } else {
        game_state.walking_style = 8;
    }

    /* Assembly lines 20-23: force leader direction to even (cardinal) */
    game_state.leader_direction &= 0xFFFE;

    /* Assembly lines 24-25: set ow.stairs_direction to -1 */
    ow.stairs_direction = 0xFFFF;
}

/* ====================================================================
 * Escalator Movement System
 *
 * Ports of:
 *   HANDLE_ESCALATOR_MOVEMENT   (asm/overworld/door/handle_escalator_movement.asm)
 *   START_ESCALATOR_MOVEMENT    (asm/overworld/door/start_escalator_movement.asm)
 *   FINISH_ESCALATOR_MOVEMENT   (asm/overworld/door/finish_escalator_movement.asm)
 *   ESCALATOR_OFFSET_TABLE      (asm/data/unknown/C06E02.asm)
 * ==================================================================== */

/* ESCALATOR_OFFSET_TABLE — 4 escalator types × 3 parameter groups.
 * [0-3]: entry X offsets, [4-7]: exit X offsets, [8-11]: movement directions.
 * Indexed by (high byte of DOOR_FOUND, after XBA+AND+ASL). */
static const uint16_t escalator_entry_x_offsets[4] = { 0x08, 0x00, 0x00, 0x08 };
static const uint16_t escalator_exit_x_offsets[4]  = { 0x00, 0x08, 0x00, 0x08 };
static const uint16_t escalator_directions[4]      = { 0x06, 0x02, 0x06, 0x02 };

/* Escalator target coordinates — saved for callback use */
static uint16_t escalator_new_x;
static uint16_t escalator_new_y;
/* Flag set when entering/exiting escalator (assembly: UNREAD_7E5DBA) */
static uint16_t escalator_stair_active;

/* START_ESCALATOR_MOVEMENT callback — port of C06E2C.asm.
 * Sets walking_style to ESCALATOR, snaps leader to target coords. */
static void start_escalator_movement_callback(void) {
    game_state.walking_style = 12;  /* WALKING_STYLE::ESCALATOR */
    ow.player_movement_flags = 0;
    game_state.leader_x_coord = escalator_new_x;
    game_state.leader_y_coord = escalator_new_y;
    game_state.leader_y_frac = 0;
    game_state.leader_x_frac = 0;
}

/* FINISH_ESCALATOR_MOVEMENT callback — port of C06E4A.asm.
 * Clears escalator state and snaps leader to target coords. */
static void finish_escalator_movement_callback(void) {
    ow.stairs_direction = 0xFFFF;
    game_state.walking_style = 0;
    ow.player_movement_flags = 0;
    escalator_stair_active = 0;
    game_state.leader_x_coord = escalator_new_x;
    game_state.leader_y_coord = escalator_new_y;
    game_state.leader_y_frac = 0;
    game_state.leader_x_frac = 0;
}

/* HANDLE_ESCALATOR_MOVEMENT — port of C06E6E.asm.
 * Called when stepping onto a DOOR_TYPE_3 tile.
 * door_val: DOOR_FOUND value (bit 15 = exit, bits 8-9 = escalator variant).
 * tile_x, tile_y: door tile coordinates. */
static void handle_escalator_movement(uint16_t door_val,
                                       uint16_t tile_x, uint16_t tile_y) {
    if (ow.demo_frames_left) return;

    clear_auto_movement_buffer();

    uint16_t tile_x_px = tile_x << 3;
    uint16_t tile_y_px = tile_y << 3;

    if (door_val & 0x8000) {
        /* ---- EXIT ESCALATOR ---- */
        /* Assembly lines 37-40: check if on escalator */
        if (game_state.walking_style != 12)  /* ESCALATOR */
            return;

        /* Clear walking style, set movement flags */
        game_state.walking_style = 0;
        ow.player_movement_flags = 3;

        /* Get escalator variant index from saved entrance direction */
        uint16_t variant = (ow.escalator_entrance_direction >> 8) & 0xFF;
        if (variant > 3) variant = 0;

        /* Calculate target X (tile_x_px + exit offset) */
        uint16_t target_x = tile_x_px + escalator_exit_x_offsets[variant];

        /* Calculate path steps from current leader to target */
        uint16_t steps = calculate_movement_path_steps(
            game_state.leader_x_coord, game_state.leader_y_coord,
            target_x, tile_y_px);

        /* Record repeated movement in the exit direction */
        record_repeated_auto_movement(escalator_directions[variant], 16);

        /* Save target coords for callback */
        escalator_new_x = target_x;
        escalator_new_y = tile_y_px;

        /* Schedule finish callback (steps + 1 frames) */
        schedule_overworld_task(finish_escalator_movement_callback, steps + 1);
        queue_auto_movement_step();

        ow.escalator_entrance_direction = 0;
        escalator_stair_active = 1;
    } else {
        /* ---- ENTER ESCALATOR ---- */
        if (game_state.walking_style == 12)  /* already on escalator */
            return;

        escalator_stair_active = 1;

        /* Save DOOR_FOUND for exit to read back the variant */
        ow.escalator_entrance_direction = door_val;

        /* Get variant index */
        uint16_t variant = (door_val >> 8) & 0xFF;
        if (variant > 3) variant = 0;

        /* Set leader direction to the escalator's movement direction */
        game_state.leader_direction = escalator_directions[variant];
        ow.player_movement_flags = 3;

        /* Calculate target X (tile_x_px + entry offset) */
        uint16_t target_x = tile_x_px + escalator_entry_x_offsets[variant];

        /* Calculate path steps */
        uint16_t steps = calculate_movement_path_steps(
            game_state.leader_x_coord, game_state.leader_y_coord,
            target_x, tile_y_px);

        /* Save target coords */
        escalator_new_x = target_x;
        escalator_new_y = tile_y_px;

        /* Schedule start callback */
        schedule_overworld_task(start_escalator_movement_callback, steps + 1);
        queue_auto_movement_step();
    }

    ow.stairs_direction = 0xFFFF;
}

/* ====================================================================
 * Stairs Movement System
 *
 * Ports of:
 *   HANDLE_STAIRS_MOVEMENT       (asm/overworld/door/handle_stairs_movement.asm)
 *   GET_STAIRS_MOVEMENT_DIRECTION (asm/overworld/door/get_stairs_movement_direction.asm)
 *   HANDLE_STAIRS_ENTER          (asm/overworld/door/handle_stairs_enter.asm)
 *   HANDLE_STAIRS_LEAVE          (asm/overworld/door/handle_stairs_leave.asm)
 *
 * Stair data tables (from asm/data/unknown/C3E200-C3E228):
 *   STAIRS_DIRECTION_TABLE       — diagonal movement direction per variant
 *   STAIRS_FACING_DIRECTION_TABLE— exit facing direction
 *   STAIRS_ENTER_Y/X_OFFSET_TABLE— entry pixel offsets
 *   STAIRS_EXIT_Y/X_OFFSET_TABLE — exit pixel offsets
 * ==================================================================== */

/* 4 stair variants, indexed by variant byte (0-3).
 * Note: in the assembly, the Y_OFFSET table values are added to X coords
 * and the X_OFFSET table values are added to Y coords.  We name them
 * by what they actually affect to avoid confusion. */
static const uint16_t stairs_direction_table[4]       = { 7, 1, 5, 3 };
  /* UP_LEFT, UP_RIGHT, DOWN_LEFT, DOWN_RIGHT */
static const uint16_t stairs_facing_direction_table[4] = { 2, 6, 2, 6 };
  /* RIGHT, LEFT, RIGHT, LEFT */
/* Assembly STAIRS_ENTER_Y_OFFSET → added to tile_x_px → STAIRS_NEW_X */
static const uint16_t stairs_enter_x_adj[4]           = { 0, 8, 0, 8 };
/* Assembly STAIRS_ENTER_X_OFFSET → added to tile_y_px → STAIRS_NEW_Y */
static const uint16_t stairs_enter_y_adj[4]           = { 0, 0, 8, 8 };
/* Assembly STAIRS_EXIT_Y_OFFSET → added to tile_x_px → STAIRS_NEW_X */
static const uint16_t stairs_exit_x_adj[4]            = { 8, 0, 8, 0 };
/* Assembly STAIRS_EXIT_X_OFFSET → added to tile_y_px → STAIRS_NEW_Y */
static const uint16_t stairs_exit_y_adj[4]            = { 8, 8, 0, 0 };

/* Stairs target coordinates — saved for callback use */
static uint16_t stairs_new_x;
static uint16_t stairs_new_y;

/* GET_STAIRS_MOVEMENT_DIRECTION — port of C0705F.asm.
 * Checks if the leader is facing a valid direction to enter stairs.
 * Sets ow.auto_movement_direction.
 * Returns 0 if allowed (proceed with entry), non-zero if blocked. */
static uint16_t get_stairs_movement_direction(uint16_t door_val) {
    uint16_t leader_dir = game_state.leader_direction;
    uint16_t result = 1;  /* default: blocked */

    switch (door_val) {
    case 0x0000:
        /* Variant 0: auto direction = LEFT (6).
         * Allowed when dir != 0 && (dir & 3) == 0. */
        if (leader_dir != 0 && (leader_dir & 0x0003) == 0)
            result = 0;
        ow.auto_movement_direction = 6;
        break;
    case 0x0100:
        /* Variant 1: auto direction = RIGHT (2).
         * Allowed when dir != 0 && (dir & 3) == 0. */
        if (leader_dir != 0 && (leader_dir & 0x0003) == 0)
            result = 0;
        ow.auto_movement_direction = 2;
        break;
    case 0x0200:
        /* Variant 2: auto direction = LEFT (6).
         * Allowed when (dir & 7) != 0 (any non-zero direction). */
        if ((leader_dir & 0x0007) != 0)
            result = 0;
        ow.auto_movement_direction = 6;
        break;
    case 0x0300:
        /* Variant 3: auto direction = RIGHT (2).
         * Allowed when (dir & 7) != 0 (any non-zero direction). */
        if ((leader_dir & 0x0007) != 0)
            result = 0;
        ow.auto_movement_direction = 2;
        break;
    default:
        return 1;  /* unknown variant — blocked */
    }

    return result;
}

/* Forward declarations for stairs callbacks */
static void handle_stairs_enter_callback(void);
static void handle_stairs_leave_callback(void);

/* HANDLE_STAIRS_MOVEMENT — port of C070CB.asm.
 * Called when stepping onto a DOOR_TYPE_4 tile.
 * door_val: DOOR_FOUND value (high byte = variant 0-3).
 * tile_x, tile_y: door tile coordinates. */
static void handle_stairs_movement(uint16_t door_val,
                                    uint16_t tile_x, uint16_t tile_y) {
    if (ow.demo_frames_left) return;

    clear_auto_movement_buffer();

    /* Extract variant from high byte: XBA; AND #$00FF */
    uint16_t variant = (door_val >> 8) & 0xFF;
    if (variant > 3) variant = 0;

    uint16_t tile_x_px = tile_x << 3;
    uint16_t tile_y_px = tile_y << 3;

    if (game_state.walking_style == 0) {
        /* ---- ENTERING STAIRS ---- */

        /* Check if leader direction allows entering.
         * Assembly passes full door_val (TXA) to GET_STAIRS_MOVEMENT_DIRECTION. */
        uint16_t result = get_stairs_movement_direction(door_val);
        if (result != 0)
            return;  /* blocked — wrong direction */

        /* Set leader to auto-movement direction */
        game_state.leader_direction = ow.auto_movement_direction;
        ow.not_moving_in_same_direction_faced = 0;
        ow.player_movement_flags = 3;
        escalator_stair_active = 1;

        /* Set STAIRS_DIRECTION = variant << 8 (XBA; AND #$FF00) */
        ow.stairs_direction = variant << 8;

        /* Calculate entry target position.
         * Assembly: @LOCAL03 = tile_x_px + STAIRS_ENTER_Y_OFFSET (→ target X)
         *           @VIRTUAL02 = tile_y_px + STAIRS_ENTER_X_OFFSET (→ target Y) */
        uint16_t target_x = tile_x_px + stairs_enter_x_adj[variant];
        uint16_t target_y = tile_y_px + stairs_enter_y_adj[variant];

        /* Calculate path steps */
        uint16_t steps = calculate_movement_path_steps(
            game_state.leader_x_coord, game_state.leader_y_coord,
            target_x, target_y);

        /* Assembly: if 0 steps, force to 1 */
        if (steps == 0) steps = 1;

        /* Record repeated diagonal movement (6 repeats) */
        record_repeated_auto_movement(stairs_direction_table[variant], 6);

        /* Save target coords: @LOCAL03 → STAIRS_NEW_X, @VIRTUAL02 → STAIRS_NEW_Y */
        stairs_new_x = target_x;
        stairs_new_y = target_y;

        /* Schedule enter callback */
        schedule_overworld_task(handle_stairs_enter_callback, steps);
    } else {
        /* ---- LEAVING STAIRS ---- */

        /* Calculate exit target position.
         * Same pattern: EXIT_Y_OFFSET → tile_x, EXIT_X_OFFSET → tile_y */
        uint16_t target_x = tile_x_px + stairs_exit_x_adj[variant];
        uint16_t target_y = tile_y_px + stairs_exit_y_adj[variant];

        /* Calculate path steps */
        uint16_t steps = calculate_movement_path_steps(
            game_state.leader_x_coord, game_state.leader_y_coord,
            target_x, target_y);
        if (steps == 0) steps = 1;

        /* Record repeated facing-direction movement (12 repeats) */
        record_repeated_auto_movement(stairs_facing_direction_table[variant], 12);

        /* Save target coords */
        stairs_new_x = target_x;
        stairs_new_y = target_y;

        /* Schedule leave callback */
        schedule_overworld_task(handle_stairs_leave_callback, steps);
    }

    queue_auto_movement_step();
}

/* HANDLE_STAIRS_ENTER callback — port of C06F82.asm.
 * Checks if leader has reached the stair entry point.
 * If yes, sets walking_style to STAIRS and snaps coords.
 * If no, reschedules for 1 more frame. */
static void handle_stairs_enter_callback(void) {
    bool reached = false;

    if (ow.stairs_direction == 0 || ow.stairs_direction == 256) {
        /* Vertical stairs (variant 0/1, UP_LEFT/UP_RIGHT):
         * Assembly: STAIRS_NEW_Y - 1 compared to leader_y (BLTEQ = signed <=).
         * Reached when (stairs_new_y - 1) > leader_y_coord (signed). */
        if ((int16_t)(stairs_new_y - 1) > (int16_t)game_state.leader_y_coord)
            reached = true;
    } else {
        /* Horizontal stairs (variant 2/3, DOWN_LEFT/DOWN_RIGHT):
         * Assembly: STAIRS_NEW_Y + 1 compared to leader_y (BCS = unsigned >=).
         * Reached when (stairs_new_y + 1) < leader_y_coord (unsigned). */
        if ((uint16_t)(stairs_new_y + 1) < (uint16_t)game_state.leader_y_coord)
            reached = true;
    }

    if (reached) {
        game_state.walking_style = 13;  /* WALKING_STYLE::STAIRS */
        game_state.leader_x_coord = stairs_new_x;
        game_state.leader_y_coord = stairs_new_y;
        game_state.leader_y_frac = 0;
        game_state.leader_x_frac = 0;
    } else {
        /* Not reached — try again next frame */
        schedule_overworld_task(handle_stairs_enter_callback, 1);
    }
}

/* HANDLE_STAIRS_LEAVE callback — port of C06FED.asm.
 * Checks if leader has reached the stair exit point.
 * If yes, clears stairs state and snaps coords.
 * If no, reschedules for 1 more frame. */
static void handle_stairs_leave_callback(void) {
    bool reached = false;

    if (ow.stairs_direction == 0 || ow.stairs_direction == 256) {
        /* Ascending (variant 0/1):
         * Assembly: leader_y_coord compared to STAIRS_NEW_Y (BCS = unsigned >=).
         * Reached when leader_y_coord < stairs_new_y (unsigned). */
        if ((uint16_t)game_state.leader_y_coord < (uint16_t)stairs_new_y)
            reached = true;
    } else {
        /* Descending (variant 2/3):
         * Assembly: leader_y_coord compared to STAIRS_NEW_Y (BLTEQ = signed <=).
         * Reached when leader_y_coord > stairs_new_y (signed). */
        if ((int16_t)game_state.leader_y_coord > (int16_t)stairs_new_y)
            reached = true;
    }

    if (reached) {
        ow.stairs_direction = 0xFFFF;
        game_state.walking_style = 0;
        ow.player_movement_flags = 0;
        escalator_stair_active = 0;
        game_state.leader_x_coord = stairs_new_x;
        game_state.leader_y_coord = stairs_new_y;
        game_state.leader_y_frac = 0;
        game_state.leader_x_frac = 0;
    } else {
        schedule_overworld_task(handle_stairs_leave_callback, 1);
    }
}

/* ---- PROCESS_DOOR_AT_TILE (C07526) ---- */

uint16_t process_door_at_tile(uint16_t x_tile, uint16_t y_tile) {
    uint8_t door_type = find_door_at_position(x_tile, y_tile);

    uint16_t result = 0;

    switch (door_type) {
    case DOOR_TYPE_0:
        check_door_event_flag(dr.door_found);
        result = 0;
        break;
    case DOOR_TYPE_1:
        set_walking_style_stairs(dr.door_found);
        result = 1;
        break;
    case DOOR_TYPE_2:
        try_activate_door(dr.door_found);
        result = 0;
        break;
    case DOOR_TYPE_3:
        handle_escalator_movement(dr.door_found, x_tile, y_tile);
        result = 0;
        break;
    case DOOR_TYPE_4:
        handle_stairs_movement(dr.door_found, x_tile, y_tile);
        result = 1;
        break;
    case DOOR_TYPE_5:
    case DOOR_TYPE_7:
        /* DOOR_HANDLER_NOP — no-op */
        result = 0;
        break;
    case DOOR_TYPE_6:
        /* DOOR_HANDLER_TYPE6 — no-op */
        result = 0;
        break;
    default:
        /* No door found (0xFF) */
        break;
    }

    return result;
}

/* ---- CHECK_DOOR_IN_DIRECTION (C065C2) ---- */

void check_door_in_direction(uint16_t direction) {
    /* Assembly lines 9-19: compute tile position with direction offset */
    uint16_t dir_idx = direction & 7;
    uint16_t tile_x = (game_state.leader_x_coord >> 3)
                     + door_dir_x_offsets[dir_idx];
    uint16_t tile_y = (game_state.leader_y_coord >> 3)
                     + door_dir_y_offsets[dir_idx];

    /* Assembly lines 28-31: special case for LEFT (direction 6) — decrement x */
    if (direction == 6) {
        tile_x--;
    }

    /* Assembly lines 33-38: first search at (tile_x, tile_y) */
    uint8_t result = find_door_at_position(tile_x, tile_y);

    /* Assembly lines 39-48: if not found, try x+1 */
    if (result == 0xFF) {
        result = find_door_at_position(tile_x + 1, tile_y);
    }

    /* Assembly lines 50-66: if found and type is 6 (DOOR_TYPE_6),
     * set up MAP_OBJECT_TEXT and ow.interacting_npc_id = -2 */
    if (result != 0xFF && result == DOOR_TYPE_6) {
        const uint8_t *door = get_door_data_entry(dr.door_found & 0x7FFF);
        if (door) {
            /* Read text pointer (offset 0, dword) */
            uint32_t text_ptr = read_u32_le(door);
            dr.door_found_type = result;
            ow.map_object_text = text_ptr;
            ow.interacting_npc_id = 0xFFFE;
        }
    }
}

/* ---- CHECK_DOOR_NEAR_LEADER (C4334A) ---- */

void check_door_near_leader(uint16_t direction) {
    /* Assembly lines 16-45: compute tile position */
    uint16_t dir_idx = direction & 7;
    uint16_t tile_x = (game_state.leader_x_coord >> 3)
                     + door_dir_x_offsets[dir_idx];

    uint16_t tile_y;
    if (direction == 4) {
        /* Assembly lines 28-36: direction DOWN uses (y+1) >> 3 */
        tile_y = ((game_state.leader_y_coord + 1) >> 3)
                + door_dir_y_offsets[dir_idx];
    } else {
        tile_y = (game_state.leader_y_coord >> 3)
                + door_dir_y_offsets[dir_idx];
    }

    /* Assembly lines 49-62: check entity collision, if 0x82 match nudge */
    uint16_t surface = check_entity_collision(
        tile_x * 8, tile_y * 8,
        game_state.current_party_members, direction);

    if ((surface & 0x0082) == 0x0082) {
        /* Nudge by direction offset */
        tile_x += door_dir_x_offsets[dir_idx];
        tile_y += door_dir_y_offsets[dir_idx];
    }

    /* Assembly lines 75-99: search for door, try x, x+1, x-1 */
    uint8_t result = find_door_at_position(tile_x, tile_y);

    if (result == 0xFF) {
        result = find_door_at_position(tile_x + 1, tile_y);
    }

    if (result == 0xFF) {
        result = find_door_at_position(tile_x - 1, tile_y);
    }

    /* Assembly lines 101-117: if found and type is 5 (DOOR_TYPE_5),
     * set up MAP_OBJECT_TEXT and ow.interacting_npc_id = -2 */
    if (result != 0xFF && result == DOOR_TYPE_5) {
        const uint8_t *door = get_door_data_entry(dr.door_found & 0x7FFF);
        if (door) {
            uint32_t text_ptr = read_u32_le(door);
            dr.door_found_type = result;
            ow.map_object_text = text_ptr;
            ow.interacting_npc_id = 0xFFFE;
        }
    }
}

/* ---- INIT_TRANSITION_SCROLL_VELOCITY (port of C42631) ----
 * Sets up background scroll velocity for the transition effect.
 * The assembly uses COSINE_SINE/COSINE with byte angles (0-255 = 0-360°).
 * We replicate the same fixed-point result using C math.
 *
 * Assembly parameter mapping:
 *   A = speed (from config.unknown5)
 *   X = direction * 4 (from config.direction << 2) */
static void init_transition_scroll_velocity(uint16_t speed,
                                             uint16_t direction_x4) {
    dr.transition_x_velocity = 0;
    dr.transition_y_velocity = 0;

    if (speed != 0) {
        /* Assembly: ADC #$0080 / AND #$00FF — add 128 (180°) to byte angle */
        uint8_t angle = (uint8_t)((direction_x4 + 0x80) & 0xFF);

        /* COSINE_SINE returns speed * sin(angle) in 8.8 fixed point.
         * Then stored at TRANSITION_X_VELOCITY_FRAC+1, effectively << 8 within the
         * 32-bit velocity field. So total shift is << 8 from 8.8 = << 8.
         * Net: velocity = speed * sin(angle) * 256 in 16.16 fixed point,
         * i.e. speed * sin(angle) pixels-per-frame when divided by 65536.
         * But actually the result is already in the right range for smooth scroll. */
        double rad = (double)angle * 2.0 * M_PI / 256.0;

        /* COSINE_SINE → X velocity component: speed * sin(angle).
         * COSINE → Y velocity component: speed * sin(angle-64) = -speed * cos(angle).
         * Assembly uses COSINE (not COSINE_SINE) for Y, so Y velocity is NEGATIVE cosine.
         * Assembly stores trig result in middle 16 bits of 32-bit field. */
        int16_t sin_val = (int16_t)round((double)speed * sin(rad));
        int16_t cos_val = (int16_t)round((double)speed * cos(rad));

        dr.transition_x_velocity = (int32_t)sin_val << 8;
        dr.transition_y_velocity = -(int32_t)cos_val << 8;
    }

    /* Initialize position from current BG scroll */
    dr.transition_x_accum = (int32_t)ppu.bg_hofs[0] << 16;
    dr.transition_y_accum = (int32_t)ppu.bg_vofs[0] << 16;
}

/* ---- UPDATE_TRANSITION_SCROLL (port of C4268A) ----
 * Applies scroll velocity to background positions each frame. */
static void update_transition_scroll(void) {
    dr.transition_x_accum += dr.transition_x_velocity;
    uint16_t new_x = (uint16_t)(dr.transition_x_accum >> 16);
    ppu.bg_hofs[0] = new_x;
    ppu.bg_hofs[1] = new_x;

    dr.transition_y_accum += dr.transition_y_velocity;
    uint16_t new_y = (uint16_t)(dr.transition_y_accum >> 16);
    ppu.bg_vofs[0] = new_y;
    ppu.bg_vofs[1] = new_y;

    /* Assembly calls SCROLL_MAP_TO_POSITION(BG1_X_POS, BG1_Y_POS) here
     * to update tilemap streaming. For now this just updates the scroll
     * registers; map_refresh_tilemaps handles tile streaming separately. */
    map_refresh_tilemaps(new_x + VIEWPORT_CENTER_X, new_y + VIEWPORT_CENTER_Y);
}

/* ---- SCREEN_TRANSITION (port of asm/overworld/screen_transition.asm) ----
 * Main screen transition effect used by door transitions.
 *
 * transition_type: index into SCREEN_TRANSITION_CONFIG_TABLE (0-33).
 * mode: 1 = exit (fade out), 0 = enter (fade in). */
void screen_transition(uint8_t transition_type, uint8_t mode) {
    if (transition_type >= SCREEN_TRANSITION_CONFIG_COUNT) return;
    const ScreenTransitionConfig *cfg = &screen_transition_config[transition_type];

    /* Read config fields */
    uint8_t duration                  = cfg->duration;
    uint8_t animation_id              = cfg->animation_id;
    uint8_t animation_flags           = cfg->animation_flags;
    uint8_t fade_style                = cfg->fade_style;
    uint8_t direction                 = cfg->direction;
    uint8_t unknown5                  = cfg->scroll_speed;
    uint8_t secondary_duration        = cfg->secondary_duration;
    uint8_t secondary_animation_id    = cfg->secondary_animation_id;
    uint8_t secondary_animation_flags = cfg->secondary_animation_flags;

    /* Assembly lines 28-34: resolve duration.
     * 0xFF = special case → 900 frames. */
    uint16_t eff_duration = (uint16_t)duration;
    if (duration == 0xFF) {
        eff_duration = 900;
    }

    /* Assembly lines 37-46: init transition scroll velocity.
     * direction * 4 gives byte angle, speed from unknown5. */
    uint16_t dir_x4 = (uint16_t)direction << 2;
    init_transition_scroll_velocity((uint16_t)unknown5, dir_x4);

    if (mode == 1) {
        /* ====== EXIT TRANSITION (fade out) ====== */

        /* Assembly line 50: disable all entities, wait 2 frames */
        disable_all_entities();
        wait_frames_with_updates(2);

        /* Assembly lines 53-70: init primary swirl animation if set */
        if (animation_id != 0) {
            uint16_t swirl_options = (uint16_t)animation_flags + 2;
            init_swirl_effect((uint16_t)animation_id, swirl_options);
        }

        /* Assembly lines 72-90: set up palette fade.
         * Load current ert.palettes faded by fade_style into ert.buffer,
         * then compute fade slopes over eff_duration frames. */
        load_palette_to_fade_buffer(fade_style);
        prepare_palette_fade_slopes((int16_t)eff_duration, 0xFFFF);

        /* Assembly lines 94-116: exit transition frame loop */
        for (uint16_t frame = 0; frame < eff_duration; frame++) {
            if (ert.palette_upload_mode != 0) {
                wait_for_vblank();
            }
            update_map_palette_animation();
            oam_clear();
            update_transition_scroll();
            update_entity_screen_positions();
            run_actionscript_frame();
            update_screen();
            update_swirl_effect();
            wait_for_vblank();
        }

        /* Assembly lines 117-141: finalize exit transition.
         * Assembly: LDA #50; CLC; SBC fade_style → 49 - fade_style
         * BRANCHLTEQS @FADE_TO_WHITE when 49 - fade_style <= 0 → fade_style >= 49
         * If fade_style >= 49, fade palette to white.
         * If fade_style < 49, just force blank. */
        if (fade_style >= 49) {
            /* Fade to white: fill ert.palettes with 0x7FFF, upload */
            for (int i = 0; i < 256; i++) {
                ert.palettes[i] = 0x7FFF;
            }
            ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
            wait_for_vblank();
            dr.wipe_palettes_on_map_load = 1;
        } else {
            /* Force blank: set INIDISP to force blank, disable HDMA */
            set_force_blank(true);
            ppu.window_hdma_active = false;
            wait_for_vblank();
        }

        /* Assembly lines 143: enable entities */
        enable_all_entities();
    } else {
        /* ====== ENTER TRANSITION (fade in) ====== */

        /* Assembly lines 146-158: determine enter mode.
         * Assembly: LDA #50; CLC; SBC fade_style → 49 - fade_style
         * BRANCHLTEQS @ENTER_FADE_MODE_SET when fade_style >= 49 → keep X=0 (palette)
         * LDX #1 when fade_style < 49 → brightness mode
         * fade_style >= 49 → use palette fade (mode 0)
         * fade_style < 49 → use brightness fade_in (mode 1) */
        uint8_t enter_mode = 0;
        if (fade_style < 49) {
            enter_mode = 1;
        }

        if (enter_mode == 1) {
            /* Assembly line 164: brightness-based fade in */
            fade_in(1, 1);
        } else {
            /* Assembly lines 166-173: palette-based fade.
             * Buffer[0..511] already contains the new palette target, set up
             * by load_map_at_sector's WIPE_PALETTES_ON_MAP_LOAD handling
             * (copy_fade_buffer_to_palettes copies ert.palettes[] → ert.buffer[]).
             * Compute slopes from current ert.palettes[] (white) toward ert.buffer[]. */
            prepare_palette_fade_slopes((int16_t)secondary_duration, 0xFFFF);
        }

        /* Assembly lines 174-190: init secondary swirl animation if set */
        if (secondary_animation_id != 0) {
            init_swirl_effect((uint16_t)secondary_animation_id,
                              (uint16_t)secondary_animation_flags);
        }

        /* Assembly lines 192-227: enter transition frame loop */
        for (uint16_t frame = 0; frame < (uint16_t)secondary_duration; frame++) {
            if (enter_mode == 0) {
                if (ert.palette_upload_mode != 0) {
                    wait_for_vblank();
                }
                update_map_palette_animation();
            }
            oam_clear();
            run_actionscript_frame();
            update_swirl_effect();
            update_screen();
            wait_for_vblank();

            /* Assembly lines 211-213: disable entities on frame 1 */
            if (frame == 1) {
                disable_all_entities();
            }
        }

        /* Assembly lines 228-230: finalize palette fade if in palette mode */
        if (enter_mode == 0) {
            finalize_palette_fade();
        }
    }

    /* ====== TRANSITION CLEANUP (shared) ====== */

    /* Assembly lines 232-236: stop oval window (skip during Giygas prayer phase).
     * BCS = unsigned >=, so skip when bt.giygas_phase >= START_PRAYING (4). */
    if (bt.giygas_phase < GIYGAS_START_PRAYING)
        stop_oval_window();

    /* Assembly line 237: re-enable entities */
    enable_all_entities();

    /* Assembly lines 238-239: clear ladder/stairs state */
    ow.ladder_stairs_tile_y = 0;
    ow.ladder_stairs_tile_x = 0;
}

/* ---- GET_SCREEN_TRANSITION_SOUND_EFFECT ---- */

uint16_t get_screen_transition_sound_effect(uint16_t transition_id,
                                             uint16_t get_start) {
    if (transition_id >= SCREEN_TRANSITION_CONFIG_COUNT) return 0;
    const ScreenTransitionConfig *cfg = &screen_transition_config[transition_id];
    return get_start ? cfg->start_sound_effect : cfg->ending_sound_effect;
}

/* ---- PROCESS_DOOR_INTERACTIONS (C06B3D) ---- */

void process_door_interactions(void) {
    /* Phase 1: drain current interaction queue, saving type-10 (door text)
     * entries into dr.door_interactions[]. */
    uint16_t saved_count = 0;
    memset(dr.door_interactions, 0, sizeof(dr.door_interactions));

    uint16_t iterations = 0;
    while (iterations < 4 &&
           ow.current_queued_interaction != ow.next_queued_interaction) {
        uint16_t type = ow.queued_interactions[ow.current_queued_interaction].type;
        if (type == 10) {
            uint32_t text_ptr =
                ow.queued_interactions[ow.current_queued_interaction].data_ptr;
            if (saved_count < MAX_DOOR_INTERACTIONS) {
                dr.door_interactions[saved_count] = text_ptr;
                saved_count++;
            }
        }
        ow.current_queued_interaction =
            (ow.current_queued_interaction + 1) & (INTERACTION_QUEUE_SIZE - 1);
        iterations++;
    }

    /* Null-terminate the list */
    if (saved_count < MAX_DOOR_INTERACTIONS) {
        dr.door_interactions[saved_count] = 0;
    }

    /* Phase 2: re-queue saved interactions as type 10 */
    for (uint16_t i = 0; i < MAX_DOOR_INTERACTIONS; i++) {
        if (dr.door_interactions[i] == 0) break;
        queue_interaction(10, dr.door_interactions[i]);
    }
}

/* ---- DOOR_TRANSITION (door_transition.asm) ---- */

void door_transition(uint32_t door_data_snes_ptr) {
    /* Convert SNES ROM pointer to offset into our door_data ert.buffer.
     * door_data_snes_ptr is a full 24-bit SNES address ($CFxxxx).
     * Offset = low 16 bits (since DOOR_DATA is at bank start). */
    uint16_t door_offset = (uint16_t)(door_data_snes_ptr & 0xFFFF);
    const uint8_t *door = get_door_data_entry(door_offset);
    if (!door) {
        dr.using_door = 0;
        return;
    }

    /* Assembly lines 11-19: check and display door text if non-NULL */
    uint32_t text_ptr = read_u32_le(door);
    if (text_ptr != 0) {
        display_text_and_wait_for_fade(text_ptr);
    }

    /* Assembly lines 21-22: clear ladder/stairs tile coords */
    ow.ladder_stairs_tile_y = 0;
    ow.ladder_stairs_tile_x = 0;

    /* Assembly lines 23-45: check event flag */
    uint16_t event_flag_raw = read_u16_le(door + 4);  /* door_data::event_flag */
    if (event_flag_raw != 0) {
        uint16_t flag_id = event_flag_raw & 0x7FFF;
        bool flag_value = event_flag_get(flag_id);

        bool expected = (event_flag_raw > 0x8000);

        if (flag_value != expected) {
            dr.using_door = 0;
            return;
        }
    }

    /* Assembly lines 47-59: clear event flags 1-10 */
    for (uint16_t flag = 1; flag <= 10; flag++) {
        event_flag_clear(flag);
    }

    /* Assembly lines 60-64: process door interactions and clear state */
    process_door_interactions();
    clear_party_sprite_hide_flags();
    ow.entity_fade_entity = -1;
    ow.player_intangibility_frames = 0;

    /* Assembly lines 65-74: play transition-out sound effect */
    uint8_t transition_type = door[10];  /* door_data::unknown10 */
    uint16_t sfx_out = get_screen_transition_sound_effect(transition_type, 1);
    play_sfx(sfx_out);

    /* Assembly lines 75-86: transition out */
    if (ow.disabled_transitions) {
        fade_out(1, 1);
    } else {
        screen_transition(transition_type, 1);
    }

    /* Assembly lines 87-100: compute destination coordinates.
     * unknown6 low 14 bits = dest y tile, unknown8 = dest x tile.
     * Both shifted left by 3 to get pixel coordinates.
     * Assembly: @VIRTUAL02 = door[8]<<3 (A=x), @VIRTUAL04 = (door[6]&0x3FFF)<<3 (X=y). */
    uint16_t unknown6 = read_u16_le(door + 6);
    uint16_t unknown8 = read_u16_le(door + 8);

    uint16_t dest_x = unknown8 << 3;
    uint16_t dest_y = (unknown6 & 0x3FFF) << 3;

    /* Assembly lines 101-117: extract bits 14-15 of unknown6 for direction class.
     * Assembly: SEP; LDA #14; TAY; REP; LDA unknown6; JSL ASR8_POSITIVE; ASL; TAX
     * ASR8_POSITIVE with Y=14 does 14 logical right shifts → bits 14-15 (0-3).
     * ASL then multiplies by 2 → byte offset 0,2,4,6 into DOOR_ENTRY_DIRECTION_TABLE.
     * direction 2 (RIGHT) → skip adjustment, otherwise add 8 to X. */
    uint16_t dir_class_idx = (unknown6 >> 14) & 3;
    uint16_t dir_class = door_dest_direction_table[dir_class_idx];
    if (dir_class != 2) {
        dest_x += 8;
    }

    /* Assembly lines 119-140: resolve music for destination sector.
     * Debug mode 6 skips music resolve — not relevant for retail. */
    resolve_map_sector_music(dest_x, dest_y);

    /* Assembly lines 141-146: load destination map */
    load_map_at_position(dest_x, dest_y);
    ow.player_has_moved_since_map_load = 0;
    game_state.walking_style = 0;

    /* Assembly lines 147-163: determine direction for party placement */
    uint16_t party_direction = door_dest_direction_table[dir_class_idx];

    /* Assembly lines 161-163: set leader position and load party */
    set_leader_position_and_load_party(dest_x, dest_y, party_direction);

    /* Assembly lines 164-169: debug replay save (skipped for now) */

    /* Assembly lines 170-171: apply music and flush entity queue */
    apply_next_map_music();
    flush_entity_creation_queue();

    /* Assembly lines 172-181: play transition-in sound effect.
     * SFX 0 means "no ending SFX" — skip play_sfx(0) to avoid stopping the
     * start SFX (e.g. SFX::STAIRS) prematurely. On SNES, load_map_at_position
     * takes many frames of real time so the start SFX finishes naturally before
     * play_sfx(0) executes; on the C port the map loads instantly. */
    sfx_out = get_screen_transition_sound_effect(transition_type, 0);
    if (sfx_out != 0) {
        play_sfx(sfx_out);
    }

    /* Assembly lines 182-193: transition in */
    if (ow.disabled_transitions) {
        fade_in(1, 1);
    } else {
        screen_transition(transition_type, 0);
    }

    /* Assembly lines 194-198: finalize */
    ow.stairs_direction = 0xFFFF;
    ow.player_has_done_something_this_frame = 0;
    spawn_buzz_buzz();
    dr.using_door = 0;
}
