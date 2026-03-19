/*
 * Entity callbacks — movement, screen position, draw, and sprite rendering.
 *
 * Ports of:
 *   APPLY_ENTITY_DELTA_POSITION_ENTRY2 (asm/overworld/entity/apply_entity_delta_position_entry2.asm)
 *   COPY_ENTITY_ABS_TO_SCREEN          (asm/overworld/entity/copy_entity_abs_to_screen.asm)
 *   ENTITY_SCREEN_COORDS_BG1           (asm/overworld/entity/entity_screen_coords_bg1.asm)
 *   DRAW_TITLE_LETTER (draw callback)     (asm/intro/draw_title_letter.asm)
 *   DRAW_ENTITY_SPRITE                 (asm/overworld/entity/draw_entity_sprite.asm)
 *   BUILD_ENTITY_DRAW_LIST             (asm/overworld/entity/build_entity_draw_list.asm)
 *   DISPATCH_SPRITE_DRAW_BY_PRIORITY   (asm/overworld/entity/dispatch_sprite_draw_by_priority.asm)
 *   RENDER_ALL_PRIORITY_SPRITES        (asm/overworld/entity/render_all_priority_sprites.asm)
 *   WRITE_SPRITEMAP_TO_OAM             (asm/overworld/entity/write_spritemap_to_oam.asm)
 */
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "data/assets.h"
#include "game/overworld.h"
#include "include/binary.h"
#include "snes/ppu.h"
#include <stdlib.h>
#include <string.h>

/* Spritemap bank IDs in the priority queue.
 * Bank 0 = title screen spritemap (frame index lookup).
 * Bank 1 = overworld entity spritemap (direct offset into overworld_spritemaps[]).
 * Bank 2 = overlay spritemap (byte offset into overlay_data[]).
 * Bank 3 = town map icon spritemap (byte offset into town_map_spritemap_data[]). */
#define SMAP_BANK_TITLE    0
#define SMAP_BANK_OVERWORLD 1
#define SMAP_BANK_OVERLAY   2
#define SMAP_BANK_TOWNMAP   3

/* ---- Entity overlay data (entity_overlays.asm) ---- */

/* Base within-bank ROM address of the overlay data blob in bank $C4.
 * All script bytecode pointers are within-bank addresses that must be
 * converted to ert.buffer offsets by subtracting this base. */
#define OVERLAY_ROM_BASE 0x0E31

/* Buffer offsets of animation scripts within the overlay data blob.
 * Computed from the assembly layout: count(1) + sprites(16) + frames(110) = 127 */
#define OVERLAY_SCRIPT_SWEATING       127
#define OVERLAY_SCRIPT_MUSHROOMIZED   179
#define OVERLAY_SCRIPT_RIPPLE         191
#define OVERLAY_SCRIPT_BIG_RIPPLE     211

/* VRAM base address for overlay sprite tiles (VRAM::OVERLAY_BASE) */
#define VRAM_OVERLAY_BASE 0x5600

/* Overlay and town map spritemap data — compile-time linked, always available */
#define overlay_data       ASSET_DATA(ASSET_OVERWORLD_SPRITES_ENTITY_OVERLAY_DATA_BIN)
#define overlay_data_size  ASSET_SIZE(ASSET_OVERWORLD_SPRITES_ENTITY_OVERLAY_DATA_BIN)
#define town_map_spritemap_data       ASSET_DATA(ASSET_TOWN_MAPS_ICON_SPRITEMAPS_BIN)
#define town_map_spritemap_data_size  ASSET_SIZE(ASSET_TOWN_MAPS_ICON_SPRITEMAPS_BIN)

/* OAM writing limit */
#define OAM_MAX 128

/*
 * APPLY_ENTITY_DELTA_POSITION_ENTRY2 (C09FC8)
 *
 * 16.16 fixed-point position update: pos += delta with carry.
 */
static void apply_entity_delta_position(int16_t ent) {
    /* X */
    uint32_t fx = (uint32_t)entities.frac_x[ent] +
                  (uint32_t)entities.delta_frac_x[ent];
    uint16_t carry = (fx >> 16) & 1;
    entities.frac_x[ent] = (uint16_t)fx;
    entities.abs_x[ent] = (int16_t)((uint16_t)entities.abs_x[ent] +
                                     (uint16_t)entities.delta_x[ent] + carry);

    /* Y */
    uint32_t fy = (uint32_t)entities.frac_y[ent] +
                  (uint32_t)entities.delta_frac_y[ent];
    carry = (fy >> 16) & 1;
    entities.frac_y[ent] = (uint16_t)fy;
    entities.abs_y[ent] = (int16_t)((uint16_t)entities.abs_y[ent] +
                                     (uint16_t)entities.delta_y[ent] + carry);
}

/*
 * ENTITY_SCREEN_COORDS_BG1 (C0A023)
 *
 * For the overworld, screen coords = abs - BG scroll.
 * Not used for title screen letters (they use COPY_ENTITY_ABS_TO_SCREEN).
 */
static void entity_screen_coords_bg1(int16_t ent) {
    /* For the title screen, BG1 scroll is 0 so this is the same as copy */
    entities.screen_x[ent] = entities.abs_x[ent] - (int16_t)ppu.bg_hofs[0];
    entities.screen_y[ent] = entities.abs_y[ent] - (int16_t)ppu.bg_vofs[0];
}

/*
 * COPY_ENTITY_ABS_TO_SCREEN (C0A0BB)
 *
 * Simply copies absolute position to screen position.
 * Used by title screen letter sprites (no camera offset).
 */
static void copy_entity_abs_to_screen(int16_t ent) {
    entities.screen_x[ent] = entities.abs_x[ent];
    entities.screen_y[ent] = entities.abs_y[ent];
}

/*
 * ENTITY_WORLD_TO_SCREEN (C0A055)
 *
 * BG3-relative screen coords: screen = abs - BG3 scroll.
 * Used by ending credits entities and other BG3-based scenes.
 */
static void entity_screen_coords_bg3(int16_t ent) {
    entities.screen_x[ent] = entities.abs_x[ent] - (int16_t)ppu.bg_hofs[2];
    entities.screen_y[ent] = entities.abs_y[ent] - (int16_t)ppu.bg_vofs[2];
}

/*
 * ENTITY_SCREEN_COORDS_BG1_WITH_Z (C0A03A)
 *
 * Like entity_screen_coords_bg1 but subtracts entity Z from screen Y.
 * Used by flying/jumping entities (bouncing items, butterflies).
 */
static void entity_screen_coords_bg1_with_z(int16_t ent) {
    entities.screen_x[ent] = entities.abs_x[ent] - (int16_t)ppu.bg_hofs[0];
    entities.screen_y[ent] = entities.abs_y[ent] - (int16_t)ppu.bg_vofs[0]
                           - entities.abs_z[ent];
}

/*
 * ENTITY_SCREEN_COORDS_BG3_WITH_Z (C0A0A0)
 *
 * BG3-relative with Z subtraction. Combines BG3 scroll and Z-height.
 * Used by scenes like Tessie/lake where BG3 is the active layer.
 */
static void entity_screen_coords_bg3_with_z(int16_t ent) {
    entities.screen_x[ent] = entities.abs_x[ent] - (int16_t)ppu.bg_hofs[2];
    entities.screen_y[ent] = entities.abs_y[ent] - (int16_t)ppu.bg_vofs[2]
                           - entities.abs_z[ent];
}

/*
 * APPLY_ENTITY_DELTA_POSITION_3D (C09FF1)
 *
 * Applies X/Y/Z delta position then updates surface flags.
 * Used by entities that move in 3D (flying objects, bouncing items).
 * Assembly: JSR APPLY_ENTITY_DELTA_POSITION_ENTRY2
 *           then adds Z fraction/integer delta, then JSL UPDATE_ENTITY_SURFACE_FLAGS.
 */
static void apply_entity_delta_position_3d(int16_t ent) {
    /* Apply X/Y delta (same as APPLY_ENTITY_DELTA_POSITION_ENTRY2) */
    apply_entity_delta_position(ent);

    /* Apply Z delta with 16.16 fixed-point carry */
    uint32_t fz = (uint32_t)entities.frac_z[ent] +
                  (uint32_t)entities.delta_frac_z[ent];
    uint16_t carry = (fz >> 16) & 1;
    entities.frac_z[ent] = (uint16_t)fz;
    entities.abs_z[ent] = (int16_t)((uint16_t)entities.abs_z[ent] +
                                     (uint16_t)entities.delta_z[ent] + carry);

    /* UPDATE_ENTITY_SURFACE_FLAGS */
    extern uint16_t lookup_surface_flags(int16_t x, int16_t y, uint16_t size_code);
    uint16_t sf = lookup_surface_flags(
        entities.abs_x[ent], entities.abs_y[ent], entities.sizes[ent]);
    entities.surface_flags[ent] = sf;
}

/* Water speed multipliers (from include/config.asm).
 * Fixed-point 16.16: upper 16 bits = integer, lower 16 = fraction. */
#define SHALLOW_WATER_SPEED 0x8000u   /* 0.5x */
#define DEEP_WATER_SPEED    0x547Au   /* ~0.33x */

/*
 * ENTITY_PHYSICS_FORCE_MOVE (C0A37A)
 *
 * Applies delta position then looks up surface flags from the map.
 * Surface flags determine OAM priority overrides (behind rooftops)
 * and visual effects (wading in shallow water).
 *
 * Assembly: LDX $88; JSR APPLY_ENTITY_DELTA_POSITION_ENTRY3;
 *           JSL UPDATE_ENTITY_SURFACE_FLAGS; RTS
 *
 * NOTE: Water speed scaling is NOT done here. In the original game,
 * water speed adjustment is handled by ADJUST_POSITION_HORIZONTAL/
 * ADJUST_POSITION_VERTICAL, called from UPDATE_LEADER_MOVEMENT
 * (a separate code path). Do NOT add water scaling here.
 */
