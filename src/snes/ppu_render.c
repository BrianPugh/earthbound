#include "snes/ppu.h"
#include "game/battle_bg.h"
#include "include/binary.h"
#include "platform/platform.h"
#include <string.h>
#include <stdio.h>

/* Embedded ports can define these at build time to optimize rendering:
 *
 *   EB_PPU_RAM_SECTION — section name for placing hot functions in fast memory.
 *     Examples: ".time_critical" (RP2040), ".dtcm" (STM32 DTCM-RAM),
 *              ".iram1" (ESP32).  Eliminates instruction cache misses
 *              for the decode + scanline render inner loops (~3KB).
 *
 *   EB_PPU_FORCE_SPEED_OPT — when defined, forces -O3 for this file even when
 *     the library is compiled with -Os.  Useful when the port uses -Os
 *     globally to reduce code size / cache pressure, but still wants
 *     maximum speed for the PPU rendering hot path. */
#ifdef EB_PPU_FORCE_SPEED_OPT
#pragma GCC optimize("O3")
#endif

#ifdef EB_PPU_RAM_SECTION
#define PPU_HOT_FUNC(name) \
    __attribute__((section(EB_PPU_RAM_SECTION "." #name))) name
#else
#define PPU_HOT_FUNC(name) name
#endif

/* EB_PPU_LINEBUF_ATTR — embedded ports can route the per-scanline working buffers
 * into fast memory (e.g. STM32 DTCM) by defining this at build time. Defaults
 * to nothing on desktop, so the buffers stay in normal BSS. */
#ifndef EB_PPU_LINEBUF_ATTR
#define EB_PPU_LINEBUF_ATTR
#endif

#ifdef EB_PPU_PROFILE
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

/* Tile row cache entry for bitplane decode caching. 12 bytes, 4-aligned so
 * indices[] (offset 4) can be read and written as two words. */
typedef struct __attribute__((aligned(4))) {
    uint16_t key;       /* TILE_CACHE_KEY(tile_addr, pixel_y), TILE_CACHE_EMPTY = invalid */
    uint8_t opaque;     /* 1 = 2bpp/4bpp row with no transparent pixel; else 0 */
    uint8_t pad;
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
    /* Merged main screen, one 32-bit word per pixel:
     *   [31:16] color (RGB565)   [15:8] layer bitmask   [7:0] global priority
     * Interleaving color with gp|lm halves the memory ops in the emit and
     * composite hot loops (1 str/ldr instead of 2 strh/ldrh into two separate
     * arrays = two cache lines per pixel). */
    uint32_t *main;
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
     * temp-buffer layer (both buffers enter each use all-zero via consumer
     * store-back + a once-per-frame clear). Requires no_window + main_on;
     * falls back to the windowed/NW path otherwise. */
    uint8_t uncond;
    TileRowCacheEntry *tile_cache; /* per-context tile cache (may be in fast SRAM) */
    int ctx_id;                    /* render context: indexes the per-context row plan */
    /* Emit-span tracking: emit_tile_run widens [emit_lo, emit_hi) with every
     * run that survives the blank-row skip. The temp-layer merge uses it to
     * scan only the columns the temp render could have written — on the
     * overworld BG3 (HUD/text) is usually entirely blank, collapsing the
     * 256-column merge scan to nothing. Callers that consume the span must
     * re-init it (lo = INT_MAX, hi = 0) before each render_bg_scanline call. */
    int emit_lo, emit_hi;
} BGRenderCtx;

/* Helpers to extract fields from a packed main word (or a 16-bit gp_lm —
 * GP/LM live in the same low bits either way). */
#define GP_LM_GP(v)    ((uint8_t)(v))
#define GP_LM_LM(v)    ((uint8_t)((v) >> 8))
#define MAIN_COLOR(v)  ((uint16_t)((v) >> 16))
#define MAIN_PACK(color, gp_lm) \
    (((uint32_t)(uint16_t)(color) << 16) | (uint16_t)(gp_lm))

/* Shadow palette: SNES BGR555 converted to BGR565 once per frame.
 * Eliminates per-pixel bgr555_to_pixel() calls during rendering.
 * BGR555→BGR565 is a single shift (same channel order, widen green). */
static uint16_t cgram_render[256];

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
/* Contexts after the first may use a smaller cache to save RAM (a port sets
 * EB_PPU_SECONDARY_TILE_CACHE_BITS); they then hit less often. */
#ifndef EB_PPU_SECONDARY_TILE_CACHE_BITS
#define EB_PPU_SECONDARY_TILE_CACHE_BITS TILE_CACHE_BITS
#endif
#define TILE_CACHE_SIZE_SECONDARY (1 << EB_PPU_SECONDARY_TILE_CACHE_BITS)
#if EB_PPU_NUM_RENDER_CONTEXTS == 1
#define CTX_TILE_CACHE_MASK(ctx) TILE_CACHE_MASK
#else
#define CTX_TILE_CACHE_MASK(ctx) \
    ((ctx) == 0 ? TILE_CACHE_MASK : TILE_CACHE_SIZE_SECONDARY - 1)
#endif
#define TILE_CACHE_EMPTY 0xFFFF
/* Every tile address is a multiple of 16: BG tiles sit at nba * 0x2000 plus
 * tile_num * 16/32/64, OBJ tiles at obsel-derived 0x2000 multiples plus
 * t * 32. So tile_addr >> 4 (12 bits, VRAM is 64 KB) and the row (3 bits)
 * pack into 15 bits, one-to-one, and never reach TILE_CACHE_EMPTY. */
#define TILE_CACHE_KEY(tile_addr, pixel_y) \
    ((uint16_t)(((tile_addr) >> 4) | ((uint32_t)(pixel_y) << 12)))
_Static_assert(sizeof(TileRowCacheEntry) == 12, "tile row cache entry must stay 12 bytes");

#if EB_PPU_NUM_RENDER_CONTEXTS == 1
static TileRowCacheEntry tile_row_cache[1][TILE_CACHE_SIZE];
#define CTX_TILE_CACHE(ctx) tile_row_cache[ctx]
#endif /* multi-context: in FrameRebuiltBufs, next to the line buffers */

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

/* spread_nyb[b]: byte -> u32 placing input bit (7-i) at bit 4*i (the low bit of
 * output nibble i). OR'ing spread[b0] | spread[b1]<<1 | spread[b2]<<2 |
 * spread[b3]<<3 then yields a u32 whose nibble i is the full 4bpp index for
 * pixel i (pixel 0 = MSB of each plane). File-scope const so it stays in flash
 * rodata when decode_4bpp_row is RAM-placed (cf. win_layer_bits). 1KB. */
static const uint32_t spread_nyb[256] = {
    0x00000000u, 0x10000000u, 0x01000000u, 0x11000000u, 0x00100000u, 0x10100000u, 0x01100000u, 0x11100000u,
    0x00010000u, 0x10010000u, 0x01010000u, 0x11010000u, 0x00110000u, 0x10110000u, 0x01110000u, 0x11110000u,
    0x00001000u, 0x10001000u, 0x01001000u, 0x11001000u, 0x00101000u, 0x10101000u, 0x01101000u, 0x11101000u,
    0x00011000u, 0x10011000u, 0x01011000u, 0x11011000u, 0x00111000u, 0x10111000u, 0x01111000u, 0x11111000u,
    0x00000100u, 0x10000100u, 0x01000100u, 0x11000100u, 0x00100100u, 0x10100100u, 0x01100100u, 0x11100100u,
    0x00010100u, 0x10010100u, 0x01010100u, 0x11010100u, 0x00110100u, 0x10110100u, 0x01110100u, 0x11110100u,
    0x00001100u, 0x10001100u, 0x01001100u, 0x11001100u, 0x00101100u, 0x10101100u, 0x01101100u, 0x11101100u,
    0x00011100u, 0x10011100u, 0x01011100u, 0x11011100u, 0x00111100u, 0x10111100u, 0x01111100u, 0x11111100u,
    0x00000010u, 0x10000010u, 0x01000010u, 0x11000010u, 0x00100010u, 0x10100010u, 0x01100010u, 0x11100010u,
    0x00010010u, 0x10010010u, 0x01010010u, 0x11010010u, 0x00110010u, 0x10110010u, 0x01110010u, 0x11110010u,
    0x00001010u, 0x10001010u, 0x01001010u, 0x11001010u, 0x00101010u, 0x10101010u, 0x01101010u, 0x11101010u,
    0x00011010u, 0x10011010u, 0x01011010u, 0x11011010u, 0x00111010u, 0x10111010u, 0x01111010u, 0x11111010u,
    0x00000110u, 0x10000110u, 0x01000110u, 0x11000110u, 0x00100110u, 0x10100110u, 0x01100110u, 0x11100110u,
    0x00010110u, 0x10010110u, 0x01010110u, 0x11010110u, 0x00110110u, 0x10110110u, 0x01110110u, 0x11110110u,
    0x00001110u, 0x10001110u, 0x01001110u, 0x11001110u, 0x00101110u, 0x10101110u, 0x01101110u, 0x11101110u,
    0x00011110u, 0x10011110u, 0x01011110u, 0x11011110u, 0x00111110u, 0x10111110u, 0x01111110u, 0x11111110u,
    0x00000001u, 0x10000001u, 0x01000001u, 0x11000001u, 0x00100001u, 0x10100001u, 0x01100001u, 0x11100001u,
    0x00010001u, 0x10010001u, 0x01010001u, 0x11010001u, 0x00110001u, 0x10110001u, 0x01110001u, 0x11110001u,
    0x00001001u, 0x10001001u, 0x01001001u, 0x11001001u, 0x00101001u, 0x10101001u, 0x01101001u, 0x11101001u,
    0x00011001u, 0x10011001u, 0x01011001u, 0x11011001u, 0x00111001u, 0x10111001u, 0x01111001u, 0x11111001u,
    0x00000101u, 0x10000101u, 0x01000101u, 0x11000101u, 0x00100101u, 0x10100101u, 0x01100101u, 0x11100101u,
    0x00010101u, 0x10010101u, 0x01010101u, 0x11010101u, 0x00110101u, 0x10110101u, 0x01110101u, 0x11110101u,
    0x00001101u, 0x10001101u, 0x01001101u, 0x11001101u, 0x00101101u, 0x10101101u, 0x01101101u, 0x11101101u,
    0x00011101u, 0x10011101u, 0x01011101u, 0x11011101u, 0x00111101u, 0x10111101u, 0x01111101u, 0x11111101u,
    0x00000011u, 0x10000011u, 0x01000011u, 0x11000011u, 0x00100011u, 0x10100011u, 0x01100011u, 0x11100011u,
    0x00010011u, 0x10010011u, 0x01010011u, 0x11010011u, 0x00110011u, 0x10110011u, 0x01110011u, 0x11110011u,
    0x00001011u, 0x10001011u, 0x01001011u, 0x11001011u, 0x00101011u, 0x10101011u, 0x01101011u, 0x11101011u,
    0x00011011u, 0x10011011u, 0x01011011u, 0x11011011u, 0x00111011u, 0x10111011u, 0x01111011u, 0x11111011u,
    0x00000111u, 0x10000111u, 0x01000111u, 0x11000111u, 0x00100111u, 0x10100111u, 0x01100111u, 0x11100111u,
    0x00010111u, 0x10010111u, 0x01010111u, 0x11010111u, 0x00110111u, 0x10110111u, 0x01110111u, 0x11110111u,
    0x00001111u, 0x10001111u, 0x01001111u, 0x11001111u, 0x00101111u, 0x10101111u, 0x01101111u, 0x11101111u,
    0x00011111u, 0x10011111u, 0x01011111u, 0x11011111u, 0x00111111u, 0x10111111u, 0x01111111u, 0x11111111u,
};

/* LUT-based bitplane spread: builds all 8 nibbles in parallel via spread_nyb,
 * then expands the packed nibbles to one byte per pixel. Bit-identical to the
 * scalar version (verified exhaustively per-plane + 5M random fuzz on host). */
static void PPU_HOT_FUNC(decode_4bpp_row)(const uint8_t *tile_data, int row,
                            uint8_t *out_indices) {
    const uint8_t *bp01 = tile_data + row * 2;
    const uint8_t *bp23 = tile_data + 16 + row * 2;
    uint32_t packed = spread_nyb[bp01[0]]
                    | (spread_nyb[bp01[1]] << 1)
                    | (spread_nyb[bp23[0]] << 2)
                    | (spread_nyb[bp23[1]] << 3);
    /* Expand the 8 packed nibbles (4-bit, stride 4) to one byte per pixel, in
     * two u32 halves (low = pixels 0-3, high = pixels 4-7). Kept as u32 (not a
     * u64) so the stores stay register->memory; memcpy of constant size 4 is
     * lowered to a single aligned str (out_indices is 4-byte aligned). */
    uint32_t lo = packed & 0xFFFFu;
    lo = (lo | (lo << 8)) & 0x00FF00FFu;
    lo = (lo | (lo << 4)) & 0x0F0F0F0Fu;
    uint32_t hi = packed >> 16;
    hi = (hi | (hi << 8)) & 0x00FF00FFu;
    hi = (hi | (hi << 4)) & 0x0F0F0F0Fu;
    /* Two word stores (no memcpy call). out_indices is always &cache[].indices[0]
     * which is 4-byte aligned (indices sits at offset 4 after the u32 key), so a
     * plain aligned store is safe; may_alias avoids strict-aliasing UB.
     * Little-endian: byte i of each half = pixel i. */
    typedef uint32_t __attribute__((may_alias)) u32_alias;
    ((u32_alias *)out_indices)[0] = lo;
    ((u32_alias *)out_indices)[1] = hi;
}

/* Mode 3 only (the gas station intro's BG1), and only on a tile-row cache
 * miss, so it stays out of EB_PPU_RAM_SECTION. noinline keeps GCC from
 * folding it into the RAM-resident caller, which would cost the same RAM. */
static __attribute__((noinline)) void decode_8bpp_row(const uint8_t *tile_data, int row,
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

/* Colour of byte k (pixel k) of a decoded 2bpp/4bpp row word w. Indexes pal
 * by byte offset, (byte k) * 2, which the M0+ extracts in two shifts. Exact
 * only while every index is < 128: (w << 16) >> 23 also picks up bit 7 of
 * byte k-1. 2bpp/4bpp indices are < 16, and only those rows are marked
 * opaque, so 8bpp rows never come here. */
static inline __attribute__((always_inline))
uint16_t row_rgb(const uint16_t *pal, uint32_t w, int k)
{
    uint32_t ofs = (k == 3) ? (w >> 24) << 1 : (w << (24 - 8 * k)) >> 23;
    return *(const uint16_t *)((const uint8_t *)pal + ofs);
}

/* Fully opaque 8-pixel row into a merge buffer that may already hold pixels
 * (no-window layer after the first): write each pixel whose gp beats the
 * stored one. w0/w1 are the row's pixels 0-3 and 4-7, already flipped.
 * Out of line on purpose: inlined into render_bg_scanline's tile loops it
 * has only r0-r3 free on the M0+ and reloads pal, gp and gp_lm from the
 * stack for every pixel; as a leaf it keeps all of them in registers. */
static __attribute__((noinline)) void PPU_HOT_FUNC(emit_opaque_row_prio)(
    uint32_t *dst, uint32_t w0, uint32_t w1, const uint16_t *pal, uint16_t gp_lm)
{
    const uint8_t gp = GP_LM_GP(gp_lm);
#define PX_PRIO(i, w, k) do { \
    if (gp > GP_LM_GP(dst[i])) dst[i] = MAIN_PACK(row_rgb(pal, w, k), gp_lm); \
} while (0)
    PX_PRIO(0, w0, 0); PX_PRIO(1, w0, 1); PX_PRIO(2, w0, 2); PX_PRIO(3, w0, 3);
    PX_PRIO(4, w1, 0); PX_PRIO(5, w1, 1); PX_PRIO(6, w1, 2); PX_PRIO(7, w1, 3);
#undef PX_PRIO
}

/* General emit: any run of a decoded row, with transparency, the sub screen
 * and window masks. These take the runs the opaque fast path in
 * emit_tile_run doesn't (partial and transparent rows, sub screen, windowed
 * layers). Out of line for the same reason as emit_opaque_row_prio: as
 * leaves the loops get the registers, and render_bg_scanline carries one
 * copy of the loops instead of one per emit_tile_run call site. Split into
 * a main-only and a main+sub function (rather than a per-pixel sub_gp NULL
 * check, or one function whose register allocation serves neither).
 *
 * src walks the run forward: an hflipped row is first byte-reversed into
 * the caller's 8-byte `flipped` buffer. */
static inline __attribute__((always_inline))
const uint8_t *run_src(const uint8_t *indices, int start_px, bool hflip,
                       uint32_t flipped[2])
{
    if (!hflip) return indices + start_px;
    typedef const uint32_t __attribute__((may_alias)) u32_alias;
    flipped[0] = __builtin_bswap32(((u32_alias *)indices)[1]);
    flipped[1] = __builtin_bswap32(((u32_alias *)indices)[0]);
    return (const uint8_t *)flipped + start_px;
}

#define EMIT_PIXELS_LOOP(BODY) do { \
    for (int _i = 0; _i < count; _i++) { \
        uint32_t _cidx = src[_i]; \
        if (_cidx == 0) continue; \
        uint16_t _rgb = pal[_cidx]; \
        BODY; \
    } \
} while (0)

/* Windowed variant: per-pixel window masks + priority compare. tm_line is
 * always valid (never NULL) — caller provides an all-0xFF array when windows
 * are inactive, eliminating the NULL check. */
#define EMIT_PIXELS(HAS_SUB) EMIT_PIXELS_LOOP( \
    if ((tm[_i] & layer_bit) && gp > GP_LM_GP(dst[_i])) \
        dst[_i] = MAIN_PACK(_rgb, gp_lm); \
    if (HAS_SUB && (ts[_i] & layer_bit) && gp > sub_gp[_i]) { \
        sub_gp[_i] = gp; \
        sub_color[_i] = _rgb; \
    })

/* No-window variant: windows globally inactive, so eff_tm_line/eff_ts_line
 * are uniform. The per-pixel `tm_line[_sx] & layer_bit` load+AND collapses
 * to the loop-invariant booleans main_on/sub_on — one memory load removed
 * from the critical path of every emitted pixel. */
#define EMIT_PIXELS_NW(HAS_SUB) EMIT_PIXELS_LOOP( \
    if (main_on && gp > GP_LM_GP(dst[_i])) \
        dst[_i] = MAIN_PACK(_rgb, gp_lm); \
    if (HAS_SUB && sub_on && gp > sub_gp[_i]) { \
        sub_gp[_i] = gp; \
        sub_color[_i] = _rgb; \
    })

