/*
 * buffer_layout.h — the ert.buffer[] budget: named regions, checked.
 *
 * ert.buffer mirrors the asm's BUFFER ($7F0000, .RES $F800) but is only
 * BUFFER_SIZE (0x5000, 20 KB) here; see entity.h's BUFFER REGION MAP for the
 * history and the per-phase notes. Different game phases reuse the same
 * offsets, so the budget is:
 *
 *   1. Every region is a BUF_<NAME> offset plus a BUF_<NAME>_SIZE, and
 *      BUF_ASSERT_FITS() checks it ends inside BUFFER_SIZE.
 *   2. Regions that are live at the same time get BUF_ASSERT_DISJOINT()
 *      (the "CONCURRENT REGIONS" block below). Regions that share bytes on
 *      purpose are listed under "DELIBERATE ALIASES", each with the reason
 *      they never hold live data at the same time.
 *   3. Code reaches the buffer through BUF_REGION(region, rel, len),
 *      buf_at(off, len) or one of the typed overlays at the end. In Debug
 *      builds these assert that [off, off + len) stays inside the region
 *      (BUF_REGION) or inside BUFFER_SIZE (buf_at); Release compiles the
 *      checks out.
 *
 * Naming convention: BUF_<SUBSYSTEM>_<PURPOSE>, size BUF_<...>_SIZE.
 */
#ifndef BUFFER_LAYOUT_H
#define BUFFER_LAYOUT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "entity/entity.h"   /* ert, BUFFER_SIZE, BUFFER_*_CHECK */

/* ---- Accessors ---- */

/* &ert.buffer[off], asserting (Debug) that [off, off + len) is in the buffer.
 * For staging whose offset comes from data (asset tables, decompressed
 * headers, bump allocators) rather than from one budgeted region. */
static inline uint8_t *buf_at(size_t off, size_t len) {
    BUFFER_BOUNDS_CHECK(off, len);
    (void)len;
    return &ert.buffer[off];
}

/* &ert.buffer[base + rel], asserting (Debug) that [rel, rel + len) is inside
 * the region [base, base + size). Use through BUF_REGION(). */
static inline uint8_t *buf_region_at(size_t base, size_t size,
                                     size_t rel, size_t len) {
    BUFFER_REGION_CHECK(base + rel, len, base, size);
    BUFFER_BOUNDS_CHECK(base + rel, len);
    (void)size;
    (void)len;
    return &ert.buffer[base + rel];
}

/* The whole buffer as a decomp target (BUF_DECOMP_SCRATCH). Pass
 * BUF_DECOMP_SCRATCH_SIZE as the decomp limit. */
static inline uint8_t *buf_decomp_scratch(void) {
    return &ert.buffer[0];
}

/* BUF_REGION(BUF_FOO, rel, len) == &ert.buffer[BUF_FOO + rel], checked against
 * BUF_FOO_SIZE. `region` must be the bare BUF_* name (its _SIZE is pasted). */
