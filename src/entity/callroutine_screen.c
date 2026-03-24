/*
 * Callroutine screen/display/decompress functions.
 * Extracted from callroutine.c.
 */
#include "entity/callroutine_internal.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "game/flyover.h"
#include "game/overworld.h"
#include "game/ending.h"
#include "game/position_buffer.h"
#include "core/decomp.h"
#include "core/math.h"
#include "data/assets.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/constants.h"
#include "core/memory.h"
#include "core/log.h"
#include "game/audio.h"
#include "game/battle.h"
#include "game_main.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- Static cr_* functions moved from callroutine.c ---- */

int16_t cr_decompress_title_data(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    int16_t param = scripts.tempvar[scr];

    AssetId id = (param == 0) ?
        ASSET_E1AE83_BIN_LZHAL : ASSET_E1AEFD_BIN_LZHAL;

    size_t comp_size = ASSET_SIZE(id);
    const uint8_t *comp_data = ASSET_DATA(id);
    if (comp_data) {
        decomp(comp_data, comp_size, ert.buffer, BUFFER_SIZE);
    }
    return 0;
}

int16_t cr_load_title_palette(int16_t ent, int16_t scr,
                              uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    ert.palette_upload_mode = 0;  /* Assembly line 9: STZ PALETTE_UPLOAD_MODE */

    size_t comp_size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_PAL_LZHAL);
    const uint8_t *comp_data = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_PAL_LZHAL);
    if (comp_data) {
        /* Decompress directly into ert.palettes, matching the assembly which
         * writes to PALETTES in-place.  The decompressed palette is only 256
         * bytes (128 colors, indices 0-127).  Colors 128-255 must NOT be
         * zeroed — they retain the sprite palette from the initial load. */
        decomp(comp_data, comp_size, (uint8_t *)ert.palettes,
               sizeof(ert.palettes));
    }

    copy_fade_buffer_to_palettes();
    memset(ert.palettes, 0, 256);
    prepare_palette_fade_slopes(165, 0xFFFF);
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    return 0;
}

int16_t cr_show_entity_sprite(int16_t ent, int16_t scr,
                              uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    int16_t slot = ert.current_entity_slot;
    int16_t offset = ENT(slot);
    entities.spritemap_ptr_hi[offset] &= 0x7FFF;
    return 0;
}

/* ENTITY_HDMA_SCANLINE_WIDTH_TABLE (C474F6) — half-width of oval window
 * at each of the 11 scanline rows around an entity sprite. */
static const uint8_t entity_hdma_scanline_width[11] = {
    0x10, 0x10, 0x0F, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x09, 0x06, 0x03
};

/* render_entity_hdma_window — Port of RENDER_ENTITY_HDMA_WINDOW.
 * Builds per-scanline window tables creating a vertical beam above the entity
 * that narrows into an oval at the entity position.
 *
 * Assembly builds an HDMA table with:
 *   - Header: (screen_y+5) scanlines with beam rect (ent_x±16)
 *   - Oval: up to 11 scanlines narrowing per the width table
 *   - Tail: remaining scanlines with no window (128/127)
 */
void render_entity_hdma_window(int16_t entity_offset,
                               uint8_t *wh_left_table,
                               uint8_t *wh_right_table) {
    int16_t ent_y = entities.abs_y[entity_offset];
    int16_t ent_x = entities.abs_x[entity_offset];
    int16_t screen_y = (int16_t)((uint16_t)ent_y - ppu.bg_vofs[0]) + 4;
    int16_t screen_x = (int16_t)((uint16_t)ent_x - ppu.bg_hofs[0]);

    /* Fill entire table with "no window" (left > right) */
    for (int s = 0; s < VIEWPORT_HEIGHT; s++) {
        wh_left_table[s] = 128;
        wh_right_table[s] = 127;
    }

    /* Compute header beam rect (ent_x ± 16, screen-relative) */
    int16_t hdr_left  = (int16_t)((uint16_t)ent_x - 16 - ppu.bg_hofs[0]);
    int16_t hdr_right = (int16_t)((uint16_t)ent_x + 16 - ppu.bg_hofs[0]);

    /* Convert viewport-space X to SNES coordinate space.
     * WH table values are compared against SNES-space wx in pixel_is_window_masked. */
    screen_x -= VIEWPORT_PAD_LEFT;
    hdr_left -= VIEWPORT_PAD_LEFT;
    hdr_right -= VIEWPORT_PAD_LEFT;
    uint8_t beam_l, beam_r;

    if ((uint16_t)hdr_left < 256) {
        beam_l = (uint8_t)hdr_left;
        beam_r = (uint16_t)hdr_right >= 256 ? 0xFF : (uint8_t)hdr_right;
    } else if ((uint16_t)hdr_right < 256) {
        beam_l = 0;
        beam_r = (uint8_t)hdr_right;
    } else {
        beam_l = 128;
        beam_r = 127;
    }

    /* Header: scanlines 0..(screen_y+4) get the beam rectangle.
     * Assembly writes count = (screen_y+4+1) = screen_y+5 scanlines. */
    if ((uint16_t)screen_y < 0x8000) {
        int hdr_end = screen_y + 4;
        if (hdr_end >= VIEWPORT_HEIGHT) hdr_end = VIEWPORT_HEIGHT - 1;
        for (int s = 0; s <= hdr_end; s++) {
            wh_left_table[s]  = beam_l;
            wh_right_table[s] = beam_r;
        }
    }

    /* Oval scanlines: up to 11 rows starting at scanline (screen_y+5).
     * Assembly: visible_count = min(screen_y + 4 + 11, 10).
     * table_start = 10 - visible_count (skip top rows if entity near top). */
    int16_t check = screen_y + 4 + 11;
    if ((uint16_t)check >= 0x8000)
        return;

    int visible_count = check >= 10 ? 10 : (int)check;
    int table_start = 10 - visible_count;
    int oval_start = screen_y + 5;

    for (int i = 0; i <= visible_count; i++) {
        int s = oval_start + i;
        if (s < 0 || s >= VIEWPORT_HEIGHT)
            continue;

        int half_w = entity_hdma_scanline_width[table_start + i];
        int16_t left  = screen_x - half_w;
        int16_t right = screen_x + half_w - 1;

        if ((uint16_t)left < 256) {
            wh_left_table[s] = (uint8_t)left;
            wh_right_table[s] = (uint16_t)right >= 256 ? 0xFF : (uint8_t)right;
        } else if ((uint16_t)right < 256) {
            wh_left_table[s] = 0;
            wh_right_table[s] = (uint8_t)right;
        }
        /* else both off screen — keep 128/127 */
    }
}

