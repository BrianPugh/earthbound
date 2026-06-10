#include "game/text.h"
#include "game/window.h"
#include "game/display_text.h"
#include "game/display_text_internal.h"  /* dt_make_child_init */
#include "game/game_state.h"
#include "game/inventory.h"
#include "game/overworld.h"
#include "game/audio.h"
#include "game/battle.h"
#include "game/map_loader.h"
#include "core/memory.h"
#include "core/log.h"
#include "core/mode_stack.h"
#include "core/embedded.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "snes/ppu.h"
#include "core/decomp.h"
#include "data/assets.h"
#include "include/binary.h"
#include "include/constants.h"
#include "include/pad.h"
#include "platform/platform.h"
#include <string.h>
#include <stdio.h>

#define CMD_LABEL_SIZE 10  /* CMD_WINDOW_TEXT entry size (EB-encoded, null-padded) */

/* --- Font data loaded from assets --- */

typedef struct {
    const uint8_t *glyph_data;  /* 1bpp glyph bitmaps */
    const uint8_t *width_data;  /* per-character pixel widths */
    size_t   glyph_size;  /* total glyph data size */
    uint8_t  height;      /* pixel rows per glyph */
    uint8_t  bytes_per_glyph; /* bytes per glyph in data */
    bool     loaded;
} FontData;

static FontData fonts[5]; /* NORMAL, MRSATURN, BATTLE, TINY, LARGE */

/* window_gfx_loaded: tracks whether text_load_window_gfx() has been called.
 * The decompressed data goes into the global ert.buffer[] (entity.h),
 * matching the assembly's use of BUFFER for LOAD_WINDOW_GFX.
 * upload_text_tiles_to_vram() reads from the same ert.buffer[]. */
static bool window_gfx_loaded = false;

/* Flavour ert.palettes (pointer to asset data, 8 flavours x 8 sub-ert.palettes x 4 colors x 2 bytes) */
static const uint16_t *flavour_palettes = NULL;

/* --- VWF State --- */

uint8_t __attribute__((aligned(4))) vwf_buffer[VWF_BUFFER_SIZE];
uint16_t vwf_x;        /* current pixel X position */
uint16_t vwf_tile;     /* current tile index in ert.buffer */
static uint16_t vwf_pixels_rendered; /* tracks how many pixels rendered */

/* VWF save/restore aliases into the tail of ert.buffer (BUF_VWF_SAVE).
 * Phase-exclusive with pathfinding — see buffer_layout.h for details. */
_Static_assert(VWF_BUFFER_SIZE == BUF_VWF_SAVE_SIZE,
               "BUF_VWF_SAVE_SIZE in buffer_layout.h must match VWF_BUFFER_SIZE");
#define vwf_saved_buffer (ert.buffer + BUF_VWF_SAVE)

/* Extra pixels added between each character (US = 1, naming screen = 0) */
uint8_t character_padding = 1;

/* TEXT_RENDER_STATE: tracks current VWF tile VRAM positions.
 * Port of assembly's TEXT_RENDER_STATE BSS struct.
 * pixels_rendered: VWF_X value at last flush
 * upper_vram_position: VRAM tile ID for current upper 8x8 tile (0 = none)
 * lower_vram_position: VRAM tile ID for current lower 8x8 tile (0 = none) */
typedef struct {
    uint16_t pixels_rendered;
    uint16_t upper_vram_position;
    uint16_t lower_vram_position;
} TextRenderState;

static TextRenderState text_render_state;

/* --- Font loading --- */

static bool load_font(uint8_t font_id, AssetId gfx_id, AssetId width_id,
                      uint8_t height) {
    if (font_id >= 5) return false;

    FontData *f = &fonts[font_id];

    f->glyph_size = ASSET_SIZE(gfx_id);
    f->glyph_data = ASSET_DATA(gfx_id);
    if (!f->glyph_data) {
        LOG_WARN("Failed to load font gfx (asset id %d)\n", gfx_id);
        return false;
    }

    f->width_data = ASSET_DATA(width_id);
    if (!f->width_data) {
        LOG_WARN("Failed to load font widths (asset id %d)\n", width_id);
        f->glyph_data = NULL;
        return false;
    }

    f->height = height;
    f->bytes_per_glyph = (uint8_t)(f->glyph_size / FONT_CHAR_COUNT);
    f->loaded = true;
    return true;
}

const uint8_t *font_get_glyph(uint8_t font_id, uint8_t char_index) {
    if (font_id >= 5 || !fonts[font_id].loaded) return NULL;
    if (char_index >= FONT_CHAR_COUNT) return NULL;
    return fonts[font_id].glyph_data + (size_t)char_index * fonts[font_id].bytes_per_glyph;
}

uint8_t font_get_width(uint8_t font_id, uint8_t char_index) {
    if (font_id >= 5 || !fonts[font_id].loaded) return 8;
    if (char_index >= FONT_CHAR_COUNT) return 8;
    return fonts[font_id].width_data[char_index];
}

uint8_t font_get_height(uint8_t font_id) {
    if (font_id >= 5 || !fonts[font_id].loaded) return 8;
    return fonts[font_id].height;
}

/* --- Convert 1bpp to 2bpp for VRAM --- */

static void convert_1bpp_to_2bpp(const uint8_t *src, uint8_t *dst, int rows) {
    /* Font glyph data is stored in inverted format:
       0 = drawn pixel, 1 = background (already matches plane 1 convention).
       Result: glyph pixels → color 1 (near-white text),
               background  → color 3 (near-black, opaque).
       This keeps text tiles opaque so the battle BG doesn't bleed through. */
    for (int row = 0; row < rows; row++) {
        dst[row * 2]     = 0xFF;      /* plane 0: always set */
        dst[row * 2 + 1] = src[row];  /* plane 1: already inverted in asset */
    }
}

/* --- Text system init --- */

void text_system_init(void) {
    /* Assembly: ENABLE_WORD_WRAP is set to 0xFF during file select
     * (show_file_select_menu.asm:60) and stays set for the rest of the game.
     * Initialize it here since the C port doesn't run that assembly path. */
    dt.enable_word_wrap = 0xFF;

    /* Load fonts from extracted assets */
    /* main.gfx: 3072 bytes = 96 chars x 32 bytes (1bpp, 16px tall interleaved) */
    load_font(FONT_ID_NORMAL, ASSET_FONTS_MAIN_GFX, ASSET_FONTS_MAIN_BIN, 16);

    /* tiny.gfx: 768 bytes = 96 chars x 8 bytes (1bpp, 8px tall) */
    load_font(FONT_ID_TINY, ASSET_FONTS_TINY_GFX, ASSET_FONTS_TINY_BIN, 8);

    /* battle.gfx: 1536 bytes = 96 chars x 16 bytes (1bpp, 16px tall) */
    load_font(FONT_ID_BATTLE, ASSET_FONTS_BATTLE_GFX, ASSET_FONTS_BATTLE_BIN, 16);

    /* large.gfx: 3072 bytes = 96 chars x 32 bytes */
    load_font(FONT_ID_LARGE, ASSET_FONTS_LARGE_GFX, ASSET_FONTS_LARGE_BIN, 16);

    /* mrsaturn.gfx: 3072 bytes = 96 chars x 32 bytes */
    load_font(FONT_ID_MRSATURN, ASSET_FONTS_MRSATURN_GFX, ASSET_FONTS_MRSATURN_BIN, 16);

    /* Cursor arrow tiles (0x41/0x51 and 0x28D/0x29D) come from the
       TEXT_WINDOW_GFX asset uploaded by text_load_window_gfx(). */
}

void text_upload_font_tiles(void) {
    /* Upload tiny font glyphs to VRAM so fixed-width PRINT_STRING rendering works.
       The TEXT_WINDOW_GFX scattered upload only covers a subset of EB character
       positions.  This fills in all 96 characters (EB codes 0x50-0xAF) using
       the tiny font (8x8 1bpp → 2bpp conversion). */
    FontData *f = &fonts[FONT_ID_TINY];
    if (!f->loaded) return;

    uint32_t vram_base = VRAM_TEXT_LAYER_TILES * 2;

    for (int i = 0; i < FONT_CHAR_COUNT && i < (int)(f->glyph_size / f->bytes_per_glyph); i++) {
        uint16_t tile_idx = 0x50 + i; /* EB char code = tile index */
        const uint8_t *glyph_1bpp = f->glyph_data + (size_t)i * f->bytes_per_glyph;

        uint8_t tile_2bpp[16];
        convert_1bpp_to_2bpp(glyph_1bpp, tile_2bpp, 8);

        uint32_t vram_offset = vram_base + tile_idx * 16;
        if (vram_offset + 16 <= VRAM_SIZE) {
            memcpy(ppu.vram + vram_offset, tile_2bpp, 16);
        }
    }
}

void text_load_window_gfx(void) {
    /* Load and decompress TEXT_WINDOW_GFX (locale-specific asset) */
    size_t compressed_size = ASSET_SIZE(ASSET_GRAPHICS_TEXT_WINDOW_GFX_LZHAL);
    const uint8_t *compressed = ASSET_DATA(ASSET_GRAPHICS_TEXT_WINDOW_GFX_LZHAL);
    if (!compressed) {
        LOG_WARN("Failed to load text window graphics\n");
        return;
    }

    /* Text window GFX processing needs up to ~19 KB (memmove to 0x4A00).
     * Uses ert.buffer — MUST NOT use decomp_staging (arrangement_buffer)
     * because arrangement data is needed for ongoing overworld tile lookups.
     * Using decomp_staging here caused Tenda Village sprite corruption: the
     * map arrangement was clobbered, so tile rendering produced garbage on
     * first movement after a text/window GFX reload.
     *
     * This function is the sole reason BUFFER_SIZE is 20 KB instead of 16 KB.
     * To reduce further, the in-place rearrangement (memmove from 0x1000 to
     * 0x2000, 0x2A00 bytes) and VWF name compositing would need to be
     * restructured to work within a smaller region or write directly to
     * ppu.vram. */
    memset(ert.buffer, 0, BUFFER_SIZE);
    size_t decompressed_size = decomp(compressed, compressed_size,
                                      ert.buffer, BUFFER_SIZE);

    if (decompressed_size == 0) {
        LOG_WARN("Failed to decompress text window graphics\n");
        return;
    }

    window_gfx_loaded = true;

    /*
     * Per FILE_SELECT_INIT assembly (USA path):
     *
     * 1. MEMCPY24: copy $2A00 bytes from BUFFER+$2000 to BUFFER+$1000
     *    This rearranges the decompressed tile data in-place.
     *
     * 2. UPLOAD_TEXT_TILES_TO_VRAM mode 1:
     *    Mode 1 first:  BUFFER+$2000 → TEXT_LAYER_TILES+$1000, $1800 bytes
     *    Falls through to mode 0 (scattered text tile uploads):
     *      BUFFER+$0000 → TEXT_LAYER_TILES+$0000, $0450 bytes
     *      BUFFER+$04F0 → TEXT_LAYER_TILES+$0278, $0060 bytes
     *      BUFFER+$05F0 → TEXT_LAYER_TILES+$02F8, $00B0 bytes
     *      BUFFER+$0700 → TEXT_LAYER_TILES+$0380, $00A0 bytes
     *      BUFFER+$0800 → TEXT_LAYER_TILES+$0400, $0010 bytes
     *      BUFFER+$0900 → TEXT_LAYER_TILES+$0480, $0010 bytes
     *
     * VRAM addresses in assembly are word addresses.
     * TEXT_LAYER_TILES = $6000 word = $C000 byte.
     * The COPY_TO_VRAM1 destination offsets are word offsets, so:
     *   +$1000 word = +$2000 byte, +$0278 word = +$04F0 byte, etc.
     * The ert.buffer source offsets and VRAM byte offsets (relative to base) match.
     */

    /* Step 1: Rearrange in-place.
       Assembly MEMCPY24 copies $2A00 bytes from BUFFER+$1000 to BUFFER+$2000.
       (file_select_init.asm: @LOCAL00=dest=DP+$0E=BUFFER+$2000,
        @LOCAL01=src=DP+$12=BUFFER+$1000; MEMCPY24 reads [$12] writes [$0E])
       This shifts tile data up by $1000 so that the mode 1 upload (from BUFFER+$2000)
       reads the decompressed tiles that were originally at BUFFER+$1000.
       With decompressed size 0x1A00, BUFFER+$1000..$19FF has valid tile data;
       after this copy, BUFFER+$2000..$29FF gets that data, making cursor frame 1
       tiles (0x28D at buf[$28D0] = original buf[$18D0]) available. */
    memmove(ert.buffer + 0x2000, ert.buffer + 0x1000, 0x2A00);

    /* Step 1b: Conditional FLAVOURED_TEXT_GFX overlay (load_window_gfx.asm lines 29-43).
     * TEXT_WINDOW_PROPERTIES is a 5-entry table (3 bytes each: WORD offset, BYTE property).
     * Property byte == 8 means decompress FLAVOURED_TEXT_GFX to BUFFER+$100.
     * US retail: Plain (flavour 1) has property 1, all others (2-5) have property 8. */
    {
        static const uint8_t flavour_properties[5] = { 0x01, 0x08, 0x08, 0x08, 0x08 };
        uint8_t flavour_idx = game_state.text_flavour;  /* 1-indexed, matching assembly */
        if (flavour_idx >= 1 && flavour_idx <= 5) {
            uint8_t property = flavour_properties[flavour_idx - 1];  /* DEC to 0-index */
            if (property == 8) {
                size_t flav_comp_size = ASSET_SIZE(ASSET_GRAPHICS_FLAVOURED_TEXT_GFX_LZHAL);
                const uint8_t *flav_comp = ASSET_DATA(ASSET_GRAPHICS_FLAVOURED_TEXT_GFX_LZHAL);
                if (flav_comp) {
                    decomp(flav_comp, flav_comp_size,
                           ert.buffer + 0x100, BUFFER_SIZE - 0x100);
                }
            }
        }
    }

    /* Step 1c: Render character names into ert.buffer+$2A00 (load_window_gfx.asm lines 45-188).
     * For each of the 4 party characters, render their name using the BATTLE font
     * via direct BLIT_VWF_GLYPH calls with a fixed 6px advance, then copy the
     * rendered tiles into the ert.buffer for VRAM upload. */
    {
        uint8_t font_height = font_get_height(FONT_ID_BATTLE);

        for (int char_idx = 0; char_idx < 4; char_idx++) {
            /* Fill VWF ert.buffer with 0xFF (opaque dark background = color 3) */
            memset(vwf_buffer, 0xFF, VWF_BUFFER_SIZE);

            /* Reset VWF state: tile 0, x=2 (2px left margin, matching asm lines 59-60, 77-78) */
            vwf_tile = 0;
            vwf_x = 2;
            memset(&text_render_state, 0, sizeof(text_render_state));

            /* Render each character of the name (asm lines 82-105).
             * Assembly calls BLIT_VWF_GLYPH directly with fixed 6px advance (@LOCAL06),
             * bypassing RENDER_VWF_CHARACTER. */
            for (int i = 0; i < 5 && party_characters[char_idx].name[i]; i++) {
                uint8_t eb_char = party_characters[char_idx].name[i];
                if (eb_char < 0x50) continue;
                uint8_t char_index = (eb_char - 0x50) & 0x7F;
                if (char_index >= FONT_CHAR_COUNT) continue;
                const uint8_t *glyph = font_get_glyph(FONT_ID_BATTLE, char_index);
                if (!glyph) continue;
                blit_vwf_glyph(glyph, font_height, 6);
            }

            /* Copy 4 tiles from VWF ert.buffer into ert.buffer+$2A00 (asm lines 109-183).
             * Layout: upper 8x8 halves contiguous, then lower 8x8 halves at +256.
             * Each VWF tile = 32 bytes (16 upper + 16 lower). */
            for (int tile = 0; tile < 4; tile++) {
                /* Upper 8x8 half (first 16 bytes of VWF tile) */
                memcpy(ert.buffer + 0x2A00 + char_idx * 64 + tile * 16,
                       vwf_buffer + tile * VWF_TILE_BYTES, 16);
                /* Lower 8x8 half (next 16 bytes of VWF tile) */
                memcpy(ert.buffer + 0x2A00 + char_idx * 64 + tile * 16 + 256,
                       vwf_buffer + tile * VWF_TILE_BYTES + 16, 16);
            }
        }
    }

    /* Step 1d: Composite checkerboard pattern into name tiles (load_window_gfx.asm lines 189-226).
     * The checkerboard tile at BUFFER+$70 is applied to background pixels of all 32 name
     * tile halves (4 chars × 4 tiles × 2 halves = 32 × 16 bytes). For each 2bpp row:
     * plane 0 gets the checkerboard pattern OR'd with the inverted plane 1 (background mask),
     * while plane 1 (glyph pixels) is preserved unchanged. */
    {
        uint8_t *name_data = ert.buffer + 0x2A00;
        const uint8_t *checker = ert.buffer + 0x70;  /* checkerboard pattern tile */

        for (int tile = 0; tile < 32; tile++) {
            for (int row = 0; row < 8; row++) {
                int off = tile * 16 + row * 2;
                uint8_t plane1 = name_data[off + 1];  /* high byte */
                uint8_t mask = ~plane1;                /* background pixels */
                uint8_t check = checker[row * 2];      /* checkerboard plane 0 */
                name_data[off] = check | mask;         /* apply pattern to plane 0 */
                /* plane 1 unchanged */
            }
        }
    }

    /* Step 1e: Composite checkerboard into status equip text tiles
     * (load_window_gfx.asm lines 227-300).
     * Iterates STATUS_EQUIP_WINDOW_TEXT_2 entries; for each non-blank entry,
     * looks up the source tile in BUFFER, applies the checkerboard pattern
     * (same algorithm as name tiles), and writes to BUFFER+$2C00.
     * These become VRAM tiles 0x2C0+ — the status affliction icons shown
     * in the HP/PP window and equip screen. */
    {
        const uint8_t *raw = ASSET_DATA(ASSET_DATA_STATUS_EQUIP_TILE_TABLES_BIN);
        /* TEXT_2 starts at offset 49 words (7 slots × 7 cols) past TEXT_1 */
        const uint16_t *text2 = (const uint16_t *)(raw + AFFLICTION_GROUP_COUNT * 7 * 2);
        uint8_t *dest = ert.buffer + 0x2C00;
        const uint8_t *checker = ert.buffer + 0x70;

        for (int i = 0; i < AFFLICTION_GROUP_COUNT * 7; i++) {
            uint16_t val = text2[i];
            if (val == 0) break;    /* null terminator */
            if (val == 32) continue; /* blank — skip without advancing dest */

            /* Source tile: ((val & 0xFFF0) + val) × 16 bytes into BUFFER */
            uint32_t src_off = (uint32_t)((val & 0xFFF0) + val) * 16;
            const uint8_t *src = ert.buffer + src_off;

            /* Upper 8×8 half (8 rows × 2 bytes) */
            for (int row = 0; row < 8; row++) {
                uint8_t plane1 = src[row * 2 + 1];
                dest[row * 2]     = checker[row * 2] | (uint8_t)~plane1;
                dest[row * 2 + 1] = plane1;
            }
            /* Lower 8×8 half (+256 bytes) */
            for (int row = 0; row < 8; row++) {
                uint8_t plane1 = src[256 + row * 2 + 1];
                dest[256 + row * 2]     = checker[row * 2] | (uint8_t)~plane1;
                dest[256 + row * 2 + 1] = plane1;
            }
            dest += 16;
        }
    }

    /* Step 2: Upload tiles to VRAM matching UPLOAD_TEXT_TILES_TO_VRAM mode 1 */
    uint32_t vram_base = VRAM_TEXT_LAYER_TILES * 2; /* $C000 byte address */

    /* Mode 1: border/window tiles from BUFFER+$2000 */
    memcpy(ppu.vram + vram_base + 0x2000, ert.buffer + BUF_TEXT_LAYER2_TILES, 0x1800);

    /* Mode 0 fallthrough: scattered text/UI tile uploads */
    memcpy(ppu.vram + vram_base + 0x0000, ert.buffer + BUF_TEXT_TILES_BLOCK1, 0x0450);
    memcpy(ppu.vram + vram_base + 0x04F0, ert.buffer + BUF_TEXT_TILES_BLOCK2, 0x0060);
    memcpy(ppu.vram + vram_base + 0x05F0, ert.buffer + BUF_TEXT_TILES_BLOCK3, 0x00B0);
    memcpy(ppu.vram + vram_base + 0x0700, ert.buffer + BUF_TEXT_TILES_BLOCK4, 0x00A0);
    memcpy(ppu.vram + vram_base + 0x0800, ert.buffer + BUF_TEXT_TILES_BLOCK5, 0x0010);
    memcpy(ppu.vram + vram_base + 0x0900, ert.buffer + BUF_TEXT_TILES_BLOCK6, 0x0010);
}

void text_load_flavour_palette(uint8_t flavour) {
    if (!flavour_palettes) {
        const uint8_t *pal_data = ASSET_DATA(ASSET_GRAPHICS_TEXT_WINDOW_FLAVOUR_PALETTES_PAL);
        if (pal_data) {
            flavour_palettes = (const uint16_t *)pal_data;
        } else {
            LOG_WARN("Failed to load flavour palettes\n");
            return;
        }
    }

    /* Apply the selected flavour to ert.palettes[] mirror (BG ert.palettes 0-1).
       Assembly writes to PALETTES, then NMI syncs to CGRAM hardware.
       (2 ert.palettes x 16 colors = 32 colors = BPP4PALETTE_SIZE * 2 bytes) */
    const uint16_t *src = &flavour_palettes[flavour * 32];
    memcpy(ert.palettes, src, 32 * sizeof(uint16_t));
    memcpy(ppu.cgram, src, 32 * sizeof(uint16_t));
}

/* Port of LOAD_CHARACTER_WINDOW_PALETTE (C47F87.asm).
 * Checks the last party member's status — if unconscious/diamondized
 * (and transitions not disabled), loads a special death palette.
 * Otherwise loads the palette for the current text_flavour. */
void load_character_window_palette(void) {
    /* Ensure flavour palette data is loaded */
    if (!flavour_palettes) {
        const uint8_t *pal_data = ASSET_DATA(ASSET_GRAPHICS_TEXT_WINDOW_FLAVOUR_PALETTES_PAL);
        if (pal_data) {
            flavour_palettes = (const uint16_t *)pal_data;
        } else {
            return;
        }
    }

    /* Assembly lines 7-20: check last party member's status */
    uint8_t count = game_state.player_controlled_party_count;
    if (count > 0) {
        uint8_t char_id = game_state.player_controlled_party_members[count - 1];
        uint8_t status = party_characters[char_id].afflictions[0]; /* PERSISTENT_EASYHEAL */
        if ((status == 1 || status == 2) && !ow.disabled_transitions) {
            /* UNCONSCIOUS or DIAMONDIZED: load death palette (set 5, offset 320) */
            memcpy(ert.palettes, &flavour_palettes[5 * 32], 32 * sizeof(uint16_t));
            ert.palettes[0] = 0; /* force color 0 transparent (assembly line 50) */
            ert.palette_upload_mode = PALETTE_UPLOAD_BG_ONLY;
            return;
        }
    }

    /* Normal path: load palette for current text_flavour (1-indexed, DEC to 0-index) */
    uint8_t flavour_raw = game_state.text_flavour;
    if (flavour_raw == 0 || flavour_raw > 7)
        return;
    uint8_t flavour = flavour_raw - 1;  /* assembly DEC */
    memcpy(ert.palettes, &flavour_palettes[flavour * 32], 32 * sizeof(uint16_t));

    ert.palettes[0] = 0; /* force color 0 transparent (assembly line 50) */
    ert.palette_upload_mode = PALETTE_UPLOAD_BG_ONLY;
}

