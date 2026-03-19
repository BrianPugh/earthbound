/* Flyover text system — port of C4 bank flyover rendering functions.
 *
 * Assembly sources:
 *   BLIT_FONT_TO_VWF_BUFFER     (C49875.asm, 145 lines)
 *   RENDER_LARGE_FONT_CHARACTER  (C4999B.asm, 80 lines)
 *   INVERT_VWF_BUFFER            (C4984B.asm, 24 lines)
 *   INIT_FLYOVER_TEXT_SCREEN     (C49A56.asm, 113 lines)
 *   UPLOAD_VWF_BUFFER_TO_VRAM    (C49B6E.asm, 88 lines)
 *   SCROLL_FLYOVER_TEXT          (C49C56.asm, 95 lines US)
 *   ADVANCE_FLYOVER_PIXEL_OFFSET (C49CA8.asm, 20 lines)
 *   RENDER_FLYOVER_TEXT_CHARACTER (C49D16.asm, 8 lines)
 *   RENDER_PARTY_MEMBER_NAME     (C49CC3.asm, 38 lines)
 *   ADVANCE_TEXT_LINE_POSITION   (C49D1E.asm, 46 lines)
 *   PLAY_FLYOVER_SCRIPT          (C49EC4.asm, 122 lines)
 *   COFFEETEA_SCENE              (coffee_tea_scene.asm, 139 lines)
 *   UNDRAW_FLYOVER_TEXT          (undraw_flyover_text.asm, 29 lines)
 */

#include "game/flyover.h"
#include "game/display_text.h"
#include "game/text.h"
#include "game/window.h"
#include "game/battle.h"
#include "game/fade.h"
#include "game/game_state.h"
#include "game/overworld.h"
#include "entity/entity.h"
#include "snes/ppu.h"
#include "data/assets.h"
#include "include/constants.h"
#include "platform/platform.h"
#include <string.h>

#include "game_main.h"

/* ======================================================================
 * Flyover VWF ert.buffer — separate from the window system's VWF ert.buffer.
 * Layout: 26 tiles per row, each tile = 16 bytes (8x8 2bpp).
 *   Bytes 0-415:   Row 0 top halves (26 × 16)
 *   Bytes 416-831: Row 0 bottom halves (26 × 16)
 *   Bytes 832+:    Additional rows
 * Total: VWF_BUFFER_SIZE = 52 × 32 = 1664 bytes
 * ====================================================================== */

/* Share the normal window VWF buffer — flyover is a blocking full-screen
 * takeover that never runs render_all_windows(), so the two uses are
 * mutually exclusive.  flyover_init_screen() memsets before first use. */
#define flyover_vwf vwf_buffer

/* Flyover state globals (matching assembly ram.asm variables) */
static uint16_t flyover_vwf_x;
static uint16_t flyover_vwf_y;
static uint16_t flyover_tiles_per_row;
static uint16_t flyover_tile_offset;
static uint16_t flyover_dirty_min;
static uint16_t flyover_dirty_max;
static uint16_t flyover_screen_offset;
static uint16_t flyover_pixel_offset;
static uint16_t flyover_byte_offset;

/* TILES_PER_ROW * BYTES_PER_TILE = 26 * 16 = 416 */
#define FLYOVER_ROW_BYTES    416
/* Total VRAM size for flyover tiles: 32 rows × 416 bytes = 13312 = 0x3400 */
#define FLYOVER_VRAM_TOTAL   0x3400

/* ======================================================================
 * BLIT_FONT_TO_VWF_BUFFER (C49875)
 *
 * Blits a font glyph into the flyover VWF ert.buffer at the current position.
 * Handles sub-byte pixel alignment and tile boundary overlap.
 *
 * Assembly params: A=width, X=font_default_width (unused), Y=vwf_base,
 *                  @PARAM03=glyph_ptr (32-bit)
 * C port params: glyph_data=1bpp glyph, width=pixel width to blit
 * ====================================================================== */