/*
 * Tick callback ROM addresses — used by dispatch_tick_callback.
 */
#define TICK_ADDR_UPDATE_OVERWORLD_FRAME            0xC05200
#define TICK_ADDR_UPDATE_LEADER_MOVEMENT            0xC04236
#define TICK_ADDR_UPDATE_ENTITY_ANIMATION          0xC0A6E3
#define TICK_ADDR_UPDATE_FOLLOWER_STATE            0xC04D78
#define TICK_ADDR_SIMPLE_SCREEN_POS_CALLBACK       0xC48BE1
#define TICK_ADDR_SIMPLE_SCREEN_POS_CALLBACK_OFS   0xC48C02
#define TICK_ADDR_ENTITY_PATHFINDING_STEP          0xC0D7F7
#define TICK_ADDR_RESET_ENTITY_PATHFINDING         0xC0D7E0
#define TICK_ADDR_MAKE_PARTY_LOOK_AT_ENTITY        0xC48B3B
#define TICK_ADDR_ANIMATED_BACKGROUND_CALLBACK     0xC48BDA
#define TICK_ADDR_CENTRE_SCREEN_ON_ENTITY          0xC48C2B
#define TICK_ADDR_CENTRE_SCREEN_ON_ENTITY_OFS      0xC48C3E
#define TICK_ADDR_NOP                              0xC0E979
#define TICK_ADDR_UPDATE_ENTITY_SURFACE_AND_GRAPHICS 0xC0E97C
#define TICK_ADDR_PSI_TELEPORT_ALPHA_TICK          0xC0E28F
#define TICK_ADDR_UPDATE_PARTY_ENTITY_FROM_BUFFER  0xC0E3C1
#define TICK_ADDR_PSI_TELEPORT_BETA_TICK           0xC0E516
#define TICK_ADDR_PSI_TELEPORT_SUCCESS_TICK        0xC0E674
#define TICK_ADDR_PSI_TELEPORT_DECELERATE_TICK     0xC0E776
#define TICK_ADDR_HANDLE_CAST_SCROLLING           0xC4E51E
#define TICK_ADDR_SETUP_ENTITY_HDMA_WINDOW_CH4   0xC476A5
#define TICK_ADDR_SETUP_ENTITY_HDMA_WINDOW_CH5   0xC47705
#define TICK_ADDR_APPLY_ENTITY_RECT_WINDOW       0xC479E9
#define TICK_ADDR_CENTER_CAMERA_WITH_RECT_WINDOW 0xC47A27
#define TICK_ADDR_UPDATE_PARTY_FOLLOWER_MOVEMENT 0xEF031E

/*
 * dispatch_tick_callback — called from script.c after running all scripts
 * for an entity. Dispatches the entity's tick callback by ROM address.
 *
 * Port of the JUMP_TO_LOADED_MOVEMENT_PTR call in RUN_ENTITY_SCRIPTS_AND_TICK.
 */
