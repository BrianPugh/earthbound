/*
 * Map sector loading system.
 *
 * Port of:
 *   LOAD_MAP_AT_POSITION  — asm/overworld/load_map_at_position.asm
 *   LOAD_MAP_AT_SECTOR    — asm/overworld/load_map_at_sector.asm
 *   GET_MAP_BLOCK_CACHED  — asm/overworld/map/get_map_block_cached.asm (block ID lookup)
 *   LOAD_MAP_ROW_TO_VRAM  — asm/overworld/map/load_map_row_to_vram.asm (tilemap fill)
 *
 * The overworld map pipeline:
 *   1. Chunk files (maps/tiles/chunk_XX.bin) contain the spatial layout
 *   2. Chunks 9/10 are meta-chunks with 2-bit high block ID fields
 *   3. Chunks 1-8 hold the low 8 bits of block IDs
 *   4. block_id = (high_2bits << 8) | low_byte  (10-bit, 0-1023)
 *   5. Arrangement data is a lookup table: block_id → 4×4 tile entries
 *   6. BG1 gets tile as-is; BG2 gets tile|$2000 if tile_index < 384, else 0
 */

#include "game/map_loader.h"
#include "game/door.h"
#include "game/overworld.h"
#include "game/game_state.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "entity/sprite.h"
#include "snes/ppu.h"
#include "core/decomp.h"
#include "data/assets.h"
#include "include/binary.h"
#include "include/constants.h"
#include "game/audio.h"
#include "game/fade.h"
#include "game/text.h"
#include <stdio.h>

extern uint16_t photograph_map_loading_mode;
#include <string.h>

/* Forward declarations */
#include "game_main.h"
static uint16_t get_tileset_id(uint16_t tileset_combo);

/* --- Data tables loaded from assets --- */

/* GLOBAL_MAP_TILESETPALETTE_DATA: 2560 bytes (32 columns × 80 rows).
 * Each byte: bits 7-3 = tileset_combo, bits 2-0 = palette_index.
 * Index: sector_y * 32 + sector_x. */
static const uint8_t *tilesetpalette_data = NULL;
static size_t tilesetpalette_size = 0;

/* TILESET_TABLE: 32 entries × 2 bytes (uint16_t).
 * Maps tileset_combo → tileset_id (0-19). */
static const uint8_t *tileset_table_data = NULL;
static size_t tileset_table_size = 0;

/* --- NPC placement data tables loaded from assets --- */

/* SPRITE_PLACEMENT_PTR_TABLE: 32×40 row-major grid of 16-bit within-bank pointers.
 * 32 columns (X, horizontal, 0-31) × 40 rows (Y, vertical, 0-39).
 * Index: (sector_y * 32 + sector_x) * 2.  2560 bytes total. */
static const uint8_t *sprite_placement_ptr_table = NULL;
static size_t sprite_placement_ptr_table_size = 0;

/* SPRITE_PLACEMENT_TABLE: per-sector NPC placement records.
 * Each sector's data: uint16_t count, then count × {uint16_t id, uint8_t x, uint8_t y}. */
static const uint8_t *sprite_placement_table = NULL;
static size_t sprite_placement_table_size = 0;

/* Within-bank base address of SPRITE_PLACEMENT_TABLE (for pointer conversion) */
#define SPRITE_PLACEMENT_TABLE_LOWORD 0x6BE7

/* NPC_CONFIG_TABLE: 1584 entries × 17 bytes.
 * npc_config: type(1), sprite(2), direction(1), event_script(2),
 *             event_flag(2), appearance_style(1), text_pointer(4), union(4). */
static const NpcConfig *npc_config_table = NULL;
static size_t npc_config_table_count = 0;

/* --- Event-triggered tile change tables loaded from assets ---
 * EVENT_CONTROL_PTR_TABLE: 20 entries × 2 bytes (within-bank offsets).
 * Maps tileset_id → tile event data offset.
 * TILE_EVENT_CONTROL_TABLE: variable-length list of map_tile_event entries. */
static const uint8_t *event_control_ptr_table_data = NULL;
static size_t event_control_ptr_table_size = 0;
static const uint8_t *tile_event_data = NULL;
static size_t tile_event_data_size = 0;
#define NUM_TILESETS 20

/* NpcConfig struct defined in map_loader.h (17 bytes) */

/* --- Collision tile data tables loaded from assets --- */

/* MAP_DATA_TILE_COLLISION_ARRANGEMENT_TABLE: 2293 entries × 16 bytes.
 * Each entry is a 4×4 grid of collision flags for one arrangement type.
 * Indexed by byte offset (collision_ptr + sub_row * 4 + sub_col). */
static const uint8_t *collision_arrangement_table = NULL;
static size_t collision_arrangement_table_size = 0;

/* Per-tileset collision pointer arrays (all 20 contiguous in one blob).
 * Each tileset has an array of 16-bit values: TILE_COLLISION_BUFFER[block_id]
 * gives a byte offset into collision_arrangement_table. */
static const uint8_t *collision_pointers_blob = NULL;
static size_t collision_pointers_blob_size = 0;

/* Offset within collision_pointers_blob for each tileset (0-19).
 * From linker map: POINTERS_N address - POINTERS_0 base ($D88F50). */
static const uint32_t tileset_collision_offsets[20] = {
    0x000000, /* tileset  0: $D88F50 */
    0x000680, /* tileset  1: $D895D0 */
    0x000D1A, /* tileset  2: $D89C6A */
    0x001390, /* tileset  3: $D8A2E0 */
    0x0017A8, /* tileset  4: $D8A6F8 */
    0x001EF6, /* tileset  5: $D8AE46 */
    0x002134, /* tileset  6: $D8B084 */
    0x00280A, /* tileset  7: $D8B75A */
    0x002DE4, /* tileset  8: $D8BD34 */
    0x0032CC, /* tileset  9: $D8C21C */
    0x003A16, /* tileset 10: $D8C966 */
    0x0040E4, /* tileset 11: $D8D034 */
    0x004676, /* tileset 12: $D8D5C6 */
    0x004A12, /* tileset 13: $D8D962 */
    0x0050F6, /* tileset 14: $D8E046 */
    0x00528C, /* tileset 15: $D8E1DC */
    0x0053AA, /* tileset 16: $D8E2FA */
    0x0056B6, /* tileset 17: $D8E606 */
    0x005964, /* tileset 18: $D8E8B4 */
    0x005CDE, /* tileset 19: $D8EC2E */
};

/* ml.tile_collision_buffer and ml.tile_collision_loaded now in MapLoaderState ml. */

/* MAP_DATA_PER_SECTOR_ATTRIBUTES_TABLE: 2560 entries × 2 bytes.
 * Sector grid: 32 columns (256px wide) × 80 rows (128px tall).
 * Low byte bits 0-2 = MAP_SECTOR_MISC_CONFIG, bits 3-5 = town_map, bits 6-7 = flags.
 * High byte = rideable item (ITEM enum). */
static const uint8_t *sector_attributes_data = NULL;
static size_t sector_attributes_size = 0;

/* ml.current_sector_attributes now in MapLoaderState ml. */

/* Entity collision geometry tables (ENTITY_SIZE_COUNT entries × 2 bytes each).
 * Index = entity size code (from ENTITY_SIZES). */
static const uint8_t *entity_collision_left_x = NULL;
static const uint8_t *entity_collision_top_y = NULL;
static const uint8_t *entity_collision_width = NULL;
static const uint8_t *entity_collision_tile_count = NULL;
static const uint8_t *entity_collision_height_offset = NULL;

/* --- Map music data tables loaded from assets --- */

/* MAP_DATA_PER_SECTOR_MUSIC: 2560 bytes (80 rows × 32 columns).
 * Each byte is a music zone ID that indexes OVERWORLD_EVENT_MUSIC_PTR_TABLE.
 * Index: (y_coord / 128) * 32 + (x_coord >> 8). */
static const uint8_t *per_sector_music_data = NULL;
static size_t per_sector_music_size = 0;

/* OVERWORLD_EVENT_MUSIC_PTR_TABLE: 165 entries × 2 bytes.
 * Within-bank pointers to OVERWORLD_EVENT_MUSIC_TABLE entries. */
static const uint8_t *event_music_ptr_table = NULL;
static size_t event_music_ptr_table_size = 0;

/* OVERWORLD_EVENT_MUSIC_TABLE: variable-length records.
 * Each record: {uint16_t flag, uint16_t music_track}.
 * Lists are terminated by flag==0 (NULL). */
static const uint8_t *event_music_table = NULL;
static size_t event_music_table_size = 0;

/* Within-bank base address of OVERWORLD_EVENT_MUSIC_TABLE (for pointer conversion) */
#define EVENT_MUSIC_TABLE_LOWORD 0x5A39

/* Consolidated map loader runtime state. */
MapLoaderState ml = {
    .loaded_tileset_combo = -1,
    .loaded_palette_index = -1,
};

/* ml.loaded_map_music_entry_offset now in MapLoaderState ml. */

void invalidate_loaded_tileset_combo(void) {
    ml.loaded_tileset_combo = -1;
}

/* --- Map tile arrangement ert.buffer ---
 * Decompressed from maps/arrangements/{id}.arr.lzhal.
 * This is a BLOCK LOOKUP TABLE, not a spatial map.
 * Each block has 16 entries (4 columns × 4 rows), each 2 bytes.
 * Index: arrangement_buffer[(block_id * 16 + sub_row * 4 + sub_col) * 2] */
SharedScratch shared_scratch;
static size_t arrangement_size = 0;
static bool arrangement_loaded = false;

/* --- Map chunk data (spatial layout) ---
 * 10 chunk files: chunks 1-8 hold low bytes of block IDs,
 * chunks 9-10 hold 2-bit high fields packed 4 per byte.
 * Port of GET_MAP_BLOCK_CACHED (asm/overworld/map/get_map_block_cached.asm). */
#define NUM_CHUNKS 10
static const uint8_t *chunk_data[NUM_CHUNKS]; /* chunks 1-10 (0-indexed) */
static size_t chunk_size[NUM_CHUNKS];
static bool chunks_loaded = false;

/* Chunk files are not a simple numeric family (they have "chunk_" prefix
 * and some are locale-specific). Use a static lookup table of AssetIds. */
static const AssetId chunk_asset_ids[NUM_CHUNKS] = {
    ASSET_MAPS_TILES_CHUNK_01_BIN,  /* chunk_01 */
    ASSET_MAPS_TILES_CHUNK_02_BIN,  /* chunk_02 */
    ASSET_MAPS_TILES_CHUNK_03_BIN,  /* chunk_03 (locale-specific via alias) */
    ASSET_MAPS_TILES_CHUNK_04_BIN,  /* chunk_04 */
    ASSET_MAPS_TILES_CHUNK_05_BIN,  /* chunk_05 */
    ASSET_MAPS_TILES_CHUNK_06_BIN,  /* chunk_06 */
    ASSET_MAPS_TILES_CHUNK_07_BIN,  /* chunk_07 */
    ASSET_MAPS_TILES_CHUNK_08_BIN,  /* chunk_08 (locale-specific via alias) */
    ASSET_MAPS_TILES_CHUNK_09_BIN,  /* chunk_09 */
    ASSET_MAPS_TILES_CHUNK_10_BIN,  /* chunk_10 */
};

static bool load_chunks(void) {
    if (chunks_loaded) return true;

    for (int i = 0; i < NUM_CHUNKS; i++) {
        chunk_data[i] = ASSET_DATA(chunk_asset_ids[i]);
        chunk_size[i] = ASSET_SIZE(chunk_asset_ids[i]);
        if (!chunk_data[i]) {
            /* Non-fatal — some chunks may not exist for all scenes */
            chunk_size[i] = 0;
        }
    }

    chunks_loaded = true;
    return true;
}

/* Look up a block ID at block coordinates (bx, by).
 * bx = tile_x / 4 (0-255), by = tile_y / 4 (0-319).
 *
 * Port of GET_MAP_BLOCK_CACHED (asm/overworld/map/get_map_block_cached.asm):
 *   1. Y = (by >> 3) << 8 | bx — 2D index into chunk data
 *   2. Meta chunk = (by & 4) ? chunk_10 : chunk_9
 *   3. High 2 bits = (meta_byte >> ((by & 3) * 2)) & 3
 *   4. Data chunk = chunks_table[by & 7] (chunks 1-8)
 *   5. Low byte = data_chunk[Y]
 *   6. block_id = (high << 8) | low */
static uint16_t get_block_id(uint16_t bx, uint16_t by) {
    uint16_t chunk_y = by >> 3;
    uint16_t Y = (chunk_y << 8) | bx;

    /* Meta chunk: chunk 9 (index 8) or chunk 10 (index 9) */
    int meta_idx = (by & 4) ? 9 : 8;
    if (!chunk_data[meta_idx] || Y >= chunk_size[meta_idx])
        return 0;
    uint8_t meta_byte = chunk_data[meta_idx][Y];

    /* Extract 2-bit high field based on by & 3 */
    uint8_t shift = (by & 3) * 2;
    uint8_t high = (meta_byte >> shift) & 3;

    /* Data chunk: chunks 1-8 selected by by & 7 (0-indexed: indices 0-7) */
    int data_idx = by & 7;
    if (!chunk_data[data_idx] || Y >= chunk_size[data_idx])
        return 0;
    uint8_t low = chunk_data[data_idx][Y];

    return ((uint16_t)high << 8) | low;
}

/* Read a 2-byte tile entry from the arrangement table.
 * arrangement_index = block_id * 16 + sub_row * 4 + sub_col
 * Each entry is 2 bytes (little-endian tilemap word).
 *
 * Port of LOAD_MAP_ROW_TO_VRAM (C00E16): the arrangement stores 4×4 tiles
 * per block in row-major order: row offset = (tile_y & 3) * 4,
 * column offset = (tile_x & 3). */
static uint16_t get_arrangement_tile(uint16_t block_id, uint8_t sub_col, uint8_t sub_row) {
    size_t index = (size_t)block_id * 16 + sub_row * 4 + sub_col;
    size_t byte_offset = index * 2;
    if (byte_offset + 2 > arrangement_size)
        return 0;
    return read_u16_le(&arrangement_buffer[byte_offset]);
}

bool map_loader_init(void) {
    if (tilesetpalette_data) return true; /* already loaded */

    tilesetpalette_data = ASSET_DATA(ASSET_DATA_GLOBAL_MAP_TILESETPALETTE_DATA_BIN);
    tilesetpalette_size = ASSET_SIZE(ASSET_DATA_GLOBAL_MAP_TILESETPALETTE_DATA_BIN);
    if (!tilesetpalette_data) {
        fprintf(stderr, "map_loader: failed to load global_map_tilesetpalette_data.bin\n");
        return false;
    }

    tileset_table_data = ASSET_DATA(ASSET_DATA_TILESET_TABLE_BIN);
    tileset_table_size = ASSET_SIZE(ASSET_DATA_TILESET_TABLE_BIN);
    if (!tileset_table_data) {
        fprintf(stderr, "map_loader: failed to load tileset_table.bin\n");
        tilesetpalette_data = NULL;
        return false;
    }

    load_chunks();

    /* Load NPC placement data (non-fatal if missing) */
    sprite_placement_ptr_table = ASSET_DATA(ASSET_DATA_SPRITE_PLACEMENT_PTR_TABLE_BIN);
    sprite_placement_ptr_table_size = ASSET_SIZE(ASSET_DATA_SPRITE_PLACEMENT_PTR_TABLE_BIN);
    sprite_placement_table = ASSET_DATA(ASSET_DATA_SPRITE_PLACEMENT_TABLE_BIN);
    sprite_placement_table_size = ASSET_SIZE(ASSET_DATA_SPRITE_PLACEMENT_TABLE_BIN);
    {
        size_t npc_config_bytes = ASSET_SIZE(ASSET_DATA_NPC_CONFIG_TABLE_BIN);
        npc_config_table = (const NpcConfig *)ASSET_DATA(ASSET_DATA_NPC_CONFIG_TABLE_BIN);
        npc_config_table_count = npc_config_bytes / sizeof(NpcConfig);
    }
    if (!sprite_placement_ptr_table || !sprite_placement_table || !npc_config_table) {
        fprintf(stderr, "map_loader: NPC data tables not available (non-fatal)\n");
    }

    /* Load sector attributes table (non-fatal if missing) */
    sector_attributes_data = ASSET_DATA(ASSET_DATA_PER_SECTOR_ATTRIBUTES_BIN);
    sector_attributes_size = ASSET_SIZE(ASSET_DATA_PER_SECTOR_ATTRIBUTES_BIN);
    if (!sector_attributes_data) {
        fprintf(stderr, "map_loader: sector attributes not available (non-fatal)\n");
    }

    /* Load collision data tables */
    collision_arrangement_table = ASSET_DATA(ASSET_DATA_MAP_COLLISION_ARRANGEMENT_TABLE_BIN);
    collision_arrangement_table_size = ASSET_SIZE(ASSET_DATA_MAP_COLLISION_ARRANGEMENT_TABLE_BIN);
    collision_pointers_blob = ASSET_DATA(ASSET_DATA_MAP_COLLISION_POINTERS_BLOB_BIN);
    collision_pointers_blob_size = ASSET_SIZE(ASSET_DATA_MAP_COLLISION_POINTERS_BLOB_BIN);
    if (!collision_arrangement_table || !collision_pointers_blob) {
        fprintf(stderr, "map_loader: collision data not available (non-fatal)\n");
    }

    /* Load entity collision geometry tables */
    entity_collision_left_x = ASSET_DATA(ASSET_DATA_ENTITY_COLLISION_LEFT_X_BIN);
    entity_collision_top_y = ASSET_DATA(ASSET_DATA_ENTITY_COLLISION_TOP_Y_BIN);
    entity_collision_width = ASSET_DATA(ASSET_DATA_ENTITY_COLLISION_WIDTH_BIN);
    entity_collision_tile_count = ASSET_DATA(ASSET_DATA_ENTITY_COLLISION_TILE_COUNT_BIN);
    entity_collision_height_offset = ASSET_DATA(ASSET_DATA_ENTITY_COLLISION_HEIGHT_OFFSET_BIN);

    /* Load event-triggered tile change tables (non-fatal if missing) */
    event_control_ptr_table_data = ASSET_DATA(ASSET_MAPS_EVENT_CONTROL_PTR_TABLE_BIN);
    event_control_ptr_table_size = ASSET_SIZE(ASSET_MAPS_EVENT_CONTROL_PTR_TABLE_BIN);
    tile_event_data = ASSET_DATA(ASSET_MAPS_TILE_EVENT_CONTROL_TABLE_BIN);
    tile_event_data_size = ASSET_SIZE(ASSET_MAPS_TILE_EVENT_CONTROL_TABLE_BIN);
    if (!event_control_ptr_table_data || !tile_event_data) {
        fprintf(stderr, "map_loader: event control tables not available (non-fatal)\n");
    }

    /* Load map music data tables (non-fatal if missing) */
    per_sector_music_data = ASSET_DATA(ASSET_DATA_PER_SECTOR_MUSIC_BIN);
    per_sector_music_size = ASSET_SIZE(ASSET_DATA_PER_SECTOR_MUSIC_BIN);
    event_music_ptr_table = ASSET_DATA(ASSET_DATA_OVERWORLD_EVENT_MUSIC_PTR_TABLE_BIN);
    event_music_ptr_table_size = ASSET_SIZE(ASSET_DATA_OVERWORLD_EVENT_MUSIC_PTR_TABLE_BIN);
    event_music_table = ASSET_DATA(ASSET_DATA_OVERWORLD_EVENT_MUSIC_TABLE_BIN);
    event_music_table_size = ASSET_SIZE(ASSET_DATA_OVERWORLD_EVENT_MUSIC_TABLE_BIN);
    if (!per_sector_music_data || !event_music_ptr_table || !event_music_table) {
        fprintf(stderr, "map_loader: music data not available (non-fatal)\n");
    }

    return true;
}

