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

/* PPU_LINEBUF_ATTR — embedded ports can route the per-scanline working buffers
 * into fast memory (e.g. STM32 DTCM) by defining this at build time. Defaults
 * to nothing on desktop, so the buffers stay in normal BSS. */
#ifndef PPU_LINEBUF_ATTR
#define PPU_LINEBUF_ATTR
#endif

#ifdef PPU_PROFILE
PPUProfile ppu_profile;
#define PROF_SECTION(name) uint64_t _prof_##name = platform_timer_ticks()
#define PROF_END(name, acc) (acc) += (uint32_t)(platform_timer_ticks() - _prof_##name)

/* Cheap per-call BG work counters (increments only — no timers in the inner
 * loop, so they don't distort the PROF section timings).  Accumulated across
 * BG_PROF_FRAMES frames, then printf'd (with the last frame's phase timings)
 * and reset.  Purely diagnostic; no effect on rendering. */
#define BG_PROF_FRAMES 60
typedef struct {
    uint32_t frames;
    uint32_t scanlines;   /* BG-phase scanlines entered */
    uint32_t layers;      /* layer-scanlines actually rendered */
    uint32_t emit_calls;  /* emit_tile_run entries past the VRAM bounds check */
    uint32_t blank_skips; /* blank-row early returns (no decode, no emit) */
    uint32_t cache_hits;
    uint32_t cache_misses;/* == tile-row decodes */
    uint32_t pixels;      /* sum of per-call pixel counts */
    uint32_t elided_tiles;/* fully-transparent tiles elided from the row plan */
    uint32_t opaque_runs; /* emit runs whose tile row is fully opaque (no transparent px) */
    uint32_t sub_emit_calls;/* emit runs that also rendered the sub-screen (color math on) */
} BGCounters;
static BGCounters g_bgc;
#define BGC_INC(field)        (g_bgc.field++)
#define BGC_ADD(field, n)     (g_bgc.field += (uint32_t)(n))
#else
#define PROF_SECTION(name) ((void)0)
#define PROF_END(name, acc) ((void)0)
#define BGC_INC(field)        ((void)0)
#define BGC_ADD(field, n)     ((void)0)
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
#ifdef PPU_PROFILE
    uint8_t opaque;     /* 1 = no transparent pixel in this row (diagnostic only) */
#endif
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
    /* No-window fast path: when windows are globally inactive, eff_tm_line/
     * eff_ts_line are uniform, so the per-pixel window mask collapses to these
     * loop-invariant booleans and the per-pixel tm_line/ts_line loads drop out. */
    uint8_t no_window;       /* 1 = use the window-mask-free emit variant */
    uint8_t main_on;         /* (base_tm & layer_bit)!=0; valid when no_window */
    uint8_t sub_on;          /* (base_ts & layer_bit)!=0; valid when no_window */
    /* 1 = target merge buffer is known-empty (freshly cleared) for every pixel
     * this layer writes, so the per-pixel priority load+compare is dead — the
     * write always wins. Set for the first BG layer into best_bg and for every
     * temp-buffer layer (temp_gp_lm is memset per layer). Requires no_window +
     * main_on; falls back to the windowed/NW path otherwise. */
    uint8_t uncond;
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
    BGC_INC(emit_calls);
    BGC_ADD(pixels, count);

    const uint8_t *tile_data = &ppu.vram[tile_addr];

    /* Quick blank-row check: if all bitplane bytes for this row are zero,
     * every pixel is transparent — skip decode + emit entirely. */
    {
        const uint8_t *bp = tile_data + pixel_y * 2;
        uint8_t any = bp[0] | bp[1];
        if (bpp >= 4) { any |= bp[16] | bp[17]; }
        if (bpp == 8) { any |= bp[32] | bp[33] | bp[48] | bp[49]; }
        if (!any) { BGC_INC(blank_skips); return; }
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
            BGC_INC(cache_hits);
            indices = cache[cache_idx].indices;
        } else {
            /* Cache miss — decode and store */
            BGC_INC(cache_misses);
            uint8_t *dest = cache[cache_idx].indices;
            if (bpp == 8)
                decode_8bpp_row(tile_data, pixel_y, dest);
            else if (bpp == 4)
                decode_4bpp_row(tile_data, pixel_y, dest);
            else
                decode_2bpp_row(tile_data, pixel_y, dest);
            cache[cache_idx].key = cache_key;
            indices = dest;
#ifdef PPU_PROFILE
            uint8_t _opq = 1;
            for (int _k = 0; _k < 8; _k++) if (dest[_k] == 0) { _opq = 0; break; }
            cache[cache_idx].opaque = _opq;
#endif
        }
    }
#ifdef PPU_PROFILE
    if (ctx->tile_cache[cache_idx].opaque) BGC_INC(opaque_runs);
#endif

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

    /* No-window variant: windows globally inactive, so eff_tm_line/eff_ts_line
     * are uniform. The per-pixel `tm_line[_sx] & layer_bit` load+AND collapses
     * to the loop-invariant booleans main_on/sub_on — one memory load removed
     * from the critical path of every emitted pixel. */
#define EMIT_PIXELS_NW(HAS_SUB) do { \
    for (int _i = 0; _i < count; _i++) { \
        int _px = hflip ? (7 - (start_px + _i)) : (start_px + _i); \
        uint8_t _cidx = indices[_px]; \
        if (_cidx == 0) continue; \
        uint16_t _rgb = (bpp == 8) ? cgram_render[_cidx] \
                                    : cgram_render[pal_base + _cidx]; \
        int _sx = screen_x + _i; \
        if (main_on && gp > GP_LM_GP(ctx->main_gp_lm[_sx])) { \
            ctx->main_gp_lm[_sx] = gp_lm; \
            ctx->main_color[_sx] = _rgb; \
        } \
        if (HAS_SUB && sub_on && gp > ctx->sub_gp[_sx]) { \
            ctx->sub_gp[_sx] = gp; \
            ctx->sub_color[_sx] = _rgb; \
        } \
    } \
} while(0)

    /* Unconditional variant: the merge buffer is known-empty for this layer's
     * pixels (uncond), so the priority load+compare is dropped — the write
     * always wins. main is written unconditionally (selection guarantees
     * main_on); sub is gated by the loop-invariant sub_on. Each screen_x is
     * written at most once per layer, so no intra-layer overwrite hazard. */