void dispatch_tick_callback(uint32_t rom_addr, int16_t entity_offset) {
    uint16_t dummy_pc;
    switch (rom_addr) {
    case TICK_ADDR_UPDATE_OVERWORLD_FRAME:
        /* C05200: UPDATE_OVERWORLD_FRAME — init entity (slot 23) tick callback.
         * Set by EVENT_001 (main overworld tick). Handles animated tiles/ert.palettes, then calls
         * UPDATE_LEADER_MOVEMENT internally. */
        update_overworld_frame(entity_offset);
        break;
    case TICK_ADDR_UPDATE_LEADER_MOVEMENT:
        /* C04236: UPDATE_LEADER_MOVEMENT — leader entity tick callback.
         * Syncs position_index, checks tile collision, writes ert.buffer.
         * NOTE: In normal gameplay this is NOT set as a tick callback directly.
         * UPDATE_OVERWORLD_FRAME calls it. This case exists for completeness. */
        update_leader_movement(entity_offset);
        break;
    case TICK_ADDR_UPDATE_ENTITY_ANIMATION:
        cr_update_entity_animation(entity_offset, -1, 0, &dummy_pc);
        break;
    case TICK_ADDR_UPDATE_FOLLOWER_STATE:
        /* C04D78: UPDATE_FOLLOWER_STATE — reads position from
         * PLAYER_POSITION_BUFFER and updates follower entity. */
        update_follower_state(entity_offset);
        break;
    case TICK_ADDR_SIMPLE_SCREEN_POS_CALLBACK:
        /* C48BE1: ACTIONSCRIPT_SIMPLE_SCREEN_POSITION_CALLBACK
         * screen_x = abs_x - BG1_X_POS, screen_y = abs_y - BG1_Y_POS.
         * Used by attract mode entities (set in INIT_ENTITY_FOR_ATTRACT_MODE). */
        entities.screen_x[entity_offset] = entities.abs_x[entity_offset]
                                          - (int16_t)ppu.bg_hofs[0];
        entities.screen_y[entity_offset] = entities.abs_y[entity_offset]
                                          - (int16_t)ppu.bg_vofs[0];
        break;
    case TICK_ADDR_SIMPLE_SCREEN_POS_CALLBACK_OFS:
        /* C48C02: ACTIONSCRIPT_SIMPLE_SCREEN_POSITION_CALLBACK_OFFSET
         * Same as above but adds var0/var1 offsets. */
        entities.screen_x[entity_offset] = entities.abs_x[entity_offset]
                                          - (int16_t)ppu.bg_hofs[0]
                                          + entities.var[0][entity_offset];
        entities.screen_y[entity_offset] = entities.abs_y[entity_offset]
                                          - (int16_t)ppu.bg_vofs[0]
                                          + entities.var[1][entity_offset];
        break;
    case TICK_ADDR_RESET_ENTITY_PATHFINDING:
        /* C0D7E0: RESET_ENTITY_PATHFINDING — if pathfinding state is non-zero,
         * set it to 1 (request reset). Used as tick callback by event 026. */
        if (entities.pathfinding_states[entity_offset] != 0)
            entities.pathfinding_states[entity_offset] = 1;
        break;
    case TICK_ADDR_ENTITY_PATHFINDING_STEP: {
        /* C0D7F7: ENTITY_PATHFINDING_STEP — execute one frame of pathfinding
         * movement toward the next waypoint in the entity's path.
         *
         * Checks if entity is within 3 pixels of current target. If so,
         * advances to next waypoint. Sets direction and velocity toward target.
         * When all waypoints consumed, clears pathfinding state and sets
         * obstacle_flags bit 7. */
        int16_t ent = entity_offset;

        /* Only proceed if pathfinding state == -1 (active) */
        if (entities.pathfinding_states[ent] != -1)
            break;

        int16_t size = entities.sizes[ent];
        uint16_t path_ptr = ert.entity_path_points[ent];
        int16_t from_x = entities.abs_x[ent];
        int16_t from_y = entities.abs_y[ent];

        /* Compute target from current path point */
        int16_t path_y = (int16_t)read_u16_le(&ert.delivery_paths[path_ptr]);
        int16_t path_x = (int16_t)read_u16_le(&ert.delivery_paths[path_ptr + 2]);
        int16_t target_x = (int16_t)((ert.pathfinding_target_centre_x - ert.pathfinding_target_width) * 8
                         + path_x * 8
                         + entity_collision_x_offset[size]);
        int16_t target_y = (int16_t)((ert.pathfinding_target_centre_y - ert.pathfinding_target_height) * 8
                         + path_y * 8
                         - sprite_hitbox_enable[size]
                         + entity_collision_y_offset[size]);

        /* Check if entity has reached the current waypoint (within 3 pixels) */
        int16_t diff_x = from_x - target_x;
        int16_t diff_y = from_y - target_y;
        if (abs(diff_x) < 3 && abs(diff_y) < 3) {
            /* Reached waypoint — advance to next */
            ert.entity_path_point_counts[ent]--;
            if (ert.entity_path_point_counts[ent] > 0) {
                path_ptr += 4;
                ert.entity_path_points[ent] = path_ptr;

                /* Recompute target from next waypoint */
                path_y = (int16_t)read_u16_le(&ert.delivery_paths[path_ptr]);
                path_x = (int16_t)read_u16_le(&ert.delivery_paths[path_ptr + 2]);
                target_x = (int16_t)((ert.pathfinding_target_centre_x - ert.pathfinding_target_width) * 8
                         + path_x * 8
                         + entity_collision_x_offset[size]);
                target_y = (int16_t)((ert.pathfinding_target_centre_y - ert.pathfinding_target_height) * 8
                         + path_y * 8
                         - sprite_hitbox_enable[size]
                         + entity_collision_y_offset[size]);
            }
        }

        if (ert.entity_path_point_counts[ent] > 0) {
            /* Set direction and velocity toward target */
            uint16_t fine_dir = calculate_direction_fine(from_x, from_y, target_x, target_y);
            set_velocity_from_direction(ent, (int16_t)fine_dir);
            int16_t dir = quantize_direction((int16_t)fine_dir);
            entities.moving_directions[ent] = dir;
            entities.directions[ent] = dir;
        } else {
            /* Pathfinding complete — clear state, set obstacle bit 7 */
            entities.pathfinding_states[ent] = 0;
            entities.obstacle_flags[ent] |= 0x0080;
        }
        break;
    }
    case TICK_ADDR_MAKE_PARTY_LOOK_AT_ENTITY: {
        /* C48B3B: MAKE_PARTY_LOOK_AT_ACTIVE_ENTITY — make all party members
         * face toward the current entity. Assembly only runs on even frames
         * (FRAME_COUNTER & 1 == 0). Iterates party members, calculates
         * direction, and updates sprite if direction changed. */
        if (core.frame_counter & 1)
            break;
        int16_t ent_off = entity_offset;
        for (int i = 0; i < (game_state.party_count & 0xFF); i++) {
            /* Check party_order[i] is a valid member (< 16) */
            uint8_t order = game_state.party_order[i];
            if (order >= 16)
                continue;
            int16_t pm_off = ENT((int16_t)read_u16_le(&game_state.party_entity_slots[i * 2]));
            int16_t dir = calculate_direction_8(
                entities.abs_x[pm_off], entities.abs_y[pm_off],
                entities.abs_x[ent_off], entities.abs_y[ent_off]);
            if (entities.directions[pm_off] != dir) {
                entities.directions[pm_off] = dir;
                render_entity_sprite(pm_off);
            }
        }
        break;
    }
    case TICK_ADDR_ANIMATED_BACKGROUND_CALLBACK:
        /* C48BDA: ACTIONSCRIPT_ANIMATED_BACKGROUND_CALLBACK —
         * Just calls UPDATE_BATTLE_SCREEN_EFFECTS. */
        update_battle_screen_effects();
        break;
    case TICK_ADDR_CENTRE_SCREEN_ON_ENTITY:
        /* C48C2B: ACTIONSCRIPT_CENTRE_SCREEN_ON_ENTITY_CALLBACK —
         * Centers camera on entity position via CENTER_SCREEN. */
        center_screen(
            (uint16_t)entities.abs_x[entity_offset],
            (uint16_t)entities.abs_y[entity_offset]);
        break;
    case TICK_ADDR_CENTRE_SCREEN_ON_ENTITY_OFS:
        /* C48C3E: ACTIONSCRIPT_CENTRE_SCREEN_ON_ENTITY_CALLBACK_OFFSET —
         * Same as above but adds var0 (X offset) and var1 (Y offset). */
        center_screen(
            (uint16_t)(entities.abs_x[entity_offset] + entities.var[0][entity_offset]),
            (uint16_t)(entities.abs_y[entity_offset] + entities.var[1][entity_offset]));
        break;
    case TICK_ADDR_PSI_TELEPORT_ALPHA_TICK:
        psi_teleport_alpha_tick(entity_offset);
        break;
    case TICK_ADDR_UPDATE_PARTY_ENTITY_FROM_BUFFER:
        update_party_entity_from_buffer(entity_offset);
        break;
    case TICK_ADDR_PSI_TELEPORT_BETA_TICK:
        psi_teleport_beta_tick(entity_offset);
        break;
    case TICK_ADDR_PSI_TELEPORT_SUCCESS_TICK:
        psi_teleport_success_tick(entity_offset);
        break;
    case TICK_ADDR_PSI_TELEPORT_DECELERATE_TICK:
        psi_teleport_decelerate_tick(entity_offset);
        break;
    case TICK_ADDR_NOP:
        /* C0E979: TICK_CALLBACK_NOP — no-op tick callback.
         * Used as placeholder when no per-frame handler is needed. */
        break;
    case TICK_ADDR_HANDLE_CAST_SCROLLING:
        /* C4E51E: HANDLE_CAST_SCROLLING — cast scene tick callback.
         * Sets BG3_Y to entity Y, clears tilemap rows as they scroll. */
        handle_cast_scrolling(entity_offset);
        break;
    case TICK_ADDR_UPDATE_ENTITY_SURFACE_AND_GRAPHICS: {
        /* C0E97C: UPDATE_ENTITY_SURFACE_AND_GRAPHICS —
         * Updates entity surface flags from map data, then refreshes
         * party member graphics. Used during teleport failure.
         * Port of asm/overworld/entity/update_entity_surface_and_graphics.asm. */
        uint16_t slot = entity_offset;
        int16_t x = (int16_t)entities.abs_x[entity_offset];
        int16_t y = (int16_t)entities.abs_y[entity_offset];
        entities.surface_flags[entity_offset] =
            lookup_surface_flags(x, y, entities.sizes[entity_offset]);
        int16_t party_idx = (int16_t)(slot - PARTY_LEADER_ENTITY_INDEX);
        update_party_entity_graphics(
            entities.var[0][entity_offset],  /* char_id from var0 */
            (uint16_t)-1,                    /* walking_style = -1 (force update) */
            entity_offset,
            party_idx);
        break;
    }
    case TICK_ADDR_APPLY_ENTITY_RECT_WINDOW: {
        /* C479E9: APPLY_ENTITY_RECT_WINDOW — per-frame tick callback.
         * Creates a rectangular window around the entity using var0 as half-size.
         * Calls SETUP_RECT_WINDOW_HDMA → ENABLE_OBJ_HDMA which sets WOBJSEL=0xA0
         * and drives WH0/WH1 via HDMA channel 4.
         *
         * Assembly reads an uninitialized stack variable for "top" which is 0
         * on real SNES (zeroed memory), making the window span from scanline 0
         * down to (screen_y + var0) — creating a beam from the sky. */
        int16_t screen_x = (int16_t)((uint16_t)entities.abs_x[entity_offset]
                           - ppu.bg_hofs[0]);
        int16_t screen_y = (int16_t)((uint16_t)entities.abs_y[entity_offset]
                           - ppu.bg_vofs[0]);
        int16_t var0 = entities.var[0][entity_offset];

        /* Convert viewport-space X to SNES coordinate space for WH values.
         * Y stays in viewport space since top/bottom are table indices. */
        screen_x -= VIEWPORT_PAD_LEFT;

        int16_t top    = 0;  /* assembly reads uninitialized stack → 0 on SNES */
        int16_t bottom = clamp_to_range(screen_y + var0, VIEWPORT_HEIGHT);
        int16_t left   = clamp_to_range(screen_x - var0, 256);
        int16_t right  = clamp_to_range(screen_x + var0, 256);

        /* Fill per-scanline window tables */
        for (int s = 0; s < VIEWPORT_HEIGHT; s++) {
            if (s >= top && s < bottom) {
                ppu.wh0_table[s] = (uint8_t)left;
                ppu.wh1_table[s] = (uint8_t)right;
            } else {
                ppu.wh0_table[s] = 128;
                ppu.wh1_table[s] = 127;
            }
        }
        ppu.window_hdma_active = true;
        /* ENABLE_OBJ_HDMA sets WOBJSEL to enable both color windows */
        ppu.wobjsel = 0xA0;
        break;
    }
    case TICK_ADDR_SETUP_ENTITY_HDMA_WINDOW_CH4:
        /* C476A5: SETUP_ENTITY_HDMA_WINDOW_CH4 — per-frame tick callback.
         * Builds oval window around entity using WH0/WH1 (HDMA channel 4). */
        render_entity_hdma_window(entity_offset, ppu.wh0_table, ppu.wh1_table);
        ppu.window_hdma_active = true;
        break;
    case TICK_ADDR_SETUP_ENTITY_HDMA_WINDOW_CH5:
        /* C47705: SETUP_ENTITY_HDMA_WINDOW_CH5 — per-frame tick callback.
         * Builds oval window around entity using WH2/WH3 (HDMA channel 5). */
        render_entity_hdma_window(entity_offset, ppu.wh2_table, ppu.wh3_table);
        ppu.window2_hdma_active = true;
        break;
    case TICK_ADDR_CENTER_CAMERA_WITH_RECT_WINDOW: {
        /* C47A27: CENTER_CAMERA_WITH_RECT_WINDOW — per-frame tick callback.
         * Centers camera Y on the current entity, then creates a rectangular
         * HDMA window around the party leader's Y position.
         * Port of asm/text/window/center_camera_with_rect_window.asm.
         * Used in event script 583. */
        uint16_t leader_slot = game_state.current_party_members;
        int16_t ent_y = (int16_t)entities.abs_y[entity_offset];

        /* Center camera on this entity (assembly: STA BG1_Y_POS) */
        int16_t camera_y = ent_y - VIEWPORT_CENTER_Y;
        ppu.bg_vofs[0] = (uint16_t)camera_y;

        /* Compute party leader's screen-relative Y (viewport space) */
        int16_t leader_y = (int16_t)entities.abs_y[ENT(leader_slot)];
        int16_t rel_y = leader_y - camera_y;

        /* SETUP_RECT_WINDOW_HDMA params:
         *   top    = rel_y - 96
         *   bottom = rel_y + 96
         *   left   = 16
         *   right  = 240 */
        int16_t top    = clamp_to_range(rel_y - 96, VIEWPORT_HEIGHT);
        int16_t bottom = clamp_to_range(rel_y + 96, VIEWPORT_HEIGHT);
        int16_t left   = clamp_to_range(16, 256);
        int16_t right  = clamp_to_range(240, 256);

        for (int s = 0; s < VIEWPORT_HEIGHT; s++) {
            if (s >= top && s < bottom) {
                ppu.wh0_table[s] = (uint8_t)left;
                ppu.wh1_table[s] = (uint8_t)right;
            } else {
                ppu.wh0_table[s] = 128;
                ppu.wh1_table[s] = 127;
            }
        }
        ppu.window_hdma_active = true;
        ppu.wobjsel = 0xA0;
        break;
    }
    case TICK_ADDR_UPDATE_PARTY_FOLLOWER_MOVEMENT: {
        /* EF031E: UPDATE_PARTY_FOLLOWER_MOVEMENT — per-frame tick callback.
         * Alternative follower movement for non-standard party members
         * (e.g., Bubble Monkey). Reads position from the position buffer
         * using char_struct::position_index, handles bubble monkey behavioral
         * modes (normal, hide, distracted).
         * Port of asm/misc/update_party_follower_movement.asm. */
        int16_t slot = entity_offset;
        int16_t char_id = entities.var[0][entity_offset];
        int16_t var1 = entities.var[1][entity_offset];

        /* var1 indexes into party_characters[] */
        if (var1 < 0 || var1 >= TOTAL_PARTY_COUNT) break;
        CharStruct *cs = &party_characters[var1];

        uint16_t pos_idx = cs->position_index;
        PositionBufferEntry *entry = &pb.player_position_buffer[pos_idx & 0xFF];

        /* Update entity position from buffer */
        entities.abs_x[entity_offset] = entry->x_coord;
        entities.abs_y[entity_offset] = entry->y_coord;

        if (entry->walking_style != 0) {
            /* Non-zero walking style: normal follower update */
            update_party_entity_graphics(char_id, entry->walking_style,
                                         entity_offset,
                                         slot - PARTY_LEADER_ENTITY_INDEX);
            uint16_t new_idx = adjust_party_member_visibility(
                entity_offset, char_id, pos_idx, 30);
            cs->position_index = new_idx & 0xFF;
        } else {
            /* Walking style 0: Bubble monkey behavioral modes */
            if (ow.bubble_monkey_mode == 0 || ow.bubble_monkey_mode == 2) {
                /* Mode 0 (normal) / 2 (returning): adjust visibility */
                uint16_t new_idx = adjust_party_member_visibility(
                    entity_offset, char_id, pos_idx, 12);
                uint16_t old_idx = pos_idx & 0xFF;
                cs->position_index = new_idx & 0xFF;

                if (old_idx == (new_idx & 0xFF) ||
                    ((old_idx + 1) & 0xFF) == (new_idx & 0xFF)) {
                    /* Position close: use buffer walking_style for graphics */
                    update_party_entity_graphics(
                        char_id, entry->walking_style,
                        entity_offset,
                        slot - PARTY_LEADER_ENTITY_INDEX);
                    if (!game_state.leader_moved)
                        goto follower_update_timer;
                } else {
                    /* Position changed: use walking_style 14 (running) */
                    update_party_entity_graphics(
                        char_id, 14,
                        entity_offset,
                        slot - PARTY_LEADER_ENTITY_INDEX);
                }

                /* Update direction and clear var7 bits 15,14,13 */
                entities.directions[entity_offset] = (int16_t)entry->direction;
                entities.var[7][entity_offset] &= (int16_t)0x1FFF;
            } else if (ow.bubble_monkey_mode == 1) {
                /* Mode 1 (hiding): set var7 bits 14,13,12 */
                entities.var[7][entity_offset] |= (int16_t)0x7000;
            } else if (ow.bubble_monkey_mode == 3) {
                /* Mode 3 (distracted): static pose + spacing hide */
                entities.var[7][entity_offset] |= (int16_t)0x7000;

                ow.bubble_monkey_distracted_next_direction_change_time--;
                if (ow.bubble_monkey_distracted_next_direction_change_time == 0) {
                    ow.bubble_monkey_distracted_direction_changes_left--;
                    if (ow.bubble_monkey_distracted_direction_changes_left == 0) {
                        ow.bubble_monkey_movement_change_timer = 15;
                        ow.bubble_monkey_distracted_next_direction_change_time =
                            (uint16_t)-1;
                    }

                    /* Randomize direction change interval */
                    ow.bubble_monkey_distracted_next_direction_change_time =
                        ((rng_next_byte() << 2) & 0x0F) + 4;

                    /* Flip direction between 2 and 6 (right/left) */
                    ow.bubble_monkey_distracted_next_direction ^= 0x0004;
                    entities.directions[entity_offset] =
                        (int16_t)ow.bubble_monkey_distracted_next_direction;
                }
            }

follower_update_timer:
            /* Update var3 = 4 (animation speed) */
            entities.var[3][entity_offset] = 4;

            /* Decrement movement change timer */
            if (ow.bubble_monkey_movement_change_timer != 0)
                ow.bubble_monkey_movement_change_timer--;
            if (ow.bubble_monkey_movement_change_timer == 0) {
                update_bubble_monkey_mode(char_id, cs->position_index);
                if (ow.bubble_monkey_mode == 3) {
                    /* Initialize distracted state */
                    ow.bubble_monkey_distracted_direction_changes_left = 4;
                    ow.bubble_monkey_distracted_next_direction = 6;
                    ow.bubble_monkey_distracted_next_direction_change_time = 15;
                    ow.bubble_monkey_movement_change_timer = (uint16_t)-1;
                } else {
                    ow.bubble_monkey_movement_change_timer = 60;
                }
            }

            /* Update surface flags from position buffer */
            entities.surface_flags[entity_offset] = entry->tile_flags;
        }
        break;
    }
    default:
        LOG_WARN("WARN: unknown tick callback $%06X for entity %d\n",
                 rom_addr, entity_offset);
        break;
    }
}