void map_loader_free(void) {
    tilesetpalette_data = NULL;
    tilesetpalette_size = 0;
    tileset_table_data = NULL;
    tileset_table_size = 0;
    ml.loaded_tileset_combo = -1;
    ml.loaded_palette_index = -1;
    arrangement_loaded = false;
    arrangement_size = 0;
    for (int i = 0; i < NUM_CHUNKS; i++) {
        chunk_data[i] = NULL;
        chunk_size[i] = 0;
    }
    chunks_loaded = false;
    sprite_placement_ptr_table = NULL;
    sprite_placement_ptr_table_size = 0;
    sprite_placement_table = NULL;
    sprite_placement_table_size = 0;
    npc_config_table = NULL;
    npc_config_table_count = 0;
    sector_attributes_data = NULL;
    sector_attributes_size = 0;
    collision_arrangement_table = NULL;
    collision_arrangement_table_size = 0;
    collision_pointers_blob = NULL;
    collision_pointers_blob_size = 0;
    ml.tile_collision_loaded = false;
    entity_collision_left_x = NULL;
    entity_collision_top_y = NULL;
    entity_collision_width = NULL;
    entity_collision_tile_count = NULL;
    entity_collision_height_offset = NULL;
    per_sector_music_data = NULL;
    per_sector_music_size = 0;
    event_music_ptr_table = NULL;
    event_music_ptr_table_size = 0;
    event_music_table = NULL;
    event_music_table_size = 0;
    event_control_ptr_table_data = NULL;
    event_control_ptr_table_size = 0;
    tile_event_data = NULL;
    tile_event_data_size = 0;
}

/* --- LOAD_TILE_COLLISION ---
 * Port of asm/overworld/load_tile_collision.asm.
 *
 * Loads the per-tileset collision pointer array into ml.tile_collision_buffer.
 * Each entry maps a block_id to a byte offset into the collision arrangement table.
 *
 * tileset_id: tileset index (0-19). */
static void load_tile_collision(uint16_t tileset_id) {
    if (!collision_pointers_blob || tileset_id >= 20) return;

    uint32_t blob_offset = tileset_collision_offsets[tileset_id];
    /* Copy up to 960 entries (1920 bytes) from the blob.
     * Assembly copies exactly 960 entries regardless of actual table size. */
    int count = 960;
    for (int i = 0; i < count; i++) {
        uint32_t src = blob_offset + (uint32_t)i * 2;
        if (src + 2 > collision_pointers_blob_size) {
            ml.tile_collision_buffer[i] = 0;
        } else {
            ml.tile_collision_buffer[i] = read_u16_le(collision_pointers_blob + src);
        }
    }
    ml.tile_collision_loaded = true;
}

/* --- REPLACE_BLOCK ---
 * Port of asm/overworld/replace_block.asm.
 *
 * Copies one block's arrangement data (32 bytes = 16 tile entries × 2 bytes)
 * from source block to destination block in arrangement_buffer.
 * Also copies the ml.tile_collision_buffer entry.
 *
 * dest_block: destination block ID
 * src_block: source block ID */
static void replace_block(uint16_t dest_block, uint16_t src_block) {
    uint32_t dest_off = (uint32_t)dest_block * 32;
    uint32_t src_off = (uint32_t)src_block * 32;
    if (dest_off + 32 > SHARED_SCRATCH_SIZE || src_off + 32 > SHARED_SCRATCH_SIZE)
        return;
    memcpy(arrangement_buffer + dest_off, arrangement_buffer + src_off, 32);
    if (dest_block < 960 && src_block < 960) {
        ml.tile_collision_buffer[dest_block] = ml.tile_collision_buffer[src_block];
    }
}

/* --- LOAD_MAP_BLOCK_EVENT_CHANGES ---
 * Port of asm/overworld/load_map_block_event_changes.asm.
 *
 * For a given tileset_id, scans the tile event control table.
 * Each entry has an event flag and a list of block replacement pairs.
 * If the event flag matches the expected state, all block pairs are applied
 * (e.g., removing a pencil statue after getting the pencil eraser).
 *
 * tileset_id: tileset index (0-19). */
static void load_map_block_event_changes(uint16_t tileset_id) {
    if (!event_control_ptr_table_data || !tile_event_data) return;
    if (tileset_id >= NUM_TILESETS) return;
    if (event_control_ptr_table_size < NUM_TILESETS * 2) return;

    /* Read the within-bank pointer for this tileset and for tileset 0.
     * ptr_table[0] points to the start of tile_event_data in the bank,
     * so we use it as the base to convert bank offsets to blob offsets. */
    uint16_t entry_ptr = read_u16_le(event_control_ptr_table_data + tileset_id * 2);
    uint16_t base_ptr = read_u16_le(event_control_ptr_table_data);
    size_t idx = (size_t)(entry_ptr - base_ptr);

    while (idx + 4 <= tile_event_data_size) {
        uint16_t event_flag_word = read_u16_le(tile_event_data + idx);
        if (event_flag_word == 0) break;  /* null terminator = end of list */

        uint16_t flag_number = event_flag_word & 0x7FFF;
        /* Bit 15 = EVENT_FLAG_UNSET ($8000).
         * Assembly: LDX #0; CMP #$8000; BCC skip; LDX #1
         * Bit 15 clear → expected=0 → replace when flag is NOT set.
         * Bit 15 set   → expected=1 → replace when flag IS set. */
        uint16_t expected_value = (event_flag_word & 0x8000) ? 1 : 0;
        uint16_t actual_value = event_flag_get(flag_number) ? 1 : 0;
        uint16_t count = read_u16_le(tile_event_data + idx + 2);

        if (actual_value == expected_value) {
            /* Match: apply block replacements */
            idx += 4;  /* skip event_flag + count header */
            for (uint16_t i = 0; i < count; i++) {
                if (idx + 4 > tile_event_data_size) break;
                uint16_t dest = read_u16_le(tile_event_data + idx);
                uint16_t src = read_u16_le(tile_event_data + idx + 2);
                replace_block(dest, src);
                idx += 4;
            }
        } else {
            /* No match: skip past the block pairs */
            idx += 4 + (size_t)count * 4;
        }
    }
}

/* --- LOAD_CURRENT_MAP_BLOCK_EVENTS ---
 * Port of asm/overworld/map/load_current_map_block_events.asm.
 * Reads the currently loaded tileset and re-applies event tile changes.
 * Called from event scripts after flag changes (e.g., pencil statue removal). */
void load_current_map_block_events(void) {
    if (ml.loaded_tileset_combo < 0) return;
    uint16_t tileset_id = get_tileset_id((uint16_t)ml.loaded_tileset_combo);
    load_map_block_event_changes(tileset_id);
}

/* --- FILL_COLLISION_TILES ---
 * Populates the 64×64 ml.loaded_collision_tiles grid from chunk data + collision tables.
 *
 * Port of LOAD_COLLISION_ROW (asm/overworld/load_collision_row.asm) applied to
 * the full visible area. For each tile in the 64×64 grid:
 *   1. Get block_id from chunk data (same as tilemap fill)
 *   2. Look up collision pointer: ml.tile_collision_buffer[block_id]
 *   3. Read arrangement entry at pointer + sub_row * 4 + sub_col
 *   4. Store collision flag byte in ml.loaded_collision_tiles */
static void fill_collision_tiles(int16_t view_x_tile, int16_t view_y_tile) {
    if (!ml.tile_collision_loaded || !collision_arrangement_table || !chunks_loaded)
        return;

    /* Center the 64-tile fill range around the viewport.
     * view_x/y_tile is the viewport's top-left corner. The viewport is
     * 32×28 tiles (256×224 px). The SNES assembly streams columns/rows from
     * both edges as the camera scrolls, giving equal coverage on all sides.
     * The C port refills all 64 tiles at once — centering gives 16 tiles of
     * padding on each side of the 32-tile viewport, matching the assembly's
     * effective coverage and ensuring nearby off-screen entities (like NPCs
     * behind counters) have valid collision data. */
    int16_t start_x = view_x_tile - 16;
    int16_t start_y = view_y_tile - 16;

    for (int ty = 0; ty < 64; ty++) {
        int16_t world_ty = start_y + ty;
        uint16_t by = (uint16_t)world_ty / 4;       /* block row */
        uint8_t sub_row = (uint16_t)world_ty & 3;

        for (int tx = 0; tx < 64; tx++) {
            int16_t world_tx = start_x + tx;
            uint16_t bx = (uint16_t)world_tx / 4;       /* block column */
            uint8_t sub_col = (uint16_t)world_tx & 3;

            /* Out-of-bounds tiles get zero collision flags. */
            /* Look up collision flags for this tile.
             * Only write to the grid if this tile is within the loaded tileset area.
             * The assembly streams individual rows/columns, so out-of-range cells
             * are never overwritten and retain their previous valid data. The C port's
             * full 64×64 refill must skip out-of-tileset tiles to match — otherwise
             * cells that previously held correct data (from a fill at a different view
             * position) get clobbered with unrelated block-0 collision values. */
            if (world_tx < 0 || world_ty < 0)
                continue;
            if (!tilesetpalette_data)
                continue;
            uint32_t tp_index = (uint32_t)((uint16_t)world_ty >> 4) * 32 + ((uint16_t)world_tx >> 5);
            if (tp_index >= tilesetpalette_size ||
                (tilesetpalette_data[tp_index] >> 3) != (uint8_t)ml.loaded_tileset_combo)
                continue;

            uint16_t block_id = get_block_id(bx, by);
            uint8_t flags = 0;
            if (block_id < 960) {
                uint16_t coll_ptr = ml.tile_collision_buffer[block_id];
                uint32_t arr_off = (uint32_t)coll_ptr + sub_row * 4 + sub_col;
                if (arr_off < collision_arrangement_table_size) {
                    flags = collision_arrangement_table[arr_off];
                }
            }

            uint16_t cx = world_tx & 63;
            uint16_t cy = world_ty & 63;
            ml.loaded_collision_tiles[cy * 64 + cx] = flags;

        }
    }
}

/* --- LOOKUP_SURFACE_FLAGS ---
 * Port of asm/overworld/collision/lookup_surface_flags.asm.
 *
 * Looks up collision/surface flags at a world position for an entity of given size.
 * Samples the ml.loaded_collision_tiles grid at multiple points within the entity's
 * collision rect and ORs all flags together.
 *
 * x, y: world pixel coordinates
 * size_code: entity size index (from entities.sizes[]) */
uint16_t lookup_surface_flags(int16_t x, int16_t y, uint16_t size_code) {
    if (!entity_collision_left_x || !entity_collision_top_y ||
        !entity_collision_height_offset || !entity_collision_tile_count ||
        !entity_collision_width) {
        return 0;
    }

    if (size_code > 16) size_code = 0;
    uint16_t size_off = size_code * 2;

    /* Read entity collision geometry from size tables */
    uint16_t left_off = read_u16_le(entity_collision_left_x + size_off);
    uint16_t top_off = read_u16_le(entity_collision_top_y + size_off);
    uint16_t height_off = read_u16_le(entity_collision_height_offset + size_off);
    uint16_t tile_count = read_u16_le(entity_collision_tile_count + size_off);
    uint16_t width_px = read_u16_le(entity_collision_width + size_off);

    /* Calculate collision rect edges (matching assembly exactly):
     * left_x = x - left_offset
     * top_y = y - top_offset + height_offset */
    int16_t left_x = x - (int16_t)left_off;
    int16_t top_y = y - (int16_t)top_off + (int16_t)height_off;

    uint8_t flags = 0;

    /* ACCUMULATE_COLLISION_FLAGS_VERTICAL (C05639):
     * Sample at (left_x, top_y) first, then iterate downward from (left_x, (top_y+7)/8).
     * Assembly: lookup_surface_flags computes top_y = y - top_offset + height_offset,
     * stores it in CHECKED_COLLISION_TOP_Y, then passes it as A to this routine.
     * Uses top_y (not raw y). */
    {
        uint16_t cx = ((uint16_t)left_x >> 3) & 0x3F;
        uint16_t cy_first = ((uint16_t)top_y >> 3) & 0x3F;
        flags |= ml.loaded_collision_tiles[cy_first * 64 + cx];

        uint16_t cy_start = ((uint16_t)top_y + 7) >> 3;
        for (uint16_t i = 0; i < tile_count; i++) {
            uint16_t cy = (cy_start + i) & 0x3F;
            flags |= ml.loaded_collision_tiles[cy * 64 + cx];
        }
    }

    /* ACCUMULATE_COLLISION_FLAGS_HORIZONTAL (C056D0):
     * Sample at (right_x, top_y) first, then iterate downward from (right_x, (top_y+7)/8). */
    {
        uint16_t right_x = ((uint16_t)(width_px * 8 + left_x - 1) >> 3) & 0x3F;
        uint16_t cy_first = ((uint16_t)top_y >> 3) & 0x3F;
        flags |= ml.loaded_collision_tiles[cy_first * 64 + right_x];

        uint16_t cy_start = ((uint16_t)top_y + 7) >> 3;
        for (uint16_t i = 0; i < tile_count; i++) {
            uint16_t cy = (cy_start + i) & 0x3F;
            flags |= ml.loaded_collision_tiles[cy * 64 + right_x];
        }
    }

    return flags;
}

/* --- LOAD_SECTOR_ATTRS ---
 * Port of asm/overworld/load_sector_attributes.asm.
 *
 * Looks up the per-sector attributes word for a world position.
 * Sector grid: 32 columns (256px wide) × 80 rows (128px tall).
 * A = x_pixels (first param), X = y_pixels (second param).
 *
 * Returns the 16-bit sector attributes word and stores it in
 * ml.current_sector_attributes (mirroring the assembly global). */
uint16_t load_sector_attrs(uint16_t x_pixels, uint16_t y_pixels) {
    if (!sector_attributes_data) return 0;

    /* Assembly: XBA; AND #$00FF → sector_x = x_pixels >> 8 */
    uint16_t sector_x = x_pixels >> 8;

    /* Assembly: TXA; AND #$FF80; LSR; LSR → (y_pixels & 0xFF80) >> 2
     * This equals (y_pixels / 128) * 32 */
    uint16_t y_index = (y_pixels & 0xFF80) >> 2;

    /* Assembly: CLC; ADC @VIRTUAL02; ASL → (y_index + sector_x) * 2 */
    uint16_t byte_offset = (y_index + sector_x) * 2;

    if ((size_t)byte_offset + 2 > sector_attributes_size) return 0;

    uint16_t val = read_u16_le(sector_attributes_data + byte_offset);
    ml.current_sector_attributes = val;
    return val;
}

/* Helper: get NPC config entry with bounds check. */
static const NpcConfig *get_npc_config(uint16_t npc_id) {
    if (!npc_config_table || npc_id >= npc_config_table_count) return NULL;
    return &npc_config_table[npc_id];
}

uint16_t get_npc_config_event_flag(uint16_t npc_id) {
    const NpcConfig *cfg = get_npc_config(npc_id);
    return cfg ? cfg->event_flag : 0;
}

uint8_t get_npc_config_direction(uint16_t npc_id) {
    if (npc_id == 0xFFFF) return 4;  /* DOWN */
    const NpcConfig *cfg = get_npc_config(npc_id);
    return cfg ? cfg->direction : 4;
}

uint16_t get_npc_config_sprite(uint16_t npc_id) {
    const NpcConfig *cfg = get_npc_config(npc_id);
    return cfg ? cfg->sprite : 0;
}

uint8_t get_npc_config_type(uint16_t npc_id) {
    const NpcConfig *cfg = get_npc_config(npc_id);
    return cfg ? cfg->type : 0;
}

uint32_t get_npc_config_text_pointer(uint16_t npc_id) {
    const NpcConfig *cfg = get_npc_config(npc_id);
    return cfg ? cfg->text_pointer : 0;
}

uint16_t get_npc_config_item(uint16_t npc_id) {
    const NpcConfig *cfg = get_npc_config(npc_id);
    return cfg ? (uint16_t)cfg->item : 0;
}

uint32_t get_npc_config_text_pointer2(uint16_t npc_id) {
    const NpcConfig *cfg = get_npc_config(npc_id);
    return cfg ? cfg->item : 0;  /* union: item field doubles as text_pointer2 */
}

/* Look up tileset ID from tileset_combo via TILESET_TABLE */
static uint16_t get_tileset_id(uint16_t tileset_combo) {
    if (!tileset_table_data) return 0;
    size_t offset = (size_t)tileset_combo * 2;
    if (offset + 2 > tileset_table_size) return 0;
    return read_u16_le(&tileset_table_data[offset]);
}

/* Load a compressed asset file (LZHAL) by AssetId and decompress into a buffer.
 * Returns decompressed size, or 0 on failure. */
static size_t load_and_decompress(AssetId id, uint8_t *dst, size_t dst_max) {
    const uint8_t *compressed = ASSET_DATA(id);
    size_t compressed_size = ASSET_SIZE(id);
    if (!compressed) {
        fprintf(stderr, "map_loader: failed to load asset %d\n", (int)id);
        return 0;
    }

    size_t decompressed_size = decomp(compressed, compressed_size, dst, dst_max);
    return decompressed_size;
}

/* ROM low-word addresses of each MAP_DATA_PALETTE_N file (from linker map).
 * Used to resolve event-flag palette overrides, which encode ROM addresses
 * in the palette data itself (assembly lines 42-63 of LOAD_MAP_PAL). */
static const uint16_t palette_file_rom_low[32] = {
    0x7CA7, 0x7FA7, 0x81E7, 0x84E7, 0x8667, 0x87E7, 0x8AE7, 0x9027,
    0x90E7, 0x9267, 0x96E7, 0x9CE7, 0xA2E7, 0xA8E7, 0xABE7, 0xB1E7,
    0xB7E7, 0xBAE7, 0xC0E7, 0xC1A7, 0xC6E7, 0xCCE7, 0xD0A7, 0xD467,
    0xD767, 0xDB27, 0xE127, 0xE5A7, 0xE967, 0xEDE7, 0xF267, 0xF4A7,
};

/* Resolve a palette ROM low-word address to (file_number, byte_offset).
 * Returns false if the address doesn't fall within any known palette file. */
static bool resolve_palette_rom_address(uint16_t rom_low, int *file_num,
                                         size_t *byte_offset) {
    for (int i = 31; i >= 0; i--) {
        if (palette_file_rom_low[i] <= rom_low) {
            *file_num = i;
            *byte_offset = rom_low - palette_file_rom_low[i];
            return true;
        }
    }
    return false;
}

/* Forward declarations for post-fade sprite palette adjustment. */
static void adjust_sprite_palettes_by_average(void);
static void load_special_sprite_palette(void);

/* Reload sprite group ert.palettes from asset files into ert.palettes[128..255].
 * Port of MEMCPY16 from SPRITE_GROUP_PALETTES in load_map_palette.asm line 78-81
 * and load_map_at_sector.asm line 78. */
static void reload_sprite_group_palettes(void) {
    for (int p = 0; p < 8; p++) {
        const uint8_t *spr_pal = ASSET_DATA(ASSET_OVERWORLD_SPRITES_PALETTES(p));
        size_t spr_pal_size = ASSET_SIZE(ASSET_OVERWORLD_SPRITES_PALETTES(p));
        if (spr_pal && spr_pal_size >= BPP4PALETTE_SIZE) {
            memcpy(&ert.palettes[128 + p * 16], spr_pal, BPP4PALETTE_SIZE);
        }
    }
}