static void flyover_blit_font(const uint8_t *glyph_data, uint16_t width) {
    uint16_t pixel_sub = flyover_pixel_offset % 8;  /* sub-byte position */
    uint16_t base_pos = flyover_byte_offset;         /* byte offset of current tile */

    /* Process two halves: top 8x8 (offset 0) and bottom 8x8 (offset +416) */
    for (int half = 0; half < 2; half++) {
        uint16_t write_pos = base_pos + (uint16_t)(half * FLYOVER_ROW_BYTES);
        for (int row = 0; row < 8; row++) {
            /* Read glyph byte (1bpp, inverted: 0=pixel, 1=bg) */
            uint8_t glyph_byte = glyph_data[half * 8 + row];

            /* Create mask: invert → shift right by pixel_sub → re-invert */
            uint8_t shifted = (uint8_t)((uint8_t)(~glyph_byte) >> pixel_sub);
            uint8_t mask = ~shifted;

            /* AND mask into plane 1 (odd byte), copy to plane 0 (even byte) */
            uint8_t result = flyover_vwf[write_pos + 1] & mask;
            flyover_vwf[write_pos + 1] = result;
            flyover_vwf[write_pos]     = result;

            write_pos += 2;  /* 2 bytes per row (plane 0 + plane 1) */
        }
    }

    /* Update pixel offset */
    flyover_pixel_offset += width;

    /* Check if we've crossed into the next tile */
    uint16_t new_byte_offset = (flyover_pixel_offset / 8) * 16;
    if (new_byte_offset == flyover_byte_offset) return;

    /* Handle overlap: glyph spans two tiles — blit remaining bits into next tile */
    flyover_byte_offset = new_byte_offset;
    uint16_t left_shift = 8 - pixel_sub;

    uint16_t overlap_base = new_byte_offset;

    for (int half = 0; half < 2; half++) {
        uint16_t write_pos = overlap_base + (uint16_t)(half * FLYOVER_ROW_BYTES);
        for (int row = 0; row < 8; row++) {
            uint8_t glyph_byte = glyph_data[half * 8 + row];
            uint8_t shifted = (uint8_t)((uint8_t)(~glyph_byte) << left_shift);
            uint8_t mask = ~shifted;

            uint8_t result = flyover_vwf[write_pos + 1] & mask;
            flyover_vwf[write_pos + 1] = result;
            flyover_vwf[write_pos]     = result;

            write_pos += 2;
        }
    }
}

/* ======================================================================
 * RENDER_LARGE_FONT_CHARACTER (C4999B)
 *
 * Renders an EB character (0x50+) using FONT_ID_LARGE into the flyover
 * VWF ert.buffer. Handles wide glyphs (>8px) by blitting in 8px chunks.
 * ====================================================================== */
static void flyover_render_large_char(uint16_t eb_char) {
    uint8_t glyph_index = (uint8_t)((eb_char - 0x50) & 0x7F);

    const uint8_t *glyph = font_get_glyph(FONT_ID_LARGE, glyph_index);
    if (!glyph) return;

    /* Get per-character pixel width from width data table */
    uint8_t char_width = font_get_width(FONT_ID_LARGE, glyph_index);
    uint16_t total_width = (uint16_t)char_width + 1;  /* assembly: INC @VIRTUAL02 */

    /* Column stride between 8-pixel chunks of a wide glyph.
     * Assembly uses font_table_entry::width (16), NOT bytes_per_glyph (32).
     * LARGE font: 32 bytes/glyph = two 16-byte columns (each 8px × 16 rows, 1bpp). */
    uint8_t font_stride = font_get_height(FONT_ID_LARGE);

    /* For wide glyphs (>8px), blit in 8-pixel chunks */
    const uint8_t *src = glyph;
    while (total_width > 8) {
        flyover_blit_font(src, 8);
        total_width -= 8;
        src += font_stride;  /* assembly: ADC @VIRTUAL04 (default width = stride) */
    }

    /* Blit remaining pixels */
    flyover_blit_font(src, total_width);
}

/* ======================================================================
 * RENDER_FLYOVER_TEXT_CHARACTER (C49D16) — thin wrapper
 * ====================================================================== */
static void flyover_render_text_char(uint16_t eb_char) {
    flyover_render_large_char(eb_char);
}