/* ---- Inline switch case wrappers ---- */

int16_t cr_disable_obj_hdma(int16_t entity_offset, int16_t script_offset,
                            uint16_t pc, uint16_t *out_pc) {
    /* Port of C4248A: disable HDMA channel 4, zero WOBJSEL.
     * Disables the per-scanline window tables and color window. */
    *out_pc = pc;
    ppu.wobjsel = 0;
    ppu.window_hdma_active = false;
    return 0;
}

int16_t cr_init_window_registers(int16_t entity_offset, int16_t script_offset,
                                 uint16_t pc, uint16_t *out_pc) {
    /* Port of C423DC: initialize SNES window registers */
    *out_pc = pc;
    ppu.wh0 = 0x80;
    ppu.wh1 = 0x7F;
    ppu.wh2 = 0x80;
    ppu.wh3 = 0x7F;
    ppu.cgwsel = 0x10;
    ppu.tmw = 0x13;
    ppu.wbglog = 0;
    ppu.wobjlog = 0;
    return 0;
}

int16_t cr_decomp_itoi_production(int16_t entity_offset, int16_t script_offset,
                                  uint16_t pc, uint16_t *out_pc) {
    /* Port of asm/intro/decomp_itoi_production.asm.
     * Decompresses and uploads the "Produced by SHIGESATO ITOI" overlay:
     *   1. Arrangement (tilemap) → VRAM $7C00 (BG3 tilemap, 0x800 bytes)
     *   2. Graphics (tiles) → VRAM $6000 (BG3 tiles, 0x400 bytes)
     *   3. Palette → ert.palettes[] mirror, set upload mode 24
     * SET_BUFFER_TILEMAP_PRIORITY: ORs 0x2000 onto all 1024 tilemap words. */
    *out_pc = pc;

    /* Step 1: Arrangement tilemap — decompress directly to VRAM $7C00 */
    size_t comp_sz = ASSET_SIZE(ASSET_INTRO_ATTRACT_PRODUCED_BY_ITOI_ARR_LZHAL);
    const uint8_t *comp = ASSET_DATA(ASSET_INTRO_ATTRACT_PRODUCED_BY_ITOI_ARR_LZHAL);
    if (comp) {
        uint8_t *arr_dst = &ppu.vram[0x7C00 * 2];
        memset(arr_dst, 0, 0x800);
        decomp(comp, comp_sz, arr_dst, 0x800);
        /* SET_BUFFER_TILEMAP_PRIORITY: set priority bit on 1024 tilemap entries */
        for (int i = 0; i < 1024; i++) {
            uint16_t val = read_u16_le(&arr_dst[i * 2]);
            val |= 0x2000;
            arr_dst[i * 2] = val & 0xFF;
            arr_dst[i * 2 + 1] = val >> 8;
        }
    }

    /* Step 2: Graphics tiles — decompress directly to VRAM $6000 */
    comp_sz = ASSET_SIZE(ASSET_INTRO_ATTRACT_PRODUCED_BY_ITOI_GFX_LZHAL);
    comp = ASSET_DATA(ASSET_INTRO_ATTRACT_PRODUCED_BY_ITOI_GFX_LZHAL);
    if (comp) {
        decomp(comp, comp_sz, &ppu.vram[0x6000 * 2], 0x400);
    }

    /* Step 3: Shared palette */
    comp_sz = ASSET_SIZE(ASSET_INTRO_ATTRACT_NINTENDO_ITOI_PAL_LZHAL);
    comp = ASSET_DATA(ASSET_INTRO_ATTRACT_NINTENDO_ITOI_PAL_LZHAL);
    if (comp) {
        decomp(comp, comp_sz, (uint8_t *)ert.palettes, sizeof(ert.palettes));
        ert.palettes[0] = 0;  /* STZ PALETTES — transparent color 0 */
        ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    }
    return 0;
}