/* Load map palette from maps/palettes/{tileset_combo}.pal (no override).
 *
 * Port of LOAD_MAP_PALETTE (asm/system/palette/load_map_palette.asm):
 *   1. MAP_PALETTE_PTR_TABLE[tileset_combo] → base pointer to palette data
 *   2. Offset within file = palette_index × 192
 *   3. fade_frames == 0: Copy 192 bytes to PALETTES sub-ert.palettes 2-7 (instant)
 *   4. fade_frames > 0: Smooth per-channel fade over N frames, then:
 *      - Slam final BG palette
 *      - Reload sprite ert.palettes from SPRITE_GROUP_PALETTES
 *      - ADJUST_SPRITE_PALETTES_BY_AVERAGE + LOAD_SPECIAL_SPRITE_PALETTE
 *
 * Used by CC_1F_E1 (SET_MAP_PALETTE script command). Does NOT check
 * event flag overrides — scripts provide the final palette index directly. */
void load_map_palette(uint16_t tileset_combo, uint16_t palette_index,
                      uint16_t fade_frames) {
    /* Assembly line 21: clear palette animation flag */
    ml.map_palette_animation_loaded = 0;

    const uint8_t *pal_data = ASSET_DATA(ASSET_MAPS_PALETTES(tileset_combo));
    size_t pal_size = ASSET_SIZE(ASSET_MAPS_PALETTES(tileset_combo));
    if (!pal_data) {
        fprintf(stderr, "map_loader: failed to load palette %d\n", tileset_combo);
        return;
    }

    size_t offset = (size_t)palette_index * (BPP4PALETTE_SIZE * 6);
    if (offset + BPP4PALETTE_SIZE * 6 > pal_size) {
        fprintf(stderr, "map_loader: palette %d too small for index %d "
                "(offset=%zu, size=%zu)\n", tileset_combo, palette_index, offset, pal_size);
        return;
    }

    if (fade_frames == 0) {
        /* Instant path (assembly lines 44-48) */
        memcpy(&ert.palettes[32], pal_data + offset, BPP4PALETTE_SIZE * 6);
        ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
        return;
    }

    /* Fade path (assembly lines 49-89).
     * Port of INITIALIZE_MAP_PALETTE_FADE + UPDATE_MAP_PALETTE_FADE.
     * Performs smooth per-channel color interpolation from current palette
     * to target palette over fade_frames frames using 8.8 fixed-point. */

    /* Parse target palette (96 colors for sub-ert.palettes 2-7).
     * Use ert.buffer as scratch — it's free during palette fades (the
     * tileset decompress that last used it is complete by this point,
     * and the blocking fade loop doesn't run game logic). */
    uint16_t *target  = (uint16_t *)&ert.buffer[BUF_FLASH_TARGET];    /* 192 bytes */
    int16_t  *r_accum = (int16_t  *)&ert.buffer[BUF_FLASH_ACCUM_R]; /* 192 bytes */
    int16_t  *g_accum = (int16_t  *)&ert.buffer[BUF_FLASH_ACCUM_G]; /* 192 bytes */
    int16_t  *b_accum = (int16_t  *)&ert.buffer[BUF_FLASH_ACCUM_B]; /* 192 bytes */
    int16_t  *r_incr  = (int16_t  *)&ert.buffer[BUF_FLASH_SLOPE_R]; /* 192 bytes */
    int16_t  *g_incr  = (int16_t  *)&ert.buffer[BUF_FLASH_SLOPE_G]; /* 192 bytes */
    int16_t  *b_incr  = (int16_t  *)&ert.buffer[BUF_FLASH_SLOPE_B]; /* 192 bytes */
    memcpy(target, pal_data + offset, 96 * sizeof(uint16_t));

    /* INITIALIZE_MAP_PALETTE_FADE:
     * For each color channel, compute 8.8 fixed-point accumulator and slope.
     * slope = ((target_channel - current_channel) << 8) / frames */
    int16_t div = (int16_t)fade_frames;

    for (int i = 0; i < 96; i++) {
        uint16_t cur = ert.palettes[32 + i];
        uint16_t tgt = target[i];

        int16_t cur_r = cur & 0x1F;
        int16_t cur_g = (cur >> 5) & 0x1F;
        int16_t cur_b = (cur >> 10) & 0x1F;

        int16_t tgt_r = tgt & 0x1F;
        int16_t tgt_g = (tgt >> 5) & 0x1F;
        int16_t tgt_b = (tgt >> 10) & 0x1F;

        r_accum[i] = cur_r << 8;
        g_accum[i] = cur_g << 8;
        b_accum[i] = cur_b << 8;

        r_incr[i] = ((tgt_r - cur_r) * 256) / div;
        g_incr[i] = ((tgt_g - cur_g) * 256) / div;
        b_incr[i] = ((tgt_b - cur_b) * 256) / div;
    }

    /* UPDATE_MAP_PALETTE_FADE each frame (assembly lines 60-72).
     * Assembly only calls WAIT_UNTIL_NEXT_FRAME, NOT a full render tick. */
    for (uint16_t f = 0; f < fade_frames; f++) {
        wait_for_vblank();
        for (int i = 0; i < 96; i++) {
            r_accum[i] += r_incr[i];
            g_accum[i] += g_incr[i];
            b_accum[i] += b_incr[i];

            uint16_t r = (uint16_t)((r_accum[i] >> 8) & 0x1F);
            uint16_t g = (uint16_t)((g_accum[i] >> 8) & 0x1F);
            uint16_t b = (uint16_t)((b_accum[i] >> 8) & 0x1F);

            ert.palettes[32 + i] = r | (g << 5) | (b << 10);
        }
        ert.palette_upload_mode = PALETTE_UPLOAD_BG_ONLY;
    }

    /* After fade: slam final BG palette (assembly lines 73-77) */
    memcpy(&ert.palettes[32], target, 96 * sizeof(uint16_t));

    /* Reload sprite ert.palettes from SPRITE_GROUP_PALETTES (assembly lines 78-81) */
    reload_sprite_group_palettes();

    /* Adjust sprite ert.palettes for new map lighting (assembly lines 82-83) */
    adjust_sprite_palettes_by_average();
    load_special_sprite_palette();

    /* Assembly lines 84-89: set full upload and wait for DMA */
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    wait_for_vblank();
}

/* Load map palette with event flag override checking.
 *
 * Port of LOAD_MAP_PAL (asm/overworld/load_map_palette.asm):
 * Same as load_map_palette but also checks event flag overrides (lines 42-63):
 *   - First word of palette set: if non-zero, it's an event flag marker
 *   - flag_num = first_word & 0x7FFF
 *   - If value > 0x8000: override triggers when flag IS SET
 *   - If value <= 0x8000: override triggers when flag is UNSET
 *   - Override target ROM address at byte offset 32 in the palette set
 *   - Loops until a non-override palette set is reached
 *
 * Used by load_map_at_sector (overworld map loading). */
static void load_map_palette_overworld(uint16_t tileset_combo,
                                        uint16_t palette_index) {
    int file_num = tileset_combo;
    size_t byte_offset = (size_t)palette_index * (BPP4PALETTE_SIZE * 6);

    for (int iter = 0; iter < 4; iter++) {
        const uint8_t *pal_data = ASSET_DATA(ASSET_MAPS_PALETTES(file_num));
        size_t pal_size = ASSET_SIZE(ASSET_MAPS_PALETTES(file_num));
        if (!pal_data) {
            fprintf(stderr, "map_loader: failed to load palette %d\n", file_num);
            return;
        }

        if (byte_offset + BPP4PALETTE_SIZE * 6 > pal_size) {
            fprintf(stderr, "map_loader: palette %d too small for offset %zu "
                    "(size=%zu)\n", file_num, byte_offset, pal_size);
            return;
        }

        const uint8_t *pal_set = pal_data + byte_offset;

        /* Copy 192 bytes to sub-ert.palettes 2-7 */
        memcpy(&ert.palettes[32], pal_set, BPP4PALETTE_SIZE * 6);

        /* Check for event flag override (assembly LOAD_MAP_PAL lines 42-63) */
        uint16_t first_word = read_u16_le(&pal_set[0]);
        if (first_word == 0) {
            break;
        }

        uint16_t flag_num = first_word & 0x7FFF;
        bool flag_is_set = event_flag_get(flag_num);
        bool expected_set = (first_word > 0x8000);

        if (flag_is_set != expected_set) {
            break;
        }

        /* Override: read target ROM low-word from bytes 32-33 */
        uint16_t override_low = read_u16_le(&pal_set[32]);

        if (!resolve_palette_rom_address(override_low, &file_num, &byte_offset)) {
            fprintf(stderr, "map_loader: failed to resolve palette override "
                    "0x%04X\n", override_low);
            break;
        }
    }

    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
}

/* animate_palette_fade_to_map removed — its logic is now inside load_map_palette's
 * fade path (assembly LOAD_MAP_PALETTE handles both instant and fade). */

/* Fill BG1/BG2 tilemaps from chunk data + arrangement table.
 *
 * Port of LOAD_MAP_ROW_TO_VRAM (asm/overworld/map/load_map_row_to_vram.asm).
 *
 * For each tile in the visible area:
 *   1. Convert tile coords to block coords and sub-position
 *   2. Look up block_id from chunk data (get_block_id)
 *   3. Look up tile entry from arrangement table (get_arrangement_tile)
 *   4. BG1 = tile entry as-is
 *   5. BG2 = tile entry | $2000 if tile_index < 384, else 0
 *   6. Write to VRAM tilemaps */
static void fill_tilemaps(int16_t view_x_tile, int16_t view_y_tile) {
    if (!arrangement_loaded || !chunks_loaded) return;

    LOG_TRACE("map_loader: fill_tilemaps view=(%d,%d) arr_size=%zu\n",
              view_x_tile, view_y_tile, arrangement_size);

    /* BG1 tilemap at VRAM byte $7000 (word $3800), 64×32 entries.
     * BG2 tilemap at VRAM byte $B000 (word $5800), 64×32 entries.
     * Each tilemap is split into two 32-wide nametables (HORIZONTAL mode). */

    for (int ty = 0; ty < 32; ty++) {
        int16_t world_ty = view_y_tile + ty;
        uint16_t by = (uint16_t)world_ty / 4;       /* block row */
        uint8_t sub_row = (uint16_t)world_ty & 3;   /* row within block (0-3) */

        for (int tx = 0; tx < 64; tx++) {
            int16_t world_tx = view_x_tile + tx;
            uint16_t bx = (uint16_t)world_tx / 4;       /* block column */
            uint8_t sub_col = (uint16_t)world_tx & 3;   /* column within block (0-3) */

            /* Out-of-bounds tiles (negative coords or beyond map) get empty block.
             * Assembly: LOAD_MAP_ROW checks block_row < 320 (unsigned), so negative
             * rows wrap to huge values and fail → block_id = 0 (empty). */
            uint16_t block_id = 0;
            if (world_tx < 0 || world_ty < 0) {
                /* Off the top/left edge of the map — leave as empty */
            } else if (tilesetpalette_data) {
                uint32_t tp_index = (uint32_t)((uint16_t)world_ty >> 4) * 32 + ((uint16_t)world_tx >> 5);
                if (tp_index < tilesetpalette_size &&
                    (tilesetpalette_data[tp_index] >> 3) == (uint8_t)ml.loaded_tileset_combo) {
                    block_id = get_block_id(bx, by);
                }
            }

            /* Look up tile entry from arrangement */
            uint16_t bg1_tile = get_arrangement_tile(block_id, sub_col, sub_row);

            /* BG2 logic from LOAD_MAP_ROW_TO_VRAM:
             * If tile index (bits 0-9) < 384: BG2 = tile | $2000
             * Otherwise: BG2 = 0 (transparent) */
            uint16_t bg2_tile;
            if ((bg1_tile & 0x03FF) < 384) {
                bg2_tile = bg1_tile | 0x2000;
            } else {
                bg2_tile = 0;
            }

            /* Compute VRAM tilemap address.
             * The tilemap is a circular ert.buffer: world tile (wx, wy) maps to
             * tilemap position (wx & 63, wy & 31). The SNES hardware uses
             * the scroll registers to select which entries are visible.
             *
             * 64-wide tilemap = two 32-wide nametables.
             * First nametable: columns 0-31, second: columns 32-63.
             * Each row = 32 words. Second nametable offset = 0x400 words. */
            uint16_t tm_col = world_tx & 63;
            uint16_t tm_row = world_ty & 31;
            uint16_t nt_x = tm_col & 31;
            uint16_t nt_half = (tm_col >= 32) ? 1 : 0;
            uint16_t word_offset = tm_row * 32 + nt_x + nt_half * 0x400;

            /* BG1 tilemap at byte $7000 */
            size_t bg1_addr = 0x7000 + word_offset * 2;
            if (bg1_addr + 1 < sizeof(ppu.vram)) {
                ppu.vram[bg1_addr]     = bg1_tile & 0xFF;
                ppu.vram[bg1_addr + 1] = bg1_tile >> 8;
            }
            /* BG2 tilemap at byte $B000 */
            size_t bg2_addr = 0xB000 + word_offset * 2;
            if (bg2_addr + 1 < sizeof(ppu.vram)) {
                ppu.vram[bg2_addr]     = bg2_tile & 0xFF;
                ppu.vram[bg2_addr + 1] = bg2_tile >> 8;
            }
        }
    }
}

/* ---- LOAD_BG_PALETTE (port of C0A1F2) ----
 *
 * Port of asm/system/palette/load_bg_palette.asm.
 * Copies 192 bytes (96 colors for sub-ert.palettes 2-7) from the palette
 * animation ert.buffer frame [index] into ert.palettes[32..127].
 *
 * In assembly, the PALETTE_ANIM_BUFFER_PTR_TABLE at C0A20C provides absolute
 * RAM addresses ($B800, $B8C0, ...) for each frame. The spacing is $C0 = 192
 * bytes. In the C port, we compute the offset directly: index * 192.
 *
 * PALETTES is at $7E:0200. The MVN destination $0240 = PALETTES + $40 = ert.palettes[32]. */
void load_bg_palette(uint16_t index) {
    uint32_t offset = (uint32_t)index * 192;
    if (offset + 192 > sizeof(ml.animated_map_palette_buffer)) return;
    memcpy(&ert.palettes[32], ml.animated_map_palette_buffer + offset, 192);
    ert.palette_upload_mode = PALETTE_UPLOAD_BG_ONLY;
}

/* ---- ANIMATE_PALETTE (port of animate_palette.asm) ----
 *
 * Port of asm/system/animate_palette.asm.
 * Called each frame from update_overworld_frame() when MAP_PALETTE_ANIMATION_LOADED.
 * Decrements the animation timer. When it expires, loads the next palette frame
 * and resets the timer from the delays[] table. Wraps index back to 0 when
 * delays[index] == 0 (sentinel). */
void animate_palette(void) {
    if (--ml.overworld_palette_anim.timer != 0) return;

    /* Timer expired — check for wrap (bounds check before array access) */
    if (ml.overworld_palette_anim.index >= 9)
        FATAL("animate_palette: index=%u out of bounds\n", ml.overworld_palette_anim.index);
    if (ml.overworld_palette_anim.delays[ml.overworld_palette_anim.index] == 0) {
        ml.overworld_palette_anim.index = 0;
    }

    /* Set timer for this frame and load the palette */
    ml.overworld_palette_anim.timer =
        ml.overworld_palette_anim.delays[ml.overworld_palette_anim.index];
    load_bg_palette(ml.overworld_palette_anim.index);
    ml.overworld_palette_anim.index++;
}

/* ---- ANIMATE_TILESET (port of animate_tileset.asm) ----
 *
 * Port of asm/system/animate_tileset.asm.
 * Called each frame from update_overworld_frame() when LOADED_ANIMATED_TILE_COUNT > 0.
 * For each active tileset animation entry, decrements frames_until_update.
 * When it expires:
 *   1. Reset counter to frame_delay
 *   2. If current_frame >= animation_limit: wrap to 0, reset source offset
 *   3. Copy copy_size bytes from ml.animated_tileset_buffer to ppu.vram
 *   4. Advance source offset and frame counter */
void animate_tileset(void) {
    for (int i = 0; i < ml.loaded_animated_tile_count; i++) {
        OverworldTilesetAnim *anim = &ml.overworld_tileset_anim[i];

        if (--anim->frames_until_update != 0) continue;

        /* Timer expired — reset delay */
        anim->frames_until_update = anim->frame_delay;

        /* Check for frame wrap */
        if (anim->current_frame >= anim->animation_limit) {
            anim->current_frame = 0;
            anim->current_source_offset = anim->source_offset;
        }

        /* Copy tile data from animation ert.buffer to VRAM.
         * Assembly uses PREPARE_VRAM_COPY (mode=0, i.e. word-sequential)
         * which queues a DMA to the VRAM destination address.
         * destination_address is a VRAM word address — multiply by 2 for byte offset. */
        uint32_t vram_byte_offset = (uint32_t)anim->destination_address * 2;
        uint32_t src_offset = anim->current_source_offset;
        uint16_t size = anim->copy_size;

        if (src_offset + size <= sizeof(ml.animated_tileset_buffer) &&
            vram_byte_offset + size <= sizeof(ppu.vram)) {
            memcpy(&ppu.vram[vram_byte_offset],
                   ml.animated_tileset_buffer + src_offset, size);
        }

        /* Advance source offset and frame counter */
        anim->current_source_offset += size;
        anim->current_frame++;
    }
}

/* ---- LOAD_TILESET_ANIM (port of load_tileset_anim.asm) ----
 *
 * Port of asm/system/load_tileset_anim.asm.
 * Called from load_map_at_sector after loading tileset GFX.
 *
 * 1. Look up TILESET_ANIMATION_PROPERTIES for current tileset
 * 2. If count > 0, decompress tile animation GFX into ml.animated_tileset_buffer
 * 3. Parse per-entry metadata into ml.overworld_tileset_anim[] */
static void load_tileset_anim(uint16_t tileset_id) {
    ml.loaded_animated_tile_count = 0;

    /* Load tileset animation properties.
     * Assembly: MAP_DATA_WEIRD_TILE_ANIMATION_PTR_TABLE[LOADED_MAP_TILESET]
     * → TILESET_ANIMATION_PROPERTIES_N → count byte + entries. */
    const uint8_t *props = ASSET_DATA(ASSET_MAPS_ANIM_PROPS(tileset_id));
    size_t props_size = ASSET_SIZE(ASSET_MAPS_ANIM_PROPS(tileset_id));
    if (!props || props_size < 1) {
        return;
    }

    uint8_t count = props[0];
    if (count == 0 || count > 8) {
        return;
    }

    /* Verify we have enough property data (count + count * 8 bytes per entry) */
    size_t needed = 1 + (size_t)count * 8;
    if (props_size < needed) {
        return;
    }

    /* Load and decompress tile animation GFX.
     * Assembly: MAP_DATA_TILE_ANIMATION_PTR_TABLE[LOADED_MAP_TILESET]
     * → compressed tile data → ANIMATED_TILESET_BUFFER. */
    size_t gfx_size = load_and_decompress(ASSET_MAPS_ANIM_GFX(tileset_id), ml.animated_tileset_buffer,
                                          sizeof(ml.animated_tileset_buffer));
    if (gfx_size == 0) {
        return;
    }

    /* Parse properties into ml.overworld_tileset_anim[] entries.
     * Each entry in the properties file (overworld_tileset_anim_entry):
     *   unknown0 (.byte) — animation frame limit
     *   frame_delay (.byte) — frames between updates
     *   copy_size (.word) — bytes per frame
     *   source_offset (.word) — initial source offset in ert.buffer
     *   destination_address (.word) — VRAM word destination */
    ml.loaded_animated_tile_count = count;
    const uint8_t *p = props + 1;
    for (int i = 0; i < count; i++) {
        OverworldTilesetAnim *anim = &ml.overworld_tileset_anim[i];
        anim->animation_limit   = p[0];              /* unknown0 */
        anim->frame_delay       = p[1];              /* frame_delay */
        anim->frames_until_update = p[1];            /* starts at frame_delay */
        anim->copy_size         = read_u16_le(&p[2]); /* little-endian word */
        anim->source_offset     = read_u16_le(&p[4]);
        anim->current_source_offset = anim->source_offset;
        anim->destination_address = read_u16_le(&p[6]);
        anim->current_frame     = 0;
        p += 8;
    }
}