/* ======================================================================
 * RENDER_PARTY_MEMBER_NAME (C49CC3)
 *
 * Renders up to 5 characters of a party member's name using LARGE font.
 * Stops early if a character code is <= 0x50 (space = end of name).
 *
 * Assembly: char_id is 1-based party member index.
 * BRANCHGTS after CLC;SBC #79: branches while char > 0x50 (0x50 = space)
 * ====================================================================== */
static void flyover_render_party_name(uint16_t char_id) {
    if (char_id < 1 || char_id > 4) return;
    CharStruct *ch = &party_characters[char_id - 1];

    for (int i = 0; i < 5; i++) {
        uint8_t c = ch->name[i];
        /* Assembly: CLC; SBC #79; BRANCHGTS → branches when c >= 0x50 */
        if (c < 0x50) break;  /* characters below 0x50 are not valid EB text */
        flyover_render_large_char((uint16_t)c);
    }
}

/* ======================================================================
 * INVERT_VWF_BUFFER (C4984B)
 *
 * XORs entire flyover VWF ert.buffer with 0xFFFF.
 * Called before VRAM upload to convert from mask format
 * (0=glyph, 1=bg) to display format (1=glyph, 0=bg).
 * ====================================================================== */
static void flyover_invert_vwf(void) {
    uint16_t *buf = (uint16_t *)flyover_vwf;
    /* 32 * 26 = 832 words = 1664 bytes = full ert.buffer */
    for (int i = 0; i < 32 * 26; i++) {
        buf[i] ^= 0xFFFF;
    }
}

/* ======================================================================
 * Helper: set BG3 VRAM location registers.
 * Port of SET_BG3_VRAM_LOCATION (set_bg3_vram_location.asm).
 *
 * Assembly params: A=tilemap_size, X=tilemap_base, Y=tile_base
 * BG3SC = ((X>>8) & 0xFC) | (A & 0x03)
 * BG34NBA low nibble = (Y>>12) & 0x0F
 * Scroll positions reset to 0.
 * ====================================================================== */
static void set_bg3_vram_location(uint16_t tilemap_size, uint16_t tilemap_base,
                                   uint16_t tile_base) {
    ppu.bg_sc[2] = (uint8_t)(((tilemap_base >> 8) & 0xFC) | (tilemap_size & 0x03));
    ppu.bg_nba[1] = (uint8_t)((ppu.bg_nba[1] & 0xF0) | ((tile_base >> 12) & 0x0F));
    ppu.bg_hofs[2] = 0;
    ppu.bg_vofs[2] = 0;
}

/* ======================================================================
 * INIT_FLYOVER_TEXT_SCREEN (C49A56)
 *
 * Initializes the flyover text display:
 *   1. Force blank, set BG3 VRAM location
 *   2. Clear tile data in VRAM
 *   3. Load flyover palette (MOVEMENT_TEXT_STRING_PALETTE)
 *   4. Fill VWF ert.buffer with 0xFF
 *   5. Build BG2_BUFFER tilemap (26 usable tiles per row, 32 rows)
 *   6. Upload tilemap to VRAM
 *   7. Initialize flyover state variables
 * ====================================================================== */