static void entity_physics_force_move(int16_t ent) {
    apply_entity_delta_position(ent);

    /* Port of UPDATE_ENTITY_SURFACE_FLAGS (C0C7DB):
     * looks up the map surface at the entity's position and stores
     * the result in ENTITY_SURFACE_FLAGS.
     * Assembly passes CURRENT_ENTITY_SLOT as Y to LOOKUP_SURFACE_FLAGS,
     * which internally reads ENTITY_SIZES[slot]. The C port pre-resolves
     * this: entities.sizes[ent] is already the size code. */
    extern uint16_t lookup_surface_flags(int16_t x, int16_t y, uint16_t size_code);
    uint16_t sf = lookup_surface_flags(
        entities.abs_x[ent], entities.abs_y[ent], entities.sizes[ent]);
    entities.surface_flags[ent] = sf;
}

/*
 * Movement callback dispatcher.
 *
 * Port of the Phase 2 movement callback table in RUN_ACTIONSCRIPT_FRAME.
 */
void call_move_callback(int16_t entity_offset) {
    switch (entities.move_callback[entity_offset]) {
    case CB_MOVE_FORCE_MOVE:
        entity_physics_force_move(entity_offset);
        break;
    case CB_MOVE_PARTY_SPRITE:
        /* UPDATE_PARTY_SPRITE_POSITION (C0A26B):
         * Party follower move callback — does NOT apply delta position.
         * Instead computes screen coords from abs coords (screen = abs - bg_scroll).
         * The actual position update is done by UPDATE_FOLLOWER_STATE (tick callback)
         * which reads from PLAYER_POSITION_BUFFER.
         * The full ROM version also handles overlap avoidance via FOLLOWER_DISTANCE_CHECK_TABLE,
         * but the common path is just entity_screen_coords_bg1. */
        entity_screen_coords_bg1(entity_offset);
        break;
    case CB_MOVE_DELTA_3D:
        apply_entity_delta_position_3d(entity_offset);
        break;
    case CB_MOVE_WITH_COLLISION:
        /* ENTITY_PHYSICS_WITH_COLLISION (C0A360):
         * If pathfinding active (bit 15 set), force move (delta + surface flags).
         * If obstacle flags & 0xD0 (bits 4,6,7), zero all velocity.
         * If collided with entity (bit 15 set), force move.
         * Otherwise do nothing (entity is blocked). */
        if ((int16_t)entities.pathfinding_states[entity_offset] < 0) {
            entity_physics_force_move(entity_offset);
        } else if (entities.obstacle_flags[entity_offset] & 0x00D0) {
            /* Zero all velocity components (MOVEMENT_CODE_39) */
            entities.delta_frac_x[entity_offset] = 0;
            entities.delta_x[entity_offset] = 0;
            entities.delta_frac_y[entity_offset] = 0;
            entities.delta_y[entity_offset] = 0;
            entities.delta_frac_z[entity_offset] = 0;
            entities.delta_z[entity_offset] = 0;
        } else if ((int16_t)entities.collided_objects[entity_offset] < 0) {
            entity_physics_force_move(entity_offset);
        }
        break;
    case CB_MOVE_SIMPLE_COLLISION:
        /* ENTITY_PHYSICS_SIMPLE_COLLISION (C0A384):
         * Same obstacle/collision checks as WITH_COLLISION, but
         * calls APPLY_DELTA directly (no surface flags update). */
        if ((int16_t)entities.pathfinding_states[entity_offset] < 0) {
            apply_entity_delta_position(entity_offset);
        } else if (entities.obstacle_flags[entity_offset] & 0x00D0) {
            entities.delta_frac_x[entity_offset] = 0;
            entities.delta_x[entity_offset] = 0;
            entities.delta_frac_y[entity_offset] = 0;
            entities.delta_y[entity_offset] = 0;
            entities.delta_frac_z[entity_offset] = 0;
            entities.delta_z[entity_offset] = 0;
        } else if ((int16_t)entities.collided_objects[entity_offset] < 0) {
            apply_entity_delta_position(entity_offset);
        }
        break;
    case CB_MOVE_NOP:
        /* APPLY_ENTITY_DELTA_POSITION_ENTRY4 (C09FF0): bare RTS, intentional no-op.
         * Used to freeze entities in place (e.g., before enemy despawn). */
        break;
    default:
        apply_entity_delta_position(entity_offset);
        break;
    }
}

/*
 * Screen position callback dispatcher.
 */
void call_screen_pos_callback(int16_t entity_offset) {
    switch (entities.screen_pos_callback[entity_offset]) {
    case CB_POS_COPY_ABS:
        copy_entity_abs_to_screen(entity_offset);
        break;
    case CB_POS_NOP:
        /* POSITION_CALLBACK_NOP: do nothing. Screen position is set by
         * the tick callback (e.g., ACTIONSCRIPT_SIMPLE_SCREEN_POSITION_CALLBACK). */
        break;
    case CB_POS_SCREEN_BG3:
        entity_screen_coords_bg3(entity_offset);
        break;
    case CB_POS_SCREEN_BG1_Z:
        entity_screen_coords_bg1_with_z(entity_offset);
        break;
    case CB_POS_SCREEN_BG3_Z:
        entity_screen_coords_bg3_with_z(entity_offset);
        break;
    case CB_POS_FORCE_MOVE:
        /* ENTITY_PHYSICS_FORCE_MOVE used as position callback (script 626).
         * Applies delta position + surface flags from the position callback slot,
         * giving the entity a second delta application per frame. */
        entity_physics_force_move(entity_offset);
        break;
    default:
        entity_screen_coords_bg1(entity_offset);
        break;
    }
}

/*
 * WRITE_SPRITEMAP_TO_OAM (C08CD5)
 *
 * Renders a spritemap to the OAM ert.buffer.
 * Each spritemap entry is 5 bytes:
 *   [0] y_offset (signed)
 *   [1] tile number
 *   [2] attr (vhoopppc)
 *   [3] x_offset (signed)
 *   [4] special_flags (bit 7 = end, bit 0 = large 16x16)
 *
 * y_offset == 0x80 is a special "follow link" marker (unused for title).
 */
void write_spritemap_to_oam(const uint8_t *spritemap, int16_t base_x,
                            int16_t base_y) {
    int idx = 0;

    LOG_TRACE("write_spritemap_to_oam: base=(%d,%d) oam_idx=%u first5=[%02X %02X %02X %02X %02X]\n",
              base_x, base_y, ert.oam_write_index,
              spritemap[0], spritemap[1], spritemap[2], spritemap[3], spritemap[4]);

    while (ert.oam_write_index < OAM_MAX) {
        int8_t y_off = (int8_t)spritemap[idx + 0];
        uint8_t tile  = spritemap[idx + 1];
        uint8_t attr  = spritemap[idx + 2];
        int8_t x_off = (int8_t)spritemap[idx + 3];
        uint8_t flags = spritemap[idx + 4];

        /* Check for follow-link marker (y_offset == 0x80).
         * Assembly: LDA tile,Y (16-bit) → TAY → BRA @UNKNOWN3
         * The tile+attr bytes form a 16-bit byte offset into the spritemap
         * data, allowing multi-part spritemaps to chain to other entries. */
        if (y_off == (int8_t)0x80) {
            idx = read_u16_le(&spritemap[idx + 1]);
            continue;
        }

        /* Compute screen position */
        int16_t sy = base_y + y_off - 1;
        int16_t sx = base_x + x_off;

        /* Y bounds check: visible if sy in [0, VIEWPORT_HEIGHT) or [-32, -1] (wrap).
         * Assembly uses 0xE0 (224) for native SNES height; we use VIEWPORT_HEIGHT. */
        if ((uint16_t)sy >= VIEWPORT_HEIGHT && (uint16_t)sy < 0xFFE0) {
            if (flags & 0x80)
                break;
            idx += 5;
            continue;
        }

        /* X bounds check: skip sprites fully off-screen.
         * For expanded viewports, sprites at X in [256, VIEWPORT_WIDTH+32) are valid.
         * Assembly only allowed X high byte == 0 or 0xFF (9-bit SNES OAM range). */
        if (sx >= VIEWPORT_WIDTH + 32 || sx < -32) {
            if (flags & 0x80)
                break;
            idx += 5;
            continue;
        }
        uint8_t x_lo = (uint8_t)(sx & 0xFF);
        uint8_t x_hi_bit = (uint8_t)((sx >> 8) & 1);

        /* Size bit from special_flags bit 0 */
        uint8_t size_bit = (flags & 0x01);

        /* Write to OAM */
        uint16_t oi = ert.oam_write_index;
        ppu.oam[oi].x = x_lo;
        ppu.oam[oi].y = (uint8_t)(sy & 0xFF);
        ppu.oam[oi].tile = tile;
        ppu.oam[oi].attr = attr;
        ppu.oam_full_x[oi] = sx;  /* Full 16-bit X for expanded viewport */
        ppu.oam_full_y[oi] = sy;  /* Full 16-bit Y for expanded viewport */

        /* OAM high table: 2 bits per sprite, packed 4 per byte */
        uint16_t hi_byte_idx = oi >> 2;
        uint8_t hi_bit_pos = (oi & 3) * 2;
        uint8_t hi_val = (size_bit << 1) | x_hi_bit;
        ppu.oam_hi[hi_byte_idx] &= ~(0x03 << hi_bit_pos);
        ppu.oam_hi[hi_byte_idx] |= (hi_val << hi_bit_pos);

        LOG_TRACE("  OAM[%u]: x=%d y=%d tile=0x%02X attr=0x%02X size=%u xhi=%u\n",
                  oi, (int)x_lo, (int)(uint8_t)(sy & 0xFF), tile, attr, size_bit, x_hi_bit);

        ert.oam_write_index++;

        if (flags & 0x80)
            break;  /* end of spritemap */

        idx += 5;
    }
}