#define EMIT_PIXELS_UNCOND(HAS_SUB) do { \
    for (int _i = 0; _i < count; _i++) { \
        int _px = hflip ? (7 - (start_px + _i)) : (start_px + _i); \
        uint8_t _cidx = indices[_px]; \
        if (_cidx == 0) continue; \
        uint16_t _rgb = (bpp == 8) ? cgram_render[_cidx] \
                                    : cgram_render[pal_base + _cidx]; \
        int _sx = screen_x + _i; \
        ctx->main_gp_lm[_sx] = gp_lm; \
        ctx->main_color[_sx] = _rgb; \
        if (HAS_SUB && sub_on) { \
            ctx->sub_gp[_sx] = gp; \
            ctx->sub_color[_sx] = _rgb; \
        } \
    } \
} while(0)

    if (ctx->sub_gp) BGC_INC(sub_emit_calls);

    if (ctx->uncond && ctx->main_on) {
        const bool sub_on = ctx->sub_on;
        if (ctx->sub_gp)
            EMIT_PIXELS_UNCOND(1);
        else
            EMIT_PIXELS_UNCOND(0);
    } else if (ctx->no_window) {
        const bool main_on = ctx->main_on;
        const bool sub_on  = ctx->sub_on;
        if (ctx->sub_gp)
            EMIT_PIXELS_NW(1);
        else
            EMIT_PIXELS_NW(0);
    } else {
        if (ctx->sub_gp)
            EMIT_PIXELS(1);
        else
            EMIT_PIXELS(0);
    }
#undef EMIT_PIXELS
#undef EMIT_PIXELS_NW
#undef EMIT_PIXELS_UNCOND
}

/* BG tilemap-row plan cache.
 *
 * Vertical scroll is constant within a frame, so the (up to) 8 output scanlines
 * that share a tile row read the *identical* tilemap entries and produce the same
 * per-column plan {tile, palette, prio, flip, screen_x, run length}. The column
 * loop below (tilemap address arithmetic + ppu.vram reads + field extraction)
 * therefore recomputes the same plan ~7/8 of the time. We decode the plan once per
 * tile row and replay it for that row's other scanlines; per-scanline pixel
 * decode/compositing (emit_tile_run) still runs every line since it depends on the
 * row-within-tile. Measured ~11% off BG on the overworld.
 *
 * Correctness: keyed on (tile_row_map, scroll_x, render_width) and invalidated at
 * frame start, so a replay can only occur for scanlines of the same frame that map
 * to the same tile row at the same scroll — exactly when the plan is identical.
 * 8x8 tiles only; 16x16 and bg2 HDMA distortion (per-scanline scroll) fall back to
 * the full loop. Assumes a single render context (the cache is global, not per
 * ctx_id) — auto-disabled otherwise. */
#ifndef PPU_BG_ROW_CACHE
#define PPU_BG_ROW_CACHE 1
#endif
#if PPU_BG_ROW_CACHE && PPU_NUM_RENDER_CONTEXTS != 1
#undef PPU_BG_ROW_CACHE
#define PPU_BG_ROW_CACHE 0
#endif

#if PPU_BG_ROW_CACHE
#define BG_ROW_PLAN_MAXCOL 48   /* >= max tile columns across render_width (~42) */
typedef struct {
    uint16_t tile_num;
    uint8_t  x_in_tile, pixels, prio_bit, palette;
    bool     hflip, vflip;
    uint16_t screen_x;
} BGRowPlanEntry;
static BGRowPlanEntry bg_row_plan[4][BG_ROW_PLAN_MAXCOL];
static struct {
    int row;       /* cached tile_row_map; -1 = invalid */
    int scroll_x;  /* plan is valid only for this horizontal scroll ... */
    int width;     /* ... and this render_width */
    int count;     /* number of entries in bg_row_plan[bg] */
} bg_row_cache[4];
#endif

#if PPU_BG_ROW_CACHE
/* A tile whose entire graphic (all bytes_per_tile bytes) is zero is palette
 * index 0 everywhere — fully transparent on every row (emit_tile_run would skip
 * every pixel via `if (_cidx == 0) continue`). Detected once while building the
 * row plan and elided from it, so it costs nothing on any of the tile row's
 * scanlines, not just the per-row blank-skip. On the overworld ~80% of BG tiles
 * are fully transparent, so this roughly doubles the row cache's gain. */
static inline bool tile_fully_blank(uint32_t tile_addr, int bytes_per_tile) {
    if (tile_addr + bytes_per_tile > VRAM_SIZE) return false;
    const uint8_t *p = &ppu.vram[tile_addr];
    uint8_t any = 0;
    for (int i = 0; i < bytes_per_tile; i++) any |= p[i];
    return any == 0;
}
#endif