static void flyover_init_screen(void) {
    force_blank_and_wait_vblank();

    /* SET_BG3_VRAM_LOCATION(NORMAL, TEXT_LAYER_TILEMAP=0x7C00, TEXT_LAYER_TILES=0x6000) */
    set_bg3_vram_location(0, VRAM_TEXT_LAYER_TILEMAP, VRAM_TEXT_LAYER_TILES);

    /* Clear all tile data: COPY_TO_VRAM1P ert.buffer(=0), TEXT_LAYER_TILES, $3800 bytes */
    memset(&ppu.vram[VRAM_TEXT_LAYER_TILES * 2], 0, 0x3800);

    /* Load flyover palette: MOVEMENT_TEXT_STRING_PALETTE → PALETTES[0..3] (8 bytes) */
    {
        size_t pal_size;
        pal_size = ASSET_SIZE(ASSET_DATA_MOVEMENT_TEXT_STRING_PALETTE_BIN);
        const uint8_t *pal_data = ASSET_DATA(ASSET_DATA_MOVEMENT_TEXT_STRING_PALETTE_BIN);
        if (pal_data && pal_size >= 8) {
            memcpy(ert.palettes, pal_data, 8);
        }
    }
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;

    /* Fill VWF ert.buffer with 0xFF (all background) */
    memset(flyover_vwf, 0xFF, sizeof(flyover_vwf));

    /* Build BG2_BUFFER tilemap: 32 rows × 32 columns.
     * Columns 0-2 and 29-31: empty (0). Columns 3-28: tile indices with palette bit.
     * Tile indices start at 16 and increment across all rows. */
    uint16_t tile_idx = 16;
    for (int row = 0; row < 32; row++) {
        int tilemap_row_offset = row * 64;  /* 32 columns × 2 bytes */

        /* Clear first 3 entries (6 bytes) */
        win.bg2_buffer[tilemap_row_offset + 0] = 0;
        win.bg2_buffer[tilemap_row_offset + 1] = 0;
        win.bg2_buffer[tilemap_row_offset + 2] = 0;
        win.bg2_buffer[tilemap_row_offset + 3] = 0;
        win.bg2_buffer[tilemap_row_offset + 4] = 0;
        win.bg2_buffer[tilemap_row_offset + 5] = 0;

        /* Fill columns 3-28 (26 tiles) with tile indices + 0x2000 (palette/priority) */
        for (int col = 3; col < 29; col++) {
            uint16_t entry = tile_idx + 0x2000;
            int pos = tilemap_row_offset + col * 2;
            win.bg2_buffer[pos]     = (uint8_t)(entry & 0xFF);
            win.bg2_buffer[pos + 1] = (uint8_t)(entry >> 8);
            tile_idx++;
        }

        /* Clear last 3 entries (6 bytes) */
        win.bg2_buffer[tilemap_row_offset + 58] = 0;
        win.bg2_buffer[tilemap_row_offset + 59] = 0;
        win.bg2_buffer[tilemap_row_offset + 60] = 0;
        win.bg2_buffer[tilemap_row_offset + 61] = 0;
        win.bg2_buffer[tilemap_row_offset + 62] = 0;
        win.bg2_buffer[tilemap_row_offset + 63] = 0;
    }

    /* Upload tilemap to VRAM: COPY_TO_VRAM1P win.bg2_buffer, TEXT_LAYER_TILEMAP, $800 */
    memcpy(&ppu.vram[VRAM_TEXT_LAYER_TILEMAP * 2], win.bg2_buffer, 0x800);

    /* Initialize flyover state */
    flyover_tiles_per_row = 26;
    flyover_tile_offset = 0;
    flyover_dirty_min = 0xFFFF;  /* -1 */
    flyover_dirty_max = 0;
    flyover_vwf_x = 0;
    flyover_vwf_y = 0;
    flyover_pixel_offset = 0;
    flyover_byte_offset = 0;

    blank_screen_and_wait_vblank();
}

/* ======================================================================
 * UPLOAD_VWF_BUFFER_TO_VRAM (C49B6E)
 *
 * Inverts the VWF ert.buffer and uploads it to VRAM at the correct position
 * based on flyover_screen_offset. Handles VRAM wrapping when the upload
 * extends past the 32-row boundary.
 *
 * VRAM layout: 32 rows of 26 tiles at TEXT_LAYER_TILES + 0x150 (word offset).
 * Each row = 0x1A0 bytes (26 tiles × 16 bytes). Total = 0x3400 bytes.
 * Upload size = 0x04E0 bytes (3 rows).
 * ====================================================================== */
