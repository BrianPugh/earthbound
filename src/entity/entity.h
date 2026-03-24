/*
 * Entity system data structures and public API.
 *
 * Port of the SNES entity tables from ram.asm.
 * The ROM uses a Structure-of-Arrays layout where each entity property
 * is a separate array indexed by entity slot number.
 *
 * Key ASM sources:
 *   INIT_ENTITY_SYSTEM     — asm/overworld/init_entity_system.asm
 *   INIT_ENTITY_WIPE       — asm/overworld/init_entity.asm
 *   INIT_ENTITY            — asm/overworld/init_entity.asm
 *   ALLOCATE_ENTITY_SLOT   — asm/overworld/allocate_entity_slot.asm
 *   LINK_ENTITY_TO_LIST    — asm/overworld/link_entity_to_list.asm
 *   DEACTIVATE_ENTITY      — deactivation + free
 */
#ifndef ENTITY_ENTITY_H
#define ENTITY_ENTITY_H

#include "core/types.h"
#include "include/constants.h"
#include "game/game_state.h"

/* Script stack size per script slot (bytes) */
#define SCRIPT_STACK_SIZE 16

/* Sentinel value for "no next entity/script" */
#define ENTITY_NONE (-1)

/* Convert entity slot number to array index.
 * Previously (slot * 2) to mirror the ROM's word-indexed layout.
 * Now identity — arrays are packed, indexed directly by slot number. */
#define ENT(slot) (slot)

/*
 * Entity tables — all arrays indexed directly by entity slot number.
 * Size = MAX_ENTITIES entries.
 */
