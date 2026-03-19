/*
 * Sprite loading infrastructure — VRAM allocation, tile upload, spritemap management.
 *
 * Ports of:
 *   LOAD_SPRITE_GROUP_PROPERTIES   (asm/overworld/entity/load_sprite_group_properties.asm)
 *   ALLOCATE_SPRITE_VRAM           (asm/overworld/entity/allocate_sprite_vram.asm)
 *   UPLOAD_SPRITE_TO_VRAM          (asm/overworld/entity/upload_sprite_to_vram.asm)
 *   LOAD_OVERWORLD_SPRITEMAPS      (asm/overworld/entity/load_overworld_spritemaps.asm)
 *   FIND_FREE_7E4682               (asm/overworld/find_free_space_7E4682.asm)
 *   CLEAR_OVERWORLD_SPRITEMAPS     (asm/overworld/entity/clear_overworld_spritemaps.asm)
 *   RENDER_ENTITY_SPRITE           (asm/overworld/render_entity_sprite.asm)
 */
#ifndef ENTITY_SPRITE_H
#define ENTITY_SPRITE_H

#include "include/binary.h"

#include "core/types.h"

/* ---- VRAM Allocation ---- */

/* Max VRAM sprite allocation slots */
#define SPRITE_VRAM_TABLE_SIZE 88

/* VRAM allocation table — each byte: 0=free, bit 7 set=allocated */
extern uint8_t sprite_vram_table[SPRITE_VRAM_TABLE_SIZE];

/* ---- OVERWORLD_SPRITEMAPS buffer ---- */

/* 5-byte spritemap entry (matches assembly spritemap struct) */
#define SPRITEMAP_ENTRY_SIZE 5
#define OVERWORLD_SPRITEMAPS_COUNT 179
/* Assembly buffer is 0x380 bytes; CLEAR_OVERWORLD_SPRITEMAPS writes 0x380,
 * and FIND_FREE_7E4682 can scan up to byte 899. Use 0x384 for safety. */
#define OVERWORLD_SPRITEMAPS_SIZE 0x384

/* Special_flags 0xFF = free slot marker */
#define SPRITEMAP_FREE_MARKER 0xFF

extern uint8_t overworld_spritemaps[OVERWORLD_SPRITEMAPS_SIZE];

/* ---- Sprite grouping data ---- */

/* sprite_grouping struct layout (9 bytes header + variable spritepointerarray):
 *   [0] height (byte)
 *   [1] width (byte) — high nybble = tile width, low nybble = other param
 *   [2] size (byte) — sprite type/direction count
 *   [3] palette (byte)
 *   [4] hitbox_width_ud (byte)
 *   [5] hitbox_height_ud (byte)
 *   [6] hitbox_width_lr (byte)
 *   [7] hitbox_height_lr (byte)
 *   [8] spritebank (byte)
 *   [9..] spritepointerarray — 1 bank byte + N × 2-byte within-bank offsets
 */
#define SPRITE_GROUPING_HEADER_SIZE 9

/* Offsets within spritemap_config blob (C42AEB-C430EC) */
#define SMCFG_HITBOX_ENABLE_OFF    0          /* C42AEB: 17 × 2 bytes */
#define SMCFG_CONFIG_PTRS_OFF      34         /* SPRITEMAP_CONFIG_POINTERS (C42B0D): 17 × 4 bytes */
#define SMCFG_CONFIG_DATA_OFF      102        /* C42B51: variable */
#define SMCFG_VRAM_SLOT_TABLE_OFF  1185       /* C42F8C: 88 × 2 bytes */
#define SMCFG_TILE_INDEX_TABLE_OFF 1361       /* C4303C: 88 × 2 bytes */

/* VRAM base address for OBJ tiles */
#define VRAM_OBJ_BASE 0x4000

/* ---- Sprite graphics banks ---- */

#define SPRITE_BANK_COUNT 5
#define SPRITE_BANK_FIRST 0x11   /* SNES banks $11-$15 ($D1-$D5 HiROM) */

extern const uint8_t *sprite_banks[SPRITE_BANK_COUNT];
extern uint32_t sprite_bank_sizes[SPRITE_BANK_COUNT];

/* ---- Runtime-loaded support data ---- */