/* Render a single BG layer scanline, merging directly into the priority
 * buffers via BGRenderCtx.  render_width controls how many screen pixels
 * to process (may be EB_VIEWPORT_WIDTH or SNES_WIDTH). */
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

    /* Per-scanline horizontal offset for BG2 (HDMA distortion emulation).
     * The distortion table only covers the native content rows. In a wide
     * viewport a FILL layer (e.g. the gas station's Giygas static) also renders
     * the letterbox gutter rows (scanline < 0 or >= the content height). Those
     * have no table entry, so leaving them undistorted made the gutter render
     * as a rigid block while the content churned. Reflect an out-of-range
     * scanline back into the table so every gutter row gets its own ripple
     * offset and churns like the content; reflection (vs. clamp/wrap) keeps the
     * offset continuous across the gutter/content seam. */
    if (bg_index == 1 && bg2_distortion_active) {
        int dist_y = scanline;
        if (dist_y < 0)
            dist_y = -dist_y;                                  /* reflect at top edge */
        else if (dist_y >= BATTLEBG_MAX_SCANLINES)
            dist_y = 2 * (BATTLEBG_MAX_SCANLINES - 1) - dist_y; /* reflect at bottom edge */
        if (dist_y < 0) dist_y = 0;                            /* safety clamp */
        else if (dist_y >= BATTLEBG_MAX_SCANLINES) dist_y = BATTLEBG_MAX_SCANLINES - 1;
        scroll_x = (scroll_x + bg2_scanline_hoffset[dist_y]) & 0x3FF;
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

#if PPU_BG_ROW_CACHE
    /* Replay the cached column plan when this scanline shares its tile row with an
     * already-decoded scanline of this frame (the common case: 7 of every 8). */
    bool row_cacheable = !big_tiles
                         && !(bg_index == 1 && bg2_distortion_active)
                         && render_width <= 8 * BG_ROW_PLAN_MAXCOL;
    if (row_cacheable
        && bg_row_cache[bg_index].row == tile_row_map
        && bg_row_cache[bg_index].scroll_x == scroll_x
        && bg_row_cache[bg_index].width == render_width) {
        const BGRowPlanEntry *pl = bg_row_plan[bg_index];
        int n = bg_row_cache[bg_index].count;
        for (int i = 0; i < n; i++) {
            int eff_py = pl[i].vflip ? (7 - pixel_y_in_tile) : pixel_y_in_tile;
            emit_tile_run(pl[i].tile_num, eff_py, pl[i].x_in_tile, pl[i].pixels,
                          pl[i].hflip, pl[i].screen_x, pl[i].prio_bit, pl[i].palette,
                          bpp, tile_data_base, bytes_per_tile, ctx);
        }
        return;
    }
    int plan_n = 0;
#endif

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
#if PPU_BG_ROW_CACHE
            /* While building the plan, elide fully-transparent tiles (see
             * tile_fully_blank): they emit nothing on any row, so dropping them
             * from the plan saves their emit overhead on the whole tile row. */
            if (row_cacheable && tile_fully_blank(
                    tile_data_base + (uint32_t)tile_num * bytes_per_tile,
                    bytes_per_tile)) {
                BGC_INC(elided_tiles);
            } else {
                emit_tile_run(tile_num, eff_py, x_in_tile, pixels_this_tile,
                             hflip, screen_x, prio_bit, palette,
                             bpp, tile_data_base, bytes_per_tile, ctx);
                if (row_cacheable && plan_n < BG_ROW_PLAN_MAXCOL) {
                    BGRowPlanEntry *e = &bg_row_plan[bg_index][plan_n++];
                    e->tile_num  = tile_num;
                    e->x_in_tile = (uint8_t)x_in_tile;
                    e->pixels    = (uint8_t)pixels_this_tile;
                    e->prio_bit  = prio_bit;
                    e->palette   = palette;
                    e->hflip     = hflip;
                    e->vflip     = vflip;
                    e->screen_x  = (uint16_t)screen_x;
                }
            }
#else
            emit_tile_run(tile_num, eff_py, x_in_tile, pixels_this_tile,
                         hflip, screen_x, prio_bit, palette,
                         bpp, tile_data_base, bytes_per_tile, ctx);
#endif
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
#if PPU_BG_ROW_CACHE
    /* Commit the plan for replay by this row's other scanlines. Only when it fit
     * (plan_n < MAXCOL); if it ever overflowed we leave the cache invalid and just
     * recompute — correct, only slower. */
    if (row_cacheable && plan_n < BG_ROW_PLAN_MAXCOL) {
        bg_row_cache[bg_index].row      = tile_row_map;
        bg_row_cache[bg_index].scroll_x = scroll_x;
        bg_row_cache[bg_index].width    = render_width;
        bg_row_cache[bg_index].count    = plan_n;
    }
#endif
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

/* Object sizes based on OBSEL: {small_w, small_h}, {large_w, large_h}. */
static const int obj_sizes[8][2][2] = {
    {{8, 8}, {16, 16}},
    {{8, 8}, {32, 32}},
    {{8, 8}, {64, 64}},
    {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}},
    {{32, 32}, {64, 64}},
    {{16, 32}, {32, 64}},
    {{16, 32}, {32, 32}},
};

/* ---- OBJ per-frame sprite bucketing (Candidate 2) -------------------------
 * render_obj_scanline previously rescanned all 128 OAM sprites on EVERY
 * scanline (128 x ~224 = ~28.7k sprite-tests/frame, the vast majority rejected
 * by the row<0||row>=h test).  Instead, bucket sprite geometry once per frame
 * by the 8-scanline band(s) each sprite covers; each scanline then iterates
 * only the sprites whose band it falls in.  Build is O(128) once per frame;
 * per-scanline cost drops from 128 to (#sprites overlapping that band).  Same
 * redundancy shape as bg_row_cache.  Priority (lower OAM index wins) is
 * preserved by appending in 127->0 order so the low index is emitted last.
 * Global arrays => single render context only, auto-disabled otherwise. */
#ifndef PPU_OBJ_BUCKET
#define PPU_OBJ_BUCKET 1
#endif
#if PPU_OBJ_BUCKET && PPU_NUM_RENDER_CONTEXTS != 1
#undef PPU_OBJ_BUCKET
#define PPU_OBJ_BUCKET 0
#endif

#if PPU_OBJ_BUCKET
#define OBJ_BAND_SHIFT 3                               /* 8-scanline bands */
#define OBJ_NUM_BANDS  ((256 >> OBJ_BAND_SHIFT) + 1)   /* covers scanline 0..255 */

/* Per-sprite geometry, precomputed for sprites that intersect the rendered
 * scanline range. Mirrors the values render_obj_scanline used to recompute. */
typedef struct {
    int16_t  x;          /* spr_x (screen space, incl. sprite_x_offset) */
    int16_t  y;          /* spr_y (scanline space, incl. sprite_y_offset) */
    uint8_t  w, h;
    uint8_t  attr;       /* raw OAM attr (vflip/hflip/prio/pal/tile-hi) */
    uint16_t tile_num;   /* spr->tile | ((attr & 1) << 8) */
} ObjActive;
static ObjActive obj_active[128];
static uint8_t   obj_band[OBJ_NUM_BANDS][128]; /* slot indices, priority order */
static uint8_t   obj_band_count[OBJ_NUM_BANDS];