/* ---- LOAD_PALETTE_ANIM (port of load_palette_anim.asm) ----
 *
 * Port of asm/system/load_palette_anim.asm.
 * Called from load_map_at_sector after loading the map palette.
 *
 * The palette animation index is stored in ert.palettes[80] (first color of
 * sub-palette 5). If 0, no animation. Otherwise it's a 1-based index
 * into the palette animation secondary table.
 *
 * The secondary table (maps/anim_pal/meta_table.bin) contains 31 variable-length
 * entries, each: 4 bytes pointer (ROM, ignored in C) + 1 byte count + count delay
 * bytes. The compressed palette data is in maps/anim_pal/{index}.pal.lzhal. */
static void load_palette_anim(void) {
    ml.map_palette_animation_loaded = 0;
    memset(&ml.overworld_palette_anim, 0, sizeof(ml.overworld_palette_anim));

    /* Check ert.palettes[80] — first color of sub-palette 5.
     * This serves as the palette animation index (1-based). */
    uint16_t anim_index = ert.palettes[80];
    if (anim_index == 0) return;

    /* Load the secondary table to find count and delays for this index.
     * Assembly: MAP_DATA_PALETTE_ANIM_POINTER_TABLE[anim_index-1] →
     * MAP_DATA_PALETTE_ANIM_SECONDARY_TABLE_ENTRY_N → {ptr, count, delays[]}. */
    const uint8_t *meta = ASSET_DATA(ASSET_MAPS_ANIM_PAL_META_TABLE_BIN);
    size_t meta_size = ASSET_SIZE(ASSET_MAPS_ANIM_PAL_META_TABLE_BIN);
    if (!meta || meta_size < 5) {
        return;
    }

    /* Walk the variable-length entries to find entry (anim_index - 1).
     * Each entry: 4 bytes ptr + 1 byte count + count bytes delays. */
    const uint8_t *p = meta;
    const uint8_t *end = meta + meta_size;
    for (int i = 0; i < (int)(anim_index - 1); i++) {
        if (p + 5 > end) { return; }
        uint8_t cnt = p[4];
        p += 5 + cnt;
    }
    if (p + 5 > end) { return; }

    uint8_t count = p[4];
    if (count == 0 || count > 9 || p + 5 + count > end) {
        return;
    }

    /* Clear all delays, then fill from metadata */
    for (int i = 0; i < 9; i++)
        ml.overworld_palette_anim.delays[i] = 0;
    for (int i = 0; i < count; i++)
        ml.overworld_palette_anim.delays[i] = p[5 + i];

    /* Load compressed palette animation data.
     * Assembly: dereferences the ptr field to find compressed data.
     * C port: loads from maps/anim_pal/{index}.pal.lzhal. */
    size_t decomp_size = load_and_decompress(ASSET_MAPS_ANIM_PAL(anim_index),
                                             ml.animated_map_palette_buffer,
                                             sizeof(ml.animated_map_palette_buffer));
    if (decomp_size == 0) return;

    /* Initialize animation state.
     * Assembly: timer = delays[0], index = 1, MAP_PALETTE_ANIMATION_LOADED = 1. */
    ml.overworld_palette_anim.timer = ml.overworld_palette_anim.delays[0];
    ml.overworld_palette_anim.index = 1;
    ml.map_palette_animation_loaded = 1;
}

/* --- Sprite palette lighting adjustment ---
 *
 * Port of:
 *   PREPARE_AVERAGE_FOR_SPRITE_PALETTES  — asm/overworld/prepare_average_for_sprite_palettes.asm
 *   ADJUST_SPRITE_PALETTES_BY_AVERAGE    — asm/overworld/adjust_sprite_palettes_by_average.asm
 *   LOAD_SPECIAL_SPRITE_PALETTE          — asm/overworld/load_special_sprite_palette.asm
 *   GET_COLOUR_AVERAGE                   — asm/system/get_colour_average.asm
 *
 * Also uses ADJUST_SINGLE_COLOUR (already ported in overworld.c).
 *
 * When loading a map, sprite ert.palettes are loaded from fixed asset files
 * at full brightness.  The assembly then
 * adjusts them to match the map's lighting by computing the ratio between
 * the current map palette's average color and a reference palette's average,
 * then scaling every sprite palette color by that ratio.
 */

/* Saved baseline averages (scaled ×8) from the reference palette. */
/* saved_colour_average_red/green/blue now in MapLoaderState ml. */

/* Port of GET_COLOUR_AVERAGE (asm/system/get_colour_average.asm).
 * Computes average R, G, B over an array of BGR555 colors,
 * skipping transparent entries (& 0x7FFF == 0).
 * Results are scaled ×8 for 3 bits of fractional precision. */
static void get_colour_average(const uint16_t *colors, int num_colors,
                               uint16_t *avg_r, uint16_t *avg_g,
                               uint16_t *avg_b) {
    uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
    int count = 0;

    for (int i = 0; i < num_colors; i++) {
        uint16_t c = colors[i];
        if ((c & 0x7FFF) == 0) continue;
        sum_r += c & 0x1F;
        sum_g += (c >> 5) & 0x1F;
        sum_b += (c >> 10) & 0x1F;
        count++;
    }

    if (count == 0) {
        *avg_r = *avg_g = *avg_b = 0;
        return;
    }

    *avg_r = (uint16_t)((sum_r * 8) / count);
    *avg_g = (uint16_t)((sum_g * 8) / count);
    *avg_b = (uint16_t)((sum_b * 8) / count);
}

/* Port of PREPARE_AVERAGE_FOR_SPRITE_PALETTES
 * (asm/overworld/prepare_average_for_sprite_palettes.asm).
 *
 * Loads the reference palette (MAP_PALETTE_PTR_TABLE[1] = tileset_combo 1)
 * into ert.palettes[32..127] and computes baseline color averages.
 * The caller will overwrite ert.palettes[32..127] with the actual map palette
 * afterwards, so this write is temporary. */
static void prepare_average_for_sprite_palettes(void) {
    const uint8_t *pal_data = ASSET_DATA(ASSET_MAPS_PALETTES(1));
    size_t pal_size = ASSET_SIZE(ASSET_MAPS_PALETTES(1));
    if (!pal_data || pal_size < BPP4PALETTE_SIZE * 6) {
        ml.saved_colour_average_red = 0;
        ml.saved_colour_average_green = 0;
        ml.saved_colour_average_blue = 0;
        return;
    }

    /* Copy first 6 sub-ert.palettes (192 bytes) into ert.palettes[32..127]. */
    memcpy(&ert.palettes[32], pal_data, BPP4PALETTE_SIZE * 6);

    get_colour_average(&ert.palettes[32], 96,
                       &ml.saved_colour_average_red,
                       &ml.saved_colour_average_green,
                       &ml.saved_colour_average_blue);
}

/* Port of ADJUST_SPRITE_PALETTES_BY_AVERAGE
 * (asm/overworld/adjust_sprite_palettes_by_average.asm).
 *
 * Computes the ratio between the current map palette's average color
 * and the saved reference average, then scales every sprite palette
 * color (ert.palettes[128..255]) by that ratio.  Only darkens — if any
 * channel ratio exceeds 1.0, the adjustment is skipped entirely. */
static void adjust_sprite_palettes_by_average(void) {
    uint16_t avg_r, avg_g, avg_b;
    get_colour_average(&ert.palettes[32], 96, &avg_r, &avg_g, &avg_b);

    if (ml.saved_colour_average_red == 0 || ml.saved_colour_average_green == 0 ||
        ml.saved_colour_average_blue == 0)
        return;

    /* 8.8 fixed-point ratios: 256 = 1.0 */
    uint16_t ratio_r = (uint16_t)((avg_r * 256) / ml.saved_colour_average_red);
    uint16_t ratio_g = (uint16_t)((avg_g * 256) / ml.saved_colour_average_green);
    uint16_t ratio_b = (uint16_t)((avg_b * 256) / ml.saved_colour_average_blue);

    /* Combined ratio for greyscale colors */
    uint16_t ratio_combined = (ratio_r + ratio_g + ratio_b) / 3;

    /* Only darken, never brighten */
    if (ratio_r > 256 || ratio_g > 256 || ratio_b > 256)
        return;

    for (int i = 128; i < 256; i++) {
        uint16_t c = ert.palettes[i];
        uint16_t r = c & 0x1F;
        uint16_t g = (c >> 5) & 0x1F;
        uint16_t b = (c >> 10) & 0x1F;

        uint16_t orig_r = r, orig_g = g, orig_b = b;
        uint16_t new_r, new_g, new_b;

        if (r == g && g == b) {
            /* Greyscale: use combined ratio for all channels */
            new_r = (r * ratio_combined) >> 8;
            new_g = (g * ratio_combined) >> 8;
            new_b = (b * ratio_combined) >> 8;
        } else {
            new_r = (r * ratio_r) >> 8;
            new_g = (g * ratio_g) >> 8;
            new_b = (b * ratio_b) >> 8;
        }

        if (new_r > 31) new_r = 31;
        if (new_g > 31) new_g = 31;
        if (new_b > 31) new_b = 31;

        /* Smooth: limit each channel's change to ±6 */
        new_r = adjust_single_colour(orig_r, new_r);
        new_g = adjust_single_colour(orig_g, new_g);
        new_b = adjust_single_colour(orig_b, new_b);

        ert.palettes[i] = (new_b << 10) | (new_g << 5) | new_r;
    }
}

/* Port of LOAD_SPECIAL_SPRITE_PALETTE
 * (asm/overworld/load_special_sprite_palette.asm).
 *
 * Checks ert.palettes[64] (sub-palette 4, color 0 — normally transparent).
 * If non-zero, treats it as a sub-palette index and copies that
 * sub-palette's 16 colors into sprite sub-palette 4 (ert.palettes[192..207]). */
static void load_special_sprite_palette(void) {
    uint16_t pal_index = ert.palettes[64];
    if (pal_index == 0) return;

    uint16_t *src = &ert.palettes[pal_index * 16];
    memcpy(&ert.palettes[192], src, 16 * sizeof(uint16_t));
}

/* --- LOAD_MAP_AT_SECTOR ---
 * Port of asm/overworld/load_map_at_sector.asm.
 *
 * sector_x: X sector index (0-31)
 * sector_y: Y sector index (0-79) */
void load_map_at_sector(uint16_t sector_x, uint16_t sector_y) {
    if (!tilesetpalette_data || !tileset_table_data) {
        fprintf(stderr, "map_loader: data tables not loaded\n");
        return;
    }

    /* Check for teleport destination override (asm lines 15-30) */
    if (ow.current_teleport_destination_x | ow.current_teleport_destination_y) {
        sector_x = ow.current_teleport_destination_x >> 5;
        sector_y = ow.current_teleport_destination_y >> 4;
    }

    /* Look up sector data */
    size_t sector_index = (size_t)sector_y * 32 + sector_x;
    if (sector_index >= tilesetpalette_size) {
        fprintf(stderr, "map_loader: sector (%d, %d) out of range (size=%zu)\n",
                sector_x, sector_y, tilesetpalette_size);
        return;
    }

    uint8_t sector_byte = tilesetpalette_data[sector_index];
    uint16_t palette_index = sector_byte & 0x07;
    uint16_t tileset_combo = sector_byte >> 3;
    uint16_t tileset_id = get_tileset_id(tileset_combo);

    /* Load tile arrangement data (always reload — different tilesets may share IDs). */
    arrangement_size = load_and_decompress(ASSET_MAPS_ARRANGEMENTS(tileset_id),
                                           arrangement_buffer, SHARED_SCRATCH_SIZE);
    arrangement_loaded = (arrangement_size > 0);

    /* Load collision pointer array for this tileset (unconditional).
     * Port of load_map_at_sector.asm line 73: JSR LOAD_TILE_COLLISION.
     * Must be called before LOAD_MAP_BLOCK_EVENT_CHANGES so that event-driven
     * block replacements modify fresh collision data, not stale data. */
    load_tile_collision(tileset_id);

    /* Apply event-triggered tile changes (always, even if tileset unchanged).
     * Port of load_map_at_sector.asm line 76: JSL LOAD_MAP_BLOCK_EVENT_CHANGES.
     * Called unconditionally because event flags can change between loads. */
    load_map_block_event_changes(tileset_id);

    /* Load tileset GFX if changed.
     * Assembly decompresses into BUFFER then DMA's the first 0x7000 bytes
     * to VRAM $0000. We decompress directly to ppu.vram — the first 0x7000
     * bytes land at the correct position, and any overflow into higher VRAM
     * is harmless (overwritten by tilemaps). Intentional divergence from
     * assembly to avoid ert.buffer dependency. */
    if (tileset_combo != ml.loaded_tileset_combo) {
        load_and_decompress(ASSET_MAPS_GFX(tileset_id), ppu.vram, sizeof(ppu.vram));
        /* Clear collision grid when switching tilesets. The assembly's per-row/column
         * streaming naturally leaves out-of-range cells untouched, but the C port's
         * skip-out-of-tileset logic means stale data from the previous tileset could
         * persist in cells not covered by the new tileset's fill range. Zeroing the
         * grid ensures no stale collision flags (e.g., outdoor 0x80) bleed through. */
        memset(ml.loaded_collision_tiles, 0, sizeof(ml.loaded_collision_tiles));
        ml.loaded_tileset_combo = tileset_combo;
    }

    /* Assembly line 77: compute baseline color averages from the reference
     * palette (tileset_combo=1) before loading the actual map palette.
     * This temporarily writes to ert.palettes[32..127]. */
    prepare_average_for_sprite_palettes();

    /* Load palette: file selected by tileset_combo, slice by palette_index.
     * Uses overworld version with event flag override checking
     * (port of LOAD_MAP_PAL, not LOAD_MAP_PALETTE). */
    load_map_palette_overworld(tileset_combo, palette_index);
    ml.loaded_palette_index = palette_index;

    /* Load sprite ert.palettes into sub-ert.palettes 8-15 (OBJ ert.palettes).
     * Port of LOAD_MAP_AT_SECTOR's MEMCPY16 from SPRITE_GROUP_PALETTES.
     * Assembly: copies 8 × 32 bytes from SPRITE_GROUP_PALETTES to PALETTES+BPP4PALETTE_SIZE*8. */
    reload_sprite_group_palettes();

    /* Assembly line 115: adjust sprite ert.palettes to match map lighting. */
    adjust_sprite_palettes_by_average();

    /* Assembly line 116: apply map-specific sprite palette overrides. */
    load_special_sprite_palette();

    /* Load overlay sprite graphics and init entity overlay state
     * (assembly line 119: JSL LOAD_OVERLAY_SPRITES) */
    load_overlay_sprites();

    /* Load tileset animation (assembly line 120: JSR LOAD_TILESET_ANIM) */
    load_tileset_anim(tileset_id);

    /* Load palette animation (assembly line 121: JSR LOAD_PALETTE_ANIM) */
    load_palette_anim();

    /* Assembly lines 130-133: restore window palette and disable palette sync.
     * LOAD_CHARACTER_WINDOW_PALETTE ensures ert.palettes[0..31] always have correct
     * window colors.  SET_PALETTE_UPLOAD_MODE(0) prevents NMI palette DMA during
     * map loading; callers are responsible for re-enabling sync when ready. */
    if (!photograph_map_loading_mode) {
        load_character_window_palette();
        ert.palette_upload_mode = PALETTE_UPLOAD_NONE;
    }

    /* Save sub-ert.palettes 2–15 to MAP_PALETTE_BACKUP.
     * Assembly (load_map_at_sector.asm:136-147): copies 14 sub-ert.palettes
     * (BPP4PALETTE_SIZE*14 = 448 bytes) from PALETTES+64 (skipping sub-pals 0-1)
     * into MAP_PALETTE_BACKUP starting at offset 0.
     * RESTORE_MAP_PALETTE (C4978E) later updates with a full 512-byte copy
     * from PALETTES into MAP_PALETTE_BACKUP (saves current state to backup). */
    memcpy(ml.map_palette_backup, &ert.palettes[32], 14 * 16 * sizeof(uint16_t));

    /* Assembly lines 148-158: WIPE_PALETTES_ON_MAP_LOAD handling.
     * When set (by exit transition's fade-to-white path):
     * 1. Copy ert.palettes[] → ert.buffer[0..511] (save new palette as fade target)
     * 2. Fill ert.palettes[] with white (0xFFFF)
     * The enter transition will then fade from white → ert.buffer[] target. */
    if (dr.wipe_palettes_on_map_load) {
        copy_fade_buffer_to_palettes();
        memset(ert.palettes, 0xFF, 256 * sizeof(uint16_t));
        dr.wipe_palettes_on_map_load = 0;
    }

    /* Assembly lines 160-168: photograph mode palette clear.
     * Save current ert.palettes to fade ert.buffer, then black out sub-ert.palettes 1-15
     * (keep sub-palette 0 intact). The photograph system uses the fade ert.buffer
     * as the target for the post-photograph palette restore. */
    if (photograph_map_loading_mode) {
        copy_fade_buffer_to_palettes();
        memset(&ert.palettes[16], 0, 15 * 16 * sizeof(uint16_t));
    }

    /* Assembly lines 170-175 (@STORE_LOADED_STATE): re-enable palette sync
     * and store tracking variables.
     * The earlier SET_PALETTE_UPLOAD_MODE(0) at line 132 temporarily disabled
     * palette DMA during the backup phase; now restore to FULL so the next
     * NMI (or sync_palettes_to_cgram) pushes ert.palettes to CGRAM. */
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    ow.loaded_map_tile_combo = (int16_t)tileset_combo;
    ow.loaded_map_palette = (int16_t)palette_index;
}

/* --- LOAD_YOUR_SANCTUARY_LOCATION ---
 *
 * Port of:
 *   LOAD_YOUR_SANCTUARY_LOCATION      (asm/overworld/load_your_sanctuary_location.asm)
 *   LOAD_YOUR_SANCTUARY_LOCATION_DATA (asm/overworld/load_your_sanctuary_location_data.asm)
 *   PREPARE_YOUR_SANCTUARY_LOCATION_PALETTE_DATA
 *                                     (asm/overworld/prepare_your_sanctuary_location_palette_data.asm)
 *   PREPARE_YOUR_SANCTUARY_LOCATION_TILE_ARRANGEMENT_DATA
 *                                     (asm/overworld/prepare_your_sanctuary_location_tile_arrangement_data.asm)
 *   PREPARE_YOUR_SANCTUARY_LOCATION_TILESET_DATA
 *                                     (asm/overworld/prepare_your_sanctuary_location_tileset_data.asm)
 *
 * YOUR_SANCTUARY_TILE_DATA: 8 entries of two uint16 each.
 *   v0 = world tile X of sanctuary center (sector_x = v0 >> 5)
 *   v1 = world tile Y of sanctuary center (sector_y = v1 >> 4)
 *   Both encode the map sector used to look up tileset/palette.
 *
 * Results:
 *   ert.buffer[sanctuary_idx * 0x800]             : 30×32 tilemap (960 uint16 words = 0x780 bytes)
 *   ert.buffer[0x4000 + sanctuary_idx * 0x200]    : 8 BG sub-palettes (256 bytes)
 *   ppu.vram[0xC000 + tile_index * 32]            : packed 4bpp tile graphics for used tiles
 *   ml.next_your_sanctuary_location_tile_index    : updated for next sanctuary
 *   ml.total_your_sanctuary_loaded_tileset_tiles  : updated count
 */