typedef struct {
    /* Linked list management */
    int16_t first_entity;
    int16_t last_entity;
    int8_t next_entity[MAX_ENTITIES];          /* -1 to 29 */

    /* Script linkage: which script slot is assigned to this entity */
    int16_t script_table[MAX_ENTITIES];       /* ENTITY_SCRIPT_TABLE — event script ID or -1 */
    int8_t script_index[MAX_ENTITIES];        /* ENTITY_SCRIPT_INDEX_TABLE — -1 to 69 */

    /* Position (16.16 fixed-point) */
    int16_t abs_x[MAX_ENTITIES];
    int16_t abs_y[MAX_ENTITIES];
    int16_t abs_z[MAX_ENTITIES];
    uint16_t frac_x[MAX_ENTITIES];
    uint16_t frac_y[MAX_ENTITIES];
    uint16_t frac_z[MAX_ENTITIES];

    /* Velocity (16.16 fixed-point delta per frame) */
    int16_t delta_x[MAX_ENTITIES];
    int16_t delta_y[MAX_ENTITIES];
    int16_t delta_z[MAX_ENTITIES];
    uint16_t delta_frac_x[MAX_ENTITIES];
    uint16_t delta_frac_y[MAX_ENTITIES];
    uint16_t delta_frac_z[MAX_ENTITIES];

    /* Screen coordinates (set by screen position callback) */
    int16_t screen_x[MAX_ENTITIES];
    int16_t screen_y[MAX_ENTITIES];

    /* Animation */
    int8_t animation_frame[MAX_ENTITIES];      /* -1 to ~7 */
    uint16_t current_displayed_sprites[MAX_ENTITIES]; /* ENTITY_CURRENT_DISPLAYED_SPRITES */

    /* Drawing */
    int16_t draw_priority[MAX_ENTITIES];
    uint16_t spritemap_ptr_lo[MAX_ENTITIES];  /* ENTITY_SPRITEMAP_POINTER_LOW */
    uint16_t spritemap_ptr_hi[MAX_ENTITIES];  /* bit 15 = hidden */

    /* Callbacks (stored as indices into callback tables) */
    uint8_t move_callback[MAX_ENTITIES];       /* 0–7 */
    uint8_t screen_pos_callback[MAX_ENTITIES]; /* 0–7 */
    uint8_t draw_callback[MAX_ENTITIES];       /* 0–2 */
    uint16_t tick_callback_lo[MAX_ENTITIES];
    uint16_t tick_callback_hi[MAX_ENTITIES];   /* bit 15 = disabled, bit 14 = skip move */

    /* Per-entity script variables (8 variable slots) */
    int16_t var[8][MAX_ENTITIES];

    /* Sprite properties (set by CREATE_ENTITY from sprite_grouping data) */
    int16_t sprite_ids[MAX_ENTITIES];              /* ENTITY_SPRITE_IDS */
    uint16_t vram_address[MAX_ENTITIES];           /* ENTITY_VRAM_ADDRESS */
    uint16_t byte_widths[MAX_ENTITIES];            /* ENTITY_BYTE_WIDTHS */
    uint8_t tile_heights[MAX_ENTITIES];            /* ENTITY_TILE_HEIGHTS — 2–10 */
    uint16_t graphics_ptr_lo[MAX_ENTITIES];        /* ENTITY_GRAPHICS_PTR_LOW */
    uint16_t graphics_ptr_hi[MAX_ENTITIES];        /* ENTITY_GRAPHICS_PTR_HIGH */
    uint8_t graphics_sprite_bank[MAX_ENTITIES];    /* ENTITY_GRAPHICS_SPRITE_BANK — 0–255 */
    uint16_t spritemap_sizes[MAX_ENTITIES];        /* ENTITY_SPRITEMAP_SIZES */
    uint16_t spritemap_begin_idx[MAX_ENTITIES];    /* ENTITY_SPRITEMAP_BEGINNING_INDICES */
    uint8_t sizes[MAX_ENTITIES];                   /* ENTITY_SIZES — 0–16 */
#define ENTITY_SIZE_COUNT 17  /* Number of entity size codes (0–16) */

    /* Hitbox properties */
    uint8_t hitbox_enabled[MAX_ENTITIES];          /* ENTITY_HITBOX_ENABLED — 0 or 1 */
    uint8_t hitbox_ud_widths[MAX_ENTITIES];        /* ENTITY_HITBOX_UP_DOWN_WIDTHS — 0–32 */
    uint8_t hitbox_ud_heights[MAX_ENTITIES];       /* ENTITY_HITBOX_UP_DOWN_HEIGHTS — 0–32 */
    uint8_t hitbox_lr_widths[MAX_ENTITIES];        /* ENTITY_HITBOX_LEFT_RIGHT_WIDTHS — 0–32 */
    uint8_t hitbox_lr_heights[MAX_ENTITIES];       /* ENTITY_HITBOX_LEFT_RIGHT_HEIGHTS — 0–32 */
    uint16_t upper_lower_body_divides[MAX_ENTITIES]; /* ENTITY_UPPER_LOWER_BODY_DIVIDES */

    /* Movement and identity */
    int8_t moving_directions[MAX_ENTITIES];        /* ENTITY_MOVING_DIRECTIONS — 0–7 */
    int8_t directions[MAX_ENTITIES];               /* ENTITY_DIRECTIONS — 0–7 */
    uint16_t movement_speeds[MAX_ENTITIES];        /* ENTITY_MOVEMENT_SPEEDS */
    uint16_t npc_ids[MAX_ENTITIES];                /* ENTITY_NPC_IDS */
    int16_t enemy_ids[MAX_ENTITIES];               /* ENTITY_ENEMY_IDS */
    int16_t enemy_spawn_tiles[MAX_ENTITIES];       /* ENTITY_ENEMY_SPAWN_TILES */
    int8_t collided_objects[MAX_ENTITIES];         /* ENTITY_COLLIDED_OBJECTS — -1 to 29 */
    uint8_t surface_flags[MAX_ENTITIES];           /* ENTITY_SURFACE_FLAGS — bits 0–3 */
    uint8_t obstacle_flags[MAX_ENTITIES];          /* ENTITY_OBSTACLE_FLAGS — bits 4,6,7 */
    int16_t pathfinding_states[MAX_ENTITIES];       /* ENTITY_PATHFINDING_STATES — 0=inactive, 1=reset, -1=active (bit 15 used) */
    uint8_t walking_styles[MAX_ENTITIES];          /* ENTITY_WALKING_STYLES — small enum */
    uint16_t animation_fingerprints[MAX_ENTITIES]; /* ENTITY_ANIMATION_FINGERPRINTS */
    uint8_t overlay_flags[MAX_ENTITIES];           /* ENTITY_OVERLAY_FLAGS — bit 1: nausea, bit 0: mushroom */
    uint8_t use_8dir_sprites[MAX_ENTITIES];        /* C port flag: 0 or 1 */
    int8_t butterfly_orbit_direction[MAX_ENTITIES]; /* ENTITY_BUTTERFLY_ORBIT_DIRECTION — 0 or 1 */

    /* Overlay animation state (4 types x 3 arrays each) */
    uint8_t ripple_overlay_ptrs[MAX_ENTITIES];             /* ENTITY_RIPPLE_OVERLAY_PTRS — 0–211 */
    uint8_t ripple_next_update[MAX_ENTITIES];              /* ENTITY_RIPPLE_NEXT_UPDATE_FRAMES — 0–60 */
    uint16_t ripple_spritemaps[MAX_ENTITIES];              /* ENTITY_RIPPLE_SPRITEMAPS */
    uint8_t big_ripple_overlay_ptrs[MAX_ENTITIES];         /* ENTITY_BIG_RIPPLE_OVERLAY_PTRS — 0–211 */
    uint8_t big_ripple_next_update[MAX_ENTITIES];          /* ENTITY_BIG_RIPPLE_NEXT_UPDATE_FRAMES — 0–60 */
    uint16_t big_ripple_spritemaps[MAX_ENTITIES];          /* ENTITY_BIG_RIPPLE_SPRITEMAPS */
    uint16_t weak_enemy_value[MAX_ENTITIES];              /* ENTITY_WEAK_ENEMY_VALUE */
    uint8_t sweating_overlay_ptrs[MAX_ENTITIES];           /* ENTITY_SWEATING_OVERLAY_PTRS — 0–211 */
    uint8_t sweating_next_update[MAX_ENTITIES];            /* ENTITY_SWEATING_NEXT_UPDATE_FRAMES — 0–60 */
    uint16_t sweating_spritemaps[MAX_ENTITIES];            /* ENTITY_SWEATING_SPRITEMAPS */
    uint8_t mushroomized_overlay_ptrs[MAX_ENTITIES];       /* ENTITY_MUSHROOMIZED_OVERLAY_PTRS — 0–211 */
    uint8_t mushroomized_next_update[MAX_ENTITIES];        /* ENTITY_MUSHROOMIZED_NEXT_UPDATE_FRAMES — 0–60 */
    uint16_t mushroomized_spritemaps[MAX_ENTITIES];        /* ENTITY_MUSHROOMIZED_SPRITEMAPS */

    /* Allocation control */
    uint16_t alloc_min_slot;
    uint16_t alloc_max_slot;
} EntitySystem;