/* Port of UPDATE_TEXT_WINDOW_PALETTE (C3E450.asm).
 * Alternates between two sub-palette offsets based on core.frame_counter bit 2,
 * creating a blinking animation on the HP/PP window borders.
 * Writes 4 colors (BPP2PALETTE_SIZE) to palette sub-palette 5. */
void update_text_window_palette(void) {
    if (!flavour_palettes)
        return;

    uint8_t flavour_raw = game_state.text_flavour;  /* 1-indexed */
    if (flavour_raw == 0 || flavour_raw > 7)
        return;
    uint8_t flavour = flavour_raw - 1;  /* assembly DEC */

    /* Assembly: bit 2 of core.frame_counter selects between two sub-palette sources.
     * Bit 2 set → offset +8 bytes (sub-palette 1, colors 4-7)
     * Bit 2 clear → offset +40 bytes (sub-palette 5, colors 20-23) */
    const uint16_t *flavour_base = &flavour_palettes[flavour * 32];
    const uint16_t *src;
    if (core.frame_counter & 4)
        src = &flavour_base[4];   /* sub-palette 1 (offset +8 bytes) */
    else
        src = &flavour_base[20];  /* sub-palette 5 (offset +40 bytes) */

    /* Copy 4 colors to palette sub-palette 5 (ert.palettes[20..23]) */
    memcpy(&ert.palettes[20], src, 4 * sizeof(uint16_t));
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
}

/* ---- SHOW_HPPP_WINDOWS (port of asm/text/show_hppp_windows.asm) ----
 * Assembly:
 *   JSR CLEAR_BATTLE_MENU_CHARACTER_INDICATOR
 *   LDA #1 → STA RENDER_HPPP_WINDOWS, STA REDRAW_ALL_WINDOWS
 *   LDA #-1 → STA CURRENTLY_DRAWN_HPPP_WINDOWS */
void show_hppp_windows(void) {
    clear_battle_menu_character_indicator();
    ow.render_hppp_windows = 1;
    ow.redraw_all_windows = 1;
    ow.currently_drawn_hppp_windows = 0xFF;  /* force redraw of all HPPP windows */
}

/* Port of DISPLAY_MONEY_WINDOW (asm/text/window/display_money_window.asm).
 * Saves text attributes, creates the carried-money window, prints "$N",
 * then restores attributes. Uses its own backup slot (separate from
 * CC_18_02's display_text_state backup). */
void display_money_window(void) {
    /* Save current window text attributes (local backup) */
    uint16_t saved_id = win.current_focus_window;
    uint16_t saved_text_x = 0, saved_text_y = 0, saved_pixel_x = 0, saved_attrs = 0, saved_font = 0;
    uint8_t saved_padding = 0;
    bool have_backup = false;
    if (saved_id != WINDOW_ID_NONE) {
        WindowInfo *sw = get_window(saved_id);
        if (sw) {
            saved_text_x = sw->text_x;
            saved_text_y = sw->text_y;
            saved_pixel_x = sw->cursor_pixel_x;
            saved_padding = sw->number_padding;
            saved_attrs = sw->curr_tile_attributes;
            saved_font = sw->font;
            have_backup = true;
        }
    }

    /* CREATE_WINDOW(CARRIED_MONEY=0x0A) */
    create_window(0x0A);
    WindowInfo *mw = (win.current_focus_window != WINDOW_ID_NONE)
                     ? get_window(win.current_focus_window) : NULL;
    if (mw) {
        mw->number_padding = 5;
        /* Clear window text */
        mw->text_x = 0;
        mw->text_y = 0;
        mw->cursor_pixel_x = 0;
        /* Clear per-window content tilemap */
        {
            uint16_t cw = mw->width - 2;
            uint16_t itr = mw->height - 2;
            uint16_t total = cw * itr;
            if (total > mw->content_tilemap_size) total = mw->content_tilemap_size;
            for (uint16_t i = 0; i < total; i++) {
                free_tile_safe(mw->content_tilemap[i]);
                mw->content_tilemap[i] = 0;
            }
        }
    }

    /* Assembly (display_money_window.asm:12-17): SET_INSTANT_PRINTING, print, CLEAR */
    set_instant_printing();

    /* Port of PRINT_MONEY_IN_WINDOW (asm/text/window/print_money_in_window.asm).
     * Right-aligns "$N" within the money window using pixel-level positioning. */
    {
        char money_buf[16];
        snprintf(money_buf, sizeof(money_buf), "$%u", (unsigned)game_state.money_carried);

        /* Calculate total pixel width of the money string */
        uint16_t total_pixel_width = 0;
        for (int c = 0; money_buf[c]; c++) {
            uint8_t eb = ascii_to_eb_char(money_buf[c]);
            uint8_t glyph_idx = (eb - 0x50) & 0x7F;
            total_pixel_width += font_get_width(FONT_ID_NORMAL, glyph_idx) + character_padding;
        }
        total_pixel_width += character_padding;  /* Extra trailing padding (asm lines 114-120) */

        /* Right-align: position = (window_width - 1) * 8 - total_pixel_width
         * Assembly lines 130-137: (width-1) << 3, then subtract total width */
        /* Assembly width is display-2 (content width); use (content_w - 1) * 8 */
        uint16_t right_pixel = (uint16_t)((mw->width - 3) * 8);
        uint16_t start_pixel = (right_pixel > total_pixel_width)
                              ? (right_pixel - total_pixel_width) : 0;

        set_text_pixel_position(0, start_pixel);
        print_string(money_buf);
    }

    clear_instant_printing();

    /* Restore text attributes */
    if (saved_id != WINDOW_ID_NONE && have_backup) {
        WindowInfo *rw = get_window(saved_id);
        if (rw) {
            win.current_focus_window = saved_id;
            rw->text_x = saved_text_x;
            rw->text_y = saved_text_y;
            rw->cursor_pixel_x = saved_pixel_x;
            rw->number_padding = saved_padding;
            rw->curr_tile_attributes = saved_attrs;
            rw->font = saved_font;
        }
    }
}

/* Text label constants — generated by ebtools pack-all from dialogue YAML. */
#include "data/text_refs.h"

/* CHECK_PSI_AFFLICTION_BLOCK — Port of asm/text/check_psi_affliction_block.asm (49 lines).
 *
 * Returns 1 if the character CAN use PSI (no blocking afflictions),
 * 0 if an affliction blocks PSI.
 *
 * Checks each of the 7 affliction groups against PSI_BLOCK_BY_AFFLICTION_TABLE
 * (asm/data/unknown/C3F0B0.asm). Blocking afflictions:
 *   Group 0: Unconscious (1), Diamondized (2)
 *   Group 2: Asleep (1), Solidified (4)
 *   Group 4: Can't concentrate (1)
 */
static uint16_t check_psi_affliction_block(uint16_t char_id) {
    /* PSI_BLOCK_BY_AFFLICTION_TABLE: 7 groups × 7 entries.
     * Entry = 1 means that affliction blocks PSI.
     * Indexed as [group * 7 + (affliction_value - 1)].
     * From asm/data/unknown/C3F0B0.asm. */
    static const uint8_t psi_block_table[7 * 7] = {
        /* Group 0 */ 1, 1, 0, 0, 0, 0, 0,  /* Unconscious, Diamondized block */
        /* Group 1 */ 0, 0, 0, 0, 0, 0, 0,
        /* Group 2 */ 1, 0, 0, 1, 0, 0, 0,  /* Asleep, Solidified block */
        /* Group 3 */ 0, 0, 0, 0, 0, 0, 0,
        /* Group 4 */ 1, 0, 0, 0, 0, 0, 0,  /* Can't concentrate blocks */
        /* Group 5 */ 0, 0, 0, 0, 0, 0, 0,
        /* Group 6 */ 0, 0, 0, 0, 0, 0, 0,
    };

    uint16_t char_idx = char_id - 1;
    for (int group = 0; group < AFFLICTION_GROUP_COUNT; group++) {
        uint8_t affliction = party_characters[char_idx].afflictions[group];
        if (affliction == 0) continue;
        /* Assembly returns on the FIRST non-zero affliction group */
        int idx = group * 7 + (affliction - 1);
        if (idx < 0 || idx >= (int)sizeof(psi_block_table))
            FATAL("check_psi_affliction_block: idx=%d out of range (group=%d, affliction=%u)\n",
                  idx, group, affliction);
        if (psi_block_table[idx])
            return 0;  /* blocked */
        return 1;  /* non-zero affliction but not blocking — still return */
    }
    return 1;  /* no afflictions — not blocked */
}

/* CHECK_CHARACTER_PSI_AVAILABILITY — Port of asm/text/check_character_psi_availability.asm (34 lines).
 *
 * Returns 1 if the character (1-based) can use PSI, 0 otherwise.
 * Jeff (char_id 3) never has PSI. Other characters are checked via
 * CHECK_PSI_AFFLICTION_BLOCK and CHECK_CHARACTER_HAS_PSI_ABILITY.
 *
 * The assembly takes 3 params: char_id (A), usability (X), category (Y).
 * COUNT_CHARACTERS_WITH_PSI calls with usability=1 (overworld), category=15 (all). */
static uint16_t check_character_psi_availability(uint16_t char_id) {
    /* Assembly line 18-19: Jeff always returns 0 */
    if (char_id == PARTY_MEMBER_JEFF) return 0;

    /* Assembly line 21: CHECK_PSI_AFFLICTION_BLOCK */
    if (!check_psi_affliction_block(char_id)) return 0;

    /* Assembly line 27: CHECK_CHARACTER_HAS_PSI_ABILITY.
     * COUNT_CHARACTERS_WITH_PSI passes usability=1 (overworld), category=15 (all).
     * This checks if the character has any overworld-usable PSI ability. */
    if (!check_character_has_psi_ability(char_id, PSI_USE_OVERWORLD, 0x0F)) return 0;

    return 1;
}

/* FIND_FIRST_CHARACTER_WITH_PSI — Port of asm/text/find_first_character_with_psi.asm.
 *
 * Iterates party members; returns 1-based index of first member with PSI,
 * or 0 if nobody can use PSI. */
static uint16_t find_first_character_with_psi(void) {
    uint8_t count = game_state.player_controlled_party_count;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t member = game_state.party_members[i];
        if (check_character_psi_availability(member))
            return i + 1;  /* 1-based party index */
    }
    return 0;
}

/* COUNT_CHARACTERS_WITH_PSI — Port of asm/text/count_characters_with_psi.asm.
 *
 * Returns the number of party members who can use PSI. */
static uint16_t count_characters_with_psi(void) {
    uint8_t count = game_state.player_controlled_party_count;
    uint16_t psi_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t member = game_state.party_members[i];
        if (check_character_psi_availability(member))
            psi_count++;
    }
    return psi_count;
}

/* BUILD_COMMAND_MENU — Port of asm/text/menu/build_command_menu.asm.
 *
 * Builds the 6-item command menu (Talk to, Goods, PSI, Equip, Check, Status)
 * in a 2-column × 3-row grid on WINDOW::COMMAND_MENU.
 *
 * PSI is omitted if no party member can use PSI.
 * Sound effect per item: Talk to/Check = 1 (CURSOR1), others = 27 (MENU_OPEN_CLOSE).
 * Goods gets SFX 1 if only 1 party member and they have no items.
 *
 * Positions from DEBUG_MENU_ELEMENT_SPACING_DATA:
 *   Talk to(0,0)  Goods(6,0)
 *   PSI(0,1)      Equip(6,1)
 *   Check(0,2)    Status(6,2)
 */
static uint8_t skip_adding_command_text;
/* restore_menu_backup moved to win.restore_menu_backup (window.h WindowSystemState) */

static void build_command_menu(void) {
    if (skip_adding_command_text) {
        skip_adding_command_text = 0;
        print_menu_items();
        return;
    }

    /* Command labels loaded from ROM (CMD_WINDOW_TEXT, 6 × 10 bytes EB-encoded padded) */
    const uint8_t *cmd_data = ASSET_DATA(ASSET_US_DATA_COMMAND_WINDOW_TEXT_BIN);
    /* Grid positions from DEBUG_MENU_ELEMENT_SPACING_DATA */
    static const uint8_t cmd_x[6] = { 0, 6, 0, 6, 0, 6 };
    static const uint8_t cmd_y[6] = { 0, 0, 1, 1, 2, 2 };

    for (int i = 0; i < 6; i++) {
        uint16_t userdata = i + 1; /* 1=TALK_TO .. 6=STATUS */

        /* PSI (command 3, index 2): skip if no party member has PSI.
         * Port of assembly lines 28-31. */
        if (userdata == 3 && find_first_character_with_psi() == 0)
            continue;

        /* SFX: Talk to(1) and Check(5) get SFX 1 (auto-select),
         * others get SFX 27 (menu open/close). */
        uint8_t sfx = (userdata == 1 || userdata == 5) ? 1 : 27;

        /* Goods (2): SFX 1 if only 1 party member with no items. */
        if (userdata == 2 &&
            (game_state.player_controlled_party_count & 0xFF) == 1) {
            uint8_t member = game_state.party_members[0];
            if (get_character_item(member, 1) == 0)
                sfx = 1;
        }

        /* Decode EB-encoded command label to ASCII for menu item */
        const uint8_t *eb = cmd_data + i * CMD_LABEL_SIZE;
        char ascii_buf[CMD_LABEL_SIZE + 1];
        int len = 0;
        for (int j = 0; j < CMD_LABEL_SIZE && eb[j] != 0; j++)
            ascii_buf[len++] = eb_char_to_ascii(eb[j]);
        ascii_buf[len] = '\0';
        add_menu_item(ascii_buf, userdata, cmd_x[i], cmd_y[i]);

        /* Store sound_effect on the just-added item.
         * Port of ADD_MENU_ITEM_WITH_SOUND (asm/text/menu/add_menu_item_with_sound.asm). */
        WindowInfo *w = get_window(win.current_focus_window);
        if (w && w->menu_count > 0) {
            w->menu_items[w->menu_count - 1].sound_effect = sfx;
        }
    }

    skip_adding_command_text = 0;
    print_menu_items();
}

/* ===================================================================
 * Equipment Menu System
 *
 * Ports of DISPLAY_EQUIPMENT_MENU (C19F29), DISPLAY_CHARACTER_EQUIPMENT_STATS
 * (C1A1D8), SHOW_EQUIPMENT_AND_STATS (C1A778), PREVIEW_*_EQUIP_STATS
 * (C22562, C225AC, C2260D, C22673), and EQUIPMENT_CHANGE_MENU (C1A795).
 * =================================================================== */

/* Equipment menu globals (BSS variables in assembly).
 * COMPARE_EQUIPMENT_MODE: when set, stats window shows current vs preview.
 * CHARACTER_FOR_EQUIP_MENU: 1-based char_id for the preview callbacks.
 * TEMPORARY_*: equipment slot indices for preview calculations. */
static uint8_t compare_equipment_mode;
static uint8_t character_for_equip_menu;
static uint8_t temporary_weapon;
static uint8_t temporary_body_gear;
static uint8_t temporary_arms_gear;
static uint8_t temporary_other_gear;

/* Equip text asset — combined STATUS_EQUIP_WINDOW_TEXT_8 through _13.
 * Offsets within the block:
 *   0: "Offense:" (8 bytes, EB-encoded)
 *   8: "Defense:" (8 bytes)
 *  16: slot display labels, 4 × 11 bytes ("  Weapon", etc.)
 *  60: slot title names, 4 × 8 bytes ("Weapons", "Body", "Arms", "Others")
 *  92: "(Nothing) " (10 bytes)
 * 102: "None" (5 bytes) */
#define ETEXT8_OFF   0
#define ETEXT8_LEN   8
#define ETEXT9_OFF   8
#define ETEXT9_LEN   8
#define ETEXT10_OFF  16
#define ETEXT10_STRIDE 11
#define ETEXT11_OFF  60
#define ETEXT11_STRIDE 8
#define ETEXT12_OFF  92
#define ETEXT12_LEN  10
#define ETEXT13_OFF  102
#define ETEXT13_LEN  5

static const uint8_t *equip_text_data;  /* loaded from status_equip_window_text_8_13.bin */

static void load_equip_text_data(void) {
    if (!equip_text_data)
        equip_text_data = ASSET_DATA(ASSET_DATA_STATUS_EQUIP_WINDOW_TEXT_8_13_BIN);
}

/* Helper: get equipment item strength value from item config.
 * Reads the signed strength byte from item_parameters for the given
 * item at inventory slot `equip_index` (1-based) of character `char_idx` (0-based).
 * If char_idx == 3 (Poo), reads from the second strength byte (+1 offset).
 * Returns 0 if the slot is empty or invalid. */
static int16_t get_equipment_strength(uint16_t char_idx, uint8_t equip_index) {
    if (equip_index == 0) return 0;
    uint8_t item_id = party_characters[char_idx].items[equip_index - 1];
    if (item_id == 0) return 0;

    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry) return 0;

    /* Poo uses the second strength byte (assembly: @LOCAL02 = 1 for Poo) */
    int param_offset = (char_idx == 3) ? 1 : 0;
    uint8_t raw = entry->params[ITEM_PARAM_STRENGTH + param_offset];

    /* Convert unsigned 0-255 to signed: SEC; SBC #$80; EOR #$FF80 (assembly pattern).
     * raw 0x00-0x7F → positive 0..127, raw 0x80-0xFF → negative -128..-1 */
    return (int16_t)(((uint16_t)raw - 0x0080) ^ 0xFF80);
}

/* Clamp a stat value to 0..255 range (assembly pattern: BRANCHLTEQS checks). */
static uint16_t clamp_stat(int16_t val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint16_t)val;
}

/* DISPLAY_EQUIPMENT_MENU — Port of src/inventory/equipment/display_equipment_menu.asm (242 lines).
 *
 * Shows the 4 equipment slots (Weapon/Body/Arms/Other) in WINDOW::EQUIP_MENU
 * with current item names. Each slot is a selectable menu option.
 * char_id: 1-based character ID. */
static void display_equipment_menu(uint16_t char_id) {
    uint16_t char_idx = char_id - 1;
    create_window(WINDOW_EQUIP_MENU);
    window_tick_without_instant_printing();

    /* Multi-party: set pagination window for scroll indicator */
    if ((game_state.player_controlled_party_count & 0xFF) != 1) {
        dt.pagination_window = WINDOW_EQUIP_MENU;
    }

    /* Set window title to character name */
    {
        char name_buf[WINDOW_TITLE_SIZE];
        int j;
        for (j = 0; j < 5 && party_characters[char_idx].name[j]; j++)
            name_buf[j] = eb_char_to_ascii(party_characters[char_idx].name[j]);
        name_buf[j] = '\0';
        set_window_title(WINDOW_EQUIP_MENU, name_buf, 6);
    }

    load_equip_text_data();

    /* For each equipment slot (0-3): add menu option with slot label,
     * then print ": <item name>" after it. */
    uint16_t line = 0;
    for (uint16_t slot = 0; slot < EQUIP_COUNT; slot++) {
        dt.force_left_text_alignment = 1;

        /* Add slot label ("  Weapon", "      Body", etc.) as menu option.
         * Assembly: ADD_POSITIONED_MENU_OPTION with text_x=0, text_y=slot. */
        char slot_label[16];
        if (equip_text_data) {
            eb_to_ascii_buf(equip_text_data + ETEXT10_OFF + slot * ETEXT10_STRIDE,
                            ETEXT10_STRIDE, slot_label);
        } else {
            static const char *fallback[] EB_NORELOC = { "  Weapon", "      Body", "     Arms", "     Other" };
            snprintf(slot_label, sizeof(slot_label), "%s", fallback[slot]);
        }
        /* add_menu_item with explicit position: column 0, row = slot.
         * Userdata = slot+1 (1=Weapon..4=Other), matching assembly's
         * counted-mode selection_menu return. */
        add_menu_item(slot_label, slot + 1, 0, slot);

        /* Read equipment slot index for this character.
         * equipment[WEAPON..OTHER] is a 1-based inventory slot index (0 = nothing). */
        uint8_t equip_slot_idx = party_characters[char_idx].equipment[slot];

        /* Build item name string */
        char item_buf[32];
        if (equip_slot_idx == 0) {
            /* No item equipped — show "(Nothing) " from TEXT_12 */
            if (equip_text_data) {
                eb_to_ascii_buf(equip_text_data + ETEXT12_OFF, ETEXT12_LEN, item_buf);
            } else {
                snprintf(item_buf, sizeof(item_buf), "(Nothing)");
            }
        } else {
            /* Read item at this inventory slot */
            uint8_t item_id = party_characters[char_idx].items[equip_slot_idx - 1];
            const ItemConfig *item_entry = get_item_entry(item_id);
            int offset = 0;

            /* Check if equipped — if so, prepend equip marker (assembly: CHAR::EQUIPPED) */
            if (check_item_equipped(char_id, equip_slot_idx)) {
                item_buf[0] = EB_CHAR_EQUIPPED;
                offset = 1;
            }

            /* Copy item name (EB → ASCII) */
            if (item_entry) {
                for (int j = 0; j < ITEM_NAME_LEN && (offset + j) < (int)sizeof(item_buf) - 1; j++)
                    item_buf[offset + j] = eb_char_to_ascii(item_entry->name[j]);
            }
            item_buf[offset + ITEM_NAME_LEN] = '\0';
        }

        /* Print ": <item>" at column 6, current line.
         * Assembly: SET_FOCUS_TEXT_CURSOR(X=line, A=6), PRINT_LETTER ':',
         * PRINT_LETTER ' ', PRINT_STRING(ert.buffer, 49). */
        set_focus_text_cursor(6, line);
        print_string(": ");
        print_string(item_buf);
        line++;
    }

    print_menu_items();
    dt.force_left_text_alignment = 0;
    clear_instant_printing();
}

/* DISPLAY_CHARACTER_EQUIPMENT_STATS — Port of src/inventory/equipment/display_character_equipment_stats.asm (620 lines).
 *
 * Shows Offense and Defense values in WINDOW::EQUIPMENT_STATS.
 * Uses base_offense/defense + equipment strength bonuses.
 * When compare_equipment_mode is set, also shows a preview column
 * with TEMPORARY_* equipment stats.
 *
 * char_id: 1-based character ID. */