/*
 * Add a spritemap to a priority queue.
 *
 * Port of DISPATCH_SPRITE_DRAW_BY_PRIORITY (C08C58) → SPRITE_PRIORITY_DISPATCH_TABLE.
 * Each priority level has a queue of pending spritemaps to render.
 *
 * The ROM stores 16-bit ROM offsets + bank bytes in the queue. In the C port,
 * we store a spritemap identifier + bank type, and look up the actual
 * spritemap data at render time.
 *
 * bank: SMAP_BANK_TITLE (frame index) or SMAP_BANK_OVERWORLD (ert.buffer offset).
 */
void queue_sprite_draw(uint16_t smap_id, int16_t x, int16_t y,
                       uint16_t priority, uint16_t bank) {
    if (priority >= 4)
        priority = 1;  /* default */
    SpritePriorityQueue *q = &sprite_priority[priority];
    uint16_t off = q->offset;
    if (off >= MAX_PRIORITY_SPRITES)
        return;  /* queue full */

    q->spritemaps[off] = smap_id;
    q->sprite_x[off] = x;
    q->sprite_y[off] = y;
    q->spritemap_banks[off] = bank;
    q->offset = off + 1;
}

/* ---- Entity Overlay System ---- */


/*
 * LOAD_OVERLAY_SPRITES (C4B26B)
 *
 * Loads overlay sprite graphics to VRAM and initializes all entity
 * overlay animation state. Called during map load.
 *
 * Phase 1: Iterate through the 4 overlay sprite entries in the blob.
 *   For each: load 2 palette variants of sprite graphics via
 *   load_sprite_tiles_to_vram, starting at VRAM::OVERLAY_BASE.
 *
 * Phase 2: Initialize all entity overlay pointer arrays to the
 *   starting offsets of their animation scripts.
 */
void load_overlay_sprites(void) {

    /* Phase 1: Load overlay sprite graphics to VRAM.
     * Overlay data[0] = count (4), data[1..16] = 4 sprite entries × 4 bytes each.
     * Each entry: [sprite_id:2LE][palette1:1][palette2:1] */
    uint8_t count = overlay_data[0];
    uint16_t vram_pos = VRAM_OVERLAY_BASE;
    const uint8_t *sprite_entry = overlay_data + 1;

    for (uint8_t i = 0; i < count && i < 4; i++) {
        uint16_t sprite_id = read_u16_le(sprite_entry);
        uint8_t palette1 = sprite_entry[2];
        uint8_t palette2 = sprite_entry[3];

        vram_pos = load_sprite_tiles_to_vram(sprite_id, palette1, vram_pos);
        vram_pos = load_sprite_tiles_to_vram(sprite_id, palette2, vram_pos);

        sprite_entry += 4;
    }

    /* Phase 2: Initialize overlay animation pointers for all entities.
     * Each entity gets the starting offset of each animation script. */
    for (int slot = 0; slot < MAX_ENTITIES; slot++) {
        int16_t x = (int16_t)slot;
        entities.mushroomized_overlay_ptrs[x] = OVERLAY_SCRIPT_MUSHROOMIZED;
        entities.sweating_overlay_ptrs[x]     = OVERLAY_SCRIPT_SWEATING;
        entities.ripple_overlay_ptrs[x]       = OVERLAY_SCRIPT_RIPPLE;
        entities.big_ripple_overlay_ptrs[x]   = OVERLAY_SCRIPT_BIG_RIPPLE;
        entities.mushroomized_next_update[x]  = 0;
        entities.sweating_next_update[x]      = 0;
        entities.ripple_next_update[x]        = 0;
        entities.big_ripple_next_update[x]    = 0;
        entities.mushroomized_spritemaps[x]   = 0;
        entities.sweating_spritemaps[x]       = 0;
        entities.ripple_spritemaps[x]         = 0;
        entities.big_ripple_spritemaps[x]     = 0;
    }
}

/*
 * EXECUTE_OVERLAY_ANIMATION_SCRIPT (C0AD56)
 *
 * Bytecode interpreter for overlay animation scripts.
 * Commands (each 4 bytes: opcode:2LE + arg:2LE):
 *   $0001 SHOWFRAME arg  — store frame data offset to spritemaps array
 *   $0002 DELAY arg      — return delay, advance script pointer
 *   $0003 JUMPTO arg     — jump to script offset (loop)
 *
 * Parameters:
 *   script_offset: ert.buffer offset of current script position in overlay_data
 *   spritemaps: pointer to entity's spritemaps array
 *   entity_offset: entity index into the spritemaps array
 *   out_new_ptr: receives the updated script pointer (ert.buffer offset)
 *
 * Returns: delay frame count
 */
static uint16_t execute_overlay_animation_script(uint16_t script_offset,
                                                  uint16_t *spritemaps,
                                                  int16_t entity_offset,
                                                  uint16_t *out_new_ptr) {
    uint16_t y = 0;

    for (;;) {
        if (script_offset + y + 4 > overlay_data_size) {
            *out_new_ptr = script_offset;
            return 1;
        }

        uint16_t cmd = read_u16_le(overlay_data + script_offset + y);
        y += 2;
        uint16_t arg = read_u16_le(overlay_data + script_offset + y);
        y += 2;

        if (cmd == 0x0001) {
            /* SHOWFRAME: store frame data pointer.
             * arg is a ROM within-bank address, convert to ert.buffer offset.
             * arg == 0 means "no frame" (sentinel). */
            if (arg == 0)
                spritemaps[entity_offset] = 0;
            else
                spritemaps[entity_offset] = arg - OVERLAY_ROM_BASE;
        } else if (cmd == 0x0003) {
            /* JUMPTO: arg is ROM address of new script position */
            script_offset = arg - OVERLAY_ROM_BASE;
            y = 0;
        } else {
            /* DELAYNEXTFRAME (cmd == 0x0002): return delay, update script ptr */
            *out_new_ptr = script_offset + y;
            return arg;
        }
    }
}

/*
 * DRAW_ENTITY_OVERLAYS (C0AC43)
 *
 * Draws water ripple, sweating, and mushroomized overlays for an entity.
 * Called during entity sprite rendering, after main sprite is queued.
 *
 * Checks ENTITY_SURFACE_FLAGS for water/ripple state and ENTITY_OVERLAY_FLAGS
 * for sweating (bit 15) and mushroomized (bit 14).
 *
 * entity_offset: entity array index
 * priority: draw priority from the entity's draw_priority
 */
static void draw_entity_overlays(int16_t entity_offset, uint16_t priority) {
    /* Surface offset: 0 normally, 5 when entity is on water surface.
     * This shifts to the alternate-palette sub-frame in each overlay frame. */
    uint16_t surface_offset = 0;
    if (entities.surface_flags[entity_offset] & 0x0001)
        surface_offset = 5;

    /* Check for ripple overlay (surface_flags bits 2-3) */
    uint16_t ripple_bits = entities.surface_flags[entity_offset] & 0x000C;
    if (ripple_bits != 0) {
        if (ripple_bits == 0x0004) {
            /* Value 4 = draw sweat instead of ripple (jump to sweat section) */
            goto draw_sweat;
        }

        if (entities.byte_widths[entity_offset] != 0x0040) {
            /* Big ripple (for entities with byte_widths != 0x40) */
            if (entities.big_ripple_next_update[entity_offset] == 0) {
                uint16_t new_ptr;
                uint16_t delay = execute_overlay_animation_script(
                    entities.big_ripple_overlay_ptrs[entity_offset],
                    entities.big_ripple_spritemaps,
                    entity_offset, &new_ptr);
                entities.big_ripple_overlay_ptrs[entity_offset] = new_ptr;
                entities.big_ripple_next_update[entity_offset] = delay;
            }
            entities.big_ripple_next_update[entity_offset]--;

            int16_t x = entities.screen_x[entity_offset];
            int16_t y = entities.screen_y[entity_offset] + 8;  /* +8 for big ripple */
            uint16_t smap = entities.big_ripple_spritemaps[entity_offset];
            /* Big ripple adds surface_offset twice (10 bytes for 2-entry frames) */
            smap += surface_offset + surface_offset;
            queue_sprite_draw(smap, x, y, priority, SMAP_BANK_OVERLAY);
        } else {
            /* Small ripple (for entities with byte_widths == 0x40) */
            if (entities.ripple_next_update[entity_offset] == 0) {
                uint16_t new_ptr;
                uint16_t delay = execute_overlay_animation_script(
                    entities.ripple_overlay_ptrs[entity_offset],
                    entities.ripple_spritemaps,
                    entity_offset, &new_ptr);
                entities.ripple_overlay_ptrs[entity_offset] = new_ptr;
                entities.ripple_next_update[entity_offset] = delay;
            }
            entities.ripple_next_update[entity_offset]--;

            int16_t x = entities.screen_x[entity_offset];
            int16_t y = entities.screen_y[entity_offset];
            uint16_t smap = entities.ripple_spritemaps[entity_offset] + surface_offset;
            queue_sprite_draw(smap, x, y, priority, SMAP_BANK_OVERLAY);
        }
    }

    /* Check for overlay flags (sweating / mushroomized) */
    uint8_t oflags = entities.overlay_flags[entity_offset];
    if (oflags == 0)
        return;

    /* Bit 1 = sweating/nausea */
    if (!(oflags & 0x02))
        goto after_sweat;

draw_sweat:
    /* Only for entities at slot >= 23 (party members / important NPCs) */
    if (entity_offset < 23)
        return;

    if (entities.sweating_next_update[entity_offset] == 0) {
        uint16_t new_ptr;
        uint16_t delay = execute_overlay_animation_script(
            entities.sweating_overlay_ptrs[entity_offset],
            entities.sweating_spritemaps,
            entity_offset, &new_ptr);
        entities.sweating_overlay_ptrs[entity_offset] = new_ptr;
        entities.sweating_next_update[entity_offset] = delay;
    }
    entities.sweating_next_update[entity_offset]--;

    {
        int16_t x = entities.screen_x[entity_offset];
        int16_t y = entities.screen_y[entity_offset];
        uint16_t smap = entities.sweating_spritemaps[entity_offset];
        if (smap != 0) {  /* 0 = no frame (SHOWFRAME $0000 pause) */
            smap += surface_offset;
            queue_sprite_draw(smap, x, y, priority, SMAP_BANK_OVERLAY);
        }
    }

after_sweat:
    /* Bit 14 = mushroomized */
    if (!(entities.overlay_flags[entity_offset] & 0x01))
        return;
    if (entity_offset < 23)
        return;

    if (entities.mushroomized_next_update[entity_offset] == 0) {
        uint16_t new_ptr;
        uint16_t delay = execute_overlay_animation_script(
            entities.mushroomized_overlay_ptrs[entity_offset],
            entities.mushroomized_spritemaps,
            entity_offset, &new_ptr);
        entities.mushroomized_overlay_ptrs[entity_offset] = new_ptr;
        entities.mushroomized_next_update[entity_offset] = delay;
    }
    entities.mushroomized_next_update[entity_offset]--;

    {
        int16_t x = entities.screen_x[entity_offset];
        int16_t y = entities.screen_y[entity_offset];
        uint16_t smap = entities.mushroomized_spritemaps[entity_offset] + surface_offset;
        queue_sprite_draw(smap, x, y, priority, SMAP_BANK_OVERLAY);
    }
}