static void flyover_upload_vwf_to_vram(void) {
    flyover_invert_vwf();

    uint16_t row_start = flyover_screen_offset * FLYOVER_ROW_BYTES;  /* offset * 0x1A0 */
    uint16_t upload_size = 0x04E0;  /* 3 rows */
    uint16_t vram_tile_start = VRAM_TEXT_LAYER_TILES + 0x150;  /* word address */

    if (row_start + upload_size > FLYOVER_VRAM_TOTAL) {
        /* Wrap case: upload spans the 32-row boundary */
        uint16_t first_part = FLYOVER_VRAM_TOTAL - row_start;
        if (first_part > 0) {
            uint32_t vram_dest = (uint32_t)(vram_tile_start * 2) +
                                 (uint32_t)(flyover_screen_offset * 208 * 2);
            memcpy(&ppu.vram[vram_dest], flyover_vwf, first_part);
        }

        uint16_t second_part = upload_size - first_part;
        if (second_part > 0) {
            /* Wrapped portion goes to start of VRAM tile area */
            uint32_t wrap_src_offset = FLYOVER_VRAM_TOTAL - row_start;
            uint32_t vram_dest = (uint32_t)(vram_tile_start * 2);
            memcpy(&ppu.vram[vram_dest], flyover_vwf + wrap_src_offset, second_part);
        }
    } else {
        /* No wrap: straight copy */
        uint32_t vram_dest = (uint32_t)(vram_tile_start * 2) +
                             (uint32_t)(flyover_screen_offset * 208 * 2);
        memcpy(&ppu.vram[vram_dest], flyover_vwf, upload_size);
    }

    flyover_dirty_min = 0xFFFF;
    flyover_dirty_max = 0;

    wait_for_vblank();
}

/* ======================================================================
 * SCROLL_FLYOVER_TEXT (C49C56, US version)
 *
 * Advances the text position by 'advance_y' pixels vertically.
 * Updates screen_offset for VRAM row wrapping.
 * Clears the VWF ert.buffer for the next line of text.
 * Resets horizontal position.
 * ====================================================================== */
static void flyover_scroll_text(uint16_t advance_y) {
    flyover_vwf_y += advance_y;
    flyover_vwf_x = 0;

    /* Assembly (US): advance_y/8 + 1 rows added to screen offset */
    uint16_t rows = (flyover_vwf_y >> 3) + 1;
    flyover_screen_offset += rows;
    if (flyover_screen_offset >= 32)
        flyover_screen_offset -= 32;

    /* US version: wait for DMA, then clear entire VWF ert.buffer */
    memset(flyover_vwf, 0xFF, sizeof(flyover_vwf));

    /* Keep only sub-tile Y offset */
    flyover_vwf_y &= 0x0007;

    /* Reset horizontal position */
    flyover_pixel_offset = 0;
    flyover_byte_offset = 0;
}

/* ======================================================================
 * ADVANCE_FLYOVER_PIXEL_OFFSET (C49CA8)
 *
 * Advances the horizontal pixel position by (param + 8).
 * Updates flyover_byte_offset accordingly.
 * Used by flyover script opcode 0x01 for indentation/spacing.
 * ====================================================================== */
static void flyover_advance_pixel_offset(uint16_t advance) {
    uint16_t val = (advance & 0xFF) + 8;
    flyover_pixel_offset += val;
    /* byte_offset = (pixel_offset / 8) * 16 */
    flyover_byte_offset = (flyover_pixel_offset >> 3) << 4;
}

/* ======================================================================
 * ADVANCE_TEXT_LINE_POSITION (C49D1E)
 *
 * Smoothly scrolls BG3 by advancing a sub-pixel accumulator.
 * Returns the updated accumulator value.
 * Used by COFFEETEA_SCENE for smooth line-by-line scrolling.
 *
 * Assembly: accumulator format = low 8 bits are sub-pixel,
 * high 8 bits count whole-pixel scrolls.
 * ====================================================================== */
static uint16_t flyover_advance_text_line(uint16_t accumulator) {
    oam_clear();

    uint16_t old_high = accumulator & 0xFF00;
    /* US retail: advance by 64 per tick */
    uint16_t new_accum = accumulator + 64;
    uint16_t new_high = new_accum & 0xFF00;

    if (new_high != old_high) {
        /* Crossed a whole-pixel boundary: scroll BG3 */
        int16_t delta = (int16_t)(new_high - old_high);
        /* ASR8_POSITIVE by 8: right shift by 8 = divide by 256 */
        int16_t pixels = delta >> 8;
        ppu.bg_vofs[2] += (uint16_t)pixels;
        update_screen();
    }

    return new_accum;
}

/* ======================================================================
 * UNDRAW_FLYOVER_TEXT (undraw_flyover_text.asm)
 *
 * Restores the normal BG3 text layer configuration after flyover mode.
 * ====================================================================== */
