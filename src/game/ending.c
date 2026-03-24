/*
 * Ending sequence — cast scene + staff credits.
 *
 * Port of the asm/ending/ assembly files for EarthBound (US retail).
 * See ending.h for full list of ported routines.
 */

#include "game/ending.h"
#include "game/overworld.h"
#include "game/text.h"
#include "game/game_state.h"
#include "game/battle.h"
#include "game/fade.h"
#include "game/flyover.h"
#include "game/map_loader.h"
#include "game/window.h"
#include "game/position_buffer.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "entity/script.h"
#include "core/decomp.h"
#include "data/assets.h"
#include "snes/ppu.h"
#include "include/constants.h"
#include "data/event_script_data.h"
#include "include/binary.h"
#include <string.h>

/* =====================================================================
 *  State variables (ports of RAM globals from ram.asm)
 * ===================================================================== */

/* Credits state */
static uint16_t credits_current_row;
static uint16_t credits_dma_queue_start;
static uint16_t credits_dma_queue_end;
static uint16_t credits_next_credit_position;
static uint16_t credits_scroll_position_frac;  /* fixed_point::fraction */
static uint16_t credits_scroll_position_int;   /* fixed_point::integer */
static uint16_t credits_row_wipe_threshold;

/* Credits script data pointer — offset into the loaded staff_text asset */
static const uint8_t *credits_script_ptr;

/* Cast scene state */
static uint16_t cast_text_row;
uint16_t cast_tile_offset;
static uint16_t initial_cast_entity_sleep_frames;

/* FORCE_NORMAL_FONT_FOR_LENGTH_CALCULATIONS */
static uint8_t force_normal_font_for_length_calc;

/* Loaded assets */
static const uint8_t *photographer_cfg_data;
static size_t photographer_cfg_size;
static const uint8_t *cast_seq_formatting_data;
static const uint8_t *staff_text_data;
static size_t staff_text_size;
static const uint8_t *party_cast_tile_ids_data;
static const uint8_t *guardian_text_data;

/* Photograph map loading state — checked by map loader during photo rendering. */
uint16_t photograph_map_loading_mode;
uint16_t cur_photo_display;

/* Credits DMA queue: 128 entries × 9 bytes each = 1152 bytes.
 * Assembly stores this in PLAYER_POSITION_BUFFER (which is unused during
 * ending). We overlay onto pb.player_position_buffer (3072 bytes) to match. */
#define CREDITS_DMA_QUEUE_SIZE 128
#define CREDITS_DMA_ENTRY_SIZE 9
#define credits_dma_queue ((uint8_t *)pb.player_position_buffer)

/* Side array to store source pointers for the credits DMA queue.
 * The queue entry's 4-byte src field (bytes 3-6) was used to store a raw
 * pointer, which truncates on 64-bit systems. Instead, we store the pointer
 * here indexed by queue slot, and the queue entry's src field is unused. */
static const uint8_t *credits_dma_src_ptrs[CREDITS_DMA_QUEUE_SIZE];

/* Per-frame callback function pointer.
 * Defaults to process_overworld_tasks, matching the assembly's
 * SET_IRQ_CALLBACK in initialize_overworld_state.asm. Credits swap
 * this to credits_scroll_frame, then restore it back. */
frame_callback_fn frame_callback = NULL;  /* set to process_overworld_tasks by init_overworld */

/* =====================================================================
 *  Helper: VRAM copy (replacement for PREPARE_VRAM_COPY)
 *
 *  In the assembly, PREPARE_VRAM_COPY queues a DMA to VRAM.
 *  In the C port, we write directly to ppu.vram[].
 *  mode: 0 = normal byte copy, 3 = word fill
 * ===================================================================== */
static void vram_copy(const uint8_t *src, uint16_t vram_word_addr,
                      uint16_t size_bytes, uint8_t mode) {
    uint32_t byte_addr = (uint32_t)vram_word_addr * 2;
    if (mode == 3) {
        /* Word fill: replicate the first 2 bytes */
        uint8_t lo = src[0], hi = src[1];
        for (uint16_t i = 0; i < size_bytes; i += 2) {
            if (byte_addr + i + 1 < VRAM_SIZE) {
                ppu.vram[byte_addr + i]     = lo;
                ppu.vram[byte_addr + i + 1] = hi;
            }
        }
    } else {
        if (byte_addr + size_bytes <= VRAM_SIZE) {
            memcpy(&ppu.vram[byte_addr], src, size_bytes);
        }
    }
}

/* =====================================================================
 *  Helper: set BG VRAM locations (ports of SET_BG*_VRAM_LOCATION)
 *  tilemap_size: 0=NORMAL, 1=HORIZONTAL, 2=VERTICAL, 3=BOTH
 *  tilemap_base: VRAM word address of tilemap
 *  tile_base: VRAM word address of tile data
 * ===================================================================== */
static void set_bg1_vram_location(uint16_t tilemap_size, uint16_t tilemap_base,
                                   uint16_t tile_base) {
    ppu.bg_sc[0] = (uint8_t)((tilemap_base >> 8) | tilemap_size);
    ppu.bg_nba[0] = (ppu.bg_nba[0] & 0xF0) | ((tile_base >> 12) & 0x0F);
}

static void set_bg2_vram_location(uint16_t tilemap_size, uint16_t tilemap_base,
                                   uint16_t tile_base) {
    ppu.bg_sc[1] = (uint8_t)((tilemap_base >> 8) | tilemap_size);
    ppu.bg_nba[0] = (ppu.bg_nba[0] & 0x0F) | ((tile_base >> 8) & 0xF0);
}

static void set_bg3_vram_location(uint16_t tilemap_size, uint16_t tilemap_base,
                                   uint16_t tile_base) {
    ppu.bg_sc[2] = (uint8_t)((tilemap_base >> 8) | tilemap_size);
    ppu.bg_nba[1] = (ppu.bg_nba[1] & 0xF0) | ((tile_base >> 12) & 0x0F);
}

/* =====================================================================
 *  Helper: set OAM size (port of SET_OAM_SIZE)
 * ===================================================================== */
static void set_oam_size(uint16_t obsel) {
    ppu.obsel = (uint8_t)obsel;
}

/* =====================================================================
 *  CHANGE_VWF_2BPP_TO_3_COLOUR (asm/ending/change_vwf_2bpp_to_3_colour.asm)
 *
 *  Processes VWF ert.buffer: for each pair of bytes (plane0, plane1),
 *  examines each bit position. If both planes set, clears both.
 *  Otherwise: new_plane0 = old_plane0, new_plane1 = old_plane1.
 *  This converts 2bpp text into 3-colour (max) for cast name rendering.
 * ===================================================================== */
static void change_vwf_2bpp_to_3_colour(uint16_t tile_count) {
    uint16_t total_pairs = tile_count * 16; /* 16 byte-pairs per tile */
    for (uint16_t i = 0; i < total_pairs; i++) {
        uint8_t p0 = vwf_buffer[i * 2];
        uint8_t p1 = vwf_buffer[i * 2 + 1];
        uint8_t new_p0 = 0, new_p1 = 0;
        for (int bit = 7; bit >= 0; bit--) {
            new_p0 <<= 1;
            new_p1 <<= 1;
            uint8_t b0 = (p0 >> bit) & 1;
            uint8_t b1 = (p1 >> bit) & 1;
            if (b0 && b1) {
                /* Both set: clear both bits */
            } else {
                new_p0 |= b0;
                new_p1 |= b1;
            }
        }
        vwf_buffer[i * 2] = new_p0;
        vwf_buffer[i * 2 + 1] = new_p1;
    }
}