/*
 * Script tables — all arrays indexed directly by script slot number.
 * Size = MAX_SCRIPTS entries.
 */
typedef struct {
    int8_t next_script[MAX_SCRIPTS];              /* -1 to 69 */
    int16_t sleep_frames[MAX_SCRIPTS];
    uint16_t pc[MAX_SCRIPTS];
    uint8_t pc_bank[MAX_SCRIPTS];                 /* 0–63 bank index, 0xFF = none */
    uint8_t stack_offset[MAX_SCRIPTS];            /* 0–SCRIPT_STACK_SIZE */
    int16_t tempvar[MAX_SCRIPTS];
    uint8_t stack[MAX_SCRIPTS][SCRIPT_STACK_SIZE];
} ScriptSystem;

/* Sprite priority queue (4 levels, 32 entries each) */
#define MAX_PRIORITY_SPRITES 32

typedef struct {
    uint16_t spritemaps[MAX_PRIORITY_SPRITES];
    int16_t sprite_x[MAX_PRIORITY_SPRITES];
    int16_t sprite_y[MAX_PRIORITY_SPRITES];
    uint16_t spritemap_banks[MAX_PRIORITY_SPRITES];
    uint16_t offset;  /* next free slot index */
} SpritePriorityQueue;

/* BG scroll table layer count */
#define MAX_BG_LAYERS 4