/* YOUR_SANCTUARY_TILE_DATA table (from asm/data/unknown/C4DE78.asm).
 * 8 entries × 2 uint16: [world_tile_x, world_tile_y] for each sanctuary. */
static const uint16_t your_sanctuary_tile_data[8][2] = {
    { 0x0097, 0x0030 },  /* Sanctuary 0: Giant Step */
    { 0x0183, 0x03f5 },  /* Sanctuary 1: Lilliput Steps */
    { 0x0023, 0x01e4 },  /* Sanctuary 2: Milky Well */
    { 0x0058, 0x029c },  /* Sanctuary 3: Rainy Circle */
    { 0x01df, 0x0208 },  /* Sanctuary 4: Magnet Hill */
    { 0x01bd, 0x030b },  /* Sanctuary 5: Pink Cloud */
    { 0x034b, 0x0258 },  /* Sanctuary 6: Lumine Hall */
    { 0x02fe, 0x04cc },  /* Sanctuary 7: Fire Spring */
};

void load_your_sanctuary_location(uint16_t sanctuary_idx) {
    if (sanctuary_idx >= 8) return;
    if (!tilesetpalette_data || !tileset_table_data) return;

    uint16_t v0 = your_sanctuary_tile_data[sanctuary_idx][0];
    uint16_t v1 = your_sanctuary_tile_data[sanctuary_idx][1];

    /*
     * --- Step 1: Look up tileset_combo and palette from GLOBAL_MAP_TILESETPALETTE_DATA ---
     * sector_x = v0 >> 5  (tiles-per-sector = 32, so tile_x >> 5 = sector_x)
     * sector_y = v1 >> 4  (tiles-per-sector = 16 vertically, so tile_y >> 4 = sector_y)
     * Index = sector_y * 32 + sector_x
     */
    uint16_t sector_x = v0 >> 5;
    uint16_t sector_y = v1 >> 4;
    size_t tp_index = (size_t)sector_y * 32 + sector_x;
    if (tp_index >= tilesetpalette_size) {
        fprintf(stderr, "load_your_sanctuary_location: sector (%u,%u) out of range\n",
                sector_x, sector_y);
        return;
    }
    uint8_t tp_byte = tilesetpalette_data[tp_index];
    uint16_t palette_index = tp_byte & 0x07;
    uint16_t tileset_combo  = tp_byte >> 3;
    uint16_t tileset_id     = get_tileset_id(tileset_combo);

    /* Store LOADED_MAP_TILE_COMBO (used by PREPARE_YOUR_SANCTUARY_LOCATION_TILE_ARRANGEMENT_DATA
     * to match tiles to the correct tileset). */
    ow.loaded_map_tile_combo = (int16_t)tileset_combo;

    /*
     * --- Step 2: Load and save palette (PREPARE_YOUR_SANCTUARY_LOCATION_PALETTE_DATA) ---
     * Prepare sprite-palette averages, load map palette, adjust sprite palettes,
     * then copy 8 sub-palettes (256 bytes) from ert.palettes to
     * ert.buffer[0x4000 + sanctuary_idx * 0x200].
     */
    prepare_average_for_sprite_palettes();
    reload_sprite_group_palettes();
    load_map_palette_overworld(tileset_combo, palette_index);
    adjust_sprite_palettes_by_average();
    ert.palette_upload_mode = PALETTE_UPLOAD_NONE;

    /* Copy 8 BG sub-palettes (= BPP4PALETTE_SIZE * 8 = 256 bytes) to sanctuary palette slot.
     * Always uses slot 0 — sanctuaries are loaded on demand, not pre-cached.
     * Intentional divergence from assembly, which caches all 8 in BUFFER. */
    {
        size_t pal_dst_offset = BUF_SANCTUARY_PALETTES;
        /* BPP4PALETTE_SIZE * 8 = 256 bytes = 128 uint16 colors */
        memcpy(&ert.buffer[pal_dst_offset], ert.palettes,
               (size_t)BPP4PALETTE_SIZE * 8);
    }

    /*
     * --- Step 3: Decompress arrangement data directly into arrangement_buffer ---
     * Port of DECOMP(src=MAP_DATA_TILE_ARRANGEMENT_N, dst=BUFFER+$8000).
     * Intentional divergence: assembly stages through BUFFER+$8000 then copies
     * to the arrangement lookup buffer. We decompress directly, avoiding the
     * intermediate copy and freeing ert.buffer from sanctuary decomp usage.
     */
    arrangement_size = load_and_decompress(ASSET_MAPS_ARRANGEMENTS(tileset_id),
                                           arrangement_buffer,
                                           SHARED_SCRATCH_SIZE);
    arrangement_loaded = (arrangement_size > 0);
    if (!arrangement_loaded) {
        fprintf(stderr, "load_your_sanctuary_location: failed to load arrangement for tileset %u\n", tileset_id);
        return;
    }

    /*
     * --- Step 4: Build tilemap and mark used tiles
     *             (PREPARE_YOUR_SANCTUARY_LOCATION_TILE_ARRANGEMENT_DATA) ---
     *
     * Port of asm/overworld/prepare_your_sanctuary_location_tile_arrangement_data.asm.
     *
     * start_tile_x = v0 - 16   (leftmost tile of 32-wide sanctuary view)
     * start_tile_y = v1 - 14   (topmost tile of 30-tall sanctuary view)
     *
     * For each (row 0..29, col 0..31):
     *   world_tx = start_tile_x + col
     *   world_ty = start_tile_y + row
     *   block_id = get_block_id(world_tx >> 2, world_ty >> 2)
     *   arr_index = block_id * 16 + (world_ty & 3) * 4 + (world_tx & 3)
     *   tile_entry = arrangement_buffer[arr_index]  (16-bit tilemap word)
     *   Mark LOADED_MAP_BLOCKS[tile_entry & 0x3FF] = 0xFFFF (tile is used)
     *   Write tile_entry to tilemap at ert.buffer[sanctuary_idx * 0x800 + row * 64 + col * 2]
     *
     * LOADED_MAP_BLOCKS: 1024 uint16 entries tracking which arrangement tile
     * indices are referenced. Initialised to 0 at the start of each sanctuary.
     */
    uint16_t *loaded_map_blocks = (uint16_t *)(ert.buffer + BUF_SANCTUARY_MAP_BLOCKS);
    memset(loaded_map_blocks, 0, 1024 * sizeof(uint16_t));

    ml.your_sanctuary_loaded_tileset_tiles = 0;

    int16_t start_tile_x = (int16_t)v0 - 16;
    int16_t start_tile_y = (int16_t)v1 - 14;

    /* Tilemap destination: 30 rows × 32 cols × 2 bytes.
     * Always slot 0 — sanctuaries loaded on demand, not pre-cached. */
    size_t tilemap_base = BUF_SANCTUARY_TILEMAPS;

    /* Ensure chunk data is loaded (needed for get_block_id lookups) */
    load_chunks();

    for (int row = 0; row < 30; row++) {
        for (int col = 0; col < 32; col++) {
            int16_t world_tx = start_tile_x + col;
            int16_t world_ty = start_tile_y + row;

            /* Negative coordinates → no valid tile */
            if (world_tx < 0 || world_ty < 0) {
                size_t dst_off = tilemap_base + (size_t)(row * 32 + col) * 2;
                if (dst_off + 2 <= BUFFER_SIZE)
                    write_u16_le(&ert.buffer[dst_off], 0);
                continue;
            }

            uint16_t block_x = (uint16_t)world_tx >> 2;
            uint16_t block_y = (uint16_t)world_ty >> 2;
            uint8_t  sub_col = (uint16_t)world_tx & 3;
            uint8_t  sub_row = (uint16_t)world_ty & 3;

            /* REDIRECT_C0A156 = GET_MAP_BLOCK_CACHED(block_x, block_y) */
            uint16_t block_id = get_block_id(block_x, block_y);

            /* Verify this tile belongs to the sanctuary's tileset.
             * Assembly (prepare_your_sanctuary_location_tile_arrangement_data.asm lines 64-76):
             *   index = (block_y & ~3) * 8 + sector_x  → GLOBAL_MAP_TILESETPALETTE_DATA[index]
             *   tileset_combo = byte >> 3
             *   If tileset_combo != LOADED_MAP_TILE_COMBO: block_id = 0 (@NOT_MATCHING_TILESET)
             * When mismatched, block_id = 0 is used to look up block 0's arrangement. */
            {
                /* Compute per-tile sector coordinates:
                 * sector_x = world_tx >> 5,  sector_y = world_ty >> 4 */
                uint16_t tx_sector_x = (uint16_t)world_tx >> 5;
                uint16_t tx_sector_y = (uint16_t)world_ty >> 4;
                size_t tile_tp_idx = (size_t)tx_sector_y * 32 + tx_sector_x;
                if (tile_tp_idx < tilesetpalette_size) {
                    uint8_t  tile_tp    = tilesetpalette_data[tile_tp_idx];
                    uint16_t tile_combo = tile_tp >> 3;
                    if (tile_combo != (uint16_t)ow.loaded_map_tile_combo) {
                        /* @NOT_MATCHING_TILESET: use block_id = 0 */
                        block_id = 0;
                    }
                }
            }

            uint16_t tile_entry = get_arrangement_tile(block_id, sub_col, sub_row);

            /* Mark this tile as used in loaded_map_blocks */
            {
                uint16_t tile_idx = tile_entry & 0x03FF;
                if (tile_idx < 1024)
                    loaded_map_blocks[tile_idx] = 0xFFFF;
            }

            size_t dst_off = tilemap_base + (size_t)(row * 32 + col) * 2;
            if (dst_off + 2 <= BUFFER_SIZE)
                write_u16_le(&ert.buffer[dst_off], tile_entry);
        }
    }

    /*
     * --- Step 5: Decompress tileset graphics into decomp_staging ---
     * Arrangement data was consumed in step 4, so the shared scratch is free.
     * Intentional divergence: assembly uses BUFFER+$8000; we use decomp_staging
     * (shared_scratch.decomp) to avoid ert.buffer dependency entirely.
     */
    size_t gfx_size = load_and_decompress(ASSET_MAPS_GFX(tileset_id),
                                          decomp_staging,
                                          SHARED_SCRATCH_SIZE);
    if (gfx_size == 0) {
        fprintf(stderr, "load_your_sanctuary_location: failed to load gfx for tileset %u\n", tileset_id);
        return;
    }

    /*
     * --- Step 6: Copy used tiles to VRAM and remap tilemap
     *             (PREPARE_YOUR_SANCTUARY_LOCATION_TILESET_DATA) ---
     *
     * For each tile_idx 0..1023:
     *   If loaded_map_blocks[tile_idx] != 0 (used):
     *     - src = decomp_staging[tile_idx * 32]  (32 bytes = 8x8 4bpp tile)
     *     - dst VRAM word addr = 0x6000 + next_tile_index * 16  (16 words = 32 bytes)
     *     - dst VRAM byte addr = 0xC000 + next_tile_index * 32
     *     - loaded_map_blocks[tile_idx] = next_tile_index (remap index)
     *     - next_tile_index++
     *
     * Then remap the tilemap entries in ert.buffer[tilemap_base]:
     *   new_entry = (old_entry & 0xFC00) | loaded_map_blocks[old_entry & 0x3FF]
     */
    for (int tile_idx = 0; tile_idx < 1024; tile_idx++) {
        if (!loaded_map_blocks[tile_idx]) continue;

        /* Source tile in decompressed gfx buffer */
        size_t src_off = (size_t)tile_idx * 32;
        if (src_off + 32 > gfx_size) continue;

        /* Destination in VRAM: word addr = 0x6000 + next_tile_index * 16
         * Byte addr = (0x6000 + next_tile_index * 16) * 2
         *           = 0xC000 + next_tile_index * 32 */
        uint16_t next_idx = ml.next_your_sanctuary_location_tile_index;
        uint32_t vram_byte = (uint32_t)0xC000 + (uint32_t)next_idx * 32;
        if (vram_byte + 32 <= sizeof(ppu.vram)) {
            memcpy(&ppu.vram[vram_byte], &decomp_staging[src_off], 32);
        }

        /* Record the new VRAM tile index in loaded_map_blocks for remapping */
        loaded_map_blocks[tile_idx] = next_idx;
        ml.next_your_sanctuary_location_tile_index++;
        ml.your_sanctuary_loaded_tileset_tiles++;
    }

    /* Remap tilemap entries: replace original tile_idx with VRAM tile index */
    for (int i = 0; i < 30 * 32; i++) {
        size_t dst_off = tilemap_base + (size_t)i * 2;
        if (dst_off + 2 > BUFFER_SIZE) break;
        uint16_t entry    = read_u16_le(&ert.buffer[dst_off]);
        uint16_t tile_idx = entry & 0x03FF;
        uint16_t new_entry = (entry & 0xFC00)
                           | (tile_idx < 1024 ? loaded_map_blocks[tile_idx] : 0);
        write_u16_le(&ert.buffer[dst_off], new_entry);
    }

    /* Update totals (TOTAL_YOUR_SANCTUARY_LOADED_TILESET_TILES += YOUR_SANCTUARY_LOADED_TILESET_TILES) */
    ml.total_your_sanctuary_loaded_tileset_tiles += ml.your_sanctuary_loaded_tileset_tiles;

    /* Mark this sanctuary as loaded */
    ml.loaded_your_sanctuary_locations[sanctuary_idx] = 1;
}

/* --- SPAWN_NPCS_AT_SECTOR ---
 * Faithful port of asm/overworld/npc/spawn_npcs_at_sector.asm.
 *
 * NPC placement uses a different sector grid from tileset loading:
 *   40 columns × 32 rows, each sector = 256×256 pixels.
 *   Pointer table is column-major: byte_offset = sector_x * 64 + sector_y * 2.
 *
 * Key checks ported from the assembly:
 *   1. Tileset check (lines 76-121): verify NPC's tile is in loaded tileset
 *   2. Duplicate check (lines 122-125): skip if NPC entity already exists
 *   3. Screen bounds (lines 144-199): spawn only within [-65, 320) of camera
 *   4. Appearance/event flag (lines 236-253): visibility based on flags
 *   5. No animation_frame override: scripts set it via SET_ANIMATION opcode */
void spawn_npcs_at_sector(uint16_t sector_x, uint16_t sector_y) {
    if (!ow.npc_spawns_enabled) return;
    if (sector_x >= 32) return;   /* C0222B.asm lines 24-27: first param (x) < 32 */
    if (sector_y >= 40) return;   /* C0222B.asm lines 29-32: second param (y) < 40 */
    if (!sprite_placement_ptr_table || !sprite_placement_table || !npc_config_table) return;

    /* Row-major lookup: byte_offset = sector_y * 64 + sector_x * 2.
     * Port of C0222B.asm lines 34-42. */
    uint32_t ptr_idx = ((uint32_t)sector_y * 32 + sector_x) * 2;
    if (ptr_idx + 2 > sprite_placement_ptr_table_size) return;

    uint16_t rom_ptr = read_u16_le(sprite_placement_ptr_table + ptr_idx);
    if (rom_ptr == 0) return;  /* No NPCs in this sector */

    /* Convert within-bank ROM pointer to ert.buffer offset */
    if (rom_ptr < SPRITE_PLACEMENT_TABLE_LOWORD) return;
    uint32_t buf_off = rom_ptr - SPRITE_PLACEMENT_TABLE_LOWORD;
    if (buf_off + 2 > sprite_placement_table_size) return;

    /* First word = NPC count for this sector (C0222B.asm lines 47-48) */
    uint16_t count = read_u16_le(sprite_placement_table + buf_off);
    buf_off += 2;

    for (uint16_t i = 0; i < count; i++) {
        if (buf_off + 4 > sprite_placement_table_size) break;

        /* sprite_placement: id(2), x_coord(1), y_coord(1)
         * C0222B.asm lines 56-75.
         * IMPORTANT: Despite the field names, the assembly uses y_coord (byte 3)
         * for the horizontal world position and x_coord (byte 2) for vertical.
         * See lines 126-147: world_x = first_param*256 + y_coord,
         * world_y = second_param*256 + x_coord, then world_x is compared to BG1_X_POS. */
        uint16_t npc_id = read_u16_le(sprite_placement_table + buf_off);
        uint8_t local_x = sprite_placement_table[buf_off + 3];  /* y_coord field → horizontal */
        uint8_t local_y = sprite_placement_table[buf_off + 2];  /* x_coord field → vertical */
        buf_off += 4;

        /* --- Tileset check (C0222B.asm lines 76-121) ---
         * Compute the NPC's position in the tilesetpalette grid and verify
         * it matches the currently loaded tileset. This prevents spawning
         * NPCs from neighboring map areas that have a different tileset.
         *
         * Assembly trace (with corrected field mapping):
         *   val_horiz = sector_x * 32 + local_x / 8    (@LOCAL05)
         *   val_vert  = sector_y * 32 + local_y / 8    (@VIRTUAL02)
         *   tp_sx = val_horiz >> 5  ≈ map_sector_x     (PUSHed, then PLY)
         *   tp_sy = val_vert >> 4   ≈ map_sector_y     (OPTIMIZED_MULT 32)
         *   tp_index = tp_sy * 32 + tp_sx               (row-major)
         *   tileset = tilesetpalette[tp_index] >> 3
         */
        {
            uint16_t val_horiz = (uint16_t)(sector_x * 32 + local_x / 8);
            uint16_t val_vert  = (uint16_t)(sector_y * 32 + local_y / 8);
            uint16_t tp_sx = val_horiz >> 5;   /* horizontal / 32 = map_sector_x */
            uint16_t tp_sy = val_vert >> 4;    /* vertical / 16 = map_sector_y */
            uint32_t tp_index = (uint32_t)tp_sy * 32 + tp_sx;
            if (tp_index >= tilesetpalette_size) {
                continue;
            }
            uint8_t tp_byte = tilesetpalette_data[tp_index];
            uint16_t npc_tileset_combo = tp_byte >> 3;
            if (ml.loaded_tileset_combo < 0 ||
                npc_tileset_combo != (uint16_t)ml.loaded_tileset_combo) {
                continue;
            }
        }

        /* --- Duplicate check (C0222B.asm lines 122-125) ---
         * Skip if this NPC already exists as an entity. */
        if (find_entity_by_npc_id(npc_id) >= 0) {
            continue;
        }

        /* --- World coordinates (C0222B.asm lines 126-143) ---
         * world = sector * 256 + local_offset */
        int16_t world_x = (int16_t)(sector_x * 256 + local_x);
        int16_t world_y = (int16_t)(sector_y * 256 + local_y);

        /* --- Screen bounds check (C0222B.asm lines 144-199) --- */
        {
            int16_t screen_rel_x = (int16_t)((uint16_t)world_x - ppu.bg_hofs[0]);
            int16_t screen_rel_y = (int16_t)((uint16_t)world_y - ppu.bg_vofs[0]);

            /* On-screen pop-in prevention (C0222B.asm lines 158-176):
             * When NPC_SPAWNS_ENABLED != 1, skip NPCs currently visible on
             * screen to prevent visible pop-in. Attract mode (enabled=1) skips
             * this check. */
            if (ow.npc_spawns_enabled != 1) {
                bool on_screen_x = (uint16_t)screen_rel_x < VIEWPORT_WIDTH;
                bool on_screen_y = (uint16_t)screen_rel_y < VIEWPORT_HEIGHT;
                if (on_screen_x && on_screen_y) {
                    continue;
                }
            }

            /* Extended bounds check (C0222B.asm lines 178-199):
             * Assembly: LDA #.LOWORD(-64); CLC; SBC screen_rel; JUMPGTS @SKIP_NPC
             * CLC;SBC gives -64 - screen_rel - 1 = -(screen_rel + 65).
             * JUMPGTS = strictly signed > 0, so skips when -(screen_rel+65) > 0
             * i.e., when screen_rel < -65 (= screen_rel <= -66).
             * Value -65 gives result 0 which does NOT trigger JUMPGTS (not strictly > 0).
             * Only spawn NPCs within (-65, 320) i.e., screen_rel must be >= -65. */
            if (screen_rel_x < -65 || screen_rel_x >= (VIEWPORT_WIDTH + 64)) {
                continue;
            }
            if (screen_rel_y < -65 || screen_rel_y >= (VIEWPORT_HEIGHT + 96)) {
                continue;
            }
        }

        /* --- NPC config lookup (C0222B.asm lines 200-206) --- */
        const NpcConfig *cfg = get_npc_config(npc_id);
        if (!cfg) continue;

        /* --- Appearance/event flag check (C0222B.asm lines 236-253) ---
         * appearance_style: 0=always, 1=show_if_flag_off, 2=show_if_flag_on.
         *   DEC / DEC / EOR flag_value / AND #1 / BEQ skip */
        uint8_t appearance = cfg->appearance_style;
        if (appearance != 0) {
            uint16_t event_flag_id = cfg->event_flag;
            bool flag_value = event_flag_get(event_flag_id);
            if (!(((appearance - 2) ^ flag_value) & 1)) {
                continue;
            }
        }

        uint16_t sprite_id = cfg->sprite;
        uint16_t script_id = cfg->event_script;
        uint8_t direction = cfg->direction;

        /* --- Create entity (C0222B.asm lines 293-310) --- */
        int16_t created_slot = create_entity(sprite_id, script_id, -1,
                                             world_x, world_y);

        /* --- Set direction and NPC ID (C0222B.asm lines 333-348) --- */
        if (created_slot >= 0) {
            int16_t ent_offset = created_slot;
            entities.directions[ent_offset] = direction;
            entities.npc_ids[ent_offset] = npc_id;
        }
    }
}

