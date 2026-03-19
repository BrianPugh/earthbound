/*
 * Map sector loading system.
 *
 * Port of:
 *   LOAD_MAP_AT_POSITION — asm/overworld/load_map_at_position.asm
 *   LOAD_MAP_AT_SECTOR   — asm/overworld/load_map_at_sector.asm
 *
 * Loads overworld map tile graphics, arrangements, and palettes
 * based on world coordinates or sector indices.
 */
#ifndef GAME_MAP_LOADER_H
#define GAME_MAP_LOADER_H

#include "core/types.h"
#include "include/binary.h"

/* NPC configuration entry (17 bytes, packed to match ROM binary layout).
 * 1584 entries in the table. */
#define NPC_CONFIG_COUNT 1584

PACKED_STRUCT
typedef struct {
    uint8_t  type;              /*  0: NPC_TYPE (1=PERSON, 2=ITEM_BOX, 3=OBJECT) */
    uint16_t sprite;            /*  1: sprite ID (LE) */
    uint8_t  direction;         /*  3: default facing direction */
    uint16_t event_script;      /*  4: event script ID (LE) */
    uint16_t event_flag;        /*  6: event flag ID (LE) */
    uint8_t  appearance_style;  /*  8: 0=always, 1=show_if_flag_off, 2=show_if_flag_on */
    uint32_t text_pointer;      /*  9: SNES ROM address of dialogue (LE) */
    uint32_t item;              /* 13: item ID (<256) or money (>=256), or text_pointer2 (LE) */
} NpcConfig;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(NpcConfig, 17);

/* Load data tables needed for map loading (tilesetpalette data, tileset table).
 * Call once at startup. Returns true on success. */
bool map_loader_init(void);

/* Free map loader data tables. */
void map_loader_free(void);

/* Load the map sector for the given pixel coordinates.
 * Converts pixel position to sector, loads tileset GFX, arrangement, palette.
 * Port of LOAD_MAP_AT_POSITION (asm/overworld/load_map_at_position.asm). */
void load_map_at_position(uint16_t x_pixels, uint16_t y_pixels);

/* Reload map tiles/collision/VRAM at given position WITHOUT clearing entities.
 * Used after battles to restore the overworld without despawning roaming enemies.
 * Port of RELOAD_MAP_AT_POSITION (asm/overworld/reload_map_at_position.asm). */
void reload_map_at_position(uint16_t x_pixels, uint16_t y_pixels);

/* Load the map sector at the given sector indices.
 * Loads tileset GFX, arrangement data, and palette.
 * Port of LOAD_MAP_AT_SECTOR (asm/overworld/load_map_at_sector.asm). */
void load_map_at_sector(uint16_t sector_x, uint16_t sector_y);

/* Invalidate the tileset combo cache so the next load_map_at_sector()
 * will reload tileset GFX even if the combo hasn't changed.
 * Assembly sets LOADED_MAP_TILE_COMBO = -1 in RELOAD_MAP (line 10)
 * and RELOAD_MAP_AT_POSITION (line 28). */
void invalidate_loaded_tileset_combo(void);

/* Refill the full tilemap and collision grids from current camera position.
 * Port of LOAD_INITIAL_MAP_DATA (asm/overworld/map/load_initial_map_data.asm). */
void load_initial_map_data(void);

/* Spawn NPCs in the given NPC sector (256×256 pixel sectors).
 * Port of SPAWN_NPCS_AT_SECTOR (asm/overworld/npc/spawn_npcs_at_sector.asm). */
void spawn_npcs_at_sector(uint16_t sector_x, uint16_t sector_y);

/* Refresh BG1/BG2 tilemaps for the current camera position.
 * Call each frame (or when camera crosses a tile boundary) to stream
 * new map tiles into the tilemap as the camera scrolls.
 * Parameters are the camera center in pixels. */
void map_refresh_tilemaps(uint16_t cam_x_pixels, uint16_t cam_y_pixels);