#define BUF_REGION(region, rel, len) \
    buf_region_at((region), region##_SIZE, (size_t)(rel), (size_t)(len))

/* ---- Compile-time budget checks ---- */

/* Each macro pastes `_SIZE` itself: an argument handed on to another macro
 * would be expanded to a number first. */
#define BUF_REGION_END(region) ((region) + region##_SIZE)
#define BUF_ASSERT_FITS(region) \
    _Static_assert((region) + region##_SIZE <= BUFFER_SIZE, \
                   #region " must end inside BUFFER_SIZE")
#define BUF_ASSERT_DISJOINT(a, b) \
    _Static_assert((a) + a##_SIZE <= (b) || (b) + b##_SIZE <= (a), \
                   #a " and " #b " are live together and must not overlap")
/* `inner` lies inside `outer` (a sub-block of one owner's staging). */
#define BUF_ASSERT_WITHIN(inner, outer) \
    _Static_assert((inner) >= (outer) && \
                   (inner) + inner##_SIZE <= (outer) + outer##_SIZE, \
                   #inner " must lie inside " #outer)

/* ===== Decompression scratch (whole buffer) ===== */
/* Decomp target for loads that are copied out (to VRAM/CGRAM) before the
 * caller returns, plus the animation sequences and title gradients that later
 * callroutines read back (see DELIBERATE ALIASES). Measured decompressed
 * sizes: town maps 0x4840, animation sequences <= 0x4618 (lightning reflect),
 * text window GFX 0x1A00, photograph palettes 0xF00, credits BG2 0x900,
 * credits font 0xC00, title gradients <= 0x280. */
#define BUF_DECOMP_SCRATCH       0x0000
#define BUF_DECOMP_SCRATCH_SIZE  BUFFER_SIZE

/* ===== Palette Fade Engine (callroutine_palette.c, title_screen.c, gas_station.c) ===== */
/* 256 colors × 2 bytes = 512 bytes per array; total 3584 bytes (0x0E00). */
#define BUF_FADE                 0x0000
#define BUF_FADE_SIZE            0x0E00
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
 * the same buffer region as the entity fade sprite copies is safe. */
#define BUF_MAP_FADE             0x0E00
#define BUF_MAP_FADE_SIZE        0x0700
#define BUF_MAP_FADE_TARGET      0x0E00  /* Target palette */
#define BUF_MAP_FADE_SLOPE_R     0x0F00  /* Red channel slopes */
#define BUF_MAP_FADE_SLOPE_G     0x1000  /* Green channel slopes */
#define BUF_MAP_FADE_SLOPE_B     0x1100  /* Blue channel slopes */
#define BUF_MAP_FADE_ACCUM_R     0x1200  /* Red channel accumulators */
#define BUF_MAP_FADE_ACCUM_G     0x1300  /* Green channel accumulators */
#define BUF_MAP_FADE_ACCUM_B     0x1400  /* Blue channel accumulators */
#define BUF_MAP_FADE_COLOR_COUNT 96      /* Colors in palette groups 2-7 */

/* ===== LOAD_MAP_PALETTE fade (map_loader.c: CC_1F_E1, GAME_MODE_MAP_PALETTE_FADE) ===== */
/* The asm's INITIALIZE_MAP_PALETTE_FADE arrays (BUFFER+$7800..$7EFF, $100
 * stride) packed into one 96-color layout at the buffer start. Lives across
 * frames, but only inside GAME_MODE_MAP_PALETTE_FADE, which runs no entity
 * scripts. (The FLASH name is historical.) */
#define BUF_FLASH                0x0000
#define BUF_FLASH_ARRAY_SIZE     192     /* 96 × 2 bytes */
#define BUF_FLASH_SIZE           (7 * BUF_FLASH_ARRAY_SIZE)  /* 0x540 */
#define BUF_FLASH_TARGET         0x0000  /* 96 × 2 = 192 bytes */
#define BUF_FLASH_ACCUM_R        192     /* Red accumulators (192 bytes) */
#define BUF_FLASH_ACCUM_G        384     /* Green accumulators */
#define BUF_FLASH_ACCUM_B        576     /* Blue accumulators */
#define BUF_FLASH_SLOPE_R        768     /* Red increments */
#define BUF_FLASH_SLOPE_G        960     /* Green increments */
#define BUF_FLASH_SLOPE_B        1152    /* Blue increments */

/* ===== Entity Fade (callbacks.c, INIT_ENTITY_FADE_STATE) ===== */
/* Sprite copies: DOUBLE_ENTITY_FADE_STATE bump-allocates 2 copies of each
 * fading sprite (tile_byte_width * height_px bytes) from BUFFER+$0000 upward,
 * with no bound check, into the space below the metadata at BUFFER+$7C00
 * (31 KB). The allocator only resets when a fade batch starts with no fade
 * entity alive, so concurrent fades accumulate. Per sprite: 384 B for a
 * 16x24 NPC (most sprites), 3072 B for the vehicles, 4096 B Tessie, 5120 B
 * Dungeon Man. 0x2B00 holds 28 NPCs, or 2 Dungeon Men plus 2 NPCs.
 * Aliases BUF_FADE_* (palette fade) like the asm, and BUF_MAP_FADE_* (only
 * used with entities disabled). */
#define BUF_ENTITY_FADE_SPRITES      0x0000
#define BUF_ENTITY_FADE_SPRITES_SIZE 0x2B00

/* Metadata: 20-byte EntityFadeEntry records, 1024 B like the asm ($7C00,
 * 51 entries). Relocated to sit right after the sprite copies. */
#define BUF_ENTITY_FADE_STATES   (BUF_ENTITY_FADE_SPRITES + BUF_ENTITY_FADE_SPRITES_SIZE) /* 0x2B00 */
#define BUF_ENTITY_FADE_STATES_SIZE 1024
#define ENTITY_FADE_STATES_MAX   (BUF_ENTITY_FADE_STATES_SIZE / 20)  /* 51 */

/* ===== Entity Tile Merge Slot Table (callbacks.c) ===== */
/* 64 16-bit slots. Relocated from assembly's 0x7F00 to right after the
 * entity fade states. */
#define BUF_TILE_MERGE_SLOTS     (BUF_ENTITY_FADE_STATES + BUF_ENTITY_FADE_STATES_SIZE) /* 0x2F00 */
#define BUF_TILE_MERGE_SLOTS_SIZE 128

/* ===== Pathfinding (pathfinding.c) ===== */
/* The asm keeps both in its own PATHFINDING_BUFFER. Both are rebuilt by every
 * pathfinding call and are dead once it returns. */
#define BUF_PATHFINDING_MATRIX      0x3000  /* BFS collision grid, rows × columns bytes */
#define BUF_PATHFINDING_MATRIX_SIZE 0x1000  /* max 64 × 64 */
#define BUF_PF_HEAP                 0x4000  /* BFS heap/scratch (16-bit words) */
#define BUF_PF_HEAP_SIZE            0x0C00  /* asm PATHFINDING_BUFFER heap: 3072 bytes */

/* ===== VWF Save/Restore (text.c) ===== */
/* 1664-byte snapshot of vwf_buffer, used by vwf_render_string_at(),
 * vwf_render_eb_string_at(), and render_title_to_vram() to save/restore
 * VWF state when rendering titles or inline strings. Saved and restored
 * inside one call. Overlaps BUF_PF_HEAP at 0x4980–0x5000 (see aliases). */
#define BUF_VWF_SAVE_SIZE        1664  /* VWF_BUFFER_TILES(52) * VWF_TILE_BYTES(32) */
#define BUF_VWF_SAVE             (BUFFER_SIZE - BUF_VWF_SAVE_SIZE)  /* 0x4980 */

/* ===== Text Window GFX (text.c text_load_window_gfx, battle_ui.c) ===== */
/* LOAD_WINDOW_GFX / FILE_SELECT_INIT staging, all inside one call:
 * decompress TEXT_WINDOW_GFX (0x1A00) to 0, MEMCPY24 0x2A00 bytes from
 * $1000 up to $2000, overlay FLAVOURED_TEXT_GFX at $100, compose the party
 * name tiles at $2A00 and the status icons at $2C00, then upload. The peak
 * is the moved block's end, 0x4A00. UPLOAD_TEXT_TILES_TO_VRAM (battle_ui.c)
 * re-reads the upload blocks. */
#define BUF_TEXT_GFX             0x0000
#define BUF_TEXT_GFX_SIZE        0x4A00
#define BUF_TEXT_GFX_CHECKER     0x0070  /* checkerboard pattern tile (16 B) */
#define BUF_TEXT_GFX_CHECKER_SIZE 0x0010
#define BUF_TEXT_GFX_FLAVOUR     0x0100  /* FLAVOURED_TEXT_GFX overlay (decomp) */
#define BUF_TEXT_GFX_FLAVOUR_SIZE (BUFFER_SIZE - BUF_TEXT_GFX_FLAVOUR)
#define BUF_TEXT_GFX_MOVE_SRC    0x1000  /* MEMCPY24 source */
#define BUF_TEXT_GFX_MOVE_DST    0x2000  /* MEMCPY24 destination */
#define BUF_TEXT_GFX_MOVE_SIZE   0x2A00
#define BUF_TEXT_NAME_TILES      0x2A00  /* 4 names × 4 tiles; lower halves at +256 */
#define BUF_TEXT_NAME_TILES_SIZE 0x0200
#define BUF_TEXT_STATUS_TILES    0x2C00  /* status icons, 16 B each; lower halves at +256 */
#define BUF_TEXT_STATUS_TILES_SIZE (0x100 + 7 * 7 * 16) /* 49 entries max (AFFLICTION_GROUP_COUNT × 7) */

/* Upload blocks (UPLOAD_TEXT_TILES_TO_VRAM): source → TEXT_LAYER_TILES word offset. */
#define BUF_TEXT_TILES_BLOCK1    0x0000  /* → TEXT_LAYER_TILES+0x000 */
#define BUF_TEXT_TILES_BLOCK1_SIZE 0x0450
#define BUF_TEXT_TILES_BLOCK2    0x04F0  /* → TEXT_LAYER_TILES+0x278 */
#define BUF_TEXT_TILES_BLOCK2_SIZE 0x0060
#define BUF_TEXT_TILES_BLOCK3    0x05F0  /* → TEXT_LAYER_TILES+0x2F8 */
#define BUF_TEXT_TILES_BLOCK3_SIZE 0x00B0
#define BUF_TEXT_TILES_BLOCK4    0x0700  /* → TEXT_LAYER_TILES+0x380 */
#define BUF_TEXT_TILES_BLOCK4_SIZE 0x00A0
#define BUF_TEXT_TILES_BLOCK5    0x0800  /* → TEXT_LAYER_TILES+0x400 */
#define BUF_TEXT_TILES_BLOCK5_SIZE 0x0010
#define BUF_TEXT_TILES_BLOCK6    0x0900  /* → TEXT_LAYER_TILES+0x480 */
#define BUF_TEXT_TILES_BLOCK6_SIZE 0x0010
#define BUF_TEXT_LAYER2_TILES    0x2000  /* → TEXT_LAYER_TILES+0x1000 */
#define BUF_TEXT_LAYER2_TILES_SIZE 0x1800

/* ===== Battle (battle_ui.c, battle.c) ===== */
/* LOAD_BATTLE_SPRITE scatter-copies each enemy sprite's tile rows to the
 * BATTLE_SPRITE_BUFFER_OFFSETS slots; SETUP_BATTLE_ENEMY_SPRITES then DMAs
 * $2000 or $3000 bytes from BUFFER to VRAM. Dead after that DMA. */
#define BUF_BATTLE_SPRITE_TILES      0x0000
#define BUF_BATTLE_SPRITE_TILES_SIZE 0x3000
/* DESATURATE_PALETTES backs up all 256 colors here; the instant-win fade-in
 * uses them as its fade target. */
#define BUF_BATTLE_PALETTE_SAVE      0x2000
#define BUF_BATTLE_PALETTE_SAVE_SIZE 0x0200

/* ===== PSI Animation (battle_psi.c) ===== */
/* PsiAnimationState.arr_bundle_buf: one 8-frame bundle of the PSI
 * arrangement (.arr.bundled), decompressed on demand while the animation
 * plays. (The asm decompresses the whole arrangement to BUFFER.) */
#define BUF_PSI_ARR_BUNDLE       0x0000
#define BUF_PSI_ARR_BUNDLE_SIZE  0x2000  /* 8 frames × 0x400 */

/* ===== Your Sanctuary (map_loader.c, callroutine.c) ===== */
/* Like the assembly, each of the 8 locations is loaded once into its own slot
 * and its tiles appended to VRAM, so redisplaying a location is a copy.
 * Assembly: tilemaps at BUFFER + idx*$800, palettes at BUFFER+$4000 + idx*$200.
 * Palettes are packed at a $100 stride here (only 256 bytes of each $200 slot
 * are used) so the transient LOADED_MAP_BLOCKS scratch fits in BUFFER_SIZE.
 * Arrangement/GFX are read in place from their decompressed .raw assets.
 * Live from INITIALIZE_YOUR_SANCTUARY_DISPLAY to the end of that montage. */
#define BUF_SANCTUARY_TILEMAPS   0x0000  /* 8 × 0x800 (30×32×2 = 0x780 bytes used) */
#define BUF_SANCTUARY_TILEMAP_STRIDE 0x0800
#define BUF_SANCTUARY_TILEMAPS_SIZE  (8 * BUF_SANCTUARY_TILEMAP_STRIDE)
#define BUF_SANCTUARY_PALETTES   0x4000  /* 8 × 256 bytes */
#define BUF_SANCTUARY_PALETTE_STRIDE 0x0100
#define BUF_SANCTUARY_PALETTES_SIZE  (8 * BUF_SANCTUARY_PALETTE_STRIDE)
#define BUF_SANCTUARY_MAP_BLOCKS 0x4800  /* 1024 × uint16 (transient during load) */
#define BUF_SANCTUARY_MAP_BLOCKS_SIZE 0x0800

/* ===== Lumine Hall wall text (callroutine.c, EVENT_353) ===== */
/* LOAD_CHARACTER_PORTRAIT_SCREEN / ADVANCE_TILEMAP_ANIMATION_FRAME. One byte
 * per cell (the asm stores 16-bit tilemap words at BUFFER+$0000/$1000/$4000,
 * peaking near $7AB0). The glyph staging is dead once the columns are
 * decoded, so the half-step scroll columns overlay it. Peak 0x39D8. */
#define BUF_PORTRAIT_GLYPHS       0x0000  /* VWF tiles, 16-tile blocks of 512 B (transient) */
#define BUF_PORTRAIT_GLYPHS_SIZE  0x1C00  /* tiles 0..223 */
#define BUF_PORTRAIT_SCROLL       0x0000  /* half-step columns, 8 B each, overlays the glyphs */
#define BUF_PORTRAIT_SCROLL_SIZE  BUF_PORTRAIT_GLYPHS_SIZE  /* up to 896 columns */
#define PORTRAIT_MAX_NAME_TILES   9       /* 4 + 9 + 6 + 205 = 224 decoded tiles */
#define BUF_PORTRAIT_COLUMNS      0x1C00  /* whole columns, 8 B each */
#define BUF_PORTRAIT_COLUMNS_SIZE ((29 + 4 * (4 + PORTRAIT_MAX_NAME_TILES + 6 + 205) + 30) * 8) /* 0x1DD8 */

/* ===== Ending / Credits (ending.c) ===== */
/* Credits init decomps to BUF_DECOMP_SCRATCH and uploads at once (BG2 tiles
 * from BUF_CREDITS_LAYER2). Cast tile GFX composes directly in ppu.vram.
 * Only the cast palette and the name tilemap persist during the cast roll. */
#define BUF_CREDITS_LAYER2       0x0700  /* Credits init: BG2 tiles (transient) */
#define BUF_CREDITS_LAYER2_SIZE  0x2000
#define BUF_CREDITS_PALETTE      0x0E00  /* CAST_NAMES_PALETTE, 8 × 32 B (decomp 0x100) */
#define BUF_CREDITS_PALETTE_SIZE 0x0100
#define BUF_CREDITS_TILEMAP      0x0F00  /* Cast name tilemap: 2 rows × 32 words */
#define BUF_CREDITS_TILEMAP_SIZE 0x0080
/* Offsets into the cast VRAM image (ending.c cast_vram[]), not ert.buffer. */
#define BUF_CREDITS_GFX_1        0x0200
#define BUF_CREDITS_GFX_2        0x0600

/* ===== Town map (town_map.c) ===== */
/* LOAD_TOWN_MAP_DATA: the decompressed town map (0x4840 B) is 2 palettes,
 * the tilemap, then the 4bpp tiles. Decompressed after the fade-out and
 * uploaded by load_town_map_finish() (the town map mode runs nothing else
 * in between). */
#define BUF_TOWN_MAP_PALETTE      0x0000
#define BUF_TOWN_MAP_PALETTE_SIZE 0x0040
#define BUF_TOWN_MAP_TILEMAP      0x0040
#define BUF_TOWN_MAP_TILEMAP_SIZE 0x0800
#define BUF_TOWN_MAP_GFX          0x0840
#define BUF_TOWN_MAP_GFX_SIZE     0x4000

/* ===== Save scratch (game_state.c, EntityRuntimeState union) ===== */
/* ert.save_scratch (one SaveBlock, 0x500 B) overlays the buffer here while a
 * save slot is read, written or copied (synchronously, inside one call). */
#define BUF_SAVE_SCRATCH         0x1280
#define BUF_SAVE_SCRATCH_SIZE    sizeof(SaveBlock)

/* =====================================================================
 * Region bounds.
 * ===================================================================== */
BUF_ASSERT_FITS(BUF_DECOMP_SCRATCH);
BUF_ASSERT_FITS(BUF_FADE);
BUF_ASSERT_FITS(BUF_MAP_FADE);
BUF_ASSERT_FITS(BUF_FLASH);
BUF_ASSERT_FITS(BUF_ENTITY_FADE_SPRITES);
BUF_ASSERT_FITS(BUF_ENTITY_FADE_STATES);
BUF_ASSERT_FITS(BUF_TILE_MERGE_SLOTS);
BUF_ASSERT_FITS(BUF_PATHFINDING_MATRIX);
BUF_ASSERT_FITS(BUF_PF_HEAP);
BUF_ASSERT_FITS(BUF_VWF_SAVE);
BUF_ASSERT_FITS(BUF_TEXT_GFX);
BUF_ASSERT_FITS(BUF_TEXT_GFX_FLAVOUR);
BUF_ASSERT_FITS(BUF_BATTLE_SPRITE_TILES);
BUF_ASSERT_FITS(BUF_BATTLE_PALETTE_SAVE);
BUF_ASSERT_FITS(BUF_PSI_ARR_BUNDLE);
BUF_ASSERT_FITS(BUF_SANCTUARY_TILEMAPS);
BUF_ASSERT_FITS(BUF_SANCTUARY_PALETTES);
BUF_ASSERT_FITS(BUF_SANCTUARY_MAP_BLOCKS);
BUF_ASSERT_FITS(BUF_PORTRAIT_GLYPHS);
BUF_ASSERT_FITS(BUF_PORTRAIT_SCROLL);
BUF_ASSERT_FITS(BUF_PORTRAIT_COLUMNS);
BUF_ASSERT_FITS(BUF_CREDITS_LAYER2);
BUF_ASSERT_FITS(BUF_CREDITS_PALETTE);
BUF_ASSERT_FITS(BUF_CREDITS_TILEMAP);
BUF_ASSERT_FITS(BUF_TOWN_MAP_GFX);
BUF_ASSERT_FITS(BUF_SAVE_SCRATCH);

/* Sub-blocks of one owner's staging. */
BUF_ASSERT_WITHIN(BUF_TEXT_GFX_CHECKER, BUF_TEXT_GFX);
BUF_ASSERT_WITHIN(BUF_TEXT_LAYER2_TILES, BUF_TEXT_GFX);
BUF_ASSERT_WITHIN(BUF_TEXT_NAME_TILES, BUF_TEXT_LAYER2_TILES);
BUF_ASSERT_WITHIN(BUF_TEXT_STATUS_TILES, BUF_TEXT_LAYER2_TILES);
BUF_ASSERT_DISJOINT(BUF_TEXT_NAME_TILES, BUF_TEXT_STATUS_TILES);
BUF_ASSERT_WITHIN(BUF_TEXT_TILES_BLOCK1, BUF_TEXT_GFX);
BUF_ASSERT_WITHIN(BUF_TEXT_TILES_BLOCK2, BUF_TEXT_GFX);
BUF_ASSERT_WITHIN(BUF_TEXT_TILES_BLOCK3, BUF_TEXT_GFX);
BUF_ASSERT_WITHIN(BUF_TEXT_TILES_BLOCK4, BUF_TEXT_GFX);
BUF_ASSERT_WITHIN(BUF_TEXT_TILES_BLOCK5, BUF_TEXT_GFX);
BUF_ASSERT_WITHIN(BUF_TEXT_TILES_BLOCK6, BUF_TEXT_GFX);
_Static_assert(BUF_TEXT_GFX_MOVE_DST + BUF_TEXT_GFX_MOVE_SIZE == BUF_REGION_END(BUF_TEXT_GFX),
               "BUF_TEXT_GFX must end where the MEMCPY24 destination ends");
BUF_ASSERT_WITHIN(BUF_TOWN_MAP_PALETTE, BUF_DECOMP_SCRATCH);
BUF_ASSERT_WITHIN(BUF_TOWN_MAP_TILEMAP, BUF_DECOMP_SCRATCH);
BUF_ASSERT_WITHIN(BUF_TOWN_MAP_GFX, BUF_DECOMP_SCRATCH);

/* =====================================================================
 * CONCURRENT REGIONS — live at the same time, so they must not overlap.
 * ===================================================================== */

/* Overworld, across frames: an entity fade batch (sprite copies, metadata,
 * dissolve slot table) persists while entity scripts keep running, and those
 * scripts can pathfind and render windows (VWF save) meanwhile. */
BUF_ASSERT_DISJOINT(BUF_ENTITY_FADE_SPRITES, BUF_ENTITY_FADE_STATES);
BUF_ASSERT_DISJOINT(BUF_ENTITY_FADE_SPRITES, BUF_TILE_MERGE_SLOTS);
BUF_ASSERT_DISJOINT(BUF_ENTITY_FADE_STATES, BUF_TILE_MERGE_SLOTS);
BUF_ASSERT_DISJOINT(BUF_ENTITY_FADE_SPRITES, BUF_PATHFINDING_MATRIX);
BUF_ASSERT_DISJOINT(BUF_ENTITY_FADE_STATES, BUF_PATHFINDING_MATRIX);
BUF_ASSERT_DISJOINT(BUF_TILE_MERGE_SLOTS, BUF_PATHFINDING_MATRIX);
BUF_ASSERT_DISJOINT(BUF_ENTITY_FADE_SPRITES, BUF_PF_HEAP);
BUF_ASSERT_DISJOINT(BUF_ENTITY_FADE_STATES, BUF_PF_HEAP);
BUF_ASSERT_DISJOINT(BUF_TILE_MERGE_SLOTS, BUF_PF_HEAP);
BUF_ASSERT_DISJOINT(BUF_ENTITY_FADE_SPRITES, BUF_VWF_SAVE);
BUF_ASSERT_DISJOINT(BUF_ENTITY_FADE_STATES, BUF_VWF_SAVE);
BUF_ASSERT_DISJOINT(BUF_TILE_MERGE_SLOTS, BUF_VWF_SAVE);
/* One pathfinding call uses the matrix and the heap together. */
BUF_ASSERT_DISJOINT(BUF_PATHFINDING_MATRIX, BUF_PF_HEAP);

/* GAME_MODE_MAP_PALETTE_FADE (CC_1F_E1) keeps its arrays across frames and
 * must leave a paused entity fade batch's metadata intact. */
BUF_ASSERT_DISJOINT(BUF_FLASH, BUF_ENTITY_FADE_STATES);
BUF_ASSERT_DISJOINT(BUF_FLASH, BUF_TILE_MERGE_SLOTS);
/* The comeback map fade runs with the palette fade engine's arrays live. */
BUF_ASSERT_DISJOINT(BUF_MAP_FADE, BUF_FADE);

/* Instant win: the fade-in copies the saved palettes into the fade target,
 * then fades with the palette fade engine. */
BUF_ASSERT_DISJOINT(BUF_BATTLE_PALETTE_SAVE, BUF_FADE);
/* A PSI animation can play after DESATURATE_PALETTES saved the palettes. */
BUF_ASSERT_DISJOINT(BUF_BATTLE_PALETTE_SAVE, BUF_PSI_ARR_BUNDLE);

/* Your Sanctuary montage: all 8 cached locations plus the load scratch. */
BUF_ASSERT_DISJOINT(BUF_SANCTUARY_TILEMAPS, BUF_SANCTUARY_PALETTES);
BUF_ASSERT_DISJOINT(BUF_SANCTUARY_TILEMAPS, BUF_SANCTUARY_MAP_BLOCKS);
BUF_ASSERT_DISJOINT(BUF_SANCTUARY_PALETTES, BUF_SANCTUARY_MAP_BLOCKS);

/* Lumine Hall: the columns are decoded from the glyphs, then the scroll is
 * built from the columns (overlaying the dead glyphs). */
BUF_ASSERT_DISJOINT(BUF_PORTRAIT_GLYPHS, BUF_PORTRAIT_COLUMNS);
BUF_ASSERT_DISJOINT(BUF_PORTRAIT_SCROLL, BUF_PORTRAIT_COLUMNS);

/* Cast roll: the palette and the name tilemap persist together. */
BUF_ASSERT_DISJOINT(BUF_CREDITS_PALETTE, BUF_CREDITS_TILEMAP);

/* =====================================================================
 * DELIBERATE ALIASES — overlapping regions that are never live together.
 *
 *  - BUF_FADE / BUF_ENTITY_FADE_SPRITES: same as the asm (both at BUFFER+0).
 *    Palette fades (door transitions, battle entry) run while no entity fade
 *    batch holds sprite copies.
 *  - BUF_MAP_FADE / BUF_ENTITY_FADE_SPRITES: the comeback sequence disables
 *    all entities first.
 *  - BUF_FLASH / BUF_ENTITY_FADE_SPRITES, BUF_SANCTUARY_TILEMAPS: the map
 *    palette fade mode runs no game tick and is not used by the sanctuary
 *    montage. (The asm's arrays at $7800 alias the fade metadata at $7C00
 *    instead.)
 *  - BUF_VWF_SAVE / BUF_PF_HEAP: each is saved and consumed within one call
 *    (window rendering vs one script's pathfinding call).
 *  - BUF_PSI_ARR_BUNDLE / BUF_FADE, BUF_BATTLE_SPRITE_TILES, BUF_TEXT_GFX:
 *    every PSI animation starts with no bundle resident
 *    (arr_current_bundle = -1), and no palette fade, sprite load or window
 *    GFX reload runs while one plays.
 *  - BUF_BATTLE_PALETTE_SAVE / BUF_BATTLE_SPRITE_TILES, BUF_TEXT_GFX: the
 *    sprite tiles and the text GFX staging are uploaded before
 *    DESATURATE_PALETTES can run (same offsets as the asm).
 *  - BUF_BATTLE_PALETTE_SAVE, BUF_FADE / BUF_ENTITY_FADE_SPRITES: the instant
 *    win runs from the battle entry, with the overworld paused.
 *  - BUF_TEXT_GFX / everything: text_load_window_gfx() zeroes the whole
 *    buffer first. It runs at file select, game start, after a battle and in
 *    UNDRAW_FLYOVER_TEXT. Unlike the asm, whose staging ends below the fade
 *    metadata at $7C00, it also clears BUF_ENTITY_FADE_STATES, so it relies
 *    on no entity fade batch being in flight at those points.
 *  - BUF_DECOMP_SCRATCH / everything: one-shot decomps (town map, credits,
 *    photograph palettes) are consumed before returning. The animation
 *    sequence (LOAD_ANIMATION_SEQUENCE_FRAME, up to 0x4618 B) and the title
 *    gradients (DECOMPRESS_TITLE_DATA) are read back by later callroutines of
 *    the same cutscene script, during which no entity fade or pathfinding
 *    runs (the asm decompresses both to BUFFER+0 as well).
 *  - BUF_SAVE_SCRATCH / BUF_ENTITY_FADE_SPRITES, BUF_MAP_FADE: save I/O is
 *    synchronous and runs from menus and phones with no fade in flight.
 *  - BUF_PORTRAIT_* / BUF_FADE and the entity fade regions: the Lumine Hall
 *    text runs with those idle.
 *  - BUF_CREDITS_PALETTE / BUF_FADE: the cast roll's end fade only writes
 *    the fade target, after the last cast palette upload.
 * ===================================================================== */

/* =====================================================================
 * Typed struct overlays for structured buffer regions.
 *
 * These provide type-safe access to well-defined regions, replacing manual
 * byte arithmetic with array indexing.  Usage:
 *
 *   PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
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

_Static_assert(sizeof(PaletteFadeBuffer) == BUF_FADE_SIZE,
               "PaletteFadeBuffer must be 3584 bytes");
_Static_assert(offsetof(PaletteFadeBuffer, slope_r) == BUF_FADE_SLOPE_R,
               "slope_r offset mismatch");
_Static_assert(offsetof(PaletteFadeBuffer, accum_b) == BUF_FADE_ACCUM_B,
               "accum_b offset mismatch");

/* Zero the six slope/accumulator arrays (everything after target[]) in one
 * pass, through the struct's bytes so the write may span members. */
static inline void palette_fade_clear_slopes(PaletteFadeBuffer *fade) {
    memset((uint8_t *)fade + offsetof(PaletteFadeBuffer, slope_r), 0,
           sizeof(*fade) - offsetof(PaletteFadeBuffer, slope_r));
}

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

_Static_assert(sizeof(MapPaletteFadeBuffer) == BUF_MAP_FADE_SIZE,
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

/* ert.save_scratch (the union in EntityRuntimeState) sits at BUF_SAVE_SCRATCH. */
_Static_assert(offsetof(EntityRuntimeState, save_scratch) -
               offsetof(EntityRuntimeState, buffer) == BUF_SAVE_SCRATCH,
               "ert.save_scratch must sit at BUF_SAVE_SCRATCH");

/* ---- Typed accessors ---- */

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