/* Unconditional variant: the merge buffer is known-empty for this layer's
 * pixels (uncond), so the priority load+compare is dropped — the write
 * always wins. main is written unconditionally (selection guarantees
 * main_on); sub is gated by the loop-invariant sub_on. Each screen_x is
 * written at most once per layer, so no intra-layer overwrite hazard. */
#define EMIT_PIXELS_UNCOND(HAS_SUB) EMIT_PIXELS_LOOP( \
    dst[_i] = MAIN_PACK(_rgb, gp_lm); \
    if (HAS_SUB && sub_on) { \
        sub_gp[_i] = gp; \
        sub_color[_i] = _rgb; \
    })

/* Main screen only (ctx->sub_gp == NULL). */
static __attribute__((noinline)) void PPU_HOT_FUNC(emit_row_main)(
    const BGRenderCtx *ctx, int screen_x, const uint16_t *pal, uint16_t gp_lm,
    const uint8_t *indices, int start_px, int count, bool hflip)
{
    uint32_t flipped[2];
    const uint8_t *src = run_src(indices, start_px, hflip, flipped);
    const uint8_t gp = GP_LM_GP(gp_lm);
    uint32_t *const dst = ctx->main + screen_x;
    const bool main_on = ctx->main_on;
    /* HAS_SUB = 0 removes every sub-screen use; these keep the dead
     * branches well-formed. */
    const bool sub_on = false;
    uint8_t *const sub_gp = NULL;
    uint16_t *const sub_color = NULL;
    const uint8_t *const ts = NULL;
    if (ctx->uncond && main_on) {
        EMIT_PIXELS_UNCOND(0);
    } else if (ctx->no_window) {
        EMIT_PIXELS_NW(0);
    } else {
        const uint8_t *const tm = ctx->tm_line + screen_x;
        const uint8_t layer_bit = ctx->layer_bit;
        EMIT_PIXELS(0);
    }
}

/* Main and sub screen (ctx->sub_gp != NULL, colour math on). */
static __attribute__((noinline)) void PPU_HOT_FUNC(emit_row_sub)(
    const BGRenderCtx *ctx, int screen_x, const uint16_t *pal, uint16_t gp_lm,
    const uint8_t *indices, int start_px, int count, bool hflip)
{
    uint32_t flipped[2];
    const uint8_t *src = run_src(indices, start_px, hflip, flipped);
    const uint8_t gp = GP_LM_GP(gp_lm);
    uint32_t *const dst = ctx->main + screen_x;
    const bool main_on = ctx->main_on;
    const bool sub_on = ctx->sub_on;
    uint8_t *const sub_gp = ctx->sub_gp + screen_x;
    uint16_t *const sub_color = ctx->sub_color + screen_x;
    if (ctx->uncond && main_on) {
        EMIT_PIXELS_UNCOND(1);
    } else if (ctx->no_window) {
        EMIT_PIXELS_NW(1);
    } else {
        const uint8_t *const tm = ctx->tm_line + screen_x;
        const uint8_t *const ts = ctx->ts_line + screen_x;
        const uint8_t layer_bit = ctx->layer_bit;
        EMIT_PIXELS(1);
    }
}
#undef EMIT_PIXELS_LOOP
#undef EMIT_PIXELS
#undef EMIT_PIXELS_NW
#undef EMIT_PIXELS_UNCOND

/* Emit a run of pixels from a single decoded 8x8 tile row.
 * Writes directly to merged priority buffers via BGRenderCtx, applying
 * window masks and global priority comparison in one pass. */