/* --- LOAD_MAP_AT_POSITION ---
 * Port of asm/overworld/load_map_at_position.asm.
 *
 * x_pixels, y_pixels: world position in pixels. */
void load_map_at_position(uint16_t x_pixels, uint16_t y_pixels) {
    if (!map_loader_init()) return;

    /* Assembly line 15: CLEAR_ALL_ENEMIES */
    clear_all_enemies();

    /* Convert pixel coordinates to tile coordinates (8 pixels per tile) */
    uint16_t x_tile = x_pixels >> 3;
    uint16_t y_tile = y_pixels >> 3;

    /* Convert to sector coordinates.
     * From assembly: x >> 3 >> 5 = x >> 8 (256 pixels per sector = 32 tiles)
     *                y >> 3 >> 4 = y >> 7 (128 pixels per sector = 16 tiles) */
    uint16_t sector_x = x_tile >> 5;
    uint16_t sector_y = y_tile >> 4;

    /* Load sector data (tileset GFX, arrangement, palette) */
    load_map_at_sector(sector_x, sector_y);

    /* Set up VRAM display settings — assembly skips in photograph mode */
    if (!photograph_map_loading_mode)
        overworld_setup_vram();

    /* Fill tilemaps from chunk data + arrangement table.
     * Center the view on the player position. */
    int16_t view_x_tile = (int16_t)x_tile - (VIEWPORT_CENTER_X / 8);
    int16_t view_y_tile = (int16_t)y_tile - (VIEWPORT_CENTER_Y / 8);
    fill_tilemaps(view_x_tile, view_y_tile);
    fill_collision_tiles(view_x_tile, view_y_tile);

    /* Assembly lines 118-122: Wait for any active fade to complete
     * before setting up VRAM/palette display settings. */
    wait_for_fade_complete();

    /* Set BG scroll positions to center on player BEFORE spawning NPCs.
     * spawn_npcs_at_sector's screen bounds check reads ppu.bg_hofs/bg_vofs
     * to filter NPCs outside [-65, VIEWPORT_WIDTH+64) pixels of camera.
     * Screen center offset: VIEWPORT_CENTER pixels. */
    ppu.bg_hofs[0] = x_pixels - VIEWPORT_CENTER_X;
    ppu.bg_hofs[1] = x_pixels - VIEWPORT_CENTER_X;
    ppu.bg_vofs[0] = y_pixels - VIEWPORT_CENTER_Y;
    ppu.bg_vofs[1] = y_pixels - VIEWPORT_CENTER_Y;

    /* Normalize ow.npc_spawns_enabled to 1 before initial spawn.
     * Assembly lines 130-133: if NPC_SPAWNS_ENABLED != 0, set to 1.
     * This enables the "attract mode" path in spawn_npcs_at_sector
     * (ow.npc_spawns_enabled == 1 skips pop-in prevention). */
    if (ow.npc_spawns_enabled) {
        ow.npc_spawns_enabled = 1;
    }

    /* Spawn NPCs in visible sectors.
     * NPC sectors are 256×256 pixels (32×32 tiles).
     * Grid is 32 columns (X, 0-31) × 40 rows (Y, 0-39).
     * Convert camera pixel position to NPC sector coordinates and
     * spawn in a 3×3 grid of sectors around the camera. */
    {
        uint16_t npc_sx = x_pixels >> 8;
        uint16_t npc_sy = y_pixels >> 8;
        uint16_t sx_start = (npc_sx > 0) ? npc_sx - 1 : 0;
        uint16_t sy_start = (npc_sy > 0) ? npc_sy - 1 : 0;
        uint16_t sx_end = (npc_sx + 1 < 32) ? npc_sx + 1 : 31;
        uint16_t sy_end = (npc_sy + 1 < 40) ? npc_sy + 1 : 39;
        for (uint16_t sy = sy_start; sy <= sy_end; sy++) {
            for (uint16_t sx = sx_start; sx <= sx_end; sx++) {
                spawn_npcs_at_sector(sx, sy);
            }
        }
    }

    /* Set ow.npc_spawns_enabled to -1 after initial spawn.
     * Assembly lines 183-186: when NPC_SPAWNS_ENABLED was non-zero, set it
     * to $FFFF (-1). This enables the on-screen pop-in prevention check
     * in spawn_npcs_at_sector during subsequent scroll-based spawning. */
    if (ow.npc_spawns_enabled) {
        ow.npc_spawns_enabled = 0xFF;
    }

    /* Assembly lines 165-182: Spawn enemies across the initial visible area.
     * Calls SPAWN_HORIZONTAL for each row from -8 to (VIEWPORT_HEIGHT/8+12),
     * covering the full vertical extent of the screen plus margin. */
    {
        int16_t init_screen_x = (int16_t)(x_tile - (VIEWPORT_CENTER_X / 8));
        int16_t init_screen_y = (int16_t)(y_tile - (VIEWPORT_CENTER_Y / 8));
        for (int row = -8; row < (VIEWPORT_HEIGHT / 8 + 12); row++) {
            spawn_horizontal((uint16_t)(init_screen_x - 8),
                             (uint16_t)(init_screen_y + row));
        }
    }

    /* Initialize scroll tracking state.
     * Assembly lines 188-191: SCREEN_LEFT_X = x_tile - VIEWPORT_CENTER/8,
     * SCREEN_TOP_Y = y_tile - VIEWPORT_CENTER/8. */
    ml.screen_left_x = (int16_t)(x_tile - (VIEWPORT_CENTER_X / 8));
    ml.screen_top_y = (int16_t)(y_tile - (VIEWPORT_CENTER_Y / 8));

    /* Enable layers: BG1 + BG2 + BG3 + OBJ (assembly skips in photograph mode) */
    if (!photograph_map_loading_mode)
        ppu.tm = 0x17;

}

/* --- RELOAD_MAP_AT_POSITION ---
 * Port of asm/overworld/reload_map_at_position.asm.
 *
 * Like load_map_at_position but does NOT clear entities, set up VRAM mode,
 * spawn NPCs, write TM, or touch NPC_SPAWNS_ENABLED.
 * Used after battles to reload map tiles without despawning roaming enemies. */
void reload_map_at_position(uint16_t x_pixels, uint16_t y_pixels) {
    if (!map_loader_init()) return;

    /* Assembly lines 12-15: set screen position */
    /* (SCREEN_X/Y_PIXELS — used internally for BG scroll calc below) */

    /* Assembly lines 16-25: convert to tile coordinates */
    uint16_t x_tile = x_pixels >> 3;
    uint16_t y_tile = y_pixels >> 3;

    /* Assembly lines 26-28: invalidate palette and tileset caches */
    ow.loaded_map_palette = -1;
    ow.loaded_map_tile_combo = -1;

    /* Assembly lines 29-41: convert to sector coords, load sector */
    uint16_t sector_x = x_tile >> 5;
    uint16_t sector_y = y_tile >> 4;
    load_map_at_sector(sector_x, sector_y);

    /* Assembly lines 42-59: compute view offsets */
    int16_t view_x_tile = (int16_t)x_tile - (VIEWPORT_CENTER_X / 8);
    int16_t view_y_tile = (int16_t)y_tile - (VIEWPORT_CENTER_Y / 8);

    /* Assembly lines 60-110: invalidate streaming cache, fill tilemaps + collision */
    fill_tilemaps(view_x_tile, view_y_tile);
    fill_collision_tiles(view_x_tile, view_y_tile);

    /* Assembly lines 128-132: wait for any active fade to complete */
    wait_for_fade_complete();

    /* Assembly lines 133-142: set BG scroll positions */
    ppu.bg_hofs[0] = x_pixels - VIEWPORT_CENTER_X;
    ppu.bg_hofs[1] = x_pixels - VIEWPORT_CENTER_X;
    ppu.bg_vofs[0] = y_pixels - VIEWPORT_CENTER_Y;
    ppu.bg_vofs[1] = y_pixels - VIEWPORT_CENTER_Y;

    /* Assembly lines 143-146: set scroll tracking state */
    ml.screen_left_x = view_x_tile;
    ml.screen_top_y = view_y_tile;
}

/* --- LOAD_INITIAL_MAP_DATA ---
 * Port of asm/overworld/map/load_initial_map_data.asm.
 *
 * Invalidates column/row streaming state and refills the full 64×64
 * tilemap and collision grids from the current camera position.
 *
 * Assembly calculates view start as (BG_SCROLL - 128) / 8, which
 * centers the 64-tile grid on the player. */
void load_initial_map_data(void) {
    /* BG1_X_POS = ppu.bg_hofs[0], BG1_Y_POS = ppu.bg_vofs[0] */
    uint16_t view_x_tile = (uint16_t)((int16_t)ppu.bg_hofs[0] - VIEWPORT_CENTER_X) >> 3;
    uint16_t view_y_tile = (uint16_t)((int16_t)ppu.bg_vofs[0] - VIEWPORT_CENTER_Y) >> 3;
    fill_tilemaps(view_x_tile, view_y_tile);
    fill_collision_tiles(view_x_tile, view_y_tile);
}

/* --- SPAWN_NPCS_IN_COLUMN ---
 * Port of asm/overworld/npc/spawn_npcs_in_column.asm.
 *
 * Iterates tile positions along a vertical column and
 * calls spawn_npcs_at_sector() for each new NPC sector encountered.
 *
 * x_tile:       tile X position of the column
 * y_tile_start: tile Y position of the column top */
static void spawn_npcs_in_column(int16_t x_tile, int16_t y_tile_start) {
    if (!ow.npc_spawns_enabled) return;
    if (x_tile < 0) return;

    uint16_t npc_sector_x = (uint16_t)x_tile >> 5;
    uint16_t prev_sector_y = 0x8000;  /* sentinel: impossible sector */
    int16_t limit = y_tile_start + (VIEWPORT_HEIGHT / 8 + 4);

    for (int16_t y = y_tile_start; y != limit; y++) {
        if (y < 0) continue;
        uint16_t sector_y = (uint16_t)y >> 5;
        if (sector_y != prev_sector_y) {
            spawn_npcs_at_sector(npc_sector_x, sector_y);
            prev_sector_y = sector_y;
        }
    }
}

/* --- SPAWN_NPCS_IN_ROW ---
 * Port of asm/overworld/npc/spawn_npcs_in_row.asm.
 *
 * Iterates tile positions along a horizontal row and calls spawn_npcs_at_sector()
 * for each new NPC sector encountered.
 *
 * left_x_tile: leftmost tile X of the visible area
 * row_y_tile:  tile Y position of the row */
static void spawn_npcs_in_row(int16_t left_x_tile, int16_t row_y_tile) {
    if (!ow.npc_spawns_enabled) return;
    if (row_y_tile < 0) return;

    uint16_t npc_sector_y = (uint16_t)row_y_tile >> 5;
    uint16_t prev_sector_x = 0x8000;  /* sentinel */
    int16_t start_x = left_x_tile - 2;
    int16_t limit = left_x_tile + (VIEWPORT_WIDTH / 8 + 4);

    for (int16_t x = start_x; x != limit; x++) {
        if (x < 0) continue;
        uint16_t sector_x = (uint16_t)x >> 5;
        if (sector_x != prev_sector_x) {
            spawn_npcs_at_sector(sector_x, npc_sector_y);
            prev_sector_x = sector_x;
        }
    }
}

/* --- MAP_REFRESH_TILEMAPS ---
 * Port of REFRESH_MAP_AT_POSITION (asm/overworld/refresh_map_at_position.asm).
 *
 * Updates BG scroll registers and incrementally streams NPC spawns as the
 * camera scrolls. ml.screen_left_x/ml.screen_top_y track the current view origin
 * in tiles and are moved one tile at a time toward the target, spawning
 * NPCs for newly visible columns/rows at each step.
 *
 * The tilemap itself is refilled entirely via fill_tilemaps() (the assembly
 * streams individual columns/rows, but the full refill is functionally
 * equivalent for the C port).
 *
 * cam_x_pixels, cam_y_pixels: camera center in world pixels. */
void map_refresh_tilemaps(uint16_t cam_x_pixels, uint16_t cam_y_pixels) {
    /* Step 1: Compute BG scroll positions and update PPU registers.
     * Assembly lines 14-20: BG1/2_X/Y_POS = scroll params. */
    int16_t scroll_x = (int16_t)(cam_x_pixels - VIEWPORT_CENTER_X);
    int16_t scroll_y = (int16_t)(cam_y_pixels - VIEWPORT_CENTER_Y);
    ppu.bg_hofs[0] = (uint16_t)scroll_x;
    ppu.bg_hofs[1] = (uint16_t)scroll_x;
    ppu.bg_vofs[0] = (uint16_t)scroll_y;
    ppu.bg_vofs[1] = (uint16_t)scroll_y;

    /* Step 2: Compute target tile positions (signed right shift).
     * Assembly lines 21-54: @VIRTUAL04 = scroll_x >> 3, @VIRTUAL02 = scroll_y >> 3. */
    int16_t target_x_tile = scroll_x >> 3;
    int16_t target_y_tile = scroll_y >> 3;

    /* Step 3: X-axis scroll — move ml.screen_left_x toward target one tile
     * at a time, spawning NPCs in newly visible columns.
     * Assembly lines 55-142: @UNKNOWN3..@UNKNOWN5 loop. */
    while (ml.screen_left_x != target_x_tile) {
        if ((ml.screen_left_x - target_x_tile) < 0) {
            /* Scrolling RIGHT: SCREEN_LEFT_X < target.
             * Assembly lines 58-97. */
            ml.screen_left_x++;
            spawn_npcs_in_column(ml.screen_left_x + (VIEWPORT_WIDTH / 8 + 2 + VIEWPORT_PAD_LEFT / 8), target_y_tile - 1);
            /* Assembly lines 89-96: SPAWN_VERTICAL */
            spawn_vertical((uint16_t)(ml.screen_left_x + (VIEWPORT_WIDTH / 8 + 8)),
                           (uint16_t)(target_y_tile - 8));
        } else {
            /* Scrolling LEFT: SCREEN_LEFT_X > target.
             * Assembly lines 99-137. */
            ml.screen_left_x--;
            spawn_npcs_in_column(ml.screen_left_x - 3 - VIEWPORT_PAD_LEFT / 8, target_y_tile - 1);
            /* Assembly lines 130-137: SPAWN_VERTICAL */
            spawn_vertical((uint16_t)(ml.screen_left_x - 8),
                           (uint16_t)(target_y_tile - 8));
        }
    }

    /* Step 4: Y-axis scroll — move ml.screen_top_y toward target one tile
     * at a time, spawning NPCs in newly visible rows.
     * Assembly lines 144-233: @UNKNOWN7..@UNKNOWN9 loop. */
    while (ml.screen_top_y != target_y_tile) {
        if ((ml.screen_top_y - target_y_tile) < 0) {
            /* Scrolling DOWN: SCREEN_TOP_Y < target.
             * Assembly lines 147-188. */
            ml.screen_top_y++;
            spawn_npcs_in_row(target_x_tile, ml.screen_top_y + (VIEWPORT_HEIGHT / 8 + 1 + VIEWPORT_PAD_TOP / 8));
            /* Assembly lines 181-188: SPAWN_HORIZONTAL */
            spawn_horizontal((uint16_t)(target_x_tile - 8),
                             (uint16_t)(ml.screen_top_y + (VIEWPORT_HEIGHT / 8 + 8)));
        } else {
            /* Scrolling UP: SCREEN_TOP_Y > target.
             * Assembly lines 191-228. */
            ml.screen_top_y--;
            spawn_npcs_in_row(target_x_tile, ml.screen_top_y - 1 - VIEWPORT_PAD_TOP / 8);
            /* Assembly lines 221-228: SPAWN_HORIZONTAL */
            spawn_horizontal((uint16_t)(target_x_tile - 8),
                             (uint16_t)(ml.screen_top_y - 8));
        }
    }

    /* Step 5: Refill tilemaps centered on camera.
     * The assembly streams individual columns/rows via LOAD_MAP_COLUMN_TO_VRAM /
     * LOAD_MAP_ROW_TO_VRAM. The C port refills the entire tilemap instead. */
    int16_t view_x_tile = (int16_t)(cam_x_pixels >> 3) - (VIEWPORT_CENTER_X / 8);
    int16_t view_y_tile = (int16_t)(cam_y_pixels >> 3) - (VIEWPORT_CENTER_Y / 8);
    fill_tilemaps(view_x_tile, view_y_tile);
    fill_collision_tiles(view_x_tile, view_y_tile);
}

/* =========================================================================
 * Tile collision system
 *
 * Port of:
 *   GET_COLLISION_TILE_AND_CHECK_LADDER  — asm/overworld/collision/get_collision_tile_and_check_ladder.asm
 *   CHECK_COLLISION_TILE_PATTERN         — asm/overworld/collision/check_collision_tile_pattern.asm
 *   TEST_COLLISION_NORTH                 — asm/overworld/collision/test_collision_north.asm
 *   TEST_COLLISION_SOUTH                 — asm/overworld/collision/test_collision_south.asm
 *   TEST_COLLISION_WEST                  — asm/overworld/collision/test_collision_west.asm
 *   TEST_COLLISION_EAST                  — asm/overworld/collision/test_collision_east.asm
 *   TEST_COLLISION_DIAGONAL              — asm/overworld/collision/test_collision_diagonal.asm
 *   CHECK_DIRECTIONAL_COLLISION          — asm/overworld/collision/check_directional_collision.asm
 *
 * The collision grid (ml.loaded_collision_tiles[64*64]) stores per-tile flags.
 * Bits 6-7 (0xC0) indicate solid wall.  Bit 4 (0x10) indicates ladder/stairs.
 * CHECK_DIRECTIONAL_COLLISION samples 6 points in a 2×3 grid around the
 * entity position and returns surface flags.
 * ========================================================================= */