/* BUFFER — general-purpose work area in SNES WRAM (RAM2 segment).
 *
 * In the original game (compiled with VUCC), this was likely multiple separate
 * C variables that the compiler placed contiguously in WRAM starting at $7F0000.
 * The disassembly labels it as a single `.RES $F800` block (BUFFER, 63488 bytes)
 * followed by TILE_COLLISION_BUFFER (`.RES $800`, 2048 bytes) at $7FF800.
 *
 * The C port currently treats the entire region as one 64 KB buffer, which also
 * serves as the decompression target for small transient decomps.
 *
 * All named offset constants live in entity/buffer_layout.h.
 *
 * === BUFFER REGION MAP ===
 *
 * The buffer is TIME-SHARED — different game phases reuse the same offsets.
 * Within a phase, regions are spatially partitioned to avoid conflicts.
 *
 * PALETTE FADE ENGINE (callroutine_palette.c, title_screen.c, gas_station.c)
 *   Assembly: prepare_palette_fade_slopes.asm, update_map_palette_animation.asm
 *   Active during: screen transitions (overworld, battle, intro)
 *   BUF_FADE_TARGET   0x0000–0x01FF  Target palette (256 × uint16)  512 B
 *   BUF_FADE_SLOPE_R  0x0200–0x03FF  Red channel slopes             512 B
 *   BUF_FADE_SLOPE_G  0x0400–0x05FF  Green channel slopes           512 B
 *   BUF_FADE_SLOPE_B  0x0600–0x07FF  Blue channel slopes            512 B
 *   BUF_FADE_ACCUM_R  0x0800–0x09FF  Red channel accumulators       512 B
 *   BUF_FADE_ACCUM_G  0x0A00–0x0BFF  Green channel accumulators     512 B
 *   BUF_FADE_ACCUM_B  0x0C00–0x0DFF  Blue channel accumulators      512 B
 *   Total: 0x0E00 = 3584 bytes
 *
 * TEXT WINDOW GFX TILE UPLOADS (text.c, battle_ui.c)
 *   Assembly: upload_text_tiles_to_vram.asm
 *   Active during: text init (overworld/battle), consumed immediately
 *   BUF_TEXT_TILES_BLOCK1  0x0000  0x450 B → TEXT_LAYER_TILES+0x000
 *   BUF_TEXT_TILES_BLOCK2  0x04F0  0x060 B → TEXT_LAYER_TILES+0x278
 *   BUF_TEXT_TILES_BLOCK3  0x05F0  0x0B0 B → TEXT_LAYER_TILES+0x2F8
 *   BUF_TEXT_TILES_BLOCK4  0x0700  0x0A0 B → TEXT_LAYER_TILES+0x380
 *   BUF_TEXT_TILES_BLOCK5  0x0800  0x010 B → TEXT_LAYER_TILES+0x420
 *   BUF_TEXT_TILES_BLOCK6  0x0900  0x010 B → TEXT_LAYER_TILES+0x480
 *   BUF_TEXT_LAYER2_TILES  0x2000  0x1800 B → TEXT_LAYER_TILES+0x1000
 *
 * BATTLE PALETTE SAVE (battle_ui.c, battle.c)
 *   Assembly: desaturate_palettes.asm
 *   BUF_BATTLE_PALETTE_SAVE  0x2000  512 B — palettes saved for battle fade
 *
 * ENTITY FADE SPRITE DATA (callbacks.c — double_entity_fade_state)
 *   Active during: overworld entity spawn/despawn wipe effects
 *   0x0000+        Bump-allocated sprite tile copies (grows upward)
 *   Typical: a few hundred bytes (one entity sprite ≈ 32–128 B × 2)
 *   NOT concurrent with palette fade — fades finish before transitions.
 *
 * ENTITY FADE METADATA (callbacks.c — entity_fade_entries)
 *   Assembly: init_entity_fade_states_buffer.asm
 *   Active during: overworld (concurrent with sprite data above)
 *   BUF_ENTITY_FADE_STATES  0x7C00–0x7FFF  20-byte entries     1024 B
 *
 * ENTITY TILE MERGE SLOT TABLE (callbacks.c — cr_animate_entity_tile_merge)
 *   Assembly: clear_spritemap_buffer.asm, animate_entity_tile_merge.asm
 *   Active during: overworld entity wipe animation
 *   BUF_TILE_MERGE_SLOTS  0x7F00–0x7F7F  64-entry slot table    128 B
 *
 * PATHFINDING COLLISION MATRIX + HEAP (pathfinding.c)
 *   Assembly: initialize_pathfinding_for_entities.asm
 *   Active during: overworld NPC pathfinding (on-demand)
 *   BUF_PATHFINDING_MATRIX  0x3000–0x4000   BFS collision grid  max 4 KB (64×64)
 *   BUF_PF_HEAP             0x4000–0x4C00   BFS heap/scratch    3 KB
 *
 * MAP PALETTE FADE (overworld_palette.c)
 *   Assembly: initialize_map_palette_fade.asm, update_map_palette_fade.asm
 *   Active during: overworld map transitions (concurrent with entity fade)
 *   BUF_MAP_FADE_TARGET    0x7800  Target palette (96 colors)    192 B
 *   BUF_MAP_FADE_SLOPE_R   0x7900  Red channel slopes            192 B
 *   BUF_MAP_FADE_SLOPE_G   0x7A00  Green channel slopes          192 B
 *   BUF_MAP_FADE_SLOPE_B   0x7B00  Blue channel slopes           192 B
 *   BUF_MAP_FADE_ACCUM_R   0x7C00  Red channel accumulators      192 B
 *   BUF_MAP_FADE_ACCUM_G   0x7D00  Green channel accumulators    192 B
 *   BUF_MAP_FADE_ACCUM_B   0x7E00  Blue channel accumulators     192 B
 *
 * DECOMPRESSION SCRATCH (various files via decomp() calls)
 *   Active during: all phases, temporary (consumed immediately)
 *   Notable consumers still using ert.buffer:
 *     - PSI arrangements:  bundled format (.arr.bundled), 8 KB per bundle
 *     - PSI GFX:           decomp → buffer[BUF_PSI_GFX], to VRAM
 *     - Ending credits:    decomp → BUF_CREDITS_GFX_1/2, BUF_CREDITS_PALETTE
 *     - Town map GFX:      decomp → buffer[0x0000]
 *     - Battle sprites:    decomp → buffer[BUF_BATTLE_SPRITE_DECOMP]
 *   Optimized to bypass ert.buffer (decompress directly to destination):
 *     - Intro GFX/tilemaps: → ppu.vram
 *     - Game-over GFX:      → ppu.vram
 *     - Map tileset GFX:    → ppu.vram
 *     - Sanctuary arrangements: → arrangement_buffer (static 32 KB)
 *     - Sanctuary tile GFX:    → arrangement_buffer, then tile-by-tile to ppu.vram
 *
 * YOUR SANCTUARY TILEMAP BUILDER (map_loader.c)
 *   Assembly: display_your_sanctuary_location.asm
 *   Active during: sanctuary screen loading (exclusive phase)
 *   BUF_SANCTUARY_TILEMAPS   0x0000  Single tilemap (0x780 B)    ~2 KB
 *   BUF_SANCTUARY_PALETTES   0x0800  Single palette (256 B)      ~0.3 KB
 *   BUF_SANCTUARY_MAP_BLOCKS 0x0900  Tile usage map (2 KB)       transient during load
 *   Intentional divergence: assembly pre-caches all 8 (20 KB).
 *   C port loads on demand into slot 0, trading CPU for RAM.
 *   Arrangement/GFX decomp uses arrangement_buffer (not ert.buffer).
 *
 * SANCTUARY FLASH FADE (map_loader.c)
 *   Active during: sanctuary screen (exclusive phase, shares buffer with above)
 *   BUF_FLASH_TARGET/SLOPE_R/G/B/ACCUM_R/G/B  compact 96-color layout
 *
 * NAMING SCREEN RENDERER (callroutine.c)
 *   Active during: naming screen (exclusive phase)
 *   BUF_NAME_TILES    0x2000  VWF staging area                  ~8 KB
 *   BUF_NAME_TILEMAP  0x4000  Name display tilemap              ~2 KB
 *
 * ENDING / CREDITS (ending.c)
 *   Assembly: ending/ directory
 *   Active during: ending sequence (exclusive phase)
 *   Cast tile GFX (32 KB) composes directly in ppu.vram — no ert.buffer.
 *   Credits init decomps: ~10 KB transient at 0x0000 (immediate VRAM upload)
 *   BUF_CREDITS_PALETTE  0x0E00  Special cast palette data (256 B)
 *   BUF_CREDITS_TILEMAP  0x0F00  Cast name scroll workspace (128 B)
 *
 * TILEMAP ANIMATION (callroutine.c)
 *   Assembly: advance_tilemap_animation_frame.asm
 *   BUF_TILEMAP_ANIM_LOWER  0x1000  Lower animation buffer
 *   BUF_TILEMAP_ANIM_UPPER  0x4000  Upper animation buffer
 *
 * INTRO/TITLE SCREEN (logo_screen.c, title_screen.c, gas_station.c)
 *   GFX and arrangements decompress directly to ppu.vram — no ert.buffer
 *   usage. Palette fade during gas station uses BUF_FADE_* (3.5 KB).
 *   NOTE: Intentional divergence from assembly, which stages through BUFFER
 *   then DMAs to VRAM. The SNES requires DMA from a contiguous RAM source;
 *   the C port's ppu.vram is a regular byte array, so the intermediate copy
 *   is unnecessary. This eliminates the 49 KB intro buffer requirement.
 *
 * === GAME PHASE CONCURRENCY (verified against assembly) ===
 *
 * Overworld:  BUF_FADE_TARGET OR entity_fade_sprite(0x0000) [not both]
 *             + BUF_ENTITY_FADE_STATES + BUF_TILE_MERGE_SLOTS
 *             + BUF_PATHFINDING_MATRIX [on-demand]
 *             + BUF_MAP_FADE_* [during map transitions]
 *             + decomp scratch [transient]
 * Battle:     BUF_FADE_TARGET + decomp scratch [transient]
 *             + BUF_BATTLE_PALETTE_SAVE [during desaturate]
 *             (PSI arrangements use bundled format in PsiAnimationState, not ert.buffer)
 * Naming:     BUF_NAME_TILES + BUF_NAME_TILEMAP [exclusive]
 * Sanctuary:  BUF_SANCTUARY_TILEMAPS + BUF_SANCTUARY_PALETTES + BUF_SANCTUARY_MAP_BLOCKS (~4.3 KB)
 *             + BUF_FLASH_* [exclusive] (decomp uses arrangement_buffer)
 * Intro:      BUF_FADE_* only (GFX decompresses directly to ppu.vram)
 * Ending:     BUF_CREDITS_* [exclusive]
 *
 * === OVERLAP SAFETY ANALYSIS ===
 *
 * Suspect 1: BUF_MAP_FADE_ACCUM_R (0x7C00) vs BUF_ENTITY_FADE_STATES (0x7C00)
 *   SAFE. Two code paths use BUF_MAP_FADE_ACCUM_R:
 *   (a) animate_map_palette_change() — only called from play_comeback_sequence(),
 *       which calls disable_all_entities() first. Entity fade data is stale.
 *   (b) load_map_palette() — C port uses compact BUF_FLASH_* layout at 0x0000,
 *       NOT the 0x7C00 region. Avoids the conflict entirely.
 *   Both assembly and C port rely on temporal separation: fade loops call only
 *   vblank wait, never a full game tick, so entity scripts don't execute.
 *
 * Suspect 2: BUF_FADE_TARGET (0x0000) vs PSI arrangements
 *   NO LONGER APPLICABLE. PSI arrangements now use bundled format decompressed
 *   into PsiAnimationState.arr_bundle_buf, not ert.buffer.
 *
 * Suspect 3: Entity fade sprite alloc (0x0000+) vs BUF_PATHFINDING_MATRIX (0x3000)
 *   THEORETICAL RISK. The bump allocator has no bounds check (matching assembly).
 *   Worst case: 3 max-size entities (5120 B each) = 15360 B, exceeding 0x3000.
 *   Async spawn fades (GENERATE_ACTIVE_SPRITE) don't wait for completion, so
 *   pathfinding could theoretically run concurrently. In practice unlikely
 *   during normal gameplay but possible with heavily scripted spawn events.
 *   The original SNES game has the same vulnerability.
 *
 * === DECOMPRESSED ASSET SIZES (measured via ebtools) ===
 *
 * Of 477 compressed (.lzhal) assets, 462 decompress to ≤32 KB, 473 to ≤48 KB.
 * Only 3 assets exceed 32 KB when decompressed into ert.buffer:
 *
 *   E1CFAF.gfx.lzhal           64 KB  Game-over GFX (two 32 KB variants)
 *   psianims/arrangements/20   64 KB  Starstorm gamma (54 frames used of 64)
 *   psianims/arrangements/21   64 KB  Starstorm omega (all 64 frames used)
 *
 * Intro assets (gas_station 49 KB, title 45 KB, logo 32 KB) decompress
 * directly to ppu.vram and no longer use ert.buffer.
 *
 * The game-over asset is the largest single-decomp bottleneck — it packs
 * two sprites (normal + Paula-leader) into one blob. Splitting into two
 * 32 KB assets would halve the requirement.
 *
 * PSI arrangements 20/21 (Starstorm gamma/omega) decompress to exactly
 * 64 KB = 64 frames × 0x400 bytes. On SNES, this overflows BUFFER ($F800)
 * into TILE_COLLISION_BUFFER ($800) — benign since collision data is unused
 * during battle. The C port uses BUFFER_SIZE (0x10000) so no overflow occurs.
 * Arrangement 0 (48 KB) and 19 (48 KB) are the next largest.
 *
 * === EMBEDDED PORT OPTIMIZATION ===
 *
 * For RAM-constrained platforms, this buffer can be split into:
 *   1. Palette fade arrays (3.5 KB, BUF_FADE_*) — union with entity fade sprite
 *   2. Entity fade metadata (1 KB, BUF_ENTITY_FADE_STATES) — always resident
 *   3. Tile merge slot table (128 B, BUF_TILE_MERGE_SLOTS) — always resident
 *   4. Decomp/phase scratch (union) — sized by largest asset needed at runtime
 *
 * Peak concurrent usage by phase (after all optimizations):
 *   Overworld:   ~17 KB (palette fade + entity fade + pathfinding at 0x3000
 *                + pf_heap at 0x4000, ends at 0x4C00)
 *   Battle:      ~4 KB (palette fade + scatter-copy to ~0x3A00 in ert.buffer,
 *                decomp uses arrangement_buffer; PSI uses bundled format)
 *   Sanctuary:   ~2.3 KB (single-slot on-demand; decomp uses arrangement_buffer)
 *   Naming:      ~10 KB (relocated from 0x4000 to 0x2000)
 *   Ending:      ~10 KB transient (cast tile GFX direct to ppu.vram)
 *   Intro:       3.5 KB (palette fade only — GFX direct to ppu.vram)
 *   Game-over:   0 KB (GFX direct to ppu.vram)
 *   Map tileset: 0 KB (GFX direct to ppu.vram)
 *   PSI GFX:     0 KB (direct to ppu.vram)
 *
 * Highest concurrent offset during overworld: ~0x4C00 (pathfinding + heap).
 * Most exclusive phases fit within ~15 KB, but text_load_window_gfx()
 * peaks at 0x4A00 (~19 KB) and runs during overworld (cannot use
 * decomp_staging because it aliases arrangement_buffer which must persist).
 * BUFFER_SIZE is therefore 0x5000 (20 KB). No features are degraded.
 */
