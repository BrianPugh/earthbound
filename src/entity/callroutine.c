/*
 * CALLROUTINE dispatch — maps ROM addresses to C function implementations.
 *
 * In the ROM, opcode 0x42 reads a 24-bit ROM address and calls it via
 * JUMP_TO_LOADED_MOVEMENT_PTR. Since we load raw bytecode from the donor
 * ROM, the embedded addresses are the original ROM addresses.
 * We dispatch based on these addresses.
 *
 * Each callroutine receives:
 *   - entity_offset: current entity being processed
 *   - script_offset: current script being executed
 *   - pc: current program counter (after the 3 address bytes)
 *   - out_pc: updated PC (may consume extra parameter bytes)
 *
 * Returns: value to store in tempvar (matches ROM's A register return).
 */
#include "entity/callroutine_internal.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "game/text.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "game/display_text.h"
#include "core/decomp.h"
#include "core/math.h"
#include "data/assets.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/constants.h"
#include "game/audio.h"
#include "core/memory.h"
#include "game/battle.h"
#include "game/fade.h"
#include "game/flyover.h"
#include "game/ending.h"
#include "game/window.h"
#include "game/oval_window.h"
#include "game/position_buffer.h"
#include "game/overworld.h"
#include "game/inventory.h"
#include "game/door.h"
#include "entity/pathfinding.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "game_main.h"

/* Animation sequence metadata from ANIMATION_SEQUENCE_POINTERS (bytes [4..7] of each 8-byte entry).
 * [0..1] = tile_size (LE), [2] = frame_count, [3] = delay. */
static const uint8_t anim_seq_meta[][4] = {
    {0x00, 0x00, 0x00, 0x00},  /* 0: NULL */
    {0x10, 0x1C, 0x06, 0x03},  /* 1: lightning_reflect */
    {0xA0, 0x05, 0x07, 0x10},  /* 2: lightning_strike */
    {0xC0, 0x03, 0x08, 0x08},  /* 3: starman_jr_teleport */
    {0xA0, 0x0A, 0x02, 0x10},  /* 4: boom */
    {0x40, 0x00, 0x03, 0x08},  /* 5: zombies */
    {0x20, 0x01, 0x02, 0x08},  /* 6: the_end (USA) */
};


/* PHOTOGRAPHER_CFG_TABLE — loaded from ending/photographer_cfg.bin.
 * Used by SET_PHOTOGRAPHER_POSITION to place the photographer entity. */
static const uint8_t *photographer_cfg_cr;

/* DELIVERY_SECTOR_PASSABLE_TABLE — from C3DFE8.
 * Indexed by (sector_attrs & 7); nonzero = delivery path is passable. */
static const uint8_t delivery_sector_passable_table[8] = {
    1, 1, 1, 0, 1, 1, 0, 0
};

/* Bank index for read helpers — shared with callroutine sub-files via
 * callroutine_internal.h (sb/sw inline helpers). */
int cr_bank_idx;

void disable_other_entity_callbacks(void) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        ert.entity_callback_flags_backup[i] = entities.tick_callback_hi[i];
    }
    int16_t ent = entities.first_entity;
    while (ent >= 0) {
        if (ent != ert.current_entity_offset) {
            entities.tick_callback_hi[ent] |= 0xC000;
        }
        ent = entities.next_entity[ent];
    }
}

/* ---- CALLROUTINE implementations ---- */

static int16_t cr_update_battle_screen_effects(int16_t ent, int16_t scr,
                                               uint16_t pc, uint16_t *out_pc) {
    (void)ent; (void)scr;
    *out_pc = pc;
    update_battle_screen_effects();
    return 0;
}

static int16_t cr_spawn_entity(int16_t ent, int16_t scr,
                               uint16_t pc, uint16_t *out_pc) {
    uint16_t script_id = sw(pc);
    *out_pc = pc + 2;

    /* Pass ROM script ID directly — resolve_script_id handles all lookups */
    entity_init_wipe(script_id);
    return 0;
}

/* dispatch_tick_callback and render_entity_hdma_window moved to callroutine_screen.c */

/* =========================================================================
 * LOAD_CHARACTER_PORTRAIT_SCREEN helpers
 *
 * Port of asm/misc/load_character_portrait_screen.asm (US) and the
 * dependent subroutines RENDER_CHARACTER_NAME_DISPLAY (US) and
 * DECODE_PLANAR_TILEMAP (bank04).
 *
 * The C port uses the JP ADVANCE_TILEMAP_ANIMATION_FRAME which reads from
 * ert.buffer+$2000 / ert.buffer+$4000.  The US version writes to
 * ert.buffer+$1000, so the interleave step here writes to $2000 instead.
 * ========================================================================= */

/*
 * LUMINE_HALL_TEXT — EBTEXT-encoded bytes loaded from donor ROM.
 * Prefix = [0..3], name-suffix = [4..9], body = [10..214].
 * Total 215 bytes, locale-specific (US/JP differ).
 */
#define LUMINE_HALL_TEXT_LEN  215
#define LUMINE_HALL_BODY_LEN  205  /* chars at index 10..214 */

/*
 * rcnd_buf_offset — current byte offset into ert.buffer for VWF tile copy.
 * Advances by 16 per tile, skipping the lower-tile region on block boundaries.
 */
static void rcnd_advance_offset(uint16_t *buf_offset) {
    *buf_offset += 16;
    if ((*buf_offset & 0xFF) == 0) {   /* divisible by 256 */
        *buf_offset += 256;
    }
}

/*
 * rcnd_copy_tile — copy one completed VWF tile (32 bytes = 16 upper + 16 lower)
 * from vwf_buffer[tile_idx] into ert.buffer at [buf_offset] (upper) and
 * [buf_offset + 256] (lower).
 *
 * tile_idx: index into vwf_buffer (each tile is VWF_TILE_BYTES = 32 bytes)
 * buf_offset: destination byte offset within ert.buffer
 */
static void rcnd_copy_tile(uint16_t tile_idx, uint16_t buf_offset) {
    const uint8_t *src = vwf_buffer + (uint32_t)tile_idx * VWF_TILE_BYTES;
    /* Upper 8 rows = first 16 bytes of VWF tile */
    if ((uint32_t)buf_offset + 16 <= 0x4000)
        memcpy(&ert.buffer[buf_offset], src, 16);
    /* Lower 8 rows = next 16 bytes of VWF tile, at buf_offset + 256 */
    if ((uint32_t)buf_offset + 256 + 16 <= 0x4000)
        memcpy(&ert.buffer[buf_offset + 256], src + 16, 16);
}

/*
 * rcnd_render_glyph — render one NORMAL-font glyph (RENDER_FONT_GLYPH equivalent)
 * into vwf_buffer at the current vwf_x position, using blit_vwf_glyph().
 *
 * eb_char: EB character code (0x50+)
 */
static void rcnd_render_glyph(uint8_t eb_char) {
    if (eb_char < 0x50) return;
    uint8_t char_idx = (eb_char - 0x50) & 0x7F;
    if (char_idx >= FONT_CHAR_COUNT) return;
    const uint8_t *glyph = font_get_glyph(FONT_ID_NORMAL, char_idx);
    uint8_t width = font_get_width(FONT_ID_NORMAL, char_idx);
    uint8_t height = font_get_height(FONT_ID_NORMAL);
    if (!glyph) return;
    blit_vwf_glyph(glyph, height, width);
}

/*
 * render_character_name_display — port of RENDER_CHARACTER_NAME_DISPLAY
 * (asm/intro/render_character_name_display.asm, US version).
 *
 * Renders "I'm <name>....  <lumine_body>" into ert.buffer in the block tile
 * layout expected by DECODE_PLANAR_TILEMAP.
 *
 * Returns: number of tiles copied (used to compute total_rows for var0).
 */
static uint16_t render_character_name_display(void) {
    /* Load lumine hall text from donor ROM */
    const uint8_t *lumine_hall_text = ASSET_DATA(ASSET_DATA_TEXT_LUMINE_HALL_TEXT_BIN);

    /* Init VWF: fill with 0xFF (opaque dark background), reset position */
    memset(vwf_buffer, 0xFF, VWF_BUFFER_SIZE);
    vwf_tile = 0;
    vwf_x = 0;

    /* Track state for tile copy to ert.buffer */
    uint16_t buf_offset = 0;    /* current position in ert.buffer */
    uint16_t tile_copied = 0;   /* how many tiles have been flushed to ert.buffer */

    /*
     * Helper: flush all newly completed tiles (tile_copied..vwf_tile-1) to ert.buffer.
     * Tiles are "complete" once vwf_tile has moved past them.
     * The buf_offset advances with boundary adjustment (like COPY_LUMINE_TILES).
     */
#define FLUSH_COMPLETE_TILES(do_boundary_check) \
    do { \
        uint16_t cur_tile = vwf_x >> 3; \
        while (tile_copied < cur_tile) { \
            rcnd_copy_tile(tile_copied, buf_offset); \
            tile_copied++; \
            if (do_boundary_check) { \
                rcnd_advance_offset(&buf_offset); \
            } else { \
                buf_offset += 16; \
            } \
        } \
    } while (0)

    /* --- Phase 1: render prefix "I'm " (4 chars from lumine_hall_text[0..3]) --- */
    /* Assembly: LDA #0; JSL RENDER_FONT_GLYPH for chars at LUMINE_HALL_TEXT[0..3] */
    for (int i = 0; i < 4; i++) {
        rcnd_render_glyph(lumine_hall_text[i]);
    }

    /* --- Phase 2: render character name (up to 5 chars, clamped to 5 in US) --- */
    int name_len = 0;
    while (name_len < 5 && party_characters[0].name[name_len])
        name_len++;
    if (name_len > 5) name_len = 5;
    for (int i = 0; i < name_len; i++) {
        rcnd_render_glyph(party_characters[0].name[i]);
    }

    /* --- Phase 3: render 6-char suffix from lumine_hall_text[4..9] --- */
    for (int i = 4; i < 10; i++) {
        rcnd_render_glyph(lumine_hall_text[i]);
    }

    /* Copy initial tiles (COPY_INITIAL_TILES — no boundary check yet) */
    {
        uint16_t ntiles = vwf_x >> 3;
        for (uint16_t t = tile_copied; t < ntiles; t++) {
            rcnd_copy_tile(t, buf_offset);
            buf_offset += 16;
        }
        tile_copied = ntiles;
    }

    /* Save the partial (in-progress) last tile to ert.buffer.
     * This preserves the prefix glyph pixels in the boundary tile before VWF reset.
     * (Assembly copies 32 bytes of VWF_BUFFER[tile_copied] to ert.buffer at buf_offset.)
     * The lumine rendering will overwrite this when the tile completes. */
    rcnd_copy_tile(tile_copied, buf_offset);
    /* Don't advance buf_offset — we'll overwrite this tile when it completes */

    /* Save sub-pixel state: VWF_X mod 8 (partial pixel offset into current tile).
     * Assembly does: VWF_X = VWF_X % 8; VWF_TILE = 0; MEMSET16 VWF_BUFFER with 0xFF. */
    vwf_x = vwf_x & 7;
    vwf_tile = 0;  /* reset tile counter for lumine text rendering */
    /* Reset vwf_buffer for the lumine text rendering phase */
    memset(vwf_buffer, 0xFF, VWF_BUFFER_SIZE);

    /* --- Phase 4: render 205 body chars from lumine_hall_text[10..214] ---
     * Each time a tile boundary is crossed (vwf_tile advances), flush the
     * completed tile to ert.buffer with boundary check (COPY_LUMINE_TILES). */
    int body_start = 10;
    uint16_t vwf_tile_prev = 0;  /* tracks previous vwf_tile to detect tile completions */

    for (int i = 0; i < LUMINE_HALL_BODY_LEN && (body_start + i) < LUMINE_HALL_TEXT_LEN; i++) {
        uint8_t eb_char = lumine_hall_text[body_start + i];
        rcnd_render_glyph(eb_char);

        /* Flush any newly completed tiles (tiles where vwf_tile advanced past them) */
        uint16_t new_vwf_tile = vwf_tile;
        while (vwf_tile_prev < new_vwf_tile) {
            rcnd_copy_tile(vwf_tile_prev, buf_offset);
            tile_copied++;
            vwf_tile_prev++;
            rcnd_advance_offset(&buf_offset);
        }
    }

    /* Flush final partial tile from the lumine text rendering */
    {
        rcnd_copy_tile(vwf_tile, buf_offset);
        tile_copied++;
        buf_offset += 16;
    }

#undef FLUSH_COMPLETE_TILES

    /* Return total tiles copied = total portrait "columns" */
    return tile_copied;
}

/*
 * decode_planar_tilemap — port of DECODE_PLANAR_TILEMAP
 * (asm/system/dma/decode_planar_tilemap.asm).
 *
 * Reads a 16×16 pixel (2bpp) tile from ert.buffer at the offset computed
 * from char_code, extracts 2-bit pixel column values across 4 bit-pair levels,
 * and writes 32 uint16 values (64 bytes) to *dest_ptr.
 *
 * The source layout in ert.buffer (set up by render_character_name_display):
 *   Block b (tiles b*16 .. b*16+15):
 *     Upper: ert.buffer[b*512 + (k&0xF)*16 .. +15]
 *     Lower: ert.buffer[b*512 + (k&0xF)*16 + 256 .. +15]
 *   where k = char_code, b = (char_code >> 4).
 *
 * On return, *dest_ptr is advanced past the 64 bytes written.
 */
static void decode_planar_tilemap(uint8_t char_code, uint16_t **dest_ptr) {
    /* Compute source offset in ert.buffer (matches assembly formula) */
    uint16_t lo = (uint16_t)(char_code & 0x0F);
    uint16_t up = (uint16_t)(char_code & 0xF0) << 1;  /* (char_code & 0xF0) * 2 */
    uint16_t combined = up + lo;
    uint32_t src_off = (uint32_t)combined << 4;        /* * 16 */

    /* Guard against out-of-bounds access (ert.buffer is 64KB, src_off max ~0x1EF0) */
    if (src_off + 16 + 256 + 16 > BUFFER_SIZE) return;

    uint16_t *dest = *dest_ptr;

    /* 4 bit-pair levels: shift = 6, 4, 2, 0 */
    for (int lev = 3; lev >= 0; lev--) {
        int shift = lev * 2;  /* 6, 4, 2, 0 */

        /* Upper tile: 4 groups of 4 source bytes each */
        uint32_t src = src_off;
        for (int grp = 0; grp < 4; grp++) {
            /* (b0 XOR b1) AND b0 — extracts plane-0-dominant pixels */
            uint8_t b0 = ert.buffer[src + 0];
            uint8_t b1 = ert.buffer[src + 1];
            uint16_t lo_val = (uint16_t)((b0 ^ b1) & b0);

            uint8_t b2 = ert.buffer[src + 2];
            uint8_t b3 = ert.buffer[src + 3];
            uint16_t hi_val = (uint16_t)((b2 ^ b3) & b2);

            /* Extract 2 bits at shift position, combine into 4-bit result */
            uint16_t result = (uint16_t)(((hi_val >> shift) & 3) << 2)
                            | (uint16_t)((lo_val >> shift) & 3);
            *dest++ = result;
            src += 4;
        }

        /* Lower tile: same but source is +256 bytes from upper */
        src = src_off + 256;
        for (int grp = 0; grp < 4; grp++) {
            uint8_t b0 = ert.buffer[src + 0];
            uint8_t b1 = ert.buffer[src + 1];
            uint16_t lo_val = (uint16_t)((b0 ^ b1) & b0);

            uint8_t b2 = ert.buffer[src + 2];
            uint8_t b3 = ert.buffer[src + 3];
            uint16_t hi_val = (uint16_t)((b2 ^ b3) & b2);

            uint16_t result = (uint16_t)(((hi_val >> shift) & 3) << 2)
                            | (uint16_t)((lo_val >> shift) & 3);
            *dest++ = result;
            src += 4;
        }
    }

    *dest_ptr = dest;
}

/*
 * callroutine_dispatch — called from opcodes.c for CALLROUTINE opcode.
 */