void undraw_flyover_text(void) {
    /* SET_BG3_VRAM_LOCATION(NORMAL, 0x7C00, 0x6000) — same as init */
    set_bg3_vram_location(0, VRAM_TEXT_LAYER_TILEMAP, VRAM_TEXT_LAYER_TILES);

    /* Restore battle screen tilemap to VRAM */
    upload_battle_screen_to_vram();

    /* Reload window border GFX */
    text_load_window_gfx();

    /* US version: upload text tiles (param=2) */
    upload_text_tiles_to_vram(2);

    /* Reload character window palette */
    load_character_window_palette();

    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
}

/* ======================================================================
 * Helper: load_background_animation (load_background_animation.asm)
 *
 * Sets up BG mode 1, configures BG1/BG2 VRAM locations, loads battle BG.
 * Used by COFFEETEA_SCENE for coffee/tea animated backgrounds.
 * ====================================================================== */
void load_background_animation(uint16_t bg1_layer, uint16_t bg2_layer) {
    force_blank_and_wait_vblank();

    /* SET_BGMODE(9): mode 1 + BG3 priority bit */
    ppu.bgmode = 0x09;

    /* SET_BG1_VRAM_LOCATION(size=NORMAL, tilemap=0x5800, tiles=0x0000) */
    ppu.bg_sc[0] = 0x58;
    ppu.bg_nba[0] = (uint8_t)(ppu.bg_nba[0] & 0xF0);  /* BG1 tile base = 0 */
    ppu.bg_hofs[0] = 0;
    ppu.bg_vofs[0] = 0;

    /* SET_BG2_VRAM_LOCATION(size=NORMAL, tilemap=0x5C00, tiles=0x1000) */
    ppu.bg_sc[1] = 0x5C;
    ppu.bg_nba[0] = (uint8_t)((ppu.bg_nba[0] & 0x0F) | 0x10);  /* BG2 tile base = 0x1000 */
    ppu.bg_hofs[1] = 0;
    ppu.bg_vofs[1] = 0;

    /* LOAD_BATTLE_BG(bg1_layer, bg2_layer, letterbox=4) */
    load_battle_bg(bg1_layer, bg2_layer, 4);

    blank_screen_and_wait_vblank();
}

/* ======================================================================
 * Helper: blocking fade with mosaic.
 * Port of FADE_IN_WITH_MOSAIC / FADE_OUT_WITH_MOSAIC.
 * These loop internally, calling wait_for_vblank() each step.
 * ====================================================================== */
static void flyover_fade_in_blocking(uint16_t step, uint16_t delay_frames,
                                      uint16_t mosaic_enable) {
    ppu.inidisp = 0x00;  /* brightness 0, force blank off */
    ppu.mosaic = 0;

    while (1) {
        uint8_t brightness = ppu.inidisp & 0x0F;
        uint16_t next = brightness + step;
        if (next >= 0x0F) {
            ppu.inidisp = (ppu.inidisp & 0xF0) | 0x0F;
            break;
        }
        ppu.inidisp = (ppu.inidisp & 0xF0) | (uint8_t)next;

        if (mosaic_enable) {
            uint8_t inv = (~brightness) & 0x0F;
            ppu.mosaic = (uint8_t)((inv << 4) | mosaic_enable);
        }

        for (uint16_t i = 0; i < delay_frames; i++)
            wait_for_vblank();
    }
}

static void flyover_fade_out_blocking(uint16_t step, uint16_t delay_frames,
                                       uint16_t mosaic_enable) {
    while (1) {
        ppu.mosaic = 0;
        uint8_t brightness = ppu.inidisp & 0x0F;
        if (ppu.inidisp & 0x80) break;
        int16_t next = (int16_t)brightness - (int16_t)step;
        if (next < 0) break;
        ppu.inidisp = (ppu.inidisp & 0xF0) | (uint8_t)next;

        if (mosaic_enable) {
            uint8_t inv = (~(uint8_t)next) & 0x0F;
            ppu.mosaic = (uint8_t)((inv << 4) | mosaic_enable);
        }

        for (uint16_t i = 0; i < delay_frames; i++)
            wait_for_vblank();
    }
    ppu.inidisp = 0x80;  /* force blank */
}