/*
 * DRAW_ENTITY_SPRITE — general overworld entity draw callback.
 *
 * Reads the entity's spritemap data from the OVERWORLD_SPRITEMAPS ert.buffer
 * and queues it for OAM rendering. Also triggers per-frame VRAM tile
 * upload via render_entity_sprite().
 *
 * Port of the draw path in DISPATCH_SPRITE_DRAW_BY_PRIORITY (asm/overworld/entity/dispatch_sprite_draw_by_priority.asm)
 * for general entities with direction-based spritemaps.
 *
 * The entity's spritemap_ptr_lo stores the byte offset into overworld_spritemaps[].
 * The spritemap_sizes stores (sprites_per_frame × 5) — bytes per direction.
 * Direction selects which chunk of spritemaps to use:
 *   offset = spritemap_ptr_lo + direction_index × spritemap_sizes
 */
static void draw_entity_sprite(int16_t entity_offset) {
    /* Assembly's DRAW_ENTITY_SPRITE does NOT call RENDER_ENTITY_SPRITE.
     * Tile uploads happen during the tick phase only (via update_entity_sprite
     * or render_entity_sprite called from entity tick callbacks). */

    uint16_t smap_base = entities.spritemap_ptr_lo[entity_offset];
    uint16_t smap_size = entities.spritemap_sizes[entity_offset];

    /* Assembly has no early return for smap_size == 0; it continues to
     * draw_entity_overlays. When smap_size == 0, dir_idx * 0 = 0 (harmless). */

    /* ASM (DRAW_ENTITY_SPRITE, C0A3A4 lines 3-9):
     * LDA ENTITY_CURRENT_DISPLAYED_SPRITES,X / AND #$0001 / BEQ @SKIP
     * If bit 0 is set, advance spritemap pointer by one chunk size.
     * This is the data-driven direction→spritemap chunk mapping:
     * the sprite_grouping tile pointer low bit indicates which of the
     * 2 loaded spritemap direction chunks to use. */
    uint16_t dir_idx = entities.current_displayed_sprites[entity_offset] & 0x0001;

    uint16_t smap_offset = smap_base + dir_idx * smap_size;
    if (smap_offset >= OVERWORLD_SPRITEMAPS_SIZE)
        return;

    /* Port of DRAW_ENTITY_SPRITE (C0A3A4) lines 11-58:
     * Override OAM priority bits (bits 5:4 of attr byte) in the spritemap
     * ert.buffer in-place. Default priority = 3 (0x30). Surface flags can
     * reduce to priority 2 (0x20) for upper/lower body halves. */
    {
        uint8_t upper_prio = 0x30;  /* OAM priority 3 */
        uint8_t lower_prio = 0x30;
        uint16_t sf = entities.surface_flags[entity_offset];
        if (sf & 1) lower_prio = 0x20;  /* Priority 2 */
        if (sf & 2) upper_prio = 0x20;

        uint16_t divide = entities.upper_lower_body_divides[entity_offset];
        uint8_t upper_count = (uint8_t)(divide >> 8);
        uint8_t lower_count = (uint8_t)(divide & 0xFF);

        uint16_t off = smap_offset;
        for (uint8_t i = 0; i < upper_count && off + 4 < OVERWORLD_SPRITEMAPS_SIZE; i++) {
            overworld_spritemaps[off + 2] = (overworld_spritemaps[off + 2] & 0xCF) | upper_prio;
            off += SPRITEMAP_ENTRY_SIZE;
        }
        for (uint8_t i = 0; i < lower_count && off + 4 < OVERWORLD_SPRITEMAPS_SIZE; i++) {
            overworld_spritemaps[off + 2] = (overworld_spritemaps[off + 2] & 0xCF) | lower_prio;
            off += SPRITEMAP_ENTRY_SIZE;
        }
    }

    /* Priority delegation: if bit 15 is set, extract slot from bits 0-5
     * and use that other entity's draw priority instead.
     * If bit 14 is clear, clear own priority (one-shot delegation). */
    uint16_t priority = (uint16_t)entities.draw_priority[entity_offset];
    if (priority & 0x8000) {
        uint16_t delegate_offset = (priority & 0x003F);
        if (delegate_offset >= MAX_ENTITIES)
            FATAL("draw_priority delegate offset %u out of bounds (ent=%d)\n",
                  delegate_offset, entity_offset);
        {
            priority = (uint16_t)entities.draw_priority[delegate_offset];
        }
        if (!(entities.draw_priority[entity_offset] & 0x4000)) {
            entities.draw_priority[entity_offset] = 0;
        }
    }
    draw_entity_overlays(entity_offset, priority);

    int16_t x = entities.screen_x[entity_offset];
    int16_t y = entities.screen_y[entity_offset];

    LOG_TRACE("draw_entity_sprite: ent=%d smap_base=%u smap_size=%u dir_idx=%u "
              "smap_off=%u pos=(%d,%d) abs=(%d,%d) prio=%u disp=0x%04X\n",
              entity_offset, smap_base, smap_size, dir_idx, smap_offset,
              x, y, entities.abs_x[entity_offset], entities.abs_y[entity_offset],
              priority, entities.current_displayed_sprites[entity_offset]);
    queue_sprite_draw(smap_offset, x, y, priority, SMAP_BANK_OVERWORLD);
}

/*
 * DRAW_TITLE_LETTER — title screen letter draw callback.
 *
 * Reads the animation pointer table to find the spritemap for the current
 * animation frame, then dispatches to the priority sprite draw queue.
 */
static void draw_title_letter(int16_t entity_offset) {
    int16_t frame = entities.animation_frame[entity_offset];
    if (frame < 0 || frame >= ANIMATION_FRAME_COUNT)
        return;

    uint16_t priority = (uint16_t)entities.draw_priority[entity_offset];
    int16_t x = entities.screen_x[entity_offset];
    int16_t y = entities.screen_y[entity_offset];

    queue_sprite_draw((uint16_t)frame, x, y, priority, SMAP_BANK_TITLE);
}

/*
 * RENDER_ALL_PRIORITY_SPRITES (C08B8E)
 *
 * Flushes all priority queues to OAM, in order from priority 0 (back) to 3 (front).
 * Supports both title screen spritemaps (bank=0) and overworld spritemaps (bank=1).
 */
void render_all_priority_sprites(void) {
    for (int prio = 0; prio < 4; prio++) {
        SpritePriorityQueue *q = &sprite_priority[prio];
        for (uint16_t i = 0; i < q->offset; i++) {
            uint16_t smap_id = q->spritemaps[i];
            uint16_t bank = q->spritemap_banks[i];

            const uint8_t *spritemap = NULL;

            if (bank == SMAP_BANK_TITLE) {
                /* Title screen: smap_id is a frame index */
                if (smap_id < ANIMATION_FRAME_COUNT && title_spritemap_data) {
                    spritemap = title_spritemap_data + title_spritemap_offsets[smap_id];
                }
            } else if (bank == SMAP_BANK_OVERWORLD) {
                /* Overworld entity: smap_id is byte offset into overworld_spritemaps[] */
                if (smap_id < OVERWORLD_SPRITEMAPS_SIZE) {
                    spritemap = overworld_spritemaps + smap_id;
                }
            } else if (bank == SMAP_BANK_OVERLAY) {
                /* Overlay spritemap: smap_id is byte offset into overlay_data[] */
                if (smap_id < overlay_data_size) {
                    spritemap = overlay_data + smap_id;
                }
            } else if (bank == SMAP_BANK_TOWNMAP) {
                /* Town map icon spritemap: smap_id is byte offset into town_map_spritemap_data[] */
                if (smap_id < town_map_spritemap_data_size) {
                    spritemap = town_map_spritemap_data + smap_id;
                }
            }

            if (spritemap) {
                write_spritemap_to_oam(spritemap,
                                       q->sprite_x[i], q->sprite_y[i]);
            }
        }
    }
}