int16_t callroutine_dispatch(uint32_t rom_addr, int16_t entity_offset,
                             int16_t script_offset, uint16_t pc,
                             uint16_t *out_pc) {
    /* Set bank for read helpers (callroutines may read extra params) */
    cr_bank_idx = (int)scripts.pc_bank[script_offset];

    switch (rom_addr) {
    case ROM_ADDR_UPDATE_MINI_GHOST_POSITION:
        return cr_update_mini_ghost_position(entity_offset, script_offset,
                                             pc, out_pc);
    case ROM_ADDR_UPDATE_BATTLE_SCREEN_EFFECTS:
        return cr_update_battle_screen_effects(entity_offset, script_offset,
                                               pc, out_pc);
    case ROM_ADDR_SPAWN_ENTITY:
        return cr_spawn_entity(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_DECOMPRESS_TITLE_DATA:
        return cr_decompress_title_data(entity_offset, script_offset,
                                        pc, out_pc);
    case ROM_ADDR_LOAD_TITLE_PALETTE:
        return cr_load_title_palette(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_CYCLE_ENTITY_PALETTE:
        return cr_cycle_entity_palette(entity_offset, script_offset,
                                       pc, out_pc);
    case ROM_ADDR_SET_STATE_PAUSED:
        return cr_set_state_paused(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_SHOW_ENTITY_SPRITE:
        return cr_show_entity_sprite(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_UPDATE_PALETTE_ANIM:
        return cr_update_palette_anim(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_SET_STATE_RUNNING:
        return cr_set_state_running(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_FINALIZE_PALETTE_FADE:
        return cr_finalize_palette_fade(entity_offset, script_offset,
                                        pc, out_pc);
    case ROM_ADDR_LOAD_FILE_SELECT_PALETTES:
        return cr_load_file_select_palettes(entity_offset, script_offset,
                                            pc, out_pc);
    case ROM_ADDR_FILL_PALETTES_WHITE:
        return cr_fill_palettes_white(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_FILL_PALETTES_BLACK:
        return cr_fill_palettes_black(entity_offset, script_offset, pc, out_pc);
    /* Gas station callroutines */
    case ROM_ADDR_LOAD_GAS_STATION_PALETTE:
        return cr_load_gas_station_palette(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_LOAD_GAS_STATION_FLASH_PALETTE:
        return cr_load_gas_station_flash_palette(entity_offset, script_offset,
                                                  pc, out_pc);
    /* Naming screen callroutines */
    case ROM_ADDR_SET_ENTITY_DIRECTION_AND_FRAME:
        return cr_set_entity_direction_and_frame(entity_offset, script_offset,
                                                  pc, out_pc);
    case ROM_ADDR_DEALLOCATE_ENTITY_SPRITE:
        return cr_deallocate_entity_sprite(entity_offset, script_offset,
                                            pc, out_pc);
    case ROM_ADDR_IS_X_LESS_THAN_ENTITY:
        return cr_is_x_less_than_entity(entity_offset, script_offset,
                                         pc, out_pc);
    case ROM_ADDR_IS_Y_LESS_THAN_ENTITY:
        return cr_is_y_less_than_entity(entity_offset, script_offset,
                                         pc, out_pc);
    case ROM_ADDR_MOVEMENT_CMD_CLEAR_LOOP_COUNTER:
        return cr_clear_loop_counter(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_MOVEMENT_CMD_LOOP:
        return cr_movement_loop(entity_offset, script_offset, pc, out_pc);
    /* File select / overworld entity sprite callroutines */
    case ROM_ADDR_RENDER_ENTITY_SPRITE_ENTRY3:
        /* Port of C0A4A8 RENDER_ENTITY_SPRITE_ENTRY3.
         * Assembly: STZ USE_SECOND_SPRITE_FRAME; JSL IS_ENTITY_ON_SCREEN;
         *           BNE → RENDER_ENTITY_SPRITE_SETUP_AND_RENDER (ENTRY4).
         * Uploads frame-0 tiles to VRAM if the entity is on screen.
         * IMPORTANT: The assembly does NOT modify ENTITY_ANIMATION_FRAME —
         * it only sets the transient USE_SECOND_SPRITE_FRAME global to 0.
         * Entity visibility is controlled separately by SET_ANIMATION (0x3B).
         * The per-frame draw callback handles ongoing tile rendering. */
        *out_pc = pc;
        render_entity_sprite(entity_offset);
        return 0;
    case ROM_ADDR_RENDER_ENTITY_SPRITE_ME1:
        return cr_render_entity_sprite_me1(entity_offset, script_offset,
                                            pc, out_pc);
    case ROM_ADDR_RENDER_ENTITY_SPRITE_ME2:
        return cr_render_entity_sprite_me2(entity_offset, script_offset,
                                            pc, out_pc);
    case ROM_ADDR_RESET_ENTITY_ANIMATION:
        return cr_reset_entity_animation(entity_offset, script_offset,
                                          pc, out_pc);
    /* ---- Overworld movement callroutines ---- */
    case ROM_ADDR_SET_ENTITY_MOVEMENT_SPEED:
        return cr_set_entity_movement_speed(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_SET_ENTITY_MOVEMENT_SPEED_ENTRY2:
        return cr_set_entity_movement_speed_entry2(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_GET_ENTITY_MOVEMENT_SPEED:
        return cr_get_entity_movement_speed(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_SET_DIRECTION8:
        return cr_set_direction8(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_SET_DIRECTION:
        return cr_set_direction(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_GET_ENTITY_DIRECTION:
        return cr_get_entity_direction(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_RAND_HIGH_BYTE:
        return cr_rand_high_byte(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_SET_DIR_VELOCITY:
        return cr_movement_cmd_set_dir_velocity(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_GET_ENTITY_OBSTACLE_FLAGS:
        return cr_get_entity_obstacle_flags(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_GET_ENTITY_PATHFINDING_STATE:
        return cr_get_entity_pathfinding_state(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_SET_SURFACE_FLAGS_CR:
        return cr_set_surface_flags(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_SET_ENTITY_VELOCITY_FROM_DIR:
        return cr_set_entity_velocity_from_dir(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVE_ENTITY_DISTANCE:
        return cr_move_entity_distance(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_HALVE_ENTITY_DELTA_Y:
        return cr_halve_entity_delta_y(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_LOAD_CURRENT_MAP_BLOCK_EVENTS:
        return cr_load_current_map_block_events(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_REVERSE_DIRECTION_8:
        return cr_reverse_direction_8(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_QUANTIZE_ENTITY_DIRECTION:
        return cr_quantize_entity_direction(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_CALCULATE_DIRECTION_TO_TARGET:
        return cr_calculate_direction_to_target(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_DISABLE_ENTITY_COLLISION2:
        return cr_disable_entity_collision2(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_DISPLAY_TEXT:
        return cr_movement_display_text(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_PLAY_SOUND:
        return cr_movement_cmd_play_sound(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_COPY_SPRITE_POS:
        return cr_movement_cmd_copy_sprite_pos(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_COPY_LEADER_POS:
        return cr_movement_cmd_copy_leader_pos(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_STORE_OFFSET_POSITION:
        return cr_movement_store_offset_position(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_GET_EVENT_FLAG:
        return cr_movement_cmd_get_event_flag(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_SET_EVENT_FLAG:
        return cr_movement_cmd_set_event_flag(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_QUEUE_INTERACTION:
        return cr_movement_queue_interaction(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_STORE_NPC_POS:
        return cr_movement_cmd_store_npc_pos(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_STORE_SPRITE_POS:
        return cr_movement_cmd_store_sprite_pos(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_FACE_TOWARD_NPC:
        return cr_movement_cmd_face_toward_npc(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_FACE_TOWARD_SPRITE:
        return cr_movement_cmd_face_toward_sprite(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_SET_BOUNDING_BOX:
        return cr_movement_set_bounding_box(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_SET_POS_FROM_SCREEN:
        return cr_movement_set_pos_from_screen(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_SET_NPC_ID:
        return cr_movement_cmd_set_npc_id(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_ANIMATE_PAL_FADE:
        return cr_movement_cmd_animate_pal_fade(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_SETUP_SPOTLIGHT:
        return cr_movement_cmd_setup_spotlight(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_APPLY_COLOR_MATH:
        return cr_movement_cmd_apply_color_math(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_PRINT_CAST_NAME:
        return cr_movement_cmd_print_cast_name(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_PRINT_CAST_VAR0:
        return cr_movement_cmd_print_cast_var0(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVEMENT_CMD_PRINT_CAST_PARTY:
        return cr_movement_cmd_print_cast_party(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVE_TOWARD_NO_SPRITE_CB:
        return cr_move_toward_no_sprite_cb(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_MOVE_TOWARD_REVERSED_CB:
        return cr_move_toward_reversed_cb(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_UPDATE_DIR_VELOCITY_CB:
        return cr_update_dir_velocity_cb(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_UPDATE_DIR_VELOCITY_REVERSED_CB:
        return cr_update_dir_velocity_reversed_cb(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_ACTIONSCRIPT_PREPARE_ENTITY:
        return cr_actionscript_prepare_entity(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_ACTIONSCRIPT_PREPARE_AT_LEADER:
        return cr_actionscript_prepare_at_leader(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_ACTIONSCRIPT_PREPARE_AT_SELF:
        return cr_actionscript_prepare_at_self(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_ACTIONSCRIPT_FADE_OUT:
        return cr_actionscript_fade_out(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_ACTIONSCRIPT_FADE_IN:
        return cr_actionscript_fade_in(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_ACTIONSCRIPT_GET_PARTY_MEMBER_POS:
        return cr_actionscript_get_party_member_pos(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_PREPARE_ENTITY_AT_TELEPORT_DEST:
        return cr_prepare_entity_at_teleport_dest(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_DISABLE_OBJ_HDMA:
        return cr_disable_obj_hdma(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_INIT_WINDOW_REGISTERS_CR:
        return cr_init_window_registers(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_DECOMP_ITOI_PRODUCTION:
        return cr_decomp_itoi_production(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_DECOMP_NINTENDO_PRESENTATION:
        return cr_decomp_nintendo_presentation(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_PLAY_FLYOVER_SCRIPT:
        return cr_play_flyover_script(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_CHOOSE_RANDOM:
        return cr_choose_random(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_TEST_PLAYER_IN_AREA:
        return cr_test_player_in_area(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_MAKE_PARTY_LOOK_AT_ENTITY:
        return cr_make_party_look_at_entity(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_MOVEMENT_CMD_CALC_TRAVEL_FRAMES:
        return cr_movement_cmd_calc_travel_frames(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_MOVEMENT_CMD_RETURN_2:
        return cr_movement_cmd_return_2(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_IS_ENTITY_NEAR_LEADER:
        return cr_is_entity_near_leader(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_CHECK_ENTITY_OBSTACLES:
        /* Port of C05E76 CHECK_CURRENT_ENTITY_OBSTACLES.
         * Wraps CHECK_ENTITY_OBSTACLE_FLAGS for CURRENT_ENTITY_SLOT.
         * Returns obstacle flags (8-bit, 0 = clear path). */
        *out_pc = pc;
        return check_current_entity_obstacles();
    case ROM_ADDR_CHECK_ENEMY_MOVEMENT_OBSTACLES:
        /* Port of C05E82 CHECK_ENEMY_MOVEMENT_OBSTACLES.
         * Tile collision + enemy run restrictions for enemy movement. */
        *out_pc = pc;
        return check_enemy_movement_obstacles();
    case ROM_ADDR_CHECK_NPC_PLAYER_OBSTACLES:
        /* Port of C05ECE CHECK_NPC_PLAYER_OBSTACLES.
         * Player-style tile collision + enemy run restrictions for NPC movement. */
        *out_pc = pc;
        return check_npc_player_obstacles();
    case ROM_ADDR_CHECK_PROSPECTIVE_NPC_COLLISION:
        /* Port of C06478 CHECK_PROSPECTIVE_NPC_COLLISION.
         * Entity-vs-party/NPC collision at prospective position. */
        *out_pc = pc;
        check_prospective_npc_collision();
        return 0;
    case ROM_ADDR_FADE_OUT_WITH_MOSAIC: {
        /* Port of C08814 FADE_OUT_WITH_MOSAIC.
         * Reads 6 bytes: three 16-bit params.
         *   A = brightness decrease per frame (applied to low 4 bits of INIDISP)
         *   X = frame delay between steps (number of vblanks to wait)
         *   Y = mosaic BG enable mask (non-zero enables mosaic during fade)
         * Loops: decrease brightness, optionally apply mosaic, wait delay frames.
         * Exits when brightness goes negative or force blank already set.
         * Final state: force blank ($80) + disable HDMA. */
        uint16_t decrease = sw(pc);
        uint16_t delay = sw(pc + 2);
        uint16_t mosaic_bgs = sw(pc + 4);
        *out_pc = pc + 6;

        while (1) {
            ppu.mosaic = 0;
            uint8_t cur = ppu.inidisp;
            if (cur & 0x80) break;  /* already force blank */
            int new_b = (int)(cur & 0x0F) - (int)decrease;
            if (new_b < 0) break;
            ppu.inidisp = (uint8_t)new_b;
            if (mosaic_bgs) {
                /* UPDATE_MOSAIC_FROM_BRIGHTNESS (C087AB):
                 * mosaic_size = (~brightness << 4) & 0xF0
                 * Inversely proportional: dimmer = coarser mosaic. */
                uint8_t inv = (uint8_t)((uint8_t)new_b ^ 0xFF);
                ppu.mosaic = (uint8_t)(((inv << 4) & 0xF0) | (mosaic_bgs & 0x0F));
            }
            for (uint16_t i = 0; i < delay; i++)
                wait_for_vblank();
        }
        ppu.inidisp = 0x80;  /* force blank */
        ppu.window_hdma_active = false;  /* disable HDMA effects */
        wait_for_vblank();
        return 0;
    }
    case ROM_ADDR_SPAWN_ENTITY_RELATIVE: {
        /* Port of C0A98B MOVEMENT_CMD_CREATE_ENTITY.
         * Reads sprite ID and script ID, spawns entity at current entity position.
         * Assembly: CREATE_ENTITY_AT_CURRENT_POSITION (C46534) reads current
         * entity's abs_x/abs_y and calls CREATE_ENTITY(sprite, script, -1, x, y). */
        uint16_t sprite_id = sw(pc);
        uint16_t script_id = sw(pc + 2);
        *out_pc = pc + 4;
        int16_t slot = ert.current_entity_slot;
        int16_t ex = entities.abs_x[slot];
        int16_t ey = entities.abs_y[slot];
        create_entity(sprite_id, script_id, -1, ex, ey);
        return 0;
    }
    /* Attract mode / overworld entity callroutines */
    case ROM_ADDR_SET_ENTITY_DIRECTION_VELOCITY:
        return cr_set_entity_direction_velocity(entity_offset, script_offset,
                                                 pc, out_pc);
    case ROM_ADDR_UPDATE_ENTITY_ANIMATION:
        return cr_update_entity_animation(entity_offset, script_offset,
                                           pc, out_pc);
    case ROM_ADDR_CLEAR_CURRENT_ENTITY_COLLISION:
        return cr_clear_current_entity_collision(entity_offset, script_offset,
                                                  pc, out_pc);
    case ROM_ADDR_INITIALIZE_PARTY_MEMBER_ENTITY:
        return cr_initialize_party_member_entity(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_UPDATE_FOLLOWER_VISUALS:
        return cr_update_follower_visuals(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_SRAM_CHECK_ROUTINE_CHECKSUM:
        return cr_sram_check_routine_checksum(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_INFLICT_SUNSTROKE_CHECK:
        return cr_inflict_sunstroke_check(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_CHECK_ENTITY_ENEMY_COLLISION:
        return cr_check_entity_enemy_collision(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_GET_OVERWORLD_STATUS:
        return cr_get_overworld_status(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_CHECK_PROSPECTIVE_ENTITY_COLLISION:
        return cr_check_prospective_entity_collision(entity_offset, script_offset, pc, out_pc);
    case ROM_ADDR_CHECK_NPC_PATROL_BOUNDARY: {
        /* C47269: Compare entity position against var0-var3 patrol rectangle.
         * Returns direction to walk based on which boundary was exceeded:
         *   abs_x < var0 → 3 (DOWN_RIGHT, move right)
         *   abs_x > var1 → 7 (UP_LEFT, move left)
         *   abs_y < var2 → 5 (DOWN_LEFT, move down)
         *   abs_y > var3 → 1 (UP_RIGHT, move up)
         *   inside       → 0 (UP, default/idle) */
        *out_pc = pc;
        int16_t offset = ENT(ert.current_entity_slot);
        int16_t abs_x = entities.abs_x[offset];
        int16_t abs_y = entities.abs_y[offset];
        int16_t v0 = entities.var[0][offset];
        int16_t v1 = entities.var[1][offset];
        int16_t v2 = entities.var[2][offset];
        int16_t v3 = entities.var[3][offset];
        if ((uint16_t)abs_x < (uint16_t)v0) return 3;
        if ((uint16_t)abs_x > (uint16_t)v1) return 7;
        if ((uint16_t)abs_y < (uint16_t)v2) return 5;
        if ((uint16_t)abs_y > (uint16_t)v3) return 1;
        return 0;
    }
    case ROM_ADDR_UPDATE_ENTITY_SURFACE_FLAGS: {
        /* C0C7DB: UPDATE_ENTITY_SURFACE_FLAGS — looks up terrain flags from
         * the collision map at the entity's (X,Y) position and stores them
         * in ENTITY_SURFACE_FLAGS. Calls lookup_surface_flags() which reads
         * the LOADED_COLLISION_TILES grid. */
        *out_pc = pc;
        int16_t offset = ENT(ert.current_entity_slot);
        int16_t x = entities.abs_x[offset];
        int16_t y = entities.abs_y[offset];
        uint16_t size = entities.sizes[offset];
        entities.surface_flags[offset] = lookup_surface_flags(x, y, size);
        return 0;
    }
    case ROM_ADDR_SET_NPC_DIR_FROM_FLAG: {
        /* C0C353/C0C30C: SET_NPC_DIRECTION_FROM_FLAG — reads the NPC config's
         * event_flag field, checks if the flag is set, and sets entity direction
         * to UP (0) if set, DOWN (4) if not. Then re-renders the sprite.
         *
         * Port of asm/overworld/npc/set_npc_direction_from_event_flag.asm:
         *   NPC_ID = ENTITY_NPC_IDS[offset]
         *   event_flag = NPC_CONFIG_TABLE[NPC_ID * 17 + 6]
         *   direction = GET_EVENT_FLAG(event_flag) ? UP : DOWN
         *   RENDER_ENTITY_SPRITE */
        *out_pc = pc;
        int16_t offset = ENT(ert.current_entity_slot);
        uint16_t npc_id = entities.npc_ids[offset];
        uint16_t event_flag_id = get_npc_config_event_flag(npc_id);
        if (event_flag_get(event_flag_id)) {
            entities.directions[offset] = 0;  /* UP */
        } else {
            entities.directions[offset] = 4;  /* DOWN */
        }
        /* Assembly calls RENDER_ENTITY_SPRITE_ENTRY2 after setting direction. */
        render_entity_sprite(offset);
        return 0;
    }
    case ROM_ADDR_IS_ENTITY_COLLISION_ENABLED: {
        /* C0A6B8: Returns -1 if entity collision is active (ENTITY_COLLIDED_OBJECTS
         * is non-negative), 0 if collision is disabled (bit 15 set).
         * Assembly: LDX $88 / LDA ENTITY_COLLIDED_OBJECTS,X / BMI → return 0 */
        *out_pc = pc;
        if (entities.collided_objects[entity_offset] < 0) {
            return 0;   /* bit 15 set → collision disabled */
        }
        return -1;  /* collision enabled */
    }
    case ROM_ADDR_CHECK_ENTITY_CAN_PURSUE: {
        /* Port of C0C48F CHECK_ENTITY_CAN_PURSUE.
         * 1. If pathfinding active → return 0 (busy)
         * 2. If player intangible → return -1
         * 3. Otherwise → classify Manhattan distance to leader:
         *    0=close (≤128), 1=medium (≤160), 2=medium-far (≤256), 3=far (>256)
         *    (Port of CLASSIFY_ENTITY_LEADER_DISTANCE, C0C363) */
        *out_pc = pc;
        int16_t offset = ENT(ert.current_entity_slot);
        if (entities.pathfinding_states[offset] != 0)
            return 0;
        if (ow.player_intangibility_frames != 0)
            return -1;
        /* CLASSIFY_ENTITY_LEADER_DISTANCE inline */
        int16_t dx = (int16_t)game_state.leader_x_coord - entities.abs_x[offset];
        int16_t dy = (int16_t)game_state.leader_y_coord - entities.abs_y[offset];
        uint16_t abs_dx = (uint16_t)(dx < 0 ? -dx : dx);
        uint16_t abs_dy = (uint16_t)(dy < 0 ? -dy : dy);
        uint16_t dist = abs_dx + abs_dy;
        if (dist > 256) return 3;
        if (dist > 160) return 2;
        if (dist > 128) return 1;
        return 0;
    }
    case ROM_ADDR_CHECK_ENTITY_CAN_PURSUE_SHORT: {
        /* Port of C0C4AF CHECK_ENTITY_CAN_PURSUE_SHORT.
         * Same as CHECK_ENTITY_CAN_PURSUE but uses shorter distance thresholds:
         *   0=close (≤64), 1=medium (≤80), 2=medium-far (≤128), 3=far (>128)
         *   (Port of CLASSIFY_ENTITY_LEADER_DISTANCE_SHORT, C0C3F9) */
        *out_pc = pc;
        int16_t offset = ENT(ert.current_entity_slot);
        if (entities.pathfinding_states[offset] != 0)
            return 0;
        if (ow.player_intangibility_frames != 0)
            return -1;
        /* CLASSIFY_ENTITY_LEADER_DISTANCE_SHORT inline */
        int16_t dx = (int16_t)game_state.leader_x_coord - entities.abs_x[offset];
        int16_t dy = (int16_t)game_state.leader_y_coord - entities.abs_y[offset];
        uint16_t abs_dx = (uint16_t)(dx < 0 ? -dx : dx);
        uint16_t abs_dy = (uint16_t)(dy < 0 ? -dy : dy);
        uint16_t dist = abs_dx + abs_dy;
        if (dist > 128) return 3;
        if (dist > 80) return 2;
        if (dist > 64) return 1;
        return 0;
    }
    case ROM_ADDR_CALCULATE_DIRECTION_TO_LEADER: {
        /* C0C62B: Calculate direction from entity to target (var[6], var[7]).
         * Like CALCULATE_DIRECTION_TO_TARGET but with flee flag logic for enemies.
         * If npc_id > 0x7FFF (enemy entity) and CHECK_ENEMY_SHOULD_FLEE returns
         * nonzero, ORs 0x8000 into the direction to signal fleeing. */
        *out_pc = pc;
        int16_t offset = ENT(ert.current_entity_slot);
        uint16_t flee_flag = 0;
        if (entities.npc_ids[offset] > 0x7FFF) {
            if (check_enemy_should_flee())
                flee_flag = 0x8000;
        }
        int16_t target_x = entities.var[6][offset];
        int16_t target_y = entities.var[7][offset];
        uint16_t dir = calculate_direction_fine(
            entities.abs_x[offset], entities.abs_y[offset],
            target_x, target_y);
        return (int16_t)(dir + flee_flag);
    }
    case ROM_ADDR_CALCULATE_SLEEP_FRAMES: {
        /* C0CA4E: Calculate sleep frames from tempvar (distance param) and
         * current entity velocity. Same logic as C0A6A2 but param comes
         * from tempvar (A register) instead of reading 2 script bytes. */
        *out_pc = pc;  /* 0 extra bytes consumed */
        int16_t cs_param = scripts.tempvar[script_offset];
        int16_t cs_offset = ENT(ert.current_entity_slot);

        int32_t cs_dx = (int32_t)(((uint32_t)(int32_t)entities.delta_x[cs_offset] << 16) |
                          (uint32_t)entities.delta_frac_x[cs_offset]);
        int32_t cs_dy = (int32_t)(((uint32_t)(int32_t)entities.delta_y[cs_offset] << 16) |
                          (uint32_t)entities.delta_frac_y[cs_offset]);

        int32_t cs_abs_dx = cs_dx < 0 ? -cs_dx : cs_dx;
        int32_t cs_abs_dy = cs_dy < 0 ? -cs_dy : cs_dy;
        int32_t cs_dominant = cs_abs_dx > cs_abs_dy ? cs_abs_dx : cs_abs_dy;

        if (cs_dominant > 0) {
            int32_t cs_param32 = (int32_t)cs_param << 16;
            int32_t cs_frames = cs_param32 / cs_dominant;
            scripts.sleep_frames[script_offset] = (int16_t)cs_frames;
        } else {
            scripts.sleep_frames[script_offset] = 0;
        }
        return 0;
    }
    case ROM_ADDR_STORE_LEADER_POS_TO_ENTITY_VARS: {
        /* C46B65: Store leader position into entity script var[6] (X) and var[7] (Y).
         * Used before CALCULATE_DIRECTION_TO_LEADER to set the target. */
        *out_pc = pc;
        int16_t offset = ENT(ert.current_entity_slot);
        entities.var[6][offset] = (int16_t)game_state.leader_x_coord;
        entities.var[7][offset] = (int16_t)game_state.leader_y_coord;
        return 0;
    }
    case ROM_ADDR_SET_ENTITY_SLEEP_FRAMES: {
        /* C40023: Set entity script sleep frames from CURRENT_ENTITY_SLOT low nybble.
         * Assembly: LDA CURRENT_ENTITY_SLOT / AND #$000F / STA ENTITY_SCRIPT_SLEEP_FRAMES,X */
        *out_pc = pc;
        int16_t slot = ert.current_entity_slot & 0x0F;
        scripts.sleep_frames[script_offset] = slot;
        return 0;
    }
    case ROM_ADDR_GET_NPC_DEFAULT_DIRECTION: {
        /* C46914: Return default facing direction from NPC config table.
         * If entity has no NPC (npc_id == -1), returns 4 (DOWN). */
        *out_pc = pc;
        int16_t ent_off = ENT(ert.current_entity_slot);
        uint16_t npc_id = (uint16_t)entities.npc_ids[ent_off];
        return (int16_t)get_npc_config_direction(npc_id);
    }
    case ROM_ADDR_SET_ENTITY_DIRECTION_CR: {
        /* C46957: Set entity direction from tempvar. Only updates + re-renders
         * if direction actually changed. Port of SET_ENTITY_DIRECTION. */
        *out_pc = pc;
        int16_t new_dir = scripts.tempvar[script_offset];
        int16_t ent_off = ENT(ert.current_entity_slot);
        if (entities.directions[ent_off] != new_dir) {
            entities.directions[ent_off] = new_dir;
            render_entity_sprite(ent_off);
        }
        return 0;
    }
    case ROM_ADDR_INIT_PARTY_POSITION_BUFFER:
        /* C03F1E: Fill PLAYER_POSITION_BUFFER with leader's current state
         * and place all party member entities at the leader's position.
         * Port of asm/overworld/party/init_party_position_buffer.asm. */
        *out_pc = pc;
        init_party_position_buffer();
        return 0;
    case ROM_ADDR_CREATE_ENTITY_BG3: {
        /* C0A99F: MOVEMENT_CMD_CREATE_ENTITY_BG3.
         * Reads 2 words from script stream: sprite_id and script_id.
         * Calls CREATE_ENTITY_AT_V01_PLUS_BG3Y which creates entity at
         * (var0, var1 + BG3_Y_POS) with sleep frame staggering. */
        uint16_t ceb_sprite = sw(pc);
        uint16_t ceb_script = sw(pc + 2);
        *out_pc = pc + 4;
        create_entity_at_v01_plus_bg3y(ceb_sprite, ceb_script, ert.current_entity_slot);
        return 0;
    }
    case ROM_ADDR_CALCULATE_SLEEP_FROM_VARS: {
        /* C0CC11: Calculate sleep frames from distance to (var6, var7) target
         * divided by dominant entity velocity. Same as CALCULATE_SLEEP_FRAMES
         * but distance is from entity position to var6/var7 instead of tempvar. */
        *out_pc = pc;
        int16_t csv_off = ENT(ert.current_entity_slot);
        int16_t target_x = entities.var[6][csv_off];
        int16_t target_y = entities.var[7][csv_off];
        int16_t dx = target_x - entities.abs_x[csv_off];
        int16_t dy = target_y - entities.abs_y[csv_off];
        int16_t abs_dx = dx < 0 ? -dx : dx;
        int16_t abs_dy = dy < 0 ? -dy : dy;

        int32_t vel;
        if (abs_dx >= abs_dy) {
            vel = (int32_t)(((uint32_t)(int32_t)entities.delta_x[csv_off] << 16) |
                   (uint16_t)entities.delta_frac_x[csv_off]);
        } else {
            vel = (int32_t)(((uint32_t)(int32_t)entities.delta_y[csv_off] << 16) |
                   (uint16_t)entities.delta_frac_y[csv_off]);
        }
        int32_t abs_vel = vel < 0 ? -vel : vel;
        int16_t max_dist = abs_dx >= abs_dy ? abs_dx : abs_dy;

        int16_t frames = 1;
        if (abs_vel > 0) {
            frames = (int16_t)(((uint32_t)(int32_t)max_dist << 16) / abs_vel);
            if (frames < 1) frames = 1;
        }
        scripts.sleep_frames[script_offset] = frames;
        return 0;
    }

    case ROM_ADDR_ENABLE_ALL_ENTITIES:
        *out_pc = pc;
        enable_all_entities();
        return 0;

    case ROM_ADDR_SETUP_ENTITY_COLOR_MATH:
        /* Port of C474A8 SETUP_ENTITY_COLOR_MATH.
         * Reads entity var0 for current entity, determines color math mode:
         *   var0 >= 0: add mode (cgadsub = $33), intensity = var0
         *   var0 < 0:  subtract mode (cgadsub = $B3), intensity = -var0
         * Then chains to SETUP_COLOR_MATH_WINDOW (C4249A). */
        *out_pc = pc;
        setup_entity_color_math();
        return 0;

    case ROM_ADDR_GET_DIRECTION_ROTATED_CLOCKWISE: {
        /* Port of GET_DIRECTION_ROTATED_CLOCKWISE (C0C682).
         * Takes rotation amount in tempvar (A register in assembly),
         * adds it to the current entity's direction, wraps to 0-7.
         * Used for steering NPCs relative to their current facing. */
        *out_pc = pc;
        int16_t rotation = scripts.tempvar[script_offset];
        int16_t cur_dir = entities.directions[ENT(ert.current_entity_slot)];
        return (int16_t)((rotation + cur_dir) & 7);
    }
    case ROM_ADDR_GET_PAD_PRESS:
        /* Port of GET_PAD_PRESS (C468A9.asm).
         * Returns currently pressed buttons (new presses this frame). */
        *out_pc = pc;
        return (int16_t)core.pad1_pressed;
    case ROM_ADDR_GET_PAD_STATE:
        /* Port of GET_PAD_STATE (C468AF.asm).
         * Returns currently held buttons (physical pad state). */
        *out_pc = pc;
        return (int16_t)core.pad1_held;
    case ROM_ADDR_IS_BELOW_LEADER_Y: {
        /* Port of IS_BELOW_LEADER_Y (C46903.asm).
         * Compares tempvar (A register) against leader_y_coord.
         * Returns TRUE (1) if tempvar > leader_y_coord, FALSE (0) otherwise.
         * Assembly: BLTEQ → branch if A <= leader_y. */
        *out_pc = pc;
        int16_t val = scripts.tempvar[script_offset];
        return (val > (int16_t)game_state.leader_y_coord) ? 1 : 0;
    }
    case ROM_ADDR_GET_LEADER_DIRECTION_OFFSET: {
        /* Port of GET_LEADER_DIRECTION_OFFSET (C46A6E.asm).
         * Looks up leader_direction in DIRECTION_TO_DIAGONAL_TABLE.
         * Maps 8-way direction to nearest diagonal (UP_RIGHT=1 or DOWN_LEFT=5). */
        static const int16_t diag_table[8] = {1, 1, 1, 5, 5, 5, 5, 1};
        *out_pc = pc;
        uint16_t dir = game_state.leader_direction & 7;
        return diag_table[dir];
    }
    case ROM_ADDR_GET_CARDINAL_DIRECTION: {
        /* Port of GET_CARDINAL_DIRECTION (C46A9A.asm).
         * Looks up tempvar in DIRECTION_TO_CARDINAL_TABLE.
         * Maps 8-way direction to nearest cardinal (UP=0, RIGHT=2, DOWN=4, LEFT=6). */
        static const int16_t cardinal_table[8] = {0, 2, 2, 2, 4, 6, 6, 6};
        *out_pc = pc;
        int16_t dir = scripts.tempvar[script_offset] & 7;
        return cardinal_table[dir];
    }
    case ROM_ADDR_GET_PARTY_COUNT:
        /* Port of GET_PARTY_COUNT (C47333.asm).
         * Returns game_state.party_count as 8-bit value. */
        *out_pc = pc;
        return (int16_t)(game_state.party_count & 0xFF);
    case ROM_ADDR_IS_PLAYER_BUSY: {
        /* Port of IS_PLAYER_BUSY (EF0F60.asm).
         * Returns TRUE if the player is "busy" (fading, window open, entity fade
         * active, status suppressed, special walking style, etc.).
         * Used by NPC AI scripts to wait before triggering interactions. */
        *out_pc = pc;
        /* Check 1: fade active (fade step != 0) */
        if (fade_active())
            return 1;
        /* Check 2: screen brightness < 15 (screen not fully on) */
        if (core.screen_brightness < 15)
            return 1;
        /* Check 3: any window open */
        if (any_window_open())
            return 1;
        /* Check 4: entity fade in progress */
        if (ow.entity_fade_entity != -1)
            return 1;
        /* Check 5: overworld status suppression */
        if (ow.overworld_status_suppression)
            return 1;
        /* Check 6: leader entity spritemap high bit set (loading/disabled) */
        {
            uint16_t leader_slot = game_state.current_party_members;
            int16_t leader_off = (int16_t)(ENT(leader_slot));
            if (entities.spritemap_ptr_hi[leader_off] & 0x8000)
                return 1;
        }
        /* Check 7: entity tick callbacks disabled for leader entity.
         * Assembly uses hardcoded ENTITY_TICK_CALLBACK_HIGH+46 (slot 23). */
        {
            uint16_t tick_hi = entities.tick_callback_hi[23];
            if (tick_hi & 0xC000) {  /* OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED */
                /* If disabled, check walking style for special movement styles.
                 * Assembly checks LADDER(7), ROPE(8), ESCALATOR(12), STAIRS(13). */
                uint16_t ws = game_state.walking_style;
                if (ws == 7 || ws == 8 || ws == 12 || ws == 13)
                    return 1;
                return 0;
            }
        }
        /* Check 8: pending interactions + walking style.
         * Assembly checks LADDER(7), ROPE(8), ESCALATOR(12), STAIRS(13). */
        {
            int16_t result = (int16_t)ow.pending_interactions;
            uint16_t ws = game_state.walking_style;
            if (ws == 7 || ws == 8 || ws == 12 || ws == 13)
                return 1;
            return result;
        }
    }

    case ROM_ADDR_NULL_CALLROUTINE:
        /* Port of C4CC2C NULL_CALLROUTINE — pure no-op. */
        *out_pc = pc;
        return 0;

    case ROM_ADDR_FORCE_RENDER_ENTITY_SPRITE:
        /* Port of C0AAAC FORCE_RENDER_ENTITY_SPRITE.
         * Sets sprite update offset and calls RENDER_ENTITY_SPRITE_8DIR.
         * In C port, render_entity_sprite handles both 4/8 dir. */
        *out_pc = pc;
        render_entity_sprite(entity_offset);
        return 0;

    case ROM_ADDR_MOVE_TOWARD_TARGET_CB:
        return cr_move_toward_target_cb(entity_offset, script_offset,
                                          pc, out_pc);

    case ROM_ADDR_CHECK_ENTITY_COLLISION_AHEAD:
        /* Port of C0D0D9 CHECK_ENTITY_COLLISION_AHEAD.
         * Calls CHECK_ENTITY_COLLISION_PATH(X=0x3C, A=3).
         * Returns -1 if collision found (coords in var6/var7), 0 if clear. */
        *out_pc = pc;
        return check_entity_collision_ahead(ert.current_entity_slot);

    case ROM_ADDR_SET_DEFAULT_TM:
        /* Port of C0EE47 SET_DEFAULT_TM.
         * Sets TM_MIRROR to $13 (BG1 + BG2 + OBJ on main screen). */
        *out_pc = pc;
        ppu.tm = 0x13;
        return 0;

    case ROM_ADDR_RESTORE_ENTITY_CALLBACK_FLAGS:
        /* Port of C09F71 RESTORE_ENTITY_CALLBACK_FLAGS.
         * Restores tick_callback_hi from backup (saved by DISABLE_OTHER_ENTITY_CALLBACKS). */
        *out_pc = pc;
        for (int i = 0; i < MAX_ENTITIES; i++) {
            entities.tick_callback_hi[i] = ert.entity_callback_flags_backup[i];
        }
        return 0;

    case ROM_ADDR_SAVE_ENTITY_POSITION:
        /* Port of C0D7B3 SAVE_ENTITY_POSITION.
         * Saves current entity's abs_x/y to backup variables. */
        *out_pc = pc;
        ert.actionscript_backup_x = entities.abs_x[ENT(ert.current_entity_slot)];
        ert.actionscript_backup_y = entities.abs_y[ENT(ert.current_entity_slot)];
        return 0;

    case ROM_ADDR_RESTORE_ENTITY_POSITION:
        /* Port of C0D7C7 RESTORE_ENTITY_ACTIONSCRIPT_POSITION.
         * Restores current entity's abs_x/y from backup variables. */
        *out_pc = pc;
        entities.abs_x[ENT(ert.current_entity_slot)] = ert.actionscript_backup_x;
        entities.abs_y[ENT(ert.current_entity_slot)] = ert.actionscript_backup_y;
        return 0;

    case ROM_ADDR_LOAD_INITIAL_MAP_DATA_FAR:
        /* Port of C47369 LOAD_INITIAL_MAP_DATA_FAR.
         * Far wrapper for LOAD_INITIAL_MAP_DATA. */
        *out_pc = pc;
        load_initial_map_data();
        return 0;

    case ROM_ADDR_GET_RANDOM_NPC_DELAY:
        /* Port of C467B4 GET_RANDOM_NPC_DELAY.
         * Returns (RAND() & 0x1F) + 12, range 12-43. */
        *out_pc = pc;
        return (int16_t)((rng_next_byte() & 0x1F) + 12);

    case ROM_ADDR_GET_RANDOM_SCREEN_DELAY: {
        /* Port of C467C2 GET_RANDOM_SCREEN_DELAY.
         * Returns (RAND() & 0x1F) + ((256 - screen_y) >> 2).
         * Entities higher on screen get longer delays (farther from visible area). */
        *out_pc = pc;
        int16_t rand_part = rng_next_byte() & 0x1F;
        int16_t screen_y = entities.screen_y[ENT(ert.current_entity_slot)];
        int16_t y_part = (int16_t)(((uint16_t)(256 - (uint16_t)screen_y)) >> 2);
        return (int16_t)(rand_part + y_part);
    }

    case ROM_ADDR_INIT_OVAL_WINDOW_FAR:
        /* Port of C49841 INIT_OVAL_WINDOW_FAR.
         * Far wrapper for INIT_OVAL_WINDOW(1). */
        *out_pc = pc;
        init_oval_window(1);
        return 0;

    case ROM_ADDR_LOAD_TEXT_LAYER_TILEMAP:
        /* Port of C4981F LOAD_TEXT_LAYER_TILEMAP.
         * Clears TEXT_LAYER_TILEMAP in VRAM (0x800 bytes of zeros).
         * Assembly: COPY_TO_VRAM1 BLANK_TILE_DATA, VRAM::TEXT_LAYER_TILEMAP, $0800, 3 */
        *out_pc = pc;
        memset(win.bg2_buffer, 0, BG2_BUFFER_SIZE);
        memset(ppu.vram + VRAM_TEXT_LAYER_TILEMAP * 2, 0, 0x800);
        return 0;

    case ROM_ADDR_DISABLE_OTHER_ENTITY_CALLBACKS:
        *out_pc = pc;
        disable_other_entity_callbacks();
        return 0;

    case ROM_ADDR_DISABLE_OTHER_ENTITY_UPDATES: {
        /* Port of C0D77F DISABLE_OTHER_ENTITY_UPDATES.
         * Loops through all entity slots (0 to MAX_ENTITIES-1),
         * skipping current entity and slot 23.
         * Sets OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED on all others. */
        *out_pc = pc;
        for (int i = 0; i < MAX_ENTITIES; i++) {
            if (i == (int)ert.current_entity_slot) continue;
            if (i == 23) continue;
            entities.tick_callback_hi[i] |= 0xC000;
        }
        return 0;
    }

    case ROM_ADDR_DISABLE_ALL_ENTITIES: {
        /* Port of C0943C DISABLE_ALL_ENTITIES.
         * Walks the active entity linked list and sets
         * OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED on all entities. */
        *out_pc = pc;
        int16_t ent = entities.first_entity;
        while (ent >= 0) {
            entities.tick_callback_hi[ent] |= 0xC000;
            ent = entities.next_entity[ent];
        }
        return 0;
    }

    case ROM_ADDR_IS_PSI_ANIMATION_ACTIVE:
        /* Port of C2EACF IS_PSI_ANIMATION_ACTIVE.
         * Returns 1 if PSI animation or swirl is active, 0 otherwise. */
        *out_pc = pc;
        return is_psi_animation_active() ? 1 : 0;

    case ROM_ADDR_SETUP_FULLSCREEN_WINDOW_CLIPPING:
        /* Port of C4240A: set windows 1+2 to fullscreen, enable BG1+BG2+OBJ masking */
        *out_pc = pc;
        ppu.wh0 = 0x00;
        ppu.wh2 = 0x00;
        ppu.wh1 = 0xFF;
        ppu.wh3 = 0xFF;
        ppu.cgwsel = 0x20;
        ppu.tmw = 0x13;
        ppu.wbglog = 0x00;
        ppu.wobjlog = 0x00;
        return 0;

    case ROM_ADDR_SETUP_DARK_WINDOW_EFFECT:
        /* Port of C424D1: subtract color math, dark gray ($EF = intensity 15) */
        *out_pc = pc;
        ppu.wobjsel = 0x20;
        ppu.wh0 = 0x80;
        ppu.wh1 = 0x7F;
        ppu.tmw = 0x13;
        ppu.wbglog = 0x00;
        ppu.wobjlog = 0x00;
        ppu.cgwsel = 0x20;
        ppu.cgadsub = 0xB3;
        { uint8_t intensity = 0xEF & 0x1F; /* 15 */
          ppu.coldata_r = intensity;
          ppu.coldata_g = intensity;
          ppu.coldata_b = intensity; }
        return 0;

    case ROM_ADDR_SETUP_WHITE_WINDOW_EFFECT:
        /* Port of C42509: subtract color math, white ($FF = intensity 31) */
        *out_pc = pc;
        ppu.wobjsel = 0x20;
        ppu.wh0 = 0x00;
        ppu.wh1 = 0xFF;
        ppu.tmw = 0x13;
        ppu.wbglog = 0x00;
        ppu.wobjlog = 0x00;
        ppu.cgwsel = 0x20;
        ppu.cgadsub = 0xB3;
        { uint8_t intensity = 0xFF & 0x1F; /* 31 */
          ppu.coldata_r = intensity;
          ppu.coldata_g = intensity;
          ppu.coldata_b = intensity; }
        return 0;

    case ROM_ADDR_SETUP_DUAL_DARK_WINDOW_EFFECT:
        /* Port of C4258C: dual windows (1+2), subtract, dark ($EF = intensity 15) */
        *out_pc = pc;
        ppu.wobjsel = 0xA0;
        ppu.wh0 = 0x80;
        ppu.wh2 = 0x80;
        ppu.wh1 = 0x7F;
        ppu.wh3 = 0x7F;
        ppu.tmw = 0x13;
        ppu.wbglog = 0x00;
        ppu.wobjlog = 0x00;
        ppu.cgwsel = 0x20;
        ppu.cgadsub = 0xB3;
        { uint8_t intensity = 0xEF & 0x1F; /* 15 */
          ppu.coldata_r = intensity;
          ppu.coldata_g = intensity;
          ppu.coldata_b = intensity; }
        return 0;

    case ROM_ADDR_INSTANT_WIN_PP_RECOVERY:
        /* Port of C2654C INSTANT_WIN_PP_RECOVERY.
         * Flashes screen purple twice, recovers 20 PP for NESS/PAULA/POO. */
        *out_pc = pc;
        instant_win_pp_recovery();
        return 0;

    case ROM_ADDR_SCALE_DIRECTION_DISTANCE:
        /* Port of C46B2D SCALE_DIRECTION_DISTANCE.
         * Multiplies tempvar by 0x2000 to convert 8-way SNES direction to fine direction.
         * Fine direction space: 0x0000 per dir step = 0x2000. */
        *out_pc = pc;
        return (int16_t)((uint16_t)scripts.tempvar[script_offset] * (uint16_t)0x2000);

    case ROM_ADDR_SETUP_PSI_TELEPORT_DEPARTURE:
        /* Port of C48B2C SETUP_PSI_TELEPORT_DEPARTURE.
         * Sets teleport style to 5 and leader direction to RIGHT (2). */
        *out_pc = pc;
        ow.psi_teleport_style = 5;
        game_state.leader_direction = 2;  /* DIRECTION::RIGHT */
        return 0;

    case ROM_ADDR_DISABLE_CURRENT_ENTITY_COLLISION:
        /* Port of C0A6D1 DISABLE_CURRENT_ENTITY_COLLISION.
         * Assembly: LDX $88; LDA #ENTITY_COLLISION_DISABLED; STA ENTITY_COLLIDED_OBJECTS,X */
        *out_pc = pc;
        entities.collided_objects[entity_offset] = ENTITY_COLLISION_DISABLED;
        return 0;

    case ROM_ADDR_STORE_ENTITY_POSITION_TO_VARS:
        /* Port of C46C45 STORE_ENTITY_POSITION_TO_VARS.
         * Copies abs_x → var0, abs_y → var1 for the current entity. */
        *out_pc = pc;
        entities.var[0][entity_offset] = entities.abs_x[entity_offset];
        entities.var[1][entity_offset] = entities.abs_y[entity_offset];
        return 0;

    case ROM_ADDR_DISABLE_HDMA_CHANNEL4_ALT:
    case ROM_ADDR_DISABLE_HDMA_CHANNEL4:
        /* Port of C4257F/C425F3: clear HDMA channel 4 bit (0x10) in HDMAEN_MIRROR.
         * HDMA channel 4 drives WH0/WH1 per-scanline window tables (spotlight).
         * Disabling it reverts to static wh0/wh1 values. */
        *out_pc = pc;
        ppu.window_hdma_active = false;
        return 0;

    case ROM_ADDR_DISABLE_HDMA_CHANNEL5:
        /* Port of C42624: clear HDMA channel 5 bit (0x20) in HDMAEN_MIRROR.
         * HDMA channel 5 drives WH2/WH3 per-scanline window tables. */
        *out_pc = pc;
        ppu.window2_hdma_active = false;
        return 0;

    case ROM_ADDR_GET_DIRECTION_FROM_PLAYER_TO_ENTITY:
        /* Port of C0C4F7 GET_DIRECTION_FROM_PLAYER_TO_ENTITY.
         * Returns 8-way direction from current entity toward the party leader. */
        *out_pc = pc;
        return get_direction_from_player_to_entity();

    case ROM_ADDR_CHECK_ENEMY_SHOULD_FLEE:
        /* Port of C0C524 CHECK_ENEMY_SHOULD_FLEE.
         * Checks run-away event flags and party level vs enemy level
         * thresholds to decide if current enemy should flee. */
        *out_pc = pc;
        return (int16_t)check_enemy_should_flee();

    case ROM_ADDR_CHOOSE_ENTITY_DIRECTION_TO_PLAYER:
        /* Port of C0C615 CHOOSE_ENTITY_DIRECTION_TO_PLAYER.
         * If enemy should flee → direction away from player.
         * Otherwise → direction toward player. */
        *out_pc = pc;
        return choose_entity_direction_to_player();

    case ROM_ADDR_RESTORE_ENTITY_POSITION_FROM_VARS:
        /* Port of C46C87 RESTORE_ENTITY_POSITION_FROM_VARS.
         * Copies var6 → abs_x, var7 → abs_y for the current entity.
         * Inverse of STORE_LEADER_POS_TO_ENTITY_VARS. */
        *out_pc = pc;
        entities.abs_x[entity_offset] = entities.var[6][entity_offset];
        entities.abs_y[entity_offset] = entities.var[7][entity_offset];
        return 0;

    case ROM_ADDR_SET_ENTITY_RANDOM_SCREEN_X: {
        /* Port of C46D23 SET_ENTITY_RANDOM_SCREEN_X.
         * Sets abs_x = RAND() + BG1_X_POS + 112, abs_y = BG1_Y_POS. */
        *out_pc = pc;
        int16_t rx = (int16_t)((uint16_t)rng_next_byte()
                     + (uint16_t)ppu.bg_hofs[0] + 112);
        entities.abs_x[entity_offset] = rx;
        entities.abs_y[entity_offset] = (int16_t)ppu.bg_vofs[0];
        return 0;
    }

    case ROM_ADDR_IS_BATTLE_PENDING:
        /* Port of C0D59B IS_BATTLE_PENDING.
         * Returns 1 if ow.battle_swirl_countdown != 0 or ow.enemy_has_been_touched != 0. */
        *out_pc = pc;
        return (ow.battle_swirl_countdown || ow.enemy_has_been_touched) ? 1 : 0;

    case ROM_ADDR_CLEAR_ENTITY_COLLISION2:
        /* Port of C0A838 CLEAR_CURRENT_ENTITY_COLLISION2.
         * Sets collided_objects to ENTITY_COLLISION_NO_OBJECT (0xFFFF = no collision).
         * Different from DISABLE (0x8000): this resets to "no object" rather than disabling. */
        *out_pc = pc;
        entities.collided_objects[entity_offset] = -1;  /* 0xFFFF */
        return 0;

    case ROM_ADDR_RESTORE_MAP_PALETTE:
        /* Port of C4978E RESTORE_MAP_PALETTE.
         * Despite the name, this saves current PALETTES into MAP_PALETTE_BACKUP
         * (MEMCPY16 source=PALETTES, dest=MAP_PALETTE_BACKUP, 512 bytes).
         * Called before palette fades to update the backup with current state. */
        *out_pc = pc;
        memcpy(ml.map_palette_backup, ert.palettes, 256 * sizeof(uint16_t));
        ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
        return 0;

    case ROM_ADDR_STEER_ENTITY_TOWARD_DIRECTION: {
        /* Port of C0CEBE STEER_ENTITY_TOWARD_DIRECTION.
         * Gradually turns entity toward a target fine direction (in tempvar).
         * Reads var4 as current fine direction, var3 as target speed.
         * Adjusts direction by ±0x0800 per frame (45°/4 increments).
         * Also accelerates movement_speed toward var3. */
        *out_pc = pc;
        uint16_t target_dir = (uint16_t)scripts.tempvar[script_offset];
        uint16_t cur_dir = (uint16_t)entities.var[4][entity_offset];
        uint16_t new_dir = cur_dir;

        if (target_dir != cur_dir) {
            /* Determine turn direction (clockwise or counter-clockwise) */
            int clockwise;
            if (target_dir > cur_dir) {
                uint16_t diff = target_dir - cur_dir;
                clockwise = (diff < 0x8000) ? 1 : 0;
            } else {
                uint16_t diff = cur_dir - target_dir;
                clockwise = (diff < 0x8000) ? 0 : 1;
            }
            if (clockwise) {
                new_dir = cur_dir + 0x0800;
            } else {
                new_dir = cur_dir - 0x0800;
            }
        }

        /* Accelerate: if movement_speed < var3, add 16 */
        uint16_t cur_speed = entities.movement_speeds[entity_offset];
        uint16_t target_speed = (uint16_t)entities.var[3][entity_offset];
        if (cur_speed < target_speed) {
            entities.movement_speeds[entity_offset] = cur_speed + 16;
        }

        /* Quantize old and new directions (QUANTIZE_ENTITY_DIRECTION stores to moving_dirs) */
        int16_t old_q = quantize_entity_direction(entity_offset, cur_dir);
        int16_t new_q = quantize_entity_direction(entity_offset, new_dir);
        if (old_q != new_q) {
            /* Sprite group changed — re-render sprite.
             * RENDER_ENTITY_SPRITE_ENTRY2 takes slot, uses animation_frame. */
            render_entity_sprite(entity_offset);
        }

        /* Store new fine direction to var4 and return it */
        entities.var[4][entity_offset] = (int16_t)new_dir;
        return (int16_t)new_dir;
    }

    case ROM_ADDR_QUANTIZE_DIRECTION: {
        /* Port of C46B51 QUANTIZE_DIRECTION.
         * Takes fine direction in tempvar, returns 8-way SNES direction. */
        *out_pc = pc;
        uint16_t fine_dir = (uint16_t)scripts.tempvar[script_offset];
        return quantize_fine_direction(fine_dir);
    }

    case ROM_ADDR_ENTITY_COORDS_RELATIVE_TO_BG3:
        /* Port of C0A06C ENTITY_COORDS_RELATIVE_TO_BG3.
         * Converts entity abs coords to BG3-relative. Uses $88 as entity offset. */
        *out_pc = pc;
        entities.abs_x[entity_offset] -= ppu.bg_hofs[2];
        entities.screen_x[entity_offset] = entities.abs_x[entity_offset];
        entities.abs_y[entity_offset] -= ppu.bg_vofs[2];
        entities.screen_y[entity_offset] = entities.abs_y[entity_offset];
        return 0;

    case ROM_ADDR_REFLECT_ENTITY_Y_AT_TARGET: {
        /* Port of C47A6B REFLECT_ENTITY_Y_AT_TARGET.
         * Reflects entity Y position around var7 target:
         * new_y = var7 - (abs_y - var7) = 2*var7 - abs_y */
        *out_pc = pc;
        int16_t target_y = entities.var[7][entity_offset];
        int16_t cur_y = entities.abs_y[entity_offset];
        int16_t diff = cur_y - target_y;
        entities.abs_y[entity_offset] = target_y - diff;
        return 0;
    }

    case ROM_ADDR_IS_ENTITY_IN_RANGE_OF_LEADER: {
        /* Port of C46EF8 IS_ENTITY_IN_RANGE_OF_LEADER.
         * Returns 1 if entity is within var2 (X range) and var3 (Y range)
         * of the party leader. Returns 0 if teleporting. */
        *out_pc = pc;
        if (ow.psi_teleport_destination != 0)
            return 0;
        int16_t dx = entities.abs_x[entity_offset] - game_state.leader_x_coord;
        if (dx < 0) dx = -dx;
        if ((uint16_t)dx >= (uint16_t)entities.var[2][entity_offset])
            return 0;
        int16_t dy = entities.abs_y[entity_offset] - game_state.leader_y_coord;
        if (dy < 0) dy = -dy;
        if ((uint16_t)dy >= (uint16_t)entities.var[3][entity_offset])
            return 0;
        return 1;
    }

    case ROM_ADDR_IS_ENTITY_STILL_ON_CAST_SCREEN:
        /* Port of C4ECE7 IS_ENTITY_STILL_ON_CAST_SCREEN.
         * Returns 1 if entity Y > BG3_Y - 8, else 0. */
        *out_pc = pc;
        return is_entity_still_on_cast_screen(entity_offset);

    case ROM_ADDR_RESTORE_OVERWORLD_STATE:
        /* Port of EF0FF6 RESTORE_OVERWORLD_STATE.
         * Clears pending interactions, sets status suppression from giegu flag,
         * plays bicycle music or updates map music. */
        *out_pc = pc;
        ow.pending_interactions = 0;
        ow.overworld_status_suppression = event_flag_get(EVENT_FLAG_WIN_GIEGU) ? 1 : 0;
        if (game_state.walking_style == WALKING_STYLE_BICYCLE) {
            change_music(82);  /* MUSIC::BICYCLE */
        } else {
            update_map_music_at_leader();
        }
        return 0;

    case ROM_ADDR_ENABLE_TESSIE_LEAVES_ENTITIES: {
        /* Port of C467E6 ENABLE_TESSIE_LEAVES_ENTITIES.
         * Finds all entities with LEAVES_FOR_TESSIE_SCENE sprite and
         * clears their TICK_DISABLED | MOVE_DISABLED flags. */
        *out_pc = pc;
        for (int i = 0; i < MAX_ENTITIES; i++) {
            if (entities.sprite_ids[i] == OVERWORLD_SPRITE_LEAVES_FOR_TESSIE_SCENE) {
                entities.tick_callback_hi[i] &= ~(OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
            }
        }
        return 0;
    }

    case ROM_ADDR_CHECK_SECTOR_USES_MINISPRITES: {
        /* Port of C2FF9A CHECK_SECTOR_USES_MINISPRITES.
         * Returns 1 if sector attributes < 3, else 0. */
        *out_pc = pc;
        uint16_t attrs = load_sector_attrs(
            game_state.leader_x_coord, game_state.leader_y_coord);
        return ((attrs & 0x0007) < 3) ? 0 : 1;
    }

    case ROM_ADDR_DISABLE_PARTY_MOVEMENT_AND_HIDE: {
        /* Port of C46712 DISABLE_PARTY_MOVEMENT_AND_HIDE.
         * Disables tick+move on leader entity, hides follower sprites. */
        *out_pc = pc;
        uint16_t leader_slot = read_u16_le(&game_state.party_entity_slots[0]);
        int16_t leader_ent = ENT(leader_slot);
        entities.tick_callback_hi[leader_ent] |= (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
        for (int i = 1; i < (game_state.party_count & 0xFF); i++) {
            uint16_t slot = read_u16_le(&game_state.party_entity_slots[i * 2]);
            entities.spritemap_ptr_hi[slot] |= 0x8000;  /* DRAW_DISABLED */
        }
        return 0;
    }

    case ROM_ADDR_ENABLE_PARTY_MOVEMENT_AND_SHOW: {
        /* Port of C4675C ENABLE_PARTY_MOVEMENT_AND_SHOW.
         * Re-enables tick+move on leader, shows follower sprites
         * (only if party_order != 9, meaning slot is occupied). */
        *out_pc = pc;
        uint16_t leader_slot2 = read_u16_le(&game_state.party_entity_slots[0]);
        entities.tick_callback_hi[leader_slot2] &= ~(OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
        for (int i = 1; i < (game_state.party_count & 0xFF); i++) {
            uint8_t order = game_state.party_order[i];
            if (order == 9) continue;  /* empty slot */
            uint16_t slot2 = read_u16_le(&game_state.party_entity_slots[i * 2]);
            entities.spritemap_ptr_hi[slot2] &= ~0x8000;  /* clear DRAW_DISABLED */
        }
        return 0;
    }

    case ROM_ADDR_RAND:
        /* Port of C08E9A RAND.
         * Returns random value 0-255. */
        *out_pc = pc;
        return (int16_t)rng_next_byte();

    case ROM_ADDR_QUEUE_NPC_TEXT_INTERACTION: {
        /* Port of C4681A QUEUE_NPC_TEXT_INTERACTION.
         * Gets NPC's text pointer from config table, queues type-8 interaction. */
        *out_pc = pc;
        int16_t npc_id = entities.npc_ids[entity_offset];
        if (npc_id == -1) return 0;
        uint32_t text_ptr = get_npc_config_text_pointer(npc_id);
        if (text_ptr == 0) return 0;
        queue_interaction(8, text_ptr);
        return 0;
    }

    case ROM_ADDR_INIT_OVAL_WINDOW:
        /* Port of C2EA15 INIT_OVAL_WINDOW.
         * Initializes oval window effect. Type in tempvar (A). */
        *out_pc = pc;
        init_oval_window((uint16_t)scripts.tempvar[script_offset]);
        return 0;

    case ROM_ADDR_CLOSE_OVAL_WINDOW:
        /* Port of C2EA74 CLOSE_OVAL_WINDOW. */
        *out_pc = pc;
        close_oval_window();
        return 0;

    case ROM_ADDR_CHECK_CAST_SCROLL_THRESHOLD:
        /* Port of C4E4F9 CHECK_CAST_SCROLL_THRESHOLD.
         * Returns 1 if var0 <= BG3_Y_POS (scroll passed threshold), else 0. */
        *out_pc = pc;
        return check_cast_scroll_threshold(entity_offset);

    case ROM_ADDR_SET_CAST_SCROLL_THRESHOLD:
        /* Port of C4E4DA SET_CAST_SCROLL_THRESHOLD.
         * Sets var0 = param * 8 + BG3_Y_POS. Param in tempvar (A). */
        *out_pc = pc;
        set_cast_scroll_threshold(scripts.tempvar[script_offset], entity_offset);
        return 0;

    /* ---- Delivery system (EF bank) ---- */

    case ROM_ADDR_GET_DELIVERY_ATTEMPTS: {
        /* Port of EF0C87 GET_DELIVERY_ATTEMPTS.
         * Returns ow.delivery_attempts[var0] for current entity. */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        if (var0 >= DELIVERY_TABLE_COUNT) return 0;
        return ow.delivery_attempts[var0];
    }

    case ROM_ADDR_RESET_DELIVERY_ATTEMPTS: {
        /* Port of EF0C97 RESET_DELIVERY_ATTEMPTS.
         * Zeros ow.delivery_attempts[var0] for current entity. */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        if (var0 < DELIVERY_TABLE_COUNT)
            ow.delivery_attempts[var0] = 0;
        return 0;
    }

    case ROM_ADDR_CHECK_DELIVERY_ATTEMPT_LIMIT: {
        /* Port of EF0CA7 CHECK_DELIVERY_ATTEMPT_LIMIT.
         * Reads attempt limit from table[var0*20 + 4] (timed_delivery::unknown4).
         * If limit == 0x00FF: return 1 (unlimited).
         * Assembly uses CMP #<-1 (=$00FF in 16-bit mode) for the unlimited check.
         * Otherwise: increments attempts and returns 1 if under limit, 0 if reached. */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        const DeliveryEntry *table = get_delivery_table();
        if (!table || var0 >= DELIVERY_TABLE_COUNT) return 0;
        uint16_t limit = table[var0].attempt_limit;
        if (limit == 0x00FF) return 1;  /* unlimited (assembly: CMP #<-1) */
        ow.delivery_attempts[var0]++;
        if (limit <= (uint16_t)ow.delivery_attempts[var0]) return 0;  /* limit reached */
        return 1;  /* under limit */
    }

    case ROM_ADDR_GET_TIMED_DELIVERY_TIME: {
        /* Port of EF0D23 GET_TIMED_DELIVERY_TIME.
         * Returns table[var0*20 + 6] (timed_delivery::unknown6). */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        const DeliveryEntry *table = get_delivery_table();
        if (!table || var0 >= DELIVERY_TABLE_COUNT) return 0;
        return (int16_t)table[var0].delivery_time;
    }

    case ROM_ADDR_SET_DELIVERY_TIMER: {
        /* Port of EF0D46 SET_DELIVERY_TIMER.
         * Sets ow.delivery_timers[var0] = table[var0*20 + 8] (delivery_time). */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        const DeliveryEntry *table = get_delivery_table();
        if (!table || var0 >= DELIVERY_TABLE_COUNT) return 0;
        ow.delivery_timers[var0] = (int16_t)table[var0].timer_value;
        return 0;
    }

    case ROM_ADDR_DECREMENT_DELIVERY_TIMER: {
        /* Port of EF0D73 DECREMENT_DELIVERY_TIMER.
         * Decrements ow.delivery_timers[var0] if non-zero. */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        if (var0 < DELIVERY_TABLE_COUNT && ow.delivery_timers[var0] != 0)
            ow.delivery_timers[var0]--;
        return 0;
    }

    case ROM_ADDR_QUEUE_DELIVERY_SUCCESS: {
        /* Port of EF0D8D QUEUE_DELIVERY_SUCCESS_INTERACTION.
         * Loads text_pointer_1 (PTR3 at offset +10) from delivery table,
         * queues interaction type 8. */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        const DeliveryEntry *table = get_delivery_table();
        if (!table || var0 >= DELIVERY_TABLE_COUNT) return 0;
        uint32_t ptr = ((uint32_t)table[var0].success_bank << 16) | table[var0].success_addr;
        queue_interaction(8, ptr);
        return 0;
    }

    case ROM_ADDR_QUEUE_DELIVERY_FAILURE: {
        /* Port of EF0DFA QUEUE_DELIVERY_FAILURE_INTERACTION.
         * Loads text_pointer_2 (PTR3 at offset +13) from delivery table,
         * queues interaction type 10. */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        const DeliveryEntry *table = get_delivery_table();
        if (!table || var0 >= DELIVERY_TABLE_COUNT) return 0;
        uint32_t ptr = ((uint32_t)table[var0].failure_bank << 16) | table[var0].failure_addr;
        queue_interaction(10, ptr);
        return 0;
    }

    case ROM_ADDR_GET_TIMED_DELIVERY_ENTER_SPEED: {
        /* Port of EF0E67 GET_TIMED_DELIVERY_ENTER_SPEED.
         * Returns table[var0*20 + 16] (timed_delivery::enter_speed). */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        const DeliveryEntry *table = get_delivery_table();
        if (!table || var0 >= DELIVERY_TABLE_COUNT) return 0;
        return (int16_t)table[var0].enter_speed;
    }

    case ROM_ADDR_GET_TIMED_DELIVERY_EXIT_SPEED: {
        /* Port of EF0E8A GET_TIMED_DELIVERY_EXIT_SPEED.
         * Returns table[var0*20 + 18] (timed_delivery::exit_speed). */
        *out_pc = pc;
        uint16_t var0 = (uint16_t)entities.var[0][entity_offset];
        const DeliveryEntry *table = get_delivery_table();
        if (!table || var0 >= DELIVERY_TABLE_COUNT) return 0;
        return (int16_t)table[var0].exit_speed;
    }

    case ROM_ADDR_INIT_DELIVERY_SEQUENCE: {
        /* Port of EF0FDB INIT_DELIVERY_SEQUENCE.
         * Sets state flags, disables other entities, plays delivery music,
         * dismounts bicycle. */
        *out_pc = pc;
        ow.overworld_status_suppression = 1;
        ow.pending_interactions = 1;
        disable_other_entity_callbacks();
        change_music(89);  /* MUSIC::DELIVERY */
        dismount_bicycle();
        return 0;
    }

    case ROM_ADDR_HANDLE_ENEMY_CONTACT: {
        /* Port of C0D5B0 HANDLE_ENEMY_CONTACT.
         * Called by enemy entity scripts to detect contact with the player party.
         * Handles first-contact (desaturate ert.palettes, camera shake, disable entities)
         * and swirl-active (recruit additional enemies into the battle).
         * Returns: 0=no contact, 1=contact detected (or magic butterfly). */
        *out_pc = pc;
        int16_t slot = ert.current_entity_slot;

        /* --- Early exits --- */
        if (bt.battle_mode_flag != 0) return 0;
        if (dr.using_door != 0) return 0;

        /* If swirl is active AND this is the touching enemy, skip straight to collision */
        int skip_collision_check = 0;
        if (ow.battle_swirl_countdown != 0 && slot == bt.touched_enemy) {
            skip_collision_check = 1;
        }

        if (!skip_collision_check) {
            if (game_state.camera_mode == 2) return 0;
            if (ow.player_movement_flags & 0x02) return 0;
            if (game_state.walking_style == WALKING_STYLE_ESCALATOR) return 0;
            if (ow.player_intangibility_frames != 0) return 0;

            /* If swirl active and entity has no remaining path points → collision */
            if (ow.battle_swirl_countdown != 0 && ert.entity_path_point_counts[slot] == 0) {
                skip_collision_check = 1;
            }
        }

        if (!skip_collision_check) {
            /* Actually check collision */
            if (check_entity_enemy_collision() == 0)
                return 0;
        }

        /* --- Collision detected --- */

        /* Magic butterfly special case: if no swirl and no prior touch,
         * and this enemy is a magic butterfly, return 1 without starting encounter */
        if (ow.battle_swirl_countdown == 0 && ow.enemy_has_been_touched == 0) {
            if (entities.enemy_ids[slot] == ENEMY_MAGIC_BUTTERFLY)
                return 1;
        }

        /* --- Start encounter or handle active swirl --- */
        if (ow.battle_swirl_countdown == 0 && ow.enemy_has_been_touched == 0) {
            /* First contact: start the encounter sequence */
            ow.enemy_has_been_touched = 1;
            desaturate_palettes();

            /* Determine pathfinding target entity.
             * ENTITY_COLLIDED_OBJECTS+46 = byte offset 46 = word index 23 */
            if (slot == entities.collided_objects[23]) {
                ert.enemy_pathfinding_target_entity = 24;
            } else {
                ert.enemy_pathfinding_target_entity = entities.collided_objects[slot];
            }
            bt.touched_enemy = slot;

            /* Disable tick+move for all entities except slot 23 (player) */
            for (int16_t i = 0; i < MAX_ENTITIES; i++) {
                if (i != 23) {
                    entities.tick_callback_hi[i] |= 0xC000;
                }
            }
            start_camera_shake();
            return 1;
        }

        /* Swirl already active: handle additional enemy joining */
        entities.collided_objects[slot] = ENTITY_COLLISION_DISABLED;
        int16_t result = 0;

        if (ow.battle_swirl_countdown != 0) {
            if (slot == bt.touched_enemy) {
                /* Original touching enemy — just disable it */
                entities.tick_callback_hi[slot] |= 0xC000;
                result = 1;
            } else {
                /* Check if this enemy matches any pathfinding enemy group */
                int16_t enemy_id = entities.enemy_ids[slot];
                int total_count = 0;  /* Y register in assembly */
                int16_t loop_idx = 0;  /* LOCAL01 */

                for (loop_idx = 0; loop_idx < 4; loop_idx++) {
                    if (enemy_id == ert.pathfinding_enemy_ids[loop_idx]) {
                        int16_t count = ert.pathfinding_enemy_counts[loop_idx];
                        if (count != 0) {
                            ert.pathfinding_enemy_counts[loop_idx] = count - 1;
                            result = 1;
                            entities.tick_callback_hi[slot] |= 0xC000;
                            /* Add to bt.enemies_in_battle list */
                            bt.enemies_in_battle_ids[bt.enemies_in_battle] = (uint16_t)enemy_id;
                            bt.enemies_in_battle++;
                        }
                    }
                    total_count += ert.pathfinding_enemy_counts[loop_idx];
                }

                if (total_count == 0 && !is_battle_swirl_active()) {
                    /* No more pathfinding enemies and swirl done — disable all entities */
                    for (int16_t i = 0; i < MAX_ENTITIES; i++) {
                        if (i != 23) {
                            entities.tick_callback_hi[i] |= 0xC000;
                        }
                    }
                    ow.battle_swirl_countdown = 1;
                }
            }
        }
        return result;
    }

    case ROM_ADDR_ADVANCE_ENTITY_TOWARD_LEADER: {
        /* Port of C0D0E6 ADVANCE_ENTITY_TOWARD_LEADER.
         * 1. Classify Manhattan distance to leader — if close (0) AND pathfinding
         *    active → teleport entity to leader position.
         * 2. Otherwise compute prospective position, check collision:
         *    - Collision → reduce movement speed by 0x1000, return 0
         *    - Clear → apply prospective position, return -1 */
        *out_pc = pc;
        int16_t ent = ENT(ert.current_entity_slot);

        /* CLASSIFY_ENTITY_LEADER_DISTANCE inline (C0C363) */
        int16_t dx = (int16_t)game_state.leader_x_coord - entities.abs_x[ent];
        int16_t dy = (int16_t)game_state.leader_y_coord - entities.abs_y[ent];
        uint16_t abs_dx = (uint16_t)(dx < 0 ? -dx : dx);
        uint16_t abs_dy = (uint16_t)(dy < 0 ? -dy : dy);
        uint16_t dist = abs_dx + abs_dy;
        int16_t classification;
        if (dist > 256)      classification = 3;
        else if (dist > 160) classification = 2;
        else if (dist > 128) classification = 1;
        else                 classification = 0;

        if (classification == 0 && entities.pathfinding_states[ent] != 0) {
            /* Close + pathfinding active → teleport to leader */
            entities.abs_x[ent] = game_state.leader_x_coord;
            entities.abs_y[ent] = game_state.leader_y_coord;
            return -1;
        }

        /* CALCULATE_PROSPECTIVE_POSITION → CHECK_ENTITY_COLLISION */
        calculate_prospective_position(ert.current_entity_slot);
        uint16_t coll = check_entity_collision(
            ow.entity_movement_prospective_x, ow.entity_movement_prospective_y,
            ert.current_entity_slot, 4);
        if (coll & 0x00C0) {
            /* Collision — reduce movement speed */
            entities.movement_speeds[ent] -= 0x1000;
            return 0;
        }
        /* No collision — apply new position */
        entities.abs_x[ent] = ow.entity_movement_prospective_x;
        entities.abs_y[ent] = ow.entity_movement_prospective_y;
        return -1;
    }

    case ROM_ADDR_ADVANCE_ENTITY_PATH_POINT: {
        /* Port of C0D98F ADVANCE_ENTITY_PATH_POINT.
         * Pops the next waypoint from the entity's path, converts tile coords
         * to pixel coords with collision offsets, stores to var6 (X) and var7 (Y).
         * Returns 1 if a point was loaded, 0 if no points remain. */
        *out_pc = pc;
        int16_t ent = ENT(ert.current_entity_slot);
        if (ert.entity_path_point_counts[ent] == 0)
            return 0;

        int16_t size = entities.sizes[ent];
        uint16_t path_ptr = ert.entity_path_points[ent];
        int16_t path_y = (int16_t)read_u16_le(&ert.delivery_paths[path_ptr]);
        int16_t path_x = (int16_t)read_u16_le(&ert.delivery_paths[path_ptr + 2]);

        /* target_x = (centre_x - width) * 8 + path_x * 8 + collision_x_offset[size] */
        int16_t target_x = (int16_t)((ert.pathfinding_target_centre_x - ert.pathfinding_target_width) * 8
                         + path_x * 8
                         + entity_collision_x_offset[size]);

        /* target_y = (centre_y - height) * 8 + path_y * 8
         *            - sprite_hitbox_enable[size] + collision_y_offset[size] */
        int16_t target_y = (int16_t)((ert.pathfinding_target_centre_y - ert.pathfinding_target_height) * 8
                         + path_y * 8
                         - sprite_hitbox_enable[size]
                         + entity_collision_y_offset[size]);

        entities.var[6][ent] = target_x;
        entities.var[7][ent] = target_y;

        /* Decrement count and advance pointer to next 4-byte point */
        ert.entity_path_point_counts[ent]--;
        ert.entity_path_points[ent] = path_ptr + 4;
        return 1;
    }

    case ROM_ADDR_CLEAR_SPRITEMAP_BUFFER: {
        /* Port of C4CEB0 CLEAR_SPRITEMAP_BUFFER.
         * Clears 64 words (128 bytes) at BUFFER + $7F00. This is the entity
         * spritemap display list used by the sprite rendering pipeline. */
        *out_pc = pc;
        memset(&ert.buffer[BUF_TILE_MERGE_SLOTS], 0, BUF_TILE_MERGE_SLOTS_SIZE);
        return 0;
    }

    case ROM_ADDR_APPLY_ENTITY_PALETTE_BRIGHTNESS:
        /* Port of C47499 APPLY_ENTITY_PALETTE_BRIGHTNESS.
         * Reads entity var0 as brightness offset, applies to all 16 sub-ert.palettes
         * by reading from MAP_PALETTE_BACKUP and writing to PALETTES (offset +2 sub-ert.palettes).
         * Used by event scripts for screen darkening/brightening effects. */
        *out_pc = pc;
        apply_palette_brightness_all(entities.var[0][entity_offset]);
        return 0;

    case ROM_ADDR_UPLOAD_SPECIAL_CAST_PALETTE:
        /* C4EC6E: UPLOAD_SPECIAL_CAST_PALETTE — copies palette from
         * decompressed data in BUFFER+$7000 to OBJ palette 12.
         * Parameter is tempvar (previous callroutine return value). */
        *out_pc = pc;
        upload_special_cast_palette(scripts.tempvar[script_offset]);
        return 0;

    /* Entity fade animation callroutines (C4CB4F-C4CED8) */
    case ROM_ADDR_CLEAR_FADE_ENTITY_FLAGS:
        *out_pc = pc;
        cr_clear_fade_entity_flags();
        return 0;

    case ROM_ADDR_UPDATE_FADE_ENTITY_SPRITES:
        *out_pc = pc;
        cr_update_fade_entity_sprites();
        return 0;

    case ROM_ADDR_HIDE_FADE_ENTITY_FRAMES:
        *out_pc = pc;
        cr_hide_fade_entity_frames();
        return 0;

    case ROM_ADDR_ANIMATE_ENTITY_TILE_COPY:
        *out_pc = pc;
        return cr_animate_entity_tile_copy();

    case ROM_ADDR_ANIMATE_ENTITY_TILE_BLEND:
        *out_pc = pc;
        return cr_animate_entity_tile_blend();

    case ROM_ADDR_ANIMATE_ENTITY_TILE_MERGE:
        *out_pc = pc;
        cr_animate_entity_tile_merge();
        return 0;

    case ROM_ADDR_RELOAD_MAP:
        /* Port of RELOAD_MAP (asm/overworld/reload_map.asm).
         * Reloads the current map sector: invalidates caches, resolves music,
         * reloads tileset/arrangement/palette, and re-enables display. */
        *out_pc = pc;
        reload_map();
        return 0;

    case ROM_ADDR_LOAD_MAP_ROW_AT_SCROLL_POS: {
        /* Port of LOAD_MAP_ROW_AT_SCROLL_POS (asm/overworld/map/load_map_row_at_scroll_pos.asm).
         * Assembly loads a single map row at the given y-tile coordinate (from tempvar)
         * with x derived from BG1_X_POS >> 3. The C port refills the entire tilemap
         * instead of streaming individual rows, so we call fill via map_refresh_tilemaps.
         * Returns tempvar unchanged (assembly does TYA at end). */
        *out_pc = pc;
        uint16_t y_param = scripts.tempvar[script_offset];
        /* Reconstruct camera center from scroll position for map_refresh_tilemaps.
         * BG1 scroll = camera - viewport_center, so camera = scroll + viewport_center. */
        uint16_t cam_x = ppu.bg_hofs[0] + VIEWPORT_CENTER_X;
        uint16_t cam_y = ppu.bg_vofs[0] + VIEWPORT_CENTER_Y;
        map_refresh_tilemaps(cam_x, cam_y);
        return y_param;
    }

    case ROM_ADDR_DISPLAY_ANTI_PIRACY_SCREEN:
        /* Port of DISPLAY_ANTI_PIRACY_SCREEN (asm/system/display_antipiracy_screen.asm).
         * Shows the anti-piracy warning screen (copy protection).
         * No-op in the C port — this is not gameplay-relevant. */
        *out_pc = pc;
        return 0;

    case ROM_ADDR_LOAD_DEBUG_CURSOR_GRAPHICS:
        /* Port of LOAD_DEBUG_CURSOR_GRAPHICS (asm/system/debug/load_debug_cursor_graphics.asm).
         * Loads debug cursor sprite tiles to OBJ VRAM. No-op in the C port
         * since the debug mode uses its own rendering. */
        *out_pc = pc;
        return 0;

    case ROM_ADDR_UNDRAW_FLYOVER_TEXT:
        /* Port of UNDRAW_FLYOVER_TEXT (asm/text/undraw_flyover_text.asm).
         * Restores normal BG3 display after flyover/town name text. */
        *out_pc = pc;
        undraw_flyover_text();
        return 0;

    case ROM_ADDR_ACTIONSCRIPT_FADE_OUT_WITH_MOSAIC:
        return cr_actionscript_fade_out_with_mosaic(entity_offset, script_offset, pc, out_pc);

    case ROM_ADDR_ACTIONSCRIPT_PREPARE_AT_TELEPORT:
        return cr_actionscript_prepare_at_teleport(entity_offset, script_offset, pc, out_pc);

    case ROM_ADDR_RENDER_ENTITY_SPRITE_ME3:
        return cr_render_entity_sprite_me3(entity_offset, script_offset, pc, out_pc);

    case ROM_ADDR_SETUP_DELIVERY_PATH_FROM_ENTITY: {
        /* Port of C0C251 SETUP_DELIVERY_PATH_FROM_ENTITY.
         * Uses pathfinding to find a path TO the current entity, then copies
         * waypoints in REVERSE order into a delivery path slot.
         * Parameter: delivery slot (from tempvar, i.e. A register).
         * Returns 0 on success, 1 on pathfinding failure. */
        *out_pc = pc;
        int16_t ent = ENT(ert.current_entity_slot);
        int16_t slot = scripts.tempvar[script_offset];

        /* Set pathfinding state to -1 (request pathfinding) */
        entities.pathfinding_states[ent] = -1;

        /* Find path from offscreen TO the current entity */
        if (pathfind_to_current_entity() != 0)
            return 1;  /* pathfinding failed */

        /* Clear pathfinding state */
        entities.pathfinding_states[ent] = 0;

        /* Decrement path_point_count by 1 (we'll copy count-1 waypoints in reverse) */
        int16_t count = ert.entity_path_point_counts[ent];
        count--;
        ert.entity_path_point_counts[ent] = count;

        /* Compute offset of the second-to-last waypoint:
         * base + (count - 1) * 4  (count is already decremented, so N-2 from original) */
        uint16_t base_ptr = ert.entity_path_points[ent];
        uint16_t src_ptr  = (uint16_t)(base_ptr + (count - 1) * 4);

        /* Compute delivery slot offset: slot * 80 bytes (20 waypoints × 4 bytes) */
        uint16_t slot_offset = (uint16_t)((slot * 5) << 4);  /* slot*5*16 = slot*80 */

        /* Store delivery slot base as the new entity_path_points */
        ert.entity_path_points[ent] = slot_offset;

        /* Copy up to 20 waypoints in REVERSE order from path into delivery slot */
        uint16_t dst_ptr = slot_offset;
        int16_t  n = count;  /* number of waypoints to copy */
        int16_t  copied = 0;
        while (n > 0 && copied < 20) {
            /* Copy 4 bytes (y: 2 bytes LE, x: 2 bytes LE) */
            ert.delivery_paths[dst_ptr]     = ert.delivery_paths[src_ptr];
            ert.delivery_paths[dst_ptr + 1] = ert.delivery_paths[src_ptr + 1];
            ert.delivery_paths[dst_ptr + 2] = ert.delivery_paths[src_ptr + 2];
            ert.delivery_paths[dst_ptr + 3] = ert.delivery_paths[src_ptr + 3];
            src_ptr = (uint16_t)(src_ptr - 4);  /* step backward through source */
            dst_ptr = (uint16_t)(dst_ptr + 4);  /* step forward in delivery slot */
            n--;
            copied++;
        }

        return 0;
    }

    case ROM_ADDR_SETUP_DELIVERY_PATH_REVERSE: {
        /* Port of C0C19B SETUP_DELIVERY_PATH_REVERSE.
         * Checks if leader's sector is passable for delivery, then pathfinds
         * TO the party leader and copies waypoints in forward order into a
         * delivery path slot.
         * Parameter: delivery slot (from tempvar, i.e. A register).
         * Returns 0 on success, 1 on failure. */
        *out_pc = pc;

        /* Check if leader's sector is passable for delivery */
        uint16_t attrs = load_sector_attrs(
            game_state.leader_x_coord,
            game_state.leader_y_coord);
        uint8_t sector_type = attrs & 0x07;
        if (delivery_sector_passable_table[sector_type] == 0)
            return 1;  /* sector not passable for delivery */

        int16_t ent = ENT(ert.current_entity_slot);
        int16_t slot = scripts.tempvar[script_offset];

        /* Set pathfinding state to -1 (request pathfinding) */
        entities.pathfinding_states[ent] = -1;

        /* Find path from offscreen TO the party leader.
         * pathfind_to_party_leader() also snaps the entity position to the
         * path start and decrements path_point_count / advances path_points by 1. */
        if (pathfind_to_party_leader() != 0)
            return 1;  /* pathfinding failed */

        /* Clear pathfinding state */
        entities.pathfinding_states[ent] = 0;

        /* Read current path base (already advanced by pathfind_to_party_leader) */
        uint16_t src_ptr = ert.entity_path_points[ent];

        /* Compute delivery slot offset: slot * 80 bytes (20 waypoints × 4 bytes) */
        uint16_t slot_offset = (uint16_t)((slot * 5) << 4);  /* slot*5*16 = slot*80 */

        /* Store delivery slot base as the new entity_path_points */
        ert.entity_path_points[ent] = slot_offset;

        /* Read path_point_count (already decremented by pathfind_to_party_leader) */
        int16_t count = ert.entity_path_point_counts[ent];

        /* Copy up to 20 waypoints in FORWARD order into delivery slot */
        uint16_t dst_ptr = slot_offset;
        int16_t  copied = 0;
        while (count > 0 && copied < 20) {
            /* Copy 4 bytes (y: 2 bytes LE, x: 2 bytes LE) */
            ert.delivery_paths[dst_ptr]     = ert.delivery_paths[src_ptr];
            ert.delivery_paths[dst_ptr + 1] = ert.delivery_paths[src_ptr + 1];
            ert.delivery_paths[dst_ptr + 2] = ert.delivery_paths[src_ptr + 2];
            ert.delivery_paths[dst_ptr + 3] = ert.delivery_paths[src_ptr + 3];
            src_ptr = (uint16_t)(src_ptr + 4);  /* step forward through source */
            dst_ptr = (uint16_t)(dst_ptr + 4);  /* step forward in delivery slot */
            count--;
            copied++;
        }

        return 0;
    }

    case ROM_ADDR_SET_PHOTOGRAPHER_POSITION: {
        /* Port of C46D4B SET_PHOTOGRAPHER_POSITION.
         * Reads photographer config entry for SPAWNING_TRAVELLING_PHOTOGRAPHER_ID,
         * sets current entity's abs_x/abs_y to photographer_x/y * 8,
         * and zeroes the fractional position. */
        *out_pc = pc;
        int16_t ent = ENT(ert.current_entity_slot);

        /* Lazy-load photographer config data */
        if (!photographer_cfg_cr)
            photographer_cfg_cr = ASSET_DATA(ASSET_ENDING_PHOTOGRAPHER_CFG_BIN);
        if (!photographer_cfg_cr) {
            LOG_WARN("WARN: SET_PHOTOGRAPHER_POSITION — failed to load photographer_cfg.bin\n");
            return 0;
        }

        uint16_t photo_id = ow.spawning_travelling_photographer_id;
        const uint8_t *cfg = &photographer_cfg_cr[photo_id * PHOTOGRAPHER_CFG_ENTRY_SIZE];

        /* photographer_x at offset 10, photographer_y at offset 12 (each uint16 LE) */
        uint16_t px = read_u16_le(&cfg[PHOTOGRAPHER_CFG_PHOTOGRAPHER_X]);
        uint16_t py = read_u16_le(&cfg[PHOTOGRAPHER_CFG_PHOTOGRAPHER_Y]);

        /* Convert tile coords to pixel coords (ASL x3 = *8) */
        entities.abs_x[ent] = (int16_t)(px * 8);
        entities.abs_y[ent] = (int16_t)(py * 8);

        /* Zero fractional positions */
        entities.frac_x[ent] = 0;
        entities.frac_y[ent] = 0;
        return 0;
    }

    /* ---- Your Sanctuary display callroutines ---- */

    case ROM_ADDR_INITIALIZE_YOUR_SANCTUARY_DISPLAY:
        /* C4DE98: INITIALIZE_YOUR_SANCTUARY_DISPLAY
         * Zeros sanctuary tile tracking variables, clears the 8-entry
         * loaded-sanctuary array, and sets TM_MIRROR to $10 (OBJ only). */
        *out_pc = pc;
        ml.next_your_sanctuary_location_tile_index = 0;
        ml.total_your_sanctuary_loaded_tileset_tiles = 0;
        ml.your_sanctuary_loaded_tileset_tiles = 0;
        ml.loaded_animated_tile_count = 0;
        ml.map_palette_animation_loaded = 0;
        for (int i = 0; i < 8; i++)
            ml.loaded_your_sanctuary_locations[i] = 0;
        ppu.tm = 0x10;  /* OBJ only on main screen */
        return 0;

    case ROM_ADDR_ENABLE_YOUR_SANCTUARY_DISPLAY:
        /* C4DED0: ENABLE_YOUR_SANCTUARY_DISPLAY
         * Sets BG1 VRAM location (tilemap=$3800, tiles=$6000, size=HORIZONTAL)
         * and enables BG1+OBJ on main screen (TM_MIRROR=$11). */
        *out_pc = pc;
        /* SET_BG1_VRAM_LOCATION(Y=$6000, X=$3800, A=HORIZONTAL=1) */
        ppu.bg_sc[0] = (uint8_t)((0x3800 >> 8) | 1);  /* tilemap base $3800, horizontal */
        ppu.bg_nba[0] = (ppu.bg_nba[0] & 0xF0) | ((0x6000 >> 12) & 0x0F);  /* tile base $6000 */
        ppu.tm = 0x11;  /* BG1 + OBJ on main screen */
        return 0;

    case ROM_ADDR_DISPLAY_YOUR_SANCTUARY_LOCATION: {
        /* C4E2D7: DISPLAY_YOUR_SANCTUARY_LOCATION
         * Loads sanctuary location graphics/palette into VRAM if not already loaded,
         * then copies tilemap data and palette from BUFFER to VRAM/CGRAM.
         * Parameter is tempvar (sanctuary index, 0-7).
         *
         * Intentional divergence: assembly pre-caches all 8 sanctuaries in BUFFER
         * (20 KB). We load on demand into slot 0 (~2.2 KB), trading CPU for RAM. */
        *out_pc = pc;
        uint16_t sanctuary_idx = scripts.tempvar[script_offset] & 0x0007;

        /* Always reload — single-slot, no caching */
        load_your_sanctuary_location(sanctuary_idx);
        render_frame_tick();

        /* Copy tilemap from BUFFER slot 0 to VRAM at $3800
         * (0x780 bytes = 30 rows x 32 words = 960 tilemap entries) */
        memcpy(&ppu.vram[0x3800 * 2], &ert.buffer[BUF_SANCTUARY_TILEMAPS], 0x0780);

        /* Copy palette from BUFFER slot 0 to CGRAM.
         * Assembly: LDX #BPP4PALETTE_SIZE * 8 = 256 bytes (8 BG sub-palettes). */
        memcpy(ert.palettes, &ert.buffer[BUF_SANCTUARY_PALETTES], BPP4PALETTE_SIZE * 8);
        ert.palette_upload_mode = PALETTE_UPLOAD_BG_ONLY;

        /* Reset scroll positions */
        ml.screen_top_y = 0;
        ml.screen_left_x = 0;
        ppu.bg_vofs[0] = 0;
        ppu.bg_hofs[0] = 0;
        return 0;
    }

    /* ---- Bubble Monkey callroutine ---- */

    case ROM_ADDR_INIT_BUBBLE_MONKEY: {
        /* EF027D: INIT_BUBBLE_MONKEY
         * Initializes bubble monkey follower state:
         * - Zeros mode, sets movement change timer to 30
         * - Sets entity var3 to 4 (animation speed)
         * - Fills the char's position buffer entry with leader coords */
        *out_pc = pc;
        ow.bubble_monkey_mode = 0;
        ow.bubble_monkey_movement_change_timer = 30;
        entities.var[3][entity_offset] = 4;

        /* Get party index from entity var1, look up char_struct's position_index */
        int16_t party_idx_bm = entities.var[1][entity_offset];
        if (party_idx_bm >= 0 && party_idx_bm < TOTAL_PARTY_COUNT) {
            uint16_t pos_idx = party_characters[party_idx_bm].position_index;
            pb.player_position_buffer[pos_idx & 0xFF].x_coord = (int16_t)game_state.leader_x_coord;
            pb.player_position_buffer[pos_idx & 0xFF].y_coord = (int16_t)game_state.leader_y_coord;
        }
        return 0;
    }

    /* ---- Butterfly movement callroutines ---- */

    case ROM_ADDR_INIT_BUTTERFLY_MOVEMENT: {
        /* C0CCCC: INIT_BUTTERFLY_MOVEMENT
         * Sets up initial movement parameters for a magic butterfly entity:
         * - var6/var7 = initial position (target center)
         * - var5 = angular velocity (from speed and fixed calculation)
         * - Random initial direction (UP or DOWN)
         * - Sets orbit direction based on initial facing
         * - Clears var4 (angle accumulator) */
        *out_pc = pc;

        /* var6 = abs_x, var7 = abs_y + 16 */
        entities.var[6][entity_offset] = entities.abs_x[entity_offset];
        entities.var[7][entity_offset] = entities.abs_y[entity_offset] + 16;

        /* Calculate angular velocity: $64800 / (speed >> 4)
         * Result: XBA + AND #$FF00 → swap bytes, keep high byte. */
        {
            uint16_t speed = entities.movement_speeds[entity_offset];
            uint16_t speed_int = speed >> 4;
            uint16_t angular_vel = 0;
            if (speed_int > 0) {
                uint32_t result = 0x64800 / (uint32_t)speed_int;
                /* XBA: swap low and high bytes; AND #$FF00: keep only high byte */
                angular_vel = (uint16_t)((result & 0xFF) << 8);
            }
            entities.var[5][entity_offset] = (int16_t)angular_vel;
        }

        /* Random initial direction: 0 (UP) or 4 (DOWN) */
        if (rng_next_byte() & 1) {
            entities.directions[entity_offset] = DIRECTION_UP;
        } else {
            entities.directions[entity_offset] = DIRECTION_DOWN;
        }

        /* Set orbit direction based on facing:
         * direction < DOWN → orbit_direction = 0 (clockwise)
         * direction >= DOWN → orbit_direction = -1 (counter-clockwise) */
        if ((uint16_t)entities.directions[entity_offset] < DIRECTION_DOWN) {
            entities.butterfly_orbit_direction[entity_offset] = 0;
        } else {
            entities.butterfly_orbit_direction[entity_offset] = -1;
        }

        /* Clear angle accumulator */
        entities.var[4][entity_offset] = 0;
        return 0;
    }

    case ROM_ADDR_UPDATE_BUTTERFLY_MOVEMENT: {
        /* C0CD50: UPDATE_BUTTERFLY_MOVEMENT
         * Per-frame orbit update for butterfly entity.
         * Updates angle (var4), calculates velocity components,
         * computes delta position to orbit around var6/var7 center,
         * and returns the facing direction (angle +/- 90 degrees). */
        *out_pc = pc;
        int16_t orbit_dir = entities.butterfly_orbit_direction[entity_offset];

        /* Update angle: add or subtract angular velocity (var5) */
        int16_t bf_angle;
        if (orbit_dir == 0) {
            bf_angle = entities.var[4][entity_offset] + entities.var[5][entity_offset];
        } else {
            bf_angle = entities.var[4][entity_offset] - entities.var[5][entity_offset];
        }
        entities.var[4][entity_offset] = bf_angle;

        /* Calculate velocity components with radius $1000 */
        int16_t vel_x, vel_y;
        calculate_velocity_components((uint16_t)bf_angle, 0x1000, &vel_x, &vel_y);

        /* The assembly extracts integer and fraction parts of the velocity,
         * then does ASR8 (arithmetic shift right by 8) on each 32-bit value.
         * vel_x (integer part) → LOCAL03, vel_y (fraction part) → LOCAL04
         * After ASR8, these become the orbit offsets from center. */
        int16_t orbit_x = vel_x >> 8;
        int16_t orbit_y = vel_y >> 8;

        /* Compute delta: (center + orbit_offset) - current_position
         * All in 16.16 fixed-point */
        int32_t center_x32 = (int32_t)entities.var[6][entity_offset] << 16;
        int32_t center_y32 = (int32_t)entities.var[7][entity_offset] << 16;
        int32_t orbit_x32 = (int32_t)orbit_x << 16;
        int32_t orbit_y32 = (int32_t)orbit_y << 16;
        int32_t cur_x32 = ((int32_t)entities.abs_x[entity_offset] << 16) |
                           entities.frac_x[entity_offset];
        int32_t cur_y32 = ((int32_t)entities.abs_y[entity_offset] << 16) |
                           entities.frac_y[entity_offset];

        int32_t dx = (center_x32 + orbit_x32) - cur_x32;
        int32_t dy = (center_y32 + orbit_y32) - cur_y32;

        entities.delta_x[entity_offset] = (int16_t)(dx >> 16);
        entities.delta_frac_x[entity_offset] = (uint16_t)(dx & 0xFFFF);
        entities.delta_y[entity_offset] = (int16_t)(dy >> 16);
        entities.delta_frac_y[entity_offset] = (uint16_t)(dy & 0xFFFF);

        /* Return facing direction: angle +/- 90 degrees ($4000) */
        uint16_t face_angle;
        if (orbit_dir == 0) {
            face_angle = (uint16_t)bf_angle + 0x4000;
        } else {
            face_angle = (uint16_t)bf_angle - 0x4000;
        }
        return (int16_t)face_angle;
    }

    /* ---- Animation sequence / visual callroutines ---- */

    case ROM_ADDR_MOVEMENT_LOAD_BATTLEBG: {
        /* Port of MOVEMENT_LOAD_BATTLEBG (asm/battle/load_battlebg_movement.asm).
         * Reads 2 words from script: bg1_layer and bg2_layer.
         * Assembly: MOVEMENT_DATA_READ16 x2, then JSL LOAD_BACKGROUND_ANIMATION. */
        uint16_t bg1_layer = sw(pc);
        uint16_t bg2_layer = sw(pc + 2);
        *out_pc = pc + 4;
        load_background_animation(bg1_layer, bg2_layer);
        return 0;
    }

    case ROM_ADDR_UPDATE_SWIRL_EFFECT:
        /* Port of UPDATE_SWIRL_EFFECT (asm/misc/update_swirl_effect.asm).
         * Per-frame oval window / battle swirl animation update.
         * Already fully implemented in oval_window.c. */
        *out_pc = pc;
        update_swirl_effect();
        return 0;

    case ROM_ADDR_ADVANCE_TILEMAP_ANIMATION_FRAME: {
        /* Port of ADVANCE_TILEMAP_ANIMATION_FRAME (asm/system/dma/advance_tilemap_animation_frame-jp.asm).
         * Reads entity var1 as current frame index. Extracts an 8-row x 30-column
         * tile region from ert.buffer+$2000 (tile data strided 16 bytes per column),
         * copies to ert.buffer+2, then writes to BG3 tilemap VRAM.
         * Increments var1; returns 0 if var1 <= var0 (more frames), 1 if done.
         *
         * The source data at BUFFER+$2000 is arranged with 16 bytes between
         * consecutive tile columns. var1 odd/even selects between two $2000-byte
         * halves. Each entry is a 16-bit tilemap word. */
        *out_pc = pc;
        int16_t ataf_slot = ert.current_entity_slot;
        uint16_t ataf_var1 = (uint16_t)entities.var[1][ataf_slot];

        /* Compute source offset: base = $2000 + (var1 >> 1) * 16.
         * If var1 is odd, add another $2000. */
        uint32_t ataf_src = 0x2000 + (uint32_t)((ataf_var1 >> 1) << 4);
        if (ataf_var1 & 1)
            ataf_src += 0x2000;

        /* Copy 30 columns x 8 rows from strided source to contiguous ert.buffer+2.
         * Column stride = 16 bytes. After 30 columns, assembly subtracts 478
         * (= 30*16 - 2) to advance source by 2 bytes for the next row. */
        uint16_t ataf_dst = 2;
        uint32_t ataf_row_src = ataf_src;
        for (int ataf_r = 0; ataf_r < 8; ataf_r++) {
            uint32_t ataf_col_src = ataf_row_src;
            for (int ataf_c = 0; ataf_c < 30; ataf_c++) {
                if (ataf_col_src + 1 < BUFFER_SIZE && ataf_dst + 1 < BUFFER_SIZE) {
                    ert.buffer[ataf_dst] = ert.buffer[ataf_col_src];
                    ert.buffer[ataf_dst + 1] = ert.buffer[ataf_col_src + 1];
                }
                ataf_dst += 2;
                ataf_col_src += 16;
            }
            ataf_row_src += 2;
        }

        /* Write the 30x8 tilemap region to BG3 VRAM.
         * Assembly calls UPDATE_TILEMAP_REGION(A=808, X=588, ptr=BUFFER).
         * A=808: tile_x = 808 & 0x3F = 40 (wraps in 64-wide map)
         * X=588: tile_y = 588 & 0x1F = 12
         * Region: 30 columns x 8 rows. */
        {
            uint16_t ataf_tile_x = 808 & 0x3F;  /* 40 */
            uint16_t ataf_tile_y = 588 & 0x1F;  /* 12 */
            uint16_t ataf_si = 2;
            for (int ataf_r2 = 0; ataf_r2 < 8; ataf_r2++) {
                uint16_t y = (ataf_tile_y + ataf_r2) & 0x1F;
                for (int ataf_c2 = 0; ataf_c2 < 30; ataf_c2++) {
                    uint16_t x = ataf_tile_x + ataf_c2;
                    uint16_t vram_base = VRAM_TEXT_LAYER_TILEMAP;
                    if (x >= 32) {
                        vram_base += 0x400;  /* second tilemap half */
                        x -= 32;
                    }
                    uint16_t vram_word = vram_base + y * 32 + x;
                    uint16_t vram_byte = vram_word * 2;
                    if (ataf_si + 1 < BUFFER_SIZE && vram_byte + 1 < VRAM_SIZE) {
                        ppu.vram[vram_byte] = ert.buffer[ataf_si];
                        ppu.vram[vram_byte + 1] = ert.buffer[ataf_si + 1];
                    }
                    ataf_si += 2;
                }
            }
        }

        /* Increment var1, compare to var0 */
        entities.var[1][ataf_slot]++;
        if ((uint16_t)entities.var[1][ataf_slot] <= (uint16_t)entities.var[0][ataf_slot])
            return 0;
        return 1;
    }

    case ROM_ADDR_LOAD_ANIMATION_SEQUENCE_FRAME: {
        /* Port of LOAD_ANIMATION_SEQUENCE_FRAME (asm/misc/load_animation_sequence_frame.asm).
         * Decompresses animation data to ert.buffer, uploads tiles to VRAM
         * at TEXT_LAYER_TILES, loads palette, sets BG3_Y_POS = -1.
         *
         * var0 = animation sequence index (0-6):
         *   0=NULL, 1=lightning_reflect, 2=lightning_strike,
         *   3=starman_jr_teleport, 4=boom, 5=zombies, 6=the_end */
        *out_pc = pc;

        static const AssetId lasf_anim_assets[] = {
            0,  /* NULL placeholder */
            ASSET_GRAPHICS_ANIMATIONS_LIGHTNING_REFLECT_ANIM_LZHAL,
            ASSET_GRAPHICS_ANIMATIONS_LIGHTNING_STRIKE_ANIM_LZHAL,
            ASSET_GRAPHICS_ANIMATIONS_STARMAN_JR_TELEPORT_ANIM_LZHAL,
            ASSET_GRAPHICS_ANIMATIONS_BOOM_ANIM_LZHAL,
            ASSET_GRAPHICS_ANIMATIONS_ZOMBIES_ANIM_LZHAL,
            ASSET_GRAPHICS_ANIMATIONS_THE_END_ANIM_LZHAL,
        };

        int16_t lasf_ent = ENT(ert.current_entity_slot);
        uint16_t lasf_id = (uint16_t)entities.var[0][lasf_ent];

        if (lasf_id == 0 || lasf_id > 6) {
            LOG_WARN("WARN: LOAD_ANIMATION_SEQUENCE_FRAME: invalid anim_id %d\n", lasf_id);
            return 0;
        }

        size_t lasf_comp_sz = ASSET_SIZE(lasf_anim_assets[lasf_id]);
        const uint8_t *lasf_comp = ASSET_DATA(lasf_anim_assets[lasf_id]);
        if (!lasf_comp) return 0;

        size_t lasf_dec_sz = decomp(lasf_comp, lasf_comp_sz, ert.buffer, BUFFER_SIZE);
        if (lasf_dec_sz == 0) return 0;

        /* Decompressed data layout (no header — tile_size from metadata table):
         * [0 .. tile_size-1]: tile graphics
         * [tile_size .. tile_size+7]: palette (BPP2PALETTE_SIZE = 8 bytes, 4 colors)
         * [tile_size+8 ..]: frame tilemap data (1792 bytes per frame) */
        uint16_t lasf_tile_size = read_u16_le(anim_seq_meta[lasf_id]);

        /* Upload tile graphics to VRAM::TEXT_LAYER_TILES */
        if (lasf_tile_size > 0 && (uint32_t)lasf_tile_size <= lasf_dec_sz) {
            uint16_t lasf_vram = VRAM_TEXT_LAYER_TILES * 2;
            uint16_t lasf_len = lasf_tile_size;
            if (lasf_vram + lasf_len > VRAM_SIZE)
                lasf_len = VRAM_SIZE - lasf_vram;
            memcpy(&ppu.vram[lasf_vram], &ert.buffer[0], lasf_len);
        }

        /* Load palette (BPP2PALETTE_SIZE = 8 bytes) after tile data */
        uint32_t lasf_pal_off = lasf_tile_size;
        if (lasf_pal_off + BPP2PALETTE_SIZE <= lasf_dec_sz) {
            memcpy(ert.palettes, &ert.buffer[lasf_pal_off], BPP2PALETTE_SIZE);
            ert.palettes[0] = 0;
            ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
        }

        /* Set BG3_Y_POS = -1 */
        ppu.bg_vofs[2] = 0xFFFF;
        return 0;
    }

    case ROM_ADDR_DISPLAY_ANIMATION_SEQUENCE_FRAME: {
        /* Port of DISPLAY_ANIMATION_SEQUENCE_FRAME (asm/misc/display_animation_sequence_frame.asm).
         * Sets BG3_Y_POS = -1, uploads a 1792-byte frame tilemap to VRAM
         * at TEXT_LAYER_TILEMAP from the decompressed animation data in ert.buffer.
         *
         * var0 = animation sequence index, var1 = current frame number.
         * Returns: 0 if last frame, otherwise delay (frames to wait). */
        *out_pc = pc;

        ppu.bg_vofs[2] = 0xFFFF;

        int16_t dasf_ent = ENT(ert.current_entity_slot);
        uint16_t dasf_id = (uint16_t)entities.var[0][dasf_ent];
        uint16_t dasf_frame = (uint16_t)entities.var[1][dasf_ent];

        if (dasf_id == 0 || dasf_id > 6) return 0;

        /* Tile data size from metadata table */
        uint16_t dasf_tile_size = read_u16_le(anim_seq_meta[dasf_id]);

        /* Frame tilemap data offset: after tiles + palette (BPP2PALETTE_SIZE=8).
         * Assembly multiplies frame by 1792 to get frame-specific tilemap. */
        uint32_t dasf_base = dasf_tile_size + BPP2PALETTE_SIZE;
        uint32_t dasf_frame_off = dasf_base + (uint32_t)dasf_frame * 1792;

        /* Upload 1792 bytes (0x700) to VRAM::TEXT_LAYER_TILEMAP */
        uint16_t dasf_vram = VRAM_TEXT_LAYER_TILEMAP * 2;
        if (dasf_frame_off + 1792 <= BUFFER_SIZE) {
            uint16_t dasf_len = 1792;
            if (dasf_vram + dasf_len > VRAM_SIZE)
                dasf_len = VRAM_SIZE - dasf_vram;
            memcpy(&ppu.vram[dasf_vram], &ert.buffer[dasf_frame_off], dasf_len);
        }

        /* Check if last frame: frame_count is at metadata byte [2] */
        uint8_t dasf_count = anim_seq_meta[dasf_id][2];
        if (dasf_frame + 1 >= dasf_count)
            return 0;  /* last frame */

        /* Return delay value from metadata byte [3] */
        return anim_seq_meta[dasf_id][3];
    }

    case ROM_ADDR_LOAD_CHARACTER_PORTRAIT_SCREEN: {
        /* Port of LOAD_CHARACTER_PORTRAIT_SCREEN (asm/misc/load_character_portrait_screen.asm).
         *
         * US version flow:
         *   1. Call RENDER_CHARACTER_NAME_DISPLAY to render VWF tiles into ert.buffer
         *      (tile layout: block b at ert.buffer[b*512], lower at +256 within block).
         *   2. Clear 29×8 words at ert.buffer+$4000 (initial scroll clear).
         *   3. Decode portrait header (4 tiles) via DECODE_PLANAR_TILEMAP → ert.buffer+$4000.
         *   4. Decode name tiles (name_len tiles).
         *   5. Decode stats tiles (6 tiles).
         *   6. Decode portrait body (205 tiles).
         *   7. Clear 30 trailing rows in ert.buffer+$4000.
         *   8. Init ert.buffer+$2000 header (8 words = $0C10).
         *      [NOTE: US assembly writes to $1000, but C port uses JP ADVANCE_TILEMAP
         *       which reads from $2000, so output goes to $2000 here.]
         *   9. Interleave ert.buffer+$4000 → ert.buffer+$2000 for all decoded rows.
         *  10. Set entity var0 = total_rows * 2.
         *  11. Write ert.buffer[0]=8, ert.buffer[1]=30 (tilemap header). */
        *out_pc = pc;

        int16_t lcps_ent = ENT(ert.current_entity_slot);

        /* Step 1: Render VWF text into ert.buffer (block tile layout).
         * render_character_name_display() fills ert.buffer[0..~0x3FFF] with
         * 2bpp tile data in the layout expected by decode_planar_tilemap(). */
        render_character_name_display();

        /* Compute name length for portrait header decode count */
        int lcps_name_len = 0;
        while (lcps_name_len < 5 && party_characters[0].name[lcps_name_len])
            lcps_name_len++;
        if (lcps_name_len > 5) lcps_name_len = 5;

        /* Step 2: Clear 29×8 words at ert.buffer+$4000 (matches assembly clear loop).
         * Assembly loops VIRTUAL06 through BUFFER+$4000, advancing past the 29-row region.
         * The decode destination starts AFTER this cleared area. */
        memset(&ert.buffer[BUF_NAME_TILEMAP], 0, 29 * 8 * 2);

        /* Set up decode destination pointer AFTER the initial 29-row clear
         * (29 rows × 8 uint16/row × 2 bytes/uint16 = 464 bytes) */
        uint16_t *tile_dest = (uint16_t *)&ert.buffer[BUF_NAME_TILEMAP + 29 * 8 * 2];

        /* Step 3: Decode portrait header tiles (4 tiles, sequential indices 0..3) */
        for (int i = 0; i < 4; i++) {
            decode_planar_tilemap((uint8_t)i, &tile_dest);
        }

        /* Step 4: Decode name tiles (lcps_name_len tiles) */
        for (int i = 0; i < lcps_name_len; i++) {
            decode_planar_tilemap((uint8_t)(4 + i), &tile_dest);
        }

        /* Step 5: Decode stats tiles (6 tiles) */
        for (int i = 0; i < 6; i++) {
            decode_planar_tilemap((uint8_t)(4 + lcps_name_len + i), &tile_dest);
        }

        /* Step 6: Decode portrait body tiles (205 tiles) */
        for (int i = 0; i < 205; i++) {
            decode_planar_tilemap((uint8_t)(4 + lcps_name_len + 6 + i), &tile_dest);
        }

        /* Step 7: Clear 30 trailing rows (8 words per row = 16 bytes per row) */
        int trailing_words = 30 * 8;
        for (int i = 0; i < trailing_words; i++) {
            *tile_dest++ = 0;
        }

        /* Compute total_scroll_rows: total interleave rows covering all data in
         * ert.buffer+$4000.
         *   - 29 initial clear rows (each 8 uint16 = 16 bytes)
         *   - decoded tiles: (4 + name_len + 6 + 205) tiles × 4 rows per tile
         *   - 30 trailing clear rows
         * Each DECODE_PLANAR_TILEMAP call writes 32 uint16 = 4 "interleave rows" of 8.
         * The interleave processes all rows so ADVANCE_TILEMAP can show the full scroll. */
        int lcps_n_tiles = 4 + lcps_name_len + 6 + 205;
        uint16_t total_scroll_rows = 29 + (uint16_t)(lcps_n_tiles * 4) + 30;

        /* Step 8: Clear ert.buffer+$2000 region (2000 bytes is ample for scroll data).
         * Assembly writes $0C10 to first 8 words, but the interleave immediately overwrites
         * those. We clear the whole region so unwritten slots default to $0C10 offset 0. */
        memset(&ert.buffer[BUF_NAME_TILES], 0, 0x2000);  /* clear only the $2000 region */

        /* Step 9: Interleave ert.buffer+$4000 → ert.buffer+$2000.
         * Port of the INTERLEAVE_ROW / INTERLEAVE_COLUMN loop from the US assembly
         * (lines 179-215 of load_character_portrait_screen.asm), adapted to write
         * to $2000 instead of $1000 for JP ADVANCE_TILEMAP compatibility.
         *
         * Source layout: ert.buffer+$4000, 8 uint16 per row, stride 16 bytes.
         * Row pairs: upper = source[i], lower = source[i+16 bytes] (i.e. source[i+8]).
         *
         * Assembly loop condition: LOCAL04 < @VIRTUAL04 + 30 where @VIRTUAL04 is
         * pixel-width-based; C port uses total_scroll_rows directly. */
        uint16_t *src_base = (uint16_t *)&ert.buffer[BUF_NAME_TILEMAP];
        uint16_t *dst_base = (uint16_t *)&ert.buffer[BUF_NAME_TILES];

        /* Interleave loop: process all data rows.
         * Stop one short of total so the last "lower" read (row j+1) stays in bounds. */
        uint16_t interleave_rows = total_scroll_rows > 0 ? total_scroll_rows - 1 : 0;
        for (uint16_t row = 0; row < interleave_rows; row++) {
            for (int col = 0; col < 8; col++) {
                /* Read two uint16 values 16 bytes (8 uint16) apart from source */
                uint16_t upper = src_base[col];      /* src_base[col] */
                uint16_t lower = src_base[col + 8];  /* src_base[col + 16 bytes] */

                /* Interleave: LSR lower, AND #5; ASL upper, AND #0xA; OR */
                uint16_t lbits = (lower >> 1) & 0x0005;
                uint16_t ubits = (upper << 1) & 0x000A;
                uint16_t combined = ubits | lbits;

                /* Store combined + $0C10 to tilemap destination */
                *dst_base++ = combined + 0x0C10;

                /* Update source: src_base[col] += $0C10 (for next row's tile index) */
                src_base[col] += 0x0C10;
            }
            /* Advance source by 8 uint16 (= 16 bytes, matching assembly INC *8 + INC *8) */
            src_base += 8;
        }

        /* Step 10: Set entity var0 = (total_scroll_rows - 1) * 2.
         * ADVANCE_TILEMAP increments var1 each call and stops when var1 > var0.
         * var1>>1 is the frame index into ert.buffer+$2000; last useful frame =
         * total_scroll_rows - 1 (last row with valid interleaved data). */
        entities.var[0][lcps_ent] = (int16_t)((total_scroll_rows - 1) * 2);

        /* Step 11: Write buffer header: BUFFER[0]=8 (rows), BUFFER[1]=30 (cols) */
        ert.buffer[0] = 8;
        ert.buffer[1] = 30;

        return 0;
    }

    default:
        LOG_WARN("WARN: unknown callroutine ROM address $%06X (entity=%d script=%d "
              "script_id=%d pc=0x%04X bank=%d)\n",
              rom_addr, ert.current_entity_slot, script_offset,
              entities.script_table[entity_offset],
              pc, cr_bank_idx);
        *out_pc = pc;
        return 0;
    }
}