/* ======================================================================
 * PLAY_FLYOVER_SCRIPT (C49EC4)
 *
 * Bytecode interpreter for flyover text scripts.
 * Opcodes:
 *   0x00: end of script
 *   0x01 <byte>: advance pixel offset
 *   0x02 <byte>: set screen offset
 *   0x08 <byte>: render party member name
 *   0x09: upload VWF to VRAM, wait, scroll
 *   Other: render as EB character
 *
 * After parsing, fades in (mosaic), displays for 180 frames, fades out.
 * Then restores normal display.
 * ====================================================================== */
void play_flyover_script(uint16_t id) {
    /* Script file names for each flyover ID.
     * Use .bin extension — ebtools decodes .flyover into assembly source,
     * but the C port needs raw binary bytecodes. */
    static const AssetId script_ids[8] = {
        ASSET_FLYOVER_INTRO1_BIN,
        ASSET_FLYOVER_INTRO2_BIN,
        ASSET_FLYOVER_INTRO3_BIN,
        ASSET_FLYOVER_WINTERS_INTRO1_BIN,
        ASSET_FLYOVER_WINTERS_INTRO2_BIN,
        ASSET_FLYOVER_DALAAM_INTRO1_BIN,
        ASSET_FLYOVER_DALAAM_INTRO2_BIN,
        ASSET_FLYOVER_ENDING_BIN,
    };

    if (id >= 8) return;

    /* Assembly lines 11-15: save entity 23 tick_callback_hi then disable.
     * Prevents UPDATE_OVERWORLD_FRAME from running during the flyover.
     * The saved value is restored at the end so entity 23 returns to its
     * pre-flyover state (enabled or disabled). */
    uint16_t saved_ent23_tick_hi = entities.tick_callback_hi[ENT(23)];
    entities.tick_callback_hi[ENT(23)] = saved_ent23_tick_hi |
        (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);

    flyover_init_screen();

    /* Load script data */
    size_t script_size = ASSET_SIZE(script_ids[id]);
    const uint8_t *script = ASSET_DATA(script_ids[id]);
    if (!script) return;

    dt.enable_word_wrap = 0;

    /* Parse script bytecodes */
    size_t pos = 0;
    while (pos < script_size) {
        uint8_t opcode = script[pos++];

        if (opcode == 0x00) {
            /* End of script */
            break;
        } else if (opcode == 0x02) {
            /* Set screen offset */
            if (pos < script_size) {
                flyover_screen_offset = script[pos++];
            }
        } else if (opcode == 0x09) {
            /* Upload ert.buffer, wait, scroll */
            flyover_upload_vwf_to_vram();
            wait_for_vblank();
            flyover_scroll_text(24);
        } else if (opcode == 0x01) {
            /* Advance pixel offset */
            if (pos < script_size) {
                flyover_advance_pixel_offset(script[pos++]);
            }
        } else if (opcode == 0x08) {
            /* Render party member name */
            if (pos < script_size) {
                uint16_t char_id = script[pos++];
                flyover_render_party_name(char_id);
            }
        } else {
            /* Render as EB character */
            flyover_render_text_char((uint16_t)opcode);
        }
    }

    /* Fade in with mosaic: TM=$04 (BG3 only), step=1, delay=3, no mosaic */
    ppu.tm = 0x04;
    flyover_fade_in_blocking(1, 3, 0);

    /* Display for 180 frames */
    for (int i = 0; i < 180; i++) {
        wait_for_vblank();
    }

    /* Fade out: step=1, delay=3, no mosaic */
    flyover_fade_out_blocking(1, 3, 0);

    /* Restore TM to normal: $17 = BG1+BG2+BG3+OBJ */
    ppu.tm = 0x17;

    /* Clear BG2 ert.buffer: 0x380 words = 0x700 bytes */
    memset(win.bg2_buffer, 0, 0x700);

    dt.enable_word_wrap = (uint16_t)-1;

    force_blank_and_wait_vblank();
    undraw_flyover_text();

    /* Assembly line 119-120: restore entity 23 tick_callback_hi to pre-flyover state */
    entities.tick_callback_hi[ENT(23)] = saved_ent23_tick_hi;

    blank_screen_and_wait_vblank();
}