/* Collision check working variables (from ram.asm:907-927) */
static uint16_t set_temp_entity_surface_flags;
static uint16_t temp_entity_surface_flags;

/* Collision check point offsets — 6 points in a 2×3 grid (from C200B9/C200C5).
 * Point layout (relative to CHECKED_COLLISION_LEFT_X, CHECKED_COLLISION_TOP_Y):
 *   0(-8,0)  1(0,0)  2(+7,0)
 *   3(-8,7)  4(0,7)  5(+7,7)
 */
static const int16_t collision_x_offsets[6] = { -8, 0, 7, -8, 0, 7 };
static const int16_t collision_y_offsets[6] = { 0, 0, 0, 7, 7, 7 };

/* Diagonal collision bitmasks (from C200D1).
 * Indexed by (direction & ~1): dirs 1,5 → 0x1E; dirs 3,7 → 0x33. */
static const uint16_t diagonal_masks[4] = { 0x1E, 0x33, 0x1E, 0x33 };

/*
 * get_collision_tile_and_check_ladder — look up collision byte at tile coords.
 *
 * Port of GET_COLLISION_TILE_AND_CHECK_LADDER (C054C9).
 * If the tile has bit 4 (ladder), records the tile position in
 * ow.ladder_stairs_tile_x/y globals.
 */
static uint8_t get_collision_tile_and_check_ladder(int16_t tile_x, int16_t tile_y) {
    uint16_t tx = (uint16_t)tile_x & 0x3F;
    uint16_t ty = (uint16_t)tile_y & 0x3F;
    uint8_t flags = ml.loaded_collision_tiles[ty * 64 + tx];

    if (flags & 0x10) {
        ow.ladder_stairs_tile_x = tile_x;
        ow.ladder_stairs_tile_y = tile_y;
    }

    return flags;
}

/*
 * check_collision_tile_pattern — check 6 collision points against a bitmask.
 *
 * Port of CHECK_COLLISION_TILE_PATTERN (C05769).
 * mask: 6-bit bitmask, each bit enables checking of one of the 6 points.
 * Returns a 6-bit result where each bit indicates wall collision at that point.
 * Also accumulates surface flags in temp_entity_surface_flags.
 */
static uint16_t check_collision_tile_pattern(uint16_t mask) {
    uint16_t result = 0;
    uint16_t surface_accum = 0;

    for (int i = 0; i < 6; i++) {
        if (mask & 1) {
            /* Compute tile coordinates for this check point */
            int16_t px = (int16_t)ow.checked_collision_left_x + collision_x_offsets[i];
            int16_t py = (int16_t)ow.checked_collision_top_y + collision_y_offsets[i];
            int16_t tile_x = px >> 3;
            int16_t tile_y = py >> 3;

            uint8_t flags = get_collision_tile_and_check_ladder(tile_x, tile_y);
            surface_accum |= flags;

            if (flags & 0xC0) {
                result |= 0x40;
            }
        }

        result >>= 1;
        mask >>= 1;
    }

    if (set_temp_entity_surface_flags == 1) {
        temp_entity_surface_flags = surface_accum;
    }

    return result;
}

/*
 * test_collision_north — check collision for northward movement.
 *
 * Port of TEST_COLLISION_NORTH (C057E8).
 * Checks top 3 points (mask=0x07).
 * Returns: -1 = clear, 0xFF00 = blocked, 1/7 = nudge direction.
 */
static int16_t test_collision_north(void) {
    temp_entity_surface_flags = 0;
    set_temp_entity_surface_flags++;

    uint16_t result = check_collision_tile_pattern(0x07);

    if (result == 0x07 || result == 0x02) {
        return (int16_t)0xFF00;  /* fully blocked */
    }
    if (result == 0x00) {
        return -1;  /* clear */
    }
    if (result == 0x01) {
        return 1;  /* nudge UP_RIGHT */
    }
    if (result == 0x04) {
        return 7;  /* nudge UP_LEFT */
    }
    if (result == 0x06) {
        if ((ow.checked_collision_left_x & 0x07) == 0) {
            return 7;  /* nudge UP_LEFT */
        }
    }
    return -1;  /* clear */
}

/*
 * test_collision_south — check collision for southward movement.
 *
 * Port of TEST_COLLISION_SOUTH (C0583C).
 * Checks bottom 3 points (mask=0x38).
 * Returns: -1 = clear, 0xFF00 = blocked, 3/5 = nudge direction.
 */
static int16_t test_collision_south(void) {
    temp_entity_surface_flags = 0;
    set_temp_entity_surface_flags++;

    uint16_t result = check_collision_tile_pattern(0x38);

    if (result == 0x07 || result == 0x10) {
        return (int16_t)0xFF00;  /* fully blocked */
    }
    if (result == 0x00) {
        return -1;  /* clear */
    }
    if (result == 0x08) {
        return 3;  /* nudge DOWN_RIGHT */
    }
    if (result == 0x20) {
        return 5;  /* nudge DOWN_LEFT */
    }
    if (result == 0x30) {
        if ((ow.checked_collision_left_x & 0x07) == 0) {
            return 5;  /* nudge DOWN_LEFT */
        }
    }
    return -1;  /* clear */
}

/*
 * test_collision_west — check collision for westward movement.
 *
 * Port of TEST_COLLISION_WEST (C05890).
 * Checks left 2 points (mask=0x09), with extended check and corner analysis.
 * Returns: -1 = blocked, 6 = clear, 5/7 = nudge direction.
 */
static int16_t test_collision_west(void) {
    int16_t nudge_dir = -1;
    uint16_t extended = 0;
    uint16_t corner_flags = 0;

    temp_entity_surface_flags = 0;
    set_temp_entity_surface_flags = 1;

    uint16_t result = check_collision_tile_pattern(0x09);

    if (result == 0) {
        /* No collision at primary check — try 4px further left */
        ow.checked_collision_left_x -= 4;
        result = check_collision_tile_pattern(0x09);
        if (result == 0) {
            return 6;  /* clear, allow west movement */
        }
        extended = 1;
    }

    /* Both edges blocked? (bits 0 and 3 both set = 0x09) */
    if ((result & 0x09) == 0x09) {
        if (ow.checked_collision_top_y & 0x07) {
            if (extended) return 6;
            return -1;  /* blocked */
        }
    }

    /* Check corner surfaces to determine nudge eligibility.
     * Top-left corner: tile at (left_x - 4, top_y - 2) */
    {
        int16_t cx = ((int16_t)ow.checked_collision_left_x - 4) >> 3;
        int16_t cy = ((int16_t)ow.checked_collision_top_y - 2) >> 3;
        uint16_t tx = (uint16_t)cx & 0x3F;
        uint16_t ty = (uint16_t)cy & 0x3F;
        uint8_t flags = ml.loaded_collision_tiles[ty * 64 + tx];
        if (flags & 0xC0) {
            corner_flags |= 1;
        }
    }

    /* Bottom-left corner: tile at (left_x - 4, top_y + 9) */
    {
        int16_t cx = ((int16_t)ow.checked_collision_left_x - 4) >> 3;
        int16_t cy = ((int16_t)ow.checked_collision_top_y + 9) >> 3;
        uint16_t tx = (uint16_t)cx & 0x3F;
        uint16_t ty = (uint16_t)cy & 0x3F;
        uint8_t flags = ml.loaded_collision_tiles[ty * 64 + tx];
        if (flags & 0xC0) {
            corner_flags |= 2;
        }
    }

    /* Dispatch based on collision pattern */
    if (result == 0x09) {
        /* Both edges blocked */
        if (corner_flags == 1) {
            nudge_dir = 5;  /* nudge DOWN_LEFT */
        } else if (corner_flags == 2) {
            nudge_dir = 7;  /* nudge UP_LEFT */
        } else if (corner_flags == 0) {
            /* Neither corner has wall — nudge based on Y position */
            if ((ow.checked_collision_top_y & 0x07) >= 4) {
                nudge_dir = 5;  /* nudge DOWN_LEFT */
            } else {
                nudge_dir = 7;  /* nudge UP_LEFT */
            }
        }
    } else if (result == 0x01) {
        /* Top edge blocked only */
        if (!(corner_flags & 2)) {
            nudge_dir = 5;  /* nudge DOWN_LEFT */
        }
    } else if (result == 0x08) {
        /* Bottom edge blocked only */
        if (!(corner_flags & 1)) {
            nudge_dir = 7;  /* nudge UP_LEFT */
        }
    }

    /* If extended check found no nudge, allow west movement */
    if (extended && nudge_dir == -1) {
        return 6;  /* clear */
    }

    return nudge_dir;
}

/*
 * test_collision_east — check collision for eastward movement.
 *
 * Port of TEST_COLLISION_EAST (C059EF).
 * Checks right 2 points (mask=0x24), with extended check and corner analysis.
 * Returns: -1 = blocked, 2 = clear, 1/3 = nudge direction.
 */
static int16_t test_collision_east(void) {
    int16_t nudge_dir = -1;
    uint16_t extended = 0;
    uint16_t corner_flags = 0;

    temp_entity_surface_flags = 0;
    set_temp_entity_surface_flags = 1;

    uint16_t result = check_collision_tile_pattern(0x24);

    if (result == 0) {
        /* No collision at primary check — try 4px further right */
        ow.checked_collision_left_x += 4;
        result = check_collision_tile_pattern(0x24);
        if (result == 0) {
            return 2;  /* clear, allow east movement */
        }
        extended = 1;
    }

    /* Both edges blocked? (bits 2 and 5 both set = 0x24) */
    if ((result & 0x24) == 0x24) {
        if (ow.checked_collision_top_y & 0x07) {
            if (extended) return 2;
            return -1;  /* blocked */
        }
    }

    /* Check corner surfaces to determine nudge eligibility.
     * Top-right corner: tile at (left_x + 4, top_y - 2) */
    {
        int16_t cx = ((int16_t)ow.checked_collision_left_x + 4) >> 3;
        int16_t cy = ((int16_t)ow.checked_collision_top_y - 2) >> 3;
        uint16_t tx = (uint16_t)cx & 0x3F;
        uint16_t ty = (uint16_t)cy & 0x3F;
        uint8_t flags = ml.loaded_collision_tiles[ty * 64 + tx];
        if (flags & 0xC0) {
            corner_flags |= 1;
        }
    }

    /* Bottom-right corner: tile at (left_x + 4, top_y + 9) */
    {
        int16_t cx = ((int16_t)ow.checked_collision_left_x + 4) >> 3;
        int16_t cy = ((int16_t)ow.checked_collision_top_y + 9) >> 3;
        uint16_t tx = (uint16_t)cx & 0x3F;
        uint16_t ty = (uint16_t)cy & 0x3F;
        uint8_t flags = ml.loaded_collision_tiles[ty * 64 + tx];
        if (flags & 0xC0) {
            corner_flags |= 2;
        }
    }

    /* Dispatch based on collision pattern */
    if (result == 0x24) {
        /* Both edges blocked */
        if (corner_flags == 1) {
            nudge_dir = 3;  /* nudge DOWN_RIGHT */
        } else if (corner_flags == 2) {
            nudge_dir = 1;  /* nudge UP_RIGHT */
        } else if (corner_flags == 0) {
            /* Neither corner has wall — nudge based on Y position */
            if ((ow.checked_collision_top_y & 0x07) >= 4) {
                nudge_dir = 3;  /* nudge DOWN_RIGHT */
            } else {
                nudge_dir = 1;  /* nudge UP_RIGHT */
            }
        }
    } else if (result == 0x04) {
        /* Top edge blocked only */
        if (!(corner_flags & 2)) {
            nudge_dir = 3;  /* nudge DOWN_RIGHT */
        }
    } else if (result == 0x20) {
        /* Bottom edge blocked only */
        if (!(corner_flags & 1)) {
            nudge_dir = 1;  /* nudge UP_RIGHT */
        }
    }

    /* If extended check found no nudge, allow east movement */
    if (extended && nudge_dir == -1) {
        return 2;  /* clear */
    }

    return nudge_dir;
}

/*
 * test_collision_diagonal — check collision for diagonal movement.
 *
 * Port of TEST_COLLISION_DIAGONAL (C05B4E).
 * Uses direction-specific bitmask from diagonal_masks table.
 * Returns: 0xFF00 = blocked, or original direction = clear.
 */
static int16_t test_collision_diagonal(int16_t direction) {
    temp_entity_surface_flags = 0;
    set_temp_entity_surface_flags++;

    /* Strip bit 0 to get table index: dirs 1,5→0, dirs 3,7→1 */
    uint16_t mask_idx = ((uint16_t)direction >> 1);
    uint16_t mask = diagonal_masks[mask_idx];

    uint16_t result = check_collision_tile_pattern(mask);
    if (result != 0) {
        return (int16_t)0xFF00;  /* blocked */
    }
    return direction;  /* clear */
}

/*
 * check_directional_collision — main directional collision dispatcher.
 *
 * Port of CHECK_DIRECTIONAL_COLLISION (C05B7B).
 * Checks tile collision at position (x, y) for the given movement direction.
 *
 * Sets globals:
 *   ow.final_movement_direction: adjusted direction (may differ for wall slides)
 *   ow.not_moving_in_same_direction_faced: 1 if direction was nudged
 *   ow.ladder_stairs_tile_x/y: tile coords of any ladder found
 *   ow.checked_collision_left_x/top_y: working position
 *
 * Returns surface flags. Bits 6-7 (0xC0) indicate wall blocking.
 */
uint16_t check_directional_collision(int16_t x, int16_t y, int16_t direction) {
    ow.not_moving_in_same_direction_faced = 0;
    set_temp_entity_surface_flags = 0;
    temp_entity_surface_flags = 0;

    ow.final_movement_direction = direction;
    ow.checked_collision_left_x = (uint16_t)x;
    ow.checked_collision_top_y = (uint16_t)y;

    int16_t test_result = -1;

    switch (direction) {
    case 0: /* NORTH */
        test_result = test_collision_north();
        /* Double-check: if blocked (-1 returned as no wall, not 0xFF00),
         * try 4px up to detect approaching walls (assembly lines 55-73). */
        if (test_result == -1) {
            int16_t saved_ladder = ow.ladder_stairs_tile_x;
            if ((ow.checked_collision_top_y & 0x07) >= 5) {
                break;  /* too close to tile edge, skip double-check */
            }
            ow.checked_collision_top_y -= 4;
            int16_t recheck = test_collision_north();
            if ((recheck & 0xFF00) == 0xFF00) {
                /* Double-check also blocked → keep original result */
                ow.ladder_stairs_tile_x = saved_ladder;
                break;
            }
            /* Double-check found a different collision → use it */
            test_result = recheck;
            ow.ladder_stairs_tile_x = saved_ladder;
        }
        break;

    case 4: /* SOUTH */
        test_result = test_collision_south();
        if (test_result == -1) {
            int16_t saved_ladder = ow.ladder_stairs_tile_x;
            if ((ow.checked_collision_top_y & 0x07) <= 3) {
                break;
            }
            ow.checked_collision_top_y += 4;
            int16_t recheck = test_collision_south();
            if ((recheck & 0xFF00) == 0xFF00) {
                ow.ladder_stairs_tile_x = saved_ladder;
                break;
            }
            test_result = recheck;
            ow.ladder_stairs_tile_x = saved_ladder;
        }
        break;

    case 6: /* WEST */
        test_result = test_collision_west();
        break;

    case 2: /* EAST */
        test_result = test_collision_east();
        break;

    case 1: /* UP_RIGHT */
    case 3: /* DOWN_RIGHT */
    case 5: /* DOWN_LEFT */
    case 7: /* UP_LEFT */
        test_result = test_collision_diagonal(direction);
        if ((uint16_t)test_result != 0xFF00) {
            /* Diagonal clear — override test_result with direction */
            test_result = direction;
        }
        break;

    default:
        goto process_result;
    }

process_result:
    /* If there are pending interactions, suppress ladder door detection */
    if (ow.pending_interactions) {
        ow.ladder_stairs_tile_x = -1;
    }

    /* Interpret test result */
    if (test_result == -1 || (uint16_t)test_result == 0xFF00) {
        /* No collision, or fully blocked — return surface flags as-is */
        return temp_entity_surface_flags;
    }

    /* Collision with nudge direction */
    ow.not_moving_in_same_direction_faced = (test_result != direction) ? 1 : 0;
    ow.final_movement_direction = test_result;
    return temp_entity_surface_flags & 0x003F;
}

/*
 * get_collision_at_pixel — simple single-point collision lookup.
 *
 * Port of GET_COLLISION_AT_PIXEL (asm/overworld/collision/get_collision_at_pixel.asm).
 * Looks up the collision tile at position (x, y+4) and returns
 * the collision flags byte. Also checks for ladder tiles.
 * Used in movement_locked mode (cutscenes) where directional collision
 * is skipped but surface flags are still needed.
 */
uint16_t get_collision_at_pixel(int16_t x, int16_t y) {
    temp_entity_surface_flags = 0;

    /* Assembly: y += 4, then divide both by 8 to get tile coords */
    int16_t tile_y = (y + 4) >> 3;
    int16_t tile_x = x >> 3;

    uint8_t flags = get_collision_tile_and_check_ladder(tile_x, tile_y);
    temp_entity_surface_flags = flags;

    return flags;
}

/*
 * Entity tile collision system.
 *
 * Port of CHECK_ENTITY_COLLISION (asm/overworld/collision/check_entity_collision.asm) and helpers:
 *   CHECK_COLLISION_TILES_HORIZONTAL  — C05503.asm — top edge check
 *   CHECK_COLLISION_TILES_VERTICAL    — C0559C.asm — bottom edge check
 *   ACCUMULATE_COLLISION_FLAGS_HORIZONTAL — C056D0.asm — right edge check
 *   ACCUMULATE_COLLISION_FLAGS_VERTICAL   — C05639.asm — left edge check
 *   CHECK_ENTITY_OBSTACLE_FLAGS       — C05E3B.asm
 *   CHECK_CURRENT_ENTITY_OBSTACLES    — C05E76.asm
 *
 * Each function samples collision bytes from ml.loaded_collision_tiles[] along
 * the entity's bounding box edges, ORing them together to accumulate flags.
 *
 * Data tables (from asm/data/unknown/):
 *   ENTITY_COLLISION_X_OFFSET[17]  — C42A1F.asm — X offset from center to left edge
 *   ENTITY_COLLISION_Y_OFFSET[17]  — C42A41.asm — Y offset from center to top
 *   SPRITE_HITBOX_ENABLE_TABLE[17] — C42AEB.asm — Y adjustment for hitbox
 *   ENTITY_COLLISION_WIDTH_TABLE[17]             — horizontal tile count for loop
 *   ENTITY_COLLISION_HEIGHT_TABLE[17]             — vertical tile count for loop
 */

/* ENTITY_COLLISION_X_OFFSET — center-to-left-edge offset per size code */
const int16_t entity_collision_x_offset[ENTITY_SIZE_COUNT] = {
    0x0008, 0x0008, 0x000C, 0x0010, 0x0018,
    0x0008, 0x000C, 0x0008, 0x0010, 0x0018,
    0x000C, 0x0008, 0x0010, 0x0018, 0x0020,
    0x0020, 0x0020,
};