/* =====================================================================
 *  center_vwf_for_string (port of C1FF99)
 *
 *  Centers a string within a given width (in tiles) by measuring its
 *  pixel width and setting VWF_X and VWF_TILE accordingly.
 * ===================================================================== */
static void center_vwf_for_string(const uint8_t *str, uint16_t width_tiles,
                                   int16_t font_id_or_neg1) {
    /* Measure string pixel width */
    uint8_t fid = (font_id_or_neg1 < 0) ? FONT_ID_NORMAL : (uint8_t)font_id_or_neg1;
    uint16_t total_width = 0;
    for (const uint8_t *p = str; *p != 0; p++) {
        uint8_t idx = *p - 0x50;
        if (idx < FONT_CHAR_COUNT) {
            total_width += font_get_width(fid, idx) + character_padding;
        }
    }
    /* Center within width_tiles * 8 pixels */
    uint16_t area_width = width_tiles * 8;
    uint16_t pixel_x = 0;
    if (area_width > total_width) {
        pixel_x = (area_width - total_width) / 2;
    }
    vwf_x = pixel_x;
    vwf_tile = pixel_x >> 3;
}

/* =====================================================================
 *  RENDER_CAST_NAME_TEXT (asm/ending/render_cast_name_text.asm)
 *
 *  Renders an EB-encoded string into VWF ert.buffer, converts 2bpp->3colour,
 *  then copies the VWF tiles into BUFFER at tile positions defined by
 *  tile_start_index.
 *
 *  Parameters:
 *    str_addr: pointer to EB-encoded null-terminated string (in WRAM)
 *    tile_count: number of VWF tiles to render
 *    tile_start_index: starting tile index in BUFFER for output
 * ===================================================================== */
static void render_cast_name_text(const uint8_t *str_addr, uint16_t tile_count,
                                   uint16_t tile_start_index) {
    /* Assembly lines 23-31: init VWF state */
    vwf_tile = 0;
    vwf_x = 0;
    /* Fill VWF ert.buffer with 0xFF (assembly: MEMSET16 with #$FF) */
    memset(vwf_buffer, 0xFF, 32 * 26);

    /* Center the string */
    center_vwf_for_string(str_addr, tile_count, -1);

    /* Render each character using VWF */
    for (const uint8_t *p = str_addr; *p != 0; p++) {
        uint8_t eb_char = *p;
        uint8_t idx = eb_char - 0x50;
        if (idx >= FONT_CHAR_COUNT) continue;

        /* Get glyph data and width */
        const uint8_t *glyph = font_get_glyph(FONT_ID_NORMAL, idx);
        uint8_t width = font_get_width(FONT_ID_NORMAL, idx) + character_padding;
        uint8_t height = font_get_height(FONT_ID_NORMAL);

        /* Blit glyph — assembly does multi-column blitting for wide glyphs */
        blit_vwf_glyph(glyph, height, width);
    }

    /* Convert 2bpp to 3-colour */
    change_vwf_2bpp_to_3_colour(tile_count);

    /* Copy VWF tiles directly to ppu.vram at tile positions.
     * VRAM_CAST_TILES = 0x0000, so byte offsets match the assembly's BUFFER layout.
     * Assembly: each tile's upper 8x8 goes to offset [tile_pos * 16],
     * lower 8x8 goes to offset [tile_pos * 16 + 256]. */
    uint8_t *cast_vram = &ppu.vram[VRAM_CAST_TILES * 2];
    uint16_t src_tile = 0;
    uint16_t tile_idx = tile_start_index;
    for (uint16_t t = 0; t < tile_count; t++) {
        /* Compute destination offset using tile index decomposition:
         * row = tile_idx >> 4, col = tile_idx & 0xF
         * dest = (row * 32 + col) * 16 */
        uint16_t row = (tile_idx & 0x03F0) >> 4;
        uint16_t col = tile_idx & 0x000F;
        uint16_t dest_offset = ((row * 32) + col) * 16;

        /* Copy upper 8x8 (16 bytes) from VWF buffer */
        uint16_t vwf_offset = src_tile * 32;
        memcpy(&cast_vram[dest_offset], &vwf_buffer[vwf_offset], 16);

        /* Copy lower 8x8 (16 bytes) at + 256 */
        memcpy(&cast_vram[dest_offset + 256], &vwf_buffer[vwf_offset + 16], 16);

        src_tile++;
        tile_idx++;
    }
}

/* =====================================================================
 *  PREPARE_DYNAMIC_CAST_NAME_TEXT (asm/ending/prepare_dynamic_cast_name_text.asm)
 *
 *  Pre-renders party member names + guardian texts into BUFFER tile data
 *  using RENDER_CAST_NAME_TEXT. Called during LOAD_CAST_SCENE.
 *
 *  US version: renders 4 party member names at PARTY_MEMBER_CAST_TILE_IDS,
 *  pet name at tile 448, and 3 guardian texts (Paula's dad, mom, Poo's Master).
 * ===================================================================== */
static void prepare_dynamic_cast_name_text(void) {
    if (!party_cast_tile_ids_data) return;

    uint8_t name_buf[16];

    /* Render 4 party member names */
    for (int i = 0; i < 4; i++) {
        memset(name_buf, 0, 16);
        /* Copy 5-byte name from party characters */
        memcpy(name_buf, party_characters[i].name, 5);

        uint16_t tile_id = read_u16_le(&party_cast_tile_ids_data[i * 2]);
        render_cast_name_text(name_buf, 6, tile_id);
    }

    /* Render pet name at tile 448 */
    memset(name_buf, 0, 16);
    memcpy(name_buf, game_state.pet_name, 6);
    render_cast_name_text(name_buf, 6, 448);

    if (!cast_seq_formatting_data || !guardian_text_data) return;

    /* Guardian text entries use CAST_SEQUENCE_FORMATTING offsets 39, 36, 108
     * for Paula's dad, Paula's mom, Poo's Master respectively.
     * Each formatting entry is 3 bytes: 2-byte tile position + 1-byte width. */

    /* Paula's dad: CAST_SEQUENCE_FORMATTING[13] (offset 39) */
    {
        memset(name_buf, 0, 16);
        /* Copy Paula's name (party member 1, offset 0 = name) */
        memcpy(name_buf, party_characters[1].name, 5);
        /* Append "'s dad" from guardian_text_data[0..6] */
        int len = 0;
        while (name_buf[len] != 0 && len < 5) len++;
        const uint8_t *gt1 = &guardian_text_data[0]; /* CHARACTER_GUARDIAN_TEXT_1 */
        for (int j = 0; gt1[j] != 0 && len + j < 15; j++) {
            name_buf[len + j] = gt1[j];
        }

        uint16_t tile_pos = read_u16_le(&cast_seq_formatting_data[39]);
        uint8_t width = cast_seq_formatting_data[41];
        render_cast_name_text(name_buf, width, tile_pos);
    }

    /* Paula's mom: CAST_SEQUENCE_FORMATTING[12] (offset 36) */
    {
        memset(name_buf, 0, 16);
        memcpy(name_buf, party_characters[1].name, 5);
        int len = 0;
        while (name_buf[len] != 0 && len < 5) len++;
        const uint8_t *gt2 = &guardian_text_data[7]; /* CHARACTER_GUARDIAN_TEXT_2 */
        for (int j = 0; gt2[j] != 0 && len + j < 15; j++) {
            name_buf[len + j] = gt2[j];
        }

        uint16_t tile_pos = read_u16_le(&cast_seq_formatting_data[36]);
        uint8_t width = cast_seq_formatting_data[38];
        render_cast_name_text(name_buf, width, tile_pos);
    }

    /* Poo's Master: CAST_SEQUENCE_FORMATTING[36] (offset 108) */
    {
        memset(name_buf, 0, 16);
        /* Copy Poo's name (party member 3) */
        memcpy(name_buf, party_characters[3].name, 5);
        int len = 0;
        while (name_buf[len] != 0 && len < 5) len++;
        const uint8_t *gt3 = &guardian_text_data[14]; /* CHARACTER_GUARDIAN_TEXT_3 */
        for (int j = 0; gt3[j] != 0 && len + j < 15; j++) {
            name_buf[len + j] = gt3[j];
        }

        uint16_t tile_pos = read_u16_le(&cast_seq_formatting_data[108]);
        uint8_t width = cast_seq_formatting_data[110];
        render_cast_name_text(name_buf, width, tile_pos);
    }
}