static void display_character_equipment_stats(uint16_t char_id) {
    uint16_t char_idx = char_id - 1;
    create_window(WINDOW_EQUIPMENT_STATS);
    window_tick_without_instant_printing();
    set_window_number_padding(2);

    load_equip_text_data();

    /* --- Row 0: Offense --- */
    set_focus_text_cursor(0, 0);
    {
        char label[12];
        if (equip_text_data) {
            eb_to_ascii_buf(equip_text_data + ETEXT8_OFF, ETEXT8_LEN, label);
        } else {
            snprintf(label, sizeof(label), "Offense:");
        }
        print_string(label);
    }

    /* Compute current offense: base_offense + weapon strength */
    int16_t offense = party_characters[char_idx].base_offense;
    uint8_t weapon_idx = party_characters[char_idx].equipment[EQUIP_WEAPON];
    offense += get_equipment_strength(char_idx, weapon_idx);

    /* Print offense value at pixel position 55 */
    dt.force_left_text_alignment = 1;
    set_text_pixel_position(0, 55);
    print_number(clamp_stat(offense), 0);
    dt.force_left_text_alignment = 0;

    /* --- Row 1: Defense --- */
    set_focus_text_cursor(0, 1);
    {
        char label[12];
        if (equip_text_data) {
            eb_to_ascii_buf(equip_text_data + ETEXT9_OFF, ETEXT9_LEN, label);
        } else {
            snprintf(label, sizeof(label), "Defense:");
        }
        print_string(label);
    }

    /* Compute current defense: base_defense + body + arms + other strength */
    int16_t defense = party_characters[char_idx].base_defense;
    defense += get_equipment_strength(char_idx,
                    party_characters[char_idx].equipment[EQUIP_BODY]);
    defense += get_equipment_strength(char_idx,
                    party_characters[char_idx].equipment[EQUIP_ARMS]);
    defense += get_equipment_strength(char_idx,
                    party_characters[char_idx].equipment[EQUIP_OTHER]);

    /* Print defense value at pixel position 55 */
    dt.force_left_text_alignment = 1;
    set_text_pixel_position(1, 55);
    print_number(clamp_stat(defense), 0);
    dt.force_left_text_alignment = 0;

    /* --- Preview mode: show "▶ offense  ▶ defense" with temporary equipment --- */
    if (compare_equipment_mode) {
        /* Print arrow separator at row 0, pixel 76 */
        set_text_pixel_position(0, 76);
        set_window_palette_index(1);
        print_char_with_sound(0x014E);  /* right arrow tile */
        set_window_palette_index(0);

        /* Compute preview offense: base + temporary weapon */
        int16_t preview_off = party_characters[char_idx].base_offense;
        preview_off += get_equipment_strength(char_idx, temporary_weapon);

        dt.force_left_text_alignment = 1;
        print_number(clamp_stat(preview_off), 0);
        dt.force_left_text_alignment = 0;

        /* Print arrow separator at row 1, pixel 76 */
        set_text_pixel_position(1, 76);
        set_window_palette_index(1);
        print_char_with_sound(0x014E);

        /* Compute preview defense: base + temporary body/arms/other */
        int16_t preview_def = party_characters[char_idx].base_defense;
        preview_def += get_equipment_strength(char_idx, temporary_body_gear);
        preview_def += get_equipment_strength(char_idx, temporary_arms_gear);
        preview_def += get_equipment_strength(char_idx, temporary_other_gear);

        dt.force_left_text_alignment = 1;
        print_number(clamp_stat(preview_def), 0);
        dt.force_left_text_alignment = 0;
    }

    clear_instant_printing();
}

/* SHOW_EQUIPMENT_AND_STATS — Port of src/inventory/equipment/show_equipment_and_stats.asm (17 lines).
 * Wrapper: sets compare_equipment_mode=0, calls display_equipment_menu
 * and display_character_equipment_stats. char_id: 1-based. */
static void show_equipment_and_stats(uint16_t char_id) {
    compare_equipment_mode = 0;
    display_equipment_menu(char_id);
    display_character_equipment_stats(char_id);
}

/* Cursor move callback for show_equipment_and_stats.
 * Used during character selection — updates equipment display as cursor moves.
 * Assembly passes SHOW_EQUIPMENT_AND_STATS pointer as the callback. */
static void show_equipment_and_stats_callback(uint16_t char_id) {
    if (char_id == 0) return;
    show_equipment_and_stats(char_id);
}

/* GET_WEAPON_ITEM_NAME callback (src/inventory/get_weapon_item_name.asm).
 * On_change callback for Goods menu char_select_prompt.
 * Shows the selected character's inventory in WINDOW_INVENTORY. */
static void get_weapon_item_name_callback(uint16_t char_id) {
    if (char_id == 0) return;
    inventory_get_item_name(char_id, WINDOW_INVENTORY);
}

/* GET_BODY_ITEM_NAME callback (src/inventory/get_body_item_name.asm).
 * On_change callback for Give menu char_select_prompt.
 * Shows the selected character's inventory in WINDOW_OVERWORLD_CHAR_SELECT. */
static void get_body_item_name_callback(uint16_t char_id) {
    if (char_id == 0) return;
    inventory_get_item_name(char_id, WINDOW_OVERWORLD_CHAR_SELECT);
}

/* ---------------------------------------------------------------------------
 * GAME_MODE_CHAR_SELECT callback dispatch.
 *
 * char_select_prompt() still takes raw function pointers from its callers, but
 * the run-to-completion step (mode_step_char_select) needs to invoke them from a
 * serializable ModeState that cannot hold pointers. These helpers map between
 * the known callback function pointers and stable IDs, and invoke a callback by
 * ID. All char-select callbacks (battle-style path) are enumerated here; mode 1
 * callers pass NULL and never reach this mode. Defined in text.c because four of
 * the six callbacks are static to this file. See docs/plans/savestate-unified-loop.md.
 * ------------------------------------------------------------------------- */
uint8_t cs_onchange_id(void (*fn)(uint16_t)) {
    if (fn == NULL)                              return CS_ONCHANGE_NONE;
    if (fn == show_equipment_and_stats_callback) return CS_ONCHANGE_EQUIPMENT;
    if (fn == display_character_psi_list)        return CS_ONCHANGE_PSI_LIST;
    if (fn == display_status_window)             return CS_ONCHANGE_STATUS;
    if (fn == get_weapon_item_name_callback)     return CS_ONCHANGE_WEAPON_NAME;
    if (fn == get_body_item_name_callback)       return CS_ONCHANGE_BODY_NAME;
    LOG_WARN("char_select: unknown on_change callback, ignoring");
    return CS_ONCHANGE_NONE;
}

uint8_t cs_checkvalid_id(uint16_t (*fn)(uint16_t)) {
    if (fn == NULL)                            return CS_CHECKVALID_NONE;
    if (fn == check_character_psi_availability) return CS_CHECKVALID_PSI;
    LOG_WARN("char_select: unknown check_valid callback, treating as none");
    return CS_CHECKVALID_NONE;
}

bool cs_invoke_on_change(uint8_t id, uint16_t char_id, union ModeState *out_init) {
    switch (id) {
    case CS_ONCHANGE_EQUIPMENT:   show_equipment_and_stats_callback(char_id); return false;
    case CS_ONCHANGE_PSI_LIST:    display_character_psi_list(char_id);        return false;
    case CS_ONCHANGE_STATUS:
        /* display_status_window prints label strings via display_text(), which
         * pump_modes GAME_MODE_DISPLAY_TEXT internally. That text is instant-printed
         * (no typewriter yield) so the nested pump completes within one frame and is
         * never a savestate point; converting it to a STEP_PUSH (splitting the
         * window's before/after work) is deferred until its blocking parent — the
         * Status pause-menu (Phase C) — is converted. */
        display_status_window(char_id);
        return false;
    case CS_ONCHANGE_WEAPON_NAME: get_weapon_item_name_callback(char_id);     return false;
    case CS_ONCHANGE_BODY_NAME:   get_body_item_name_callback(char_id);       return false;
    case CS_ONCHANGE_PARTY_SELECT_SCRIPT:
        /* party_character_selector battle path: display the selected member's
         * text script (1-based char_id; KING / id 0 have no script). This script
         * uses the typewriter (it yields), so request a STEP_PUSH of a nested
         * GAME_MODE_DISPLAY_TEXT child instead of recursing on the C stack. */
        if (char_id >= 1 && char_id <= 4) {
            uint32_t script_addr = dt.party_member_selection_scripts[char_id - 1];
            if (script_addr != 0 && dt_make_child_init((ModeState *)out_init, script_addr))
                return true;
        }
        return false;
    case CS_ONCHANGE_NONE:
    default:                                                                  return false;
    }
}

uint16_t cs_invoke_check_valid(uint8_t id, uint16_t char_id) {
    switch (id) {
    case CS_CHECKVALID_PSI: return check_character_psi_availability(char_id);
    case CS_CHECKVALID_NONE:
    default:                return 1;  /* no callback → all characters valid */
    }
}

/* PREVIEW_WEAPON_EQUIP_STATS — Port of asm/battle/preview_weapon_equip_stats.asm (35 lines).
 * Cursor callback for weapon equipment list: previews offense/defense
 * as if the cursor item were equipped as weapon.
 * item_userdata: 1-based inventory slot, 0xFFFF for "None", 0 = skip. */
static void preview_weapon_equip_stats(uint16_t item_userdata) {
    uint16_t idx = (item_userdata == 0xFFFF) ? 0 : item_userdata;
    temporary_weapon = (uint8_t)idx;

    uint8_t char_id = character_for_equip_menu;
    uint16_t char_idx = char_id - 1;
    temporary_body_gear = party_characters[char_idx].equipment[EQUIP_BODY];
    temporary_arms_gear = party_characters[char_idx].equipment[EQUIP_ARMS];
    temporary_other_gear = party_characters[char_idx].equipment[EQUIP_OTHER];

    display_character_equipment_stats(char_id);
}

/* PREVIEW_BODY_EQUIP_STATS — Port of asm/battle/preview_body_equip_stats.asm (45 lines). */
static void preview_body_equip_stats(uint16_t item_userdata) {
    uint8_t char_id = character_for_equip_menu;
    uint16_t char_idx = char_id - 1;
    temporary_weapon = party_characters[char_idx].equipment[EQUIP_WEAPON];

    uint16_t idx = (item_userdata == 0xFFFF) ? 0 : item_userdata;
    temporary_body_gear = (uint8_t)idx;

    temporary_arms_gear = party_characters[char_idx].equipment[EQUIP_ARMS];
    temporary_other_gear = party_characters[char_idx].equipment[EQUIP_OTHER];

    display_character_equipment_stats(char_id);
}

/* PREVIEW_ARMS_EQUIP_STATS — Port of asm/battle/preview_arms_equip_stats.asm (51 lines). */
static void preview_arms_equip_stats(uint16_t item_userdata) {
    uint8_t char_id = character_for_equip_menu;
    uint16_t char_idx = char_id - 1;
    temporary_weapon = party_characters[char_idx].equipment[EQUIP_WEAPON];
    temporary_body_gear = party_characters[char_idx].equipment[EQUIP_BODY];

    uint16_t idx = (item_userdata == 0xFFFF) ? 0 : item_userdata;
    temporary_arms_gear = (uint8_t)idx;

    temporary_other_gear = party_characters[char_idx].equipment[EQUIP_OTHER];

    display_character_equipment_stats(char_id);
}

/* PREVIEW_OTHER_EQUIP_STATS — Port of asm/battle/preview_other_equip_stats.asm (39 lines). */
static void preview_other_equip_stats(uint16_t item_userdata) {
    uint8_t char_id = character_for_equip_menu;
    uint16_t char_idx = char_id - 1;
    temporary_weapon = party_characters[char_idx].equipment[EQUIP_WEAPON];
    temporary_body_gear = party_characters[char_idx].equipment[EQUIP_BODY];
    temporary_arms_gear = party_characters[char_idx].equipment[EQUIP_ARMS];

    uint16_t idx = (item_userdata == 0xFFFF) ? 0 : item_userdata;
    temporary_other_gear = (uint8_t)idx;

    display_character_equipment_stats(char_id);
}

/* Callback array indexed by slot type (1-4). */
static void (*equip_preview_callbacks[4])(uint16_t) = {
    preview_weapon_equip_stats,
    preview_body_equip_stats,
    preview_arms_equip_stats,
    preview_other_equip_stats,
};

/* EQUIPMENT_CHANGE_MENU (src/inventory/equipment/equipment_change_menu.asm,
 * 252 lines) is folded into GAME_MODE_EQUIP_MENU (mode_step_equip_menu, with
 * the pause-menu machinery further down this file) as the EQ_SLOT /
 * EQ_SLOT_RESULT / EQ_ITEM_RESULT phases. */

/* OPEN_EQUIPMENT_MENU — Port of src/inventory/equipment/open_equipment_menu.asm (66 lines).
 *
 * Equipment menu outer loop. Single party → auto-select character.
 * Multi-party → character selection with SHOW_EQUIPMENT_AND_STATS callback.
 * Then calls EQUIPMENT_CHANGE_MENU for the selected character.
 *
 * Depends on SHOW_EQUIPMENT_AND_STATS (17 lines → DISPLAY_EQUIPMENT_MENU +
 * DISPLAY_CHARACTER_EQUIPMENT_STATS) and EQUIPMENT_CHANGE_MENU (252 lines).
 */
/* OPEN_EQUIPMENT_MENU is now GAME_MODE_EQUIP_MENU (mode_step_equip_menu, with
 * the pause-menu machinery further down this file), STEP_PUSHed by the pause
 * menu's Equip case. */

/* ---- Overworld PSI globals ---- */

/* OVERWORLD_SELECTED_PSI_USER: tracks which character is using PSI in
 * the overworld PSI menu (1-based party member ID). */
static uint16_t overworld_selected_psi_user;

/* ONLY_ONE_CHARACTER_WITH_PSI: flag set when only one party member has PSI,
 * so character selection is skipped. */
static uint16_t only_one_character_with_psi;

/* DISPLAY_PSI_ABILITY_DETAILS — Port of asm/text/menu/display_psi_ability_details.asm (54 lines).
 *
 * Called during PSI ability selection to refresh the ability list window.
 * Clears the window, re-generates the PSI list, restores the text cursor,
 * then highlights the selected PSI's base name with the given palette.
 *
 * palette_index: palette to use for highlighting (0=normal, 6=selected).
 * ability_id: index into PSI_ABILITY_TABLE.
 */
static void display_psi_ability_details(uint16_t palette_index, uint16_t ability_id) {
    set_instant_printing();

    /* Save current focus window's text_y (assembly lines 12-27) */
    WindowInfo *w = get_window(win.current_focus_window);
    uint16_t saved_text_y = w ? w->text_y : 0;

    /* Clear and redraw PSI list (assembly lines 28-33) */
    clear_window_tilemap(win.current_focus_window);
    window_tick_without_instant_printing();
    display_character_psi_list(overworld_selected_psi_user);
    print_menu_items();

    /* Restore text_y (assembly lines 34-37) */
    if (w) w->text_y = saved_text_y;

    /* Position cursor at (0, saved_text_y) (assembly lines 38-41) */
    set_focus_text_cursor(0, saved_text_y);

    /* Highlight selected PSI base name (assembly lines 42-51) */
    set_window_palette_index(palette_index);
    if (ensure_battle_psi_table()) {
        uint8_t psi_name_id = battle_psi_table[ability_id].name;
        print_psi_name(psi_name_id);
    }
    set_window_palette_index(0);

    clear_instant_printing();
}

/* OVERWORLD_PSI_MENU (asm/text/menu/overworld_psi_menu.asm, 571 lines) is now
 * GAME_MODE_PSI_MENU (mode_step_psi_menu, with the pause-menu machinery further
 * down this file), STEP_PUSHed by the pause menu's PSI case. */

/* PSI category names — structural labels matching asm/data/psi_categories.asm (US).
 * These are 8-byte PADDEDEBTEXT in ROM; we use ASCII equivalents here. */
static const char *status_psi_category_names[4] EB_NORELOC = {
    "Offense", "Recover", "Assist", "Other"
};

/* OPEN_STATUS_MENU is now GAME_MODE_STATUS_MENU (mode_step_status_menu, with
 * the pause-menu machinery further down this file), STEP_PUSHed by the pause
 * menu's Status case. */

/* GET_SECTOR_ITEM_TYPE: Port of src/inventory/get_sector_item_type.asm (29 lines).
 * Returns the item type that matches this map sector (for use-context checking).
 * If FLG_WIN_GIEGU is set and sector low bits are 0, returns ITEM_BICYCLE
 * (after defeating Giygas, bicycles work everywhere outdoors).
 * Otherwise returns the high byte of sector attributes (rideable item type). */
static uint16_t get_sector_item_type(void) {
    uint16_t attrs = load_sector_attrs(
        game_state.leader_x_coord, game_state.leader_y_coord);
    /* Assembly lines 13-21: if defeated Giygas and no special sector mode,
     * allow bicycle everywhere */
    if (event_flag_get(EVENT_FLAG_WIN_GIEGU) && (attrs & 0x0007) == 0) {
        return ITEM_BICYCLE;
    }
    /* Assembly lines 23-27: return high byte = sector's rideable item type */
    return (attrs >> 8) & 0xFF;
}

/* GET_COLLISION_AT_LEADER: Port of asm/overworld/collision/get_collision_at_leader.asm (9 lines).
 * Returns collision flags (bits 6-7) at the leader's position.
 * Used to prevent mounting a bicycle on a blocked tile. */
static uint16_t get_collision_at_leader(void) {
    return lookup_surface_flags(
        game_state.leader_x_coord,
        game_state.leader_y_coord,
        0x000C
    ) & 0x00C0;
}

/* GET_NEARBY_NPC_CONFIG_TYPE: Port of asm/text/get_nearby_npc_config_type.asm (26 lines).
 * Calls find_nearby_checkable_tpt_entry() to locate a nearby NPC, then
 * returns its config type (0=none, 1=PERSON, 2=ITEM_BOX, 3=OBJECT).
 * Sets ow.interacting_npc_id as a side effect. */
static uint8_t get_nearby_npc_config_type(void) {
    find_nearby_checkable_tpt_entry();
    /* Assembly lines 7-14: 0, -1, -2 all mean "no NPC" */
    if (ow.interacting_npc_id == 0 ||
        ow.interacting_npc_id == 0xFFFF ||
        ow.interacting_npc_id == 0xFFFE) {
        return 0;
    }
    /* Assembly lines 19-24: look up NPC config type (byte at offset 0) */
    return get_npc_config_type(ow.interacting_npc_id);
}

/* OVERWORLD_USE_ITEM (asm/overworld/use_item.asm, 545 lines) is now
 * GAME_MODE_USE_ITEM (mode_step_use_item, with the pause-menu machinery
 * further down this file), STEP_PUSHed by the pause menu's Goods→Use case. */

/* ---------------------------------------------------------------------------
 * GAME_MODE_PAUSE_MENU — run-to-completion port of open_menu_button()
 * (asm/overworld/open_menu.asm, 614 lines): the full pause menu — Talk to,
 * Goods (Use/Give/Drop/Help cascade), PSI, Equip, Check, Status. Called when A
 * is pressed in the overworld, or A/L from the HPPP display.
 *
 * The former goto-heavy for(;;) is a phase machine in the file_menu idiom: each
 * sub-menu builds its window synchronously, STEP_PUSHes SELECTION_MENU /
 * CHAR_SELECT / DISPLAY_TEXT, and reads the choice back via mode_child_result()
 * in the matching *_RESULT phase. The internal for(;;) chains no-yield
 * transitions, matching the blocking original's zero-yield gaps.
 *
 * All four former blocking sub-drivers — PSI, Equip, Status, and Goods→Use —
 * are now their own modes (GAME_MODE_PSI_MENU / EQUIP_MENU / STATUS_MENU /
 * USE_ITEM), STEP_PUSHed from here with their tails in the PM_*_RESUME
 * phases. See PauseMenuState in mode_stack.h.
 * ------------------------------------------------------------------------- */

/* Initial state for a pushed child. Must outlive the dispatch turn
 * (STEP_RESULT_PUSH_INIT copies it immediately); a file-static is fine since
 * only one child push is ever pending at a time. */
static ModeState pm_child_init;

/* Push GAME_MODE_SELECTION_MENU as a child after the focus window's menu has
 * been laid out. Replicates selection_menu()'s early-exit (no focus window /
 * empty menu → result 0 with no push), delivered inline via *result_ready so
 * the caller's *_RESULT phase handles both forms uniformly. */
static StepResult menu_push_selection(uint8_t *result_ready, uint16_t *result,
                                      uint16_t allow_cancel) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w || w->menu_count == 0) {
        *result_ready = 1;
        *result = 0;
        return STEP_RESULT_CONTINUE();
    }
    *result_ready = 0;
    pm_child_init = (ModeState){0};
    pm_child_init.selection_menu.phase        = SM_SETUP;
    pm_child_init.selection_menu.allow_cancel = (uint8_t)allow_cancel;
    return STEP_RESULT_PUSH_INIT(GAME_MODE_SELECTION_MENU, &pm_child_init);
}

/* Read the result of the menu a *_RESULT phase is processing: the popped
 * child's value, or the inline early-exit value if no child was pushed. */
static uint16_t menu_take_result(uint8_t *result_ready, uint16_t *result) {
    uint16_t r = *result_ready ? *result : (uint16_t)mode_child_result();
    *result_ready = 0;
    return r;
}

static StepResult pm_push_selection(PauseMenuState *st, uint8_t result_phase,
                                    uint16_t allow_cancel) {
    st->phase = result_phase;
    return menu_push_selection(&st->result_ready, &st->result, allow_cancel);
}

static uint16_t pm_take_result(PauseMenuState *st) {
    return menu_take_result(&st->result_ready, &st->result);
}

/* Status-menu variant (both its menus allow cancel). */
static StepResult pm_push_selection_status(StatusMenuState *st,
                                           uint8_t result_phase) {
    st->phase = result_phase;
    return menu_push_selection(&st->result_ready, &st->result, 1);
}

/* Equip-menu variant (both its menus allow cancel). */
static StepResult pm_push_selection_equip(EquipMenuState *st,
                                          uint8_t result_phase) {
    st->phase = result_phase;
    return menu_push_selection(&st->result_ready, &st->result, 1);
}

/* PSI-menu variant (the ability menu allows cancel). */
static StepResult pm_push_selection_psi(PsiMenuState *st,
                                        uint8_t result_phase) {
    st->phase = result_phase;
    return menu_push_selection(&st->result_ready, &st->menu_result, 1);
}

/* Push a DISPLAY_TEXT child for `addr`. If the address can't be resolved,
 * warn (like display_text_from_addr) and don't push — the caller has already
 * set its resume phase, so its for(;;) falls through there inline. */
static StepResult menu_push_text(uint32_t addr, bool *pushed) {
    if (dt_make_child_init(&pm_child_init, addr)) {
        *pushed = true;
        return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &pm_child_init);
    }
    LOG_WARN("WARNING: resolve_text_addr(0x%06X) returned NULL\n", addr);
    *pushed = false;
    return STEP_RESULT_CONTINUE();
}