int16_t cr_decomp_nintendo_presentation(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* Port of asm/intro/decomp_nintendo_presentation.asm.
     * Same structure as DECOMP_ITOI_PRODUCTION but with Nintendo assets. */
    *out_pc = pc;

    /* Step 1: Arrangement tilemap — decompress directly to VRAM $7C00 */
    size_t comp_sz = ASSET_SIZE(ASSET_INTRO_ATTRACT_NINTENDO_PRESENTATION_ARR_LZHAL);
    const uint8_t *comp = ASSET_DATA(ASSET_INTRO_ATTRACT_NINTENDO_PRESENTATION_ARR_LZHAL);
    if (comp) {
        uint8_t *arr_dst = &ppu.vram[0x7C00 * 2];
        decomp(comp, comp_sz, arr_dst, 0x800);
        for (int i = 0; i < 1024; i++) {
            uint16_t val = read_u16_le(&arr_dst[i * 2]);
            val |= 0x2000;
            arr_dst[i * 2] = val & 0xFF;
            arr_dst[i * 2 + 1] = val >> 8;
        }
    }

    /* Step 2: Graphics tiles — decompress directly to VRAM $6000 */
    comp_sz = ASSET_SIZE(ASSET_INTRO_ATTRACT_NINTENDO_PRESENTATION_GFX_LZHAL);
    comp = ASSET_DATA(ASSET_INTRO_ATTRACT_NINTENDO_PRESENTATION_GFX_LZHAL);
    if (comp) {
        decomp(comp, comp_sz, &ppu.vram[0x6000 * 2], 0x400);
    }

    /* Step 3: Shared palette */
    comp_sz = ASSET_SIZE(ASSET_INTRO_ATTRACT_NINTENDO_ITOI_PAL_LZHAL);
    comp = ASSET_DATA(ASSET_INTRO_ATTRACT_NINTENDO_ITOI_PAL_LZHAL);
    if (comp) {
        decomp(comp, comp_sz, (uint8_t *)ert.palettes, sizeof(ert.palettes));
        ert.palettes[0] = 0;
        ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    }
    return 0;
}