/* 20 KB. The bottleneck is text_load_window_gfx() in text.c, which uses
 * ert.buffer for in-place tile rearrangement peaking at offset 0x4A00.
 * It cannot use decomp_staging (arrangement_buffer) because arrangement
 * data must survive across text reloads during overworld gameplay.
 * See the comment in text_load_window_gfx() for optimization notes. */
#define BUFFER_SIZE 0x5000

/* DELIVERY_PATHS — persistent path waypoint storage (C port substitute for
 * PATHFINDING_BUFFER in WRAM). Sized to match assembly ($C00 = 3072 bytes). */
#define DELIVERY_PATHS_SIZE 0xC00

/* Palette upload mode constants (from include/enums.asm PALETTE_UPLOAD) */
#define PALETTE_UPLOAD_NONE     0   /* NONE = 0 * 8 */
#define PALETTE_UPLOAD_BG_ONLY  8   /* BG_ONLY = 1 * 8 */
#define PALETTE_UPLOAD_OBJ_ONLY 16  /* OBJ_ONLY = 2 * 8 */
#define PALETTE_UPLOAD_FULL     24  /* FULL = 3 * 8 */

/* ---- EntityRuntimeState: all entity module loose globals ---- */
typedef struct {
    /* Runtime context (mirrors ROM's zero-page / BSS work variables) */
    int16_t current_entity_offset;   /* $88 — entity index being processed (same as slot now) */
    int16_t current_entity_slot;     /* CURRENT_ENTITY_SLOT — same as offset after packed refactor */
    int16_t current_script_offset;   /* $8A — script index being processed (same as slot now) */
    int16_t current_script_slot;     /* CURRENT_SCRIPT_SLOT — same as offset after packed refactor */
    int16_t next_active_entity;      /* NEXT_ACTIVE_ENTITY */
    int16_t actionscript_current_script; /* temp for script chain walking */
    uint16_t actionscript_state;     /* ACTIONSCRIPT_STATE: 0=running, 1=done, 2=paused */
    uint16_t disable_actionscript;   /* DISABLE_ACTIONSCRIPT */
    int16_t last_allocated_script;   /* LAST_ALLOCATED_SCRIPT */

    /* BG scroll tables — indexed by BG layer (0-3), word-indexed.
     * Set by action script opcodes 0x31-0x38, zeroed by 0x3A and init. */
    int16_t entity_bg_h_offset_lo[MAX_BG_LAYERS];
    int16_t entity_bg_v_offset_lo[MAX_BG_LAYERS];
    int16_t entity_bg_h_offset_hi[MAX_BG_LAYERS];
    int16_t entity_bg_v_offset_hi[MAX_BG_LAYERS];
    int16_t entity_bg_h_velocity_lo[MAX_BG_LAYERS];
    int16_t entity_bg_v_velocity_lo[MAX_BG_LAYERS];
    int16_t entity_bg_h_velocity_hi[MAX_BG_LAYERS];
    int16_t entity_bg_v_velocity_hi[MAX_BG_LAYERS];

    /* ACTIONSCRIPT_BACKUP_X/Y — saved entity position for SAVE/RESTORE */
    int16_t actionscript_backup_x;
    int16_t actionscript_backup_y;

    /* ENTITY_CALLBACK_FLAGS_BACKUP — saved tick_callback_hi for DISABLE/RESTORE */
    uint16_t entity_callback_flags_backup[MAX_ENTITIES];

    /* Pathfinding arrays */
    uint16_t entity_path_points[MAX_ENTITIES];
    int16_t  entity_path_point_counts[MAX_ENTITIES];
    int16_t pathfinding_target_centre_x;
    int16_t pathfinding_target_centre_y;
    int16_t pathfinding_target_width;
    int16_t pathfinding_target_height;
    uint8_t delivery_paths[DELIVERY_PATHS_SIZE];
    int16_t pathfinding_enemy_ids[4];
    int16_t pathfinding_enemy_counts[4];
    int16_t enemy_pathfinding_target_entity;

    /* Spritemap rendering state */
    uint8_t spritemap_bank;
    uint16_t current_sprite_drawing_priority;

    /* OAM writing state */
    uint16_t oam_write_index;

    /* INIT_ENTITY_WIPE parameter passing */
    int16_t new_entity_pos_z;
    int16_t new_entity_var[8];
    int16_t new_entity_priority;

    /* Entity fade state (counters only; pointer is derived from buffer) */
    uint16_t entity_fade_states_buffer;  /* Running allocation counter */
    uint16_t entity_fade_states_length;  /* Number of 20-byte fade entries */

    /* Title screen state */
    uint16_t title_screen_quick_mode;

    /* Naming screen state */
    uint16_t wait_for_naming_screen_actionscript;

    /* Palette state */
    uint8_t palette_upload_mode;
    uint16_t palettes[256];

    /* General-purpose work buffer (BUFFER at $7F0000 in SNES WRAM RAM2 segment).
     * The save_scratch overlay shares buffer memory at an offset past the
     * always-active palette fade / entity fade / tile merge regions.
     * Save operations are synchronous (no wait_for_vblank) so the overlap
     * with inactive regions is safe. */
    union {
        uint8_t buffer[BUFFER_SIZE];
        struct {
            uint8_t _save_pad[0x1280];  /* palette fade + entity fade + tile merge */
            SaveBlock save_scratch;
        };
    };
} EntityRuntimeState;

