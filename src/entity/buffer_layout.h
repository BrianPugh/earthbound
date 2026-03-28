/*
 * buffer_layout.h — Named constants for ert.buffer[] offset regions.
 *
 * ert.buffer is a 64 KB general-purpose work area mirroring SNES WRAM at
 * $7F0000 (BUFFER in the assembly).  Different game phases reuse the same
 * offsets; see entity.h's BUFFER REGION MAP for concurrency rules.
 *
 * Constants are grouped by subsystem.  Naming convention:
 *   BUF_<SUBSYSTEM>_<PURPOSE>
 */
#ifndef BUFFER_LAYOUT_H
#define BUFFER_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

/* ===== Palette Fade Engine (callroutine_palette.c, title_screen.c, gas_station.c) ===== */
/* 256 colors × 2 bytes = 512 bytes per array; total 3584 bytes (0x0E00). */
#define BUF_FADE_TARGET     0x0000   /* Target palette (256 × uint16) */
#define BUF_FADE_SLOPE_R    0x0200   /* Red channel slopes   (256 × int16, 8.8 fixed-point) */
#define BUF_FADE_SLOPE_G    0x0400   /* Green channel slopes */
#define BUF_FADE_SLOPE_B    0x0600   /* Blue channel slopes  */
#define BUF_FADE_ACCUM_R    0x0800   /* Red channel accumulators */
#define BUF_FADE_ACCUM_G    0x0A00   /* Green channel accumulators */
#define BUF_FADE_ACCUM_B    0x0C00   /* Blue channel accumulators */
#define BUF_FADE_COLOR_COUNT 256     /* Number of palette entries in the fade arrays */

/* ===== Map Palette Fade (overworld_palette.c) ===== */
/* 96 colors (groups 2-7) × 2 bytes = 192 bytes per array.
 * Relocated from assembly's 0x7800 to pack after palette fade region.
 * Only active during comeback sequence (entities disabled), so sharing
 * the same buffer region as entity fade states is safe. */
#define BUF_MAP_FADE_TARGET      0x0E00  /* Target palette */
#define BUF_MAP_FADE_SLOPE_R     0x0F00  /* Red channel slopes */
#define BUF_MAP_FADE_SLOPE_G     0x1000  /* Green channel slopes */
#define BUF_MAP_FADE_SLOPE_B     0x1100  /* Blue channel slopes */
#define BUF_MAP_FADE_ACCUM_R     0x1200  /* Red channel accumulators */
#define BUF_MAP_FADE_ACCUM_G     0x1300  /* Green channel accumulators */
#define BUF_MAP_FADE_ACCUM_B     0x1400  /* Blue channel accumulators */
#define BUF_MAP_FADE_COLOR_COUNT 96      /* Colors in palette groups 2-7 */

/* ===== Entity Fade States (callbacks.c) ===== */
/* Relocated from assembly's 0x7C00 to pack tightly after palette fade region.
 * Active during overworld; concurrent with palette fade or entity sprite data. */
#define BUF_ENTITY_FADE_STATES   0x0E00  /* 20-byte entries, up to ~50 (1024 B) */
#define BUF_ENTITY_FADE_STATES_SIZE 1024

/* ===== Entity Tile Merge Slot Table (callbacks.c) ===== */
/* Relocated from assembly's 0x7F00 to right after entity fade states. */
#define BUF_TILE_MERGE_SLOTS     0x1200  /* 64 entries × 2 bytes = 128 B */
#define BUF_TILE_MERGE_SLOTS_SIZE 128

/* ===== Pathfinding (pathfinding.c) ===== */
#define BUF_PATHFINDING_MATRIX   0x3000  /* BFS collision grid (~2 KB, max 4 KB for 64x64) */
#define BUF_PF_HEAP              0x4000  /* BFS heap/scratch (0xC00 = 3072 bytes, ends 0x4C00) */

/* ===== VWF Save/Restore (text.c) ===== */
/* 1664-byte snapshot of vwf_buffer, used by vwf_render_string_at(),
 * vwf_render_eb_string_at(), and render_title_to_vram() to save/restore
 * VWF state when rendering titles or inline strings.
 *
 * Phase-exclusive with pathfinding: VWF save/restore runs during
 * render_all_windows() (per-frame rendering phase), while pathfinding
 * runs during entity script ticks. They never execute concurrently
 * within a single frame. Overlaps BUF_PF_HEAP at 0x4980–0x5000. */
#define BUF_VWF_SAVE_SIZE        1664  /* VWF_BUFFER_TILES(52) * VWF_TILE_BYTES(32) */
#define BUF_VWF_SAVE             (BUFFER_SIZE - BUF_VWF_SAVE_SIZE)  /* 0x4980 */