/* ENTITY_COLLISION_Y_OFFSET — center-to-top offset per size code */
const int16_t entity_collision_y_offset[ENTITY_SIZE_COUNT] = {
    0x0008, 0x0008, 0x0008, 0x0008, 0x0008,
    0x0018, 0x0018, 0x0018, 0x0018, 0x0018,
    0x0020, 0x0028, 0x0028, 0x0028, 0x0028,
    0x0038, 0x0048,
};

/* SPRITE_HITBOX_ENABLE_TABLE — Y hitbox adjustment per size code */
const int16_t sprite_hitbox_enable[ENTITY_SIZE_COUNT] = {
    0x000A, 0x0000, 0x000A, 0x000A, 0x000A,
    0x0018, 0x0018, 0x0018, 0x0010, 0x0010,
    0x0000, 0x0028, 0x0020, 0x0020, 0x0020,
    0x0000, 0x0041,
};

/* ENTITY_COLLISION_WIDTH_TABLE — horizontal tile count for collision loops */
static const int16_t entity_coll_h_count[ENTITY_SIZE_COUNT] = {
    0x0002, 0x0000, 0x0002, 0x0004, 0x0006,
    0x0002, 0x0003, 0x0002, 0x0004, 0x0006,
    0x0000, 0x0002, 0x0004, 0x0006, 0x0008,
    0x0000, 0x0006,
};

/* ENTITY_COLLISION_HEIGHT_TABLE — vertical tile count for collision loops */
static const int16_t entity_coll_v_count[ENTITY_SIZE_COUNT] = {
    0x0001, 0x0000, 0x0001, 0x0001, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0002, 0x0002,
    0x0000, 0x0001, 0x0002, 0x0002, 0x0002,
    0x0000, 0x0002,
};

/*
 * check_top_edge — check collision tiles along the top horizontal edge.
 * Port of CHECK_COLLISION_TILES_HORIZONTAL (C05503).
 *
 * Checks the tile at (left_x/8, top_y/8) then iterates rightward
 * for h_count more tiles, ORing collision bytes into temp_entity_surface_flags.
 */
static void check_top_edge(int16_t left_x, int16_t size_code) {
    uint16_t prev_flags = temp_entity_surface_flags;
    int16_t h_count = entity_coll_h_count[size_code];

    int16_t tile_row = ow.checked_collision_top_y >> 3;
    int16_t tile_col = (left_x >> 3) & 0x3F;

    /* First tile at (tile_row, tile_col) */
    uint16_t idx = ((tile_row & 0x3F) << 6) | tile_col;
    uint16_t accum = prev_flags | (ml.loaded_collision_tiles[idx] & 0xFF);

    /* Loop: check h_count more tiles starting from (left_x + 7) / 8 */
    int16_t loop_col = (left_x + 7) >> 3;
    for (int16_t i = 0; i < h_count; i++) {
        int16_t col = (loop_col + i) & 0x3F;
        idx = ((tile_row & 0x3F) << 6) | col;
        accum |= (ml.loaded_collision_tiles[idx] & 0xFF);
    }

    temp_entity_surface_flags = accum;
}

/*
 * check_bottom_edge — check collision tiles along the bottom horizontal edge.
 * Port of CHECK_COLLISION_TILES_VERTICAL (C0559C).
 *
 * Computes the bottom row from top_y + v_count*8 - 1, then checks tiles
 * from left_x rightward for h_count tiles, ORing collision bytes.
 */
static void check_bottom_edge(int16_t left_x, int16_t size_code) {
    uint16_t prev_flags = temp_entity_surface_flags;
    int16_t h_count = entity_coll_h_count[size_code];

    /* bottom_row = (top_y + v_count*8 - 1) / 8 */
    int16_t bottom_row = (ow.checked_collision_top_y +
                          entity_coll_v_count[size_code] * 8 - 1) >> 3;
    int16_t tile_col = (left_x >> 3) & 0x3F;

    /* First tile at (bottom_row, tile_col) */
    uint16_t idx = ((bottom_row & 0x3F) << 6) | tile_col;
    uint16_t accum = prev_flags | (ml.loaded_collision_tiles[idx] & 0xFF);

    /* Loop: check h_count more tiles starting from (left_x + 7) / 8 */
    int16_t loop_col = (left_x + 7) >> 3;
    for (int16_t i = 0; i < h_count; i++) {
        int16_t col = (loop_col + i) & 0x3F;
        idx = ((bottom_row & 0x3F) << 6) | col;
        accum |= (ml.loaded_collision_tiles[idx] & 0xFF);
    }

    temp_entity_surface_flags = accum;
}

/*
 * check_right_edge — check collision tiles along the right vertical edge.
 * Port of ACCUMULATE_COLLISION_FLAGS_HORIZONTAL (C056D0).
 *
 * Computes the right column from left_x + h_count*8 - 1, then checks tiles
 * from top_y downward for v_count tiles, ORing collision bytes.
 */
static void check_right_edge(int16_t top_y, int16_t size_code) {
    uint16_t prev_flags = temp_entity_surface_flags;
    int16_t v_count = entity_coll_v_count[size_code];

    /* right_col = (left_x + h_count*8 - 1) / 8 */
    int16_t right_col = (ow.checked_collision_left_x +
                         entity_coll_h_count[size_code] * 8 - 1) >> 3;

    int16_t tile_row = (top_y >> 3) & 0x3F;

    /* First tile at (tile_row, right_col) */
    uint16_t idx = (tile_row << 6) | (right_col & 0x3F);
    uint16_t accum = prev_flags | (ml.loaded_collision_tiles[idx] & 0xFF);

    /* Loop: check v_count more tiles starting from (top_y + 7) / 8 */
    int16_t loop_row = (top_y + 7) >> 3;
    for (int16_t i = 0; i < v_count; i++) {
        int16_t row = (loop_row + i) & 0x3F;
        idx = (row << 6) | (right_col & 0x3F);
        accum |= (ml.loaded_collision_tiles[idx] & 0xFF);
    }

    temp_entity_surface_flags = accum;
}

/*
 * check_left_edge — check collision tiles along the left vertical edge.
 * Port of ACCUMULATE_COLLISION_FLAGS_VERTICAL (C05639).
 *
 * Checks tiles at left_x column from top_y downward for v_count tiles,
 * ORing collision bytes into temp_entity_surface_flags.
 */
static void check_left_edge(int16_t top_y, int16_t size_code) {
    uint16_t prev_flags = temp_entity_surface_flags;
    int16_t v_count = entity_coll_v_count[size_code];

    int16_t left_col = (ow.checked_collision_left_x >> 3) & 0x3F;
    int16_t tile_row = (top_y >> 3) & 0x3F;

    /* First tile at (tile_row, left_col) */
    uint16_t idx = (tile_row << 6) | left_col;
    uint16_t accum = prev_flags | (ml.loaded_collision_tiles[idx] & 0xFF);

    /* Loop: check v_count more tiles starting from (top_y + 7) / 8 */
    int16_t loop_row = (top_y + 7) >> 3;
    for (int16_t i = 0; i < v_count; i++) {
        int16_t row = (loop_row + i) & 0x3F;
        idx = (row << 6) | left_col;
        accum |= (ml.loaded_collision_tiles[idx] & 0xFF);
    }

    temp_entity_surface_flags = accum;
}

/*
 * check_entity_collision — check tile collision for an entity moving in a direction.
 *
 * Port of CHECK_ENTITY_COLLISION (asm/overworld/collision/check_entity_collision.asm).
 *
 * Parameters:
 *   x         — entity X position (world pixels)
 *   y         — entity Y position (world pixels)
 *   entity_slot — entity slot number (indexes ENTITY_SIZES)
 *   direction — movement direction (0-7, SNES convention)
 *
 * Returns accumulated collision flags (ORed from all checked tiles).
 * Sets ow.checked_collision_left_x and ow.checked_collision_top_y globals.
 */
uint16_t check_entity_collision(int16_t x, int16_t y,
                                int16_t entity_slot, int16_t direction) {
    temp_entity_surface_flags = 0;

    /* Look up entity size from ENTITY_SIZES[slot] */
    int16_t offset = entity_slot;
    int16_t size_val = entities.sizes[offset];

    /* Compute bounding box edges */
    int16_t left_x = x - entity_collision_x_offset[size_val];
    ow.checked_collision_left_x = (uint16_t)left_x;

    int16_t top_y = y - entity_collision_y_offset[size_val]
                      + sprite_hitbox_enable[size_val];
    ow.checked_collision_top_y = (uint16_t)top_y;

    /* Dispatch based on direction, checking the leading edge(s) */
    switch (direction) {
    case 0:  /* UP */
        check_top_edge(left_x, size_val);
        break;
    case 1:  /* UP_RIGHT */
        check_right_edge(top_y, size_val);
        check_top_edge(left_x, size_val);
        break;
    case 2:  /* RIGHT */
        check_right_edge(top_y, size_val);
        break;
    case 3:  /* DOWN_RIGHT */
        check_bottom_edge(left_x, size_val);
        check_right_edge(top_y, size_val);
        break;
    case 4:  /* DOWN */
        check_bottom_edge(left_x, size_val);
        break;
    case 5:  /* DOWN_LEFT */
        check_left_edge(top_y, size_val);
        check_bottom_edge(left_x, size_val);
        break;
    case 6:  /* LEFT */
        check_left_edge(top_y, size_val);
        break;
    case 7:  /* UP_LEFT */
        check_top_edge(left_x, size_val);
        check_left_edge(top_y, size_val);
        break;
    default:
        break;
    }

    return temp_entity_surface_flags;
}

/* CHECK_PLAYER_COLLISION_AT_POSITION (asm/overworld/collision/check_player_collision_at_position.asm).
 * Checks tile collision at both top and bottom horizontal edges (non-directional).
 * Unlike check_entity_collision which is direction-based, this always checks both
 * horizontal rows of the collision box.
 * Parameters: x, y = world position; entity_slot = entity slot number.
 * Returns accumulated surface flags from all checked tiles. */
uint16_t check_player_collision_at_position(int16_t x, int16_t y,
                                             int16_t entity_slot) {
    temp_entity_surface_flags = 0;

    int16_t offset = entity_slot;
    int16_t size_val = entities.sizes[offset];

    int16_t top_y = y - entity_collision_y_offset[size_val]
                      + sprite_hitbox_enable[size_val];
    ow.checked_collision_top_y = (uint16_t)top_y;

    int16_t left_x = x - entity_collision_x_offset[size_val];
    ow.checked_collision_left_x = (uint16_t)left_x;

    check_top_edge(left_x, size_val);
    check_bottom_edge(left_x, size_val);

    return temp_entity_surface_flags;
}

/* DIRECTION_QUANTIZE_TABLE (asm/data/unknown/C0CF58.asm).
 * 63-byte spiral pattern: 7R,7D,7L,6U,6R,5D,5L,4U,4R,3D,3L,2U,2R,1D,1L.
 * Values: 1=UP, 2=RIGHT, 3=DOWN, 4=LEFT. */
static const uint8_t direction_quantize_table[63] = {
    2, 2, 2, 2, 2, 2, 2,  3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4,  1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2,     3, 3, 3, 3, 3,
    4, 4, 4, 4, 4,         1, 1, 1, 1,
    2, 2, 2, 2,            3, 3, 3,
    4, 4, 4,               1, 1,
    2, 2,                  3,
    4,
};

/* ---- CHECK_ENTITY_COLLISION_PATH (port of C0CF97) ----
 *
 * Walks a spiral pattern from the entity's collision center, checking
 * the ml.loaded_collision_tiles grid at each step. Returns -1 if collision
 * found within max_steps (and stores collision coords in var6/var7),
 * or 0 if path is clear.
 *
 * Parameters:
 *   entity_slot    — entity slot number
 *   collision_mask — collision bits to test (AND with tile flags)
 *   max_steps      — maximum number of steps to check (up to 63)
 *
 * Side effects on collision:
 *   entities.var[6][entity_offset] = collision X pixel
 *   entities.var[7][entity_offset] = collision Y pixel */
int16_t check_entity_collision_path(int16_t entity_slot, uint8_t collision_mask,
                                    int16_t max_steps) {
    int16_t entity_offset = entity_slot;
    int16_t entity_size = entities.sizes[entity_offset];

    /* Calculate starting tile position (entity center minus collision offsets) */
    int16_t start_tile_x = ((entities.abs_x[entity_offset]
                              - entity_collision_x_offset[entity_size]) >> 3) - 4;
    int16_t start_tile_y = ((entities.abs_y[entity_offset]
                              - entity_collision_y_offset[entity_size]
                              + sprite_hitbox_enable[entity_size]) >> 3) - 4;

    /* Masked coordinates for bounds checking (0-63 range) */
    int16_t tile_x_masked = start_tile_x & 0x3F;
    int16_t tile_y_masked = start_tile_y & 0x3F;

    /* Unmasked coordinates for collision pixel calculation */
    int16_t tile_x_unmasked = start_tile_x;
    int16_t tile_y_unmasked = start_tile_y;

    int16_t table_pos = 0;

    for (int16_t step = 0; step < max_steps; step++) {
        /* Check tile if within bounds */
        if (tile_x_masked < 64 && tile_y_masked < 64) {
            uint16_t idx = (uint16_t)((tile_y_masked & 0x3F) * 64
                                      + (tile_x_masked & 0x3F));
            if ((collision_mask & ml.loaded_collision_tiles[idx]) != 0) {
                /* Collision found — compute pixel coordinates */
                int16_t coll_x = tile_x_unmasked * 8
                                 + entity_collision_x_offset[entity_size];
                int16_t coll_y = tile_y_unmasked * 8
                                 - sprite_hitbox_enable[entity_size]
                                 + entity_collision_y_offset[entity_size];
                entities.var[6][entity_offset] = coll_x;
                entities.var[7][entity_offset] = coll_y;
                return -1;
            }
        }

        /* Advance position using direction table */
        if (table_pos < 63) {
            uint8_t dir = direction_quantize_table[table_pos++];
            switch (dir) {
            case 1: /* UP */
                tile_y_masked--;
                tile_y_unmasked--;
                break;
            case 2: /* RIGHT */
                tile_x_masked++;
                tile_x_unmasked++;
                break;
            case 3: /* DOWN */
                tile_y_masked++;
                tile_y_unmasked++;
                break;
            case 4: /* LEFT */
                tile_x_masked--;
                tile_x_unmasked--;
                break;
            default:
                break;
            }
        }
    }

    return 0;  /* No collision found */
}

/* ---- CHECK_ENTITY_COLLISION_AHEAD (port of C0D0D9) ----
 *
 * Thin wrapper: calls check_entity_collision_path with mask=3 and max_steps=60.
 * Used by entity movement scripts to check for obstacles ahead. */
int16_t check_entity_collision_ahead(int16_t entity_slot) {
    return check_entity_collision_path(entity_slot, 3, 0x3C);
}

/* ---- RESOLVE_MAP_SECTOR_MUSIC (port of C068F4) ----
 *
 * Port of asm/audio/resolve_map_sector_music.asm.
 * Looks up the music zone for the given world pixel coordinates,
 * walks the event-flag music chain to find the current track,
 * and sets NEXT_MAP_MUSIC_TRACK. If the music differs from current
 * and DO_MAP_MUSIC_FADE is 0, issues a fade-out via APU port 1.
 *
 * Parameters match assembly: x_coord in A, y_coord in X. */
void resolve_map_sector_music(uint16_t x_coord, uint16_t y_coord) {
    if (ow.disable_music_changes) return;
    if (!per_sector_music_data || !event_music_ptr_table || !event_music_table)
        return;

    /* Compute sector index: (y / 128) * 32 + (x >> 8) */
    uint16_t x_sector = (x_coord >> 8) & 0xFF;
    uint16_t y_sector = y_coord / 128;  /* MAP_WIDTH_TILES = 128 */
    uint16_t sector_idx = y_sector * 32 + x_sector;

    if (sector_idx >= per_sector_music_size) return;
    uint8_t music_zone = per_sector_music_data[sector_idx];

    /* Look up within-bank pointer from ptr table */
    if ((size_t)(music_zone * 2 + 2) > event_music_ptr_table_size) return;
    uint16_t ptr = read_u16_le(&event_music_ptr_table[music_zone * 2]);
    ptr &= 0x7FFF;

    /* Convert within-bank pointer to offset into event_music_table ert.buffer */
    if (ptr < EVENT_MUSIC_TABLE_LOWORD) return;
    uint16_t offset = ptr - EVENT_MUSIC_TABLE_LOWORD;

    /* Walk event flag entries (4 bytes each: flag_word, music_word) */
    while (offset + 4 <= event_music_table_size) {
        uint16_t flag_word = read_u16_le(&event_music_table[offset]);

        if (flag_word == 0) {
            /* NULL terminator — default entry, use its music */
            ml.loaded_map_music_entry_offset = offset;
            ml.next_map_music_track = event_music_table[offset + 2];
            break;
        }

        /* Check event flag */
        uint16_t flag_id = flag_word & 0x7FFF;
        int flag_state = event_flag_get(flag_id) ? 1 : 0;

        /* Assembly BLTEQ on signed CMP #$8000:
         * Only $8000 exactly is <= -32768 signed. All other values > -32768.
         * So expected=0 only when flag_word == $8000; otherwise expected=1. */
        int expected = (flag_word == 0x8000) ? 0 : 1;

        if (flag_state == expected) {
            /* Condition met — use this entry */
            ml.loaded_map_music_entry_offset = offset;
            ml.next_map_music_track = event_music_table[offset + 2];
            break;
        }

        offset += 4;  /* advance to next entry */
    }

    /* If music differs and not fading, trigger fade-out */
    if (!ml.do_map_music_fade && ml.next_map_music_track != ml.current_map_music_track) {
        write_apu_port1(2);
    }
}

/* ---- GET_MAP_MUSIC_AT_LEADER (port of C069F7) ----
 *
 * Port of asm/audio/get_map_music_at_leader.asm.
 * Resolves the music track at the leader's position.
 * Returns the next music track ID. */
uint16_t get_map_music_at_leader(void) {
    resolve_map_sector_music(game_state.leader_x_coord,
                             game_state.leader_y_coord);
    return ml.next_map_music_track;
}

/* ---- UPDATE_MAP_MUSIC_AT_LEADER (port of C06A07) ----
 *
 * Port of asm/audio/update_map_music_at_leader.asm.
 * Resolves and immediately applies the music track at the leader's position. */
void update_map_music_at_leader(void) {
    resolve_map_sector_music(game_state.leader_x_coord,
                             game_state.leader_y_coord);
    change_music(ml.next_map_music_track);
}

/* ---- APPLY_NEXT_MAP_MUSIC (port of C069AF) ----
 *
 * Port of asm/audio/apply_next_map_music.asm.
 * If NEXT_MAP_MUSIC_TRACK differs from CURRENT_MAP_MUSIC_TRACK,
 * changes the music and writes byte 3 of the matched entry to APU port 1. */
void apply_next_map_music(void) {
    if (ow.disable_music_changes) return;

    if (ml.next_map_music_track != ml.current_map_music_track) {
        ml.current_map_music_track = ml.next_map_music_track;
        change_music(ml.next_map_music_track);

        /* Read byte 3 of the matched entry (APU port 1 parameter) */
        if (event_music_table && ml.loaded_map_music_entry_offset + 4 <= event_music_table_size) {
            uint8_t apu_param = event_music_table[ml.loaded_map_music_entry_offset + 3];
            write_apu_port1(apu_param);
        }
    }
}