/* Build the per-frame sprite buckets. Call once per frame before the scanline
 * loop (OAM/obsel are frame-constant in this renderer). */
static void PPU_HOT_FUNC(build_obj_buckets)(void) {
    for (int b = 0; b < OBJ_NUM_BANDS; b++) obj_band_count[b] = 0;

    int size_sel = (ppu.obsel >> 5) & 0x07;

    /* 127->0 so each band list is ordered high-index-first; the inner emit
     * overwrites, so the last (lowest-index) sprite wins — matching HW. */
    int n = 0;
    for (int i = 127; i >= 0; i--) {
        int hi_byte = i / 4;
        int hi_shift = (i % 4) * 2;
        uint8_t hi_bits = (ppu.oam_hi[hi_byte] >> hi_shift) & 0x03;
        int size_bit = (hi_bits >> 1) & 1;

        int spr_x = ppu.oam_full_x[i] + ppu.sprite_x_offset;
        int spr_y = ppu.oam_full_y[i] + ppu.sprite_y_offset;
        int w = obj_sizes[size_sel][size_bit][0];
        int h = obj_sizes[size_sel][size_bit][1];

        /* Cull sprites that touch no rendered pixel. Horizontal: fully off
         * either edge of the line buffer. Vertical: outside [0, banded range). */
        if (spr_x >= LINE_BUF_WIDTH || spr_x + w <= 0) continue;
        int y0 = spr_y;
        int y1 = spr_y + h;                            /* exclusive */
        if (y1 <= 0 || y0 >= (OBJ_NUM_BANDS << OBJ_BAND_SHIFT)) continue;

        OAMEntry *spr = &ppu.oam[i];
        int slot = n++;
        obj_active[slot].x        = (int16_t)spr_x;
        obj_active[slot].y        = (int16_t)spr_y;
        obj_active[slot].w        = (uint8_t)w;
        obj_active[slot].h        = (uint8_t)h;
        obj_active[slot].attr     = spr->attr;
        obj_active[slot].tile_num = spr->tile | ((uint16_t)(spr->attr & 1) << 8);

        int bstart = y0 < 0 ? 0 : (y0 >> OBJ_BAND_SHIFT);
        int bend = (y1 - 1) >> OBJ_BAND_SHIFT;
        if (bend >= OBJ_NUM_BANDS) bend = OBJ_NUM_BANDS - 1;
        for (int b = bstart; b <= bend; b++)
            obj_band[b][obj_band_count[b]++] = (uint8_t)slot;
    }
}
#endif /* PPU_OBJ_BUCKET */

/* Render OBJ (sprites) for one scanline into separate color/prio arrays.
 * Returns true if any sprite overlapped this scanline (i.e. obj_prio may hold
 * nonzero entries). When false, the compositor skips its per-pixel obj_prio
 * load entirely. */