/* ===== Text Window GFX Tile Uploads (battle_ui.c) ===== */
/* Scattered text tile blocks uploaded to VRAM TEXT_LAYER_TILES. */
#define BUF_TEXT_TILES_BLOCK1    0x0000  /* 0x450 bytes → TEXT_LAYER_TILES+0x000 */
#define BUF_TEXT_TILES_BLOCK2    0x04F0  /* 0x060 bytes → TEXT_LAYER_TILES+0x278 */
#define BUF_TEXT_TILES_BLOCK3    0x05F0  /* 0x0B0 bytes → TEXT_LAYER_TILES+0x2F8 */
#define BUF_TEXT_TILES_BLOCK4    0x0700  /* 0x0A0 bytes → TEXT_LAYER_TILES+0x380 */
#define BUF_TEXT_TILES_BLOCK5    0x0800  /* 0x010 bytes → TEXT_LAYER_TILES+0x420 */
#define BUF_TEXT_TILES_BLOCK6    0x0900  /* 0x010 bytes → TEXT_LAYER_TILES+0x480 */
#define BUF_TEXT_LAYER2_TILES    0x2000  /* 0x1800 bytes → TEXT_LAYER_TILES+0x1000 */

/* ===== Your Sanctuary (map_loader.c) ===== */
/* Intentional divergence: assembly pre-caches all 8 sanctuaries (20 KB).
 * C port loads on demand into a single slot (~2.2 KB), trading CPU for RAM.
 * Arrangement/GFX decomp uses arrangement_buffer (not ert.buffer). */
#define BUF_SANCTUARY_TILEMAPS   0x0000  /* Single tilemap: 30×32×2 = 0x780 bytes */
#define BUF_SANCTUARY_PALETTES   0x0800  /* Single palette: 256 bytes */
#define BUF_SANCTUARY_MAP_BLOCKS 0x0900  /* 1024 × uint16 = 0x800 bytes (transient during load) */

/* ===== Naming Screen (callroutine.c) ===== */
/* Naming screen is an exclusive phase. Tiles at 0x0000, tilemap right after.
 * Relocated from assembly's 0x2000/0x4000 to pack tightly. */
#define BUF_NAME_TILES           0x0000  /* VWF staging area (0x2000 bytes) */
#define BUF_NAME_TILEMAP         0x2000  /* Tilemap for name display */
#define BUF_NAME_TILEMAP_HEADER  0x0000  /* 2-byte tilemap header (width, height) */

/* ===== PSI Animation (battle_psi.c) ===== */
/* PSI arrangements use .arr.bundled format — decompressed per-bundle into
 * PsiAnimationState.arr_bundle_buf (8 KB), not into ert.buffer.
 * PSI GFX decompresses directly to ppu.vram — no ert.buffer usage. */

/* ===== Ending / Credits (ending.c) ===== */
/* Credits init decomp uses 0x0000–0x26FF transiently (immediate VRAM upload).
 * Cast tile GFX composes directly in ppu.vram (not ert.buffer).
 * Only the tilemap scratch and palette data persist during playback. */
#define BUF_CREDITS_GFX_1        0x0200  /* Credits init: decomp region 1 (transient) */
#define BUF_CREDITS_GFX_2        0x0600  /* Credits init: decomp region 2 (transient) */
#define BUF_CREDITS_LAYER2       0x0700  /* Credits init: layer 2 tiles (transient) */
#define BUF_CREDITS_TILEMAP      0x0F00  /* Cast name scroll tilemap (128 B) */
#define BUF_CREDITS_PALETTE      0x0E00  /* Special cast palette data (256 B) */

/* ===== Tilemap Animation (callroutine.c) ===== */
#define BUF_TILEMAP_ANIM_LOWER   0x1000  /* Lower tilemap animation buffer */
#define BUF_TILEMAP_ANIM_UPPER   0x4000  /* Upper tilemap animation buffer */

/* ===== Sanctuary Flash Fade (map_loader.c) ===== */
/* Compact 96-color fade using sequential layout instead of 256-wide arrays. */
#define BUF_FLASH_TARGET         0x0000  /* 96 × 2 = 192 bytes */
#define BUF_FLASH_ACCUM_R        192     /* Red accumulators (192 bytes) */
#define BUF_FLASH_ACCUM_G        384     /* Green accumulators */
#define BUF_FLASH_ACCUM_B        576     /* Blue accumulators */
#define BUF_FLASH_SLOPE_R        768     /* Red increments */
#define BUF_FLASH_SLOPE_G        960     /* Green increments */
#define BUF_FLASH_SLOPE_B        1152    /* Blue increments */

/* ===== Battle Palette Save (battle_ui.c, battle.c) ===== */
#define BUF_BATTLE_PALETTE_SAVE  0x2000  /* 512 bytes: palettes saved for battle fade */

/* =====================================================================
 * Typed struct overlays for structured buffer regions.
 *
 * These provide type-safe access to well-defined regions, replacing manual
 * byte arithmetic with array indexing.  Usage:
 *
 *   PaletteFadeBuffer *fade = buf_palette_fade();
 *   fade->slope_r[i] = get_colour_fade_slope(c_r, t_r, frames);
 *   int16_t r = (fade->accum_r[i] >> 8) & 0x1F;
 *
 * All structs are packed to match the SNES little-endian byte layout.
 * Target platforms (x86, ARM) are also little-endian, so direct overlay
 * on ert.buffer[] is safe without byte swapping.
 * ===================================================================== */