static StepResult pm_push_text(PauseMenuState *st, uint8_t resume_phase,
                               uint32_t addr, bool *pushed) {
    st->phase = resume_phase;
    return menu_push_text(addr, pushed);
}

/* GAME_MODE_EQUIP_MENU step — run-to-completion port of open_equipment_menu()
 * (src/inventory/equipment/open_equipment_menu.asm, 66 lines). See
 * EquipMenuState in mode_stack.h. equipment_change_menu() stays blocking,
 * called inline (its own selection-menu cascade — a later parent conversion). */
StepResult mode_step_equip_menu(ModeState *ms) {
    EquipMenuState *st = &ms->equip_menu;

    for (;;) {
        switch ((EquipMenuPhase)st->phase) {

        case EQ_ENTER:
            save_window_text_attributes();
            /* Single party: show equipment display before entering the loop.
             * Assembly lines 21-23: if party_count==1, SHOW_EQUIPMENT_AND_STATS
             * first, then fall through to char selection which auto-selects. */
            if ((game_state.player_controlled_party_count & 0xFF) == 1)
                show_equipment_and_stats(game_state.party_members[0]);
            st->phase = EQ_SELECT;
            continue;

        case EQ_SELECT:
            /* Multi-party: character selection with "Who?" prompt.
             * SHOW_EQUIPMENT_AND_STATS is the on_change callback so the
             * equipment display updates as the player scrolls characters. */
            if ((game_state.player_controlled_party_count & 0xFF) != 1) {
                display_menu_header_text(0);  /* "Who?" */
                char_select_make_init(&pm_child_init, 0, 1,
                                      CS_ONCHANGE_EQUIPMENT, CS_CHECKVALID_NONE);
                st->phase = EQ_SELECT_RESULT;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_CHAR_SELECT, &pm_child_init);
            }
            /* Single party → auto-select, highlight in HPPP */
            st->equip_char = game_state.party_members[0];
            select_battle_menu_character(0);
            st->phase = EQ_CHANGE;
            continue;

        case EQ_SELECT_RESULT:
            st->equip_char = (uint16_t)mode_child_result();
            close_menu_header_window();
            if (st->equip_char == 0) {
                st->phase = EQ_EXIT;  /* cancelled → exit */
                continue;
            }
            st->phase = EQ_CHANGE;
            continue;

        case EQ_CHANGE:
            /* EQUIPMENT_CHANGE_MENU entry (equipment_change_menu.asm). */
            load_equip_text_data();
            st->phase = EQ_SLOT;
            continue;

        case EQ_SLOT:
            /* "Where?" header for slot selection (assembly lines 18-27).
             * The 4 slot options were added by display_equipment_menu with
             * userdata 1=Weapon..4=Other; 0 = cancel. */
            display_menu_header_text(4);  /* "Where?" */
            set_window_focus(WINDOW_EQUIP_MENU);
            return pm_push_selection_equip(st, EQ_SLOT_RESULT);

        case EQ_SLOT_RESULT: {
            st->slot_type = menu_take_result(&st->result_ready, &st->result);
            close_menu_header_window();

            if (st->slot_type == 0) {
                /* Cancelled → back to character selection (the former
                 * equipment_change_menu return + open_equipment_menu's
                 * loop/exit branch, assembly lines 52-55). */
                if ((game_state.player_controlled_party_count & 0xFF) != 1) {
                    st->phase = EQ_SELECT;
                    continue;
                }
                st->phase = EQ_EXIT;  /* single party → exit */
                continue;
            }

            uint16_t char_id = st->equip_char;
            uint16_t char_idx = char_id - 1;
            uint16_t slot_type = st->slot_type;

            /* Create the item list window for this equipment slot */
            create_window(WINDOW_EQUIP_MENU_ITEMLIST);

            /* Set window title from TEXT_11: "Weapons", "Body", "Arms", "Others" */
            if (equip_text_data) {
                char title_buf[WINDOW_TITLE_SIZE];
                const uint8_t *title_src = equip_text_data + ETEXT11_OFF
                                           + (slot_type - 1) * ETEXT11_STRIDE;
                eb_to_ascii_buf(title_src, ETEXT11_STRIDE, title_buf);
                /* STRLEN equivalent: length excluding trailing spaces/nulls */
                int title_len = (int)strlen(title_buf);
                set_window_title(WINDOW_EQUIP_MENU_ITEMLIST, title_buf, title_len);
            }

            /* Build equippable item list. Loop over inventory (14 slots),
             * add items that are equippable and match the slot type.
             * Assembly lines 48-145. */
            uint16_t item_count = 0;
            uint16_t currently_equipped_pos = (uint16_t)-1;

            for (uint16_t inv_slot = 0; inv_slot < ITEM_INVENTORY_SIZE; inv_slot++) {
                uint8_t item_id = party_characters[char_idx].items[inv_slot];
                if (item_id == 0) continue;

                /* Item must be equippable (type == 2) */
                if (get_item_type(item_id) != 2) continue;

                /* Item must match the selected slot type */
                if (get_item_subtype(item_id) != slot_type) continue;

                /* Item must be usable by this character */
                if (!check_item_usable_by(char_id, item_id)) continue;

                /* Build label: equipped marker + item name */
                char label[MENU_LABEL_SIZE];
                int offset = 0;

                /* Track if this is the currently equipped item */
                if (check_item_equipped(char_id, inv_slot + 1)) {
                    label[0] = EB_CHAR_EQUIPPED;
                    offset = 1;
                    currently_equipped_pos = item_count;
                }

                const ItemConfig *item_entry = get_item_entry(item_id);
                if (item_entry) {
                    for (int j = 0; j < ITEM_NAME_LEN && (offset + j) < MENU_LABEL_SIZE - 1; j++)
                        label[offset + j] = eb_char_to_ascii(item_entry->name[j]);
                }
                /* Assembly: STZ TEMPORARY_TEXT_BUFFER+.SIZEOF(item::name)
                 * Null terminator is at a fixed offset regardless of the marker. */
                label[ITEM_NAME_LEN] = '\0';

                /* Trim trailing spaces */
                int len = (int)strlen(label);
                while (len > 0 && label[len - 1] == ' ')
                    label[--len] = '\0';

                /* Assembly lines 137-144: ADD_MENU_ITEM_NO_POSITION with
                 * userdata = inv_slot+1 (1-based), sound_effect = 115 */
                add_menu_item_no_position(label, inv_slot + 1);

                WindowInfo *w = get_window(win.current_focus_window);
                if (w && w->menu_count > 0) {
                    w->menu_items[w->menu_count - 1].sound_effect = 115;
                }

                item_count++;
            }

            /* Add "None" option for unequipping (assembly lines 152-155).
             * Userdata = -1 (0xFFFF) to signal unequip. */
            {
                char none_label[8];
                if (equip_text_data) {
                    eb_to_ascii_buf(equip_text_data + ETEXT13_OFF, ETEXT13_LEN, none_label);
                } else {
                    snprintf(none_label, sizeof(none_label), "None");
                }
                add_menu_item_no_position(none_label, (uint16_t)-1);
            }

            /* Layout items with initial selection on the equipped item.
             * Assembly lines 156-159: LAYOUT_AND_PRINT_MENU_AT_SELECTION. */
            layout_and_print_menu_at_selection(1, 0, currently_equipped_pos);

            /* Set cursor move callback for stat preview (assembly 160-190).
             * Lives in the re-fetchable WindowInfo — safe across the push. */
            character_for_equip_menu = char_id;
            if (slot_type >= 1 && slot_type <= 4) {
                set_cursor_move_callback(equip_preview_callbacks[slot_type - 1]);
            }

            /* Show header "Which?" and run the item selection */
            compare_equipment_mode = 1;
            display_menu_header_text(1);  /* "Which?" */
            return pm_push_selection_equip(st, EQ_ITEM_RESULT);
        }

        case EQ_ITEM_RESULT: {
            uint16_t item_selection = menu_take_result(&st->result_ready,
                                                       &st->result);
            close_menu_header_window();
            clear_cursor_move_callback();

            /* Process selection */
            if (item_selection == (uint16_t)-1) {
                /* "None" selected → unequip current slot.
                 * Assembly lines 203-236: CHANGE_EQUIPPED_*(char_id, 0). */
                switch (st->slot_type) {
                case 1: change_equipped_weapon(st->equip_char, 0); break;
                case 2: change_equipped_body(st->equip_char, 0); break;
                case 3: change_equipped_arms(st->equip_char, 0); break;
                case 4: change_equipped_other(st->equip_char, 0); break;
                }
            } else if (item_selection != 0) {
                /* Item selected → equip it (assembly lines 240-242). */
                equip_item(st->equip_char, item_selection);
            }
            /* item_selection == 0 → cancelled, just close and loop */

            /* Close item list, refresh equipment display, loop to slot
             * selection (assembly lines 243-249). */
            close_window(WINDOW_EQUIP_MENU_ITEMLIST);
            show_equipment_and_stats(st->equip_char);
            st->phase = EQ_SLOT;
            continue;
        }

        case EQ_EXIT:
        default:
            close_window(WINDOW_EQUIPMENT_STATS);
            close_window(WINDOW_EQUIP_MENU);
            restore_window_text_attributes();
            return STEP_RESULT_POP(0);
        }
    }
}

/* GAME_MODE_STATUS_MENU step — run-to-completion port of open_status_menu()
 * (asm/text/menu/open_status_menu.asm, 118 lines). See StatusMenuState in
 * mode_stack.h. The status-window on_change (CS_ONCHANGE_STATUS) is instant-
 * printed and never yields; the PSI list/description cursor callbacks live in
 * the re-fetchable WindowInfo, exactly as when the blocking selection_menu()
 * pumped the same step function. */
StepResult mode_step_status_menu(ModeState *ms) {
    StatusMenuState *st = &ms->status_menu;

    for (;;) {
        switch ((StatusMenuPhase)st->phase) {

        case SU_SELECT:
            /* Assembly line 12 (and the loop-bottom @RESET_ALIGNMENT): left
             * alignment on for character selection / the status window. */
            dt.force_left_text_alignment = 1;

            /* Assembly lines 13-19: char select with DISPLAY_STATUS_WINDOW as
             * the on_change callback (mode=0, allow_cancel=1). */
            char_select_make_init(&pm_child_init, 0, 1,
                                  CS_ONCHANGE_STATUS, CS_CHECKVALID_NONE);
            st->phase = SU_SELECT_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_CHAR_SELECT, &pm_child_init);

        case SU_SELECT_RESULT: {
            uint16_t status_char = (uint16_t)mode_child_result();

            /* Assembly line 21: BEQL @EXIT (0 = cancelled) */
            if (status_char == 0) {
                st->phase = SU_EXIT;
                continue;
            }

            /* Assembly lines 22-23: Jeff (char 3) has no PSI → re-select */
            if (status_char == PARTY_MEMBER_JEFF) {
                st->phase = SU_SELECT;
                continue;
            }

            /* Set win.battle_menu_current_character_id so generate_battle_psi_
             * list_callback knows which character's PSI to display. In assembly
             * CHAR_SELECT_PROMPT sets this via SELECT_BATTLE_MENU_CHARACTER. */
            uint16_t party_count = game_state.player_controlled_party_count & 0xFF;
            for (uint16_t i = 0; i < party_count; i++) {
                if (game_state.party_members[i] == status_char) {
                    win.battle_menu_current_character_id = (int16_t)i;
                    break;
                }
            }

            /* Assembly lines 25-57: first-display flag, create the category
             * window, build the 4-item menu (userdata = category + 1). */
            st->first_display = 1;
            create_window(WINDOW_STATUS_PSI_CATEGORY);
            for (int i = 0; i < 4; i++) {
                add_menu_item_no_position(status_psi_category_names[i],
                                          (uint16_t)(i + 1));
            }
            open_window_and_print_menu(1, 0);
            st->phase = SU_CAT_HEAD;
            continue;
        }

        case SU_CAT_HEAD:
            /* @CATEGORY_MENU_LOOP head (assembly lines 59-66): focus the
             * category window; the first iteration prints the items and takes
             * one WINDOW_TICK_WITHOUT_INSTANT_PRINTING frame (the yield). */
            set_window_focus(WINDOW_STATUS_PSI_CATEGORY);
            if (st->first_display) {
                st->first_display = 0;
                print_menu_items();
                dt.instant_printing = 0;
                window_tick_work();
                dt.instant_printing = 1;
                st->phase = SU_CAT_BODY;
                return STEP_RESULT_CONTINUE();
            }
            st->phase = SU_CAT_BODY;
            continue;

        case SU_CAT_BODY:
            /* Assembly line 67: refresh the status display area (PSI list
             * background), then restore focus + clear the alignment. */
            create_window(WINDOW_STATUS_MENU);
            win.current_focus_window = WINDOW_STATUS_PSI_CATEGORY;
            dt.force_left_text_alignment = 0;

            /* Assembly lines 71-74: GENERATE_BATTLE_PSI_LIST fills the
             * TEXT_STANDARD window as the cursor moves between categories. */
            set_cursor_move_callback(generate_battle_psi_list_callback);
            return pm_push_selection_status(st, SU_CAT_RESULT);

        case SU_CAT_RESULT: {
            uint16_t category_selection = menu_take_result(&st->result_ready,
                                                           &st->result);
            clear_cursor_move_callback();

            /* Assembly line 79: BEQ @CLOSE_CATEGORY (cancelled) */
            if (category_selection == 0) {
                /* Assembly lines 103-111: close the cascade, restore focus;
                 * SU_SELECT re-sets the alignment (the loop-bottom store). */
                close_window(WINDOW_STATUS_PSI_CATEGORY);
                close_window(WINDOW_TEXT_STANDARD);
                win.current_focus_window = WINDOW_STATUS_MENU;
                st->phase = SU_SELECT;
                continue;
            }

            /* Assembly lines 81-83: empty category → loop back. */
            if (get_window_menu_option_count(WINDOW_TEXT_STANDARD) == 0) {
                st->phase = SU_CAT_HEAD;
                continue;
            }

            /* Assembly lines 85-89: focus the ability list; DISPLAY_PSI_
             * DESCRIPTION renders each ability's description as cursor moves. */
            set_window_focus(WINDOW_TEXT_STANDARD);
            bt.last_selected_psi_description = 0x00FF;  /* force first redraw */
            set_cursor_move_callback(display_psi_description);
            st->phase = SU_PSI;
            continue;
        }

        case SU_PSI:
            /* Assembly lines 90-94: PSI description browse loop. */
            return pm_push_selection_status(st, SU_PSI_RESULT);

        case SU_PSI_RESULT: {
            uint16_t psi_selection = menu_take_result(&st->result_ready,
                                                      &st->result);
            if (psi_selection != 0) {
                st->phase = SU_PSI;  /* stay browsing (BNE @PSI_DESCRIPTION_LOOP) */
                continue;
            }

            /* Assembly lines 95-101: clean up the PSI description windows. */
            clear_cursor_move_callback();
            close_window(WINDOW_PSI_TARGET_COST);
            close_window(WINDOW_PSI_DESCRIPTION);
            bt.last_selected_psi_description = 0x00FF;
            st->phase = SU_CAT_HEAD;
            continue;
        }

        case SU_EXIT:
        default:
            /* Assembly lines 115-116, plus the caller's alignment clear (the
             * pause menu's bracket now lives inside the mode). */
            close_window(WINDOW_STATUS_MENU);
            dt.force_left_text_alignment = 0;
            return STEP_RESULT_POP(0);
        }
    }
}

/* GAME_MODE_PSI_MENU step — run-to-completion port of overworld_psi_menu()
 * (asm/text/menu/overworld_psi_menu.asm, 571 lines). See PsiMenuState in
 * mode_stack.h. The teleport destination menu is a GAME_MODE_TELEPORT_MENU
 * STEP_PUSH; determine_targetting() and the battle-action execution stay
 * blocking, called inline from the step (each pumps its converted children
 * internally — later conversions). */
StepResult mode_step_psi_menu(ModeState *ms) {
    PsiMenuState *st = &ms->psi_menu;

    for (;;) {
        switch ((PsiMenuPhase)st->phase) {

        case PS_ENTER:
            st->last_ability = 0x00FF;  /* @VIRTUAL01 (0xFF = initial) */
            st->action_result = 0;
            only_one_character_with_psi = 0;
            st->phase = PS_CHAR;
            continue;

        case PS_CHAR: {
            /* @CHARACTER_SELECT loop head (assembly lines 23-67) */
            uint16_t psi_count = count_characters_with_psi();
            if (psi_count == 1) {
                /* Single PSI user — auto-select (assembly lines 26-44).
                 * last_ability == 0 here means the ability menu was cancelled
                 * on the previous round: exit instead of re-entering. */
                if ((st->last_ability & 0xFF) == 0) {
                    st->phase = PS_EXIT;
                    continue;
                }
                uint16_t first_idx = find_first_character_with_psi();
                st->char_id = game_state.party_members[first_idx - 1];
                display_character_psi_list(st->char_id);
                only_one_character_with_psi = 1;

                overworld_selected_psi_user = st->char_id;
                st->last_ability = 0x00FF;
                st->phase = PS_ABILITY;
                continue;
            }
            /* Multiple PSI users — character selection (assembly lines 45-56).
             * mode=0: L/R arrow cycling with pagination arrows. */
            display_menu_header_text(0);  /* "Who?" */
            char_select_make_init(&pm_child_init, 0, 1,
                                  CS_ONCHANGE_PSI_LIST, CS_CHECKVALID_PSI);
            st->phase = PS_CHAR_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_CHAR_SELECT, &pm_child_init);
        }

        case PS_CHAR_RESULT:
            st->char_id = (uint16_t)mode_child_result();
            close_menu_header_window();
            if (st->char_id == 0) {
                st->phase = PS_EXIT;  /* cancelled */
                continue;
            }
            overworld_selected_psi_user = st->char_id;
            st->last_ability = 0x00FF;
            st->phase = PS_ABILITY;
            continue;

        case PS_ABILITY:
            /* @PSI_ABILITY_LOOP head (assembly lines 68-90) */
            set_window_focus(WINDOW_TEXT_STANDARD);

            /* Redisplay PSI list if returning from a failed action (73-78) */
            if ((st->last_ability & 0xFF) != 0xFF) {
                display_psi_ability_details(0, st->last_ability);
                print_menu_items();
            }

            /* Select a PSI ability; the target/cost preview lives in the
             * re-fetchable WindowInfo, safe across the push. */
            set_cursor_move_callback(display_psi_target_and_cost);
            return pm_push_selection_psi(st, PS_ABILITY_RESULT);

        case PS_ABILITY_RESULT: {
            uint16_t psi_selection = menu_take_result(&st->result_ready,
                                                      &st->menu_result);
            st->last_ability = psi_selection;
            clear_cursor_move_callback();

            if ((psi_selection & 0xFF) == 0) {
                /* @CANCELLED (assembly lines 213-216): close the target/cost
                 * window and break to character select; action_result stays as
                 * is — cancel is not a PSI action. */
                close_window(WINDOW_PSI_TARGET_COST);
                st->phase = PS_CHAR;
                continue;
            }

            /* Show PSI details for the multi-PSI case (lines 91-98) */
            if (!only_one_character_with_psi)
                display_psi_ability_details(6, psi_selection);

            /* @CHECK_PP_COST: look up the PP cost (assembly lines 100-154) */
            if (!ensure_battle_psi_table()) {
                st->action_result = 0;
                st->phase = PS_HANDLE;
                continue;
            }

            st->battle_action_id = battle_psi_table[psi_selection].battle_action;
            st->pp_cost = battle_action_table ?
                battle_action_table[st->battle_action_id].pp_cost : 0;
            uint16_t char_idx = st->char_id - 1;
            if (st->pp_cost > party_characters[char_idx].current_pp) {
                /* Not enough PP (assembly lines 147-154) */
                create_window(WINDOW_TEXT_BATTLE);
                st->phase = PS_FAIL_RESUME;
                bool pushed;
                StepResult r = menu_push_text(MSG_BTL6_NOT_ENOUGH_PP_MENU, &pushed);
                if (pushed) return r;
                continue;
            }

            /* Check if this is a teleport PSI (assembly lines 155-205) */
            st->psi_category = battle_psi_table[psi_selection].category;
            if (st->psi_category == PSI_CAT_OTHER) {
                /* Teleport checks (assembly lines 166-192) */
                bool blocked = false;
                if ((game_state.party_npc_1 & 0xFF) == PARTY_NPC_DUNGEON_MAN)
                    blocked = true;
                if ((game_state.party_npc_2 & 0xFF) == PARTY_NPC_DUNGEON_MAN)
                    blocked = true;
                if (event_flag_get(EVENT_FLAG_DISABLE_TELEPORT))
                    blocked = true;
                uint16_t ws = game_state.walking_style;
                if (ws == WALKING_STYLE_LADDER || ws == WALKING_STYLE_ROPE ||
                    ws == WALKING_STYLE_ESCALATOR || ws == WALKING_STYLE_STAIRS)
                    blocked = true;
                if (!blocked) {
                    uint16_t sector_attrs = load_sector_attrs(
                        game_state.leader_x_coord, game_state.leader_y_coord);
                    if (sector_attrs & MAP_SECTOR_CANNOT_TELEPORT)
                        blocked = true;
                }

                if (blocked) {
                    /* @TELEPORT_BLOCKED (assembly lines 197-205) */
                    create_window(WINDOW_TEXT_BATTLE);
                    st->phase = PS_FAIL_RESUME;
                    bool pushed;
                    StepResult r = menu_push_text(MSG_SYS_TELEPORT_BLOCKED, &pushed);
                    if (pushed) return r;
                    continue;
                }

                /* Teleport destination menu (assembly line 193), now
                 * GAME_MODE_TELEPORT_MENU; result read in PS_TELEPORT_RESULT. */
                pm_child_init = (ModeState){0};
                pm_child_init.teleport_menu.phase = TPM_ENTER;
                st->phase = PS_TELEPORT_RESULT;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_TELEPORT_MENU, &pm_child_init);
            }

            /* @NOT_TELEPORT: targeting (lines 206-212) — the battle row/enemy
             * targeting driver, blocking inline. */
            st->action_result = determine_targetting(st->battle_action_id,
                                                     st->char_id);
            st->phase = PS_HANDLE;
            continue;
        }

        case PS_TELEPORT_RESULT:
            /* Tail of the teleport destination menu (assembly line 193):
             * its 1-based destination (0 = cancelled) is the action result. */
            st->action_result = (uint16_t)mode_child_result();
            st->phase = PS_HANDLE;
            continue;

        case PS_FAIL_RESUME:
            /* Tail of the not-enough-PP / teleport-blocked message. */
            close_focus_window();
            st->action_result = 0;
            st->phase = PS_HANDLE;
            continue;

        case PS_HANDLE: {
            /* @HANDLE_RESULT (assembly lines 217-226) */
            if ((st->action_result & 0xFF) == 0) {
                st->phase = PS_ABILITY;  /* retry ability selection */
                continue;
            }

            close_window(WINDOW_PSI_TARGET_COST);

            if ((st->last_ability & 0xFF) == 0) {
                st->phase = PS_CHAR;  /* cancelled → retry character select */
                continue;
            }

            /* ---- Execute PSI action (assembly lines 227-354) ---- */

            /* Deduct PP (assembly lines 232-267): A=char_id, X=pp_cost,
             * Y=1 (flat amount mode). */
            reduce_pp_target(st->char_id, st->pp_cost, 1);

            if (st->psi_category == PSI_CAT_OTHER) {
                /* @TELEPORT: set teleport state (assembly lines 279-289);
                 * psi_ability[ability_id].level = teleport style (α/β). */
                uint8_t teleport_style = battle_psi_table[st->last_ability].level;
                set_teleport_state((uint8_t)st->action_result, teleport_style);
                st->phase = PS_EXECUTE;  /* no description text */
                continue;
            }

            /* @NOT_TELEPORT_ACTION: set up battler structures (lines 290-354) */
            bt.current_attacker = 0;  /* BATTLERS_TABLE[0] = first battler */
            battle_init_player_stats(st->char_id, &bt.battlers_table[0]);

            /* Set attacker name (assembly lines 297-304) */
            set_battle_attacker_name(
                (const char *)party_characters[st->char_id - 1].name,
                sizeof(party_characters[0].name));

            /* Set target name if targeting a specific ally (lines 305-318) */
            uint8_t target_id = st->action_result & 0xFF;
            if (target_id != 0xFF && target_id > 0) {
                set_battle_target_name(
                    (const char *)party_characters[target_id - 1].name,
                    sizeof(party_characters[0].name));
            }

            /* Set current item to the PSI ability (lines 320-321) */
            set_current_item((uint8_t)st->last_ability);

            /* Display the action description text (assembly lines 322-354) */
            create_window(WINDOW_TEXT_STANDARD);
            st->phase = PS_EXECUTE;
            if (battle_action_table) {
                uint32_t desc_addr =
                    battle_action_table[st->battle_action_id].description_text_pointer;
                if (desc_addr != 0) {
                    bool pushed;
                    StepResult r = menu_push_text(desc_addr, &pushed);
                    if (pushed) return r;
                }
            }
            continue;
        }

        case PS_EXECUTE: {
            /* Execute the battle action function (assembly lines 355-560).
             * NULL pointer → @AFTER_CHAR_SELECT5 (result=1, no execution);
             * target 0xFF = all-party loop, else single target. The action
             * functions can display text — blocking inline. */
            uint32_t func_addr = 0;
            if (battle_action_table)
                func_addr = battle_action_table[st->battle_action_id].battle_function_pointer;

            if (func_addr != 0) {
                uint8_t target_id = st->action_result & 0xFF;

                /* Set up target battler (assembly lines 394-395) */
                bt.current_target = sizeof(Battler);  /* BATTLERS_TABLE[1] */

                if (target_id == 0xFF) {
                    /* All-party target (@MULTI_CHARACTER, lines 400-503) */
                    for (uint16_t i = 0;
                         i < (game_state.player_controlled_party_count & 0xFF);
                         i++) {
                        uint8_t member_id = game_state.party_members[i];

                        /* Set target name (assembly lines 404-421) */
                        set_battle_target_name(
                            (const char *)party_characters[member_id - 1].name,
                            sizeof(party_characters[0].name));

                        /* Init target battler (assembly lines 422-428) */
                        battle_init_player_stats(member_id, &bt.battlers_table[1]);

                        /* Call the action function (assembly lines 429-459) */
                        bt.temp_function_pointer = func_addr;
                        jump_temp_function_pointer();

                        /* Copy afflictions back (assembly lines 463-492).
                         * Note: assembly uses the loop counter for the
                         * party_characters index, matching this use of i. */
                        for (int g = 0; g < AFFLICTION_GROUP_COUNT; g++) {
                            party_characters[i].afflictions[g] =
                                bt.battlers_table[1].afflictions[g];
                        }
                    }
                } else {
                    /* Single target (@AFTER_CHAR_SELECT0, lines 504-559) */
                    if (target_id > 0)
                        battle_init_player_stats(target_id, &bt.battlers_table[1]);

                    bt.temp_function_pointer = func_addr;
                    jump_temp_function_pointer();

                    /* Copy afflictions back (assembly lines 529-559) */
                    if (target_id > 0) {
                        for (int g = 0; g < AFFLICTION_GROUP_COUNT; g++) {
                            party_characters[target_id - 1].afflictions[g] =
                                bt.battlers_table[1].afflictions[g];
                        }
                    }
                }

                /* @AFTER_CHAR_SELECT4 (assembly line 561) */
                render_and_disable_entities();
            }

            /* @AFTER_CHAR_SELECT5 (assembly lines 563-564) */
            st->action_result = 1;
            st->phase = PS_EXIT;
            continue;
        }

        case PS_EXIT:
        default:
            /* @AFTER_CHAR_SELECT6 / @DONE (assembly lines 566-570) */
            close_window(WINDOW_TEXT_STANDARD);
            return STEP_RESULT_POP(st->action_result);
        }
    }
}