/*
 * Call an entity's draw callback.
 *
 * Port of CALL_ENTITY_DRAW_CALLBACK (C0A0CA) → DRAW_ENTITY (C0A0E3).
 * The assembly dispatches through the ENTITY_DRAW_CALLBACK jump table.
 */
static void call_entity_draw(int16_t entity_offset) {
    /* DRAW_ENTITY checks: spritemap_ptr_hi bit 15 (BMI @SKIP) = hidden.
     * The BVS @SKIP that follows never branches because LDA doesn't set V;
     * V comes from a prior SBC which is always clear for normal DP values.
     * Assembly (C0A0E3.asm lines 3-5): LDA / BMI / BVS */
    if (entities.spritemap_ptr_hi[entity_offset] & 0x8000) {
        LOG_TRACE("call_entity_draw: ent=%d HIDDEN (spritemap_ptr_hi=0x%04X)\n",
                  entity_offset, entities.spritemap_ptr_hi[entity_offset]);
        return;
    }
    if (entities.animation_frame[entity_offset] < 0) {
        LOG_TRACE("call_entity_draw: ent=%d SKIPPED (animation_frame=%d)\n",
                  entity_offset, entities.animation_frame[entity_offset]);
        return;
    }

    switch (entities.draw_callback[entity_offset]) {
    case CB_DRAW_ENTITY_SPRITE:
        draw_entity_sprite(entity_offset);
        break;
    case CB_DRAW_TITLE_LETTER:
        draw_title_letter(entity_offset);
        break;
    default:
        break;
    }
}

/*
 * BUILD_ENTITY_DRAW_LIST (C0DB0F)
 *
 * Faithful port of the assembly's two-pass entity draw system:
 *
 * Pass 1: Walk the entity linked list. Entities with draw_priority == 1 are
 *   collected into a singly-linked sorting list (via draw_sorting[], mirroring
 *   ENTITY_DRAW_SORTING in RAM). All other priorities are drawn immediately.
 *
 * Pass 2: Selection sort — repeatedly find the entity with the highest ABS_Y,
 *   draw it (giving it the lowest OAM index = highest SNES sprite priority =
 *   drawn on top), then remove it from the list. The >= comparison means that
 *   for equal Y, the entity found later in the list wins.
 *
 * This Y-sorting gives correct depth ordering for a top-down RPG perspective:
 * entities lower on screen (higher Y) appear in front.
 */
void build_entity_draw_list(void) {
    /* NOTE: Assembly checks controller 2 SELECT button here and calls
     * SORT_ENTITY_DRAW_ORDER (debug draw order) if pressed, bypassing
     * the normal Y-based depth sort below. Not yet ported (debug feature). */

    /* Y-sorting linked list for priority-1 entities.
     * Mirrors ENTITY_DRAW_SORTING in assembly.
     * Indexed by entity slot; values are entity slot (== offset) or -1. */
    int16_t draw_sorting[MAX_ENTITIES];
    int16_t sort_head = -1;  /* @LOCAL04 in assembly */

    /* Pass 1: Walk entity linked list.
     * Priority-1 entities are collected into the sorting linked list.
     * All other priorities are drawn immediately via call_entity_draw(). */
    int16_t ent = entities.first_entity;
    while (ent >= 0) {
        /* Screen bounds check (assembly lines 23-41).
         * Assembly uses unsigned CMP + BCC, so we must use uint16_t.
         * On-screen range: [0, VIEWPORT_HEIGHT) ∪ [0xFFC0, 0xFFFF] (y < VH or y >= -64).
         * Off-screen: [VIEWPORT_HEIGHT, 0xFFBF] — entities in this range are skipped.
         * Same logic for X with threshold VIEWPORT_WIDTH+64. */
        uint16_t sy = (uint16_t)entities.screen_y[ent];
        uint16_t sx = (uint16_t)entities.screen_x[ent];
        if ((sy < (VIEWPORT_HEIGHT + 32) || sy >= 0xFFC0) &&
            (sx < (VIEWPORT_WIDTH + 64) || sx >= 0xFFC0)) {
            if (entities.draw_priority[ent] == 1) {
                /* Priority 1: add to sorting linked list (front-insert).
                 * Assembly lines 51-54: ENTITY_DRAW_SORTING[offset] = prev head;
                 *                       head = current slot */
                draw_sorting[ent] = sort_head;
                sort_head = ent;
            } else {
                /* Other priorities: draw immediately (assembly line 57-58) */
                call_entity_draw(ent);
            }
        }
        ent = entities.next_entity[ent];
    }

    /* Pass 2: Selection sort by descending ABS_Y (assembly lines 72-131).
     * Draw entities with highest Y first → lowest OAM index → on top. */
    while (sort_head >= 0) {
        /* Initialize best to head of list */
        int16_t best_slot = sort_head;
        int16_t best_y = entities.abs_y[ENT(sort_head)];
        int16_t prev_of_best = -1;  /* @VIRTUAL04 in assembly */
        int16_t prev_slot = sort_head;  /* @VIRTUAL02 in assembly */

        /* Traverse from second element onward */
        int16_t cur_slot = draw_sorting[ENT(sort_head)];
        while (cur_slot >= 0) {
            int16_t cur_y = entities.abs_y[ENT(cur_slot)];
            /* Assembly uses CMP + BCC = unsigned >= comparison */
            if ((uint16_t)cur_y >= (uint16_t)best_y) {
                best_y = cur_y;
                best_slot = cur_slot;
                prev_of_best = prev_slot;
            }
            prev_slot = cur_slot;
            cur_slot = draw_sorting[ENT(cur_slot)];
        }

        /* Draw the entity with highest Y (assembly line 108) */
        call_entity_draw(best_slot);

        /* Remove best from linked list (assembly lines 109-127) */
        if (prev_of_best < 0) {
            /* Best was head: advance head (assembly @UNKNOWN11) */
            sort_head = draw_sorting[ENT(best_slot)];
        } else {
            /* Splice out: prev.next = best.next (assembly lines 112-121) */
            draw_sorting[ENT(prev_of_best)] = draw_sorting[ENT(best_slot)];
        }
    }

}

/* ==================================================================
 * Entity fade state system — wipe/dissolve effects on entity spawn/despawn.
 *
 * Ports of:
 *   INIT_ENTITY_FADE_STATE         (asm/overworld/entity/init_entity_fade_state.asm)
 *   INIT_ENTITY_FADE_STATES_BUFFER (asm/overworld/entity/init_entity_fade_states_buffer.asm)
 *   DOUBLE_ENTITY_FADE_STATE       (asm/overworld/entity/double_entity_fade_state.asm)
 *   CLEAR_BUFFER_RANGE             (asm/misc/clear_buffer_range.asm)
 *   COPY_8DIR_ENTITY_SPRITE        (asm/overworld/entity/copy_8dir_entity_sprite.asm)
 *   COPY_4DIR_ENTITY_SPRITE        (asm/overworld/entity/copy_4dir_entity_sprite.asm)
 * ================================================================== */

/* ENTITY_TILE_BYTE_WIDTH_TABLE (C42A63): maps entity size code to tile row
 * byte width. ENTITY_SIZE_COUNT entries matching the assembly .WORD table. */
static const uint16_t entity_tile_byte_width_table[ENTITY_SIZE_COUNT] = {
    0x0010, 0x0010, 0x0020, 0x0020, 0x0030, 0x0010, 0x0018, 0x0010,
    0x0020, 0x0030, 0x0018, 0x0010, 0x0020, 0x0030, 0x0040, 0x0040,
    0x0040
};

/* Fade state entries — derived from ert.buffer, reconstructed at init */
static EntityFadeEntry *entity_fade_entries;  /* Pointer to fade entry array at ert.buffer[0x7C00] */

/*
 * INIT_ENTITY_FADE_STATES_BUFFER (C4C8A4)
 * Zeros the 1KB fade states area at BUFFER+$7C00 and resets counters.
 */
static void init_entity_fade_states_buffer(void) {
    ert.entity_fade_states_buffer = 0;
    ert.entity_fade_states_length = 0;
    entity_fade_entries = buf_entity_fade_entries(ert.buffer);
    memset(entity_fade_entries, 0, BUF_ENTITY_FADE_STATES_SIZE);
}

/*
 * DOUBLE_ENTITY_FADE_STATE (C4C8DB)
 * Allocates alloc_size bytes from the fade states ert.buffer.
 * Returns the old ert.buffer offset (before allocation).
 */
static uint16_t double_entity_fade_state(uint16_t alloc_size) {
    uint16_t old = ert.entity_fade_states_buffer;
    ert.entity_fade_states_buffer += alloc_size;
    return old;
}