/* Palette Fade Engine: 7 parallel arrays × 256 entries.
 * Overlays ert.buffer[BUF_FADE_TARGET .. BUF_FADE_TARGET + 0x0DFF]. */
typedef struct {
    uint16_t target[256];   /* 0x000: fade target palette (BGR555) */
    int16_t  slope_r[256];  /* 0x200: red 8.8 fixed-point slopes */
    int16_t  slope_g[256];  /* 0x400: green slopes */
    int16_t  slope_b[256];  /* 0x600: blue slopes */
    int16_t  accum_r[256];  /* 0x800: red accumulators */
    int16_t  accum_g[256];  /* 0xA00: green accumulators */
    int16_t  accum_b[256];  /* 0xC00: blue accumulators */
} PaletteFadeBuffer;        /* 0xE00 = 3584 bytes */

_Static_assert(sizeof(PaletteFadeBuffer) == 0x0E00,
               "PaletteFadeBuffer must be 3584 bytes");
_Static_assert(offsetof(PaletteFadeBuffer, slope_r) == BUF_FADE_SLOPE_R,
               "slope_r offset mismatch");
_Static_assert(offsetof(PaletteFadeBuffer, accum_b) == BUF_FADE_ACCUM_B,
               "accum_b offset mismatch");

/* Map Palette Fade: 7 parallel arrays × 96 entries.
 * Overlays ert.buffer[BUF_MAP_FADE_TARGET .. BUF_MAP_FADE_TARGET + 6*0x100].
 * NOTE: each array occupies 192 bytes but is 0x100-aligned in the buffer
 * (64 bytes of padding between arrays). The struct must reflect this. */
typedef struct {
    uint16_t target[128];   /* 0x00: target palette (96 used, 32 padding) */
    int16_t  slope_r[128];  /* 0x100: red slopes */
    int16_t  slope_g[128];  /* 0x200: green slopes */
    int16_t  slope_b[128];  /* 0x300: blue slopes */
    int16_t  accum_r[128];  /* 0x400: red accumulators */
    int16_t  accum_g[128];  /* 0x500: green accumulators */
    int16_t  accum_b[128];  /* 0x600: blue accumulators */
} MapPaletteFadeBuffer;     /* 0x700 = 1792 bytes */

_Static_assert(sizeof(MapPaletteFadeBuffer) == 0x0700,
               "MapPaletteFadeBuffer must be 1792 bytes");
_Static_assert(offsetof(MapPaletteFadeBuffer, slope_r) ==
               BUF_MAP_FADE_SLOPE_R - BUF_MAP_FADE_TARGET,
               "map slope_r offset mismatch");

/* Entity Fade State Entry: 20 bytes per entity fade.
 * Array stored at ert.buffer[BUF_ENTITY_FADE_STATES]. */
typedef struct {
    uint16_t entity_slot;      /* [0]  entity slot being faded */
    uint16_t fade_param;       /* [2]  fade type (2-5=wipe, 7-10=reveal) */
    uint16_t direction;        /* [4]  1=up, 2=down, 3=left, 4=right */
    uint16_t tile_byte_width;  /* [6]  bytes per tile row */
    uint16_t height_pixels;    /* [8]  height in pixels (tile_heights * 8) */
    uint16_t buf_offset_1;     /* [10] first sprite copy offset in buffer */
    uint16_t buf_offset_2;     /* [12] second sprite copy offset */
    uint16_t total_sprite_size;/* [14] sprite data size (width*height/2) */
    uint16_t frame_counter_1;  /* [16] animation frame counter */
    uint16_t frame_counter_2;  /* [18] animation frame counter */
} EntityFadeEntry;             /* 20 bytes */

_Static_assert(sizeof(EntityFadeEntry) == 20,
               "EntityFadeEntry must be 20 bytes");

/* ---- Accessor helpers ---- */

/* Get typed overlay for the palette fade engine region.
 * Caller must ensure buffer is in palette-fade mode (not entity-fade-sprite). */
static inline PaletteFadeBuffer *buf_palette_fade(uint8_t *buffer) {
    return (PaletteFadeBuffer *)(buffer + BUF_FADE_TARGET);
}

/* Get typed overlay for the map palette fade region. */
static inline MapPaletteFadeBuffer *buf_map_palette_fade(uint8_t *buffer) {
    return (MapPaletteFadeBuffer *)(buffer + BUF_MAP_FADE_TARGET);
}

/* Get typed overlay for entity fade state entries. */
static inline EntityFadeEntry *buf_entity_fade_entries(uint8_t *buffer) {
    return (EntityFadeEntry *)(buffer + BUF_ENTITY_FADE_STATES);
}

#endif /* BUFFER_LAYOUT_H */