extern const uint8_t *sprite_grouping_ptr_table;
extern uint16_t sprite_grouping_ptr_count;
extern const uint8_t *sprite_grouping_data_buf;
extern uint32_t sprite_grouping_data_size;
extern const uint8_t *spritemap_config;
extern uint32_t spritemap_config_size;

/* Sprite tile dimensions computed by load_sprite_group_properties */
extern uint16_t new_sprite_tile_width;
extern uint16_t new_sprite_tile_height;

/* ---- Direction mapping (from ROM, constant) ---- */

/* SPRITE_DIRECTION_MAPPING_4_DIRECTION: maps entity direction to graphics offset.
 * Used by RENDER_ENTITY_SPRITE (C0A443) for entities with 4-directional sprites.
 * Diagonals collapse to their vertical cardinal direction. */
static const uint16_t sprite_direction_mapping_4dir[12] = {
    0, 0, 1, 2, 2, 2, 3, 0, 4, 5, 6, 7
};

/* SPRITE_DIRECTION_MAPPING_8_DIRECTION: maps entity direction to graphics offset.
 * Used by RENDER_ENTITY_SPRITE_8DIR (C0A794) for entities with 8-directional sprites.
 * Cardinals map to 0-3, diagonals map to 4-7. */
static const uint16_t sprite_direction_mapping_8dir[8] = {
    0, 4, 1, 5, 2, 6, 3, 7
};

/* ---- Public API ---- */

/* Load all sprite system data from binary assets */
void load_sprite_data(void);

/* Clear the OVERWORLD_SPRITEMAPS buffer (all special_flags = 0xFF) */
void clear_overworld_spritemaps(void);

/* Load sprite group properties for a given sprite ID.
 * Sets new_sprite_tile_width, new_sprite_tile_height.
 * Returns sprite_grouping.size value.
 * *out_grouping_offset is set to the byte offset within sprite_grouping_data_buf. */
uint8_t load_sprite_group_properties(uint16_t sprite_id, uint32_t *out_grouping_offset);

/* Allocate contiguous VRAM slots for a sprite.
 * Returns starting slot index, or 0x7FFF on failure. */
uint16_t allocate_sprite_vram(uint16_t num_tiles, uint16_t entity_hint);

/* Upload sprite tiles to VRAM (clears the VRAM region with zeros).
 * Returns starting slot index (same as allocate). */
uint16_t upload_sprite_to_vram(uint16_t width, uint16_t height, uint16_t hint);

/* Find free space in OVERWORLD_SPRITEMAPS for N entries.
 * Returns byte offset into overworld_spritemaps[], or >=0x7F00 on failure. */
uint16_t find_free_spritemap_space(uint16_t num_entries_bytes);

/* Load spritemap animation data from spritemap config.
 * buf_offset: offset into overworld_spritemaps[] from find_free_spritemap_space
 * vram_index: from allocate_sprite_vram
 * palette: sprite palette index
 * config_ptr: pointer to spritemap config entry
 * sprite_count: number of sprites per direction */
void load_overworld_spritemaps(uint16_t buf_offset, uint16_t vram_index,
                               uint8_t palette, const uint8_t *config_ptr,
                               uint8_t sprite_count);

/* Load sprite tile graphics to a specific VRAM position.
 * Port of LOAD_SPRITE_TILES_TO_VRAM (C4B1B8).
 * sprite_id: OVERWORLD_SPRITE enum value
 * sub_palette_idx: palette variant (0xFF = skip)
 * vram_base: VRAM word address to load to
 * Returns next available VRAM word address. */
uint16_t load_sprite_tiles_to_vram(uint16_t sprite_id, uint16_t sub_palette_idx,
                                    uint16_t vram_base);

/* Render entity sprite — upload current frame's tile data to ppu.vram[].
 * Called per-frame for entities that need their VRAM tiles updated. */
void render_entity_sprite(int16_t entity_offset);

/* Update entity sprite (8-dir) — sets use_8dir_sprites and renders.
 * Used for party member sprites with 8-directional walking animations.
 * Port of UPDATE_ENTITY_SPRITE (asm/overworld/update_entity_sprite.asm). */
void update_entity_sprite(int16_t entity_offset);

/* Copy one row of sprite tile data to VRAM, handling OBJ tile layout splits.
 * Returns the advanced VRAM destination address for the next row. */
uint16_t vram_copy_row_safe(uint16_t dest, const uint8_t *src, uint16_t size);

#endif /* ENTITY_SPRITE_H */