static inline __attribute__((always_inline))
void emit_tile_run(
    uint16_t tile_num, int pixel_y, int start_px, int count,
    bool hflip, int screen_x, uint8_t prio_bit, uint8_t palette,
    int bpp, uint32_t tile_data_base, int bytes_per_tile,
    BGRenderCtx *ctx)
{
    uint32_t tile_addr = tile_data_base + (uint32_t)tile_num * bytes_per_tile;
    if (tile_addr + bytes_per_tile > VRAM_SIZE) return;
    BGC_INC(emit_calls);
    BGC_ADD(pixels, count);

    const uint8_t *tile_data = &ppu.vram[tile_addr];

    /* Quick blank-row check: if all bitplane bytes for this row are zero,
     * every pixel is transparent — skip decode + emit entirely. A pixel is
     * index 0 only where no plane has its bit set, so any == 0xFF also says
     * the row has no transparent pixel (the cache entry's opaque, kept 0 for
     * 8bpp rows: see row_rgb). */
    const uint8_t *bp = tile_data + pixel_y * 2;
    uint8_t any = bp[0] | bp[1];
    if (bpp >= 4) { any |= bp[16] | bp[17]; }
    if (bpp == 8) { any |= bp[32] | bp[33] | bp[48] | bp[49]; }
    if (!any) { BGC_INC(blank_skips); return; }

    /* Tile row cache lookup — avoids redundant decode across scanlines,
     * horizontal tile repeats, and cross-layer tile sharing. */
    uint16_t cache_key = TILE_CACHE_KEY(tile_addr, pixel_y);
    uint32_t cache_idx = ((tile_addr >> 1) ^ pixel_y) & CTX_TILE_CACHE_MASK(ctx->ctx_id);
    TileRowCacheEntry *row = &ctx->tile_cache[cache_idx];
    if (row->key == cache_key) {
        BGC_INC(cache_hits);
    } else {
        BGC_INC(cache_misses);
        if (bpp == 8)
            decode_8bpp_row(tile_data, pixel_y, row->indices);
        else if (bpp == 4)
            decode_4bpp_row(tile_data, pixel_y, row->indices);
        else
            decode_2bpp_row(tile_data, pixel_y, row->indices);
        row->key = cache_key;
        row->opaque = (any == 0xFF && bpp != 8);
    }
#ifdef EB_PPU_PROFILE
    if (row->opaque) BGC_INC(opaque_runs);
#endif

    /* Widen the emit span (blank rows were already skipped above, so the
     * span only grows for runs that can actually write pixels). */
    if (screen_x < ctx->emit_lo) ctx->emit_lo = screen_x;
    if (screen_x + count > ctx->emit_hi) ctx->emit_hi = screen_x + count;

    /* Per-run invariants, computed once instead of per pixel: the palette
     * row (8bpp indexes CGRAM directly, so it starts at 0), the packed gp|lm
     * word and the destination. */
    const uint16_t *pal = cgram_render
                        + ((bpp == 8) ? 0 : (bpp == 4) ? palette * 16 : palette * 4);
    const uint16_t gp_lm = prio_bit ? ctx->gp1_lm : ctx->gp0_lm;
    uint32_t *const dst = ctx->main + screen_x;

    if (ctx->sub_gp) BGC_INC(sub_emit_calls);

    /* Whole opaque row, main screen only: ~95% of runs on the overworld and in
     * battle. All 8 pixels are written (or priority-tested), so the loop is
     * unrolled with no transparency test, and the row is read as two words
     * (pixel 0 in the low byte; little-endian, as decode_4bpp_row assumes)
     * that hflip reverses with a byte swap. */
    if (count == 8 && row->opaque && !ctx->sub_gp && ctx->main_on) {
        typedef const uint32_t __attribute__((may_alias)) u32_alias;
        uint32_t w0 = ((u32_alias *)row->indices)[0];
        uint32_t w1 = ((u32_alias *)row->indices)[1];
        if (hflip) {
            uint32_t t = w0;
            w0 = __builtin_bswap32(w1);
            w1 = __builtin_bswap32(t);
        }
        if (ctx->uncond) {
            dst[0] = MAIN_PACK(row_rgb(pal, w0, 0), gp_lm);
            dst[1] = MAIN_PACK(row_rgb(pal, w0, 1), gp_lm);
            dst[2] = MAIN_PACK(row_rgb(pal, w0, 2), gp_lm);
            dst[3] = MAIN_PACK(row_rgb(pal, w0, 3), gp_lm);
            dst[4] = MAIN_PACK(row_rgb(pal, w1, 0), gp_lm);
            dst[5] = MAIN_PACK(row_rgb(pal, w1, 1), gp_lm);
            dst[6] = MAIN_PACK(row_rgb(pal, w1, 2), gp_lm);
            dst[7] = MAIN_PACK(row_rgb(pal, w1, 3), gp_lm);
            return;
        }
        if (ctx->no_window) {
            emit_opaque_row_prio(dst, w0, w1, pal, gp_lm);
            return;
        }
    }

    /* Everything else: partial rows, rows with transparent pixels, the sub
     * screen, windowed layers. */
    if (ctx->sub_gp)
        emit_row_sub(ctx, screen_x, pal, gp_lm, row->indices, start_px, count, hflip);
    else
        emit_row_main(ctx, screen_x, pal, gp_lm, row->indices, start_px, count, hflip);
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
 * the full loop. One plan per render context: a context renders its scanlines in
 * order, so the plan is only replayed within that context's own rows. */
#ifndef EB_PPU_BG_ROW_CACHE
#define EB_PPU_BG_ROW_CACHE 1
#endif
#if EB_PPU_BG_ROW_CACHE
#define BG_ROW_PLAN_MAXCOL 48   /* >= max tile columns across render_width (~42) */
typedef struct {
    uint16_t tile_num;
    uint8_t  x_in_tile, pixels, prio_bit, palette;
    bool     hflip, vflip;
    uint16_t screen_x;
} BGRowPlanEntry;
static BGRowPlanEntry bg_row_plan[EB_PPU_NUM_RENDER_CONTEXTS][4][BG_ROW_PLAN_MAXCOL];
static struct {
    int row;       /* cached tile_row_map; -1 = invalid */
    int scroll_x;  /* plan is valid only for this horizontal scroll ... */
    int width;     /* ... and this render_width */
    int count;     /* number of entries in bg_row_plan[ctx][bg] */
} bg_row_cache[EB_PPU_NUM_RENDER_CONTEXTS][4];
#endif

#if EB_PPU_BG_ROW_CACHE
/* A tile whose entire graphic (all bytes_per_tile bytes) is zero is palette
 * index 0 everywhere — fully transparent on every row (emit_tile_run would skip
 * every pixel via `if (_cidx == 0) continue`). Detected once while building the
 * row plan and elided from it, so it costs nothing on any of the tile row's
 * scanlines, not just the per-row blank-skip. On the overworld ~80% of BG tiles
 * are fully transparent, so this roughly doubles the row cache's gain. */
static inline bool tile_fully_blank(uint32_t tile_addr, int bytes_per_tile) {
    if (tile_addr + bytes_per_tile > VRAM_SIZE) return false;
    /* Word ORs: tile_addr is tile_data_base (a multiple of 0x2000) plus
     * tile_num * bytes_per_tile (16/32/64), and ppu.vram sits at offset 0 of
     * the 4-byte-aligned ppu, so every word read is aligned. may_alias avoids
     * strict-aliasing UB (same idiom as decode_4bpp_row's stores). */
    typedef uint32_t __attribute__((may_alias)) u32_alias;
    const u32_alias *p = (const u32_alias *)&ppu.vram[tile_addr];
    uint32_t any = 0;
    for (int i = 0; i < bytes_per_tile / 4; i++) any |= p[i];
    return any == 0;
}
#endif

/* Render a single BG layer scanline, merging directly into the priority
 * buffers via BGRenderCtx.  render_width controls how many screen pixels
 * to process (may be EB_VIEWPORT_WIDTH or SNES_WIDTH). */
static void PPU_HOT_FUNC(render_bg_scanline)(int bg_index, int scanline, int bpp,
                                         BGRenderCtx *ctx, int render_width,
                                         int x_pad) {
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

    /* Apply scroll offset.
     *
     * x_pad shifts a FILL-mode layer's content right by EB_VIEWPORT_PAD_LEFT so
     * its native 256px window lands at viewport x [PAD_LEFT, PAD_LEFT+256) — the
     * same horizontal span the CENTER path places content at (dst_start =
     * PAD_LEFT) and the span sprites occupy (drawn at SNES x + PAD_LEFT). Without
     * it a FILL background renders from screen x=0 and sits PAD_LEFT to the left
     * of the sprites that overlay it (battle enemies, the Sound Stone ring). The
     * gutters still fill via the tilemap's natural wrap. x_pad is 0 for the
     * centered/native paths and a no-op when the viewport equals SNES width. */
    int scroll_x = (ppu.bg_hofs[bg_index] - x_pad) & 0x3FF;
    int scroll_y = ppu.bg_vofs[bg_index] & 0x3FF;

    /* Battle-BG distortion HDMA (bg_distortion, battle_bg.h): a layer's
     * channel replaces this BG's HOFS or VOFS on every line. Layer 2's channel
     * runs after layer 1's, so it wins when both write the same register. A
     * wide viewport's gutter rows (line < 0 or >= 224) have no SNES table
     * line; bg_distortion_line() continues the ripple and the compression ramp
     * there, so the gutters churn with the content and meet it without a seam. */
    bool distorted = false;
    for (int k = 0; k < 2; k++) {
        const BgDistortion *d = &bg_distortion[k];
        if (d->layer != bg_index + 1)
            continue;
        uint16_t v = bg_distortion_line(d, scanline);
        if (d->vofs)
            scroll_y = v & 0x3FF;
        else
            scroll_x = (v - x_pad) & 0x3FF;
        distorted = true;
    }
    (void)distorted;   /* read only by the row cache */

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

#if EB_PPU_BG_ROW_CACHE
    /* Replay the cached column plan when this scanline shares its tile row with an
     * already-decoded scanline of this frame (the common case: 7 of every 8). */
    bool row_cacheable = !big_tiles
                         && !distorted
                         && render_width <= 8 * BG_ROW_PLAN_MAXCOL;
    if (row_cacheable
        && bg_row_cache[ctx->ctx_id][bg_index].row == tile_row_map
        && bg_row_cache[ctx->ctx_id][bg_index].scroll_x == scroll_x
        && bg_row_cache[ctx->ctx_id][bg_index].width == render_width) {
        const BGRowPlanEntry *pl = bg_row_plan[ctx->ctx_id][bg_index];
        int n = bg_row_cache[ctx->ctx_id][bg_index].count;
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
        /* Mode 0 gives each BG its own 32-color bank (BG1..4). Keep the
         * adjusted palette in the row plan so cached runs use the same bank. */
        if ((ppu.bgmode & BGMODE_MODE_MASK) == 0) palette += 8 * bg_index;
        uint8_t prio_bit = (entry >> 13) & 1;
        bool hflip = (entry >> 14) & 1;
        bool vflip = (entry >> 15) & 1;

        if (!big_tiles) {
            /* 8x8 tile: decode once, emit up to 8 pixels */
            int eff_py = vflip ? (7 - pixel_y_in_tile) : pixel_y_in_tile;
#if EB_PPU_BG_ROW_CACHE
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
                    BGRowPlanEntry *e = &bg_row_plan[ctx->ctx_id][bg_index][plan_n++];
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
#if EB_PPU_BG_ROW_CACHE
    /* Commit the plan for replay by this row's other scanlines. Only when it fit
     * (plan_n < MAXCOL); if it ever overflowed we leave the cache invalid and just
     * recompute — correct, only slower. */
    if (row_cacheable && plan_n < BG_ROW_PLAN_MAXCOL) {
        bg_row_cache[ctx->ctx_id][bg_index].row      = tile_row_map;
        bg_row_cache[ctx->ctx_id][bg_index].scroll_x = scroll_x;
        bg_row_cache[ctx->ctx_id][bg_index].width    = render_width;
        bg_row_cache[ctx->ctx_id][bg_index].count    = plan_n;
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

    /* SNES hardware: a halved add is (main + sub) >> 1 with no clamp
     * (20+30 → 25), a halved subtract clamps at 0 and then halves. Only an
     * unhalved add saturates at 31. The old clamp-then-halve capped every
     * halved sum at 15, so the 2-layer battle backgrounds (config 3 / 7,
     * CGADSUB $64 / $61) never got brighter than half intensity; emu-checked
     * on the Plague Rat BG (see the sub-screen backdrop note at the call). */
    if (subtract) {
        r -= sr; g -= sg; b -= sb_val;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;
    } else {
        r += sr; g += sg; b += sb_val;
    }

    if (half) { r >>= 1; g >>= 1; b >>= 1; }

    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;

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

/* The SNES PPU evaluates the sprites for a line during the line before it,
 * so an OBJ with OAM Y = y first appears on scanline y + 1. The game code
 * compensates (WRITE_SPRITEMAP_TO_OAM does `ADC base_y; DEC` before storing
 * the Y), so rendering OAM Y as-is puts every sprite 1 px too high against
 * the BG layers. */
#define OBJ_LINE_DELAY 1

/* ---- OBJ per-frame sprite bucketing (Candidate 2) -------------------------
 * render_obj_scanline previously rescanned all 128 OAM sprites on EVERY
 * scanline (128 x ~224 = ~28.7k sprite-tests/frame, the vast majority rejected
 * by the row<0||row>=h test).  Instead, bucket sprite geometry once per frame
 * by the 8-scanline band(s) each sprite covers; each scanline then iterates
 * only the sprites whose band it falls in.  Build is O(128) once per frame;
 * per-scanline cost drops from 128 to (#sprites overlapping that band).  Same
 * redundancy shape as bg_row_cache.  Priority (lower OAM index wins) is
 * preserved by appending in 127->0 order so the low index is emitted last.
 * Built once per frame by ppu_prepare_frame(), then read-only, so every render
 * context shares one set. */
#ifndef EB_PPU_OBJ_BUCKET
#define EB_PPU_OBJ_BUCKET 1
#endif

#if EB_PPU_OBJ_BUCKET
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

/* Band lists, packed back to back: band b's slot indices (priority order)
 * are obj_band_slots[obj_band_start[b] .. obj_band_start[b + 1]). A sprite
 * is at most 64 lines tall, so it lands in at most 9 bands, which bounds
 * the total at 128 * 9 entries (vs 128 per band for fixed-size lists). */
#define OBJ_MAX_BANDS_PER_SPRITE ((64 + (1 << OBJ_BAND_SHIFT) - 2) / (1 << OBJ_BAND_SHIFT) + 1)
static uint8_t   obj_band_slots[128 * OBJ_MAX_BANDS_PER_SPRITE];
static uint16_t  obj_band_start[OBJ_NUM_BANDS + 1];

/* Band range [*bstart, *bend] covered by an active sprite. */
static inline void obj_band_range(const ObjActive *a, int *bstart, int *bend) {
    int y0 = a->y;
    int y1 = a->y + a->h;                              /* exclusive */
    *bstart = y0 < 0 ? 0 : (y0 >> OBJ_BAND_SHIFT);
    *bend = (y1 - 1) >> OBJ_BAND_SHIFT;
    if (*bend >= OBJ_NUM_BANDS) *bend = OBJ_NUM_BANDS - 1;
}

/* Build the per-frame sprite buckets. Call once per frame before the scanline
 * loop (OAM/obsel are frame-constant in this renderer). */
static void PPU_HOT_FUNC(build_obj_buckets)(void) {
    /* Pass 1 counts each band's entries into obj_band_start[b]. */
    for (int b = 0; b <= OBJ_NUM_BANDS; b++) obj_band_start[b] = 0;

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
        int spr_y = ppu.oam_full_y[i] + ppu.sprite_y_offset + OBJ_LINE_DELAY;
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

        int bstart, bend;
        obj_band_range(&obj_active[slot], &bstart, &bend);
        for (int b = bstart; b <= bend; b++)
            obj_band_start[b]++;
    }

    /* Running sum: obj_band_start[b] = end of band b's list. */
    int total = 0;
    for (int b = 0; b < OBJ_NUM_BANDS; b++) {
        total += obj_band_start[b];
        obj_band_start[b] = (uint16_t)total;
    }
    obj_band_start[OBJ_NUM_BANDS] = (uint16_t)total;

    /* Pass 2 fills each list from its end, visiting slots n-1 -> 0, so a
     * band's list ends up in ascending slot order (the 127->0 OAM order
     * above) and obj_band_start[b] ends up at the start of band b. */
    for (int slot = n - 1; slot >= 0; slot--) {
        int bstart, bend;
        obj_band_range(&obj_active[slot], &bstart, &bend);
        for (int b = bstart; b <= bend; b++)
            obj_band_slots[--obj_band_start[b]] = (uint8_t)slot;
    }
}
#endif /* EB_PPU_OBJ_BUCKET */

/* Per-frame state every render context reads: the shadow palette from CGRAM
 * and the OBJ buckets. Call before signaling core 1 in dual-core mode to
 * avoid a data race on cgram_render[] and the bucket arrays. */
void ppu_prepare_frame(void) {
    for (int i = 0; i < 256; i++) {
        uint16_t c = ppu.cgram[i];
        cgram_render[i] = bgr555_to_pixel(c);
    }
#if EB_PPU_OBJ_BUCKET
    /* Bucket sprites by scanline band; render_obj_scanline then iterates only
     * the sprites overlapping each scanline (see build_obj_buckets). */
    build_obj_buckets();
#endif
}

/* Render OBJ (sprites) for one scanline into separate color/prio arrays.
 * Returns true if any sprite overlapped this scanline (i.e. obj_prio may hold
 * nonzero entries). When false, the compositor skips its per-pixel obj_prio
 * load entirely. */
#if EB_PPU_OBJ_BUCKET
/* Dirty x-span of obj_prio: all nonzero entries lie inside
 * [obj_prio_dirty_lo, obj_prio_dirty_hi). The per-scanline clear wipes only
 * this span instead of the whole LINE_BUF_WIDTH (sprites cover a narrow
 * x-range on a typical line). render_obj_scanline widens it per overlapping
 * sprite (conservative: full sprite width, transparent pixels included).
 * Initial full span makes the first clear wipe any pre-existing contents.
 * One span per render context, like obj_prio itself. */
static struct {
    int lo, hi;
} obj_prio_dirty[EB_PPU_NUM_RENDER_CONTEXTS] = {
    [0 ... EB_PPU_NUM_RENDER_CONTEXTS - 1] = { 0, LINE_BUF_WIDTH },
};
#endif

static bool PPU_HOT_FUNC(render_obj_scanline)(int ctx_id, int scanline, uint16_t *obj_color, uint8_t *obj_prio, TileRowCacheEntry *tile_cache) {
    (void)ctx_id; /* read only by the bucket's dirty-span tracking */
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

#if EB_PPU_OBJ_BUCKET
    int band = scanline >> OBJ_BAND_SHIFT;
    if (band < 0 || band >= OBJ_NUM_BANDS) return false;
    int bcnt = obj_band_start[band + 1] - obj_band_start[band];
    const uint8_t *blist = &obj_band_slots[obj_band_start[band]];

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
        int spr_y = ppu.oam_full_y[i] + ppu.sprite_y_offset + OBJ_LINE_DELAY;

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
#endif /* EB_PPU_OBJ_BUCKET */

        if (vflip) row = h - 1 - row;

        int tile_row = row >> 3;
        int pixel_y = row & 7;
        int tiles_wide = w >> 3;
        int pal_base_idx = 128 + spr_pal * 16;

#if EB_PPU_OBJ_BUCKET
        /* Widen the obj_prio dirty span for this sprite's x-range. */
        {
            int lo = spr_x < 0 ? 0 : spr_x;
            int hi = spr_x + w;
            if (hi > LINE_BUF_WIDTH) hi = LINE_BUF_WIDTH;
            if (lo < obj_prio_dirty[ctx_id].lo) obj_prio_dirty[ctx_id].lo = lo;
            if (hi > obj_prio_dirty[ctx_id].hi) obj_prio_dirty[ctx_id].hi = hi;
        }
#endif

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
            uint16_t obj_cache_key = TILE_CACHE_KEY(tile_addr, pixel_y);
            uint32_t obj_cache_idx = ((tile_addr >> 1) ^ pixel_y) & CTX_TILE_CACHE_MASK(ctx_id);
            const uint8_t *indices;
            {
                TileRowCacheEntry *cache = tile_cache;
                if (cache[obj_cache_idx].key == obj_cache_key) {
                    indices = cache[obj_cache_idx].indices;
                } else {
                    uint8_t *dest = cache[obj_cache_idx].indices;
                    decode_4bpp_row(&ppu.vram[tile_addr], pixel_y, dest);
                    cache[obj_cache_idx].key = obj_cache_key;
                    cache[obj_cache_idx].opaque = 0; /* not tracked for OBJ rows */
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

#ifdef EB_SMOOTH_CAMERA
/* Smooth camera party mask (see ppu_set_obj_mask in ppu.h). Desktop only: the
 * whole block compiles out unless the port defines EB_SMOOTH_CAMERA, and when
 * the mask is off the scanline loop pays one predictable branch per line. */
static const uint8_t *obj_mask_tags;  /* [128]: nonzero = tagged OAM entry */
static uint8_t *obj_mask_out;         /* [EB_VIEWPORT_HEIGHT][EB_VIEWPORT_WIDTH] */
static uint8_t obj_tag_line[LINE_BUF_WIDTH];

void ppu_set_obj_mask(const uint8_t *oam_tags, uint8_t *mask_out) {
    obj_mask_tags = oam_tags;
    obj_mask_out = mask_out;
}

/* Write this output row of the mask. Re-walks OAM in the same 127 -> 0 order
 * as render_obj_scanline (lowest index wins) to learn which entry owns each
 * opaque OBJ pixel, then applies the compositor's OBJ-vs-BG test to the merged
 * BG buffer (called before the compositor consumes best_bg). */
static __attribute__((noinline)) void obj_mask_scanline(
        int scanline, int out_y, int fb_x_offset, int x_start, int x_end,
        bool obj_enabled, bool no_layer_windows, const uint8_t *eff_tm_line,
        const uint8_t *obj_prio, const uint32_t *best_bg,
        const uint8_t *obj_thresh) {
    uint8_t *row = obj_mask_out + (size_t)out_y * EB_VIEWPORT_WIDTH;
    if (!obj_enabled)
        return; /* the row stays as cleared at frame start */
    if (ppu.sprite_y_offset) {
        int snes_sl = scanline - ppu.sprite_y_offset;
        if (snes_sl < 0 || snes_sl >= SNES_HEIGHT)
            return;
    }

    memset(obj_tag_line, 0, sizeof(obj_tag_line));
    uint32_t obj_base = (uint32_t)(ppu.obsel & 0x07) * 0x4000;
    uint32_t obj_gap = (uint32_t)((ppu.obsel >> 3) & 0x03) * 0x2000 + 0x2000;
    int size_sel = (ppu.obsel >> 5) & 0x07;
    for (int i = 127; i >= 0; i--) {
        const OAMEntry *spr = &ppu.oam[i];
        int size_bit = (ppu.oam_hi[i >> 2] >> ((i & 3) * 2 + 1)) & 1;
        int spr_x = ppu.oam_full_x[i] + ppu.sprite_x_offset;
        int spr_y = ppu.oam_full_y[i] + ppu.sprite_y_offset + OBJ_LINE_DELAY;
        int w = obj_sizes[size_sel][size_bit][0];
        int h = obj_sizes[size_sel][size_bit][1];
        int r = scanline - spr_y;
        if (r < 0 || r >= h || spr_x >= LINE_BUF_WIDTH || spr_x + w <= 0)
            continue;
        bool vflip = (spr->attr >> 7) & 1;
        bool hflip = (spr->attr >> 6) & 1;
        if (vflip) r = h - 1 - r;
        uint16_t tile_num = spr->tile | ((uint16_t)(spr->attr & 1) << 8);
        uint8_t tag = obj_mask_tags[i] ? 1 : 0;
        int tiles_wide = w >> 3;
        for (int tc = 0; tc < tiles_wide; tc++) {
            int eff_tc = hflip ? (tiles_wide - 1 - tc) : tc;
            uint16_t t = tile_num + eff_tc + (r >> 3) * 16;
            uint32_t tile_addr = t >= 256 ? obj_base + obj_gap + (uint32_t)(t - 256) * 32
                                          : obj_base + (uint32_t)t * 32;
            if (tile_addr + 32 > VRAM_SIZE) continue;
            int tile_screen_x = spr_x + tc * 8;
            if (tile_screen_x >= LINE_BUF_WIDTH || tile_screen_x + 8 <= 0)
                continue;
            uint8_t indices[8];
            decode_4bpp_row(&ppu.vram[tile_addr], r & 7, indices);
            for (int px = 0; px < 8; px++) {
                int sx = tile_screen_x + px;
                if (sx < 0 || sx >= LINE_BUF_WIDTH) continue;
                if (indices[hflip ? 7 - px : px])
                    obj_tag_line[sx] = tag;
            }
        }
    }

    for (int x = x_start; x < x_end; x++) {
        if (!obj_tag_line[x]) continue;
        if (!no_layer_windows && !(eff_tm_line[x] & LAYER_OBJ)) continue;
        uint8_t obj_pk = obj_prio[x];
        if (obj_pk > 0 && GP_LM_GP(best_bg[x]) < obj_thresh[obj_pk])
            row[x + fb_x_offset] = 1;
    }
}
#endif /* EB_SMOOTH_CAMERA */

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

#if EB_VIEWPORT_PAD_LEFT > 0 || EB_VIEWPORT_PAD_TOP > 0
/* Is native column wx inside per-scanline (HDMA) window `win` (1 or 2) on
 * native row wy? Only the wide-viewport gutter code below uses it. */
static inline bool hdma_window_contains(int win, int wx, int wy)
{
    int sl = wy + EB_VIEWPORT_PAD_TOP;
    uint8_t l = (win == 1) ? ppu.wh0_table[sl] : ppu.wh2_table[sl];
    uint8_t r = (win == 1) ? ppu.wh1_table[sl] : ppu.wh3_table[sl];
    return wx >= l && wx <= r;
}
#endif

/* ---- Wide-viewport gutter continuation of per-scanline (HDMA) windows ----
 *
 * Window registers only cover the native 256x224 band, so a wide-mode gutter
 * pixel (wx<0, wx>255, or a pad row) has no native coordinate. Static windows
 * are rectangles: clamping wx to the band extends them exactly. Per-scanline
 * windows such as the battle swirl and the oval window are curved shapes and
 * need their shape continued past the band edge.
 *
 * gutter_row_build() does that per viewport scanline, per band edge: each run
 * of window pixels along the edge column/row is paired with the run it joins
 * a few pixels inside the band, and both run ends are extrapolated outward
 * along that slope. An arm crossing the edge continues at its own angle; an
 * arc that only grazes the edge (its boundary nearly parallel to the edge,
 * e.g. the type-1 spiral's outer ring) closes within a few pixels. A run end
 * clipped by the band corner has no known slope and stays open, which covers
 * the corner gutters. Bits: 1 = window 1, 2 = window 2.
 *
 * The row's membership lives in a small GutterRow on the caller's stack and
 * is rebuilt for each scanline that needs it, so the feature costs no static
 * RAM (a whole-frame mask was 4.5 KB on the RP2040 build, 19 KB at 320x240). */
#if EB_VIEWPORT_PAD_LEFT > 0 || EB_VIEWPORT_PAD_TOP > 0
#define GUTTER_PAD_R (EB_VIEWPORT_WIDTH - SNES_WIDTH - EB_VIEWPORT_PAD_LEFT)
#define GUTTER_PAD_B (EB_VIEWPORT_HEIGHT - SNES_HEIGHT - EB_VIEWPORT_PAD_TOP)
#define GUTTER_MAX_X (GUTTER_PAD_R > EB_VIEWPORT_PAD_LEFT ? GUTTER_PAD_R : \
                      (EB_VIEWPORT_PAD_LEFT > 0 ? EB_VIEWPORT_PAD_LEFT : 1))
#define GUTTER_HAS_PADS 1
typedef struct {
    /* [edge: 0 left, 1 right][depth - 1] for this scanline */
    uint8_t lr[2][GUTTER_MAX_X];
    /* top/bottom pad row: 2 bits per native x, 4 x per byte */
    uint8_t tb[SNES_WIDTH / 4];
} GutterRow;
#else
#define GUTTER_HAS_PADS 0
#endif

/* Pixels inside the band over which a run end's slope is measured. */
#define GUTTER_SLOPE_PROBE 8

#if GUTTER_HAS_PADS
/* Is band pixel (depth k inward from `edge`, position t along it) inside
 * per-scanline window `win`? Edges: 0 left, 1 right, 2 top, 3 bottom. */
static bool gutter_edge_inside(int win, int edge, int k, int t)
{
    switch (edge) {
    case 0:  return hdma_window_contains(win, k, t);
    case 1:  return hdma_window_contains(win, SNES_WIDTH - 1 - k, t);
    case 2:  return hdma_window_contains(win, t, k);
    default: return hdma_window_contains(win, t, SNES_HEIGHT - 1 - k);
    }
}

/* floor(n / d) for d > 0. */
static int gutter_floor_div(int n, int d)
{
    return n >= 0 ? n / d : -((-n + d - 1) / d);
}

/* Continue window `win` past band edge `edge` into one gutter row of `gr`.
 * Left/right edges: `pos` is the native row t (may lie in a pad row) and
 * every depth of that row is filled. Top/bottom edges: `pos` is the depth d
 * (1 = adjacent to the band) and every native x of that row is filled. */
static void gutter_edge_row(GutterRow *gr, int win, int edge, int pos)
{
    const int len = edge < 2 ? SNES_HEIGHT : SNES_WIDTH;
    const int depth_max = edge == 0 ? EB_VIEWPORT_PAD_LEFT
                        : edge == 1 ? GUTTER_PAD_R
                        : edge == 2 ? EB_VIEWPORT_PAD_TOP : GUTTER_PAD_B;
    /* Range of t the gutter covers: left/right gutters include the corner
     * rows above and below the band. */
    const int t_lo = edge < 2 ? -EB_VIEWPORT_PAD_TOP : 0;
    const int t_hi = edge < 2 ? SNES_HEIGHT - 1 + GUTTER_PAD_B : SNES_WIDTH - 1;
    const uint8_t bit = (uint8_t)win;
    if (depth_max <= 0)
        return;

    for (int a0 = 0; a0 < len; ) {
        if (!gutter_edge_inside(win, edge, 0, a0)) { a0++; continue; }
        int b0 = a0;
        while (b0 + 1 < len && gutter_edge_inside(win, edge, 0, b0 + 1)) b0++;

        /* Span of the runs at depth k that overlap [a0, b0]: the shape
         * this edge run belongs to, k pixels further in. The outermost
         * overlapping runs hold the first/last inside pixel of [a0, b0]. */
        int k, ak = 0, bk = -1;
        for (k = GUTTER_SLOPE_PROBE; k >= 1; k--) {
            int p = a0;
            while (p <= b0 && !gutter_edge_inside(win, edge, k, p)) p++;
            if (p > b0)
                continue;
            int q = b0;
            while (!gutter_edge_inside(win, edge, k, q)) q--;
            ak = p;
            while (ak > 0 && gutter_edge_inside(win, edge, k, ak - 1)) ak--;
            bk = q;
            while (bk + 1 < len && gutter_edge_inside(win, edge, k, bk + 1)) bk++;
            break;
        }

        if (bk >= 0) {
            int d_first = edge < 2 ? 1 : pos;
            int d_last = edge < 2 ? depth_max : pos;
            for (int d = d_first; d <= d_last; d++) {
                int lo = a0 == 0 ? t_lo
                         : a0 + gutter_floor_div((a0 - ak) * d + k / 2, k);
                int hi = b0 == len - 1 ? t_hi
                         : b0 + gutter_floor_div((b0 - bk) * d + k / 2, k);
                if (lo < t_lo) lo = t_lo;
                if (hi > t_hi) hi = t_hi;
                if (edge < 2) {
                    if (pos >= lo && pos <= hi)
                        gr->lr[edge][d - 1] |= bit;
                } else {
                    for (int t = lo; t <= hi; t++)
                        gr->tb[t >> 2] |= (uint8_t)(bit << ((t & 3) * 2));
                }
            }
        }
        a0 = b0 + 1;
    }
}

/* Build the gutter continuation of the active per-scanline windows for
 * viewport scanline `scanline` (see above). */
static void gutter_row_build(GutterRow *gr, int scanline)
{
    memset(gr, 0, sizeof(*gr));
    int wy = scanline - EB_VIEWPORT_PAD_TOP;
    for (int win = 1; win <= 2; win++) {
        if (!(win == 1 ? ppu.window_hdma_active : ppu.window2_hdma_active))
            continue;
        gutter_edge_row(gr, win, 0, wy);
        gutter_edge_row(gr, win, 1, wy);
        if (wy < 0)
            gutter_edge_row(gr, win, 2, -wy);
        else if (wy > SNES_HEIGHT - 1)
            gutter_edge_row(gr, win, 3, wy - (SNES_HEIGHT - 1));
    }
}
#endif

/* ---- Wide-viewport gutters of a centered (non-filling) BG layer ----
 *
 * A centered layer is drawn only into its native 256x224 band. Two modes
 * continue it into the gutters, both here, out of the hot renderer (this
 * only runs while such a layer is shown):
 *
 * BG_VIEWPORT_CLAMP extends the band's edge pixels sideways (the merged
 * word).
 *
 * BG_VIEWPORT_CONTINUE continues a full-screen overlay (the photographer's
 * camera shutter, BG3 animation sequence 1) the way gutter_edge_row()
 * continues the swirl windows. Along each band edge, the colour covering
 * most of the edge fills the gutter (the shutter's black). Every other run
 * of one colour along the edge (a white blade edge, the iris hole) is paired
 * with the same-coloured run it joins up to 8 px inside, and both run ends
 * are extrapolated outward along that slope, so a line crossing the edge
 * carries on at its own angle. A run with no partner inside (an arc that
 * only grazes the edge) stops at the edge; a run end clipped by the band
 * corner stays open, which covers the corner gutters.
 *
 * `bs` is the layer's BG scanline (BG line = bs + VOFS) and `band_lo` the bs
 * of the band's first row: the band is bs in [band_lo, band_lo + 224), viewport
 * x in [PAD_LEFT, PAD_LEFT + 256). Band pixels are never touched. Everything
 * is rebuilt per scanline on the stack, so the feature costs no static RAM. */
#if EB_VIEWPORT_PAD_LEFT > 0 || EB_VIEWPORT_PAD_TOP > 0
typedef struct {
    uint32_t tilemap_base, tile_data_base;
    int sc_size_h, sc_size_v, bytes_per_tile, bpp, bg_index;
    int scroll_x, scroll_y, w_mask, h_mask, band_lo;
} GutterBgSampler;

/* Tilemap entry and pixel row/column of band pixel (px, py). */
static uint16_t gbs_entry(const GutterBgSampler *s, int px, int py,
                          int *wx, int *wy)
{
    *wx = (px + s->scroll_x) & s->w_mask;
    *wy = (py + s->band_lo + s->scroll_y) & s->h_mask;
    int col = *wx >> 3, row = *wy >> 3;
    uint32_t addr = s->tilemap_base;
    if (s->sc_size_v == 64 && row >= 32) {
        addr += 32 * 32 * 2 * (s->sc_size_h == 64 ? 2 : 1);
        row -= 32;
    }
    if (s->sc_size_h == 64 && col >= 32) {
        addr += 32 * 32 * 2;
        col -= 32;
    }
    addr += (uint32_t)(row * 32 + col) * 2;
    if (addr + 1 >= VRAM_SIZE) return 0;
    return read_u16_le(&ppu.vram[addr]);
}

/* Colour key of band pixel (px, py): 0 = transparent, else
 * 1 + (colour index | palette << 3 | priority << 6). */
static uint16_t gbs_key(const GutterBgSampler *s, int px, int py)
{
    int wx, wy;
    uint16_t entry = gbs_entry(s, px, py, &wx, &wy);
    uint32_t tile = s->tile_data_base +
                    (uint32_t)(entry & 0x3FF) * s->bytes_per_tile;
    if (tile + s->bytes_per_tile > VRAM_SIZE) return 0;
    int ty = (entry & 0x8000) ? 7 - (wy & 7) : (wy & 7);
    int tx = (entry & 0x4000) ? (wx & 7) : 7 - (wx & 7);  /* bit in plane */
    const uint8_t *row = &ppu.vram[tile + ty * 2];
    int c = 0;
    for (int p = 0; p < s->bpp; p++) {
        const uint8_t *pl = row + (p >> 1) * 16 + (p & 1);
        c |= ((*pl >> tx) & 1) << p;
    }
    if (c == 0) return 0;
    return (uint16_t)(1 + (c | ((entry >> 10) & 7) << 8 |
                           ((entry >> 13) & 1) << 11));
}

/* Draw band pixel (px, py) of the layer at viewport column x. */
static void gbs_emit(const GutterBgSampler *s, int px, int py, int x,
                     BGRenderCtx *ctx)
{
    int wx, wy;
    uint16_t entry = gbs_entry(s, px, py, &wx, &wy);
    uint8_t palette = (entry >> 10) & 0x07;
    if ((ppu.bgmode & BGMODE_MODE_MASK) == 0) palette += 8 * s->bg_index;
    int ty = wy & 7;
    emit_tile_run(entry & 0x3FF, (entry & 0x8000) ? 7 - ty : ty, wx & 7, 1,
                  (entry >> 14) & 1, x, (entry >> 13) & 1, palette,
                  s->bpp, s->tile_data_base, s->bytes_per_tile, ctx);
}

/* Band pixel at depth k inward from `edge` (0 left, 1 right, 2 top,
 * 3 bottom), position t along it. */
static void gutter_edge_pixel(int edge, int k, int t, int *px, int *py)
{
    switch (edge) {
    case 0:  *px = k;                  *py = t; break;
    case 1:  *px = SNES_WIDTH - 1 - k; *py = t; break;
    case 2:  *px = t; *py = k; break;
    default: *px = t; *py = SNES_HEIGHT - 1 - k; break;
    }
}

#define GUTTER_MAX_RUNS 32
typedef struct {
    int16_t a0, b0;   /* run on the edge */
    int16_t da, db;   /* its ends' offsets k px inside */
    uint8_t k, open_a, open_b;
} GutterRun;

typedef struct {
    int n;
    int base_t;       /* edge position of the dominant colour, -1 = none */
    GutterRun run[GUTTER_MAX_RUNS];
} GutterEdge;

static void gutter_edge_scan(const GutterBgSampler *s, int edge, GutterEdge *ge)
{
    const int len = edge < 2 ? SNES_HEIGHT : SNES_WIDTH;
    uint16_t base = 0;
    int base_len = 0;
    ge->n = 0;
    ge->base_t = -1;

    /* The dominant colour: the one with the most edge pixels. */
    {
        uint16_t keys[4] = {0};
        int cnt[4] = {0}, first[4] = {0}, nk = 0;
        for (int t = 0; t < len; t++) {
            int px, py;
            gutter_edge_pixel(edge, 0, t, &px, &py);
            uint16_t key = gbs_key(s, px, py);
            int i = 0;
            while (i < nk && keys[i] != key) i++;
            if (i == nk) {
                if (nk == 4) continue;
                keys[nk] = key; first[nk] = t; nk++;
            }
            cnt[i]++;
        }
        for (int i = 0; i < nk; i++) {
            if (cnt[i] > base_len) {
                base_len = cnt[i];
                base = keys[i];
                ge->base_t = keys[i] ? first[i] : -1;
            }
        }
    }

    for (int a0 = 0; a0 < len; ) {
        int px, py;
        gutter_edge_pixel(edge, 0, a0, &px, &py);
        uint16_t key = gbs_key(s, px, py);
        int b0 = a0;
        for (;;) {
            if (b0 + 1 >= len) break;
            gutter_edge_pixel(edge, 0, b0 + 1, &px, &py);
            if (gbs_key(s, px, py) != key) break;
            b0++;
        }
        if (key != base && ge->n < GUTTER_MAX_RUNS) {
            /* The same-coloured run k px inside that touches [a0, b0]
             * (diagonal neighbours count, for 1-px lines). */
            for (int k = GUTTER_SLOPE_PROBE; k >= 1; k--) {
                int lo = a0 > 0 ? a0 - 1 : 0;
                int hi = b0 < len - 1 ? b0 + 1 : len - 1;
                int p = lo;
                for (; p <= hi; p++) {
                    gutter_edge_pixel(edge, k, p, &px, &py);
                    if (gbs_key(s, px, py) == key) break;
                }
                if (p > hi)
                    continue;
                int q = hi;
                for (;; q--) {
                    gutter_edge_pixel(edge, k, q, &px, &py);
                    if (gbs_key(s, px, py) == key) break;
                }
                int ak = p, bk = q;
                for (; ak > 0; ak--) {
                    gutter_edge_pixel(edge, k, ak - 1, &px, &py);
                    if (gbs_key(s, px, py) != key) break;
                }
                for (; bk < len - 1; bk++) {
                    gutter_edge_pixel(edge, k, bk + 1, &px, &py);
                    if (gbs_key(s, px, py) != key) break;
                }
                GutterRun *r = &ge->run[ge->n++];
                r->a0 = (int16_t)a0; r->b0 = (int16_t)b0;
                r->da = (int16_t)(a0 - ak); r->db = (int16_t)(b0 - bk);
                r->k = (uint8_t)k;
                r->open_a = a0 == 0;
                r->open_b = b0 == len - 1;
                break;
            }
        }
        a0 = b0 + 1;
    }
}

/* Edge position whose pixel shows at depth d (>= 1) outside the edge,
 * position t, or -1 for transparent. Later runs win. */
static int gutter_edge_source(const GutterEdge *ge, int d, int t)
{
    for (int i = ge->n - 1; i >= 0; i--) {
        const GutterRun *r = &ge->run[i];
        if (!r->open_a &&
            t < r->a0 + gutter_floor_div(r->da * d + r->k / 2, r->k))
            continue;
        if (!r->open_b &&
            t > r->b0 + gutter_floor_div(r->db * d + r->k / 2, r->k))
            continue;
        return r->a0;
    }
    return ge->base_t;
}

/* Returns true when `scanline` is a pad row of the layer (no band pixels).
 * Few arguments on purpose: the call sites sit in the hot renderer, which
 * the RP2040 build keeps in RAM. */
static bool __attribute__((noinline))
bg_gutter_fill(int bg_index, int scanline, const BGRenderCtx *ctx_in,
               uint32_t *best_bg)
{
    static const uint8_t mode_bpp[8][4] = {
        {2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0},
        {8, 2, 0, 0}, {4, 2, 0, 0}, {0, 0, 0, 0}, {8, 0, 0, 0},
    };
    int bpp = mode_bpp[ppu.bgmode & BGMODE_MODE_MASK][bg_index];
    int band_lo = ppu.sprite_y_offset ? 0 : 1;
    int bs = scanline - ppu.sprite_y_offset;
    bool pad_row = bs < band_lo || bs >= band_lo + SNES_HEIGHT;

    if (ppu.bg_viewport_fill[bg_index] == BG_VIEWPORT_CLAMP) {
        /* The band span the merge wrote (see the temp-layer merge). */
        const int dst_start = EB_VIEWPORT_PAD_LEFT > 0 ? EB_VIEWPORT_PAD_LEFT : 0;
        int count = SNES_WIDTH - (EB_VIEWPORT_PAD_LEFT < 0 ? -EB_VIEWPORT_PAD_LEFT : 0);
        if (dst_start + count > EB_VIEWPORT_WIDTH)
            count = EB_VIEWPORT_WIDTH - dst_start;
        if (count <= 0) return pad_row;
        uint32_t left_w = best_bg[dst_start];
        uint32_t right_w = best_bg[dst_start + count - 1];
        for (int x = 0; x < dst_start; x++) {
            if (GP_LM_GP(left_w) > GP_LM_GP(best_bg[x]))
                best_bg[x] = left_w;
        }
        for (int x = dst_start + count; x < EB_VIEWPORT_WIDTH; x++) {
            if (GP_LM_GP(right_w) > GP_LM_GP(best_bg[x]))
                best_bg[x] = right_w;
        }
        return pad_row;
    }

    /* BG_VIEWPORT_CONTINUE. 8x8 tiles only (the shutter is BG3, mode 1). */
    if (bpp == 0 || ((ppu.bgmode >> (4 + bg_index)) & 1)) return pad_row;

    GutterBgSampler s;
    uint16_t sc_reg = ppu.bg_sc[bg_index];
    uint8_t nba = bg_index < 2 ? ppu.bg_nba[0] >> (bg_index * 4)
                               : ppu.bg_nba[1] >> ((bg_index - 2) * 4);
    s.tilemap_base = (uint32_t)(sc_reg & 0xFC) << 9;
    s.tile_data_base = (uint32_t)(nba & 0x0F) * 0x2000;
    s.sc_size_h = (sc_reg & 0x01) ? 64 : 32;
    s.sc_size_v = (sc_reg & 0x02) ? 64 : 32;
    s.bpp = bpp;
    s.bytes_per_tile = (bpp == 8) ? 64 : (bpp == 4) ? 32 : 16;
    s.bg_index = bg_index;
    s.scroll_x = ppu.bg_hofs[bg_index] & 0x3FF;
    s.scroll_y = ppu.bg_vofs[bg_index] & 0x3FF;
    s.w_mask = s.sc_size_h * 8 - 1;
    s.h_mask = s.sc_size_v * 8 - 1;
    s.band_lo = band_lo;

    /* Priority-compared emit (gutter pixels may hold a lower layer). */
    BGRenderCtx ctx = *ctx_in;
    ctx.uncond = 0;

    GutterEdge ge;
    int t = bs - band_lo;   /* native row; < 0 or >= 224 in a pad row */

    /* Top/bottom pad row: the band's columns. */
    if (t < 0 || t > SNES_HEIGHT - 1) {
        int edge = t < 0 ? 2 : 3;
        int d = t < 0 ? -t : t - (SNES_HEIGHT - 1);
        gutter_edge_scan(&s, edge, &ge);
        for (int u = 0; u < SNES_WIDTH; u++) {
            int x = u + EB_VIEWPORT_PAD_LEFT;
            if (x < 0 || x >= EB_VIEWPORT_WIDTH) continue;
            int src = gutter_edge_source(&ge, d, u);
            if (src < 0) continue;
            int px, py;
            gutter_edge_pixel(edge, 0, src, &px, &py);
            gbs_emit(&s, px, py, x, &ctx);
        }
    }

    /* Left/right gutters, corner rows included. */
    for (int edge = 0; edge < 2 && EB_VIEWPORT_PAD_LEFT > 0; edge++) {
        gutter_edge_scan(&s, edge, &ge);
        for (int d = 1; d <= EB_VIEWPORT_PAD_LEFT; d++) {
            int x = edge == 0 ? EB_VIEWPORT_PAD_LEFT - d
                              : EB_VIEWPORT_PAD_LEFT + SNES_WIDTH - 1 + d;
            if (x >= EB_VIEWPORT_WIDTH) continue;
            int src = gutter_edge_source(&ge, d, t);
            if (src < 0) continue;
            int px, py;
            gutter_edge_pixel(edge, 0, src, &px, &py);
            gbs_emit(&s, px, py, x, &ctx);
        }
    }
    return pad_row;
}
#endif

/* Scratch for one scanline's gutter row (lives on precompute_window_masks's
 * stack). gutter_row_get() builds it on first use and returns NULL when no
 * per-scanline window needs continuing. */
typedef struct {
#if GUTTER_HAS_PADS
    GutterRow row;
#endif
    bool tried;
    const void *gr;
} GutterRowSlot;

static const void *gutter_row_get(GutterRowSlot *slot, bool wide_mode, int scanline)
{
    if (!slot->tried) {
        slot->tried = true;
        slot->gr = NULL;
#if GUTTER_HAS_PADS
        if (wide_mode && (ppu.window_hdma_active || ppu.window2_hdma_active)) {
            gutter_row_build(&slot->row, scanline);
            slot->gr = &slot->row;
        }
#else
        (void)wide_mode; (void)scanline;
#endif
    }
    return slot->gr;
}

/* Window 1/2 membership at native window coordinate wx on viewport scanline
 * `scanline`, for the per-pixel dual-window paths. Gutter pixels of a
 * per-scanline window read the continuation gutter_row_build() put in `gr`
 * (NULL when no per-scanline window is active); static windows clamp wx to
 * the band. */
static inline void window_membership(const void *gr_opaque, int scanline, int wx,
                                     uint8_t w1_left, uint8_t w1_right,
                                     uint8_t w2_left, uint8_t w2_right,
                                     bool *in_w1, bool *in_w2)
{
    int cwx = wx < 0 ? 0 : (wx > 255 ? 255 : wx);
    *in_w1 = (cwx >= w1_left && cwx <= w1_right);
    *in_w2 = (cwx >= w2_left && cwx <= w2_right);
#if GUTTER_HAS_PADS
    const GutterRow *gr = (const GutterRow *)gr_opaque;
    if (!gr)
        return;
    int wy = scanline - EB_VIEWPORT_PAD_TOP;
    uint8_t bits;
    if (wx < 0 || wx > SNES_WIDTH - 1) {
        int edge = wx < 0 ? 0 : 1;
        int d = wx < 0 ? -wx : wx - (SNES_WIDTH - 1);
        if (d > GUTTER_MAX_X || (unsigned)scanline >= EB_VIEWPORT_HEIGHT) return;
        bits = gr->lr[edge][d - 1];
    } else if (wy < 0 || wy > SNES_HEIGHT - 1) {
        bits = (gr->tb[wx >> 2] >> ((wx & 3) * 2)) & 3;
    } else {
        return;
    }
    if (ppu.window_hdma_active)
        *in_w1 = (bits & 1) != 0;
    if (ppu.window2_hdma_active)
        *in_w2 = (bits & 2) != 0;
#else
    (void)gr_opaque; (void)scanline;
#endif
}

/* Precompute per-scanline window masks and color math prevention.
 * Kept as a separate noinline function to reduce ppu_render_frame code size.
 * Called only on window scanlines whose window values changed (see the
 * win_cache_key check in the caller), never on window-free frames.
 * Deliberately NOT in EB_PPU_RAM_SECTION: ppu_render_frame_ex and the other
 * hot functions now run from RAM, so this no longer competes with them for
 * the RP2040 XIP cache, and its 5 KB of SRAM is worth more than the
 * occasional XIP refill in window scenes. */
static __attribute__((noinline))
void precompute_window_masks(
    int scanline, int render_width, bool wide_mode,
    uint8_t base_tm, uint8_t base_ts,
    uint8_t *eff_tm_line, uint8_t *eff_ts_line,
    uint8_t *cm_prevented_line,
    bool color_math_active, uint8_t prevent_mode,
    const WindowClassification *wc)
{
    GutterRowSlot gslot;
    gslot.tried = false;
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
                /* Gutter pixels take the state of the window shape continued
                 * past the native edge (see window_membership). */
                bool in_w1, in_w2;
                window_membership(gutter_row_get(&gslot, wide_mode, scanline), scanline, wx, w1_left, w1_right,
                                  w2_left, w2_right, &in_w1, &in_w2);
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
                     * cm_w1_en && cm_w2_en), so it lands here. Gutter pixels
                     * take the state of the swirl continued past the native
                     * edge (see window_membership). */
                    bool in_w1, in_w2;
                    window_membership(gutter_row_get(&gslot, wide_mode, scanline), scanline, wx, w1_left, w1_right,
                                      w2_left, w2_right, &in_w1, &in_w2);
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
 * When EB_PPU_NUM_RENDER_CONTEXTS == 1 (default), this is a single set of arrays
 * with no runtime overhead vs. the old stack allocation. */
#ifdef EB_PPU_EXTERNAL_LINE_OUT
/* The port owns the output lines: each context renders its next scanline into
 * the buffer last given to ppu_set_line_out(), which the send callback can
 * change for the following line (e.g. straight into a DMA buffer). */
static pixel_t *line_out_target[EB_PPU_NUM_RENDER_CONTEXTS];
#define CTX_LINE_OUT(ctx) line_out_target[ctx]

void PPU_HOT_FUNC(ppu_set_line_out)(int ctx_id, pixel_t *buf) {
    line_out_target[ctx_id] = buf;
}
#else
static pixel_t  line_out_ctx[EB_PPU_NUM_RENDER_CONTEXTS][EB_VIEWPORT_WIDTH] EB_PPU_LINEBUF_ATTR;
#define CTX_LINE_OUT(ctx) line_out_ctx[ctx]
#endif
/* Interleaved main merge buffer: color(hi16) | lmask | gp — see BGRenderCtx.
 * Same total RAM as the two uint16 arrays it replaced. */
#if EB_PPU_NUM_RENDER_CONTEXTS == 1
static uint32_t best_bg_ctx[1][LINE_BUF_WIDTH] EB_PPU_LINEBUF_ATTR;
#define CTX_BEST_BG(ctx) best_bg_ctx[ctx]
#else
/* Context 0's two buffers whose contents ppu_render_frame_ex rebuilds at the
 * start of every frame (tile cache reset to empty, best_bg cleared), kept
 * together so they can be lent out between frames (ppu_lend_frame_scratch).
 * The other contexts keep theirs apart, with a possibly smaller cache. */
typedef struct {
    TileRowCacheEntry tile_cache[TILE_CACHE_SIZE];
    uint32_t best_bg[LINE_BUF_WIDTH];
} FrameRebuiltBufs;
static FrameRebuiltBufs frame_rebuilt0 EB_PPU_LINEBUF_ATTR;
static TileRowCacheEntry tile_cache_secondary[EB_PPU_NUM_RENDER_CONTEXTS - 1][TILE_CACHE_SIZE_SECONDARY]
    EB_PPU_LINEBUF_ATTR;
static uint32_t best_bg_secondary[EB_PPU_NUM_RENDER_CONTEXTS - 1][LINE_BUF_WIDTH] EB_PPU_LINEBUF_ATTR;
#define CTX_TILE_CACHE(ctx) \
    ((ctx) == 0 ? frame_rebuilt0.tile_cache : tile_cache_secondary[(ctx) - 1])
#define CTX_BEST_BG(ctx) \
    ((ctx) == 0 ? frame_rebuilt0.best_bg : best_bg_secondary[(ctx) - 1])

void *ppu_lend_frame_scratch(size_t *out_bytes) {
    *out_bytes = sizeof(frame_rebuilt0);
    return &frame_rebuilt0;
}
#endif
static uint16_t sub_bg_color_ctx[EB_PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] EB_PPU_LINEBUF_ATTR;
static uint8_t  sub_bg_gp_ctx[EB_PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] EB_PPU_LINEBUF_ATTR;
static uint16_t obj_color_ctx[EB_PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] EB_PPU_LINEBUF_ATTR;
static uint8_t  obj_prio_ctx[EB_PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] EB_PPU_LINEBUF_ATTR;
static uint8_t  eff_tm_line_ctx[EB_PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] EB_PPU_LINEBUF_ATTR;
static uint8_t  eff_ts_line_ctx[EB_PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] EB_PPU_LINEBUF_ATTR;
static uint8_t  cm_prevented_line_ctx[EB_PPU_NUM_RENDER_CONTEXTS][LINE_BUF_WIDTH] EB_PPU_LINEBUF_ATTR;

/* Temp buffers for wide-mode non-filling layer render path.
 * Promoted from stack to static to avoid core 1 stack overflow (4KB limit). */
static uint32_t temp_main_ctx[EB_PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH] EB_PPU_LINEBUF_ATTR;
static uint16_t temp_sub_color_ctx[EB_PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH] EB_PPU_LINEBUF_ATTR;
static uint8_t  temp_sub_gp_ctx[EB_PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH] EB_PPU_LINEBUF_ATTR;
static uint8_t  temp_tm_all_ctx[EB_PPU_NUM_RENDER_CONTEXTS][SNES_WIDTH] EB_PPU_LINEBUF_ATTR;

/* Wide-mode decision for one frame (see the call in ppu_render_frame_ex).
 * Runs once per frame, so it stays out of EB_PPU_RAM_SECTION; noinline keeps
 * GCC from folding it into the RAM-resident caller.
 *
 * Any active layer that fills the viewport (64-tile tilemap or explicit
 * bg_viewport_fill flag) triggers full-width rendering. Other screens (intro,
 * menus, gas station, file select) use 32-tile tilemaps without fill and
 * render at SNES native resolution centered. Note: bg_distortion
 * (per-scanline HDMA) is applied inside render_bg_scanline() regardless of
 * wide_mode.
 *
 * Wide mode is enabled for ANY non-native viewport (both larger AND smaller
 * than SNES) so that filling layers render at EB_VIEWPORT_WIDTH while
 * non-filling layers are centered/cropped correctly.
 *
 * Backdrop-only frame: no BG layer and no OBJ on the main screen (e.g.
 * START_ENEMY_TOUCH_FLASH's TM=0 red flash) and no sub-screen layer in colour
 * math. On the SNES every pixel is then the backdrop colour (plus fixed-colour
 * math and brightness), so it renders across the whole viewport, gutters and
 * pad rows included; the native path would leave them black. Band pixels are
 * the same either way, since no pixel depends on layer or sprite placement. */
static __attribute__((noinline)) bool frame_wide_mode(const int bg_bpp[4],
                                                      uint8_t layers_needed,
                                                      bool sub_math) {
    if (EB_VIEWPORT_WIDTH == SNES_WIDTH && EB_VIEWPORT_HEIGHT == SNES_HEIGHT)
        return false;
    bool main_empty = !ppu.tm_hdma_active && !(ppu.tm & LAYER_OBJ);
    for (int bg = 0; bg < 4; bg++) {
        if (bg_bpp[bg] == 0) continue;
        if ((layers_needed & (1 << bg)) &&
            ((ppu.bg_sc[bg] & 0x01) || ppu.bg_viewport_fill[bg]))
            return true;
        if (ppu.tm & (1 << bg)) main_empty = false;
    }
    return main_empty && !(sub_math && (ppu.ts & 0x1F));
}

void PPU_HOT_FUNC(ppu_render_frame_ex)(int ctx_id, int y_start, int y_end,
                         int y_stride, scanline_callback_t send_scanline) {
#if EB_PPU_BG_ROW_CACHE
    /* Invalidate the tilemap-row plan cache at frame start. Within a frame scroll
     * is constant, so keying replays on tile_row_map (+scroll_x/width) is correct;
     * across frames the plan may be stale, hence this reset. */
    for (int bg = 0; bg < 4; bg++)
        bg_row_cache[ctx_id][bg].row = -1;
#endif
    pixel_t  *line_out        = CTX_LINE_OUT(ctx_id);
    uint32_t *best_bg         = CTX_BEST_BG(ctx_id);
    uint16_t *sub_bg_color    = sub_bg_color_ctx[ctx_id];
    uint8_t  *sub_bg_gp       = sub_bg_gp_ctx[ctx_id];
    uint16_t *obj_color       = obj_color_ctx[ctx_id];
    uint8_t  *obj_prio        = obj_prio_ctx[ctx_id];
    uint8_t  *eff_tm_line     = eff_tm_line_ctx[ctx_id];
    uint8_t  *eff_ts_line     = eff_ts_line_ctx[ctx_id];
    uint8_t  *cm_prevented_line = cm_prevented_line_ctx[ctx_id];

    TileRowCacheEntry *tile_cache = CTX_TILE_CACHE(ctx_id);

#ifdef EB_SMOOTH_CAMERA
    if (obj_mask_out)
        memset(obj_mask_out, 0, (size_t)EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT);
#endif

    /* Check force blank */
    if (ppu.inidisp & 0x80) {
        for (int y = y_start; y < y_end; y += y_stride) {
            memset(line_out, 0, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
            send_scanline(y, line_out);
            line_out = CTX_LINE_OUT(ctx_id);
        }
        return;
    }

    uint8_t brightness = ppu.inidisp & 0x0F;

    /* Invalidate tile row cache for this context — VRAM may have changed. */
    memset(tile_cache, 0xFF,
           (CTX_TILE_CACHE_MASK(ctx_id) + 1) * sizeof(TileRowCacheEntry));

    /* Shadow palette and OBJ buckets: built by ppu_prepare_frame() before
     * this call. In single-core mode, ppu_render_frame() calls it
     * automatically. In dual-core mode, the worker calls it before
     * signaling core 1. */

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

    /* Wide mode: a filling layer, or a backdrop-only frame (frame_wide_mode). */
    bool wide_mode = frame_wide_mode(bg_bpp, layers_needed,
                                     color_math_active && use_sub_screen);

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

#ifdef EB_PPU_PROFILE
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
    /* Layer windows are in effect only when the classification put some layer
     * into a batch. TMW/TSW alone don't decide it: a layer bit with neither of
     * its W12SEL/W34SEL/WOBJSEL windows enabled masks nothing (TMW=0x13 with
     * all window selects zero is left behind after some warps). In that case
     * precompute_window_masks would fill eff_tm/ts_line with base_tm/base_ts
     * and nothing else, which is exactly the window-free state, so the
     * window-free fast paths give identical output. */
    bool has_layer_windows = false;
    if (ppu.tmw || ppu.tsw) {
        classify_window_layers(&win_class);
        has_layer_windows =
            (win_class.tm_w1_noninv | win_class.ts_w1_noninv |
             win_class.tm_w1_inv    | win_class.ts_w1_inv    |
             win_class.tm_w2_noninv | win_class.ts_w2_noninv |
             win_class.tm_w2_inv    | win_class.ts_w2_inv    |
             win_class.dual_tm      | win_class.dual_ts) != 0;
    }
    /* Color math window can be active even when TMW/TSW=0 (e.g. battle swirl
     * uses WOBJSEL for color math windowing without masking any layers). */
    bool has_cm_windows = color_math_active && (prevent_mode == 1 || prevent_mode == 2);
    bool has_windows = has_layer_windows || has_cm_windows;
    /* CM-only windows (e.g. EB's darkness/tint scenes: cgadsub math with a
     * prevent window but TMW/TSW=0) mask COLOR MATH only — the per-pixel
     * layer masks eff_tm/ts_line stay uniform (= base_tm/base_ts;
     * precompute_window_masks writes them only under `if (ppu.tmw||ppu.tsw)`).
     * Everything except the color-math prevent check can therefore use the
     * window-free fast paths, keyed on this flag instead of has_windows. */
    const bool no_layer_windows = !has_layer_windows;
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
    const bool eff_lines_frame_constant = no_layer_windows && !ppu.tm_hdma_active;
    if (eff_lines_frame_constant) {
        memset(eff_tm_line, ppu.tm, render_width);
        memset(eff_ts_line, ppu.ts, render_width);
    }

    /* Establish the best_bg / temp_main all-zero (gp==0) invariant once per
     * frame. Inside the scanline loop their consumers zero each entry as they
     * read it (store-back: the compositor for best_bg, the wide-mode
     * merge loop for temp_main), replacing the old per-scanline/per-layer
     * memsets (~11% + ~9% of the overworld frame). This full-width clear
     * catches anything outside the consumers' coverage: mosaic writes past
     * render_width in non-wide frames, render_width changes between scenes,
     * force-blank frames. */
    memset(best_bg, 0, LINE_BUF_WIDTH * sizeof(uint32_t));
    memset(temp_main_ctx[ctx_id], 0, SNES_WIDTH * sizeof(uint32_t));

    PROF_SECTION(iter);
    for (int out_y = y_start; out_y < y_end; out_y += y_stride) {
        /* Border scanline — outside the renderable area */
        if (out_y < render_y_start || out_y >= render_y_end) {
            memset(line_out, 0, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
            send_scanline(out_y, line_out);
            line_out = CTX_LINE_OUT(ctx_id);
            continue;
        }

        int scanline = out_y - fb_y_offset;
        /* Per-scanline buffers are static per-context (aliased above).
         * Merged BG priority buffers — BG layers write directly here. */
        bool need_sub = color_math_active && use_sub_screen;

        PROF_SECTION(clear);
        /* best_bg is NOT cleared here: it enters every scanline all-zero
         * — the once-per-frame clear above establishes the invariant and the
         * compositor maintains it by zeroing each entry as it reads it
         * (store-back), including the head/tail it doesn't visit. */
        if (need_sub)
            memset(sub_bg_gp, 0, render_width);
        if (layers_needed & LAYER_OBJ) {
#if EB_PPU_OBJ_BUCKET
            /* Skip the clear when no sprite touches this scanline's band: nothing
             * writes obj_prio this line and the compositor skips reading it
             * (render_obj_scanline returns false -> scanline_has_obj false), so
             * stale contents are never observed. Conservative — an occupied band
             * with no exact-scanline overlap still clears, which is harmless.
             * The clear itself is span-limited: every nonzero entry lies inside
             * the dirty span render_obj_scanline recorded (see its definition),
             * so wiping that span restores the all-zero invariant. */
            int _oband = scanline >> OBJ_BAND_SHIFT;
            if (_oband >= 0 && _oband < OBJ_NUM_BANDS && obj_band_start[_oband + 1] > obj_band_start[_oband]) {
                if (obj_prio_dirty[ctx_id].hi > obj_prio_dirty[ctx_id].lo)
                    memset(obj_prio + obj_prio_dirty[ctx_id].lo, 0,
                           obj_prio_dirty[ctx_id].hi - obj_prio_dirty[ctx_id].lo);
                obj_prio_dirty[ctx_id].lo = LINE_BUF_WIDTH;
                obj_prio_dirty[ctx_id].hi = 0;
            }
#else
            memset(obj_prio, 0, LINE_BUF_WIDTH);
#endif
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
            /* In wide mode with per-scanline windows, gutter pixels sample
             * other scanlines' windows (window_membership), so equal window
             * values on this scanline don't mean an equal result. */
            bool gutter_samples_tables = wide_mode &&
                (ppu.window_hdma_active || ppu.window2_hdma_active);
            if (gutter_samples_tables || memcmp(key, win_cache_key, 6) != 0) {
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
        /* best_bg_* enters each scanline all-zero (compositor store-back), so
         * the first BG layer to write it can skip the per-pixel priority
         * compare (uncond). Cleared once a layer writes. */
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
                /* Overworld-space scenes (sprite_y_offset 0) centre the map
                 * and its sprites through the camera (EB_VIEWPORT_CENTER_Y),
                 * so the SNES picture sits PAD_TOP rows down. An explicit FILL
                 * or TALL layer here is band content laid out from tilemap
                 * row 0 (the credits photo frame on BG2, the staff roll on
                 * BG3): drop it by PAD_TOP too, as fill_pad does for x, or
                 * the photo sits PAD_TOP low in its frame. */
                if (!ppu.sprite_y_offset &&
                    (fills_explicit ||
                     ppu.bg_viewport_fill[bg] == BG_VIEWPORT_TALL))
                    bg_scanline -= EB_VIEWPORT_PAD_TOP;
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

            /* Set up render context for this layer. Field-by-field assignment
             * instead of a designated initializer: the initializer form makes
             * GCC memset the whole struct first (~3% of the overworld frame,
             * 720 calls/frame), all of it immediately overwritten. */
            BGRenderCtx ctx;
            ctx.main = best_bg;
            ctx.sub_color = need_sub ? sub_bg_color : NULL;
            ctx.sub_gp = need_sub ? sub_bg_gp : NULL;
            ctx.tm_line = eff_tm_line;
            ctx.ts_line = eff_ts_line;
            ctx.gp0_lm = bg_gp[bg][0] | ((uint16_t)layer_bit << 8);
            ctx.gp1_lm = bg_gp[bg][1] | ((uint16_t)layer_bit << 8);
            ctx.layer_bit = layer_bit;
            /* eff_tm_line/eff_ts_line are uniform (= base_tm/base_ts) when
             * no windows are active, so the per-pixel window mask reduces to
             * these constants — enables the window-mask-free emit variant. */
            ctx.no_window = no_layer_windows;
            ctx.main_on = (base_tm & layer_bit) != 0;
            ctx.sub_on  = (base_ts & layer_bit) != 0;
            /* First layer into the just-cleared best_bg writes unconditionally.
             * Only valid layer-window-free (uncond bypasses the per-pixel
             * mask; CM-only windows don't mask layers). */
            ctx.uncond = (uint8_t)(no_layer_windows && merged_empty);
            ctx.tile_cache = tile_cache;
            ctx.ctx_id = ctx_id;
            /* Span fields are only consumed on the temp path, which re-inits
             * them before each render; keep them defined for the copy. */
            ctx.emit_lo = 0;
            ctx.emit_hi = 0;

            if (layer_fills) {
                /* x_pad centers the native 256px content under the +PAD_LEFT
                 * sprite/window space, but ONLY for layers explicitly marked
                 * BG_VIEWPORT_FILL (battle BGs, Sound Stone, file select, logo).
                 * A layer that fills merely because it has a 64-tile-wide tilemap
                 * (the overworld map) is left at x_pad=0: that world already
                 * scrolls in its own coordinate space and is centered by the
                 * camera, so a PAD_LEFT shift would desync it from entities. */
                int fill_pad = fills_explicit ? EB_VIEWPORT_PAD_LEFT : 0;
                render_bg_scanline(bg, eff_scanline, bg_bpp[bg],
                                   &ctx, EB_VIEWPORT_WIDTH, fill_pad);
            } else if (wide_mode) {
                /* Non-filling layer in wide mode: render SNES_WIDTH into
                 * temp buffers, then merge visible portion into main. */
#if EB_VIEWPORT_PAD_LEFT > 0
                /* CONTINUE: gutter pixels (whole pad rows included) come
                 * from the band edge; the band itself renders below.
                 * Without side gutters only the pad rows need it (below). */
                if (ppu.bg_viewport_fill[bg] == BG_VIEWPORT_CONTINUE) {
                    merged_empty = false;
                    if (bg_gutter_fill(bg, scanline, &ctx, best_bg))
                        goto bg_mosaic_done;
                }
#endif
                /* Rows outside the SNES picture. Scenes with a
                 * sprite_y_offset centre the 224-line band in the viewport;
                 * overworld-space scenes (sprite_y_offset 0) draw a 256-px
                 * layer top-aligned: viewport row y is BG line VOFS + y. The
                 * SNES's 224-line picture starts at scanline 1, so it only
                 * ever shows lines VOFS+1 .. VOFS+224; the rows above and
                 * below hold tilemap rows the original never displays (e.g.
                 * the staff roll's $240C fill in BG3 rows 29-31, left there
                 * for Ness's room and THE END... ?). Keep the layer to the
                 * SNES picture, as its columns already are. BG_VIEWPORT_CLAMP
                 * layers extend their edge rows, and in overworld space
                 * BG_VIEWPORT_TALL layers (text that scrolls through the
                 * picture) keep drawing to the viewport edge. */
                if ((unsigned)(snes_scanline - (ppu.sprite_y_offset ? 0 : 1)) >=
                        SNES_HEIGHT &&
                    ppu.bg_viewport_fill[bg] != BG_VIEWPORT_CLAMP &&
                    (ppu.sprite_y_offset ||
                     ppu.bg_viewport_fill[bg] != BG_VIEWPORT_TALL)) {
#if EB_VIEWPORT_PAD_LEFT <= 0 && EB_VIEWPORT_PAD_TOP > 0
                    /* CONTINUE with pad rows only (the RP2040 port, 240x240):
                     * the pad rows continue the band's top/bottom edge. With
                     * side gutters the call above already covered them. */
                    if (ppu.bg_viewport_fill[bg] == BG_VIEWPORT_CONTINUE) {
                        merged_empty = false;
                        bg_gutter_fill(bg, scanline, &ctx, best_bg);
                    }
#endif
                    goto bg_mosaic_done;
                }

                /* Per-context static temp buffers (avoids 2KB stack alloc
                 * that would overflow core 1's 4KB SCRATCH_X stack) */
                uint32_t *temp_main = temp_main_ctx[ctx_id];
                uint16_t *temp_sub_color = temp_sub_color_ctx[ctx_id];
                uint8_t *temp_sub_gp = temp_sub_gp_ctx[ctx_id];
                uint8_t *temp_tm_all = temp_tm_all_ctx[ctx_id];
                /* temp_main is NOT cleared here: like best_bg it enters
                 * every use all-zero — the once-per-frame clear above
                 * establishes the invariant and the merge loop below maintains
                 * it by zeroing each non-zero entry as it consumes it
                 * (store-back), plus head/tail guards for the span the merge
                 * doesn't visit. Was ~9% of the overworld frame (512 B per
                 * layer per scanline). */
                /* temp_tm_all is NOT cleared: the temp render always uses the
                 * uncond emit variant (uncond=1, main_on=1 below), which never
                 * reads tm_line/ts_line, and the merge below reads the *main*
                 * eff_tm_line — so temp_tm_all is write-only dead work here
                 * (~3% of the overworld frame was this memset). If the emit
                 * dispatch ever stops taking EMIT_PIXELS_UNCOND for temp, restore
                 * `memset(temp_tm_all, 0xFF, SNES_WIDTH);` */
                /* temp_sub_gp is NOT cleared for the same reason: the uncond
                 * emit writes it without reading, and the merge gates the sub
                 * screen on `gp` from temp_gp_lm — temp_sub_gp is never read
                 * anywhere, so its clear was dead work too. */

                /* Temp context pointing to temp buffers */
                BGRenderCtx temp_ctx = ctx;
                temp_ctx.main = temp_main;
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
                /* temp_main enters each use all-zero (store-back in the merge
                 * loop) and each screen_x is written once per layer, so every
                 * temp write is into an empty slot. */
                temp_ctx.uncond = 1;
                /* Track the emitted x-span so the merge below only scans
                 * columns this layer could have written. */
                temp_ctx.emit_lo = SNES_WIDTH;
                temp_ctx.emit_hi = 0;

                render_bg_scanline(bg, eff_scanline, bg_bpp[bg],
                                   &temp_ctx, SNES_WIDTH, 0);

                /* Copy visible portion into main merged buffers */
                int pad = EB_VIEWPORT_PAD_LEFT;
                int src_start = pad < 0 ? -pad : 0;
                int dst_start = pad > 0 ?  pad : 0;
                int count = SNES_WIDTH - src_start;
                if (dst_start + count > EB_VIEWPORT_WIDTH)
                    count = EB_VIEWPORT_WIDTH - dst_start;

                /* Merge only the emitted span: i covers sx in
                 * [emit_lo, emit_hi) clamped to the visible window. All
                 * other temp entries were never written this scanline, so
                 * they are still zero (store-back invariant) and the scan
                 * would skip them anyway. On the overworld BG3 is usually
                 * fully blank -> the merge collapses to nothing. */
                int i_lo = temp_ctx.emit_lo - src_start;
                int i_hi = temp_ctx.emit_hi - src_start;
                if (i_lo < 0) i_lo = 0;
                if (i_hi > count) i_hi = count;

                if (no_layer_windows && !need_sub && ctx.main_on) {
                    /* No windows: eff_tm_line is uniformly base_tm, so the
                     * per-pixel mask test reduces to the loop-invariant
                     * main_on; no sub screen -> the sub merge block is dead.
                     * Same store-back-clear contract as the generic loop. */
                    for (int i = i_lo; i < i_hi; i++) {
                        int sx = src_start + i;
                        int dx = dst_start + i;
                        uint32_t t = temp_main[sx];
                        uint8_t gp = GP_LM_GP(t);
                        if (gp == 0) continue;
                        temp_main[sx] = 0;
                        /* Interleaved word: the color rides along in one copy. */
                        if (gp > GP_LM_GP(best_bg[dx]))
                            best_bg[dx] = t;
                    }
                    goto merge_done;
                }

                for (int i = i_lo; i < i_hi; i++) {
                    int sx = src_start + i;
                    int dx = dst_start + i;
                    uint32_t t = temp_main[sx];
                    uint8_t gp = GP_LM_GP(t);
                    if (gp == 0) continue;
                    /* Store-back clear: consume the entry so the buffer
                     * re-enters the next layer/scanline all-zero (replaces the
                     * per-layer memset). Zero entries need no store — they are
                     * already zero. */
                    temp_main[sx] = 0;
                    /* Apply window mask during merge */
                    if ((eff_tm_line[dx] & layer_bit) &&
                        gp > GP_LM_GP(best_bg[dx])) {
                        best_bg[dx] = t;
                    }
                    if (need_sub &&
                        (eff_ts_line[dx] & layer_bit) &&
                        gp > sub_bg_gp[dx]) {
                        sub_bg_gp[dx] = gp;
                        sub_bg_color[dx] = temp_sub_color[sx];
                    }
                }

merge_done:
                /* The store-back only covers the merged span; re-zero any
                 * head/tail the temp render wrote but the merge didn't visit.
                 * Empty on G&W (src_start == 0, count == SNES_WIDTH) — only a
                 * viewport narrower than SNES_WIDTH + pad produces one.
                 * (Entries inside the window but outside [i_lo, i_hi) were
                 * never written this scanline — the emit span bounds all
                 * writes — so they are still zero and need no re-clear.) */
                {
                    int merged_lo = src_start;
                    int merged_hi = src_start + count;
                    if (count <= 0) { merged_lo = 0; merged_hi = 0; }
                    if (merged_lo > 0)
                        memset(temp_main, 0, merged_lo * sizeof(uint32_t));
                    if (merged_hi < SNES_WIDTH)
                        memset(temp_main + merged_hi, 0,
                               (SNES_WIDTH - merged_hi) * sizeof(uint32_t));
                }

#if EB_VIEWPORT_PAD_LEFT > 0 || EB_VIEWPORT_PAD_TOP > 0
                /* Clamp mode: extend edge pixels into the border area */
                if (ppu.bg_viewport_fill[bg] == BG_VIEWPORT_CLAMP)
                    bg_gutter_fill(bg, scanline, &ctx, best_bg);
#endif
            } else {
                render_bg_scanline(bg, eff_scanline, bg_bpp[bg],
                                   &ctx, render_width, 0);
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
                    uint32_t ref_w = best_bg[x];
                    uint8_t ref_gp = GP_LM_GP(ref_w);
                    /* Only replicate if this pixel belongs to this layer */
                    if (ref_gp != gp0 && ref_gp != gp1) continue;
                    uint32_t ref_packed =
                        MAIN_PACK(MAIN_COLOR(ref_w),
                                  ref_gp | ((uint16_t)layer_bit << 8));
                    for (int dx = 1; dx < block && (x + dx) < EB_VIEWPORT_WIDTH; dx++) {
                        best_bg[x + dx] = ref_packed;
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
            scanline_has_obj = render_obj_scanline(ctx_id, scanline, obj_color, obj_prio, tile_cache);
        }
        PROF_END(obj, prof_obj);

#ifdef EB_SMOOTH_CAMERA
        if (obj_mask_out) {
            int mx_start = fb_x_offset < 0 ? -fb_x_offset : 0;
            int mx_end = render_width;
            if (fb_x_offset + mx_end > EB_VIEWPORT_WIDTH)
                mx_end = EB_VIEWPORT_WIDTH - fb_x_offset;
            bool obj_on = scanline_has_obj &&
                (!no_layer_windows || (base_tm & LAYER_OBJ));
            obj_mask_scanline(scanline, out_y, fb_x_offset, mx_start, mx_end,
                              obj_on, no_layer_windows, eff_tm_line, obj_prio,
                              best_bg, obj_thresh);
        }
#endif

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
            bool obj_main_en = scanline_has_obj && (no_layer_windows ? (base_tm & LAYER_OBJ) != 0 : false);
            bool obj_sub_en = scanline_has_obj && (no_layer_windows ? (base_ts & LAYER_OBJ) != 0 : false);

            /* Whole-row gutter: a scanline outside the centered SNES content
             * band (top/bottom letterbox in a taller viewport) has no centered
             * main layer at any column, so every pixel is a gutter pixel. Only
             * relevant when a sub-screen color-math layer can fill it. */
            bool row_is_vgutter = wide_mode && need_sub &&
                (snes_scanline < 0 || snes_scanline >= SNES_HEIGHT);

            /* Fast path for the dominant case (all of the overworld and most
             * scenes): no windows, no color math, full brightness. The
             * generic loop below evaluates four per-pixel conditionals that
             * are all frame-constant false here (math eligibility, gutter
             * fill, color-math apply, brightness), plus computes main_layer
             * solely to feed them — and it is too big for GCC to unswitch,
             * so the dead flags are re-loaded from spill slots every pixel.
             * This specialized loop is just: pick OBJ vs BG vs backdrop,
             * store-back-clear, store. Same store-back invariant as the
             * generic loop (best_bg_gp_lm re-enters the next scanline
             * all-zero). */
            if (no_layer_windows && !color_math_active && brightness == 0x0F) {
                if (obj_main_en) {
                    for (int x = x_start; x < x_end; x++) {
                        uint32_t bg_w = best_bg[x];
                        best_bg[x] = 0;
                        uint8_t bg_gp_val = GP_LM_GP(bg_w);
                        uint8_t obj_pk = obj_prio[x];
                        uint16_t color;
                        if (obj_pk > 0 && bg_gp_val < obj_thresh[obj_pk])
                            color = obj_color[x];
                        else if (bg_gp_val > 0)
                            color = MAIN_COLOR(bg_w);
                        else
                            color = backdrop;
                        line_out[x + fb_x_offset] = color;
                    }
                } else {
                    for (int x = x_start; x < x_end; x++) {
                        uint32_t bg_w = best_bg[x];
                        best_bg[x] = 0;
                        line_out[x + fb_x_offset] =
                            GP_LM_GP(bg_w) > 0 ? MAIN_COLOR(bg_w)
                                               : backdrop;
                    }
                }
                goto composite_tail;
            }

            for (int x = x_start; x < x_end; x++) {
                uint16_t color;
                uint8_t main_layer;

                /* OBJ check: window-masked or constant. scanline_has_obj gates
                 * the load on both paths (no_layer_windows folds into obj_main_en). */
                uint8_t obj_pk;
                if (no_layer_windows)
                    obj_pk = obj_main_en ? obj_prio[x] : 0;
                else
                    obj_pk = (scanline_has_obj && (eff_tm_line[x] & LAYER_OBJ)) ? obj_prio[x] : 0;

                uint32_t bg_w = best_bg[x];
                /* Store-back clear: leave the entry zero for the next scanline
                 * (replaces the per-scanline memset — see the clear phase). */
                best_bg[x] = 0;
                uint8_t bg_gp_val = GP_LM_GP(bg_w);

                if (obj_pk > 0 && bg_gp_val < obj_thresh[obj_pk]) {
                    color = obj_color[x];
                    main_layer = LAYER_OBJ;
                } else if (bg_gp_val > 0) {
                    color = MAIN_COLOR(bg_w);
                    main_layer = GP_LM_LM(bg_w);
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
                    bool halve = half_math;
                    if (need_sub) {
                        uint8_t sub_obj_pk;
                        if (no_layer_windows)
                            sub_obj_pk = obj_sub_en ? obj_prio[x] : 0;
                        else
                            sub_obj_pk = (scanline_has_obj && (eff_ts_line[x] & LAYER_OBJ)) ? obj_prio[x] : 0;
                        uint8_t sub_gp_val = sub_bg_gp[x];
                        if (sub_obj_pk > 0 && sub_gp_val < obj_thresh[sub_obj_pk])
                            sub_color = obj_color[x];
                        else if (sub_gp_val > 0)
                            sub_color = sub_bg_color[x];
                        else {
                            /* Sub-screen backdrop: the hardware uses the fixed
                             * colour (COLDATA) and skips the half (bsnes/fullsnes).
                             * The 2-layer battle BGs (LOAD_BATTLEBG,
                             * asm/battle/load_battlebg.asm:270/561, colour-math
                             * config 7/3: TS = the other BG layer, CGADSUB $61/$64)
                             * rely on it: where the sub layer is transparent the
                             * main layer shows at full intensity. Emu-checked on
                             * the Plague Rat (BG config 3, mode 0): the original
                             * shows palette colour (24,0,6) unhalved as
                             * (198,0,49); the port drew it halved. */
                            sub_color = fixed_color;
                            halve = false;
                        }
                    } else {
                        sub_color = fixed_color;
                    }
                    color = blend_colors(color, sub_color, subtract_mode, halve);
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

composite_tail:
            /* The store-back clear above only covers [x_start, x_end); re-zero
             * any head/tail the compositor didn't visit so the next scanline's
             * all-zero invariant holds. Both ranges are empty unless the
             * viewport is narrower than the render width (never on G&W) —
             * frame-constant conditions, predicted away. */
            if (x_start > 0)
                memset(best_bg, 0, x_start * sizeof(uint32_t));
            if (x_end < render_width)
                memset(best_bg + x_end, 0,
                       (render_width - x_end) * sizeof(uint32_t));
        }
        PROF_END(comp, prof_composite);

        PROF_SECTION(snd);
        send_scanline(out_y, line_out);
        line_out = CTX_LINE_OUT(ctx_id);
        PROF_END(snd, prof_send);
    }
    PROF_END(iter, prof_iter);

#ifdef EB_PPU_PROFILE
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
    ppu_prepare_frame();
    ppu_render_frame_ex(0, 0, EB_VIEWPORT_HEIGHT, 1, send_scanline);

#ifdef EB_PPU_PROFILE
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