/* ======================================================================
 * COFFEETEA_SCENE (coffee_tea_scene.asm)
 *
 * Full-screen animated text sequence for coffee/tea breaks.
 * type: 0 = coffee, 1 = tea.
 *
 * Flow:
 *   1. Fade out with mosaic
 *   2. Init flyover screen + load coffee/tea battle BG
 *   3. Clear OAM, fade in
 *   4. Parse text script (similar to flyover but with smooth scrolling)
 *   5. Fade out, wait for completion
 *   6. Reload map, clear BG2, undraw flyover, fade in
 * ====================================================================== */

/* BATTLEBG_LAYER enum values from battlebgs.asm */
#define BATTLEBG_COFFEE1 231
#define BATTLEBG_COFFEE2 232
#define BATTLEBG_TEA1    233
#define BATTLEBG_TEA2    234

void coffeetea_scene(uint16_t type) {
    /* 1. Fade out with mosaic: step=1, delay=1, no mosaic */
    flyover_fade_out_blocking(1, 1, 0);

    /* 2. Init flyover text screen */
    flyover_init_screen();
    oam_clear();

    /* 3. Load coffee/tea background animation */
    uint16_t bg1 = (type == 0) ? BATTLEBG_COFFEE1 : BATTLEBG_TEA1;
    uint16_t bg2 = (type == 0) ? BATTLEBG_COFFEE2 : BATTLEBG_TEA2;
    load_background_animation(bg1, bg2);

    /* 4. Fade in: step=1, delay=1 */
    fade_in(1, 1);

    /* 5. Set initial screen offset */
    flyover_screen_offset = 28;

    /* 6. Load text script */
    AssetId coffee_tea_id = type == 0 ? ASSET_COFFEE_BIN : ASSET_TEA_BIN;
    size_t script_size = ASSET_SIZE(coffee_tea_id);
    const uint8_t *script = ASSET_DATA(coffee_tea_id);
    if (!script) return;

    dt.enable_word_wrap = 0;

    uint16_t scroll_accum = 0;  /* @VIRTUAL04 in assembly */

    /* Parse script bytecodes */
    size_t pos = 0;
    while (pos < script_size) {
        uint8_t opcode = script[pos++];

        if (opcode == 0x00) {
            break;
        } else if (opcode == 0x09) {
            /* Smooth scroll sequence:
             * 1. advance_text_line_position with current accum
             * 2. Upload VWF ert.buffer
             * 3. Update battle screen effects
             * 4. Loop scrolling until accumulator crosses 8192 (0x2000)
             * 5. Subtract 8192, scroll flyover text */
            scroll_accum = flyover_advance_text_line(scroll_accum);
            flyover_upload_vwf_to_vram();
            update_battle_screen_effects();

            while (scroll_accum < 8192) {
                scroll_accum = flyover_advance_text_line(scroll_accum);
                wait_and_update_battle_effects();
            }
            scroll_accum -= 8192;
            flyover_scroll_text(24);
        } else if (opcode == 0x01) {
            /* Advance pixel offset */
            if (pos < script_size) {
                flyover_advance_pixel_offset(script[pos++]);
            }
        } else if (opcode == 0x08) {
            /* Render party member name */
            if (pos < script_size) {
                uint16_t char_id = script[pos++];
                flyover_render_party_name(char_id);
            }
        } else {
            /* Render as EB character */
            flyover_render_text_char((uint16_t)opcode);
        }
    }

    /* Fade out: step=1, delay=1 */
    fade_out(1, 1);

    /* Wait for fade completion while updating battle effects */
    while (fade_active()) {
        wait_and_update_battle_effects();
        fade_update();
    }

    /* Cleanup */
    force_blank_and_wait_vblank();
    reload_map();

    /* Clear BG2 ert.buffer: 896 words = 1792 bytes */
    memset(win.bg2_buffer, 0, 1792);

    dt.enable_word_wrap = 0xFF;

    force_blank_and_wait_vblank();
    undraw_flyover_text();
    blank_screen_and_wait_vblank();

    /* Fade in: step=1, delay=1 */
    fade_in(1, 1);
}