/* GAME_MODE_USE_ITEM step — run-to-completion port of overworld_use_item()
 * (asm/overworld/use_item.asm, 545 lines): the pause menu's Goods → Use path.
 * Determines if the item can be used based on item type, character usability,
 * sector context, and nearby NPCs. If usable, runs targeting, removes
 * consumable items, and executes the item's battle action with affliction
 * writeback. See UseItemState in mode_stack.h.
 *
 * determine_targetting() and the battle-action execution via
 * jump_temp_function_pointer() stay blocking, called inline from the step
 * (each pumps its converted children internally — later conversions).
 *
 * Pops 0 if targeting was cancelled, 1 otherwise (item used or message
 * shown); the pause-menu parent branches in PM_USE_RESUME. */
StepResult mode_step_use_item(ModeState *ms) {
    UseItemState *st = &ms->use_item;

    for (;;) {
        switch ((UseItemPhase)st->phase) {

        case UI_ENTER: {
            uint32_t desc_text_addr = 0;  /* @LOCAL08: description text SNES address (0=none) */
            uint16_t can_use = 0;         /* @LOCAL07: whether item is usable */
            /* @VIRTUAL00: target character. Initially char_id; overwritten by
             * DETERMINE_TARGETTING result when can_use is true (assembly lines 247-262). */
            st->target_id = (uint8_t)st->char_id;

            /* Assembly lines 29-37: get item from inventory */
            st->item_id = get_character_item(st->char_id, st->item_slot) & 0xFF;

            /* Assembly lines 38-50: look up item config entry. A missing entry
             * skips straight to the action-window setup with can_use=0 (the
             * blocking form's goto setup_action_window). */
            const ItemConfig *item_entry = get_item_entry(st->item_id);
            st->effect_id = item_entry ? item_entry->effect_id : 0;

            if (item_entry) {
                uint8_t item_type = item_entry->type;

                /* Assembly lines 51-61: classify item by type flags.
                 * Check bits: TRANSFORM (0x10) | CANNOT_GIVE (0x20) = 0x30 */
                uint8_t type_category =
                    item_type & (ITEM_FLAG_TRANSFORM | ITEM_FLAG_CANNOT_GIVE);

                if (type_category == 0) {
                    /* @TYPE_USABLE (lines 62-76): normal usable item */
                    can_use = 1;
                    if (battle_action_table)
                        desc_text_addr =
                            battle_action_table[st->effect_id].description_text_pointer;
                } else if (type_category == ITEM_FLAG_TRANSFORM) {
                    /* @TYPE_EQUIPMENT (lines 77-80): equipment item, show equip message */
                    desc_text_addr = MSG_SYS_ITEM_IS_EQUIPMENT;
                } else if (type_category == ITEM_FLAG_CANNOT_GIVE) {
                    /* @TYPE_CANNOT_GIVE (lines 81-95): can't give but can use */
                    can_use = 1;
                    if (battle_action_table)
                        desc_text_addr =
                            battle_action_table[st->effect_id].description_text_pointer;
                } else if (!check_item_usable_by(st->char_id, st->item_id)) {
                    /* @TYPE_KEY_ITEM (lines 96-244): TRANSFORM | CANNOT_GIVE.
                     * Assembly lines 97-112: this character can't use this item */
                    desc_text_addr = MSG_GOODS4_SYS_ITEM_WRONG_USER;
                } else {
                    /* Assembly lines 113-122: check context bits (bits 2-3 of item_type).
                     * 0x00 = use anywhere, 0x04 = battle only, 0x08 = check context */
                    uint8_t context_bits = item_type & 0x0C;

                    if (context_bits == 0x00) {
                        /* @USE_ANYWHERE (lines 123-137): usable anywhere */
                        can_use = 1;
                        if (battle_action_table)
                            desc_text_addr =
                                battle_action_table[st->effect_id].description_text_pointer;
                    } else if (context_bits == 0x04) {
                        /* @BATTLE_ONLY (lines 138-141): can only be used in battle */
                        desc_text_addr = MSG_SYS_ITEM_CANT_USE_HERE;
                    } else if (context_bits == 0x08) {
                        /* @CHECK_USE_CONTEXT (lines 142-244): check sub-type (bits 0-1) */
                        uint8_t sub_type = item_type & 0x03;

                        if (sub_type == 0 || sub_type == 1) {
                            /* @USE_DEFAULT (lines 153-167): usable */
                            can_use = 1;
                            if (battle_action_table)
                                desc_text_addr =
                                    battle_action_table[st->effect_id].description_text_pointer;
                        } else if (sub_type == 2) {
                            /* @CHECK_SECTOR_TYPE (lines 168-203): compare sector item type
                             * with this item's ID. E.g., bicycle only works in bicycle
                             * sectors. */
                            uint16_t sector_item_type = get_sector_item_type();
                            if (sector_item_type != st->item_id) {
                                /* @SECTOR_MISMATCH (lines 200-203) */
                                desc_text_addr = MSG_SYS_ITEM_CANT_USE_HERE;
                            } else if (st->item_id == ITEM_BICYCLE &&
                                       get_collision_at_leader() != 0) {
                                /* Bicycle collision check (lines 176-184) */
                                desc_text_addr = MSG_SYS_BIKE_TOO_CRAMPED;
                            } else {
                                /* @SECTOR_MATCH (lines 185-199) */
                                can_use = 1;
                                if (battle_action_table)
                                    desc_text_addr =
                                        battle_action_table[st->effect_id].description_text_pointer;
                            }
                        } else if (sub_type == 3) {
                            /* @CHECK_NPC_TARGET (lines 204-244): check for nearby NPC
                             * that has a use-text response. */
                            can_use = 1;
                            uint8_t npc_type = get_nearby_npc_config_type();

                            /* Assembly lines 211-214: NPC types 1 (PERSON) and 3 (OBJECT)
                             * have text_pointer2 for "use item on NPC" text */
                            if (npc_type == 1 || npc_type == 3) {
                                /* @NPC_HAS_USE_TEXT (lines 215-225): read text_pointer2 */
                                desc_text_addr =
                                    get_npc_config_text_pointer2(ow.interacting_npc_id);
                            }

                            /* Assembly lines 226-244: if text pointer is still NULL,
                             * fall back to battle action description text */
                            if (desc_text_addr == 0 && battle_action_table)
                                desc_text_addr =
                                    battle_action_table[st->effect_id].description_text_pointer;
                        }
                    }
                    /* context_bits == 0x0C falls through without setting anything */
                }

                /* Assembly lines 245-265: targeting and consume check */
                if (can_use) {
                    /* Assembly lines 253-265: DETERMINE_TARGETTING — the battle
                     * row/enemy targeting driver, blocking inline. */
                    uint16_t target_result =
                        determine_targetting(st->effect_id, st->char_id);
                    st->target_id = target_result & 0xFF;

                    if (st->target_id == 0)
                        return STEP_RESULT_POP(0);  /* Targeting cancelled (lines 264-265) */

                    /* Assembly lines 266-274: consume item if CONSUMED_ON_USE flag set */
                    if (item_entry->flags & ITEM_FLAG_CONSUMED)
                        remove_item_from_inventory(st->char_id, st->item_slot);
                }
            }

            /* @SETUP_ACTION_WINDOW: close inventory windows, set up battle state
             * (assembly lines 275-334) */
            close_window(WINDOW_INVENTORY_MENU);
            close_window(WINDOW_INVENTORY);

            /* Set attacker name from character (assembly lines 280-287) */
            set_battle_attacker_name(
                (const char *)party_characters[st->char_id - 1].name,
                sizeof(party_characters[0].name));

            /* Set current item (assembly lines 288-291) */
            set_current_item((uint8_t)st->item_id);

            /* Open text window (assembly line 292) */
            create_window(WINDOW_TEXT_STANDARD);

            /* Set working_memory = char_id, argument_memory = item_slot
             * (assembly lines 293-308) */
            set_working_memory((uint32_t)st->char_id);
            set_argument_memory((uint32_t)st->item_slot);

            /* Set target name if targeting a specific ally (assembly lines 309-322).
             * target_id 0xFF = all targets, skip target name. */
            if (st->target_id != 0xFF) {
                set_battle_target_name(
                    (const char *)party_characters[st->target_id - 1].name,
                    sizeof(party_characters[0].name));
            }

            /* Assembly lines 323-334: if description text is NULL, use fallback */
            if (desc_text_addr == 0)
                desc_text_addr = MSG_SYS_ITEM_USE_FORBIDDEN;

            /* Assembly lines 335-357: branch on can_use / the battle action
             * function pointer. */
            uint32_t func_addr = 0;
            if (can_use && battle_action_table)
                func_addr = battle_action_table[st->effect_id].battle_function_pointer;

            if (!can_use || func_addr == 0) {
                /* @DISPLAY_TEXT_ONLY (lines 536-539): show the message only,
                 * then close the text window in UI_EXIT. */
                st->phase = UI_EXIT;
                bool pushed;
                StepResult r = menu_push_text(desc_text_addr, &pushed);
                if (pushed) return r;
                continue;
            }

            /* Assembly lines 358-379: set up attacker battler */
            bt.current_attacker = 0;  /* BATTLERS_TABLE[0] */
            battle_init_player_stats(st->char_id, &bt.battlers_table[0]);

            /* Set item as action argument (assembly lines 364-368) */
            bt.battlers_table[0].current_action_argument = (uint8_t)st->item_id;

            /* Set item slot on attacker battler (assembly lines 369-373) */
            bt.battlers_table[0].action_item_slot = (uint8_t)st->item_slot;

            /* Display description text (assembly lines 374-376); the action
             * execution resumes in UI_EXECUTE after the text pops. */
            st->phase = UI_EXECUTE;
            bool pushed;
            StepResult r = menu_push_text(desc_text_addr, &pushed);
            if (pushed) return r;
            continue;
        }

        case UI_EXECUTE: {
            /* Re-set current item after text display (assembly lines 377-380) */
            set_current_item((uint8_t)st->item_id);

            /* Set up target battler (assembly line 381-382) */
            bt.current_target = sizeof(Battler);  /* BATTLERS_TABLE[1] */

            if (st->target_id == 0xFF) {
                /* All-party target (assembly @ALL_TARGETS_LOOP, lines 388-479).
                 * Execute action on each party member. */
                uint16_t party_count = game_state.player_controlled_party_count & 0xFF;
                for (uint16_t i = 0; i < party_count; i++) {
                    uint8_t member_id = game_state.party_members[i];

                    /* Set target name (assembly lines 392-414) */
                    set_battle_target_name(
                        (const char *)party_characters[member_id - 1].name,
                        sizeof(party_characters[0].name));

                    /* Init target battler (assembly lines 415-421) */
                    battle_init_player_stats(member_id, &bt.battlers_table[1]);

                    /* Look up and call battle action function (assembly lines 422-435) */
                    bt.temp_function_pointer =
                        battle_action_table[st->effect_id].battle_function_pointer;
                    jump_temp_function_pointer();

                    /* Copy afflictions from battler back to char_struct
                     * (assembly lines 436-468).
                     * Note: assembly uses loop counter i for party_characters index,
                     * not the member_id. Ported faithfully per CLAUDE.md. */
                    for (int g = 0; g < AFFLICTION_GROUP_COUNT; g++) {
                        party_characters[i].afflictions[g] =
                            bt.battlers_table[1].afflictions[g];
                    }
                }
            } else {
                /* Specific target (assembly @SPECIFIC_TARGET, lines 480-532) */
                battle_init_player_stats(st->target_id, &bt.battlers_table[1]);

                /* Look up and call battle action function (assembly lines 483-498) */
                bt.temp_function_pointer =
                    battle_action_table[st->effect_id].battle_function_pointer;
                jump_temp_function_pointer();

                /* Copy afflictions back (assembly lines 499-532) */
                for (int g = 0; g < AFFLICTION_GROUP_COUNT; g++) {
                    party_characters[st->target_id - 1].afflictions[g] =
                        bt.battlers_table[1].afflictions[g];
                }
            }

            /* @AFTER_ACTION (assembly line 533-534) */
            render_and_disable_entities();
            st->phase = UI_EXIT;
            continue;
        }

        case UI_EXIT:
        default:
            /* @CLOSE_TEXT_WINDOW (assembly lines 540-543) */
            close_window(WINDOW_TEXT_STANDARD);
            return STEP_RESULT_POP(1);  /* TRUE */
        }
    }
}