int16_t cr_play_flyover_script(int16_t entity_offset, int16_t script_offset,
                               uint16_t pc, uint16_t *out_pc) {
    /* PLAY_FLYOVER_SCRIPT (C49EC4) — flyover text sequence.
     * Script ID comes from A register / tempvar. */
    play_flyover_script(scripts.tempvar[script_offset]);
    *out_pc = pc;
    return 0;
}

int16_t cr_choose_random(int16_t entity_offset, int16_t script_offset,
                         uint16_t pc, uint16_t *out_pc) {
    /* Variable-length: reads 1 byte (count), then count × 2 bytes (word entries).
     * Assembly: RAND → DIVISION8S_DIVISOR_POSITIVE → index = rand % count.
     * Returns the selected 16-bit entry. */
    uint8_t count = sb(pc);
    pc++;
    uint16_t idx = (count > 0) ? (uint16_t)(rng_next_byte() % count) : 0;
    int16_t result = (int16_t)sw(pc + idx * 2);
    *out_pc = pc + count * 2;
    return result;
}

int16_t cr_test_player_in_area(int16_t entity_offset, int16_t script_offset,
                               uint16_t pc, uint16_t *out_pc) {
    /* Port of C46E74: test if leader is within bounding box.
     * var[0]=center_x, var[1]=center_y, var[2]=x_radius, var[3]=y_radius.
     * Returns TRUE (-1) if within, FALSE (0) if outside.
     * Assembly lines 9-12: if PSI_TELEPORT_DESTINATION != 0, return FALSE immediately. */
    *out_pc = pc;
    if (ow.psi_teleport_destination != 0)
        return 0;  /* FALSE — player is teleporting */
    int16_t dx = entities.var[0][entity_offset] - (int16_t)game_state.leader_x_coord;
    int16_t dy = entities.var[1][entity_offset] - (int16_t)game_state.leader_y_coord;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx < entities.var[2][entity_offset] &&
        dy < entities.var[3][entity_offset]) {
        return -1;  /* TRUE */
    }
    return 0;  /* FALSE */
}