/* Look up surface/collision flags at a world position for an entity of given size.
 * size_code: index into ENTITY_COLLISION tables (from entities.sizes[]), NOT entity slot.
 * Port of LOOKUP_ENTITY_SURFACE_FLAGS (asm/overworld/collision/lookup_entity_surface_flags.asm). */
uint16_t lookup_surface_flags(int16_t x, int16_t y, uint16_t size_code);

/* Check directional tile collision at a world position.
 * Port of CHECK_DIRECTIONAL_COLLISION (asm/overworld/collision/check_directional_collision.asm).
 * Sets final_movement_direction (may differ from input direction for wall slides).
 * Sets not_moving_in_same_direction_faced, ladder_stairs_tile_x/y.
 * Returns surface flags — bits 6-7 (0xC0) indicate wall blocking. */
uint16_t check_directional_collision(int16_t x, int16_t y, int16_t direction);

/* Simple single-point collision lookup at a world position.
 * Port of GET_COLLISION_AT_PIXEL (asm/overworld/collision/get_collision_at_pixel.asm).
 * Used in movement_locked mode where directional collision is skipped. */
uint16_t get_collision_at_pixel(int16_t x, int16_t y);

/* Check tile collision for an entity moving in a given direction.
 * Port of CHECK_ENTITY_COLLISION (asm/overworld/collision/check_entity_collision.asm).
 * Samples collision bytes along the entity's leading bounding box edges
 * and returns accumulated flags. Sets checked_collision_left_x/top_y. */
uint16_t check_entity_collision(int16_t x, int16_t y,
                                int16_t entity_slot, int16_t direction);

/* Check tile collision at both horizontal edges (top + bottom).
 * Port of CHECK_PLAYER_COLLISION_AT_POSITION.
 * Unlike check_entity_collision which is direction-based, this always
 * checks both rows. Returns accumulated surface flags. */
uint16_t check_player_collision_at_position(int16_t x, int16_t y,
                                             int16_t entity_slot);

/* Walk a spiral collision check from the entity's position.
 * Returns -1 if collision found (stores coords in var6/var7), 0 if clear.
 * Port of CHECK_ENTITY_COLLISION_PATH (asm/overworld/collision/check_entity_collision_path.asm). */
int16_t check_entity_collision_path(int16_t entity_slot, uint8_t collision_mask,
                                    int16_t max_steps);

/* Check for collision obstacles ahead of the entity (mask=3, steps=60).
 * Port of CHECK_ENTITY_COLLISION_AHEAD (asm/overworld/collision/check_entity_collision_ahead.asm). */
int16_t check_entity_collision_ahead(int16_t entity_slot);

/* Get the event_flag field from the NPC config table for a given NPC ID. */
uint16_t get_npc_config_event_flag(uint16_t npc_id);

/* Get the sprite ID from the NPC config table for a given NPC ID. */
uint16_t get_npc_config_sprite(uint16_t npc_id);

/* Get the default direction field from the NPC config table for a given NPC ID.
 * Returns 4 (DOWN) if NPC ID is invalid or -1. */
uint8_t get_npc_config_direction(uint16_t npc_id);

/* Get the type field from the NPC config table (1=PERSON, 2=ITEM_BOX, 3=OBJECT). */
uint8_t get_npc_config_type(uint16_t npc_id);

/* Get the 32-bit text_pointer from the NPC config table (SNES ROM address). */
uint32_t get_npc_config_text_pointer(uint16_t npc_id);

/* Get the item field from the NPC config table (16-bit).
 * Values < 256 are item IDs; >= 256 means money (value - 256). */
uint16_t get_npc_config_item(uint16_t npc_id);

/* Get the text_pointer2 field (union at offset 13, 4 bytes) from the NPC config table.
 * This is an alternate text pointer used by NPC types 1/3 for "use item on NPC" text.
 * Returns a SNES ROM address, or 0 if invalid. */
uint32_t get_npc_config_text_pointer2(uint16_t npc_id);