/* =====================================================================
 *  PREPARE_CAST_NAME_TILEMAP (asm/ending/prepare_cast_name_tilemap.asm)
 *
 *  Writes tilemap entries for a cast name into BUFFER+$4000.
 *  Each tile gets two entries: top row and bottom row (64 bytes apart).
 *  Returns the number of tiles written (tile_count parameter, decremented).
 * ===================================================================== */
static uint16_t prepare_cast_name_tilemap(uint16_t start_tile, uint16_t col_offset,
                                           uint16_t tile_count) {
    uint16_t *tilemap = (uint16_t *)&ert.buffer[BUF_CREDITS_TILEMAP];
    uint16_t out_idx = col_offset;
    uint16_t tile = start_tile;

    for (uint16_t i = 0; i < tile_count; i++) {
        /* Decompose tile index: row = (tile & 0x3F0) >> 4, col = tile & 0xF
         * Tile number = row * 32 + col + cast_tile_offset */
        uint16_t row = (tile & 0x03F0);
        uint16_t col = tile & 0x000F;
        uint16_t tile_num = (row << 1) + col + cast_tile_offset;

        /* Top row entry */
        tilemap[out_idx] = tile_num;
        /* Bottom row entry (32 words = 64 bytes later in ert.buffer) */
        tilemap[out_idx + 32] = tile_num + 16;

        out_idx++;
        tile++;
    }

    return tile_count;
}

/* =====================================================================
 *  COPY_CAST_NAME_TILEMAP (asm/ending/copy_cast_name_tilemap.asm)
 *
 *  Copies prepared tilemap data from BUFFER+$4000 to VRAM (cast tilemap).
 *  Handles BG3 scrolling wrap-around for the 32-row tilemap.
 * ===================================================================== */
static void copy_cast_name_tilemap(uint16_t x_col, uint16_t y_row,
                                    uint16_t tile_count) {
    /* Compute tilemap row from BG3_Y_POS + y_row, wrapped to 32 rows */
    uint16_t scroll_row = (ppu.bg_vofs[2] >> 3) + y_row;
    scroll_row &= 0x1F;

    /* Compute VRAM word address for the top row:
     * base = VRAM_CAST_TILEMAP, row offset = scroll_row * 32, col offset = x_col */
    uint16_t half_count = (tile_count + 1) / 2;
    uint16_t vram_top = (scroll_row << 5) + x_col + VRAM_CAST_TILEMAP - half_count;

    /* Source: BUFFER+$4000 at x_col offset */
    uint8_t *src_top = &ert.buffer[BUF_CREDITS_TILEMAP + x_col * 2];
    vram_copy(src_top, vram_top, tile_count * 2, 0);

    /* Bottom row: next tilemap row (wrap at 31) */
    uint16_t vram_bot;
    if (scroll_row == 31) {
        vram_bot = vram_top - 0x03E0;
    } else {
        vram_bot = vram_top + 32;
    }

    /* Source for bottom row: BUFFER+$4000 + 64 bytes (32 words) offset */
    uint8_t *src_bot = &ert.buffer[BUF_CREDITS_TILEMAP + 64 + x_col * 2];
    vram_copy(src_bot, vram_bot, tile_count * 2, 0);
}

/* =====================================================================
 *  PRINT_CAST_NAME (asm/ending/print_cast_name.asm)
 *
 *  Reads cast_index from CAST_SEQUENCE_FORMATTING to get tile position
 *  and width, then prepares and copies the tilemap.
 * ===================================================================== */
void print_cast_name(uint16_t cast_index, uint16_t x_col, uint16_t y_row) {
    if (!cast_seq_formatting_data) return;

    /* Each entry is 3 bytes: 2-byte tile position + 1-byte width */
    const uint8_t *entry = &cast_seq_formatting_data[cast_index * 3];
    uint16_t tile_pos = read_u16_le(entry);
    uint8_t width = entry[2];

    uint16_t count = prepare_cast_name_tilemap(tile_pos, x_col, width);
    copy_cast_name_tilemap(x_col, y_row, count);
}

/* =====================================================================
 *  PRINT_CAST_NAME_ENTITY_VAR0 (asm/ending/print_cast_name_entity_var0.asm)
 *
 *  US version: just reads var0 as cast_index and calls print_cast_name.
 * ===================================================================== */
void print_cast_name_entity_var0(uint16_t cast_index, uint16_t x_col,
                                  uint16_t y_row, uint16_t current_entity_slot) {
    /* US version: var0 is the cast_index */
    int16_t ent_off = current_entity_slot;
    uint16_t var0 = entities.var[0][ent_off];
    print_cast_name(var0, x_col, y_row);
}

/* =====================================================================
 *  PRINT_CAST_NAME_PARTY (asm/ending/print_cast_name_party.asm)
 *
 *  US version: looks up party member or pet name tile IDs, then calls
 *  prepare_cast_name_tilemap + copy_cast_name_tilemap.
 * ===================================================================== */
void print_cast_name_party(uint16_t char_id, uint16_t x_col, uint16_t y_row) {
    if (!party_cast_tile_ids_data) return;

    uint16_t tile_pos;
    uint16_t width;

    if (char_id == 7) {
        /* Pet name: tile 448, width 6 */
        tile_pos = 448;
        width = 6;
    } else {
        /* Party member: look up from PARTY_MEMBER_CAST_TILE_IDS */
        if (char_id < 1 || char_id > 6)
            FATAL("print_cast_name_party: invalid char_id %u\n", char_id);
        uint16_t idx = char_id - 1;
        tile_pos = read_u16_le(&party_cast_tile_ids_data[idx * 2]);
        width = 6;  /* UNK_SIZE = 6 (US) */
    }

    uint16_t count = prepare_cast_name_tilemap(tile_pos, x_col, width);
    copy_cast_name_tilemap(x_col, y_row, count);
}

/* =====================================================================
 *  UPLOAD_SPECIAL_CAST_PALETTE (asm/ending/upload_special_cast_palette.asm)
 *
 *  Copies a 32-byte palette from BUFFER+$7000 offset to OBJ palette 12.
 * ===================================================================== */
void upload_special_cast_palette(uint16_t palette_index) {
    uint16_t offset = palette_index * 32;
    uint8_t *src = &ert.buffer[BUF_CREDITS_PALETTE + offset];
    memcpy(&ert.palettes[12 * 16], src, BPP4PALETTE_SIZE);
    ert.palette_upload_mode = 16; /* PALETTE_UPLOAD_OBJ_ONLY */
}

/* =====================================================================
 *  SET_CAST_SCROLL_THRESHOLD (asm/ending/set_cast_scroll_threshold.asm)
 * ===================================================================== */