StepResult mode_step_pause_menu(ModeState *ms) {
    PauseMenuState *st = &ms->pause_menu;

    for (;;) {
        switch ((PauseMenuPhase)st->phase) {

        case PM_ENTER:
            /* One-shot setup (open_menu.asm lines 14-23); no yield. */
            disable_all_entities();
            play_sfx(1);  /* SFX::CURSOR1 */
            create_window(WINDOW_COMMAND_MENU);
            skip_adding_command_text = 0;
            build_command_menu();
            win.restore_menu_backup = 0;  /* STZ RESTORE_MENU_BACKUP (line 23) */
            st->phase = PM_MAIN;
            continue;

        case PM_MAIN:
            /* @MAIN_PAUSE_MENU head */
            set_window_focus(0);  /* WINDOW::COMMAND_MENU index in open table */
            return pm_push_selection(st, PM_MAIN_RESULT, 1);

        case PM_MAIN_RESULT: {
            uint16_t selection = pm_take_result(st);
            switch (selection) {

            /* --- Talk to (assembly lines 45-54) --- */
            case 1: {  /* MENU_OPTIONS::TALK_TO */
                uint32_t text_ptr = talk_to();
                if (text_ptr == 0)
                    text_ptr = MSG_SYS_TALK_NO_TARGET;
                bool pushed;
                StepResult r = pm_push_text(st, PM_CLEANUP, text_ptr, &pushed);
                if (pushed) return r;
                continue;
            }

            /* --- Goods (assembly lines 55-61) --- */
            case 2:  /* MENU_OPTIONS::GOODS */
                /* SHOW_HPPP_AND_MONEY_WINDOWS (assembly line 56) */
                show_hppp_windows();
                display_money_window();
                st->goods_char = 0;
                st->item_slot = 0;
                st->phase = PM_GOODS_CHAR;
                continue;

            /* --- PSI (assembly lines 552-572) --- */
            case 3: {  /* MENU_OPTIONS::PSI */
                show_hppp_windows();
                display_money_window();

                /* Highlight first PSI-capable party member (lines 554-561) */
                uint16_t first_psi = find_first_character_with_psi();
                if (first_psi != 0)
                    select_battle_menu_character(first_psi - 1);

                /* OVERWORLD_PSI_MENU, now GAME_MODE_PSI_MENU. The used/
                 * cancelled branch + single-PSI sfx tail run in PM_PSI_RESUME.
                 * Note: assembly does NOT set dt.force_left_text_alignment for
                 * PSI (only STATUS). */
                pm_child_init = (ModeState){0};
                pm_child_init.psi_menu.phase = PS_ENTER;
                st->phase = PM_PSI_RESUME;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_PSI_MENU, &pm_child_init);
            }

            /* --- Equip (assembly lines 573-583) --- */
            case 4:  /* MENU_OPTIONS::EQUIP */
                show_hppp_windows();
                display_money_window();

                /* OPEN_EQUIPMENT_MENU: character selection + equipment change,
                 * now GAME_MODE_EQUIP_MENU. The single-party sfx tail (assembly
                 * lines 576-583) runs in PM_EQUIP_RESUME after it pops. */
                pm_child_init = (ModeState){0};
                pm_child_init.equip_menu.phase = EQ_ENTER;
                st->phase = PM_EQUIP_RESUME;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_EQUIP_MENU, &pm_child_init);

            /* --- Check (assembly lines 584-593) --- */
            case 5: {  /* MENU_OPTIONS::CHECK */
                uint32_t text_ptr = check_action();
                if (text_ptr == 0)
                    text_ptr = MSG_SYS_NOTHING_WRONG_HERE;
                bool pushed;
                StepResult r = pm_push_text(st, PM_CLEANUP, text_ptr, &pushed);
                if (pushed) return r;
                continue;
            }

            /* --- Status (assembly lines 594-602) --- */
            case 6:  /* MENU_OPTIONS::STATUS */
                show_hppp_windows();
                display_money_window();

                /* OPEN_STATUS_MENU, now GAME_MODE_STATUS_MENU. The
                 * FORCE_LEFT_TEXT_ALIGNMENT bracket lives inside the mode
                 * (set at SU_SELECT, cleared at SU_EXIT); no resume tail. */
                pm_child_init = (ModeState){0};
                pm_child_init.status_menu.phase = SU_SELECT;
                st->phase = PM_MAIN;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_STATUS_MENU, &pm_child_init);

            /* Cancel (B/Select) or unknown → cleanup */
            default:
                st->phase = PM_CLEANUP;
                continue;
            }
        }

        case PM_GOODS_CHAR:
            /* @GOODS character selection: single vs multi-party */
            if ((game_state.player_controlled_party_count & 0xFF) == 1) {
                /* Single party member (assembly lines 62-83) */
                uint8_t member = game_state.party_members[0];

                /* Check if character has any items */
                if (get_character_item(member, 1) == 0) {
                    st->phase = PM_MAIN;  /* no items → @MAIN_PAUSE_MENU */
                    continue;
                }

                /* Populate inventory window */
                inventory_get_item_name(member, WINDOW_INVENTORY);
                st->goods_char = member;

                /* Highlight character in HPPP window */
                select_battle_menu_character(0);
                st->phase = PM_GOODS_INV;
                continue;
            }
            /* Multi-party path (assembly lines 84-93) */
            display_menu_header_text(0);  /* "Who?" */

            /* Assembly: mode=0, allow_cancel=1, on_change=GET_WEAPON_ITEM_NAME
             * (shows inventory as cursor moves). */
            char_select_make_init(&pm_child_init, 0, 1,
                                  CS_ONCHANGE_WEAPON_NAME, CS_CHECKVALID_NONE);
            st->phase = PM_GOODS_CHAR_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_CHAR_SELECT, &pm_child_init);

        case PM_GOODS_CHAR_RESULT: {
            uint16_t goods_char = (uint16_t)mode_child_result();
            st->goods_char = goods_char;

            /* After character selection (assembly lines 94-101) */
            if (goods_char == 0) {
                /* Cancelled — close inventory and header, return to main menu */
                close_window(WINDOW_INVENTORY);
                close_menu_header_window();
                st->phase = PM_MAIN;
                continue;
            }

            /* Verify character has items (assembly lines 102-107) */
            if (get_character_item(goods_char, 1) == 0) {
                st->phase = PM_GOODS_CHAR;  /* no items → re-select character */
                continue;
            }
            st->phase = PM_GOODS_INV;
            continue;
        }

        case PM_GOODS_INV:
            /* @GOODS_SHOW_INVENTORY (assembly lines 108-117) */
            display_menu_header_text(1);  /* "Which?" */
            set_window_focus(WINDOW_INVENTORY);
            return pm_push_selection(st, PM_GOODS_INV_RESULT, 1);

        case PM_GOODS_INV_RESULT: {
            uint16_t item_slot = pm_take_result(st);
            backup_selected_menu_option();
            close_menu_header_window();

            if (item_slot == 0) {
                /* Cancelled item selection (assembly lines 120-137) */
                if ((game_state.player_controlled_party_count & 0xFF) != 1) {
                    st->phase = PM_GOODS_CHAR;  /* multi-party → re-select char */
                    continue;
                }

                /* Single party: play SFX if has items, clear indicator */
                if (get_character_item(game_state.party_members[0], 1) != 0) {
                    play_sfx(27);  /* SFX::MENU_OPEN_CLOSE */
                    clear_battle_menu_character_indicator();
                }
                close_window(WINDOW_INVENTORY);
                st->phase = PM_MAIN;
                continue;
            }
            st->item_slot = item_slot;

            /* --- Item selected: build action menu (assembly lines 138-194) --- */
            create_window(WINDOW_INVENTORY_MENU);

            /* Check if character is unconscious/diamondized (lines 140-156).
             * If affliction is UNCONSCIOUS(1) or DIAMONDIZED(2), skip "Use". */
            uint16_t char_idx = st->goods_char - 1;
            uint8_t affliction = party_characters[char_idx].afflictions[0];
            int start_action = 0;  /* 0 = include Use, 1 = skip Use */
            if (affliction != 0) {
                /* Assembly: LDA #4; CLC; SBC affliction; BRANCHLTEQS
                 * Tests 4 - affliction - 1 <= 0 → affliction >= 4.
                 * So affliction 1,2,3 → start_action=1 (skip Use). */
                if (4 - (int)affliction - 1 > 0)
                    start_action = 1;
            }

            /* Build Use/Give/Drop/Help menu (assembly lines 159-192).
             * ITEM_USE_MENU_STRINGS: "Use"(0), "Give"(1), "Drop"(2), "Help!"(3)
             * Userdata: Use=1, Give=2, Drop=3, Help!=4 */
            static const char *item_action_labels[4] EB_NORELOC = {
                "Use", "Give", "Drop", "Help!"
            };

            set_focus_text_cursor(0, 0);
            for (int i = start_action; i < 4; i++) {
                add_menu_item_no_position(item_action_labels[i], i + 1);
            }
            open_window_and_print_menu(1, 0);

            st->action_reentry = 0;     /* @VIRTUAL02 */
            st->reprint_inventory = 0;  /* @LOCAL04 */
            st->phase = PM_ACTION_MENU;
            continue;
        }

        case PM_ACTION_MENU:
            /* @ITEM_ACTION_LOOP head (assembly lines 195-230) */
            if (st->action_reentry != 0) {
                set_window_focus(WINDOW_INVENTORY);
                /* @LOCAL04 != 0 → PRINT_MENU_ITEMS on the INVENTORY focus
                 * (assembly lines 202-209): re-renders the item list that the
                 * give path's CLEAR_FOCUS_WINDOW_CONTENT wiped. The use-cancel
                 * return passes 0 (nothing was cleared). */
                if (st->reprint_inventory != 0)
                    print_menu_items();
            } else {
                set_window_focus(WINDOW_INVENTORY_MENU);
                print_menu_items();
            }
            set_window_focus(WINDOW_INVENTORY_MENU);
            return pm_push_selection(st, PM_ACTION_RESULT, 1);

        case PM_ACTION_RESULT: {
            uint16_t action = pm_take_result(st);

            if (action == 0) {
                /* @BACK_TO_ITEM_LIST: cancel → return to item list */
                close_focus_window();  /* close INVENTORY_MENU */
                set_window_focus(WINDOW_INVENTORY);
                st->phase = PM_GOODS_INV;
                continue;
            }

            switch (action) {
            case 1:
                /* @GOODS_ITEM_USE (assembly lines 236-252), now
                 * GAME_MODE_USE_ITEM. @VIRTUAL02=1 is set at entry (line 237);
                 * the used/cancelled branch runs in PM_USE_RESUME. */
                st->action_reentry = 1;
                pm_child_init = (ModeState){0};
                pm_child_init.use_item.phase = UI_ENTER;
                pm_child_init.use_item.char_id = st->goods_char;
                pm_child_init.use_item.item_slot = st->item_slot;
                st->phase = PM_USE_RESUME;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_USE_ITEM, &pm_child_init);

            case 4: {
                /* @GOODS_ITEM_HELP (assembly lines 253-263):
                 * Clear windows, set restore_menu_backup, show item help. */
                clear_window_text(0);
                clear_window_text(2);
                win.restore_menu_backup = 0xFF;

                create_window(WINDOW_TEXT_STANDARD);

                /* Get item from inventory */
                uint16_t help_item_id = get_character_item(st->goods_char, st->item_slot);

                /* Look up help_text pointer in item config table.
                 * item::help_text is a 4-byte SNES pointer at offset 35. */
                const ItemConfig *item_entry = get_item_entry(help_item_id);
                if (item_entry) {
                    uint32_t help_text_addr = item_entry->help_text & 0xFFFFFF;
                    bool pushed;
                    StepResult r = pm_push_text(st, PM_HELP_RESUME,
                                                help_text_addr, &pushed);
                    if (pushed) return r;
                    continue;
                }
                st->phase = PM_HELP_RESUME;  /* no entry → skip the text */
                continue;
            }

            case 2:
                /* @GOODS_GIVE (assembly lines 304-331):
                 * Give item to another party member. */
                set_window_focus(WINDOW_INVENTORY);

                /* Assembly (open_menu.asm:307): CLEAR_FOCUS_WINDOW_CONTENT frees
                 * the giver inventory window's BG2 content tiles before the
                 * recipient's inventory is rendered into WINDOW_OVERWORLD_CHAR_SELECT
                 * during the char select. Without this, both full inventories
                 * hold tiles at once and exhaust the ~407-tile BG2 pool
                 * ("alloc_bg2_tilemap_entry: tile exhaustion!"). */
                clear_focus_window_content_far();

                st->action_reentry = 1;
                display_menu_header_text(3);  /* "Whom?" */

                /* Select target character.
                 * Assembly: mode=2, allow_cancel=1, on_change=GET_BODY_ITEM_NAME. */
                char_select_make_init(&pm_child_init, 2, 1,
                                      CS_ONCHANGE_BODY_NAME, CS_CHECKVALID_NONE);
                st->phase = PM_GIVE_CHAR_RESULT;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_CHAR_SELECT, &pm_child_init);

            case 3: {
                /* @GOODS_DROP (assembly lines 534-551):
                 * Drop item — display confirmation text, let text script handle it. */
                create_window(WINDOW_TEXT_STANDARD);
                set_working_memory(st->goods_char);
                set_argument_memory(st->item_slot);
                bool pushed;
                StepResult r = pm_push_text(st, PM_DROP_RESUME,
                                            MSG_SYS_ITEM_DROP, &pushed);
                if (pushed) return r;
                continue;
            }

            default:
                st->phase = PM_CLEANUP;
                continue;
            }  /* switch(action) */
        }

        case PM_USE_RESUME:
            /* Tail of @GOODS_ITEM_USE after USE_ITEM pops (assembly lines
             * 240-252): an item used (or message shown) closes the whole pause
             * menu; a targeting cancel returns to the item action menu
             * (@VIRTUAL00=0, @LOCAL04=0; @VIRTUAL02 stays 1 from entry). */
            if (mode_child_result() != 0) {
                st->phase = PM_CLEANUP;
                continue;
            }
            st->reprint_inventory = 0;
            st->phase = PM_ACTION_MENU;
            continue;

        case PM_HELP_RESUME:
            /* Tail of @GOODS_ITEM_HELP after the help text */
            close_window(WINDOW_TEXT_STANDARD);

            /* Rebuild command menu and inventory list */
            set_window_focus(WINDOW_COMMAND_MENU);
            skip_adding_command_text = 1;
            build_command_menu();

            /* Rebuild inventory */
            inventory_get_item_name(st->goods_char, WINDOW_INVENTORY);
            close_window(WINDOW_INVENTORY_MENU);
            set_window_focus(WINDOW_INVENTORY);
            st->phase = PM_GOODS_INV;
            continue;

        case PM_GIVE_CHAR_RESULT: {
            uint16_t give_target = (uint16_t)mode_child_result();
            close_menu_header_window();
            close_window(WINDOW_OVERWORLD_CHAR_SELECT);

            if (give_target == 0) {
                /* Cancelled give → return to action menu, reprinting the
                 * inventory that @GOODS_GIVE's CLEAR_FOCUS_WINDOW_CONTENT
                 * wiped (assembly lines 322-327: @VIRTUAL00=1, @LOCAL04=1) */
                st->reprint_inventory = 1;
                st->phase = PM_ACTION_MENU;
                continue;
            }
            st->give_target = give_target;

            /* Check CANNOT_GIVE flag on item (assembly lines 332-361) */
            uint16_t give_item_id = get_character_item(st->goods_char, st->item_slot);
            const ItemConfig *give_entry = get_item_entry(give_item_id);
            if (give_entry && give_target != st->goods_char &&
                (give_entry->flags & ITEM_FLAG_CANNOT_GIVE)) {
                /* Can't give this item */
                create_window(WINDOW_TEXT_STANDARD);
                set_working_memory(st->goods_char);
                set_argument_memory(st->item_slot);
                bool pushed;
                StepResult r = pm_push_text(st, PM_GIVE_BLOCKED_RESUME,
                                            MSG_SYS_ITEM_EXCLUSIVE_CARRIER, &pushed);
                if (pushed) return r;
                continue;
            }

            /* Determine give status case (assembly lines 362-457).
             * case_index combines: same_char, giver_dead, has_space, target_dead */
            int case_index = 0;
            uint16_t giver_idx = st->goods_char - 1;
            uint16_t target_idx = give_target - 1;
            uint8_t giver_aff = party_characters[giver_idx].afflictions[0];
            bool giver_dead = (giver_aff == 1 || giver_aff == 2);
            /* UNCONSCIOUS=1, DIAMONDIZED=2 */

            if (st->goods_char == give_target) {
                /* Self-give */
                case_index = giver_dead ? 5 : 0;
            } else {
                /* Check target inventory space */
                bool has_space = (find_inventory_space2(give_target) != 0);

                /* Check target dead */
                uint8_t target_aff = party_characters[target_idx].afflictions[0];
                bool target_dead = (target_aff == 1 || target_aff == 2);

                if (giver_dead) {
                    /* Giver dead: cases 6-9 */
                    case_index = 6;  /* dead→alive fail */
                    if (target_dead) case_index = 7;  /* dead→dead fail */
                    if (has_space) case_index += 2;   /* +2 = success */
                } else {
                    /* Giver alive: cases 1-4 */
                    case_index = 1;  /* alive→alive fail */
                    if (target_dead) case_index = 2;  /* alive→dead fail */
                    if (has_space) case_index += 2;   /* +2 = success */
                }
            }
            st->give_case = (uint8_t)case_index;

            /* Display appropriate message (assembly lines 458-525) */
            static const uint32_t carry_msg_addrs[10] = {
                MSG_SYS_ITEM_REARRANGED_SELF,               /* 0 */
                MSG_SYS_ITEM_GIVE_FULL_BOTH_ALIVE,   /* 1 */
                MSG_SYS_ITEM_GIVE_FULL_ALIVE_KO,    /* 2 */
                MSG_SYS_ITEM_GAVE_BOTH_ALIVE,        /* 3 */
                MSG_SYS_ITEM_GAVE_ALIVE_TO_KO,         /* 4 */
                MSG_SYS_ITEM_REARRANGED_KO,                /* 5 */
                MSG_SYS_ITEM_GIVE_FULL_KO_ALIVE,    /* 6 */
                MSG_SYS_ITEM_GIVE_FULL_BOTH_KO,     /* 7 */
                MSG_SYS_ITEM_TOOK_FROM_KO,         /* 8 */
                MSG_SYS_ITEM_MOVED_KO_TO_KO,          /* 9 */
            };

            create_window(WINDOW_TEXT_STANDARD);
            /* Assembly @DISPLAY_GIVE_MESSAGE: working_memory = source char,
             * working_memory_storage = target char, argument_memory = item slot.
             * The give-success/fail messages substitute the recipient's name
             * from working_memory_storage — without it, "gave to <X>" renders
             * the wrong name (e.g. the giver) for give-to-other. */
            set_working_memory(st->goods_char);
            set_working_memory_storage(give_target);
            set_argument_memory(st->item_slot);

            bool pushed;
            StepResult r = pm_push_text(st, PM_GIVE_MSG_RESUME,
                                        carry_msg_addrs[case_index], &pushed);
            if (pushed) return r;
            continue;
        }

        case PM_GIVE_BLOCKED_RESUME:
            /* Tail of the CANNOT_GIVE message → back to the action menu,
             * reprinting the wiped inventory (assembly lines 357-361:
             * @VIRTUAL00=1, @LOCAL04=1) */
            close_window(WINDOW_TEXT_STANDARD);
            st->reprint_inventory = 1;
            st->phase = PM_ACTION_MENU;
            continue;

        case PM_GIVE_MSG_RESUME: {
            /* Successful transfers call SWAP_ITEM_INTO_EQUIPMENT.
             * Cases 0,3,4,5,8,9 are success (assembly lines 458-523). */
            uint8_t c = st->give_case;
            bool give_success = (c == 0 || c == 3 || c == 4 ||
                                 c == 5 || c == 8 || c == 9);
            if (give_success) {
                swap_item_into_equipment(st->goods_char, st->item_slot,
                                         st->give_target);
            }

            /* Cleanup: close all sub-windows, return to main menu
             * (assembly @GOODS_GIVE_DONE, lines 526-533). */
            close_window(WINDOW_TEXT_STANDARD);
            close_window(WINDOW_INVENTORY_MENU);
            close_window(WINDOW_INVENTORY);
            st->phase = PM_MAIN;
            continue;
        }

        case PM_DROP_RESUME:
            /* Tail of @GOODS_DROP after the confirmation text */
            close_window(WINDOW_TEXT_STANDARD);
            close_window(WINDOW_INVENTORY_MENU);
            close_window(WINDOW_INVENTORY);
            st->phase = PM_MAIN;
            continue;

        case PM_EQUIP_RESUME:
            /* Tail of the Equip case after EQUIP_MENU pops: single party plays
             * the close sfx and clears the indicator (assembly lines 576-583). */
            if ((game_state.player_controlled_party_count & 0xFF) == 1) {
                play_sfx(27);  /* SFX::MENU_OPEN_CLOSE */
                clear_battle_menu_character_indicator();
            }
            st->phase = PM_MAIN;
            continue;

        case PM_PSI_RESUME:
            /* Tail of the PSI case after PSI_MENU pops (assembly 562-572):
             * a used PSI closes the whole pause menu; otherwise the single-PSI
             * party plays the close sfx and clears the indicator. */
            if (mode_child_result() != 0) {
                st->phase = PM_CLEANUP;
                continue;
            }
            if (count_characters_with_psi() == 1) {
                play_sfx(27);  /* SFX::MENU_OPEN_CLOSE */
                clear_battle_menu_character_indicator();
            }
            st->phase = PM_MAIN;
            continue;

        case PM_CLEANUP:
            /* Assembly @CLEANUP_AND_CLOSE (lines 603-614) */
            clear_instant_printing();
            hide_hppp_windows();
            close_all_windows();

            /* Wait for entity fade to complete (assembly @WAIT_ENTITY_FADE) */
            st->phase = PM_DONE;
            return STEP_RESULT_PUSH(GAME_MODE_ENTITY_FADE_WAIT);

        case PM_DONE:
        default:
            enable_all_entities();
            return STEP_RESULT_POP(0);
        }
    }
}

/* OPEN_MENU_BUTTON — Port of asm/overworld/open_menu.asm.
 * Full pause menu: Talk to, Goods, PSI, Equip, Check, Status. Thin bridge over
 * GAME_MODE_PAUSE_MENU (run to completion via pump_mode while the overworld
 * post-input driver is still blocking). */
void open_menu_button(void) {
    ModeState init = { .pause_menu = { .phase = PM_ENTER } };
    pump_mode(GAME_MODE_PAUSE_MENU, &init);
}

/* OPEN_MENU_BUTTON_CHECKTALK — Port of asm/overworld/open_menu.asm lines 616-644.
 *
 * Quick talk/check: tries TALK_TO first, then CHECK if no talk result.
 * Falls back to MSG_SYS_NOPROBLEM ("Nothing problem here.").
 * Called on L button in the overworld. */
/* GAME_MODE_QUICK_CHECKTALK step — run-to-completion form of
 * open_menu_button_checktalk(). The dialogue (DISPLAY_TEXT) and the entity
 * fade-out wait (ENTITY_FADE_WAIT) are STEP_PUSHed so the quick talk/check lives
 * on the mode stack instead of holding this driver's frame on the C stack. */
StepResult mode_step_quick_checktalk(ModeState *ms) {
    QuickChecktalkState *st = &ms->quick_checktalk;

    switch ((QuickChecktalkPhase)st->phase) {
    case QCT_TEXT: {
        disable_all_entities();
        play_sfx(1);  /* SFX::CURSOR1 */

        /* Resolve target text: TALK_TO, then CHECK, then the fallback. */
        uint32_t text_ptr = talk_to();
        if (text_ptr == 0)
            text_ptr = check_action();
        if (text_ptr == 0)
            text_ptr = MSG_SYS_NOTHING_WRONG_HERE;

        st->phase = QCT_FADE;
        static ModeState dt_init;  /* outlives this dispatch (pump copies it) */
        if (dt_make_child_init(&dt_init, text_ptr))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &dt_init);
        LOG_WARN("WARNING: resolve_text_addr(0x%06X) returned NULL\n", text_ptr);
        return STEP_RESULT_CONTINUE();
    }

    case QCT_FADE:
        clear_instant_printing();
        hide_hppp_windows();
        close_all_windows();
        st->phase = QCT_DONE;
        return STEP_RESULT_PUSH(GAME_MODE_ENTITY_FADE_WAIT);

    case QCT_DONE:
    default:
        enable_all_entities();
        return STEP_RESULT_POP(0);
    }
}

/* OPEN_MENU_BUTTON_CHECKTALK — Port of asm/overworld/open_menu.asm lines 616-644.
 * Quick talk/check (L button in the overworld): tries TALK_TO first, then CHECK,
 * falling back to MSG_SYS_NOTHING_WRONG_HERE. Thin bridge over
 * GAME_MODE_QUICK_CHECKTALK (run to completion via pump_mode while overworld_post
 * is still a blocking driver). */
void open_menu_button_checktalk(void) {
    ModeState init = { .quick_checktalk = { .phase = QCT_TEXT } };
    pump_mode(GAME_MODE_QUICK_CHECKTALK, &init);
}

/* Port of OPEN_HPPP_DISPLAY (asm/text/open_hppp_display.asm).
 * Called when B/Select is pressed in the overworld.
 * Shows HP/PP windows and money, loops until dismissed.
 * A/L button opens full menu (OPEN_MENU_BUTTON).
 *
 * When compiled with -DEB_B_OPENS_MAIN_MENU, the wait loop is skipped: after
 * showing the HPPP/money windows, control falls straight into the main pause
 * menu. This lets the game be played with two action buttons (B + L). */
/* GAME_MODE_HPPP_DISPLAY step — run-to-completion form of open_hppp_display().
 * See HpppDisplayState in mode_stack.h. The full pause menu is STEP_PUSHed
 * (GAME_MODE_PAUSE_MENU owns ALL the cleanup — hide HPPP, close windows,
 * enable entities — so HD_MENU_DONE just pops). */
StepResult mode_step_hppp_display(ModeState *ms) {
    HpppDisplayState *st = &ms->hppp_display;
    static ModeState hd_child_init;  /* outlives the dispatch (pump copies it) */

    switch ((HpppDisplayPhase)st->phase) {
    case HD_ENTER:
        disable_all_entities();
        play_sfx(1);  /* SFX::CURSOR1 */

        /* SHOW_HPPP_AND_MONEY_WINDOWS
         * (asm/text/hp_pp_window/show_hppp_and_money_windows.asm) */
        show_hppp_windows();
        display_money_window();

#ifdef EB_B_OPENS_MAIN_MENU
        /* Skip the wait loop: fall straight into the main pause menu. */
        hd_child_init = (ModeState){ .pause_menu = { .phase = PM_ENTER } };
        st->phase = HD_MENU_DONE;
        return STEP_RESULT_PUSH_INIT(GAME_MODE_PAUSE_MENU, &hd_child_init);
#else
        st->phase = HD_TICK;
        st->primed = 0;
        /* fall through to the first render frame */
#endif
        /* FALLTHROUGH */

    case HD_TICK:
        /* First frame renders before any input is read (the blocking loop's
         * leading window_tick); thereafter the input acted on here is what the
         * pump's previous yield latched — same read-after-yield order. */
        if (!st->primed) {
            st->primed = 1;
            window_tick_work();
            return STEP_RESULT_CONTINUE();
        }

        if (platform_input_quit_requested())
            return STEP_RESULT_POP(0);  /* blocking `break` had no cleanup */

        /* A or L → open the full menu */
        if (core.pad1_pressed & PAD_CONFIRM) {
            hd_child_init = (ModeState){ .pause_menu = { .phase = PM_ENTER } };
            st->phase = HD_MENU_DONE;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_PAUSE_MENU, &hd_child_init);
        }

        /* B or Select → dismiss (assembly lines 17-26) */
        if (core.pad1_pressed & PAD_CANCEL) {
            play_sfx(2);  /* SFX::CURSOR2 */
            clear_instant_printing();
            hide_hppp_windows();
            close_all_windows();
            window_tick_work();
            st->phase = HD_EXIT;
            return STEP_RESULT_CONTINUE();
        }

        window_tick_work();
        return STEP_RESULT_CONTINUE();

    case HD_MENU_DONE:
        /* The pause menu did all cleanup; nothing left to do. */
        return STEP_RESULT_POP(0);

    case HD_EXIT:
    default:
        enable_all_entities();
        return STEP_RESULT_POP(0);
    }
}