/* Look up per-sector attributes at a world position.
 * Port of LOAD_SECTOR_ATTRS (asm/overworld/load_sector_attributes.asm).
 * Returns the 16-bit attributes word: low 3 bits = MAP_SECTOR_MISC_CONFIG,
 * high byte = rideable item.  Also sets current_sector_attributes. */
uint16_t load_sector_attrs(uint16_t x_pixels, uint16_t y_pixels);

/* --- Palette animation state (port of OVERWORLD_PALETTE_ANIM) --- */
typedef struct {
    uint16_t timer;
    uint16_t index;
    uint16_t delays[9];
} OverworldPaletteAnim;

/* --- Tileset animation state (port of OVERWORLD_TILESET_ANIM) --- */
typedef struct {
    uint16_t animation_limit;
    uint16_t frame_delay;
    uint16_t copy_size;
    uint16_t source_offset;
    uint16_t destination_address;
    uint16_t frames_until_update;
    uint16_t current_frame;
    uint16_t current_source_offset;
} OverworldTilesetAnim;

/* Consolidated map loader runtime state. */
typedef struct {
    /* CURRENT_SECTOR_ATTRIBUTES — last value written by load_sector_attrs(). */
    uint16_t current_sector_attributes;

    /* Map music state (from bankconfig/common/ram.asm:955-961) */
    uint16_t current_map_music_track;
    uint16_t next_map_music_track;
    uint16_t do_map_music_fade;

    /* Palette snapshot used by ADJUST_PALETTE_BRIGHTNESS as reference. */
    uint16_t map_palette_backup[256];

    /* Whether palette animation is active for the current map sector. */
    uint16_t map_palette_animation_loaded;

    /* LOADED_COLLISION_TILES — 64×64 collision grid. */
    uint8_t loaded_collision_tiles[64 * 64];

    /* Number of active tileset animations for the current sector. */
    uint16_t loaded_animated_tile_count;

    /* --- Promoted statics (saveable runtime state) --- */
    int16_t loaded_tileset_combo;
    int16_t loaded_palette_index;
    OverworldPaletteAnim overworld_palette_anim;
    OverworldTilesetAnim overworld_tileset_anim[8];
    uint8_t animated_map_palette_buffer[2048];
    uint8_t animated_tileset_buffer[8192];
    int16_t screen_left_x;
    int16_t screen_top_y;

    /* Tile collision pointer buffer: 960 entries for the current tileset.
     * tile_collision_buffer[block_id] = byte offset into collision_arrangement_table. */
    uint16_t tile_collision_buffer[960];
    bool tile_collision_loaded;

    /* Offset into event_music_table for the last matched music entry. */
    uint16_t loaded_map_music_entry_offset;

    /* Palette colour averages used by ADJUST_PALETTE_BRIGHTNESS. */
    uint16_t saved_colour_average_red;
    uint16_t saved_colour_average_green;
    uint16_t saved_colour_average_blue;

    /* Your Sanctuary display state (from ram.asm:1651-1658) */
    uint16_t next_your_sanctuary_location_tile_index;
    uint16_t total_your_sanctuary_loaded_tileset_tiles;
    uint16_t your_sanctuary_loaded_tileset_tiles;
    uint16_t loaded_your_sanctuary_locations[8];
} MapLoaderState;

extern MapLoaderState ml;

/* Resolve the music zone at world coordinates and set NEXT_MAP_MUSIC_TRACK.
 * Port of RESOLVE_MAP_SECTOR_MUSIC (asm/audio/resolve_map_sector_music.asm). */
void resolve_map_sector_music(uint16_t x_coord, uint16_t y_coord);

/* Get the music track at the leader's position.
 * Port of GET_MAP_MUSIC_AT_LEADER (asm/audio/get_map_music_at_leader.asm). */
uint16_t get_map_music_at_leader(void);

/* Resolve and immediately change to the music at the leader's position.
 * Port of UPDATE_MAP_MUSIC_AT_LEADER (asm/audio/update_map_music_at_leader.asm). */
void update_map_music_at_leader(void);