void set_cast_scroll_threshold(uint16_t param, uint16_t current_entity_slot) {
    int16_t ent_off = current_entity_slot;
    entities.var[0][ent_off] = (int16_t)(param * 8 + ppu.bg_vofs[2]);
}

/* =====================================================================
 *  CHECK_CAST_SCROLL_THRESHOLD (asm/ending/check_cast_scroll_threshold.asm)
 * ===================================================================== */
uint16_t check_cast_scroll_threshold(uint16_t current_entity_slot) {
    int16_t ent_off = current_entity_slot;
    int16_t threshold = entities.var[0][ent_off];
    if (threshold <= (int16_t)ppu.bg_vofs[2]) {
        return 1;
    }
    return 0;
}

/* =====================================================================
 *  IS_ENTITY_STILL_ON_CAST_SCREEN (asm/ending/is_entity_still_on_cast_screen.asm)
 * ===================================================================== */
uint16_t is_entity_still_on_cast_screen(uint16_t current_entity_slot) {
    int16_t ent_off = current_entity_slot;
    uint16_t bg3y = ppu.bg_vofs[2];
    uint16_t entity_y = (uint16_t)entities.abs_y[ent_off];
    if (entity_y > (bg3y - 8)) {
        return 1;
    }
    return 0;
}

/* =====================================================================
 *  CREATE_ENTITY_AT_V01_PLUS_BG3Y (asm/ending/create_entity_at_v01_plus_bg3y.asm)
 * ===================================================================== */
void create_entity_at_v01_plus_bg3y(uint16_t sprite_id, uint16_t script_id,
                                     uint16_t current_entity_slot) {
    /* Set var0 for new entity to (counter & 3), increment counter */
    ert.new_entity_var[0] = initial_cast_entity_sleep_frames & 0x03;
    initial_cast_entity_sleep_frames++;

    int16_t ent_off = current_entity_slot;
    int16_t cx = entities.var[0][ent_off];
    int16_t cy = entities.var[1][ent_off] + (int16_t)ppu.bg_vofs[2];

    create_entity(sprite_id, script_id, -1, cx, cy);
}

/* =====================================================================
 *  HANDLE_CAST_SCROLLING (asm/ending/handle_cast_scrolling.asm)
 *
 *  Tick callback for cast scene entities. Sets BG3_Y to entity Y,
 *  clears tilemap rows as they scroll past.
 * ===================================================================== */
void handle_cast_scrolling(uint16_t current_entity_slot) {
    int16_t ent_off = current_entity_slot;

    /* Set BG3 Y scroll to entity Y */
    uint16_t entity_y = (uint16_t)entities.abs_y[ent_off];
    ppu.bg_vofs[2] = entity_y;

    /* Check if var7 < entity_y; if so, clear a tilemap row */
    uint16_t var7 = (uint16_t)entities.var[7][ent_off];
    if (var7 >= entity_y) return;

    /* Advance var7 by 8 pixels */
    entities.var[7][ent_off] = (int16_t)(var7 + 8);

    /* Clear the tilemap row above the current scroll position */
    uint16_t row = ((entity_y >> 3) - 1) & 0x1F;
    uint16_t vram_addr = VRAM_CAST_TILEMAP + (row << 5);

    /* Write 64 bytes (32 words) of zeros */
    uint8_t zeros[64];
    memset(zeros, 0, 64);
    vram_copy(zeros, vram_addr, 64, 3);
}

/* =====================================================================
 *  ENQUEUE_CREDITS_DMA (asm/ending/enqueue_credits_dma.asm)
 *
 *  Queues a VRAM transfer for the credits system.
 *  9 bytes per entry: mode(1) + size(2) + src_ptr(4) + vram_dest(2)
 * ===================================================================== */
static void enqueue_credits_dma(uint8_t mode, uint16_t size_bytes,
                                 const uint8_t *src, uint16_t vram_dest) {
    uint16_t idx = credits_dma_queue_start;
    uint8_t *entry = &credits_dma_queue[idx * CREDITS_DMA_ENTRY_SIZE];

    entry[0] = mode;
    write_u16_le(&entry[1], size_bytes);
    /* Source pointer stored in side array to avoid 64-bit pointer truncation */
    credits_dma_src_ptrs[idx] = src;
    write_u16_le(&entry[7], vram_dest);

    credits_dma_queue_start = (credits_dma_queue_start + 1) & 0x7F;
}

/* =====================================================================
 *  PROCESS_CREDITS_DMA_QUEUE (asm/ending/process_credits_dma_queue.asm)
 *
 *  Executes one queued DMA transfer per call.
 * ===================================================================== */
static void process_credits_dma_queue(void) {
    if (credits_dma_queue_start == credits_dma_queue_end) return;

    uint8_t *entry = &credits_dma_queue[credits_dma_queue_end * CREDITS_DMA_ENTRY_SIZE];

    uint8_t mode = entry[0];
    uint16_t size = read_u16_le(&entry[1]);
    const uint8_t *src = credits_dma_src_ptrs[credits_dma_queue_end];
    uint16_t vram_dest = read_u16_le(&entry[7]);

    vram_copy(src, vram_dest, size, mode);

    credits_dma_queue_end = (credits_dma_queue_end + 1) & 0x7F;
}

/* =====================================================================
 *  COUNT_PHOTO_FLAGS (asm/ending/count_photo_flags.asm)
 *
 *  Counts how many photo event flags are set.
 * ===================================================================== */
static uint16_t count_photo_flags(void) {
    if (!photographer_cfg_data) return 0;

    uint16_t count = 0;
    for (int i = 0; i < NUM_PHOTOS; i++) {
        const uint8_t *entry = &photographer_cfg_data[i * PHOTOGRAPHER_CFG_ENTRY_SIZE];
        uint16_t flag = read_u16_le(&entry[PHOTOGRAPHER_CFG_EVENT_FLAG]);
        if (event_flag_get(flag)) {
            count++;
        }
    }
    return count;
}

/* =====================================================================
 *  TRY_RENDERING_PHOTOGRAPH (asm/ending/try_rendering_photograph.asm)
 *
 *  Loads map at photo location, spawns objects and party entities.
 *  Returns 1 if photo was rendered, 0 if event flag not set.
 * ===================================================================== */