/* Global entity system state (large SoA structs — kept separate) */
extern EntitySystem entities;
extern ScriptSystem scripts;
extern SpritePriorityQueue sprite_priority[4];

/* Entity runtime state (all loose globals consolidated) */
extern EntityRuntimeState ert;

/* WRITE_SPRITEMAP_TO_OAM (asm/overworld/entity/write_spritemap_to_oam.asm)
 * Writes a 5-byte-per-entry spritemap to OAM, starting at oam_write_index.
 * base_x, base_y: screen-space origin; entries have signed x/y offsets. */
void write_spritemap_to_oam(const uint8_t *spritemap, int16_t base_x,
                             int16_t base_y);

/* --- Palette fade engine (callroutine.c) --- */

/* Copy palettes[] -> buffer[0..511] (fade target setup) */
void copy_fade_buffer_to_palettes(void);

/* Compute fade slopes from palettes[] toward buffer[] target over N frames.
   mask: bitmask of which 16-color groups to fade (bit 0 = group 0, etc.) */
void prepare_palette_fade_slopes(int16_t frames, uint16_t mask);

/* Advance one frame of palette fade — adds slopes, clamps, writes palettes[] */
void update_map_palette_animation(void);

/* Fade all 256 palette entries by a brightness level and store into buffer[0..511].
 * Port of LOAD_PALETTE_TO_FADE_BUFFER (C4954C).
 * fade_style: 0=black, 50=identity, >50=white. */
