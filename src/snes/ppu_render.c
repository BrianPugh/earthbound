#include "snes/ppu.h"
#include "game/battle_bg.h"
#include "include/binary.h"
#include "platform/platform.h"
#include <string.h>
#include <stdio.h>

/* Embedded ports can define these at build time to optimize rendering:
 *
 *   PPU_RAM_SECTION — section name for placing hot functions in fast memory.
 *     Examples: ".time_critical" (RP2040), ".dtcm" (STM32 DTCM-RAM),
 *              ".iram1" (ESP32).  Eliminates instruction cache misses
 *              for the decode + scanline render inner loops (~3KB).
 *
 *   PPU_FORCE_SPEED_OPT — when defined, forces -O3 for this file even when
 *     the library is compiled with -Os.  Useful when the port uses -Os
 *     globally to reduce code size / cache pressure, but still wants
 *     maximum speed for the PPU rendering hot path. */
#ifdef PPU_FORCE_SPEED_OPT
#pragma GCC optimize("O3")
#endif

#ifdef PPU_RAM_SECTION
#define PPU_HOT_FUNC(name) \
    __attribute__((section(PPU_RAM_SECTION "." #name))) name
#else
#define PPU_HOT_FUNC(name) name
#endif

#ifdef PPU_PROFILE
PPUProfile ppu_profile;
#define PROF_SECTION(name) uint64_t _prof_##name = platform_timer_ticks()
#define PROF_END(name, acc) (acc) += (uint32_t)(platform_timer_ticks() - _prof_##name)
#else
#define PROF_SECTION(name) ((void)0)
#define PROF_END(name, acc) ((void)0)
#endif

/* Layer bit masks for TM/TS registers */
#define LAYER_BG1  0x01
#define LAYER_BG2  0x02
#define LAYER_BG3  0x04
#define LAYER_BG4  0x08
#define LAYER_OBJ  0x10

/* Tile row cache entry for bitplane decode caching. */
typedef struct {
    uint32_t key;       /* tile_addr | (pixel_y << 17), 0xFFFFFFFF = invalid */
    uint8_t indices[8]; /* decoded palette indices for this tile row */
} TileRowCacheEntry;

/* Context for merge-during-render: BG layers write directly into the
 * merged priority buffer instead of per-layer intermediate arrays.
 * Eliminates the separate merge pass and per-layer PixelInfo buffers.
 *
 * main_gp and main_lmask are packed into a single uint16_t array:
 *   low byte  = global priority (gp)
 *   high byte = layer bitmask (lmask)
 * This halves the number of stores per winning pixel and reduces register
 * pressure on the M0+ (7 pointer fields → 6). */
typedef struct {
    uint16_t *main_color;   /* merged main screen color buffer */
    uint16_t *main_gp_lm;  /* packed gp (low) + lmask (high) */
    uint16_t *sub_color;    /* merged sub screen color (NULL = no sub) */
    uint8_t  *sub_gp;       /* merged sub screen global priority (NULL = no sub) */
    const uint8_t *tm_line; /* per-pixel main screen mask (always valid, never NULL) */
    const uint8_t *ts_line; /* per-pixel sub screen mask (always valid when sub active) */
    uint16_t gp0_lm, gp1_lm; /* packed gp|layer_bit<<8 for tile prio 0 and 1 */
    uint8_t layer_bit;       /* layer bitmask (LAYER_BG1..BG4) — for window checks */
    TileRowCacheEntry *tile_cache; /* per-context tile cache (may be in fast SRAM) */
} BGRenderCtx;

/* Helper to extract gp and lmask from packed main_gp_lm value */
#define GP_LM_GP(v)    ((uint8_t)(v))
#define GP_LM_LM(v)    ((uint8_t)((v) >> 8))

/* Shadow palette: SNES BGR555 converted to BGR565 once per frame.
 * Eliminates per-pixel bgr555_to_pixel() calls during rendering.
 * BGR555→BGR565 is a single shift (same channel order, widen green). */
static uint16_t cgram_render[256];

/* Pre-build the shadow palette from CGRAM.  Call before signaling
 * core 1 in dual-core mode to avoid a data race on cgram_render[]. */
void ppu_prepare_palette(void) {
    for (int i = 0; i < 256; i++) {
        uint16_t c = ppu.cgram[i];
        cgram_render[i] = bgr555_to_pixel(c);
    }
}

/* Tile row decode cache: avoids redundant bitplane decoding.
 * An 8×8 tile row is identical across 8 consecutive scanlines, and the same
 * tile may repeat horizontally or across BG layers.  Direct-mapped with 256
 * entries keyed on (tile_addr, pixel_y).  Cleared once per frame.
 *
 * Hit rate is typically >85% (7/8 vertical + horizontal/cross-layer hits).
 * On cache hit, decode cost drops from ~200 cycles to ~7 cycles (key
 * compare + pointer return).  Net savings: ~14% of total frame time. */
#define TILE_CACHE_BITS  8
#define TILE_CACHE_SIZE  (1 << TILE_CACHE_BITS)
#define TILE_CACHE_MASK  (TILE_CACHE_SIZE - 1)
#define TILE_CACHE_EMPTY 0xFFFFFFFF

static TileRowCacheEntry tile_row_cache[PPU_NUM_RENDER_CONTEXTS][TILE_CACHE_SIZE];

/* Decode a single 2bpp tile row (8 pixels) — unrolled, no loop.
 * Not force-inlined: keeping as regular functions reduces render_bg_scanline
 * code size to fit within the 16KB RP2040 XIP cache alongside other hot code. */
static void PPU_HOT_FUNC(decode_2bpp_row)(const uint8_t *tile_data, int row,
                            uint8_t *out_indices) {
    const uint8_t *row_data = tile_data + row * 2;
    uint8_t b0 = row_data[0];
    uint8_t b1 = row_data[1];
    out_indices[0] = (b0 >> 7)       | ((b1 >> 6) & 2);
    out_indices[1] = ((b0 >> 6) & 1) | ((b1 >> 5) & 2);
    out_indices[2] = ((b0 >> 5) & 1) | ((b1 >> 4) & 2);
    out_indices[3] = ((b0 >> 4) & 1) | ((b1 >> 3) & 2);
    out_indices[4] = ((b0 >> 3) & 1) | ((b1 >> 2) & 2);
    out_indices[5] = ((b0 >> 2) & 1) | ((b1 >> 1) & 2);
    out_indices[6] = ((b0 >> 1) & 1) | (b1 & 2);
    out_indices[7] = (b0 & 1)        | ((b1 << 1) & 2);
}

static void PPU_HOT_FUNC(decode_4bpp_row)(const uint8_t *tile_data, int row,
                            uint8_t *out_indices) {
    const uint8_t *bp01 = tile_data + row * 2;
    const uint8_t *bp23 = tile_data + 16 + row * 2;
    uint8_t b0 = bp01[0], b1 = bp01[1];
    uint8_t b2 = bp23[0], b3 = bp23[1];
    out_indices[0] = (b0 >> 7)       | ((b1 >> 6) & 2) | ((b2 >> 5) & 4) | ((b3 >> 4) & 8);
    out_indices[1] = ((b0 >> 6) & 1) | ((b1 >> 5) & 2) | ((b2 >> 4) & 4) | ((b3 >> 3) & 8);
    out_indices[2] = ((b0 >> 5) & 1) | ((b1 >> 4) & 2) | ((b2 >> 3) & 4) | ((b3 >> 2) & 8);
    out_indices[3] = ((b0 >> 4) & 1) | ((b1 >> 3) & 2) | ((b2 >> 2) & 4) | ((b3 >> 1) & 8);
    out_indices[4] = ((b0 >> 3) & 1) | ((b1 >> 2) & 2) | ((b2 >> 1) & 4) | (b3 & 8);
    out_indices[5] = ((b0 >> 2) & 1) | ((b1 >> 1) & 2) | (b2 & 4)        | ((b3 << 1) & 8);
    out_indices[6] = ((b0 >> 1) & 1) | (b1 & 2)        | ((b2 << 1) & 4) | ((b3 << 2) & 8);
    out_indices[7] = (b0 & 1)        | ((b1 << 1) & 2) | ((b2 << 2) & 4) | ((b3 << 3) & 8);
}

static void PPU_HOT_FUNC(decode_8bpp_row)(const uint8_t *tile_data, int row,
                            uint8_t *out_indices) {
    const uint8_t *bp01 = tile_data + row * 2;
    const uint8_t *bp23 = tile_data + 16 + row * 2;
    const uint8_t *bp45 = tile_data + 32 + row * 2;
    const uint8_t *bp67 = tile_data + 48 + row * 2;
    uint8_t b0 = bp01[0], b1 = bp01[1];
    uint8_t b2 = bp23[0], b3 = bp23[1];
    uint8_t b4 = bp45[0], b5 = bp45[1];
    uint8_t b6 = bp67[0], b7 = bp67[1];
    out_indices[0] = (b0 >> 7)       | ((b1 >> 6) & 2) | ((b2 >> 5) & 4) | ((b3 >> 4) & 8)
                   | ((b4 >> 3) & 0x10) | ((b5 >> 2) & 0x20) | ((b6 >> 1) & 0x40) | (b7 & 0x80);
    out_indices[1] = ((b0 >> 6) & 1) | ((b1 >> 5) & 2) | ((b2 >> 4) & 4) | ((b3 >> 3) & 8)
                   | ((b4 >> 2) & 0x10) | ((b5 >> 1) & 0x20) | (b6 & 0x40) | ((b7 << 1) & 0x80);
    out_indices[2] = ((b0 >> 5) & 1) | ((b1 >> 4) & 2) | ((b2 >> 3) & 4) | ((b3 >> 2) & 8)
                   | ((b4 >> 1) & 0x10) | (b5 & 0x20) | ((b6 << 1) & 0x40) | ((b7 << 2) & 0x80);
    out_indices[3] = ((b0 >> 4) & 1) | ((b1 >> 3) & 2) | ((b2 >> 2) & 4) | ((b3 >> 1) & 8)
                   | (b4 & 0x10) | ((b5 << 1) & 0x20) | ((b6 << 2) & 0x40) | ((b7 << 3) & 0x80);
    out_indices[4] = ((b0 >> 3) & 1) | ((b1 >> 2) & 2) | ((b2 >> 1) & 4) | (b3 & 8)
                   | ((b4 << 1) & 0x10) | ((b5 << 2) & 0x20) | ((b6 << 3) & 0x40) | ((b7 << 4) & 0x80);
    out_indices[5] = ((b0 >> 2) & 1) | ((b1 >> 1) & 2) | (b2 & 4) | ((b3 << 1) & 8)
                   | ((b4 << 2) & 0x10) | ((b5 << 3) & 0x20) | ((b6 << 4) & 0x40) | ((b7 << 5) & 0x80);
    out_indices[6] = ((b0 >> 1) & 1) | (b1 & 2) | ((b2 << 1) & 4) | ((b3 << 2) & 8)
                   | ((b4 << 3) & 0x10) | ((b5 << 4) & 0x20) | ((b6 << 5) & 0x40) | ((b7 << 6) & 0x80);
    out_indices[7] = (b0 & 1) | ((b1 << 1) & 2) | ((b2 << 2) & 4) | ((b3 << 3) & 8)
                   | ((b4 << 4) & 0x10) | ((b5 << 5) & 0x20) | ((b6 << 6) & 0x40) | ((b7 << 7) & 0x80);
}

/* Emit a run of pixels from a single decoded 8x8 tile row.
 * Writes directly to merged priority buffers via BGRenderCtx, applying
 * window masks and global priority comparison in one pass. */
static inline __attribute__((always_inline))
void emit_tile_run(
    uint16_t tile_num, int pixel_y, int start_px, int count,
    bool hflip, int screen_x, uint8_t prio_bit, uint8_t palette,
    int bpp, uint32_t tile_data_base, int bytes_per_tile,
    const BGRenderCtx *ctx)
{
    uint32_t tile_addr = tile_data_base + (uint32_t)tile_num * bytes_per_tile;
    if (tile_addr + bytes_per_tile > VRAM_SIZE) return;

    const uint8_t *tile_data = &ppu.vram[tile_addr];

    /* Quick blank-row check: if all bitplane bytes for this row are zero,
     * every pixel is transparent — skip decode + emit entirely. */
    {
        const uint8_t *bp = tile_data + pixel_y * 2;
        uint8_t any = bp[0] | bp[1];
        if (bpp >= 4) { any |= bp[16] | bp[17]; }
        if (bpp == 8) { any |= bp[32] | bp[33] | bp[48] | bp[49]; }
        if (!any) return;
    }

    /* Tile row cache lookup — avoids redundant decode across scanlines,
     * horizontal tile repeats, and cross-layer tile sharing. */
    uint32_t cache_key = tile_addr | ((uint32_t)pixel_y << 17);
    uint32_t cache_idx = ((tile_addr >> 1) ^ pixel_y) & TILE_CACHE_MASK;
    const uint8_t *indices;

    {
        TileRowCacheEntry *cache = ctx->tile_cache;
        if (cache[cache_idx].key == cache_key) {
            /* Cache hit */
            indices = cache[cache_idx].indices;
        } else {
            /* Cache miss — decode and store */
            uint8_t *dest = cache[cache_idx].indices;
            if (bpp == 8)
                decode_8bpp_row(tile_data, pixel_y, dest);
            else if (bpp == 4)
                decode_4bpp_row(tile_data, pixel_y, dest);
            else
                decode_2bpp_row(tile_data, pixel_y, dest);
            cache[cache_idx].key = cache_key;
            indices = dest;
        }
    }

    int pal_base = (bpp == 8) ? 0 : (bpp == 4) ? palette * 16 : palette * 4;
    uint16_t gp_lm = prio_bit ? ctx->gp1_lm : ctx->gp0_lm;
    uint8_t gp = GP_LM_GP(gp_lm);

    /* Inner pixel loop — split into sub/no-sub variants to eliminate the
     * per-pixel sub_gp NULL check.  The sub-screen path adds ~5 loads per
     * pixel; skipping it when unused saves ~15% of render_bg_scanline time.
     * tm_line is always valid (never NULL) — caller provides an all-0xFF
     * array when windows are inactive, eliminating the NULL check. */
#define EMIT_PIXELS(HAS_SUB) do { \
    for (int _i = 0; _i < count; _i++) { \
        int _px = hflip ? (7 - (start_px + _i)) : (start_px + _i); \
        uint8_t _cidx = indices[_px]; \
        if (_cidx == 0) continue; \
        uint16_t _rgb = (bpp == 8) ? cgram_render[_cidx] \
                                    : cgram_render[pal_base + _cidx]; \
        int _sx = screen_x + _i; \
        /* Main screen: window mask + priority check, packed store */ \
        if ((ctx->tm_line[_sx] & ctx->layer_bit) && \
            gp > GP_LM_GP(ctx->main_gp_lm[_sx])) { \
            ctx->main_gp_lm[_sx] = gp_lm; \
            ctx->main_color[_sx] = _rgb; \
        } \
        if (HAS_SUB) { \
            if ((ctx->ts_line[_sx] & ctx->layer_bit) && \
                gp > ctx->sub_gp[_sx]) { \
                ctx->sub_gp[_sx] = gp; \
                ctx->sub_color[_sx] = _rgb; \
            } \
        } \
    } \
} while(0)

    if (ctx->sub_gp)
        EMIT_PIXELS(1);
    else
        EMIT_PIXELS(0);
#undef EMIT_PIXELS
}

/* Render a single BG layer scanline, merging directly into the priority
 * buffers via BGRenderCtx.  render_width controls how many screen pixels
 * to process (may be VIEWPORT_WIDTH or SNES_WIDTH). */
static void PPU_HOT_FUNC(render_bg_scanline)(int bg_index, int scanline, int bpp,
                                         const BGRenderCtx *ctx, int render_width) {
    /* BG tilemap base address (word address from register, *2 for byte) */
    uint16_t sc_reg = ppu.bg_sc[bg_index];
    uint32_t tilemap_base = (uint32_t)(sc_reg & 0xFC) << 9;
    int sc_size_h = (sc_reg & 0x01) ? 64 : 32;
    int sc_size_v = (sc_reg & 0x02) ? 64 : 32;

    /* BG tile data base address */
    uint8_t nba;
    if (bg_index < 2)
        nba = ppu.bg_nba[0] >> (bg_index * 4);
    else
        nba = ppu.bg_nba[1] >> ((bg_index - 2) * 4);
    nba &= 0x0F;
    uint32_t tile_data_base = (uint32_t)nba * 0x2000;

    int bytes_per_tile = (bpp == 8) ? 64 : (bpp == 4) ? 32 : 16;

    /* Check for 16x16 tile mode */
    bool big_tiles = (ppu.bgmode >> (4 + bg_index)) & 1;
    int tile_size = big_tiles ? 16 : 8;
    int tile_mask = tile_size - 1;      /* 7 or 15 — replaces % tile_size */
    int tile_shift = big_tiles ? 4 : 3; /* log2(tile_size) — replaces / tile_size */

    /* Apply scroll offset */
    int scroll_x = ppu.bg_hofs[bg_index] & 0x3FF;
    int scroll_y = ppu.bg_vofs[bg_index] & 0x3FF;

    /* Per-scanline horizontal offset for BG2 (HDMA distortion emulation) */
    if (bg_index == 1 && bg2_distortion_active &&
        scanline >= 0 && scanline < BATTLEBG_MAX_SCANLINES) {
        scroll_x = (scroll_x + bg2_scanline_hoffset[scanline]) & 0x3FF;
    }

    int world_width = sc_size_h * 8;
    int world_height = sc_size_v * 8;
    /* World dimensions are always power of 2 (256 or 512).
     * Use bitmask instead of % to avoid M0+ software division (~40 cyc each). */
    int world_w_mask = world_width - 1;
    int world_h_mask = world_height - 1;
    int y = (scanline + scroll_y) & world_h_mask;

    /* Pre-compute Y-axis tile values (constant for entire scanline) */
    int tile_row_map = y >> tile_shift;
    int pixel_y_in_tile = y & tile_mask;

    /* Pre-compute vertical quadrant offset */
    uint32_t v_quadrant_offset = 0;
    int tile_row_local = tile_row_map;
    if (sc_size_v == 64 && tile_row_local >= 32) {
        v_quadrant_offset = 32 * 32 * 2;
        if (sc_size_h == 64) v_quadrant_offset += 32 * 32 * 2;
        tile_row_local -= 32;
    }

    /* Tile-column loop: tilemaps wrap naturally via % world_width. */
    int screen_x = 0;
    while (screen_x < render_width) {
        int world_x = (screen_x + scroll_x) & world_w_mask;
        int x_in_tile = world_x & tile_mask;
        int pixels_this_tile = tile_size - x_in_tile;
        if (screen_x + pixels_this_tile > render_width)
            pixels_this_tile = render_width - screen_x;

        /* Tile column in map coordinates */
        int tile_col_map = world_x >> tile_shift;

        /* Apply horizontal quadrant offset */
        uint32_t map_addr = tilemap_base + v_quadrant_offset;
        int tile_col_local = tile_col_map;
        if (sc_size_h == 64 && tile_col_local >= 32) {
            map_addr += 32 * 32 * 2;
            tile_col_local -= 32;
        }

        /* Read tilemap entry (2 bytes per entry) */
        uint32_t entry_addr = map_addr + (tile_row_local * 32 + tile_col_local) * 2;
        if (entry_addr + 1 >= VRAM_SIZE) {
            screen_x += pixels_this_tile;
            continue;
        }

        uint16_t entry = read_u16_le(&ppu.vram[entry_addr]);
        uint16_t tile_num = entry & 0x3FF;
        uint8_t palette = (entry >> 10) & 0x07;
        uint8_t prio_bit = (entry >> 13) & 1;
        bool hflip = (entry >> 14) & 1;
        bool vflip = (entry >> 15) & 1;

        if (!big_tiles) {
            /* 8x8 tile: decode once, emit up to 8 pixels */
            int eff_py = vflip ? (7 - pixel_y_in_tile) : pixel_y_in_tile;
            emit_tile_run(tile_num, eff_py, x_in_tile, pixels_this_tile,
                         hflip, screen_x, prio_bit, palette,
                         bpp, tile_data_base, bytes_per_tile, ctx);
        } else {
            /* 16x16 tile: split across up to 2 horizontal sub-tiles */
            int sub_y_screen = pixel_y_in_tile >> 3;
            int sub_pixel_y = pixel_y_in_tile & 7;
            int eff_sub_y = vflip ? (1 - sub_y_screen) : sub_y_screen;
            int eff_py = vflip ? (7 - sub_pixel_y) : sub_pixel_y;

            int local_x = x_in_tile;
            int remaining = pixels_this_tile;
            int out_x = screen_x;

            while (remaining > 0) {
                int sub_x_screen = local_x >> 3;
                int start_in_sub = local_x & 7;
                int sub_count = 8 - start_in_sub;
                if (sub_count > remaining) sub_count = remaining;

                int eff_sub_x = hflip ? (1 - sub_x_screen) : sub_x_screen;
                uint16_t sub_tile = tile_num + eff_sub_x + eff_sub_y * 16;

                emit_tile_run(sub_tile, eff_py, start_in_sub, sub_count,
                             hflip, out_x, prio_bit, palette,
                             bpp, tile_data_base, bytes_per_tile, ctx);

                out_x += sub_count;
                local_x += sub_count;
                remaining -= sub_count;
            }
        }

        screen_x += pixels_this_tile;
    }
}

/* Blend two BGR565 colors for color math (add/subtract, optionally halved).
 * All channels are 5-bit precision (green stored as g5<<6 in BGR565).
 * BGR565 layout: BBBBBGGGGGGRRRRR — R[4:0], G[10:6]@5bit, B[15:11]. */
static inline __attribute__((always_inline))
uint16_t blend_colors(uint16_t main_c, uint16_t sub_c,
                      bool subtract, bool half) {
    int r = main_c & 0x1F;
    int g = (main_c >> 6) & 0x1F;
    int b = (main_c >> 11) & 0x1F;
    int sr = sub_c & 0x1F;
    int sg = (sub_c >> 6) & 0x1F;
    int sb_val = (sub_c >> 11) & 0x1F;

    if (subtract) { r -= sr; g -= sg; b -= sb_val; }
    else           { r += sr; g += sg; b += sb_val; }

    /* SNES hardware order: clamp THEN halve (not halve then clamp).
     * E.g. 20+30=50 → clamp to 31 → halve to 15, NOT 50→25→25. */
    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 31) g = 31;
    if (b < 0) b = 0; else if (b > 31) b = 31;

    if (half) { r >>= 1; g >>= 1; b >>= 1; }

    return (uint16_t)(r | (g << 6) | (b << 11));
}

/* Render OBJ (sprites) for one scanline into separate color/prio arrays. */
static void PPU_HOT_FUNC(render_obj_scanline)(int scanline, uint16_t *obj_color, uint8_t *obj_prio, TileRowCacheEntry *tile_cache) {
    /* When vertical centering is active, sprites only render within the
     * SNES-visible scanline range. The border scanlines (above/below the
     * centered area) show only BG content — no sprites. This prevents
     * "parked" sprites at high OAM Y values from wrapping into view. */
    if (ppu.sprite_y_offset) {
        int snes_sl = scanline - ppu.sprite_y_offset;
        if (snes_sl < 0 || snes_sl >= SNES_HEIGHT)
            return;
    }

    /* Object sizes based on OBSEL */
    static const int obj_sizes[8][2][2] = {
        /* {small_w, small_h}, {large_w, large_h} */
        {{8, 8}, {16, 16}},
        {{8, 8}, {32, 32}},
        {{8, 8}, {64, 64}},
        {{16, 16}, {32, 32}},
        {{16, 16}, {64, 64}},
        {{32, 32}, {64, 64}},
        {{16, 32}, {32, 64}},
        {{16, 32}, {32, 32}},
    };

    int size_sel = (ppu.obsel >> 5) & 0x07;
    uint32_t obj_base = (uint32_t)(ppu.obsel & 0x07) * 0x4000; /* byte address */
    uint32_t obj_gap = (uint32_t)((ppu.obsel >> 3) & 0x03) * 0x2000 + 0x2000;

    /* Scan sprites in reverse order (lower index = higher priority) */
    for (int i = 127; i >= 0; i--) {
        OAMEntry *spr = &ppu.oam[i];

        /* Get extended attributes from high table */
        int hi_byte = i / 4;
        int hi_shift = (i % 4) * 2;
        uint8_t hi_bits = (ppu.oam_hi[hi_byte] >> hi_shift) & 0x03;
        int size_bit = (hi_bits >> 1) & 1;

        /* Use full 16-bit X coordinate for expanded viewport support.
         * The SNES 9-bit OAM X can't represent X in [256, VIEWPORT_WIDTH). */
        int spr_x = ppu.oam_full_x[i] + ppu.sprite_x_offset;
        /* Use full 16-bit Y coordinate, mirroring the oam_full_x approach.
         * This eliminates 8-bit wrap issues on 240-line viewports. */
        int spr_y = ppu.oam_full_y[i] + ppu.sprite_y_offset;

        int w = obj_sizes[size_sel][size_bit][0];
        int h = obj_sizes[size_sel][size_bit][1];

        /* Check if sprite is on this scanline */
        int row = scanline - spr_y;
        if (row < 0 || row >= h) continue;

        bool vflip = (spr->attr >> 7) & 1;
        bool hflip = (spr->attr >> 6) & 1;
        uint8_t spr_prio = (spr->attr >> 4) & 3;
        uint8_t spr_pal = (spr->attr >> 1) & 7;
        uint16_t tile_num = spr->tile | ((uint16_t)(spr->attr & 1) << 8);

        if (vflip) row = h - 1 - row;

        int tile_row = row >> 3;
        int pixel_y = row & 7;
        int tiles_wide = w >> 3;
        int pal_base_idx = 128 + spr_pal * 16;

        /* Iterate by 8-pixel tile columns instead of individual pixels.
         * decode_4bpp_row is expensive (~32 shift/mask ops); decoding once
         * per tile instead of once per pixel is an up-to-8x speedup. */
        for (int tc = 0; tc < tiles_wide; tc++) {
            int eff_tc = hflip ? (tiles_wide - 1 - tc) : tc;

            uint16_t t = tile_num + eff_tc + tile_row * 16;

            uint32_t tile_addr;
            if (t >= 256)
                tile_addr = obj_base + obj_gap + (uint32_t)(t - 256) * 32;
            else
                tile_addr = obj_base + (uint32_t)t * 32;

            if (tile_addr + 32 > VRAM_SIZE) continue;

            /* Screen X range for this tile column */
            int tile_screen_x = spr_x + tc * 8;
            if (tile_screen_x >= LINE_BUF_WIDTH || tile_screen_x + 8 <= 0)
                continue;

            /* Tile row cache lookup for OBJ tiles (same cache as BG) */
            uint32_t obj_cache_key = tile_addr | ((uint32_t)pixel_y << 17);
            uint32_t obj_cache_idx = ((tile_addr >> 1) ^ pixel_y) & TILE_CACHE_MASK;
            const uint8_t *indices;
            {
                TileRowCacheEntry *cache = tile_cache;
                if (cache[obj_cache_idx].key == obj_cache_key) {
                    indices = cache[obj_cache_idx].indices;
                } else {
                    uint8_t *dest = cache[obj_cache_idx].indices;
                    decode_4bpp_row(&ppu.vram[tile_addr], pixel_y, dest);
                    cache[obj_cache_idx].key = obj_cache_key;
                    indices = dest;
                }
            }

            /* Emit 8 pixels from the decoded tile row */
            for (int px = 0; px < 8; px++) {
                int screen_x = tile_screen_x + px;
                if (screen_x < 0 || screen_x >= LINE_BUF_WIDTH) continue;

                int idx_x = hflip ? (7 - px) : px;
                uint8_t color_idx = indices[idx_x];
                if (color_idx == 0) continue;

                uint16_t rgb = cgram_render[pal_base_idx + color_idx];

                /* On real SNES, among overlapping sprites the lowest OAM index
                 * always wins regardless of the priority field.  Since we iterate
                 * 127→0, later (lower-index) sprites unconditionally overwrite. */
                obj_color[screen_x] = rgb;
                obj_prio[screen_x] = spr_prio + 1;
            }
        }
    }
}

/* Layer bit masks for window iteration (file scope to avoid .rodata
 * reference issues when the function is placed in RAM). */
static const uint8_t win_layer_bits[5] = {
    LAYER_BG1, LAYER_BG2, LAYER_BG3, LAYER_BG4, LAYER_OBJ
};

/* Pre-classified window layer batches — depends only on window config
 * registers (w12sel, w34sel, wobjsel, wbglog, wobjlog, tmw, tsw), which
 * are frame-constant.  Computed once per frame, reused for all scanlines. */
typedef struct {
    uint8_t tm_w1_noninv, ts_w1_noninv;
    uint8_t tm_w1_inv, ts_w1_inv;
    uint8_t tm_w2_noninv, ts_w2_noninv;
    uint8_t tm_w2_inv, ts_w2_inv;
    uint8_t dual_tm, dual_ts;
    uint8_t dual_sel[5];
    uint8_t dual_logic[5];
    int dual_count;
} WindowClassification;

static void classify_window_layers(WindowClassification *wc) {
    uint8_t win_layers = ppu.tmw | ppu.tsw;

    wc->tm_w1_noninv = 0; wc->ts_w1_noninv = 0;
    wc->tm_w1_inv = 0;    wc->ts_w1_inv = 0;
    wc->tm_w2_noninv = 0; wc->ts_w2_noninv = 0;
    wc->tm_w2_inv = 0;    wc->ts_w2_inv = 0;
    wc->dual_tm = 0;      wc->dual_ts = 0;
    wc->dual_count = 0;

    for (int li = 0; li < 5; li++) {
        uint8_t lb = win_layer_bits[li];
        if (!(win_layers & lb)) continue;

        uint8_t sel_bits, logic_bits;
        switch (lb) {
        case LAYER_BG1:
            sel_bits = ppu.w12sel & 0x0F;
            logic_bits = ppu.wbglog & 0x03;
            break;
        case LAYER_BG2:
            sel_bits = (ppu.w12sel >> 4) & 0x0F;
            logic_bits = (ppu.wbglog >> 2) & 0x03;
            break;
        case LAYER_BG3:
            sel_bits = ppu.w34sel & 0x0F;
            logic_bits = (ppu.wbglog >> 4) & 0x03;
            break;
        case LAYER_BG4:
            sel_bits = (ppu.w34sel >> 4) & 0x0F;
            logic_bits = (ppu.wbglog >> 6) & 0x03;
            break;
        case LAYER_OBJ:
            sel_bits = ppu.wobjsel & 0x0F;
            logic_bits = ppu.wobjlog & 0x03;
            break;
        default: continue;
        }

        bool w1_inv = sel_bits & 0x01;
        bool w1_en  = sel_bits & 0x02;
        bool w2_inv = sel_bits & 0x04;
        bool w2_en  = sel_bits & 0x08;
        if (!w1_en && !w2_en) continue;

        bool mask_tmw = (ppu.tmw & lb) != 0;
        bool mask_tsw = (ppu.tsw & lb) != 0;

        if (w1_en && !w2_en) {
            if (!w1_inv) {
                if (mask_tmw) wc->tm_w1_noninv |= lb;
                if (mask_tsw) wc->ts_w1_noninv |= lb;
            } else {
                if (mask_tmw) wc->tm_w1_inv |= lb;
                if (mask_tsw) wc->ts_w1_inv |= lb;
            }
        } else if (!w1_en && w2_en) {
            if (!w2_inv) {
                if (mask_tmw) wc->tm_w2_noninv |= lb;
                if (mask_tsw) wc->ts_w2_noninv |= lb;
            } else {
                if (mask_tmw) wc->tm_w2_inv |= lb;
                if (mask_tsw) wc->ts_w2_inv |= lb;
            }
        } else {
            if (mask_tmw) wc->dual_tm |= lb;
            if (mask_tsw) wc->dual_ts |= lb;
            wc->dual_sel[wc->dual_count] = sel_bits;
            wc->dual_logic[wc->dual_count] = logic_bits;
            wc->dual_count++;
        }
    }
}

/* Precompute per-scanline window masks and color math prevention.
 * Kept as a separate noinline function to reduce ppu_render_frame code size
 * below the 16KB RP2040 XIP cache threshold.  Called once per scanline.
 * Placed in SRAM via PPU_HOT_FUNC to avoid XIP cache thrashing with
 * ppu_render_frame — these two functions would otherwise ping-pong the 16KB
 * XIP cache on every scanline. */
static __attribute__((noinline))
void PPU_HOT_FUNC(precompute_window_masks)(
    int scanline, int render_width, bool wide_mode,
    uint8_t base_tm, uint8_t base_ts,
    uint8_t *eff_tm_line, uint8_t *eff_ts_line,
    uint8_t *cm_prevented_line,
    bool color_math_active, uint8_t prevent_mode,
    const WindowClassification *wc)
{
    if (ppu.tmw || ppu.tsw) {
        uint8_t w1_left, w1_right, w2_left, w2_right;
        if (ppu.window_hdma_active) {
            w1_left  = ppu.wh0_table[scanline];
            w1_right = ppu.wh1_table[scanline];
        } else {
            w1_left  = ppu.wh0;
            w1_right = ppu.wh1;
        }
        if (ppu.window2_hdma_active) {
            w2_left  = ppu.wh2_table[scanline];
            w2_right = ppu.wh3_table[scanline];
        } else {
            w2_left  = ppu.wh2;
            w2_right = ppu.wh3;
        }

        int wx_offset = wide_mode ? -VIEWPORT_PAD_LEFT : 0;

        /* Use pre-classified layer batches (computed once per frame) */
        uint8_t tm_w1_noninv = wc->tm_w1_noninv, ts_w1_noninv = wc->ts_w1_noninv;
        uint8_t tm_w1_inv = wc->tm_w1_inv, ts_w1_inv = wc->ts_w1_inv;
        uint8_t tm_w2_noninv = wc->tm_w2_noninv, ts_w2_noninv = wc->ts_w2_noninv;
        uint8_t tm_w2_inv = wc->tm_w2_inv, ts_w2_inv = wc->ts_w2_inv;
        uint8_t dual_tm = wc->dual_tm, dual_ts = wc->dual_ts;
        int dual_count = wc->dual_count;

        memset(eff_tm_line, base_tm, render_width);
        memset(eff_ts_line, base_ts, render_width);

        /* Apply batched single-window groups — one pass per group */
        #define BATCH_SPAN(ws, we, tm_mask, ts_mask) do { \
            int _xs = (int)(ws) - wx_offset; \
            int _xe = (int)(we) - wx_offset; \
            if (_xs < 0) _xs = 0; \
            if (_xe >= render_width) _xe = render_width - 1; \
            uint8_t _ktm = ~(tm_mask), _kts = ~(ts_mask); \
            for (int _x = _xs; _x <= _xe; _x++) { \
                eff_tm_line[_x] &= _ktm; \
                eff_ts_line[_x] &= _kts; \
            } \
        } while(0)

        if (tm_w1_noninv | ts_w1_noninv)
            BATCH_SPAN(w1_left, w1_right, tm_w1_noninv, ts_w1_noninv);

        if (tm_w1_inv | ts_w1_inv) {
            if (w1_left > 0)
                BATCH_SPAN(0, w1_left - 1, tm_w1_inv, ts_w1_inv);
            if (w1_right < 255)
                BATCH_SPAN(w1_right + 1, 255, tm_w1_inv, ts_w1_inv);
        }

        if (tm_w2_noninv | ts_w2_noninv)
            BATCH_SPAN(w2_left, w2_right, tm_w2_noninv, ts_w2_noninv);

        if (tm_w2_inv | ts_w2_inv) {
            if (w2_left > 0)
                BATCH_SPAN(0, w2_left - 1, tm_w2_inv, ts_w2_inv);
            if (w2_right < 255)
                BATCH_SPAN(w2_right + 1, 255, tm_w2_inv, ts_w2_inv);
        }
        #undef BATCH_SPAN

        /* Dual-window layers: per-pixel fallback (rare) */
        for (int di = 0; di < dual_count; di++) {
            uint8_t sel = wc->dual_sel[di];
            uint8_t logic = wc->dual_logic[di];
            bool d_w1_inv = sel & 0x01;
            bool d_w2_inv = sel & 0x04;
            for (int x = 0; x < render_width; x++) {
                int wx = x + wx_offset;
                if (wx < 0 || wx >= 256) continue;
                bool in_w1 = ((uint8_t)wx >= w1_left && (uint8_t)wx <= w1_right);
                bool in_w2 = ((uint8_t)wx >= w2_left && (uint8_t)wx <= w2_right);
                if (d_w1_inv) in_w1 = !in_w1;
                if (d_w2_inv) in_w2 = !in_w2;
                bool masked;
                switch (logic) {
                case 0: masked = in_w1 | in_w2; break;
                case 1: masked = in_w1 & in_w2; break;
                case 2: masked = in_w1 ^ in_w2; break;
                case 3: masked = !(in_w1 ^ in_w2); break;
                default: masked = false; break;
                }
                if (masked) {
                    if (dual_tm) eff_tm_line[x] &= ~dual_tm;
                    if (dual_ts) eff_ts_line[x] &= ~dual_ts;
                }
            }
        }
    } else {
        memset(eff_tm_line, base_tm, render_width);
        memset(eff_ts_line, base_ts, render_width);
    }

    /* Color math prevention window */
    if (color_math_active && (prevent_mode == 1 || prevent_mode == 2)) {
        uint8_t cm_sel = (ppu.wobjsel >> 4) & 0x0F;
        uint8_t cm_logic = (ppu.wobjlog >> 2) & 0x03;
        bool cm_w1_inv = cm_sel & 0x01;
        bool cm_w1_en  = cm_sel & 0x02;
        bool cm_w2_inv = cm_sel & 0x04;
        bool cm_w2_en  = cm_sel & 0x08;

        uint8_t w1_left, w1_right, w2_left, w2_right;
        if (ppu.window_hdma_active) {
            w1_left  = ppu.wh0_table[scanline];
            w1_right = ppu.wh1_table[scanline];
        } else {
            w1_left  = ppu.wh0;
            w1_right = ppu.wh1;
        }
        if (ppu.window2_hdma_active) {
            w2_left  = ppu.wh2_table[scanline];
            w2_right = ppu.wh3_table[scanline];
        } else {
            w2_left  = ppu.wh2;
            w2_right = ppu.wh3;
        }

        int wx_offset = wide_mode ? -VIEWPORT_PAD_LEFT : 0;

        if (!cm_w1_en && !cm_w2_en) {
            memset(cm_prevented_line, (prevent_mode == 1) ? 1 : 0, render_width);
        } else {
            uint8_t val_inside = (prevent_mode == 2) ? 1 : 0;
            uint8_t val_outside = (prevent_mode == 1) ? 1 : 0;

            #define CM_SPAN(ws, we, val) do { \
                int _xs = (int)(ws) - wx_offset; \
                int _xe = (int)(we) - wx_offset; \
                if (_xs < 0) _xs = 0; \
                if (_xe >= render_width) _xe = render_width - 1; \
                if (_xs <= _xe) memset(&cm_prevented_line[_xs], (val), _xe - _xs + 1); \
            } while(0)

            if (cm_w1_en && !cm_w2_en) {
                memset(cm_prevented_line, val_outside, render_width);
                if (!cm_w1_inv) {
                    CM_SPAN(w1_left, w1_right, val_inside);
                } else {
                    if (w1_left > 0) CM_SPAN(0, w1_left - 1, val_inside);
                    if (w1_right < 255) CM_SPAN(w1_right + 1, 255, val_inside);
                }
            } else if (!cm_w1_en && cm_w2_en) {
                memset(cm_prevented_line, val_outside, render_width);
                if (!cm_w2_inv) {
                    CM_SPAN(w2_left, w2_right, val_inside);
                } else {
                    if (w2_left > 0) CM_SPAN(0, w2_left - 1, val_inside);
                    if (w2_right < 255) CM_SPAN(w2_right + 1, 255, val_inside);
                }
            } else {
                for (int x = 0; x < render_width; x++) {
                    int wx = x + wx_offset;
                    if (wx < 0 || wx >= 256) {
                        cm_prevented_line[x] = val_outside;
                        continue;
                    }
                    bool in_w1 = ((uint8_t)wx >= w1_left && (uint8_t)wx <= w1_right);
                    bool in_w2 = ((uint8_t)wx >= w2_left && (uint8_t)wx <= w2_right);
                    if (cm_w1_inv) in_w1 = !in_w1;
                    if (cm_w2_inv) in_w2 = !in_w2;
                    bool in_cw;
                    switch (cm_logic) {
                    case 0: in_cw = in_w1 | in_w2; break;
                    case 1: in_cw = in_w1 & in_w2; break;
                    case 2: in_cw = in_w1 ^ in_w2; break;
                    case 3: in_cw = !(in_w1 ^ in_w2); break;
                    default: in_cw = false; break;
                    }
                    cm_prevented_line[x] = in_cw ? val_inside : val_outside;
                }
            }
            #undef CM_SPAN
        }
    } else {
        memset(cm_prevented_line, 0, render_width);
    }
}