static uint16_t try_rendering_photograph(uint16_t photo_index) {
    if (!photographer_cfg_data) return 0;

    const uint8_t *cfg = &photographer_cfg_data[photo_index * PHOTOGRAPHER_CFG_ENTRY_SIZE];

    /* Check event flag */
    uint16_t flag = read_u16_le(&cfg[PHOTOGRAPHER_CFG_EVENT_FLAG]);
    if (!event_flag_get(flag)) return 0;

    /* Set photograph loading mode */
    photograph_map_loading_mode = 1;
    cur_photo_display = photo_index;

    /* Save and disable enemy spawns */
    uint8_t saved_enemy_spawns = ow.enemy_spawns_enabled;
    ow.enemy_spawns_enabled = 0;

    /* Clear OAM ert.buffer area at $2000 (1024 words) */
    /* In assembly: BSS_START+$2000, 1024 words = 2048 bytes */
    /* This clears sprite data in OAM ert.buffer. In C port, clear OAM. */
    oam_clear();

    /* Reset palette upload mode */
    ert.palette_upload_mode = 0;

    /* Load credits map palette */
    {
        const uint8_t *pal_data = ASSET_DATA(ASSET_ENDING_E1E924_BIN);
        if (pal_data) {
            memcpy(&ert.palettes[1 * 16], &pal_data[6], BPP4PALETTE_SIZE);
        }
    }

    /* Load map at photo position */
    uint16_t map_y = read_u16_le(&cfg[PHOTOGRAPHER_CFG_MAP_Y]);
    uint16_t map_x = read_u16_le(&cfg[PHOTOGRAPHER_CFG_MAP_X]);
    load_map_at_position(map_x * 8, map_y * 8);

    /* Restore enemy spawns */
    ow.enemy_spawns_enabled = saved_enemy_spawns;
    ppu.bg_vofs[1] = 0;
    ppu.bg_hofs[1] = 0;
    photograph_map_loading_mode = 0;

    /* Spawn up to 4 objects */
    uint16_t var0_counter = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *obj = &cfg[PHOTOGRAPHER_CFG_OBJECT_CONFIG + i * PHOTOGRAPHER_OBJ_SIZE];
        uint16_t sprite = read_u16_le(&obj[PHOTOGRAPHER_OBJ_SPRITE]);
        if (sprite == 0) continue;

        ert.new_entity_var[0] = var0_counter++;
        uint16_t tx = read_u16_le(&obj[PHOTOGRAPHER_OBJ_TILE_X]) * 8;
        uint16_t ty = read_u16_le(&obj[PHOTOGRAPHER_OBJ_TILE_Y]) * 8;
        create_entity(sprite, EVENT_SCRIPT_799, -1, tx, ty);
    }

    /* Spawn up to 6 party entities */
    for (int i = 0; i < 6; i++) {
        /* Read party byte from saved photo state */
        uint8_t party_byte = game_state.saved_photo_states[photo_index].party[i];
        if (party_byte == 0) continue;

        if ((party_byte & 0x1F) >= 18 || (party_byte & 0x1F) == 0) continue;

        ert.new_entity_var[0] = var0_counter++;

        /* Get sprite variant from flags.
         * Port of GET_SPRITE_VARIANT_FROM_FLAGS (C079EC).
         * flags bits: [4:0] = char_id (1-based), [5] = angel mode, [6] = bicycle.
         * Column 0 = normal, column 1 = angel/unconscious. */
        uint16_t sprite;
        {
            uint16_t flags_val = party_byte;
            if (flags_val & 0x0040) {
                sprite = 12;  /* Bicycle sprite */
            } else {
                uint16_t column = (flags_val & 0x0020) ? 1 : 0;
                uint16_t char_id_0 = (flags_val & 0x1F) - 1;
                uint16_t val = lookup_playable_char_sprite(char_id_0, column);
                sprite = (val == 1) ? 14 : val;
            }
        }

        /* Read party member position from config */
        uint16_t party_offset = i * 4;
        uint16_t px = read_u16_le(&cfg[PHOTOGRAPHER_CFG_PARTY_CONFIG + party_offset]) * 8;
        uint16_t py = read_u16_le(&cfg[PHOTOGRAPHER_CFG_PARTY_CONFIG + party_offset + 2]) * 8;

        int16_t ent = create_entity(sprite, EVENT_SCRIPT_800, -1, px, py);

        /* Apply possession overlay if bit 7 set.
         * Port of APPLY_POSSESSION_OVERLAY_FLAG (C07A31). */
        if (party_byte & 0x80) {
            /* entity arrays indexed by slot */
            entities.overlay_flags[ENT(ent)] |= 0x01;
        }
    }

    return 1;
}

/* =====================================================================
 *  SLIDE_CREDITS_PHOTOGRAPH (asm/ending/slide_credits_photograph.asm)
 *
 *  Animates a sliding photo by computing velocity from direction/distance,
 *  then moving BG1/BG2 each frame.
 * ===================================================================== */
static void slide_credits_photograph(uint16_t photo_index) {
    if (!photographer_cfg_data) return;

    const uint8_t *cfg = &photographer_cfg_data[photo_index * PHOTOGRAPHER_CFG_ENTRY_SIZE];

    /* Get slide direction and speed */
    uint8_t direction = cfg[PHOTOGRAPHER_CFG_SLIDE_DIRECTION];
    uint8_t distance = cfg[PHOTOGRAPHER_CFG_SLIDE_DISTANCE];

    /* Calculate velocity components from direction (speed = 1024, distance = 256) */
    /* Assembly: CALCULATE_VELOCITY_COMPONENTS with speed=1024 */
    int16_t dx, dy;
    calculate_velocity_components(direction, 1024, &dx, &dy);

    /* Number of frames = distance * 256 / 256 = distance */
    uint16_t total_frames = distance;

    /* Save initial BG positions */
    int32_t accum_x = 0;
    int32_t accum_y = 0;
    uint16_t start_x = ppu.bg_hofs[0];
    uint16_t start_y = ppu.bg_vofs[0];

    for (uint16_t frame = 0; frame < total_frames; frame++) {
        accum_x += dy;  /* Note: assembly swaps X/Y components */
        accum_y += dx;

        int16_t offset_x = (int16_t)(accum_x / 256);
        int16_t offset_y = (int16_t)(accum_y / 256);

        ppu.bg_hofs[0] = start_x + offset_x;
        ppu.bg_vofs[0] = start_y + offset_y;
        ppu.bg_hofs[1] = offset_x;
        ppu.bg_vofs[1] = offset_y;

        process_credits_dma_queue();
        render_frame_tick();
    }
}

/* =====================================================================
 *  CREDITS_SCROLL_FRAME (asm/ending/credits_scroll_frame.asm)
 *
 *  Per-frame callback during credits. Processes credit script commands
 *  and advances scroll position.
 * ===================================================================== */