/*
 * COPY_8DIR_ENTITY_SPRITE (C4283F)
 * Copies the current sprite tile data for an 8-directional entity into ert.buffer[].
 * entity_slot: entity slot number (0-29)
 * dest_offset: byte offset into ert.buffer[] for the destination
 * copy_size:   number of bytes to copy (total_sprite_size from fade state)
 */
static void copy_8dir_entity_sprite(uint16_t entity_slot, uint16_t dest_offset,
                                     uint16_t copy_size) {
    uint16_t ent = entity_slot;

    /* Look up direction mapping for 8-directional entities */
    uint16_t direction = (uint16_t)entities.directions[ent];
    if (direction >= 8) return;
    uint16_t dir_mapped = sprite_direction_mapping_8dir[direction];

    /* Compute pointer into spritepointerarray:
     * gfx_ptr_lo + (dir_mapped * 4) + animation_frame → frame table entry */
    uint16_t gfx_ptr_lo = entities.graphics_ptr_lo[ent];
    int16_t anim_frame = entities.animation_frame[ent];
    uint32_t frame_ptr_off = (uint32_t)gfx_ptr_lo + (uint32_t)dir_mapped * 4
                             + (uint32_t)(anim_frame > 0 ? 2 : 0);

    if (frame_ptr_off + 2 > sprite_grouping_data_size) return;

    /* Read within-bank tile data offset, mask flags (8-dir: AND $FFFE) */
    uint16_t tile_data_offset = read_u16_le(sprite_grouping_data_buf + frame_ptr_off);
    tile_data_offset &= 0xFFF0;  /* Assembly uses AND #$FFF0 for copy functions */

    /* Resolve sprite bank */
    uint16_t bank_num = entities.graphics_sprite_bank[ent];
    int bank_idx = (int)(bank_num & 0x3F) - SPRITE_BANK_FIRST;
    if (bank_idx < 0 || bank_idx >= SPRITE_BANK_COUNT) return;
    if (!sprite_banks[bank_idx]) return;

    const uint8_t *bank_data = sprite_banks[bank_idx];
    uint32_t bank_size = sprite_bank_sizes[bank_idx];

    if ((uint32_t)tile_data_offset + copy_size > bank_size) return;
    if ((uint32_t)dest_offset + copy_size > BUFFER_SIZE) return;

    memcpy(&ert.buffer[dest_offset], bank_data + tile_data_offset, copy_size);
}

/*
 * COPY_4DIR_ENTITY_SPRITE (C42884)
 * Copies the current sprite tile data for a 4-directional entity into ert.buffer[].
 * entity_slot: entity slot number (0-29)
 * dest_offset: byte offset into ert.buffer[] for the destination
 * copy_size:   number of bytes to copy (total_sprite_size from fade state)
 */
static void copy_4dir_entity_sprite(uint16_t entity_slot, uint16_t dest_offset,
                                     uint16_t copy_size) {
    uint16_t ent = entity_slot;

    /* Look up direction mapping for 4-directional entities */
    uint16_t direction = (uint16_t)entities.directions[ent];
    if (direction >= 12) return;
    uint16_t dir_mapped = sprite_direction_mapping_4dir[direction];

    /* Compute pointer: gfx_ptr_lo + dir_mapped * 4 */
    uint16_t gfx_ptr_lo = entities.graphics_ptr_lo[ent];
    uint32_t frame_ptr_off = (uint32_t)gfx_ptr_lo + (uint32_t)dir_mapped * 4;

    if (frame_ptr_off + 2 > sprite_grouping_data_size) return;

    /* Read within-bank tile data offset, mask flags (4-dir: AND $FFF0) */
    uint16_t tile_data_offset = read_u16_le(sprite_grouping_data_buf + frame_ptr_off);
    tile_data_offset &= 0xFFF0;

    /* Resolve sprite bank */
    uint16_t bank_num = entities.graphics_sprite_bank[ent];
    int bank_idx = (int)(bank_num & 0x3F) - SPRITE_BANK_FIRST;
    if (bank_idx < 0 || bank_idx >= SPRITE_BANK_COUNT) return;
    if (!sprite_banks[bank_idx]) return;

    const uint8_t *bank_data = sprite_banks[bank_idx];
    uint32_t bank_size = sprite_bank_sizes[bank_idx];

    if ((uint32_t)tile_data_offset + copy_size > bank_size) return;
    if ((uint32_t)dest_offset + copy_size > BUFFER_SIZE) return;

    memcpy(&ert.buffer[dest_offset], bank_data + tile_data_offset, copy_size);
}

/*
 * INIT_ENTITY_FADE_STATE (C4C91A)
 *
 * Sets up a fade/wipe effect for an entity. Allocates a 20-byte fade state
 * entry and copies the entity's current sprite data into a ert.buffer.
 * A special entity (EVENT_ENTITY_WIPE, script 859) is spawned to animate the fade over time.
 *
 * Parameters:
 *   entity_slot: entity slot number (0-29), the entity being faded
 *   fade_param:  type of fade effect:
 *     0, 1, 6 = no-op (early exit)
 *     2 = wipe up,    3 = wipe down,  4 = wipe left,  5 = wipe right
 *     7 = reveal up,  8 = reveal down, 9 = reveal left, 10 = reveal right
 *
 * Fade state entry (20 bytes):
 *   [0]  entity_slot
 *   [2]  fade_param
 *   [4]  direction code (1=up, 2=down, 3=left, 4=right)
 *   [6]  tile_byte_width
 *   [8]  height_in_pixels (tile_heights * 8)
 *   [10] buffer_offset_1 (first sprite copy)
 *   [12] buffer_offset_2 (second sprite copy, = offset_1 + sprite_size)
 *   [14] total_sprite_size (width * height / 2)
 *   [16] core.frame_counter_1 (init 0)
 *   [18] core.frame_counter_2 (init 0)
 */
void init_entity_fade_state(uint16_t entity_slot, uint16_t fade_param) {
    /* Assembly lines 19-26: no-op for params 0, 1, or 6 */
    if (fade_param == 0 || fade_param == 1 || fade_param == 6)
        return;

    /* Assembly lines 27-31: check entity has non-zero tile height */
    uint16_t ent = entity_slot;
    if (entities.tile_heights[ent] == 0)
        return;

    /* Assembly lines 32-44: spawn fade entity if it doesn't exist yet */
    if (ow.entity_fade_entity == -1) {
        init_entity_fade_states_buffer();
        ert.new_entity_var[0] = 0;
        ert.new_entity_var[1] = 0;
        ert.new_entity_var[2] = 0;
        ert.new_entity_var[3] = 0;
        int16_t result = entity_init_wipe(EVENT_SCRIPT_ENTITY_WIPE);
        if (result < 0)
            return;
        ow.entity_fade_entity = result;  /* C port returns offset == slot */
    }

    /* Assembly lines 46-52: compute pointer to this fade state entry */
    EntityFadeEntry *entry = &entity_fade_entries[ert.entity_fade_states_length];

    /* Assembly lines 55-58: entry.entity_slot = entity_slot */
    entry->entity_slot = entity_slot;

    /* Assembly lines 63-69: set bit 14 of ENTITY_SPRITEMAP_POINTER_HIGH to hide entity */
    entities.spritemap_ptr_hi[ent] |= 0x4000;

    /* Assembly lines 70-72: entry.fade_param = fade_param */
    entry->fade_param = fade_param;

    /* Assembly lines 73-75: look up entity size */
    uint16_t size_code = entities.sizes[ent];

    /* Assembly lines 77-85: entry.tile_byte_width from lookup table */
    uint16_t tile_byte_width = 0;
    if (size_code < ENTITY_SIZE_COUNT)
        tile_byte_width = entity_tile_byte_width_table[size_code];
    entry->tile_byte_width = tile_byte_width;

    /* Assembly lines 86-92: entry.height_pixels = tile_heights * 8 */
    uint16_t height_pixels = entities.tile_heights[ent] * 8;
    entry->height_pixels = height_pixels;

    /* Assembly lines 93-107: entry.total_sprite_size = (width * height) / 2 */
    uint16_t total_sprite_size = (uint16_t)((uint32_t)tile_byte_width * height_pixels / 2);
    entry->total_sprite_size = total_sprite_size;

    /* Assembly lines 108-122: allocate ert.buffer space and clear it
     * Allocate total_sprite_size * 2 bytes for both sprite copies */
    uint16_t buf_offset_1 = double_entity_fade_state(total_sprite_size * 2);
    entry->buf_offset_1 = buf_offset_1;

    /* Clear the allocated region in the main ert.buffer */
    if (buf_offset_1 + total_sprite_size * 2 <= BUFFER_SIZE)
        memset(&ert.buffer[buf_offset_1], 0, total_sprite_size * 2);

    /* Assembly lines 123-137: entry.buf_offset_2 = buf_offset_1 + total_sprite_size */
    entry->buf_offset_2 = buf_offset_1 + total_sprite_size;

    /* Assembly lines 138-142: entry.frame_counter_1 = 0, entry.frame_counter_2 = 0 */
    entry->frame_counter_1 = 0;
    entry->frame_counter_2 = 0;

    /* Assembly lines 143-183: select destination ert.buffer and copy sprite data.
     * Types 2-5 (wipe): copy to buf_offset_1 (first half)
     * Types 7-10 (reveal): copy to buf_offset_2 (second half) */
    uint16_t copy_dest;
    if (fade_param >= 2 && fade_param <= 5) {
        copy_dest = buf_offset_1;  /* wipe: sprite starts visible */
    } else {
        copy_dest = buf_offset_1 + total_sprite_size;  /* reveal: sprite starts hidden */
    }

    /* Assembly lines 165-183: choose 8-dir or 4-dir copy based on entity_slot >= 24 */
    if (entity_slot >= 24) {
        copy_8dir_entity_sprite(entity_slot, copy_dest, total_sprite_size);
    } else {
        copy_4dir_entity_sprite(entity_slot, copy_dest, total_sprite_size);
    }

    /* Assembly lines 185-253: set direction-based script variables on the fade entity.
     * The fade entity's var0-var3 track how many wipes are active per direction.
     * var4 = total count of active directions. */
    int16_t fade_ent = ow.entity_fade_entity;
    uint16_t dir_code = 0;

    switch (fade_param) {
    case 2: case 7:  /* up */
        entities.var[0][fade_ent] = 1;  /* Assembly: LDA #1; STA ENTITY_SCRIPT_VAR0 */
        dir_code = 1;
        break;
    case 3: case 8:  /* down */
        entities.var[1][fade_ent] = 1;  /* Assembly: LDA #1; STA ENTITY_SCRIPT_VAR1 */
        dir_code = 2;
        break;
    case 4: case 9:  /* left */
        entities.var[2][fade_ent] = 1;  /* Assembly: LDA #1; STA ENTITY_SCRIPT_VAR2 */
        dir_code = 3;
        break;
    case 5: case 10: /* right */
        entities.var[3][fade_ent] = 1;  /* Assembly: LDA #1; STA ENTITY_SCRIPT_VAR3 */
        dir_code = 4;
        break;
    }

    /* Assembly line 212/222/232/242: entry.direction = direction code */
    entry->direction = dir_code;

    /* Assembly lines 243-254: var4 = sum of all direction counts */
    entities.var[4][fade_ent] = entities.var[0][fade_ent]
                              + entities.var[1][fade_ent]
                              + entities.var[2][fade_ent]
                              + entities.var[3][fade_ent];

    /* Assembly line 255: increment total fade state count */
    ert.entity_fade_states_length++;
}