/* Port of OPEN_HPPP_DISPLAY (asm/text/open_hppp_display.asm).
 * Called when B/Select is pressed in the overworld. Shows HP/PP windows and
 * money, waits until dismissed; A/L opens the full menu (the pause-menu mode).
 * With -DEB_B_OPENS_MAIN_MENU the wait is skipped and control falls straight
 * into the main pause menu (two-action-button play: B + L). Thin bridge over
 * GAME_MODE_HPPP_DISPLAY while overworld_post is still a blocking driver. */
void open_hppp_display(void) {
    ModeState init = { .hppp_display = { .phase = HD_ENTER } };
    pump_mode(GAME_MODE_HPPP_DISPLAY, &init);
}

void text_setup_bg3(void) {
    /* BG3SC: tilemap at word $7C00 */
    ppu.bg_sc[2] = 0x7C;

    /* BG34NBA: BG3 character data at word $6000 */
    ppu.bg_nba[1] = (ppu.bg_nba[1] & 0xF0) | 0x06;

    /* BG3 scroll to 0 */
    ppu.bg_hofs[2] = 0;
    ppu.bg_vofs[2] = 0;
}

/* --- Text output (stores text for VWF rendering in render_all_windows) --- */

/* Flag set by WRITE_CHAR_TO_WINDOW / CHECK_TEXT_FITS_IN_WINDOW when wrapping
 * inserts an auto-newline.  Consumed by VWF rendering for word-wrap indentation.
 * Port of VWF_INDENT_NEW_LINE BSS variable. */
uint8_t vwf_indent_new_line = 0;


/* (store_eb_text and compute_eb_pixel_width removed — text now rendered
 * immediately via vwf_render_character + vwf_flush_tiles_to_vram) */

/*
 * PRINT_EB_STRING — Renders EB-encoded characters immediately via VWF.
 *
 * Port of PRINT_LETTER (asm/text/print_letter.asm) → RENDER_VWF_CHARACTER
 * → FLUSH_VWF_TILES_TO_VRAM pipeline.
 *
 * For each character:
 *   1. check_text_fits_in_window (word-wrap)
 *   2. vwf_render_character → blit_vwf_glyph (into vwf_buffer)
 *   3. vwf_flush_tiles_to_vram → alloc tiles, upload VRAM, write to content_tilemap
 *   4. Update cursor_pixel_x
 */
void print_eb_string(const uint8_t *eb_str, int len) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return;

    uint8_t font_id = (uint8_t)(w->font & 0xFF);

    for (int i = 0; i < len; i++) {
        uint8_t eb = eb_str[i];
        if (eb == 0x00) break;

        /* Word-wrap indent handling (RENDER_VWF_CHARACTER lines 42-61).
         * Only applies to normal chars (>= 0x50); special chars (0x20, 0x22,
         * 0x2F) bypass indent via the @PRINT_SPECIAL path (lines 20-30). */
        if (eb >= 0x50 && vwf_indent_new_line) {
            if (eb == 0x50) continue;  /* skip spaces after word-wrap */
            w->text_x = 0;
            if (eb != 0x70) {  /* not BULLET */
                set_text_pixel_position(w->text_y, 6);
                w = get_window(win.current_focus_window);
                if (!w) return;
                font_id = (uint8_t)(w->font & 0xFF);
            }
            vwf_indent_new_line = 0;
        }

        /* Assembly: STA LAST_PRINTED_CHARACTER at @SETUP_GLYPH (normal path only) */
        if (eb >= 0x50)
            dt.last_printed_character = eb;

        /* VWF render + flush to VRAM + write to per-window tilemap.
         * vwf_render_character handles @PRINT_SPECIAL internally for
         * chars 0x20, 0x22, 0x2F. */
        vwf_render_character(eb, font_id);

        /* Assembly: RENDER_VWF_CHARACTER calls FLUSH_VWF_TILES_TO_VRAM only for
         * normal chars (>= 0x50).  Special chars (0x20, 0x22, 0x2F) jump to @DONE
         * without flushing — advance_vwf_tile already handled tile alloc.
         * Flushing after a special char would allocate an extra stale VWF tile. */
        if (eb >= 0x50)
            vwf_flush_tiles_to_vram();

        /* Update cursor pixel position */
        w = get_window(win.current_focus_window);
        if (!w) return;
        w->cursor_pixel_x = vwf_x;
    }
}

/*
 * PRINT_STRING — Converts ASCII to EB codes and renders via VWF.
 *
 * Handles embedded newlines by calling print_newline().
 * Each character is rendered immediately (same pipeline as print_eb_string).
 */
void print_string(const char *str) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return;

    uint8_t font_id = (uint8_t)(w->font & 0xFF);

    for (const char *p = str; *p; p++) {
        if (*p == '\n') {
            print_newline();
            w = get_window(win.current_focus_window);
            if (!w) return;
            continue;
        }

        /* Detect embedded EB special characters (e.g., CHAR::EQUIPPED = 0x22).
         * These are stored directly in label buffers by equip menu code.
         * Pass through to vwf_render_character which handles @PRINT_SPECIAL. */
        uint8_t c = (uint8_t)*p;
        uint8_t eb;
        if (c == EB_CHAR_EQUIPPED) {
            eb = c;  /* bypass ASCII→EB conversion */
        } else {
            eb = ascii_to_eb_char(*p);
        }

        /* Word-wrap indent handling (same as print_eb_string) */
        if (eb >= 0x50 && vwf_indent_new_line) {
            if (eb == 0x50) continue;
            w->text_x = 0;
            if (eb != 0x70) {
                set_text_pixel_position(w->text_y, 6);
                w = get_window(win.current_focus_window);
                if (!w) return;
                font_id = (uint8_t)(w->font & 0xFF);
            }
            vwf_indent_new_line = 0;
        }

        if (eb >= 0x50)
            dt.last_printed_character = eb;

        vwf_render_character(eb, font_id);

        /* Skip flush for special chars — same guard as print_eb_string */
        if (eb >= 0x50)
            vwf_flush_tiles_to_vram();

        w = get_window(win.current_focus_window);
        if (!w) return;
        w->cursor_pixel_x = vwf_x;
    }
}

void print_number(int value, int min_digits) {
    /* Port of PRINT_NUMBER (asm/text/print_number.asm).
     * Clamps to [0, 9999999], reads window number_padding for
     * right-alignment, then prints digits via print_string. */
    if (value > 9999999) value = 9999999;
    if (value < 0) value = 0;

    char buf[12];
    int digit_count = snprintf(buf, sizeof(buf), "%d", value);

    /* Assembly lines 49-69: read number_padding from focus window.
     * Bit 7 set (default 128) = padding disabled.
     * Otherwise: min_width = (padding & 0x0F) + 1, advance cursor by
     * (min_width - digit_count) * 6 pixels before printing. */
    WindowInfo *w = get_window(win.current_focus_window);
    if (w && !(w->number_padding & 0x80)) {
        int min_width = (w->number_padding & 0x0F) + 1;
        int pad_count = min_width - digit_count;
        /* Assembly ALWAYS calls ADVANCE_TEXT_CURSOR_PIXELS here, even when
         * pad_count <= 0 (result is 0 pixels).  The call triggers
         * advance_vwf_tile() + VWF buffer clear via SET_TEXT_PIXEL_POSITION,
         * which is necessary for correct VWF tile state. */
        int effective = (pad_count > 0) ? pad_count : 0;
        uint16_t pad_pixels = (uint16_t)(effective * 6);
        set_text_pixel_position(w->text_y, w->cursor_pixel_x + pad_pixels);
    } else if (min_digits > digit_count) {
        /* No window padding — fallback to caller-specified min_digits */
        snprintf(buf, sizeof(buf), "%*d", min_digits, value);
    }

    print_string(buf);
}

void clear_window_text(uint16_t window_id) {
    WindowInfo *w = get_window(window_id);
    if (!w) return;

    /* Free all tiles in per-window content_tilemap and fill with 0 */
    uint16_t content_width = w->width - 2;
    uint16_t interior_tile_rows = w->height - 2;
    uint16_t total = content_width * interior_tile_rows;
    if (total > w->content_tilemap_size) total = w->content_tilemap_size;

    for (uint16_t i = 0; i < total; i++) {
        free_tile_safe(w->content_tilemap[i]);
        w->content_tilemap[i] = 0;
    }

    /* Also clear win.bg2_buffer area so next render doesn't show stale data */
    uint16_t *tilemap = (uint16_t *)win.bg2_buffer;
    for (uint16_t ty = 0; ty < w->height; ty++) {
        for (uint16_t tx = 0; tx < w->width; tx++) {
            uint16_t map_x = w->x + tx;
            uint16_t map_y = w->y + ty;
            if (map_x < 32 && map_y < 32) {
                tilemap[map_y * 32 + map_x] = 0;
            }
        }
    }
}

/* --- EarthBound character encoding ---
 *
 * Derived from the EBTEXT macro in include/macros.asm.
 * EB character codes occupy 0x50-0xAF (96 glyphs).
 * The mapping is NOT a simple offset — several punctuation
 * characters are reordered relative to ASCII.
 */

char eb_char_to_ascii(uint8_t eb_char) {
    if (eb_char == 0x00) return '\0';
    if (eb_char < 0x50 || eb_char > 0xAF) return '?';

    /* 96-entry table: EB codes 0x50-0xAF → ASCII */
    static const char table[96] = {
        ' ', '!', '&', '{', '$', '%', '}','\'',  /* 0x50-0x57 */
        '(', ')', '*', '+', ',', '-', '.', '/',   /* 0x58-0x5F */
        '0', '1', '2', '3', '4', '5', '6', '7',  /* 0x60-0x67 */
        '8', '9', ':', ';', '<', '=', '>', '?',   /* 0x68-0x6F */
        '@', 'A', 'B', 'C', 'D', 'E', 'F', 'G',  /* 0x70-0x77 */
        'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',  /* 0x78-0x7F */
        'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W',  /* 0x80-0x87 */
        'X', 'Y', 'Z', '~', '^', '[', ']', '#',   /* 0x88-0x8F */
        '_', 'a', 'b', 'c', 'd', 'e', 'f', 'g',   /* 0x90-0x97 */
        'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',   /* 0x98-0x9F */
        'p', 'q', 'r', 's', 't', 'u', 'v', 'w',   /* 0xA0-0xA7 */
        'x', 'y', 'z', '?', '|', '?', '?', '?',   /* 0xA8-0xAF */
    };
    return table[eb_char - 0x50];
}

uint8_t ascii_to_eb_char(char ascii) {
    if (ascii == '\0') return 0x00;

    uint8_t c = (uint8_t)ascii;
    if (c < 0x20 || c > 0x7E) return 0x50; /* space for unmapped */

    /* 95-entry table: ASCII 0x20-0x7E → EB codes */
    static const uint8_t table[95] = {
        0x50,       /* 0x20 ' '  */
        0x51,       /* 0x21 '!'  */
        0x50,       /* 0x22 '"'  (unmapped → space) */
        0x8F,       /* 0x23 '#'  */
        0x54,       /* 0x24 '$'  */
        0x55,       /* 0x25 '%'  */
        0x52,       /* 0x26 '&'  */
        0x57,       /* 0x27 '\'' */
        0x58,       /* 0x28 '('  */
        0x59,       /* 0x29 ')'  */
        0x5A,       /* 0x2A '*'  */
        0x5B,       /* 0x2B '+'  */
        0x5C,       /* 0x2C ','  */
        0x5D,       /* 0x2D '-'  */
        0x5E,       /* 0x2E '.'  */
        0x5F,       /* 0x2F '/'  */
        0x60, 0x61, 0x62, 0x63, 0x64,  /* 0x30-0x34 '0'-'4' */
        0x65, 0x66, 0x67, 0x68, 0x69,  /* 0x35-0x39 '5'-'9' */
        0x6A,       /* 0x3A ':'  */
        0x6B,       /* 0x3B ';'  */
        0x6C,       /* 0x3C '<'  */
        0x6D,       /* 0x3D '='  */
        0x6E,       /* 0x3E '>'  */
        0x6F,       /* 0x3F '?'  */
        0x70,       /* 0x40 '@'  */
        0x71, 0x72, 0x73, 0x74, 0x75,  /* 0x41-0x45 'A'-'E' */
        0x76, 0x77, 0x78, 0x79, 0x7A,  /* 0x46-0x4A 'F'-'J' */
        0x7B, 0x7C, 0x7D, 0x7E, 0x7F,  /* 0x4B-0x4F 'K'-'O' */
        0x80, 0x81, 0x82, 0x83, 0x84,  /* 0x50-0x54 'P'-'T' */
        0x85, 0x86, 0x87, 0x88, 0x89,  /* 0x55-0x59 'U'-'Y' */
        0x8A,       /* 0x5A 'Z'  */
        0x8D,       /* 0x5B '['  */
        0x50,       /* 0x5C '\\' (unmapped → space) */
        0x8E,       /* 0x5D ']'  */
        0x8C,       /* 0x5E '^'  */
        0x90,       /* 0x5F '_'  */
        0x50,       /* 0x60 '`'  (unmapped → space) */
        0x91, 0x92, 0x93, 0x94, 0x95,  /* 0x61-0x65 'a'-'e' */
        0x96, 0x97, 0x98, 0x99, 0x9A,  /* 0x66-0x6A 'f'-'j' */
        0x9B, 0x9C, 0x9D, 0x9E, 0x9F,  /* 0x6B-0x6F 'k'-'o' */
        0xA0, 0xA1, 0xA2, 0xA3, 0xA4,  /* 0x70-0x74 'p'-'t' */
        0xA5, 0xA6, 0xA7, 0xA8, 0xA9,  /* 0x75-0x79 'u'-'y' */
        0xAA,       /* 0x7A 'z'  */
        0x53,       /* 0x7B '{'  */
        0xAC,       /* 0x7C '|'  */
        0x56,       /* 0x7D '}'  */
        0x8B,       /* 0x7E '~'  */
    };
    return table[c - 0x20];
}

/* --- VWF Engine --- */

/* VRAM tile index for next VWF allocation. Reset each frame. */
static uint16_t vwf_vram_next = 0x100;

void clear_vwf_indent_new_line(void) {
    vwf_indent_new_line = 0;
}

/* Print newline in the current focus window.
 * Port of PRINT_NEWLINE / REDIRECT_PRINT_NEWLINE
 * (asm/text/print_newline.asm, asm/text/print_newline_redirect.asm).
 * Resets text_x, advances text_y, resets VWF pixel position.
 *
 * Assembly: text_y is in "lines" (each line = 2 tile rows = 16px).
 * window_stats::height stores INTERIOR tile rows (config height - 2).
 * C port: WindowInfo.height stores FULL config height (including border).
 * Scroll check: text_y == (interior_height / 2) - 1. */
void print_newline(void) {
    if (win.current_focus_window == WINDOW_ID_NONE) return;
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return;

    /* Reset VWF state for new line (port of RESET_VWF_TEXT_STATE call) */
    vwf_init();

    /* Assembly scroll check: LDA height; LSR; DEC; CMP text_y; BEQ @SCROLL.
     * Assembly height = interior tile rows = config_height - 2.
     * Interior lines = (height - 2) / 2.  Scroll when text_y reaches last line. */
    uint16_t interior_lines = (w->height - 2) / 2;
    uint16_t max_text_y = interior_lines - 1;

    if (w->text_y >= max_text_y) {
        scroll_window_up(w);
    } else {
        w->text_y++;
    }
    w->text_x = 0;
    w->cursor_pixel_x = 0;
}

/* Check if a character will fit on the current window line.
 * If it won't fit, inserts a newline and sets vwf_indent_new_line flag.
 * Port of CHECK_TEXT_FITS_IN_WINDOW (asm/text/check_text_fits_in_window.asm).
 *
 * Assembly logic:
 *   current_pos = (text_x - 1) * 8 + (VWF_X & 7)   [= VWF_X]
 *   end_pos = current_pos + font_width + padding
 *   if end_pos > width * 8 → newline + set indent flag
 *
 * C port computes from text_x and vwf_x to match assembly exactly. */
void check_text_fits_in_window(uint16_t eb_char) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return;

    uint8_t glyph_index = (eb_char - 0x50) & 0x7F;
    /* Assembly always uses FONT_PTR_TABLE[0] = NORMAL font */
    uint8_t char_width = font_get_width(FONT_ID_NORMAL, glyph_index);
    uint16_t total_width = (uint16_t)char_width + character_padding;

    uint16_t current_pos = (uint16_t)((w->text_x - 1) * 8 + (vwf_x & 7));
    uint16_t end_pos = current_pos + total_width;
    uint16_t max_pixels = (w->width - 2) * 8;  /* Content width = display - 2 */

    if (end_pos > max_pixels) {
        /* Character doesn't fit — insert newline */
        print_newline();
        vwf_indent_new_line = 1;
    }
}

uint16_t get_string_pixel_width(const uint8_t *str, int16_t max_len) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return 0;
    uint8_t font_id = (uint8_t)(w->font & 0xFF);
    uint16_t total = 0;
    for (int i = 0; (max_len < 0 || i < max_len) && str[i] != 0x00; i++) {
        uint8_t glyph_index = (str[i] - 0x50) & 0x7F;
        uint8_t char_width = font_get_width(font_id, glyph_index);
        total += char_width + character_padding;
    }
    return total;
}

void print_string_with_wordwrap(const uint8_t *str, int16_t max_len) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return;

    uint16_t str_width = get_string_pixel_width(str, max_len);
    uint16_t current_pos = (uint16_t)((w->text_x - 1) * 8 + (vwf_x & 7));
    uint16_t end_pos = str_width + current_pos;
    uint16_t max_pixels = (w->width - 2) * 8;

    if (end_pos > max_pixels) {
        print_newline();
        vwf_indent_new_line = 1;
    }

    /* Print the string */
    int len = 0;
    if (max_len < 0) {
        while (str[len] != 0x00) len++;
    } else {
        while (len < max_len && str[len] != 0x00) len++;
    }
    print_eb_string(str, len);
}

void print_text_with_word_splitting(const uint8_t *str, int16_t max_len) {
    uint8_t word_buf[64];
    int buf_pos = 0;

    for (int i = 0; ; i++) {
        uint8_t ch = (max_len >= 0 && i >= max_len) ? 0x00 : str[i];
        word_buf[buf_pos] = ch;
        buf_pos++;

        if (ch == 0x50 || ch == 0x00) {
            /* Space or null = word boundary */
            if (ch == 0x50) {
                /* Include space, then null-terminate after it */
                word_buf[buf_pos] = 0x00;
            }
            /* ch == 0x00 already null-terminates */
            print_string_with_wordwrap(word_buf, -1);
            buf_pos = 0;

            if (ch == 0x00) break;
        }
    }
}

void vwf_init(void) {
    /* Port of RESET_VWF_TEXT_STATE (asm/text/vwf/reset_vwf_text_state.asm).
     * Assembly only clears 1 tile (32 bytes) at the start of VWF_BUFFER,
     * not the full 52-tile ert.buffer. */
    memset(vwf_buffer, 0xFF, VWF_TILE_BYTES);  /* clear first tile only */
    vwf_x = 0;
    vwf_tile = 0;
    vwf_pixels_rendered = 0;
    /* Clear text render state (assembly: CLEAR_TEXT_RENDER_STATE) */
    memset(&text_render_state, 0, sizeof(text_render_state));
}

void vwf_frame_reset(void) {
    /* Assembly renders titles once (at SET_WINDOW_TITLE time) into dedicated
     * VRAM starting at word address $7700, which corresponds to tile ID $02E0
     * relative to BG2 char base $6000.  The C port re-renders titles every
     * frame from render_all_windows, so we must use the same dedicated range
     * to avoid overwriting content tiles allocated by alloc_bg2_tilemap_entry
     * (which manages tiles 0-511 via the used_bg2_tile_map bitmap). */
    vwf_vram_next = 0x02E0;
}

void vwf_reserve_tiles(uint16_t count) {
    vwf_vram_next += count;
}

/*
 * ADVANCE_VWF_TILE: Port of asm/text/vwf/advance_vwf_tile.asm.
 * Advances VWF state past the current tile to a fresh tile boundary.
 * Called by SET_WINDOW_TEXT_POSITION when repositioning the cursor.
 *
 * Assembly:
 *   1. Increment VWF_TILE; if > 51 (VWF_BUFFER_TILES-1), wrap to 0 and reset VWF_X
 *   2. Otherwise: VWF_X = VWF_TILE * 8 (tile boundary)
 *   3. Clear TEXT_RENDER_STATE::upper_vram_position (force fresh alloc)
 *   4. Sync TEXT_RENDER_STATE::pixels_rendered = VWF_X
 */
void advance_vwf_tile(void) {
    vwf_tile++;
    if (vwf_tile > VWF_BUFFER_TILES - 1) {
        vwf_tile = 0;
        vwf_x = 0;
    } else {
        vwf_x = vwf_tile * 8;
    }
    text_render_state.upper_vram_position = 0;
    text_render_state.pixels_rendered = vwf_x;
}

void vwf_set_position(uint16_t pixel_x) {
    vwf_x = pixel_x;
    vwf_tile = pixel_x >> 3;
    vwf_pixels_rendered = pixel_x;
}

/*
 * Core VWF blit: render one column-strip of a glyph into the VWF ert.buffer.
 * Port of BLIT_VWF_GLYPH from asm/text/vwf/blit_vwf_glyph.asm.
 *
 * The assembly approach:
 *   1. Fill tile with 0xFF (both planes set → color 3 = opaque dark bg)
 *   2. AND inverted+shifted glyph into plane 1 only
 *   Result: glyph pixels → color 1, background → color 3 (opaque)
 *
 * VWF ert.buffer tile = 32 bytes = 8x16 px in 2bpp (upper 8x8 + lower 8x8).
 *   Bytes 0-15: upper tile (rows 0-7, plane 0/1 interleaved)
 *   Bytes 16-31: lower tile (rows 8-15, plane 0/1 interleaved)
 *
 * glyph_data: pointer to 1bpp glyph column (height bytes, top-to-bottom)
 * height: number of pixel rows (16 for NORMAL, 8 for TINY)
 * width: pixel width to advance cursor
 */