void credits_scroll_frame(void) {
    /* Check if we need to process a new credit entry */
    if (ppu.bg_vofs[2] <= credits_next_credit_position)
        goto row_wipe_check;

    {
        uint16_t row_a = credits_current_row;
        uint16_t row_b = credits_current_row + 1;
        credits_current_row = (credits_current_row + 2) & 0x0F;

        /* Compute tilemap base row from BG3 scroll */
        uint16_t base_row = ((ppu.bg_vofs[2] >> 3) + 29) & 0x1F;

        /* Compute ert.buffer addresses for the two rows */
        uint8_t *buf_row_a = &win.bg2_buffer[row_a * 64];
        uint8_t *buf_row_b = &win.bg2_buffer[row_b * 64];

        /* Bounds check: ensure script pointer is within loaded asset */
        if (!staff_text_data ||
            credits_script_ptr < staff_text_data ||
            credits_script_ptr >= staff_text_data + staff_text_size) {
            FATAL("credits_scroll_frame: script pointer out of bounds\n");
        }

        /* Read command byte from script */
        uint8_t cmd = *credits_script_ptr++;
        uint16_t char_count = 0;

        if (cmd == 1) {
            /* CMD_SMALL_TEXT: 8-pixel rows, single-height characters */
            credits_next_credit_position += 8;

            /* Write small characters to ert.buffer */
            while (*credits_script_ptr != 0) {
                uint8_t ch = *credits_script_ptr++;
                uint16_t tile = (uint16_t)ch + 0x2000;
                write_u16_le(&buf_row_a[char_count * 2], tile);
                char_count++;
            }

            /* Center: compute VRAM address */
            uint16_t half_width = char_count / 2;
            uint16_t vram_addr = (base_row << 5) + 0x6C10 - half_width;

            /* Enqueue DMA for this row */
            enqueue_credits_dma(0, char_count * 2, buf_row_a, vram_addr);

        } else if (cmd == 2) {
            /* CMD_LARGE_TEXT: 16-pixel rows, double-height characters */
            credits_next_credit_position += 16;

            while (*credits_script_ptr != 0) {
                uint8_t ch = *credits_script_ptr++;
                /* Top row tile */
                uint16_t tile_top = (uint16_t)ch + 0x2400;
                write_u16_le(&buf_row_a[char_count * 2], tile_top);
                /* Bottom row tile */
                uint16_t tile_bot = (uint16_t)ch + 0x2410;
                write_u16_le(&buf_row_b[char_count * 2], tile_bot);
                char_count++;
            }

            /* Center */
            uint16_t half_width = char_count / 2;
            uint16_t vram_top = (base_row << 5) + 0x6C10 - half_width;

            /* Enqueue DMA for top row */
            enqueue_credits_dma(0, char_count * 2, buf_row_a, vram_top);

            /* Compute bottom row VRAM address (wrap at row 31) */
            uint16_t vram_bot;
            if (base_row == 31) {
                vram_bot = vram_top - 0x03E0;
            } else {
                vram_bot = vram_top + 32;
            }

            /* Enqueue DMA for bottom row */
            enqueue_credits_dma(0, char_count * 2, buf_row_b, vram_bot);

        } else if (cmd == 3) {
            /* CMD_SPACING: skip N*8 pixels */
            uint8_t spacing = *credits_script_ptr;
            credits_next_credit_position += spacing * 8;

        } else if (cmd == 4) {
            /* CMD_PLAYER_NAME: render the player's custom name */
            /* Check if earthbound_playername is non-empty */
            if (game_state.earthbound_playername[0] == 0)
                goto skip_name;

            /* Convert EB character codes to credits font tile indices */
            uint8_t name_converted[24];
            memset(name_converted, 0, 24);
            int name_len = 0;
            for (int i = 0; i < 24 && game_state.earthbound_playername[i] != 0; i++) {
                uint8_t eb = game_state.earthbound_playername[i];
                uint8_t out;
                /* Special character handling */
                if (eb == 0xAC) { out = 124; }        /* pipe */
                else if (eb == 0xAE) { out = 126; }   /* tilde */
                else if (eb == 0xAF) { out = 127; }   /* special */
                else if (eb > 0x91) { out = eb - 80; } /* eb >= 0x92: assembly uses eb - 80 */
                else { out = eb - 48; }                /* eb <= 0x91: assembly uses eb - 48 */
                name_converted[name_len++] = out;
            }

            credits_next_credit_position += 16;

            /* Write name as large (double-height) characters */
            char_count = 0;
            for (int i = 0; i < name_len && name_converted[i] != 0 && i < 24; i++) {
                uint8_t ch = name_converted[i];
                /* Top: (ch & 0xF0) + ch + 0x2400 */
                uint16_t tile_top = (ch & 0xF0) + ch + 0x2400;
                write_u16_le(&buf_row_a[char_count * 2], tile_top);
                /* Bottom: (ch & 0xF0) + ch + 0x2410 */
                uint16_t tile_bot = (ch & 0xF0) + ch + 0x2410;
                write_u16_le(&buf_row_b[char_count * 2], tile_bot);
                char_count++;
            }

            /* Center and DMA */
            uint16_t half = char_count / 2;
            uint16_t vram_top = (base_row << 5) + 0x6C10 - half;

            enqueue_credits_dma(0, char_count * 2, buf_row_a, vram_top);

            uint16_t vram_bot;
            if (base_row == 31) {
                vram_bot = vram_top - 0x03E0;
            } else {
                vram_bot = vram_top + 32;
            }
            enqueue_credits_dma(0, char_count * 2, buf_row_b, vram_bot);

            /* Back up script pointer by 1 (assembly: DEC @VIRTUAL06) */
            skip_name:
            credits_script_ptr--;

        } else if (cmd == 0xFF) {
            /* CMD_END: disable further processing */
            credits_next_credit_position = 0xFFFF;
        }

        /* Advance past null terminator */
        credits_script_ptr++;
    }

row_wipe_check:
    /* Clear rows that have scrolled past */
    /* Assembly: BCS (skip when threshold >= bg3_y_pos); wipe when threshold < pos */
    if (credits_row_wipe_threshold < ppu.bg_vofs[2]) {
        credits_row_wipe_threshold += 8;

        /* Clear one tilemap row */
        uint16_t clear_row = ((ppu.bg_vofs[2] >> 3) - 1) & 0x1F;
        uint16_t vram_addr = 0x6C00 + (clear_row << 5);
        /* Mode 3 = word fill; only src[0] and src[1] are read.
         * Must be static — enqueue_credits_dma stores the raw pointer in the
         * persistent DMA queue, which outlives this stack frame. */
        static const uint8_t blank_tile_data[2] = {0, 0};
        enqueue_credits_dma(3, 64, blank_tile_data, vram_addr);
    }

    /* Update scroll position (fixed-point: fraction += 0x4000) */
    uint32_t frac = credits_scroll_position_frac + 0x4000;
    credits_scroll_position_frac = (uint16_t)(frac & 0xFFFF);
    if (frac >= 0x10000) {
        credits_scroll_position_int++;
    }

    /* Write BG3 Y scroll */
    ppu.bg_vofs[2] = credits_scroll_position_int;
}

/* =====================================================================
 *  LOAD_CAST_SCENE (asm/ending/load_cast_scene.asm)
 * ===================================================================== */
static void load_cast_scene(void) {
    extern uint16_t item_transformations_loaded;
    item_transformations_loaded = 0;

    fade_out_with_mosaic(1, 1, 0);
    force_blank_and_wait_vblank();
    clear_map_entities();

    /* Mark all active entities with hidden flag on spritemap_ptr_hi */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (entities.script_table[i] != -1) {
            entities.spritemap_ptr_hi[i] |= (int16_t)0x8000;
        }
    }

    /* Load battle background animation */
    load_background_animation(BATTLEBG_LAYER_NONE, BATTLEBG_LAYER_UNKNOWN279);

    /* Set up BG3 VRAM layout for cast */
    set_bg3_vram_location(BG_TILEMAP_SIZE_NORMAL, VRAM_CAST_TILEMAP, VRAM_CAST_TILES);
    set_oam_size(0x62);

    /* Clear all BG scroll positions */
    ppu.bg_hofs[2] = 0; ppu.bg_vofs[2] = 0;
    ppu.bg_hofs[1] = 0; ppu.bg_vofs[1] = 0;
    ppu.bg_hofs[0] = 0; ppu.bg_vofs[0] = 0;
    update_screen();

    /* Clear cast tilemap (VRAM::CAST_TILEMAP, 2048 bytes) */
    {
        uint8_t zeros[2] = {0, 0};
        vram_copy(zeros, VRAM_CAST_TILEMAP, 2048, 3);
    }

    /* Set force normal font flag */
    force_normal_font_for_length_calc = 0xFF;

    /* Compose cast tile data directly in ppu.vram (VRAM_CAST_TILES = 0x0000,
     * so byte offsets match). Intentional divergence from assembly, which stages
     * through BUFFER then DMAs — we skip the intermediate 32 KB copy. */
    uint8_t *cast_vram = &ppu.vram[VRAM_CAST_TILES * 2];

    /* Clear first 4096 bytes of tile data */
    memset(cast_vram, 0, 4096);

    /* Decompress CAST_SCENE_BASE_GFX to VRAM offset $200 */
    {
        const uint8_t *data = ASSET_DATA(ASSET_E1D6E1_GFX_LZHAL);
        size_t data_size = ASSET_SIZE(ASSET_E1D6E1_GFX_LZHAL);
        if (data) {
            decomp(data, data_size, &cast_vram[BUF_CREDITS_GFX_1], 32768 - BUF_CREDITS_GFX_1);
        }
    }

    /* Decompress CAST_NAMES_GFX to VRAM offset $600 */
    {
        const uint8_t *data = ASSET_DATA(ASSET_ENDING_CAST_NAMES_GFX_LZHAL);
        size_t data_size = ASSET_SIZE(ASSET_ENDING_CAST_NAMES_GFX_LZHAL);
        if (data) {
            decomp(data, data_size, &cast_vram[BUF_CREDITS_GFX_2], 32768 - BUF_CREDITS_GFX_2);
        }
    }

    /* Render dynamic cast name text (party names + guardian texts) directly to VRAM */
    prepare_dynamic_cast_name_text();

    /* Clear force normal font flag */
    force_normal_font_for_length_calc = 0;

    /* Load character window palette */
    load_character_window_palette();

    /* Load cast BG palette (CAST_BG_PALETTE) */
    {
        const uint8_t *pal = ASSET_DATA(ASSET_ENDING_CAST_BG_PALETTE_PAL);
        if (pal) {
            memcpy(ert.palettes, pal, BPP2PALETTE_SIZE * 4);
        }
    }

    /* Load sprite group ert.palettes to OBJ ert.palettes 8-15 */
    {
        const uint8_t *pal = ASSET_DATA(ASSET_SPRITE_GROUP_PALETTES_PAL);
        if (pal) {
            memcpy(&ert.palettes[8 * 16], pal, BPP4PALETTE_SIZE * 8);
        }
    }

    /* Decompress special cast palette data (CAST_NAMES_PALETTE) to BUFFER+$7000 */
    {
        const uint8_t *data = ASSET_DATA(ASSET_ENDING_CAST_NAMES_PAL_LZHAL);
        size_t data_size = ASSET_SIZE(ASSET_ENDING_CAST_NAMES_PAL_LZHAL);
        if (data) {
            decomp(data, data_size, &ert.buffer[BUF_CREDITS_PALETTE], BUFFER_SIZE - BUF_CREDITS_PALETTE);
        }
    }

    /* Set full palette upload, TM = $14 (BG2+BG3, no BG1) */
    ert.palette_upload_mode = 24; /* PALETTE_UPLOAD_FULL */
    ppu.tm = 0x14;

    cast_text_row = 0;
    cast_tile_offset = 0;
    blank_screen_and_wait_vblank();
}