int16_t cr_make_party_look_at_entity(int16_t entity_offset, int16_t script_offset,
                                     uint16_t pc, uint16_t *out_pc) {
    /* Port of C48B3B: make all party members face toward current entity.
     * Assembly checks party_order validity and only updates sprite on direction change.
     * Assembly: LDA FRAME_COUNTER; AND #1; BNEL @RETURN — only runs on even frames. */
    *out_pc = pc;
    if (core.frame_counter & 1)
        return 0;
    int16_t ent_off = ENT(ert.current_entity_slot);
    for (int i = 0; i < game_state.party_count; i++) {
        uint8_t order = game_state.party_order[i];
        if (order >= 16)
            continue;
        int16_t pm_off = ENT((int16_t)read_u16_le(&game_state.party_entity_slots[i * 2]));
        int16_t dir = calculate_direction_8(
            entities.abs_x[pm_off], entities.abs_y[pm_off],
            entities.abs_x[ent_off], entities.abs_y[ent_off]);
        if (entities.directions[pm_off] != dir) {
            entities.directions[pm_off] = dir;
            render_entity_sprite(pm_off);
        }
    }
    return 0;
}

int16_t cr_movement_cmd_calc_travel_frames(int16_t entity_offset, int16_t script_offset,
                                           uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A6A2 + C0CA4E: read 2-byte distance param from script,
     * compute sleep_frames = (param << 16) / max(abs(delta_x), abs(delta_y)),
     * store directly into scripts.sleep_frames[].
     *
     * This calculates how many frames an entity must sleep to travel
     * 'param' pixels at its current velocity. */
    int16_t param = (int16_t)sw(pc);
    *out_pc = pc + 2;
    int16_t offset = ENT(ert.current_entity_slot);

    /* Build 32-bit fixed-point velocity (16.16) */
    int32_t dx = (int32_t)(((uint32_t)(int32_t)entities.delta_x[offset] << 16) |
                  (uint32_t)entities.delta_frac_x[offset]);
    int32_t dy = (int32_t)(((uint32_t)(int32_t)entities.delta_y[offset] << 16) |
                  (uint32_t)entities.delta_frac_y[offset]);

    /* Find the dominant (larger absolute) velocity component */
    int32_t abs_dx = dx < 0 ? -dx : dx;
    int32_t abs_dy = dy < 0 ? -dy : dy;
    int32_t dominant = abs_dx > abs_dy ? abs_dx : abs_dy;

    if (dominant > 0) {
        /* sleep_frames = (param << 16) / dominant */
        int32_t param32 = (int32_t)param << 16;
        int32_t frames = param32 / dominant;
        scripts.sleep_frames[script_offset] = (int16_t)frames;
    } else {
        scripts.sleep_frames[script_offset] = 0;
    }
    return 0;
}