void load_palette_to_fade_buffer(uint8_t fade_style);

/* Copy buffer[0..511] to palettes[0..255] and trigger palette upload.
 * Port of FINALIZE_PALETTE_FADE (C49740). */
void finalize_palette_fade(void);

/* --- Public API --- */

/* Initialize the entire entity system (INIT_ENTITY_SYSTEM, C0927C) */
void entity_system_init(void);

/* DISABLE_OTHER_ENTITY_CALLBACKS — Port of C09F43.
 * Backs up tick_callback_hi, then disables all entities except current. */
void disable_other_entity_callbacks(void);

/* Rebuild the free entity list (REBUILD_ENTITY_FREE_LIST, C09CD7).
 * Re-chains free slots in ascending order after direct slot assignment. */
void rebuild_entity_free_list(void);

/* Allocate and initialize an entity with script (INIT_ENTITY_WIPE) */
/* Returns entity offset on success, or -1 on failure */
int16_t entity_init_wipe(uint16_t script_id);

/* Allocate and initialize an entity at position (INIT_ENTITY) */
int16_t entity_init(uint16_t script_id, int16_t x, int16_t y);

/* Create a fully-initialized entity with sprite (CREATE_ENTITY, C09E71 path) */
int16_t create_entity(uint16_t sprite, uint16_t script, int16_t index,
                      int16_t x, int16_t y);

/* Deactivate an entity and free its scripts */
void deactivate_entity(int16_t entity_offset);

/* Run one frame of the action script system (RUN_ACTIONSCRIPT_FRAME) */
void run_actionscript_frame(void);

/* Sync palettes[] to ppu.cgram — call during vblank */
void sync_palettes_to_cgram(void);

/* Display animated naming sprite (DISPLAY_ANIMATED_NAMING_SPRITE) */
void display_animated_naming_sprite(uint16_t start_entry);

/* Find an entity by sprite ID (FIND_ENTITY_BY_SPRITE_ID, C46028) */
/* Returns slot number (0 to MAX_ENTITIES-1) on success, or -1 if not found. */
int16_t find_entity_by_sprite_id(uint16_t sprite_id);

/* Find an entity by NPC ID (FIND_ENTITY_BY_NPC_ID_LINKED) */
/* Returns entity slot on success, or -1 if not found. */
int16_t find_entity_by_npc_id(uint16_t npc_id);

/* Reassign a new event script to an existing entity (INIT_ENTITY_UNKNOWN1/2) */
/* Frees old scripts, allocates new script slot, inits PC from ESP table. */
void reassign_entity_script(int16_t entity_slot, uint16_t script_id);

/* Find entity slot for a character ID (FIND_ENTITY_FOR_CHARACTER, C4608C).
 * char_id 0xFF = leader entity. Otherwise searches party_order[]. */
int16_t find_entity_for_character(uint8_t char_id);

/* REMOVE_ASSOCIATED_ENTITIES: Port of asm/misc/remove_associated_entities.asm.
 * Removes all entities whose draw_priority matches (parent_slot | 0xC000).
 * Used to delete floating sprites (thought bubbles, exclamation marks, etc.)
 * that are associated with a parent entity.
 * parent_slot: entity slot index (not offset). -1 is a no-op. */