static bool PPU_HOT_FUNC(render_obj_scanline)(int scanline, uint16_t *obj_color, uint8_t *obj_prio, TileRowCacheEntry *tile_cache) {
    /* When vertical centering is active, sprites only render within the
     * SNES-visible scanline range. The border scanlines (above/below the
     * centered area) show only BG content — no sprites. This prevents
     * "parked" sprites at high OAM Y values from wrapping into view. */
    if (ppu.sprite_y_offset) {
        int snes_sl = scanline - ppu.sprite_y_offset;
        if (snes_sl < 0 || snes_sl >= SNES_HEIGHT)
            return false;
    }

    bool any = false;

    uint32_t obj_base = (uint32_t)(ppu.obsel & 0x07) * 0x4000; /* byte address */
    uint32_t obj_gap = (uint32_t)((ppu.obsel >> 3) & 0x03) * 0x2000 + 0x2000;

#if PPU_OBJ_BUCKET
    int band = scanline >> OBJ_BAND_SHIFT;
    if (band < 0 || band >= OBJ_NUM_BANDS) return false;
    uint8_t bcnt = obj_band_count[band];
    const uint8_t *blist = obj_band[band];

    /* Iterate only sprites that overlap this band (priority order preserved). */
    for (int k = 0; k < bcnt; k++) {
        const ObjActive *a = &obj_active[blist[k]];

        int spr_x = a->x;
        int spr_y = a->y;
        int w = a->w;
        int h = a->h;

        /* Band is 8 scanlines wide; precise per-scanline overlap still tested. */
        int row = scanline - spr_y;
        if (row < 0 || row >= h) continue;
        any = true;

        uint8_t attr = a->attr;
        uint16_t tile_num = a->tile_num;
        bool vflip = (attr >> 7) & 1;
        bool hflip = (attr >> 6) & 1;
        uint8_t spr_prio = (attr >> 4) & 3;
        uint8_t spr_pal = (attr >> 1) & 7;
#else
    int size_sel = (ppu.obsel >> 5) & 0x07;

    /* Scan sprites in reverse order (lower index = higher priority) */
    for (int i = 127; i >= 0; i--) {
        OAMEntry *spr = &ppu.oam[i];

        /* Get extended attributes from high table */
        int hi_byte = i / 4;
        int hi_shift = (i % 4) * 2;
        uint8_t hi_bits = (ppu.oam_hi[hi_byte] >> hi_shift) & 0x03;
        int size_bit = (hi_bits >> 1) & 1;

        /* Use full 16-bit X coordinate for expanded viewport support.
         * The SNES 9-bit OAM X can't represent X in [256, EB_VIEWPORT_WIDTH). */
        int spr_x = ppu.oam_full_x[i] + ppu.sprite_x_offset;
        /* Use full 16-bit Y coordinate, mirroring the oam_full_x approach.
         * This eliminates 8-bit wrap issues on 240-line viewports. */
        int spr_y = ppu.oam_full_y[i] + ppu.sprite_y_offset;

        int w = obj_sizes[size_sel][size_bit][0];
        int h = obj_sizes[size_sel][size_bit][1];

        /* Check if sprite is on this scanline */
        int row = scanline - spr_y;
        if (row < 0 || row >= h) continue;
        any = true;

        bool vflip = (spr->attr >> 7) & 1;
        bool hflip = (spr->attr >> 6) & 1;
        uint8_t spr_prio = (spr->attr >> 4) & 3;
        uint8_t spr_pal = (spr->attr >> 1) & 7;
        uint16_t tile_num = spr->tile | ((uint16_t)(spr->attr & 1) << 8);
#endif /* PPU_OBJ_BUCKET */

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
    return any;
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

        int wx_offset = wide_mode ? -EB_VIEWPORT_PAD_LEFT : 0;

        /* Outer bounds for inverted (outside-the-window) spans.  In wide mode
         * these extend to the full viewport (in SNES-space coordinates) so the
         * padding columns outside the native 0-255 range are also masked —
         * otherwise the blanking region (e.g. the oval window spotlight) stops
         * short of the viewport edges and the filling BG shows through. */
        int inv_lo = wide_mode ? wx_offset : 0;
        int inv_hi = wide_mode ? (render_width - 1 + wx_offset) : 255;

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

        if (tm_w1_noninv | ts_w1_noninv) {
            /* Extend a non-inverted window into the wide-mode gutters when it
             * touches a native edge, so the mask is continuous past 0/255
             * (mirrors the inverted branch's inv_lo/inv_hi extension). */
            int _l = (wide_mode && w1_left  <= 0)   ? inv_lo : (int)w1_left;
            int _r = (wide_mode && w1_right >= 255) ? inv_hi : (int)w1_right;
            BATCH_SPAN(_l, _r, tm_w1_noninv, ts_w1_noninv);
        }

        if (tm_w1_inv | ts_w1_inv) {
            if (w1_left - 1 >= inv_lo)
                BATCH_SPAN(inv_lo, w1_left - 1, tm_w1_inv, ts_w1_inv);
            if (w1_right + 1 <= inv_hi)
                BATCH_SPAN(w1_right + 1, inv_hi, tm_w1_inv, ts_w1_inv);
        }

        if (tm_w2_noninv | ts_w2_noninv) {
            /* Same gutter extension for the window-2 non-inverted group. */
            int _l = (wide_mode && w2_left  <= 0)   ? inv_lo : (int)w2_left;
            int _r = (wide_mode && w2_right >= 255) ? inv_hi : (int)w2_right;
            BATCH_SPAN(_l, _r, tm_w2_noninv, ts_w2_noninv);
        }

        if (tm_w2_inv | ts_w2_inv) {
            if (w2_left - 1 >= inv_lo)
                BATCH_SPAN(inv_lo, w2_left - 1, tm_w2_inv, ts_w2_inv);
            if (w2_right + 1 <= inv_hi)
                BATCH_SPAN(w2_right + 1, inv_hi, tm_w2_inv, ts_w2_inv);
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
                /* Wide-mode gutter columns (wx<0 or wx>255) have no native
                 * window coordinate. Clamp to the nearest native edge so the
                 * window's state at the edge extends continuously into the
                 * gutter, for both inverted (e.g. oval window blanking) and
                 * non-inverted windows. Matches the color-math dual path and
                 * the single-window inv_lo/inv_hi extensions above. */
                int cwx = wx < 0 ? 0 : (wx > 255 ? 255 : wx);
                bool in_w1 = ((uint8_t)cwx >= w1_left && (uint8_t)cwx <= w1_right);
                bool in_w2 = ((uint8_t)cwx >= w2_left && (uint8_t)cwx <= w2_right);
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

        int wx_offset = wide_mode ? -EB_VIEWPORT_PAD_LEFT : 0;
        int inv_lo = wide_mode ? wx_offset : 0;
        int inv_hi = wide_mode ? (render_width - 1 + wx_offset) : 255;

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
                    /* Extend the inside-span into the gutters when the window
                     * touches a native edge, so wide-mode padding columns get
                     * the same color-math treatment (mirrors the inverted branch). */
                    int _l = (wide_mode && w1_left  <= 0)   ? inv_lo : (int)w1_left;
                    int _r = (wide_mode && w1_right >= 255) ? inv_hi : (int)w1_right;
                    CM_SPAN(_l, _r, val_inside);
                } else {
                    if (w1_left - 1 >= inv_lo) CM_SPAN(inv_lo, w1_left - 1, val_inside);
                    if (w1_right + 1 <= inv_hi) CM_SPAN(w1_right + 1, inv_hi, val_inside);
                }
            } else if (!cm_w1_en && cm_w2_en) {
                memset(cm_prevented_line, val_outside, render_width);
                if (!cm_w2_inv) {
                    /* Same gutter extension for the window-2 single-window case. */
                    int _l = (wide_mode && w2_left  <= 0)   ? inv_lo : (int)w2_left;
                    int _r = (wide_mode && w2_right >= 255) ? inv_hi : (int)w2_right;
                    CM_SPAN(_l, _r, val_inside);
                } else {
                    if (w2_left - 1 >= inv_lo) CM_SPAN(inv_lo, w2_left - 1, val_inside);
                    if (w2_right + 1 <= inv_hi) CM_SPAN(w2_right + 1, inv_hi, val_inside);
                }
            } else {
                for (int x = 0; x < render_width; x++) {
                    int wx = x + wx_offset;
                    /* The battle swirl uses BOTH color windows (wobjsel=0xA0 ->
                     * cm_w1_en && cm_w2_en), so it lands here. Window coords are
                     * native 0-255; wide-mode gutter columns (wx<0 or wx>255)
                     * have no native coordinate. Clamp to the nearest native
                     * edge so the window's state at the edge extends continuously
                     * into the gutter — equivalent to the inv_lo/inv_hi gutter
                     * extension in the single-window branches above. */
                    int cwx = wx < 0 ? 0 : (wx > 255 ? 255 : wx);
                    bool in_w1 = ((uint8_t)cwx >= w1_left && (uint8_t)cwx <= w1_right);
                    bool in_w2 = ((uint8_t)cwx >= w2_left && (uint8_t)cwx <= w2_right);
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
static pixel_t  line_out_ctx[PPU_NUM_RENDER_CONTEXTS][EB_VIEWPORT_WIDTH] PPU_LINEBUF_ATTR;
static uint16_t best_bg_color_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] PPU_LINEBUF_ATTR;
static uint16_t best_bg_gp_lm_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] PPU_LINEBUF_ATTR;
static uint16_t sub_bg_color_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] PPU_LINEBUF_ATTR;
static uint8_t  sub_bg_gp_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] PPU_LINEBUF_ATTR;
static uint16_t obj_color_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] PPU_LINEBUF_ATTR;
static uint8_t  obj_prio_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] PPU_LINEBUF_ATTR;
static uint8_t  eff_tm_line_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] PPU_LINEBUF_ATTR;
static uint8_t  eff_ts_line_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] PPU_LINEBUF_ATTR;
static uint8_t  cm_prevented_line_ctx[PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] PPU_LINEBUF_ATTR;