/* ==================================================================
 * Sprite tile data manipulation helpers for entity fade animations.
 *
 * These operate on sprite data stored in ert.buffer[] (bank $7F in assembly).
 * SNES 4bpp tiles: each 8x8 tile = 32 bytes.
 *   Bytes 0-15: bitplanes 0,1 (8 rows × 2 bytes)
 *   Bytes 16-31: bitplanes 2,3 (8 rows × 2 bytes)
 *
 * Ports of:
 *   COPY_SPRITE_TILE_DATA       (asm/overworld/entity/copy_sprite_tile_data.asm)
 *   BLEND_SPRITE_TILE_DATA      (asm/overworld/entity/blend_sprite_tile_data.asm)
 *   MERGE_SPRITE_TILE_PAIR      (asm/overworld/entity/merge_sprite_tile_pair.asm)
 *   UPLOAD_ENTITY_SPRITE_TO_VRAM (asm/overworld/entity/upload_entity_sprite_to_vram.asm)
 * ================================================================== */

/* SPRITE_PIXEL_BITMASK_TABLE (C42955): per-column bitmask for 4bpp tile words.
 * Each entry selects one pixel column across both bytes of a bitplane row. */
static const uint16_t sprite_pixel_bitmask_table[8] = {
    0x8080, 0x4040, 0x2020, 0x1010,
    0x0808, 0x0404, 0x0202, 0x0101
};

/*
 * COPY_SPRITE_TILE_DATA (C428D1)
 * Copies one pixel row of tile data from source to destination in ert.buffer[].
 * Copies tile_columns * 2 blocks of 16 bytes (covers both bitplane groups
 * for each tile column).
 *
 * Parameters:
 *   dest_off:     byte offset in ert.buffer[] for destination
 *   src_off:      byte offset in ert.buffer[] for source
 *   y_offset:     starting byte offset within tile data (selects pixel row)
 *   tile_columns: number of 8-pixel tile columns
 */
static void copy_sprite_tile_data(uint16_t dest_off, uint16_t src_off,
                                   uint16_t y_offset, uint16_t tile_columns) {
    uint16_t count = tile_columns * 2;  /* ASL: two 16-byte blocks per tile column */
    uint16_t y = y_offset;
    for (uint16_t i = 0; i < count; i++) {
        uint32_t d = (uint32_t)dest_off + y;
        uint32_t s = (uint32_t)src_off + y;
        if (d + 2 <= BUFFER_SIZE && s + 2 <= BUFFER_SIZE) {
            ert.buffer[d] = ert.buffer[s];
            ert.buffer[d + 1] = ert.buffer[s + 1];
        }
        y += 0x10;
    }
}

/*
 * BLEND_SPRITE_TILE_DATA (C428FC)
 * Blends one pixel column from source into destination across all tile rows.
 * For each tile row, copies one pixel column (selected by bitmask) from
 * source to destination across all 32 bytes of the tile.
 *
 * Parameters:
 *   dest_off:       byte offset in ert.buffer[] for destination
 *   src_off:        byte offset in ert.buffer[] for source
 *   y_param:        combined pixel column + tile offset (assembly Y register):
 *                     low 3 bits = pixel column within tile (0-7)
 *                     upper bits = tile column index × 8
 *                   So (y_param & 7) selects pixel, (y_param & ~7) * 4 = tile byte offset
 *   height_pixels:  total height in pixels (from caller's @LOCAL00)
 *   row_stride:     bytes per tile row (from caller's @LOCAL01)
 */
static void blend_sprite_tile_data(uint16_t dest_off, uint16_t src_off,
                                    uint16_t y_param,
                                    uint16_t height_pixels, uint16_t row_stride) {
    uint16_t mask = sprite_pixel_bitmask_table[y_param & 7];
    uint16_t inv_mask = mask ^ 0xFFFF;

    /* Y = (y_param & ~7) * 4 = byte offset to start of tile column */
    uint16_t y = (y_param & 0xFFF8) * 4;
    uint16_t num_rows = height_pixels >> 3;  /* number of tile rows */

    for (uint16_t row = 0; row < num_rows; row++) {
        /* Process 16 words (32 bytes) = one complete tile */
        for (uint16_t w = 0; w < 16; w++) {
            uint32_t off = (uint32_t)y + w * 2;
            uint32_t s = (uint32_t)src_off + off;
            uint32_t d = (uint32_t)dest_off + off;
            if (s + 2 > BUFFER_SIZE || d + 2 > BUFFER_SIZE)
                continue;
            uint16_t src_word = read_u16_le(&ert.buffer[s]);
            uint16_t dst_word = read_u16_le(&ert.buffer[d]);
            uint16_t result = (dst_word & inv_mask) | (src_word & mask);
            write_u16_le(&ert.buffer[d], result);
        }
        y += row_stride;
    }
}

/*
 * MERGE_SPRITE_TILE_PAIR (C42965)
 * Merges one pixel column for two consecutive 16-byte bitplane groups.
 * Used by the random dissolve effect (ANIMATE_ENTITY_TILE_MERGE).
 *
 * Parameters:
 *   dest_off:  byte offset in ert.buffer[] for destination
 *   src_off:   byte offset in ert.buffer[] for source
 *   y_offset:  byte offset to the first bitplane group
 *   pixel_col: pixel column (0-7)
 */
static void merge_sprite_tile_pair(uint16_t dest_off, uint16_t src_off,
                                    uint16_t y_offset, uint16_t pixel_col) {
    uint16_t mask = sprite_pixel_bitmask_table[pixel_col & 7];
    uint16_t inv_mask = mask ^ 0xFFFF;

    /* Process two 16-byte halves (bitplanes 0,1 then 2,3) */
    for (int half = 0; half < 2; half++) {
        uint32_t off = (uint32_t)y_offset + half * 0x10;
        uint32_t s = (uint32_t)src_off + off;
        uint32_t d = (uint32_t)dest_off + off;
        if (s + 2 > BUFFER_SIZE || d + 2 > BUFFER_SIZE)
            continue;
        uint16_t src_word = read_u16_le(&ert.buffer[s]);
        uint16_t dst_word = read_u16_le(&ert.buffer[d]);
        uint16_t result = (dst_word & inv_mask) | (src_word & mask);
        write_u16_le(&ert.buffer[d], result);
    }
}

/*
 * UPLOAD_ENTITY_SPRITE_TO_VRAM (C429AE)
 * Copies entity sprite data from ert.buffer[] to ppu.vram[].
 * Uses the entity's tile_heights, byte_widths, and vram_address.
 *
 * Parameters:
 *   entity_slot: entity slot number (0-29)
 *   src_offset:  byte offset in ert.buffer[] for source data
 */
static void upload_entity_sprite_to_vram(uint16_t entity_slot,
                                          uint16_t src_offset) {
    uint16_t ent = entity_slot;
    uint16_t tile_height = entities.tile_heights[ent];
    uint16_t byte_width = entities.byte_widths[ent];
    uint16_t vram_addr = entities.vram_address[ent];
    uint16_t src = src_offset;

    for (uint16_t row = 0; row < tile_height; row++) {
        if ((uint32_t)src + byte_width > BUFFER_SIZE)
            break;
        vram_addr = vram_copy_row_safe(vram_addr, &ert.buffer[src], byte_width);
        src += byte_width;
    }
}