/* =====================================================================
 *  PLAY_CAST_SCENE (asm/ending/play_cast_scene.asm)
 * ===================================================================== */
void play_cast_scene(void) {
    load_cast_scene();
    oam_clear();
    fade_in(1, 1);

    /* Init entity wipe with EVENT_801 */
    entity_init_wipe(EVENT_SCRIPT_801);
    ert.actionscript_state = 0;

    /* Render loop: render frames until ert.actionscript_state becomes non-zero */
    while (ert.actionscript_state == 0) {
        render_frame_tick();
        update_battle_screen_effects();
    }

    /* Fade out — assembly: step=1, delay=1, mosaic=0 */
    fade_out_with_mosaic(1, 1, 0);

    /* Deactivate all entities with script EVENT_801 */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (entities.script_table[i] == EVENT_SCRIPT_801) {
            deactivate_entity(i);
        }
    }

    /* Reset entity allocation and re-init */
    entities.alloc_min_slot = 23;
    entities.alloc_max_slot = 24;
    entity_init(EVENT_SCRIPT_001, 0, 0);
    reset_party_state();
    initialize_party();
    force_blank_and_wait_vblank();
    undraw_flyover_text();
    ppu.tm = 0x17;  /* Assembly: STA TM_MIRROR (main screen, not sub-screen) */
}

/* =====================================================================
 *  INITIALIZE_CREDITS_SCENE (asm/ending/initialize_credits_scene.asm)
 * ===================================================================== */
static void initialize_credits_scene(void) {
    force_blank_and_wait_vblank();
    clear_map_entities();

    credits_current_row = 0;
    credits_dma_queue_start = 0;
    credits_dma_queue_end = 0;

    /* Set up 3-layer BG VRAM layout */
    set_bg1_vram_location(BG_TILEMAP_SIZE_HORIZONTAL,
                          VRAM_CREDITS_LAYER_1_TILEMAP, VRAM_CREDITS_LAYER_1_TILES);
    set_bg2_vram_location(BG_TILEMAP_SIZE_BOTH,
                          VRAM_CREDITS_LAYER_2_TILEMAP, VRAM_CREDITS_LAYER_2_TILES);
    set_bg3_vram_location(BG_TILEMAP_SIZE_NORMAL,
                          VRAM_CREDITS_LAYER_3_TILEMAP, VRAM_CREDITS_LAYER_3_TILES);
    set_oam_size(0x62);

    /* Clear all BG scroll positions */
    ppu.bg_hofs[2] = 0; ppu.bg_vofs[2] = 0;
    ppu.bg_hofs[1] = 0; ppu.bg_vofs[1] = 0;
    ppu.bg_hofs[0] = 0; ppu.bg_vofs[0] = 0;
    update_screen();

    /* Clear BG1 tilemap (4096 bytes of zeros, word fill) */
    {
        uint8_t zeros[2] = {0, 0};
        vram_copy(zeros, VRAM_CREDITS_LAYER_1_TILEMAP, 4096, 3);
    }

    /* Fill BG2 tilemap with tile $240C (assembly line 39-40: COPY_TO_VRAM1P mode 9,
     * fixed source word fill).  2048 words = 4096 bytes. */
    {
        uint16_t *vram16 = (uint16_t *)&ppu.vram[VRAM_CREDITS_LAYER_2_TILEMAP * 2];
        for (int i = 0; i < 2048; i++)
            vram16[i] = 0x240C;
    }

    /* Assembly line 41: COPY_TO_VRAM1 BUFFER+1, VRAM::CREDITS_LAYER_2_TILEMAP, 4096, 15
     * Mode 15 = DMAP=$08 (fixed source), VMAIN=$80 (VMDATAH, address remapping).
     * The source BUFFER+1 is the high byte of the word $240C stored in BUFFER, i.e. $24.
     * This writes $24 to the high byte of each VRAM word in CREDITS_LAYER_2_TILEMAP.
     * Step 2 already set all high bytes to $24 (from the $240C fill), so this is a
     * no-op for this tilemap region — both passes write the same value.  Included here
     * for assembly faithfulness. */
    {
        uint32_t byte_addr = (uint32_t)VRAM_CREDITS_LAYER_2_TILEMAP * 2;
        /* Write 0x24 to the high byte of each of the 2048 words (4096 bytes / 2). */
        for (int i = 0; i < 2048; i++) {
            ppu.vram[byte_addr + i * 2 + 1] = 0x24;
        }
    }

    /* Decompress tilemap/font data (CREDITS_BG2_TILEMAP_AND_TILES → BUFFER) */
    {
        const uint8_t *data = ASSET_DATA(ASSET_ENDING_E1E94A_BIN_LZHAL);
        size_t data_size = ASSET_SIZE(ASSET_ENDING_E1E94A_BIN_LZHAL);
        if (data) {
            decomp(data, data_size, ert.buffer, BUFFER_SIZE);
        }
    }

    /* Load BG2 palette from CREDITS_BG2_PALETTE_DATA+6.
     * NOTE: This is dead code — the memset below (palette slots 1-15) will
     * zero this immediately.  The assembly does the same thing: MEMCPY16 to
     * PALETTES+BPP4PALETTE_SIZE*1, then MEMSET16 zeros BPP4PALETTE_SIZE*15
     * bytes starting at the same address.  Kept for assembly faithfulness. */
    {
        const uint8_t *data = ASSET_DATA(ASSET_ENDING_E1E924_BIN);
        if (data) {
            memcpy(&ert.palettes[1 * 16], &data[6], BPP4PALETTE_SIZE);
        }
    }

    /* Upload decompressed tilemap to BG2 (1792 bytes) */
    vram_copy(ert.buffer, VRAM_CREDITS_LAYER_2_TILEMAP, 1792, 0);

    /* Upload tile data to BG2 tiles (8192 bytes from BUFFER+$700) */
    vram_copy(&ert.buffer[BUF_CREDITS_LAYER2], VRAM_CREDITS_LAYER_2_TILES, 8192, 0);

    /* Clear BG3 tilemap (2048 bytes) */
    {
        uint8_t zeros[2] = {0, 0};
        vram_copy(zeros, VRAM_CREDITS_LAYER_3_TILEMAP, 2048, 3);
    }

    /* Decompress and upload staff credits font */
    {
        const uint8_t *data = ASSET_DATA(ASSET_ENDING_CREDITS_FONT_GFX_LZHAL);
        size_t data_size = ASSET_SIZE(ASSET_ENDING_CREDITS_FONT_GFX_LZHAL);
        if (data) {
            decomp(data, data_size, ert.buffer, BUFFER_SIZE);

            /* Upload to VRAM::CREDITS_LAYER_3_TILES + $200 */
            vram_copy(ert.buffer, VRAM_CREDITS_LAYER_3_TILES + 0x200,
                      STAFF_CREDITS_FONT_GFX_SIZE, 0);
        }
    }

    /* Load credits font palette to BG palette 0-1 */
    {
        const uint8_t *pal = ASSET_DATA(ASSET_ENDING_CREDITS_FONT_PAL);
        if (pal) {
            memcpy(ert.palettes, pal, BPP2PALETTE_SIZE * 2);
        }
    }

    /* Load sprite group ert.palettes to OBJ ert.palettes 8-15 */
    {
        const uint8_t *pal = ASSET_DATA(ASSET_SPRITE_GROUP_PALETTES_PAL);
        if (pal) {
            memcpy(&ert.palettes[8 * 16], pal, BPP4PALETTE_SIZE * 8);
        }
    }

    /* Clear palette slots 1-15 of OBJ palette (fill with 0) */
    memset(&ert.palettes[1 * 16], 0, BPP4PALETTE_SIZE * 15);

    /* Set full palette upload, TM = $17 (all layers) */
    ert.palette_upload_mode = 24; /* PALETTE_UPLOAD_FULL */
    ppu.tm = 0x17;

    credits_next_credit_position = 0;
    credits_scroll_position_frac = 0;
    credits_scroll_position_int = 0;
    credits_row_wipe_threshold = 7;

    /* Clear BG2 ert.buffer (512 words = 1024 bytes) */
    memset(win.bg2_buffer, 0, 1024);

    /* Set credits script data pointer */
    credits_script_ptr = staff_text_data;

    blank_screen_and_wait_vblank();
}