void remove_associated_entities(int16_t parent_slot);

/* Compute fine 16-bit direction from one position to another.
 * Port of CALCULATE_DIRECTION_FROM_POSITIONS (C41EFF).
 * Returns fine direction: 0x0000=UP, 0x2000=UP_RIGHT, ..., 0xE000=UP_LEFT. */
uint16_t calculate_direction_fine(int16_t from_x, int16_t from_y,
                                  int16_t to_x, int16_t to_y);

/* Compute 8-way direction from one position to another.
 * Port of CALCULATE_DIRECTION_FROM_POSITIONS (C41EFF) + quantization.
 * Returns SNES direction: 0=UP, 1=UP_RIGHT, ..., 7=UP_LEFT. */
int16_t calculate_direction_8(int16_t from_x, int16_t from_y,
                              int16_t to_x, int16_t to_y);

/* Find entity by type. Port of FIND_ENTITY_BY_TYPE (C4621C).
 * type: 0=character, 1=NPC, 2=sprite.
 * Returns entity slot or -1 if not found. */
int16_t find_entity_by_type(int16_t type, int16_t entity_id);

/* Get direction between two entities. Port of GET_DIRECTION_BETWEEN_ENTITIES (C46257).
 * source_type/target_type: 0=character, 1=NPC, 2=sprite.
 * Returns 0-7 direction from source to target. */
int16_t get_direction_between_entities(int16_t source_type, int16_t source_id,
                                       int16_t target_type, int16_t target_id);

/* SETUP_COLOR_MATH_WINDOW: Port of asm/system/palette/setup_color_math_window.asm.
 * Sets CGADSUB, CGWSEL, window registers, TMW, and fixed color data.
 *   cgadsub_val: value for CGADSUB ($33 = add, $B3 = subtract)
 *   intensity: 5-bit color intensity for COLDATA (applied to R, G, B) */
void setup_color_math_window(uint8_t cgadsub_val, uint8_t intensity);

/* SETUP_ENTITY_COLOR_MATH: Port of asm/overworld/entity/setup_entity_color_math.asm.
 * Reads current entity's var0 to determine color math mode and intensity.
 * var0 >= 0 -> add mode, var0 < 0 -> subtract mode.
 * Chains to setup_color_math_window(). */
void setup_entity_color_math(void);

/* --- Overlay system (water ripples, sweat, mushroom) --- */

/* LOAD_OVERLAY_SPRITES (C4B26B): Load overlay sprite graphics to VRAM
 * and initialize entity overlay animation state. Called during map load. */
void load_overlay_sprites(void);

/* DISPATCH_SPRITE_DRAW_BY_PRIORITY (C08C58) — queue a spritemap for OAM rendering.
 * bank: 0=title, 1=overworld, 2=overlay, 3=townmap */
#define SMAP_BANK_TOWNMAP_ID 3
void queue_sprite_draw(uint16_t smap_id, int16_t x, int16_t y,
                       uint16_t priority, uint16_t bank);

/* RENDER_ALL_PRIORITY_SPRITES (C08B8E) — flush all priority queues to OAM. */
void render_all_priority_sprites(void);

/* --- Entity fade state system (wipe/dissolve effects) ---
 *
 * Port of INIT_ENTITY_FADE_STATE (C4C91A) and helpers.
 * When an entity is spawned or despawned with a wipe effect, a special
 * "fade entity" (EVENT_ENTITY_WIPE, script 859) is created to animate the transition.
 * Each active fade is tracked as a 20-byte entry in a buffer at BUFFER+$7C00.
 */

/* Initialize entity fade state for a wipe/dissolve effect.
 * entity_slot: entity slot number (0-29)
 * fade_param: 0/1/6=no-op, 2=wipe-up, 3=wipe-down, 4=wipe-left, 5=wipe-right,
 *             7=reveal-up, 8=reveal-down, 9=reveal-left, 10=reveal-right */
void init_entity_fade_state(uint16_t entity_slot, uint16_t fade_param);

/* Entity fade animation callroutines — called from EVENT_ENTITY_WIPE (859) script.
 * Ports of C4CB4F, C4CB8F, C4CBE3, C4CC2F, C4CD44, C4CED8. */
void cr_clear_fade_entity_flags(void);
void cr_update_fade_entity_sprites(void);
void cr_hide_fade_entity_frames(void);
int16_t cr_animate_entity_tile_copy(void);
int16_t cr_animate_entity_tile_blend(void);
void cr_animate_entity_tile_merge(void);

/* --- Floating sprites (thought bubbles, exclamation marks, etc.) --- */

/* Load the floating sprite table from extracted binary data.
 * Call once during initialization. Returns true on success. */
bool floating_sprite_table_load(void);

/* Free the floating sprite table data. */
void floating_sprite_table_free(void);

/* SPAWN_FLOATING_SPRITE: Create a floating sprite attached to a parent entity.
 *   entity_slot: parent entity slot number (-1 = no-op)
 *   table_index: index into FLOATING_SPRITE_TABLE (0-11) */
void spawn_floating_sprite(int16_t entity_slot, uint16_t table_index);

/* Convenience wrappers matching assembly callers */
void spawn_floating_sprite_for_npc(uint16_t npc_id, uint16_t table_index);
void spawn_floating_sprite_for_character(uint8_t char_id, uint16_t table_index);
void spawn_floating_sprite_for_sprite(uint16_t sprite_id, uint16_t table_index);

#endif /* ENTITY_ENTITY_H */