int16_t cr_movement_cmd_return_2(int16_t entity_offset, int16_t script_offset,
                                 uint16_t pc, uint16_t *out_pc) {
    /* Assembly: LDX #$0002; RTL — always returns 2. */
    *out_pc = pc;
    return 2;
}

int16_t cr_is_entity_near_leader(int16_t entity_offset, int16_t script_offset,
                                 uint16_t pc, uint16_t *out_pc) {
    /* Port of C0C6B6: check if current entity is on screen.
     * Returns -1 (TRUE) if within extended screen bounds on both axes,
     * 0 (FALSE) otherwise. Screen center is at (leader_x-CENTER, leader_y-CENTER). */
    *out_pc = pc;
    int16_t ent_off = ENT(ert.current_entity_slot);
    int16_t rel_x = entities.abs_x[ent_off] -
                     ((int16_t)game_state.leader_x_coord - VIEWPORT_CENTER_X);
    int16_t rel_y = entities.abs_y[ent_off] -
                     ((int16_t)game_state.leader_y_coord - VIEWPORT_CENTER_Y);
    if (rel_x >= -64 && rel_x < (VIEWPORT_WIDTH + 64) && rel_y >= -64 && rel_y < (VIEWPORT_HEIGHT + 96))
        return -1;  /* on screen */
    return 0;       /* off screen */
}