/* =====================================================================
 *  PLAY_CREDITS (asm/ending/play_credits.asm)
 * ===================================================================== */
void play_credits(void) {
    /* Load assets needed for credits */
    photographer_cfg_data = ASSET_DATA(ASSET_ENDING_PHOTOGRAPHER_CFG_BIN);
    photographer_cfg_size = ASSET_SIZE(ASSET_ENDING_PHOTOGRAPHER_CFG_BIN);
    cast_seq_formatting_data = ASSET_DATA(ASSET_ENDING_CAST_SEQUENCE_FORMATTING_BIN);
    staff_text_data = ASSET_DATA(ASSET_ENDING_STAFF_TEXT_BIN);
    staff_text_size = ASSET_SIZE(ASSET_ENDING_STAFF_TEXT_BIN);
    party_cast_tile_ids_data = ASSET_DATA(ASSET_ENDING_PARTY_CAST_TILE_IDS_BIN);
    guardian_text_data = ASSET_DATA(ASSET_ENDING_GUARDIAN_TEXT_BIN);

    ow.disabled_transitions = 1;
    initialize_credits_scene();
    oam_clear();
    fade_in(1, 2);

    /* Count photos and calculate spacing */
    uint16_t photo_count = count_photo_flags();
    uint16_t photo_spacing;
    if (photo_count > 0) {
        photo_spacing = CREDITS_LENGTH / photo_count;
    } else {
        photo_spacing = CREDITS_LENGTH;
    }
    uint16_t next_photo_pos = photo_spacing;

    /* Set IRQ callback to credits_scroll_frame */
    frame_callback = credits_scroll_frame;

    /* Process each photo */
    for (int photo_idx = 0; photo_idx < NUM_PHOTOS; photo_idx++) {
        uint16_t rendered = try_rendering_photograph(photo_idx);
        if (!rendered) continue;

        /* Fade in photo over 64 frames */
        prepare_palette_fade_slopes(64, 0xFFFF);
        for (int f = 64; f > 0; f--) {
            update_map_palette_animation();
            process_credits_dma_queue();
            render_frame_tick();
        }
        finalize_palette_fade();

        /* Slide photo */
        slide_credits_photograph(photo_idx);

        /* Wait for scroll position to pass next_photo_pos */
        while ((int16_t)next_photo_pos > (int16_t)ppu.bg_vofs[2]) {
            process_credits_dma_queue();
            render_frame_tick();
        }

        /* Clear OAM (BUFFER+32, 480 bytes) */
        memset(&ert.buffer[32], 0, 480);

        /* Fade out photo over 64 frames */
        prepare_palette_fade_slopes(64, 0xFFFF);
        for (int f = 0; f < 64; f++) {
            update_map_palette_animation();
            process_credits_dma_queue();
            render_frame_tick();
        }

        /* Clear palette slots 1-15 (assembly: LDX #480; JSL MEMSET16) */
        memset(&ert.palettes[1 * 16], 0, 480);
        ert.palette_upload_mode = 24; /* PALETTE_UPLOAD_FULL */
        process_credits_dma_queue();
        render_frame_tick();

        next_photo_pos += photo_spacing;
    }

    /* Wait for scroll to reach CREDITS_LENGTH */
    while (ppu.bg_vofs[2] < CREDITS_LENGTH) {
        process_credits_dma_queue();
        render_frame_tick();
    }

    /* Reset IRQ callback to overworld tasks (assembly: SET_IRQ_CALLBACK PROCESS_OVERWORLD_TASKS) */
    frame_callback = process_overworld_tasks;

    /* Wait 2000 frames */
    for (int f = 0; f < 2000; f++) {
        render_frame_tick();
    }

    /* Fade out with mosaic */
    fade_out_with_mosaic(1, 2, 0);

    /* Reset color math window */
    setup_color_math_window(0xB3, 0);
    force_blank_and_wait_vblank();
    overworld_setup_vram();
    clear_map_entities();

    /* Reset entity allocation */
    entities.alloc_min_slot = 23;
    entities.alloc_max_slot = 24;
    entity_init(EVENT_SCRIPT_001, 0, 0);
    reset_party_state();
    initialize_party();

    /* Clear BG2 ert.buffer */
    memset(win.bg2_buffer, 0, 1024);

    undraw_flyover_text();
    ppu.tm = 0x17;  /* Assembly: STA TM_MIRROR (main screen, not sub-screen) */

    /* Restore IRQ callback to overworld tasks */
    frame_callback = process_overworld_tasks;

    ow.disabled_transitions = 0;

    /* Release loaded asset references */
    photographer_cfg_data = NULL;
    cast_seq_formatting_data = NULL;
    staff_text_data = NULL;
    staff_text_size = 0;
    party_cast_tile_ids_data = NULL;
    guardian_text_data = NULL;
}