/* Temp buffers for wide-mode non-filling layer render path.
 * Promoted from stack to static to avoid core 1 stack overflow (4KB limit). */
static uint16_t temp_color_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH] PPU_LINEBUF_ATTR;
static uint16_t temp_gp_lm_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH] PPU_LINEBUF_ATTR;
static uint16_t temp_sub_color_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH] PPU_LINEBUF_ATTR;
static uint8_t  temp_sub_gp_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH] PPU_LINEBUF_ATTR;
static uint8_t  temp_tm_all_ctx[PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH] PPU_LINEBUF_ATTR;

void PPU_HOT_FUNC(ppu_render_frame_ex)(int ctx_id, int y_start, int y_end,
                         int y_stride, scanline_callback_t send_scanline) {
#if PPU_BG_ROW_CACHE
    /* Invalidate the tilemap-row plan cache at frame start. Within a frame scroll
     * is constant, so keying replays on tile_row_map (+scroll_x/width) is correct;
     * across frames the plan may be stale, hence this reset. */
    bg_row_cache[0].row = bg_row_cache[1].row =
        bg_row_cache[2].row = bg_row_cache[3].row = -1;
#endif
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
            memset(line_out, 0, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
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
     * smaller than SNES) so that filling layers render at EB_VIEWPORT_WIDTH
     * while non-filling layers are centered/cropped correctly. */
    bool wide_mode = false;
    if (EB_VIEWPORT_WIDTH != SNES_WIDTH || EB_VIEWPORT_HEIGHT != SNES_HEIGHT) {
        for (int bg = 0; bg < 4 && !wide_mode; bg++) {
            if (!(layers_needed & (1 << bg))) continue;
            if (bg_bpp[bg] == 0) continue;
            if ((ppu.bg_sc[bg] & 0x01) || ppu.bg_viewport_fill[bg])
                wide_mode = true;
        }
    }

    int render_height = EB_VIEWPORT_HEIGHT;
    int render_width = EB_VIEWPORT_WIDTH;
    int fb_x_offset = 0;
    int fb_y_offset = 0;

    if (!wide_mode && (EB_VIEWPORT_WIDTH != SNES_WIDTH || EB_VIEWPORT_HEIGHT != SNES_HEIGHT)) {
        /* No filling layers — render at SNES resolution, centered/cropped.
         * Always render SNES_WIDTH pixels into line buffers, then crop/pad
         * when writing to the output. */
        render_height = EB_VIEWPORT_HEIGHT < SNES_HEIGHT ? EB_VIEWPORT_HEIGHT : SNES_HEIGHT;
        render_width = SNES_WIDTH;
        fb_x_offset = EB_VIEWPORT_PAD_LEFT;  /* negative when viewport < SNES */
        fb_y_offset = EB_VIEWPORT_HEIGHT > SNES_HEIGHT ? (EB_VIEWPORT_HEIGHT - SNES_HEIGHT) / 2 : 0;
    }

#ifdef PPU_PROFILE
    uint32_t prof_clear = 0, prof_bg = 0, prof_obj = 0;
    uint32_t prof_win = 0, prof_composite = 0, prof_send = 0;
    uint32_t prof_iter = 0;
    PROF_SECTION(total);
#endif

    /* Frame-constant compositing values (hoisted out of scanline loop) */
    uint16_t bm = brightness * 17; /* 0→0, 1→17, ... 14→238, 15→255 */
    bool bg3prio = (ppu.bgmode & BGMODE_BG3_PRIO) && mode == 1;
    uint16_t backdrop = cgram_render[0];

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

    /* #4: when the compositor covers the entire output line (wide mode — no
     * left/right black borders) it overwrites every line_out pixel each
     * scanline, so the per-scanline clear is dead work. This mirrors the
     * composite loop's coverage exactly: it writes line_out[x + fb_x_offset]
     * for x in [x_start, render_width), so full coverage ⟺ fb_x_offset == 0
     * && render_width == EB_VIEWPORT_WIDTH. Skipping it removes ~150 KB/frame
     * of memset (640 B × 240 lines). Loop-invariant, hoisted here. */
    const bool line_out_full_cover =
        (fb_x_offset == 0 && render_width == EB_VIEWPORT_WIDTH);

    /* Wide-mode gutter bounds (in line_out columns). A centered main layer
     * leaves only backdrop in [0,gutter_lo) and [gutter_hi,WIDTH). Where active
     * color math has a sub-screen layer, the composite loop forces such a
     * backdrop gutter to a black, math-eligible base so the filling sub layer
     * (e.g. the gas station's BG2 static) blends onto it and covers the full
     * screen. Self-limiting: scenes without sub-screen color math never trigger
     * it. In non-wide mode these collapse to the full width (no gutter). */
    const int gutter_lo = wide_mode ? EB_VIEWPORT_PAD_LEFT : 0;
    const int gutter_hi = wide_mode ? EB_VIEWPORT_PAD_LEFT + SNES_WIDTH
                                    : EB_VIEWPORT_WIDTH;

    /* eff_tm_line/eff_ts_line drive emit_tile_run's per-pixel layer-enable
     * check, and are refilled with base_tm/base_ts every scanline below. But
     * with no windows at all and no per-scanline TM/TS HDMA, base_tm/base_ts
     * are frame-constant (ppu.tm/ppu.ts) and nothing mutates these buffers
     * mid-frame (precompute_window_masks only runs when windows are active;
     * emit only reads them) — so the per-scanline fill is identical 240×/frame.
     * Fill once here and skip it in the loop. Removes ~2 × 320 × 240 ≈ 150
     * KB/frame of memset (each fill is render_width bytes). Frame-constant
     * predicate, hoisted. */
    const bool eff_lines_frame_constant = !has_windows && !ppu.tm_hdma_active;
    if (eff_lines_frame_constant) {
        memset(eff_tm_line, ppu.tm, render_width);
        memset(eff_ts_line, ppu.ts, render_width);
    }

#if PPU_OBJ_BUCKET
    /* Bucket sprites by scanline band once per frame; render_obj_scanline then
     * iterates only the sprites overlapping each scanline (see build_obj_buckets). */
    build_obj_buckets();
#endif

    PROF_SECTION(iter);
    for (int out_y = y_start; out_y < y_end; out_y += y_stride) {
        /* Border scanline — outside the renderable area */
        if (out_y < render_y_start || out_y >= render_y_end) {
            memset(line_out, 0, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
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
        if (layers_needed & LAYER_OBJ) {
#if PPU_OBJ_BUCKET
            /* Skip the clear when no sprite touches this scanline's band: nothing
             * writes obj_prio this line and the compositor skips reading it
             * (render_obj_scanline returns false -> scanline_has_obj false), so
             * stale contents are never observed. Conservative — an occupied band
             * with no exact-scanline overlap still clears, which is harmless. */
            int _oband = scanline >> OBJ_BAND_SHIFT;
            if (_oband >= 0 && _oband < OBJ_NUM_BANDS && obj_band_count[_oband] > 0)
#endif
                memset(obj_prio, 0, LINE_BUF_WIDTH);
        }

        /* Clear output line (handles left/right black borders). Skipped in
         * wide mode where the compositor overwrites every pixel (see #4). */
        if (!line_out_full_cover)
            memset(line_out, 0, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
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
        /* Skip the fill when it was hoisted out of the loop (see
         * eff_lines_frame_constant above). */
        if (!has_layer_windows && !eff_lines_frame_constant) {
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
        BGC_INC(scanlines);
        /* best_bg_* was just cleared, so the first BG layer to write it can skip
         * the per-pixel priority compare (uncond). Cleared once a layer writes. */
        bool merged_empty = true;
        for (int bg = 0; bg < 4; bg++) {
            if (!(layers_needed & (1 << bg))) continue;
            if (bg_bpp[bg] == 0) continue;

            /* Skip layers not enabled on either screen (window-independent) */
            uint8_t layer_bit = 1 << bg;
            if (no_windows && !(base_tm & layer_bit) &&
                !(need_sub && (base_ts & layer_bit)))
                continue;
            BGC_INC(layers);

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
                /* eff_tm_line/eff_ts_line are uniform (= base_tm/base_ts) when
                 * no windows are active, so the per-pixel window mask reduces to
                 * these constants — enables the window-mask-free emit variant. */
                .no_window = no_windows,
                .main_on = (base_tm & layer_bit) != 0,
                .sub_on  = (base_ts & layer_bit) != 0,
                /* First layer into the just-cleared best_bg writes unconditionally.
                 * Only valid window-free (uncond bypasses the per-pixel mask). */
                .uncond = (uint8_t)(no_windows && merged_empty),
                .tile_cache = tile_cache,
            };

            if (layer_fills) {
                render_bg_scanline(bg, eff_scanline, bg_bpp[bg],
                                   &ctx, EB_VIEWPORT_WIDTH);
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
                /* No window masking in temp — apply when merging. temp_tm_all
                 * is all-0xFF, so the layer always writes both screens here;
                 * use the window-mask-free emit variant. */
                temp_ctx.tm_line = temp_tm_all;
                temp_ctx.ts_line = temp_tm_all;
                temp_ctx.no_window = 1;
                temp_ctx.main_on = 1;
                temp_ctx.sub_on = 1;
                /* temp_gp_lm is memset to 0 above and each screen_x is written
                 * once per layer, so every temp write is into an empty slot. */
                temp_ctx.uncond = 1;

                render_bg_scanline(bg, eff_scanline, bg_bpp[bg],
                                   &temp_ctx, SNES_WIDTH);

                /* Copy visible portion into main merged buffers */
                int pad = EB_VIEWPORT_PAD_LEFT;
                int src_start = pad < 0 ? -pad : 0;
                int dst_start = pad > 0 ?  pad : 0;
                int count = SNES_WIDTH - src_start;
                if (dst_start + count > EB_VIEWPORT_WIDTH)
                    count = EB_VIEWPORT_WIDTH - dst_start;
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
                    for (int x = dst_start + count; x < EB_VIEWPORT_WIDTH; x++) {
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

            /* This layer has now written best_bg, so later layers must use the
             * priority compare. (The off-screen goto above skips this, keeping
             * merged_empty true for the next real layer.) */
            merged_empty = false;

            /* Mosaic: horizontal — replicate leftmost pixel in each block.
             * Operates on the merged buffer for this layer's pixels. */
            if (bg_mosaic) {
                int block = mosaic_size + 1;
                uint8_t gp0 = bg_gp[bg][0];
                uint8_t gp1 = bg_gp[bg][1];
                for (int x = 0; x < EB_VIEWPORT_WIDTH; x += block) {
                    uint16_t ref_color = best_bg_color[x];
                    uint8_t ref_gp = GP_LM_GP(best_bg_gp_lm[x]);
                    /* Only replicate if this pixel belongs to this layer */
                    if (ref_gp != gp0 && ref_gp != gp1) continue;
                    uint16_t ref_packed = ref_gp | ((uint16_t)layer_bit << 8);
                    for (int dx = 1; dx < block && (x + dx) < EB_VIEWPORT_WIDTH; dx++) {
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
        bool scanline_has_obj = false;
        if (layers_needed & LAYER_OBJ) {
            scanline_has_obj = render_obj_scanline(scanline, obj_color, obj_prio, tile_cache);
        }
        PROF_END(obj, prof_obj);

        /* --- Compositing: OBJ vs merged BG --- */
        PROF_SECTION(comp);
        {
            /* Pre-compute valid pixel range to eliminate per-pixel bounds check.
             * line_out is pre-cleared to black, so border pixels are already set. */
            int x_start = fb_x_offset < 0 ? -fb_x_offset : 0;
            int x_end = render_width;
            if (fb_x_offset + x_end > EB_VIEWPORT_WIDTH)
                x_end = EB_VIEWPORT_WIDTH - fb_x_offset;

            /* Hoist loop-invariant OBJ enable check. When no sprite overlapped
             * this scanline (obj_prio all-0), drop the per-pixel obj_prio load
             * entirely — scanline_has_obj folds into the enable flags. */
            bool obj_main_en = scanline_has_obj && (no_windows ? (base_tm & LAYER_OBJ) != 0 : false);
            bool obj_sub_en = scanline_has_obj && (no_windows ? (base_ts & LAYER_OBJ) != 0 : false);

            /* Whole-row gutter: a scanline outside the centered SNES content
             * band (top/bottom letterbox in a taller viewport) has no centered
             * main layer at any column, so every pixel is a gutter pixel. Only
             * relevant when a sub-screen color-math layer can fill it. */
            bool row_is_vgutter = wide_mode && need_sub &&
                (snes_scanline < 0 || snes_scanline >= SNES_HEIGHT);

            for (int x = x_start; x < x_end; x++) {
                uint16_t color;
                uint8_t main_layer;

                /* OBJ check: window-masked or constant. scanline_has_obj gates
                 * the load on both paths (no_windows folds into obj_main_en). */
                uint8_t obj_pk;
                if (no_windows)
                    obj_pk = obj_main_en ? obj_prio[x] : 0;
                else
                    obj_pk = (scanline_has_obj && (eff_tm_line[x] & LAYER_OBJ)) ? obj_prio[x] : 0;

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

                /* Color math eligibility */
                bool math_eligible = (main_layer & math_layers) != 0;

                /* Wide-mode gutter fill: a backdrop-only gutter column/row has
                 * no centered main content for a sub-screen color-math layer to
                 * blend onto, so the layer would stop at the native edge. When
                 * color math is active with a sub-screen layer, force the gutter
                 * to a black, math-eligible base so the filling sub layer (e.g.
                 * the gas station's BG2 static) blends onto it and covers the
                 * full screen. Gated on color_math_active, so non-math phases
                 * (e.g. the closing fade-to-white) leave the gutter on the
                 * backdrop, which then follows the palette. */
                if (color_math_active && need_sub && (main_layer & 0x20) &&
                    (row_is_vgutter || x < gutter_lo || x >= gutter_hi)) {
                    color = 0;
                    math_eligible = true;
                }

                /* Color math */
                if (color_math_active && math_eligible &&
                    (no_windows || !cm_prevented_line[x])) {
                    uint16_t sub_color;
                    if (need_sub) {
                        uint8_t sub_obj_pk;
                        if (no_windows)
                            sub_obj_pk = obj_sub_en ? obj_prio[x] : 0;
                        else
                            sub_obj_pk = (scanline_has_obj && (eff_ts_line[x] & LAYER_OBJ)) ? obj_prio[x] : 0;
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
    PROF_END(iter, prof_iter);

#ifdef PPU_PROFILE
    ppu_profile.total = (uint32_t)(platform_timer_ticks() - _prof_total);
    ppu_profile.iter = prof_iter;
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
    ppu_render_frame_ex(0, 0, EB_VIEWPORT_HEIGHT, 1, send_scanline);

#ifdef PPU_PROFILE
    /* Periodic BG work report — averaged over BG_PROF_FRAMES frames, with the
     * last frame's phase timings folded in (0.1 ms units, same scale as the
     * on-screen overlay) so the monitor line is self-contained. Diagnostic only. */
    if (++g_bgc.frames >= BG_PROF_FRAMES) {
        uint32_t f   = g_bgc.frames;
        uint32_t lookups = g_bgc.cache_hits + g_bgc.cache_misses;
        uint32_t hitpct  = lookups ? (g_bgc.cache_hits * 100u) / lookups : 0;
        uint32_t sl  = g_bgc.scanlines ? g_bgc.scanlines : 1;
        uint32_t div = (uint32_t)(platform_timer_ticks_per_sec() / 10000);
        if (div == 0) div = 1;
        printf("BGPROF/%lufr: lines=%lu layers/line=%lu.%02lu emit=%lu blank=%lu "
               "opaque=%lu sub=%lu "
               "decode=%lu hit=%lu%% px=%lu (px/line=%lu) elided=%lu | "
               "CLR=%lu BG=%lu OBJ=%lu WIN=%lu COMP=%lu SND=%lu TOTAL=%lu (0.1ms)\n",
               (unsigned long)f,
               (unsigned long)(g_bgc.scanlines / f),
               (unsigned long)(g_bgc.layers * 100u / sl / 100u),
               (unsigned long)(g_bgc.layers * 100u / sl % 100u),
               (unsigned long)(g_bgc.emit_calls / f),
               (unsigned long)(g_bgc.blank_skips / f),
               (unsigned long)(g_bgc.opaque_runs / f),
               (unsigned long)(g_bgc.sub_emit_calls / f),
               (unsigned long)(g_bgc.cache_misses / f),
               (unsigned long)hitpct,
               (unsigned long)(g_bgc.pixels / f),
               (unsigned long)(g_bgc.pixels / sl),
               (unsigned long)(g_bgc.elided_tiles / f),
               (unsigned long)(ppu_profile.clear / div),
               (unsigned long)(ppu_profile.bg / div),
               (unsigned long)(ppu_profile.obj / div),
               (unsigned long)(ppu_profile.win / div),
               (unsigned long)(ppu_profile.composite / div),
               (unsigned long)(ppu_profile.send / div),
               (unsigned long)(ppu_profile.total / div));
        BGCounters z = {0};
        g_bgc = z;
    }
#endif
}