/* Apply NEXT_MAP_MUSIC_TRACK if it differs from current.
 * Port of APPLY_NEXT_MAP_MUSIC (asm/audio/apply_next_map_music.asm). */
void apply_next_map_music(void);

/* Apply event-triggered tile replacements for the currently loaded tileset.
 * Port of LOAD_CURRENT_MAP_BLOCK_EVENTS (asm/overworld/map/load_current_map_block_events.asm).
 * Called from event scripts after event flags change to update map tiles. */
void load_current_map_block_events(void);

/* Load and decompress sanctuary tileset/tilemap/palette data for one sanctuary.
 * Port of LOAD_YOUR_SANCTUARY_LOCATION (asm/overworld/load_your_sanctuary_location.asm) +
 *        LOAD_YOUR_SANCTUARY_LOCATION_DATA  (asm/overworld/load_your_sanctuary_location_data.asm).
 * sanctuary_idx: 0-7, selects which sanctuary to load.
 * After this call, ert.buffer[sanctuary_idx * 0x800] holds the 30×32 tilemap,
 * ert.buffer[0x4000 + sanctuary_idx * 0x200] holds 8 sub-palettes (256 bytes),
 * and ppu.vram[$C000 + tile_index * 32] holds the packed tile graphics. */
void load_your_sanctuary_location(uint16_t sanctuary_idx);


/* Load a map palette by tileset combo and palette index.
 * fade_frames == 0: copies 192 bytes (6 sub-palettes) to PALETTES sub-palettes 2-7.
 * fade_frames > 0: smooth per-channel fade, then reloads and adjusts sprite palettes.
 * Port of LOAD_MAP_PALETTE (asm/system/palette/load_map_palette.asm). */
void load_map_palette(uint16_t tileset_combo, uint16_t palette_index,
                      uint16_t fade_frames);


/* Advance palette animation by one frame. Decrements timer, loads next
 * palette frame when expired. Call each frame from update_overworld_frame().
 * Port of ANIMATE_PALETTE (asm/system/animate_palette.asm). */
void animate_palette(void);

/* Load a palette animation frame from the palette animation buffer.
 * Copies 192 bytes (96 colors for sub-palettes 2-7) to palettes[32..127].
 * Port of LOAD_BG_PALETTE (asm/system/palette/load_bg_palette.asm). */
void load_bg_palette(uint16_t index);


/* ENTITY_COLLISION_X_OFFSET — center-to-left-edge offset per size code (17 entries) */
extern const int16_t entity_collision_x_offset[17];

/* ENTITY_COLLISION_Y_OFFSET — center-to-top offset per size code (17 entries) */
extern const int16_t entity_collision_y_offset[17];

/* SPRITE_HITBOX_ENABLE_TABLE — Y hitbox adjustment per size code (17 entries) */
extern const int16_t sprite_hitbox_enable[17];

/* Advance tileset animation by one frame. Decrements per-entry timers,
 * copies tile data to VRAM when expired. Call each frame.
 * Port of ANIMATE_TILESET (asm/system/animate_tileset.asm). */
void animate_tileset(void);

/* Shared 32 KB scratch buffer, time-shared across exclusive game phases.
 * Access via the appropriate union member to document which phase is active.
 *
 * Never concurrent — map arrangement is only live during overworld rendering,
 * sanctuary decomp only during the sanctuary screen, and battle sprite decomp
 * only during battle init. */
#define SHARED_SCRATCH_SIZE 0x8000

typedef union {
    uint8_t arrangement[SHARED_SCRATCH_SIZE]; /* Overworld: map tile arrangement lookup */
    uint8_t decomp[SHARED_SCRATCH_SIZE];      /* Sanctuary/battle: decompression staging */
} SharedScratch;

extern SharedScratch shared_scratch;

/* Convenience accessors */
#define arrangement_buffer (shared_scratch.arrangement)
#define decomp_staging     (shared_scratch.decomp)

#endif /* GAME_MAP_LOADER_H */