void blit_vwf_glyph(const uint8_t *glyph_data, uint8_t height, uint8_t width) {
    uint8_t pixel_offset = vwf_x & 7;
    uint16_t tile_idx = vwf_tile;

    if (tile_idx >= VWF_BUFFER_TILES) return;

    uint8_t *dst = vwf_buffer + tile_idx * VWF_TILE_BYTES;

    /* Fill tile with 0xFF if starting at tile boundary.
       Both planes = 1 → color 3 = opaque dark background. */
    if (pixel_offset == 0) {
        memset(dst, 0xFF, VWF_TILE_BYTES);
    }

    /* AND shifted glyph mask into plane 1 only.
       Glyph data is pre-inverted: 0 = drawn pixel, 1 = background.
       Where glyph bit is 0: plane 1 cleared → color 1 (text).
       Where glyph bit is 1: plane 1 stays 1 → color 3 (background).
       High bits introduced by right-shift must be filled with 1 (background). */
    uint8_t rows = (height > 16) ? 16 : height;
    /* Fill mask for high bits that are outside the glyph area after shifting */
    uint8_t high_fill = (uint8_t)(0xFF << (8 - pixel_offset)) & 0xFF;
    if (pixel_offset == 0) high_fill = 0;
    for (uint8_t row = 0; row < rows; row++) {
        uint8_t glyph_byte = glyph_data[row];
        uint8_t mask = (glyph_byte >> pixel_offset) | high_fill;
        dst[row * 2 + 1] &= mask;
    }

    /* Advance pixel position */
    uint16_t old_tile = tile_idx;
    vwf_x += width;
    if (vwf_x >= VWF_BUFFER_TILES * 8) {
        vwf_x -= VWF_BUFFER_TILES * 8;
    }
    uint16_t new_tile = vwf_x >> 3;
    vwf_tile = new_tile;

    /* If we crossed a tile boundary, handle overflow into next tile.
     * Assembly (blit_vwf_glyph.asm lines 102-155): ALWAYS fills the next
     * tile with 0xFF when crossing a boundary, then skips the overflow
     * glyph blit only if overflow_shift == 8 (i.e., pixel_offset == 0).
     * This ensures the next tile has clean opaque background even when
     * no glyph data overflows into it. */
    if (new_tile != old_tile) {
        if (new_tile >= VWF_BUFFER_TILES) return;

        uint8_t *dst2 = vwf_buffer + new_tile * VWF_TILE_BYTES;

        /* Fill next tile with 0xFF (assembly line 117: MEMSET16) */
        memset(dst2, 0xFF, VWF_TILE_BYTES);

        /* Blit overflow glyph data only if pixel_offset != 0
         * (assembly lines 119-120: CMP #8; BEQ @DONE skips when
         * overflow_shift == 8, i.e., pixel_offset == 0) */
        if (pixel_offset != 0) {
            uint8_t overflow_shift = 8 - pixel_offset;
            uint8_t low_fill = (uint8_t)((1 << overflow_shift) - 1);
            for (uint8_t row = 0; row < rows; row++) {
                uint8_t glyph_byte = glyph_data[row];
                uint8_t mask = (uint8_t)(glyph_byte << overflow_shift) | low_fill;
                dst2[row * 2 + 1] &= mask;
            }
        }
    }
}

void vwf_render_character(uint8_t eb_char, uint8_t font_id) {
    /* @PRINT_SPECIAL (render_vwf_character.asm lines 20-30):
     * Characters 0x2F, CHAR::EQUIPPED (0x22), and 0x20 are rendered as
     * direct tilemap tiles via PRINT_CHAR_WITH_SOUND + ADVANCE_VWF_TILE,
     * bypassing VWF glyph rendering.
     * Only active when rendering into a focus window. */
    if (eb_char == 0x2F || eb_char == EB_CHAR_EQUIPPED || eb_char == 0x20) {
        if (win.current_focus_window != WINDOW_ID_NONE) {
            print_char_with_sound((uint16_t)eb_char);
            advance_vwf_tile();
        }
        return;
    }

    if (eb_char < 0x50) return;

    uint8_t char_index = (eb_char - 0x50) & 0x7F;
    if (char_index >= FONT_CHAR_COUNT) return;

    const uint8_t *glyph = font_get_glyph(font_id, char_index);
    if (!glyph) return;

    uint8_t char_width = font_get_width(font_id, char_index);
    uint8_t height = font_get_height(font_id);
    /* Total advance = character width + inter-character padding (assembly: CHARACTER_PADDING) */
    uint8_t total_width = char_width + character_padding;

    /* For wide characters (> 8px), blit in 8px columns */
    while (total_width > 8) {
        blit_vwf_glyph(glyph, height, 8);
        glyph += height; /* advance to next column in glyph data */
        total_width -= 8;
    }

    /* Blit remaining column */
    if (total_width > 0) {
        blit_vwf_glyph(glyph, height, total_width);
    }
}

/*
 * UPLOAD_VWF_TILE_TO_VRAM — Port of asm/text/vwf/upload_vwf_tile_to_vram.asm.
 *
 * Uploads one VWF ert.buffer tile (32 bytes = upper 8x8 + lower 8x8) to VRAM
 * at the positions specified by upper_tile and lower_tile IDs.
 *
 * Parameters:
 *   vwf_tile_index: index into vwf_buffer (0-51)
 *   upper_tile: VRAM tile ID for the upper 8x8 half
 *   lower_tile: VRAM tile ID for the lower 8x8 half
 */
static void upload_vwf_tile_to_vram(uint16_t vwf_tile_index,
                                     uint16_t upper_tile,
                                     uint16_t lower_tile) {
    if (vwf_tile_index >= VWF_BUFFER_TILES) return;

    uint32_t tile_data_base = VRAM_TEXT_LAYER_TILES * 2;
    uint8_t *src = vwf_buffer + vwf_tile_index * VWF_TILE_BYTES;

    /* Upper 8x8 tile: first 16 bytes of VWF tile */
    uint32_t upper_off = tile_data_base + upper_tile * 16;
    if (upper_off + 16 <= VRAM_SIZE)
        memcpy(ppu.vram + upper_off, src, 16);

    /* Lower 8x8 tile: next 16 bytes of VWF tile */
    uint32_t lower_off = tile_data_base + lower_tile * 16;
    if (lower_off + 16 <= VRAM_SIZE)
        memcpy(ppu.vram + lower_off, src + 16, 16);
}

/*
 * VWF_FLUSH_TILES_TO_VRAM — Port of asm/text/vwf/flush_vwf_tiles_to_vram.asm.
 *
 * Called after each vwf_render_character(). Compares vwf_x with
 * text_render_state.pixels_rendered to determine if new tile columns
 * have been produced.
 *
 * For existing tiles (upper/lower_vram_position != 0): re-uploads VWF
 * ert.buffer data to the same VRAM positions (glyph was extended into them).
 *
 * For NEW tile columns: allocates tile pairs via alloc_bg2_tilemap_entry(),
 * uploads VWF data, calls write_char_to_window() to write to per-window
 * content_tilemap.
 *
 * Assembly flow:
 *   1. target_tile = VWF_X >> 3
 *   2. flushed_tile = pixels_rendered >> 3
 *   3. If existing tile allocated: re-upload at same position
 *   4. For each new tile column: alloc upper+lower, upload, write_char_to_window
 *   5. Update pixels_rendered = VWF_X
 */
void vwf_flush_tiles_to_vram(void) {
    TextRenderState *trs = &text_render_state;

    uint16_t target_tile = vwf_x >> 3;
    uint16_t flushed_tile = trs->pixels_rendered >> 3;

    /* Re-upload existing tile if one is allocated */
    if (trs->upper_vram_position != 0) {
        upload_vwf_tile_to_vram(flushed_tile,
                                trs->upper_vram_position,
                                trs->lower_vram_position);
    } else {
        /* No existing tile — back up one so the loop will process
         * from flushed_tile (assembly: DEC @VIRTUAL02, then loop check).
         * Assembly wraps 0 → 0xFFFF via unsigned decrement; the loop body
         * increments back (0xFFFF → 0) and processes the first tile. */
        flushed_tile--;
    }

    /* Allocate and upload new tile columns */
    while (flushed_tile != target_tile) {
        uint16_t upper_id = alloc_bg2_tilemap_entry();
        trs->upper_vram_position = upper_id;

        uint16_t lower_id = alloc_bg2_tilemap_entry();
        trs->lower_vram_position = lower_id;

        /* Advance to next VWF ert.buffer tile (with wrapping at 52) */
        flushed_tile++;
        if (flushed_tile >= VWF_BUFFER_TILES)
            flushed_tile = 0;

        upload_vwf_tile_to_vram(flushed_tile, upper_id, lower_id);

        /* Write to per-window content tilemap.
         * Assembly: WRITE_CHAR_TO_WINDOW with A=upper_id, X=lower_id.
         * This writes to the window's content_tilemap and advances text_x. */
        WindowInfo *w = get_window(win.current_focus_window);
        if (w) {
            uint16_t content_width = w->width - 2;

            /* Line wrapping (WRITE_CHAR_TO_WINDOW lines 41-65) */
            if (w->text_x >= content_width) {
                uint16_t interior_lines = (w->height - 2) / 2;
                uint16_t max_text_y = (interior_lines > 0) ? interior_lines - 1 : 0;

                if (w->text_y >= max_text_y) {
                    if (dt.allow_text_overflow) {
                        /* Assembly: BNE @UPDATE_CURSOR — skip tilemap write entirely,
                         * just reset text_x to 0 and leave text_y unchanged. */
                        w->text_x = 0;
                        continue;
                    }
                    scroll_window_up(w);
                } else {
                    w->text_y++;
                }
                w->text_x = 0;

                if (dt.enable_word_wrap)
                    vwf_indent_new_line = 1;
            }

            /* Compute content_tilemap position:
             * Each text line = 2 tile rows. Upper row at text_y*content_width*2,
             * lower row at text_y*content_width*2 + content_width.
             * Each position is text_x offset. */
            uint16_t row_base = w->text_y * content_width * 2;
            uint16_t upper_pos = row_base + w->text_x;
            uint16_t lower_pos = row_base + content_width + w->text_x;

            if (upper_pos < w->content_tilemap_size && lower_pos < w->content_tilemap_size) {
                /* Free old tiles at this position */
                free_tile_safe(w->content_tilemap[upper_pos]);
                free_tile_safe(w->content_tilemap[lower_pos]);

                /* Write new tile entries with attributes */
                w->content_tilemap[upper_pos] = upper_id + w->curr_tile_attributes;
                w->content_tilemap[lower_pos] = lower_id + w->curr_tile_attributes;
            }

            w->text_x++;
        }
    }

    /* Update pixels_rendered */
    trs->pixels_rendered = vwf_x;
}

/*
 * Flush one line of VWF ert.buffer to VRAM and write tilemap entries.
 * Allocates VRAM tiles from vwf_vram_next.
 *
 * For 16px fonts (is_tall): each VWF tile becomes 2 VRAM tiles (upper+lower),
 * and 2 tilemap rows are written.
 * For 8px fonts: each VWF tile becomes 1 VRAM tile, 1 tilemap row.
 */
static void vwf_flush_line(uint16_t x_tile, uint16_t y_tile, bool is_tall) {
    uint16_t tiles_used = (vwf_x + 7) >> 3;
    if (tiles_used == 0) return;
    if (tiles_used > VWF_BUFFER_TILES) tiles_used = VWF_BUFFER_TILES;

    /* Allocate VRAM tiles */
    uint16_t vram_per_col = is_tall ? 2 : 1;
    uint16_t vram_base = vwf_vram_next;
    vwf_vram_next += tiles_used * vram_per_col;

    /* Upload VWF ert.buffer tiles to VRAM */
    uint32_t tile_data_base = VRAM_TEXT_LAYER_TILES * 2;

    for (uint16_t t = 0; t < tiles_used; t++) {
        uint8_t *src = vwf_buffer + t * VWF_TILE_BYTES;

        if (is_tall) {
            /* Upper 8x8 tile: first 16 bytes of VWF tile */
            uint16_t upper_idx = vram_base + t * 2;
            uint32_t upper_off = tile_data_base + upper_idx * 16;
            if (upper_off + 16 <= VRAM_SIZE)
                memcpy(ppu.vram + upper_off, src, 16);

            /* Lower 8x8 tile: next 16 bytes of VWF tile */
            uint16_t lower_idx = vram_base + t * 2 + 1;
            uint32_t lower_off = tile_data_base + lower_idx * 16;
            if (lower_off + 16 <= VRAM_SIZE)
                memcpy(ppu.vram + lower_off, src + 16, 16);
        } else {
            /* Single 8x8 tile: first 16 bytes */
            uint16_t tile_idx = vram_base + t;
            uint32_t off = tile_data_base + tile_idx * 16;
            if (off + 16 <= VRAM_SIZE)
                memcpy(ppu.vram + off, src, 16);
        }
    }

    /* Write tilemap entries */
    uint16_t *tilemap = (uint16_t *)win.bg2_buffer;
    uint16_t attr = 0x2000; /* priority bit, palette 0 */

    for (uint16_t t = 0; t < tiles_used; t++) {
        uint16_t mx = x_tile + t;
        if (mx >= 32) break;

        if (is_tall) {
            uint16_t upper_idx = vram_base + t * 2;
            uint16_t lower_idx = vram_base + t * 2 + 1;
            if (y_tile < 32)
                tilemap[y_tile * 32 + mx] = upper_idx | attr;
            if (y_tile + 1 < 32)
                tilemap[(y_tile + 1) * 32 + mx] = lower_idx | attr;
        } else {
            uint16_t tile_idx = vram_base + t;
            if (y_tile < 32)
                tilemap[y_tile * 32 + mx] = tile_idx | attr;
        }
    }
}

void vwf_render_string_at(const char *text, uint16_t x_tile, uint16_t y_tile,
                           uint8_t font_id) {
    /* Save VWF state — title rendering must not corrupt in-progress
     * content text rendering (assembly renders titles once at set_window_title
     * time, but C port re-renders every frame from render_all_windows). */
    memcpy(vwf_saved_buffer, vwf_buffer, VWF_BUFFER_SIZE);
    uint16_t saved_x = vwf_x;
    uint16_t saved_tile = vwf_tile;
    uint16_t saved_pixels = vwf_pixels_rendered;
    TextRenderState saved_trs = text_render_state;

    uint8_t height = font_get_height(font_id);
    bool is_tall = (height > 8);
    uint16_t line_advance = is_tall ? 2 : 1;
    uint16_t start_x = x_tile;
    uint16_t cur_y = y_tile;

    vwf_init();

    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            /* Flush current line */
            vwf_flush_line(start_x, cur_y, is_tall);
            cur_y += line_advance;
            vwf_init();
            continue;
        }

        uint8_t eb = ascii_to_eb_char(text[i]);
        vwf_render_character(eb, font_id);
    }

    /* Flush final line */
    vwf_flush_line(start_x, cur_y, is_tall);

    /* Restore VWF state */
    memcpy(vwf_buffer, vwf_saved_buffer, VWF_BUFFER_SIZE);
    vwf_x = saved_x;
    vwf_tile = saved_tile;
    vwf_pixels_rendered = saved_pixels;
    text_render_state = saved_trs;
}

void vwf_render_eb_string_at(const uint8_t *eb_str, int len, uint16_t x_tile,
                              uint16_t y_tile, uint8_t font_id,
                              uint8_t pixel_offset) {
    /* Save VWF state (same rationale as vwf_render_string_at). */
    memcpy(vwf_saved_buffer, vwf_buffer, VWF_BUFFER_SIZE);
    uint16_t saved_x = vwf_x;
    uint16_t saved_tile = vwf_tile;
    uint16_t saved_pixels = vwf_pixels_rendered;
    TextRenderState saved_trs = text_render_state;

    uint8_t height = font_get_height(font_id);
    bool is_tall = (height > 8);

    vwf_init();
    if (pixel_offset) {
        vwf_set_position(pixel_offset);
    }

    for (int i = 0; i < len && eb_str[i] != 0; i++) {
        vwf_render_character(eb_str[i], font_id);
    }

    vwf_flush_line(x_tile, y_tile, is_tall);

    /* Restore VWF state */
    memcpy(vwf_buffer, vwf_saved_buffer, VWF_BUFFER_SIZE);
    vwf_x = saved_x;
    vwf_tile = saved_tile;
    vwf_pixels_rendered = saved_pixels;
    text_render_state = saved_trs;
}

int vwf_render_to_fixed_tiles(const uint8_t *eb_str, int len, uint8_t font_id,
                               uint16_t vram_tile_base) {
    uint8_t height = font_get_height(font_id);
    bool is_tall = (height > 8);

    vwf_init();

    for (int i = 0; i < len && eb_str[i] != 0; i++) {
        vwf_render_character(eb_str[i], font_id);
    }

    uint16_t tiles_used = (vwf_x + 7) >> 3;
    if (tiles_used == 0) return 0;
    if (tiles_used > VWF_BUFFER_TILES) tiles_used = VWF_BUFFER_TILES;

    /* Upload VWF ert.buffer tiles to VRAM at the caller-specified tile range,
       matching RENDER_KEYBOARD_INPUT_CHARACTER's @UPLOAD_TILE loop. */
    uint32_t tile_data_base = VRAM_TEXT_LAYER_TILES * 2;

    for (uint16_t t = 0; t < tiles_used; t++) {
        uint8_t *src = vwf_buffer + t * VWF_TILE_BYTES;

        if (is_tall) {
            uint16_t upper_idx = vram_tile_base + t * 2;
            uint32_t upper_off = tile_data_base + upper_idx * 16;
            if (upper_off + 16 <= VRAM_SIZE)
                memcpy(ppu.vram + upper_off, src, 16);

            uint16_t lower_idx = vram_tile_base + t * 2 + 1;
            uint32_t lower_off = tile_data_base + lower_idx * 16;
            if (lower_off + 16 <= VRAM_SIZE)
                memcpy(ppu.vram + lower_off, src + 16, 16);
        } else {
            uint16_t tile_idx = vram_tile_base + t;
            uint32_t off = tile_data_base + tile_idx * 16;
            if (off + 16 <= VRAM_SIZE)
                memcpy(ppu.vram + off, src, 16);
        }
    }

    return (int)tiles_used;
}

int render_title_to_vram(const char *title, uint8_t title_slot) {
    if (!title || title[0] == '\0' || title_slot == 0) return 0;

    /* Convert ASCII title to EB encoding */
    uint8_t eb_str[WINDOW_TITLE_SIZE];
    int len = 0;
    for (int i = 0; title[i] != '\0' && i < WINDOW_TITLE_SIZE - 1; i++) {
        eb_str[len++] = ascii_to_eb_char(title[i]);
    }

    /* Save VWF state — set_window_title can be called while other windows
     * have in-progress VWF rendering state. */
    memcpy(vwf_saved_buffer, vwf_buffer, VWF_BUFFER_SIZE);
    uint16_t saved_x = vwf_x;
    uint16_t saved_tile = vwf_tile;
    uint16_t saved_pixels = vwf_pixels_rendered;
    TextRenderState saved_trs = text_render_state;

    /* Render to fixed VRAM tiles: 0x02E0 + (slot-1)*16
     * Matches assembly RENDER_WINDOW_TITLE → RENDER_TINY_FONT_STRING. */
    uint16_t vram_tile_base = 0x02E0 + (title_slot - 1) * 16;
    int tile_count = vwf_render_to_fixed_tiles(eb_str, len, FONT_ID_TINY, vram_tile_base);

    /* Restore VWF state */
    memcpy(vwf_buffer, vwf_saved_buffer, VWF_BUFFER_SIZE);
    vwf_x = saved_x;
    vwf_tile = saved_tile;
    vwf_pixels_rendered = saved_pixels;
    text_render_state = saved_trs;

    return tile_count;
}

/*
 * INITIALIZE_WINDOW_FLAVOUR_PALETTE — Port of asm/text/window/initialize_window_flavour_palette.asm.
 *
 * Resets all party characters' hp_pp_window_options to 0x0400 (normal mode),
 * then loads sub-palette 5 (byte offset +40 = uint16 index +20) from the
 * current text flavour's palette data into palette sub-palette 3 (colors 12-15).
 *
 * Assembly flow:
 *   1. Loop i=0..PLAYER_CHAR_COUNT-1: party_characters[i].hp_pp_window_options = 0x0400
 *   2. Read TEXT_WINDOW_PROPERTIES[(text_flavour-1)*3] to get byte offset into flavour data
 *   3. Add 40 to get sub-palette 5 source
 *   4. MEMCPY16 BPP2PALETTE_SIZE (8 bytes = 4 colors) → PALETTES + BPP2PALETTE_SIZE*3
 *   5. Set PALETTE_UPLOAD_MODE = FULL, REDRAW_ALL_WINDOWS = 1
 */
void initialize_window_flavour_palette(void) {
    /* Step 1: Reset all hp_pp_window_options to 0x0400 (normal) */
    for (int i = 0; i < 4; i++) {
        party_characters[i].hp_pp_window_options = 0x0400;
    }

    /* Ensure flavour palette data is loaded */
    if (!flavour_palettes) {
        const uint8_t *pal_data = ASSET_DATA(ASSET_GRAPHICS_TEXT_WINDOW_FLAVOUR_PALETTES_PAL);
        if (pal_data) {
            flavour_palettes = (const uint16_t *)pal_data;
        } else {
            return;
        }
    }

    /* Step 2-3: text_flavour is 1-indexed (1=Plain..5=Peanut), matching assembly.
     * TEXT_WINDOW_PROPERTIES gives byte offsets per flavour into the palette data.
     * Assembly DECs to 0-index before table lookup.
     * For US: flavour N (0-indexed) → byte offset N*64 → uint16 index N*32.
     * Sub-palette 5 = +40 bytes = +20 uint16 entries. */
    uint8_t flavour_raw = game_state.text_flavour;
    if (flavour_raw == 0 || flavour_raw > 5) return;
    uint8_t flavour_idx = flavour_raw - 1;  /* assembly DEC */

    /* Step 4: Copy 4 colors (BPP2PALETTE_SIZE = 8 bytes) to palette sub-palette 3 */
    memcpy(&ert.palettes[12], &flavour_palettes[flavour_idx * 32 + 20], 4 * sizeof(uint16_t));

    /* Step 5: Trigger palette upload and window redraw */
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    ow.redraw_all_windows = 1;
}

/*
 * RESET_HPPP_OPTIONS_AND_LOAD_PALETTE — Port of asm/text/hp_pp_window/reset_hppp_options_and_load_palette.asm.
 *
 * Nearly identical to initialize_window_flavour_palette but loads sub-palette 3
 * (byte offset +24 = uint16 index +12) instead of sub-palette 5 (offset +40).
 *
 * Assembly: same structure as C19CDD but ADC #24 instead of ADC #40.
 */
void reset_hppp_options_and_load_palette(void) {
    /* Reset all hp_pp_window_options to 0x0400 (normal) */
    for (int i = 0; i < 4; i++) {
        party_characters[i].hp_pp_window_options = 0x0400;
    }

    if (!flavour_palettes) {
        const uint8_t *pal_data = ASSET_DATA(ASSET_GRAPHICS_TEXT_WINDOW_FLAVOUR_PALETTES_PAL);
        if (pal_data) {
            flavour_palettes = (const uint16_t *)pal_data;
        } else {
            return;
        }
    }

    uint8_t flavour_raw = game_state.text_flavour;  /* assembly: 1-indexed */
    if (flavour_raw == 0 || flavour_raw > 5) return;
    uint8_t flavour_idx = flavour_raw - 1;  /* assembly DEC */

    /* Sub-palette 3 = +24 bytes = +12 uint16 entries */
    memcpy(&ert.palettes[12], &flavour_palettes[flavour_idx * 32 + 12], 4 * sizeof(uint16_t));

    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    ow.redraw_all_windows = 1;
}