/* ==================================================================
 * Entity fade animation callroutines — called from EVENT_ENTITY_WIPE (859) script.
 *
 * Ports of:
 *   CLEAR_FADE_ENTITY_FLAGS    (asm/overworld/entity/clear_fade_entity_flags.asm)
 *   UPDATE_FADE_ENTITY_SPRITES (asm/overworld/entity/update_fade_entity_sprites.asm)
 *   HIDE_FADE_ENTITY_FRAMES    (asm/overworld/entity/hide_fade_entity_frames.asm)
 *   ANIMATE_ENTITY_TILE_COPY   (asm/overworld/entity/animate_entity_tile_copy.asm)
 *   ANIMATE_ENTITY_TILE_BLEND  (asm/overworld/entity/animate_entity_tile_blend.asm)
 *   ANIMATE_ENTITY_TILE_MERGE  (asm/overworld/entity/animate_entity_tile_merge.asm)
 * ================================================================== */

/*
 * CLEAR_FADE_ENTITY_FLAGS (C4CB4F)
 * Iterates all fade state entries and clears bit 14 of
 * ENTITY_SPRITEMAP_POINTER_HIGH for each referenced entity.
 * This makes the entity sprite visible again.
 */
void cr_clear_fade_entity_flags(void) {
    for (uint16_t i = 0; i < ert.entity_fade_states_length; i++) {
        EntityFadeEntry *entry = &entity_fade_entries[i];
        uint16_t ent = entry->entity_slot;
        entities.spritemap_ptr_hi[ent] &= 0xBFFF;  /* AND #$BFFF: clear bit 14 */
    }
}

/*
 * UPDATE_FADE_ENTITY_SPRITES (C4CB8F)
 * Iterates all fade state entries. For those with direction_code==1,
 * resets animation_frame to 0. Then re-renders each entity's sprite.
 */
void cr_update_fade_entity_sprites(void) {
    for (uint16_t i = 0; i < ert.entity_fade_states_length; i++) {
        EntityFadeEntry *entry = &entity_fade_entries[i];
        uint16_t ent = entry->entity_slot;
        if (entry->direction == 1) {
            entities.animation_frame[ent] = 0;
        }
        render_entity_sprite(ent);
    }
}

/*
 * HIDE_FADE_ENTITY_FRAMES (C4CBE3)
 * Iterates all fade state entries. For those with direction_code==1,
 * sets animation_frame to -1 (effectively hiding the entity sprite).
 */
void cr_hide_fade_entity_frames(void) {
    for (uint16_t i = 0; i < ert.entity_fade_states_length; i++) {
        EntityFadeEntry *entry = &entity_fade_entries[i];
        uint16_t ent = entry->entity_slot;
        if (entry->direction == 1) {
            entities.animation_frame[ent] = (int16_t)-1;
        }
    }
}

/*
 * ANIMATE_ENTITY_TILE_COPY (C4CC2F)
 * Directional wipe: copies one pixel row per step from buffer_2 into buffer_1.
 * Active for fade entries with direction_code == 2.
 *
 * Uses core.frame_counter_1 (entry[16]) to track progress through pixel rows.
 * When all rows are copied, increments core.frame_counter_2 (entry[18]).
 * When core.frame_counter_2 reaches 2, the effect is complete.
 *
 * Returns: count of active entries minus count of completed entries.
 */
int16_t cr_animate_entity_tile_copy(void) {
    int16_t active_count = 0;
    int16_t done_count = 0;

    for (uint16_t i = 0; i < ert.entity_fade_states_length; i++) {
        EntityFadeEntry *entry = &entity_fade_entries[i];
        if (entry->direction != 2)
            continue;
        active_count++;

        /* Check if effect is already complete */
        if (entry->frame_counter_2 >= 2) {
            done_count++;
            continue;
        }

        /* Read frame counter and compute pixel row position */
        uint16_t frame_counter = entry->frame_counter_1;
        uint16_t tile_byte_width = entry->tile_byte_width;
        uint16_t height_pixels = entry->height_pixels;
        uint16_t tile_columns = tile_byte_width >> 3;

        /* Compute byte offset: tile_row * row_stride + pixel_row_offset */
        uint16_t pixel_row = frame_counter % 8;       /* MODULUS16 by 8 */
        uint16_t tile_row = frame_counter / 8;
        uint16_t row_stride = tile_columns * 32;       /* bytes per tile row */
        uint16_t y_offset = tile_row * row_stride + pixel_row * 2;

        /* Copy one pixel row from buffer_2 to buffer_1 */
        copy_sprite_tile_data(entry->buf_offset_2, entry->buf_offset_1,
                              y_offset, tile_columns);

        /* Upload merged sprite to VRAM */
        upload_entity_sprite_to_vram(entry->entity_slot, entry->buf_offset_2);

        /* Advance frame counter by 2 (one pixel row) */
        frame_counter += 2;
        entry->frame_counter_1 = frame_counter;

        /* Check if all pixel rows have been processed */
        if (frame_counter >= height_pixels) {
            entry->frame_counter_1 = 1;
            entry->frame_counter_2++;
        }
    }

    return active_count - done_count;
}

/*
 * ANIMATE_ENTITY_TILE_BLEND (C4CD44)
 * Two-phase pixel column blend. Active for entries with direction_code == 3.
 *
 * Phase 0: blend pixel columns from buffer_1 into buffer_2 (even/odd step pattern)
 * Phase 1: blend pixel columns in reverse direction
 * When both phases complete, core.frame_counter_2 reaches 2.
 *
 * The blend alternates between two column positions based on step parity:
 *   Even step: column = step_number
 *   Odd step:  column = tile_byte_width - step_number - 1
 *
 * Returns: count of active entries minus count of completed entries.
 */
int16_t cr_animate_entity_tile_blend(void) {
    int16_t active_count = 0;
    int16_t done_count = 0;

    for (uint16_t i = 0; i < ert.entity_fade_states_length; i++) {
        EntityFadeEntry *entry = &entity_fade_entries[i];
        if (entry->direction != 3)
            continue;
        active_count++;

        uint16_t phase = entry->frame_counter_2;
        if (phase >= 2) {
            done_count++;
            continue;
        }

        uint16_t step = entry->frame_counter_1;
        uint16_t tile_byte_width = entry->tile_byte_width;
        uint16_t height_pixels = entry->height_pixels;

        /* Determine blend column based on phase and step parity */
        uint16_t blend_col;
        if (phase == 0) {
            /* Phase 0: even steps use step directly, odd use mirror */
            if (step & 1) {
                blend_col = step;
            } else {
                blend_col = tile_byte_width - step - 1;
            }
        } else {
            /* Phase 1: odd steps use step directly, even use mirror */
            if (step & 1) {
                blend_col = tile_byte_width - step - 1;
            } else {
                blend_col = step;
            }
        }

        /* Compute tile row stride */
        uint16_t tile_columns = tile_byte_width >> 3;
        uint16_t row_stride = tile_columns * 32;

        /* Blend one pixel column from buf_offset_1 into buf_offset_2 */
        blend_sprite_tile_data(entry->buf_offset_2, entry->buf_offset_1,
                               blend_col, height_pixels, row_stride);

        /* Upload to VRAM */
        upload_entity_sprite_to_vram(entry->entity_slot, entry->buf_offset_2);

        /* Advance step */
        step++;
        entry->frame_counter_1 = step;

        /* Check if phase is complete (step >= tile_byte_width / 2) */
        if (step >= tile_byte_width / 2) {
            entry->frame_counter_2 = phase + 1;
            entry->frame_counter_1 = 0;
        }
    }

    return active_count - done_count;
}

/*
 * ANIMATE_ENTITY_TILE_MERGE (C4CED8)
 * Random pixel dissolve. Active for entries with direction_code == 4.
 *
 * Each call picks a random unused pixel position from a 64-entry slot table
 * at ert.buffer[0x7F00], then for each fade entry, merges that pixel from
 * buffer_1 into buffer_2 across all tile rows and columns.
 *
 * The 64-slot table tracks which of the 64 pixel positions (8 columns × 8 rows)
 * have been used. When a slot is marked, that pixel is blended across the
 * entire sprite for all active fade entries.
 */
void cr_animate_entity_tile_merge(void) {
    uint8_t *slot_table = &ert.buffer[BUF_TILE_MERGE_SLOTS];

    /* Pick a random unused slot from the 64-entry table */
    uint16_t slot_idx = rand() % 64;
    while (read_u16_le(slot_table + slot_idx * 2) != 0) {
        slot_idx = (slot_idx + 1) & 0x3F;
    }

    /* Mark slot as used */
    write_u16_le(slot_table + slot_idx * 2, 1);

    /* Decompose slot index into pixel column and tile pixel row */
    uint16_t pixel_row = slot_idx >> 3;
    uint16_t pixel_col = slot_idx % 8;  /* MODULUS16 by 8 */

    /* Iterate all fade entries with direction_code == 4 */
    for (uint16_t i = 0; i < ert.entity_fade_states_length; i++) {
        EntityFadeEntry *entry = &entity_fade_entries[i];
        if (entry->direction != 4)
            continue;

        uint16_t tile_columns = entry->tile_byte_width >> 3;
        uint16_t tile_rows = entry->height_pixels >> 3;

        /* For each tile row × tile column, merge the selected pixel.
         * Assembly: outer = tile_rows (height/8), inner = tile_columns (width/8).
         * y_offset = (tile_row * tile_columns + tile_col) * 32 + pixel_row * 2 */
        for (uint16_t tr = 0; tr < tile_rows; tr++) {
            for (uint16_t tc = 0; tc < tile_columns; tc++) {
                uint16_t y_offset = (tr * tile_columns + tc) * 32
                                    + pixel_row * 2;
                merge_sprite_tile_pair(entry->buf_offset_2, entry->buf_offset_1,
                                       y_offset, pixel_col);
            }
        }

        /* Upload to VRAM */
        upload_entity_sprite_to_vram(entry->entity_slot, entry->buf_offset_2);
    }
}