/* Per-context static buffers for dual-core rendering.
 * Promoted from stack to static so both cores have independent working memory.
 * When PPU_NUM_RENDER_CONTEXTS == 1 (default), this is a single set of arrays
 * with no runtime overhead vs. the old stack allocation. */
static pixel_t  line_out_ctx[PPU_NUM_RENDER_CONTEXTS][VIEWPORT_WIDTH];
static uint16_t best_bg_color_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH];
static uint16_t best_bg_gp_lm_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH];
static uint16_t sub_bg_color_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH];
static uint8_t  sub_bg_gp_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH];
static uint16_t obj_color_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH];
static uint8_t  obj_prio_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH];
static uint8_t  eff_tm_line_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH];
static uint8_t  eff_ts_line_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH];
static uint8_t  cm_prevented_line_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH];

/* Temp buffers for wide-mode non-filling layer render path.
 * Promoted from stack to static to avoid core 1 stack overflow (4KB limit). */
static uint16_t temp_color_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH];
static uint16_t temp_gp_lm_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH];
static uint16_t temp_sub_color_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH];
static uint8_t  temp_sub_gp_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH];
static uint8_t  temp_tm_all_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH];

void PPU_HOT_FUNC(ppu_render_frame_ex)(int ctx_id, int y_start, int y_end,
                         int y_stride, scanline_callback_t send_scanline) {
    pixel_t  *line_out        = line_out_ctx[ctx_id];
    uint16_t *best_bg_color   = best_bg_color_ctx[ctx_id];
    uint16_t *best_bg_gp_lm   = best_bg_gp_lm_ctx[ctx_id];
    uint16_t *sub_bg_color    = sub_bg_color_ctx[ctx_id];
    uint8_t  *sub_bg_gp       = sub_bg_gp_ctx[ctx_id];
    uint16_t *obj_color       = obj_color_ctx[ctx_id];
    uint8_t  *obj_prio        = obj_prio_ctx[ctx_id];
    uint8_t  *eff_tm_line     = eff_tm_line_ctx[ctx_id];
    uint8_t  *eff_ts_line     = eff_ts_line_ctx[ctx_id];
    uint8_t  *cm_prevented_line = cm_prevented_line_ctx[ctx_id];

    TileRowCacheEntry *tile_cache = tile_row_cache[ctx_id];

    /* Check force blank */
    if (ppu.inidisp & 0x80) {
        for (int y = y_start; y < y_end; y += y_stride) {
            memset(line_out, 0, VIEWPORT_WIDTH * sizeof(pixel_t));
            send_scanline(y, line_out);
        }
        return;
    }

    uint8_t brightness = ppu.inidisp & 0x0F;

    /* Invalidate tile row cache for this context — VRAM may have changed. */
    memset(tile_cache, 0xFF, TILE_CACHE_SIZE * sizeof(TileRowCacheEntry));

    /* Shadow palette: built by ppu_prepare_palette() before this call.
     * In single-core mode, ppu_render_frame() calls it automatically.
     * In dual-core mode, the worker calls it before signaling core 1. */

    int mode = ppu.bgmode & BGMODE_MODE_MASK;

    /* Determine BPP per layer based on mode */
    int bg_bpp[4] = {0, 0, 0, 0};
    switch (mode) {
    case 0: bg_bpp[0] = 2; bg_bpp[1] = 2; bg_bpp[2] = 2; bg_bpp[3] = 2; break;
    case 1: bg_bpp[0] = 4; bg_bpp[1] = 4; bg_bpp[2] = 2; break;
    case 2: bg_bpp[0] = 4; bg_bpp[1] = 4; break;
    case 3: bg_bpp[0] = 8; bg_bpp[1] = 4; break;
    case 4: bg_bpp[0] = 8; bg_bpp[1] = 2; break;
    case 5: bg_bpp[0] = 4; bg_bpp[1] = 2; break;
    case 7: bg_bpp[0] = 8; break;
    }

    /* Mosaic: bits 7-4 = size (0=off, 1=2x2 .. 15=16x16), bits 3-0 = BG enable */
    uint8_t mosaic_size = ppu.mosaic >> MOSAIC_SIZE_SHIFT;
    uint8_t mosaic_bgs  = ppu.mosaic & MOSAIC_BG_MASK;

    /* Color math configuration (constant for all scanlines) */
    uint8_t math_layers = ppu.cgadsub & 0x3F; /* which layers are subject to math */
    bool color_math_active = (math_layers != 0);
    uint8_t prevent_mode = (ppu.cgwsel >> 4) & 3;
    if (prevent_mode == 3) color_math_active = false; /* always prevent */
    bool use_sub_screen = (ppu.cgwsel & 0x02) != 0;
    bool subtract_mode = (ppu.cgadsub & 0x80) != 0;
    bool half_math = (ppu.cgadsub & 0x40) != 0;
    /* Fixed color for color math — convert directly to BGR565 */
    uint16_t fixed_color = (uint16_t)ppu.coldata_r |
                           ((uint16_t)ppu.coldata_g << 6) |
                           ((uint16_t)ppu.coldata_b << 11);

    /* Which layers need rendering (union of main + sub screen) */
    uint8_t layers_needed = ppu.tm | ppu.ts;

    /* Detect wide mode: any active layer that fills the viewport (64-tile
     * tilemap or explicit bg_viewport_fill flag) triggers full-width rendering.
     * Other screens (intro, menus, gas station, file select) use 32-tile
     * tilemaps without fill and render at SNES native resolution centered.
     * Note: bg2_distortion (per-scanline HDMA) is applied inside
     * render_bg_scanline() regardless of wide_mode.
     *
     * Wide mode is enabled for ANY non-native viewport (both larger AND
     * smaller than SNES) so that filling layers render at VIEWPORT_WIDTH
     * while non-filling layers are centered/cropped correctly. */
    bool wide_mode = false;
    if (VIEWPORT_WIDTH != SNES_WIDTH || VIEWPORT_HEIGHT != SNES_HEIGHT) {
        for (int bg = 0; bg < 4 && !wide_mode; bg++) {
            if (!(layers_needed & (1 << bg))) continue;
            if (bg_bpp[bg] == 0) continue;
            if ((ppu.bg_sc[bg] & 0x01) || ppu.bg_viewport_fill[bg])
                wide_mode = true;
        }
    }

    int render_height = VIEWPORT_HEIGHT;
    int render_width = VIEWPORT_WIDTH;
    int fb_x_offset = 0;
    int fb_y_offset = 0;

    if (!wide_mode && (VIEWPORT_WIDTH != SNES_WIDTH || VIEWPORT_HEIGHT != SNES_HEIGHT)) {
        /* No filling layers — render at SNES resolution, centered/cropped.
         * Always render SNES_WIDTH pixels into line buffers, then crop/pad
         * when writing to the output. */
        render_height = VIEWPORT_HEIGHT < SNES_HEIGHT ? VIEWPORT_HEIGHT : SNES_HEIGHT;
        render_width = SNES_WIDTH;
        fb_x_offset = VIEWPORT_PAD_LEFT;  /* negative when viewport < SNES */
        fb_y_offset = VIEWPORT_HEIGHT > SNES_HEIGHT ? (VIEWPORT_HEIGHT - SNES_HEIGHT) / 2 : 0;
    }

#ifdef PPU_PROFILE
    uint32_t prof_clear = 0, prof_bg = 0, prof_obj = 0;
    uint32_t prof_win = 0, prof_composite = 0, prof_send = 0;
    PROF_SECTION(total);
#endif

    /* Frame-constant compositing values (hoisted out of scanline loop) */
    uint16_t bm = brightness * 17; /* 0→0, 1→17, ... 14→238, 15→255 */
    bool bg3prio = (ppu.bgmode & BGMODE_BG3_PRIO) && mode == 1;
    uint16_t backdrop = cgram_render[0];
    bool backdrop_has_math = color_math_active && (0x20 & math_layers);

    /* Fast composite path: merge BG layers into a single priority-ordered
     * buffer during rendering, then compositing is just OBJ-vs-merged-BG
     * (2-3 checks instead of 12).  Now also handles sub-screen color math
     * by building a second merged buffer for the sub screen. */
    bool fast_composite = true;

    /* Global BG priority table: maps (bg_index, prio_bit) → global priority.
     * Higher values = higher priority.  Used by the fast path to merge BG
     * layers during rendering.  OBJ interleaves at known thresholds. */
    uint8_t bg_gp[4][2]; /* [bg_index][prio_bit] */
    uint8_t obj_thresh[5]; /* [obj_prio_key 0-4]: OBJ wins if bg_gp < this */
    if (fast_composite) {
        /* Mode 1 priority (most common for EarthBound):
         * BG1p1=7, BG2p1=6, BG1p0=5, BG2p0=4, BG3p1=3, BG3p0=2
         * With BG3PRIO: BG3p1 jumps to 8 (above everything) */
        memset(bg_gp, 0, sizeof(bg_gp));
        switch (mode) {
        case 1:
            bg_gp[0][0] = 5; bg_gp[0][1] = 7;  /* BG1 */
            bg_gp[1][0] = 4; bg_gp[1][1] = 6;  /* BG2 */
            bg_gp[2][0] = 2; bg_gp[2][1] = bg3prio ? 8 : 3; /* BG3 */
            obj_thresh[0] = 0;  /* transparent: never wins */
            obj_thresh[1] = 3;  /* OBJ prio 0: beats BG3p0 */
            obj_thresh[2] = bg3prio ? 3 : 4;  /* OBJ prio 1 */
            obj_thresh[3] = 6;  /* OBJ prio 2: beats up to BG1p0 */
            obj_thresh[4] = bg3prio ? 8 : 8;  /* OBJ prio 3: beats all (except BG3PRIO high) */
            break;
        case 0:
            bg_gp[0][0] = 7; bg_gp[0][1] = 11; /* BG1 */
            bg_gp[1][0] = 5; bg_gp[1][1] = 9;  /* BG2 */
            bg_gp[2][0] = 3; bg_gp[2][1] = 6;  /* BG3 */
            bg_gp[3][0] = 1; bg_gp[3][1] = 2;  /* BG4 */
            obj_thresh[0] = 0;
            obj_thresh[1] = 2;  /* OBJ prio 0 */
            obj_thresh[2] = 5;  /* OBJ prio 1 */
            obj_thresh[3] = 7;  /* OBJ prio 2 */
            obj_thresh[4] = 11; /* OBJ prio 3 */
            break;
        default:
            /* Modes 2-7: use BG1/BG2 only priority ordering */
            bg_gp[0][0] = 3; bg_gp[0][1] = 5; /* BG1 */
            bg_gp[1][0] = 2; bg_gp[1][1] = 4; /* BG2 */
            obj_thresh[0] = 0;
            obj_thresh[1] = 2;
            obj_thresh[2] = 3;
            obj_thresh[3] = 5;
            obj_thresh[4] = 6;
            break;
        }
    }

    /* Window mask arrays: tm_line and ts_line are always valid pointers
     * (never NULL).  When no windows are active, they're filled with 0xFF
     * (all layers enabled) to eliminate per-pixel NULL checks in the
     * emit_tile_run inner loop.
     * eff_tm_line, eff_ts_line, cm_prevented_line are per-context statics
     * aliased above. */
    uint8_t win_cache_key[6] = {0xFF, 0, 0, 0, 0, 0}; /* force first compute */

    /* Classify window layers once per frame — depends only on window config
     * registers which don't change within a frame (only positions change). */
    WindowClassification win_class;
    bool has_layer_windows = (ppu.tmw || ppu.tsw);
    /* Color math window can be active even when TMW/TSW=0 (e.g. battle swirl
     * uses WOBJSEL for color math windowing without masking any layers). */
    bool has_cm_windows = color_math_active && (prevent_mode == 1 || prevent_mode == 2);
    bool has_windows = has_layer_windows || has_cm_windows;
    if (has_layer_windows)
        classify_window_layers(&win_class);

    /* Unified output Y loop: y_start/y_end/y_stride are OUTPUT coordinates.
     * Border scanlines (outside fb_y_offset..fb_y_offset+render_height-1)
     * are sent as black.  This avoids separate border loops that break
     * when the output range is split across cores. */
    int render_y_start = fb_y_offset;
    int render_y_end = fb_y_offset + render_height;

    for (int out_y = y_start; out_y < y_end; out_y += y_stride) {
        /* Border scanline — outside the renderable area */
        if (out_y < render_y_start || out_y >= render_y_end) {
            memset(line_out, 0, VIEWPORT_WIDTH * sizeof(pixel_t));
            send_scanline(out_y, line_out);
            continue;
        }

        int scanline = out_y - fb_y_offset;
        /* Per-scanline buffers are static per-context (aliased above).
         * Merged BG priority buffers — BG layers write directly here. */
        bool need_sub = color_math_active && use_sub_screen;

        PROF_SECTION(clear);
        memset(best_bg_gp_lm, 0, render_width * sizeof(uint16_t));
        if (need_sub)
            memset(sub_bg_gp, 0, render_width);
        if (layers_needed & LAYER_OBJ)
            memset(obj_prio, 0, LINE_BUF_WIDTH);

        /* Clear output line (handles left/right black borders) */
        memset(line_out, 0, VIEWPORT_WIDTH * sizeof(pixel_t));
        PROF_END(clear, prof_clear);

        /* SNES-space scanline for scenes using explicit viewport fill. */
        int snes_scanline = scanline - ppu.sprite_y_offset;

        /* Base TM/TS for this scanline (before window masking) */
        uint8_t base_tm, base_ts;
        if (ppu.tm_hdma_active) {
            if (snes_scanline >= 0 && snes_scanline < SNES_HEIGHT) {
                base_tm = ppu.tm_per_scanline[snes_scanline];
                base_ts = ppu.ts_per_scanline[snes_scanline];
            } else {
                base_tm = 0;
                base_ts = 0;
            }
        } else {
            base_tm = ppu.tm;
            base_ts = ppu.ts;
        }

        /* Compute window masks BEFORE BG rendering so emit_tile_run can
         * apply them during the merge.  eff_tm_line / eff_ts_line are always
         * valid — filled with base_tm/base_ts when no windows, so the inner
         * loop never needs a NULL-pointer check.  The fill uses base_tm/ts
         * (not 0xFF) because layers not in TM must not write to main screen,
         * and layers not in TS must not write to sub screen. */
        PROF_SECTION(win);
        bool no_windows = !has_windows;
        if (!has_layer_windows) {
            memset(eff_tm_line, base_tm, render_width);
            memset(eff_ts_line, base_ts, render_width);
        }
        if (!has_layer_windows && !has_cm_windows) {
            /* No layer windows and no color math windows — skip entirely */
        } else {
            uint8_t w1l = ppu.window_hdma_active ? ppu.wh0_table[scanline] : ppu.wh0;
            uint8_t w1r = ppu.window_hdma_active ? ppu.wh1_table[scanline] : ppu.wh1;
            uint8_t w2l = ppu.window2_hdma_active ? ppu.wh2_table[scanline] : ppu.wh2;
            uint8_t w2r = ppu.window2_hdma_active ? ppu.wh3_table[scanline] : ppu.wh3;
            uint8_t key[6] = {w1l, w1r, w2l, w2r, base_tm, base_ts};
            if (memcmp(key, win_cache_key, 6) != 0) {
                memcpy(win_cache_key, key, 6);
                precompute_window_masks(scanline, render_width, wide_mode,
                                        base_tm, base_ts,
                                        eff_tm_line, eff_ts_line,
                                        cm_prevented_line,
                                        color_math_active, prevent_mode,
                                        &win_class);
            }
        }
        PROF_END(win, prof_win);

        /* Render BG layers — merge directly into priority buffers */
        PROF_SECTION(bg);
        for (int bg = 0; bg < 4; bg++) {
            if (!(layers_needed & (1 << bg))) continue;
            if (bg_bpp[bg] == 0) continue;

            /* Skip layers not enabled on either screen (window-independent) */
            uint8_t layer_bit = 1 << bg;
            if (no_windows && !(base_tm & layer_bit) &&
                !(need_sub && (base_ts & layer_bit)))
                continue;

            bool fills_via_tilemap = (ppu.bg_sc[bg] & 0x01) != 0;
            bool fills_explicit = (ppu.bg_viewport_fill[bg] == BG_VIEWPORT_FILL);
            bool layer_fills = wide_mode && (fills_via_tilemap || fills_explicit);

            int bg_scanline;
            if (layer_fills && !fills_explicit) {
                bg_scanline = scanline;
            } else if (wide_mode) {
                bg_scanline = snes_scanline;
                if (ppu.bg_viewport_fill[bg] == BG_VIEWPORT_CLAMP) {
                    if (bg_scanline < 0) bg_scanline = 0;
                    else if (bg_scanline >= SNES_HEIGHT) bg_scanline = SNES_HEIGHT - 1;
                }
            } else {
                bg_scanline = scanline;
            }

            int eff_scanline = bg_scanline;
            bool bg_mosaic = (mosaic_size > 0) && (mosaic_bgs & (1 << bg));
            if (bg_mosaic) {
                int block = mosaic_size + 1;
                eff_scanline = (bg_scanline / block) * block;
            }

            /* Set up render context for this layer */
            BGRenderCtx ctx = {
                .main_color = best_bg_color,
                .main_gp_lm = best_bg_gp_lm,
                .sub_color = need_sub ? sub_bg_color : NULL,
                .sub_gp = need_sub ? sub_bg_gp : NULL,
                .tm_line = eff_tm_line,
                .ts_line = eff_ts_line,
                .gp0_lm = bg_gp[bg][0] | ((uint16_t)layer_bit << 8),
                .gp1_lm = bg_gp[bg][1] | ((uint16_t)layer_bit << 8),
                .layer_bit = layer_bit,
                .tile_cache = tile_cache,
            };

            if (layer_fills) {
                render_bg_scanline(bg, eff_scanline, bg_bpp[bg],
                                   &ctx, VIEWPORT_WIDTH);
            } else if (wide_mode) {
                /* Non-filling layer in wide mode: render SNES_WIDTH into
                 * temp buffers, then merge visible portion into main. */
                if (ppu.sprite_y_offset &&
                    (snes_scanline < 0 || snes_scanline >= SNES_HEIGHT) &&
                    ppu.bg_viewport_fill[bg] != BG_VIEWPORT_CLAMP)
                    goto bg_mosaic_done;

                /* Per-context static temp buffers (avoids 2KB stack alloc
                 * that would overflow core 1's 4KB SCRATCH_X stack) */
                uint16_t *temp_color = temp_color_ctx[ctx_id];
                uint16_t *temp_gp_lm = temp_gp_lm_ctx[ctx_id];
                uint16_t *temp_sub_color = temp_sub_color_ctx[ctx_id];
                uint8_t *temp_sub_gp = temp_sub_gp_ctx[ctx_id];
                uint8_t *temp_tm_all = temp_tm_all_ctx[ctx_id];
                memset(temp_gp_lm, 0, SNES_WIDTH * sizeof(uint16_t));
                memset(temp_tm_all, 0xFF, SNES_WIDTH);
                if (need_sub) memset(temp_sub_gp, 0, SNES_WIDTH);

                /* Temp context pointing to temp buffers */
                BGRenderCtx temp_ctx = ctx;
                temp_ctx.main_color = temp_color;
                temp_ctx.main_gp_lm = temp_gp_lm;
                temp_ctx.sub_color = need_sub ? temp_sub_color : NULL;
                temp_ctx.sub_gp = need_sub ? temp_sub_gp : NULL;
                /* No window masking in temp — apply when merging */
                temp_ctx.tm_line = temp_tm_all;
                temp_ctx.ts_line = temp_tm_all;

                render_bg_scanline(bg, eff_scanline, bg_bpp[bg],
                                   &temp_ctx, SNES_WIDTH);

                /* Copy visible portion into main merged buffers */
                int pad = VIEWPORT_PAD_LEFT;
                int src_start = pad < 0 ? -pad : 0;
                int dst_start = pad > 0 ?  pad : 0;
                int count = SNES_WIDTH - src_start;
                if (dst_start + count > VIEWPORT_WIDTH)
                    count = VIEWPORT_WIDTH - dst_start;
                for (int i = 0; i < count; i++) {
                    int sx = src_start + i;
                    int dx = dst_start + i;
                    uint8_t gp = GP_LM_GP(temp_gp_lm[sx]);
                    if (gp == 0) continue;
                    /* Apply window mask during merge */
                    if ((eff_tm_line[dx] & layer_bit) &&
                        gp > GP_LM_GP(best_bg_gp_lm[dx])) {
                        best_bg_gp_lm[dx] = temp_gp_lm[sx];
                        best_bg_color[dx] = temp_color[sx];
                    }
                    if (need_sub &&
                        (eff_ts_line[dx] & layer_bit) &&
                        gp > sub_bg_gp[dx]) {
                        sub_bg_gp[dx] = gp;
                        sub_bg_color[dx] = temp_sub_color[sx];
                    }
                }

                /* Clamp mode: extend edge pixels into the border area */
                if (ppu.bg_viewport_fill[bg] == BG_VIEWPORT_CLAMP && count > 0) {
                    uint16_t left_c = best_bg_color[dst_start];
                    uint16_t left_packed = best_bg_gp_lm[dst_start];
                    uint16_t right_c = best_bg_color[dst_start + count - 1];
                    uint16_t right_packed = best_bg_gp_lm[dst_start + count - 1];
                    for (int x = 0; x < dst_start; x++) {
                        if (GP_LM_GP(left_packed) > GP_LM_GP(best_bg_gp_lm[x])) {
                            best_bg_gp_lm[x] = left_packed;
                            best_bg_color[x] = left_c;
                        }
                    }
                    for (int x = dst_start + count; x < VIEWPORT_WIDTH; x++) {
                        if (GP_LM_GP(right_packed) > GP_LM_GP(best_bg_gp_lm[x])) {
                            best_bg_gp_lm[x] = right_packed;
                            best_bg_color[x] = right_c;
                        }
                    }
                }
            } else {
                render_bg_scanline(bg, eff_scanline, bg_bpp[bg],
                                   &ctx, render_width);
            }

            /* Mosaic: horizontal — replicate leftmost pixel in each block.
             * Operates on the merged buffer for this layer's pixels. */
            if (bg_mosaic) {
                int block = mosaic_size + 1;
                uint8_t gp0 = bg_gp[bg][0];
                uint8_t gp1 = bg_gp[bg][1];
                for (int x = 0; x < VIEWPORT_WIDTH; x += block) {
                    uint16_t ref_color = best_bg_color[x];
                    uint8_t ref_gp = GP_LM_GP(best_bg_gp_lm[x]);
                    /* Only replicate if this pixel belongs to this layer */
                    if (ref_gp != gp0 && ref_gp != gp1) continue;
                    uint16_t ref_packed = ref_gp | ((uint16_t)layer_bit << 8);
                    for (int dx = 1; dx < block && (x + dx) < VIEWPORT_WIDTH; dx++) {
                        best_bg_gp_lm[x + dx] = ref_packed;
                        best_bg_color[x + dx] = ref_color;
                    }
                }
            }
            bg_mosaic_done: ;
        }
        PROF_END(bg, prof_bg);

        /* Render sprites if needed on either screen */
        PROF_SECTION(obj);
        if (layers_needed & LAYER_OBJ) {
            render_obj_scanline(scanline, obj_color, obj_prio, tile_cache);
        }
        PROF_END(obj, prof_obj);

        /* --- Compositing: OBJ vs merged BG --- */
        PROF_SECTION(comp);
        {
            /* Pre-compute valid pixel range to eliminate per-pixel bounds check.
             * line_out is pre-cleared to black, so border pixels are already set. */
            int x_start = fb_x_offset < 0 ? -fb_x_offset : 0;
            int x_end = render_width;
            if (fb_x_offset + x_end > VIEWPORT_WIDTH)
                x_end = VIEWPORT_WIDTH - fb_x_offset;

            /* Hoist loop-invariant OBJ enable check */
            bool obj_main_en = no_windows ? (base_tm & LAYER_OBJ) != 0 : false;
            bool obj_sub_en = no_windows ? (base_ts & LAYER_OBJ) != 0 : false;

            for (int x = x_start; x < x_end; x++) {
                uint16_t color;
                uint8_t main_layer;

                /* OBJ check: window-masked or constant */
                uint8_t obj_pk;
                if (no_windows)
                    obj_pk = obj_main_en ? obj_prio[x] : 0;
                else
                    obj_pk = (eff_tm_line[x] & LAYER_OBJ) ? obj_prio[x] : 0;

                uint16_t bg_packed = best_bg_gp_lm[x];
                uint8_t bg_gp_val = GP_LM_GP(bg_packed);

                if (obj_pk > 0 && bg_gp_val < obj_thresh[obj_pk]) {
                    color = obj_color[x];
                    main_layer = LAYER_OBJ;
                } else if (bg_gp_val > 0) {
                    color = best_bg_color[x];
                    main_layer = GP_LM_LM(bg_packed);
                } else {
                    color = backdrop;
                    main_layer = 0x20;
                }

                /* Color math */
                if (color_math_active && (main_layer & math_layers) &&
                    (no_windows || !cm_prevented_line[x])) {
                    uint16_t sub_color;
                    if (need_sub) {
                        uint8_t sub_obj_pk;
                        if (no_windows)
                            sub_obj_pk = obj_sub_en ? obj_prio[x] : 0;
                        else
                            sub_obj_pk = (eff_ts_line[x] & LAYER_OBJ) ? obj_prio[x] : 0;
                        uint8_t sub_gp_val = sub_bg_gp[x];
                        if (sub_obj_pk > 0 && sub_gp_val < obj_thresh[sub_obj_pk])
                            sub_color = obj_color[x];
                        else if (sub_gp_val > 0)
                            sub_color = sub_bg_color[x];
                        else
                            sub_color = 0;
                    } else {
                        sub_color = fixed_color;
                    }
                    color = blend_colors(color, sub_color, subtract_mode, half_math);
                }

                /* Brightness */
                pixel_t px = color;
                if (brightness < 0x0F) {
                    uint16_t r = (color & 0x1F) * bm >> 8;
                    uint16_t g = ((color >> 6) & 0x1F) * bm >> 8;
                    uint16_t b = ((color >> 11) & 0x1F) * bm >> 8;
                    px = r | (g << 6) | (b << 11);
                }

                line_out[x + fb_x_offset] = px;
            }
        }
        PROF_END(comp, prof_composite);

        PROF_SECTION(snd);
        send_scanline(out_y, line_out);
        PROF_END(snd, prof_send);
    }

#ifdef PPU_PROFILE
    ppu_profile.total = (uint32_t)(platform_timer_ticks() - _prof_total);
    ppu_profile.clear = prof_clear;
    ppu_profile.bg = prof_bg;
    ppu_profile.obj = prof_obj;
    ppu_profile.win = prof_win;
    ppu_profile.composite = prof_composite;
    ppu_profile.send = prof_send;
    ppu_profile.ready = true;
#endif

    /* Borders are handled by the unified output Y loop above — no
     * separate top/bottom border code needed. */
}

void ppu_render_frame(scanline_callback_t send_scanline) {
    ppu_prepare_palette();
    ppu_render_frame_ex(0, 0, VIEWPORT_HEIGHT, 1, send_scanline);
}
