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
#include "entity/sprite.h"
#include "entity/entity.h"
#include "data/assets.h"
#include "snes/ppu.h"
#include "core/log.h"
#include <string.h>

/* ---- Runtime state ---- */

uint8_t sprite_vram_table[SPRITE_VRAM_TABLE_SIZE];
uint8_t overworld_spritemaps[OVERWORLD_SPRITEMAPS_SIZE];

const uint8_t *sprite_grouping_ptr_table = NULL;
uint16_t sprite_grouping_ptr_count = 0;
const uint8_t *sprite_grouping_data_buf = NULL;
uint32_t sprite_grouping_data_size = 0;
const uint8_t *spritemap_config = NULL;
uint32_t spritemap_config_size = 0;

const uint8_t *sprite_banks[SPRITE_BANK_COUNT];
uint32_t sprite_bank_sizes[SPRITE_BANK_COUNT];

uint16_t new_sprite_tile_width;
uint16_t new_sprite_tile_height;

/* Raw buffers (compiled-in data, do NOT free) */
static const uint8_t *spr_grouping_ptrs_buf = NULL;
static const uint8_t *spr_grouping_data_raw = NULL;
static const uint8_t *spr_config_buf = NULL;
static const uint8_t *spr_bank_bufs[SPRITE_BANK_COUNT];

/* ---- Data Loading ---- */

void load_sprite_data(void) {
    size_t size;

    /* Sprite grouping pointer table (464 × 4 bytes) */
    size = ASSET_SIZE(ASSET_OVERWORLD_SPRITES_SPRITE_GROUPING_PTR_TABLE_BIN);
    spr_grouping_ptrs_buf = ASSET_DATA(ASSET_OVERWORLD_SPRITES_SPRITE_GROUPING_PTR_TABLE_BIN);
    sprite_grouping_ptr_table = spr_grouping_ptrs_buf;
    sprite_grouping_ptr_count = (uint16_t)(size / 4);

    /* Sprite grouping data (variable-length structs) */
    size = ASSET_SIZE(ASSET_OVERWORLD_SPRITES_SPRITE_GROUPING_DATA_BIN);
    spr_grouping_data_raw = ASSET_DATA(ASSET_OVERWORLD_SPRITES_SPRITE_GROUPING_DATA_BIN);
    sprite_grouping_data_buf = spr_grouping_data_raw;
    sprite_grouping_data_size = (uint32_t)size;

    /* Spritemap config tables (C42AEB-C430EC) */
    size = ASSET_SIZE(ASSET_OVERWORLD_SPRITES_SPRITEMAP_CONFIG_BIN);
    spr_config_buf = ASSET_DATA(ASSET_OVERWORLD_SPRITES_SPRITEMAP_CONFIG_BIN);
    spritemap_config = spr_config_buf;
    spritemap_config_size = (uint32_t)size;

    /* Sprite graphics banks 11-15 ($D1-$D5).
     * Bank files are named by their hex SNES bank number: 11.bin–15.bin.
     * These are decimal 11–15 in the asset family index. */
    for (int i = 0; i < SPRITE_BANK_COUNT; i++) {
        int bank_file = 11 + i;  /* decimal file number, not SPRITE_BANK_FIRST (0x11) */
        size = ASSET_SIZE(ASSET_OVERWORLD_SPRITES_BANKS(bank_file));
        spr_bank_bufs[i] = ASSET_DATA(ASSET_OVERWORLD_SPRITES_BANKS(bank_file));
        sprite_banks[i] = spr_bank_bufs[i];
        sprite_bank_sizes[i] = (uint32_t)size;
    }

    /* Initialize */
    memset(sprite_vram_table, 0, sizeof(sprite_vram_table));
    clear_overworld_spritemaps();
}


/*
 * CLEAR_OVERWORLD_SPRITEMAPS (C01A86)
 * Fills the first 0x380 bytes of OVERWORLD_SPRITEMAPS with 0xFF.
 */
void clear_overworld_spritemaps(void) {
    memset(overworld_spritemaps, 0xFF,
           (0x380 < OVERWORLD_SPRITEMAPS_SIZE) ? 0x380 : OVERWORLD_SPRITEMAPS_SIZE);
}

/* ---- Sprite Group Properties ---- */

/*
 * LOAD_SPRITE_GROUP_PROPERTIES (C01DED)
 *
 * Reads sprite_grouping struct for the given sprite ID.
 * Sets new_sprite_tile_width = width >> 4 (high nybble of width byte).
 * Sets new_sprite_tile_height = height byte.
 * Returns the size byte.
 */
uint8_t load_sprite_group_properties(uint16_t sprite_id, uint32_t *out_grouping_offset) {
    if (!sprite_grouping_ptr_table || !sprite_grouping_data_buf ||
        sprite_id >= sprite_grouping_ptr_count) {
        new_sprite_tile_width = 0;
        new_sprite_tile_height = 0;
        if (out_grouping_offset) *out_grouping_offset = 0;
        return 0;
    }

    /* Read 4-byte pointer from the pointer table.
     * This is a 24-bit ROM address (lo word + bank byte + padding) that
     * points into the sprite_grouping_data region starting at $EF1A7F. */
    uint32_t ptr_off = (uint32_t)sprite_id * 4;
    uint32_t rom_ptr = read_u32_le(sprite_grouping_ptr_table + ptr_off);

    /* Convert ROM address to ert.buffer offset.
     * The sprite_grouping_data_buf starts at ROM $EF1A7F.
     * Within-bank portion: (rom_ptr & 0xFFFF), base = 0x1A7F */
    uint16_t within_bank = (uint16_t)(rom_ptr & 0xFFFF);
    uint32_t buf_off = within_bank - 0x1A7F;

    if (buf_off + SPRITE_GROUPING_HEADER_SIZE > sprite_grouping_data_size) {
        new_sprite_tile_width = 0;
        new_sprite_tile_height = 0;
        if (out_grouping_offset) *out_grouping_offset = 0;
        return 0;
    }

    const uint8_t *sg = sprite_grouping_data_buf + buf_off;

    new_sprite_tile_height = sg[0];                     /* height */
    new_sprite_tile_width = (sg[1] >> 4) & 0x0F;       /* width high nybble */

    if (out_grouping_offset) *out_grouping_offset = buf_off;

    return sg[2];  /* size */
}

/* ---- VRAM Allocation ---- */

/*
 * ALLOCATE_SPRITE_VRAM (C01B96)
 *
 * Searches SPRITE_VRAM_TABLE for 'num_tiles' consecutive free slots.
 * Marks them allocated (bit 7 set, lower bits = entity_hint | 0x80).
 * Returns starting index, or 0x7FFF on failure.
 */
uint16_t allocate_sprite_vram(uint16_t num_tiles, uint16_t entity_hint) {
    if (num_tiles == 0 || num_tiles > SPRITE_VRAM_TABLE_SIZE)
        return 0x7FFF;

    uint16_t y = 0;
    while (y <= SPRITE_VRAM_TABLE_SIZE - num_tiles) {
        /* Check for num_tiles consecutive free slots starting at y */
        uint16_t count = 0;
        uint16_t i;
        for (i = 0; i < num_tiles; i++) {
            if (sprite_vram_table[y + i] != 0) {
                break;
            }
            count++;
        }

        if (count == num_tiles) {
            /* Found space — mark as allocated */
            uint8_t mark = (uint8_t)(entity_hint | 0x80);
            for (i = 0; i < num_tiles; i++) {
                sprite_vram_table[y + i] = mark;
            }
            return y;
        }

        /* Skip past the occupied slot */
        y += i + 1;
    }

    return 0x7FFF;  /* No space found */
}

/*
 * UPLOAD_SPRITE_TO_VRAM (C01C52)
 *
 * Allocates VRAM space and clears it (the ROM uploads zeros via DMA).
 * In the C port, we just clear the corresponding ppu.vram[] region.
 *
 * Parameters:
 *   width: sprite tile width
 *   height: sprite tile height
 *   hint: allocation hint (typically 0)
 *
 * Returns: starting VRAM slot index, or 0x7FFF on failure.
 */
uint16_t upload_sprite_to_vram(uint16_t width, uint16_t height, uint16_t hint) {
    /* Round up to even (matching ASM: INC, AND #$FFFE) */
    uint16_t adj_w = (width + 1) & 0xFFFE;
    uint16_t adj_h = (height + 1) & 0xFFFE;
    uint16_t num_tiles = (adj_w * adj_h) >> 2;

    uint16_t slot = allocate_sprite_vram(num_tiles, hint);
    if (slot >= 0x7FFF)
        return 0x7FFF;

    /* Clear the VRAM region for this sprite.
     * Each slot covers 0x20 VRAM words (256 bytes).
     * The VRAM address = SPRITE_VRAM_SLOT_TABLE[slot] + VRAM_OBJ_BASE. */
    if (spritemap_config && spritemap_config_size > SMCFG_VRAM_SLOT_TABLE_OFF + (slot + num_tiles) * 2) {
        const uint8_t *vram_table = spritemap_config + SMCFG_VRAM_SLOT_TABLE_OFF;
        for (uint16_t i = slot; i < slot + num_tiles; i++) {
            uint16_t vram_word_off = read_u16_le(vram_table + i * 2);
            uint16_t vram_addr = vram_word_off + VRAM_OBJ_BASE;
            /* Clear 0x20 words = 64 bytes at both upper and lower halves */
            uint32_t byte_off = (uint32_t)vram_addr * 2;
            if (byte_off + 64 <= sizeof(ppu.vram)) {
                memset(&ppu.vram[byte_off], 0, 64);
            }
            /* Lower half is at + 64*4*2 = +512 bytes */
            uint32_t byte_off2 = byte_off + 512;
            if (byte_off2 + 64 <= sizeof(ppu.vram)) {
                memset(&ppu.vram[byte_off2], 0, 64);
            }
        }
    }

    return slot;
}

/*
 * LOAD_SPRITE_TILES_TO_VRAM (C4B1B8)
 *
 * Loads sprite tile graphics to a specific VRAM position using the sprite
 * grouping data. Used by LOAD_OVERLAY_SPRITES to place overlay sprite
 * graphics in VRAM at OVERLAY_BASE.
 *
 * Parameters:
 *   sprite_id: OVERWORLD_SPRITE enum value
 *   sub_palette_idx: selects which sprite pointer from the grouping's array
 *                    (0xFF = skip loading, return vram_base unchanged)
 *   vram_base: destination VRAM word address
 *
 * Returns: next available VRAM word address (vram_base + width)
 */
uint16_t load_sprite_tiles_to_vram(uint16_t sprite_id, uint16_t sub_palette_idx,
                                    uint16_t vram_base) {
    if (sub_palette_idx == 0xFF)
        return vram_base;

    if (!sprite_grouping_ptr_table || !sprite_grouping_data_buf ||
        sprite_id >= sprite_grouping_ptr_count)
        return vram_base;

    /* Look up sprite grouping struct */
    uint32_t ptr_off = (uint32_t)sprite_id * 4;
    uint32_t rom_ptr = read_u32_le(sprite_grouping_ptr_table + ptr_off);
    uint16_t within_bank = (uint16_t)(rom_ptr & 0xFFFF);
    uint32_t buf_off = within_bank - 0x1A7F;

    if (buf_off + SPRITE_GROUPING_HEADER_SIZE > sprite_grouping_data_size)
        return vram_base;

    const uint8_t *sg = sprite_grouping_data_buf + buf_off;
    uint8_t raw_width = sg[1];                  /* width byte */
    uint16_t width_bytes = (uint16_t)raw_width * 2;  /* ASL: transfer size per half */
    uint8_t bank_byte = sg[8];                  /* sprite graphics bank */

    /* Read sprite data pointer from spritepointerarray[sub_palette_idx] */
    uint32_t spr_ptr_off = buf_off + 9 + (uint32_t)sub_palette_idx * 2;
    if (spr_ptr_off + 2 > sprite_grouping_data_size)
        return vram_base;
    uint16_t data_ptr = read_u16_le(sprite_grouping_data_buf + spr_ptr_off);
    data_ptr &= 0xFFFE;  /* clear bit 0 (flag bit) */

    /* Find sprite graphics bank */
    int bank_idx = (int)(bank_byte & 0x3F) - SPRITE_BANK_FIRST;
    if (bank_idx < 0 || bank_idx >= SPRITE_BANK_COUNT || !sprite_banks[bank_idx])
        return vram_base;

    const uint8_t *bank_data = sprite_banks[bank_idx];
    uint32_t bank_size = sprite_bank_sizes[bank_idx];

    if ((uint32_t)data_ptr + (uint32_t)width_bytes * 2 > bank_size)
        return vram_base;

    /* Copy upper half to VRAM at vram_base (PREPARE_VRAM_COPY mode 0) */
    uint32_t vram_byte = (uint32_t)vram_base * 2;
    if (vram_byte + width_bytes <= sizeof(ppu.vram))
        memcpy(&ppu.vram[vram_byte], &bank_data[data_ptr], width_bytes);

    /* Copy lower half to VRAM at vram_base + 256 words */
    uint32_t vram_byte2 = ((uint32_t)vram_base + 256) * 2;
    if (vram_byte2 + width_bytes <= sizeof(ppu.vram))
        memcpy(&ppu.vram[vram_byte2], &bank_data[data_ptr + width_bytes], width_bytes);

    /* Return next VRAM position: vram_base + raw_width */
    return vram_base + raw_width;
}

/* ---- Spritemap Buffer Allocation ---- */

/*
 * FIND_FREE_7E4682 (find_free_space_7E4682.asm)
 *
 * Searches the OVERWORLD_SPRITEMAPS ert.buffer for contiguous free space.
 * Free entries have special_flags == 0xFF.
 *
 * Parameter: num_entries_bytes = number of bytes needed (must be multiple of 5)
 * Returns: byte offset into overworld_spritemaps[], or >= 0x7F00 on failure.
 */
uint16_t find_free_spritemap_space(uint16_t num_entries_bytes) {
    uint16_t x = 0;
    uint16_t start = 0;

    while (x < 0x380) {
        /* Check if this entry is free */
        if (x + 4 >= OVERWORLD_SPRITEMAPS_SIZE ||
            overworld_spritemaps[x + 4] != SPRITEMAP_FREE_MARKER) {
            /* Occupied — skip past and reset search */
            x += SPRITEMAP_ENTRY_SIZE;
            start = x;
            continue;
        }

        /* Free entry found — check if we have enough contiguous space */
        if (x + num_entries_bytes >= 0x380) {
            return 0x7FFF;  /* Not enough space at end */
        }

        /* Scan forward to verify all entries in range are free */
        uint16_t scan = start;
        bool all_free = true;
        while (scan < start + num_entries_bytes) {
            if (overworld_spritemaps[scan + 4] != SPRITEMAP_FREE_MARKER) {
                /* Not free — skip past */
                start = scan + SPRITEMAP_ENTRY_SIZE;
                x = start;
                all_free = false;
                break;
            }
            scan += SPRITEMAP_ENTRY_SIZE;
        }

        if (all_free) {
            return start;
        }
    }

    return 0x7FFF;
}

/* ---- Spritemap Loading ---- */

/*
 * LOAD_OVERWORLD_SPRITEMAPS (C01D38)
 *
 * Loads spritemap animation data from the spritemap config tables
 * into the OVERWORLD_SPRITEMAPS ert.buffer.
 *
 * The config data format (pointed to by SPRITEMAP_CONFIG_POINTERS entries):
 *   byte[0]: sprite count per frame
 *   byte[1]: (unused here)
 *   Then for each sprite entry (5 bytes):
 *     [0] y_offset
 *     [1] tile (unused — overwritten by SPRITE_TILE_INDEX_TABLE lookup)
 *     [2] flags (ORed with palette and tile high byte)
 *     [3] x_offset
 *     [4] special_flags
 *
 * The function loads 2 directions worth of spritemaps.
 *
 * Parameters:
 *   buf_offset: byte offset in overworld_spritemaps[]
 *   vram_index: VRAM slot from allocate_sprite_vram
 *   palette: palette number (0-7)
 *   config_ptr: pointer to the spritemap config entry data
 *   sprite_count: sprites per frame from config_ptr[0]
 */
void load_overworld_spritemaps(uint16_t buf_offset, uint16_t vram_index,
                               uint8_t palette, const uint8_t *config_ptr,
                               uint8_t sprite_count) {
    if (!spritemap_config || !config_ptr)
        return;

    const uint8_t *tile_table = spritemap_config + SMCFG_TILE_INDEX_TABLE_OFF;
    uint16_t x = buf_offset;
    const uint8_t *src = config_ptr + 2;  /* Skip the 2-byte header */

    LOG_TRACE("load_overworld_spritemaps: buf_off=%u vram_idx=%u pal=%u count=%u\n",
              buf_offset, vram_index, palette, sprite_count);

    /* Load 2 directions of spritemaps */
    for (int dir = 0; dir < 2; dir++) {
        for (uint8_t s = 0; s < sprite_count; s++) {
            if (x + SPRITEMAP_ENTRY_SIZE > OVERWORLD_SPRITEMAPS_SIZE)
                return;

            /* y_offset */
            overworld_spritemaps[x + 0] = src[0];

            /* tile: look up from SPRITE_TILE_INDEX_TABLE using (vram_index + s) */
            uint16_t tile_idx = vram_index + s;
            uint16_t tile_val = 0;
            if (tile_idx < SPRITE_VRAM_TABLE_SIZE) {
                tile_val = read_u16_le(tile_table + tile_idx * 2);
            }
            /* Low byte → tile number */
            overworld_spritemaps[x + 1] = (uint8_t)(tile_val & 0xFF);

            /* flags: merge palette, tile high byte, and original flags */
            uint8_t orig_flags = src[2] & 0xFE;  /* Clear bit 0 */
            uint8_t tile_hi = (uint8_t)((tile_val >> 8) & 0xFF);
            overworld_spritemaps[x + 2] = orig_flags | palette | tile_hi;

            /* x_offset */
            overworld_spritemaps[x + 3] = src[3];

            /* special_flags */
            overworld_spritemaps[x + 4] = src[4];

            src += SPRITEMAP_ENTRY_SIZE;
            x += SPRITEMAP_ENTRY_SIZE;
        }
    }
}

/* ---- Per-Frame Sprite Tile Upload ---- */

/*
 * PREPARE_VRAM_COPY_ROW_SAFE (C0A56E) — helper for OBJ VRAM tile upload.
 *
 * SNES OBJ tiles are arranged in 0x100-word (512 byte) groups where:
 *   - Words 0x??00-0x??FF hold the upper 8-pixel rows of a tile row
 *   - Words 0x??00+0x100 hold the lower 8-pixel rows of those same tiles
 *
 * When a sprite data row crosses a 0x100-word boundary, the data past the
 * boundary belongs to the NEXT tile group's upper half (not the lower half
 * of the current group). This function handles that split.
 *
 * After the copy, advances the destination for the next row:
 *   - If currently in upper half: move to lower half (+0x100)
 *   - If currently in lower half: compute advance to next tile group
 *
 * Parameters:
 *   dest: VRAM word address (destination)
 *   src:  pointer to source tile data
 *   size: bytes to copy (byte_width of sprite)
 *
 * Returns: advanced VRAM destination for the next row.
 */
uint16_t vram_copy_row_safe(uint16_t dest, const uint8_t *src, uint16_t size) {
    uint16_t word_count = size >> 1;
    uint16_t end_word = dest + word_count - 1;

    if ((end_word ^ dest) & 0x0100) {
        /* Row crosses 0x100-word boundary — split into two copies.
         * ASM lines 10-49: save state, copy first part, copy second part
         * at boundary + 0x100, restore state. */
        uint16_t boundary = (dest + 0x0100) & 0xFF00;
        uint16_t first_words = boundary - dest;
        uint16_t first_bytes = first_words * 2;

        /* Copy first part: dest to boundary */
        uint32_t byte_addr1 = (uint32_t)dest * 2;
        if (byte_addr1 + first_bytes <= sizeof(ppu.vram)) {
            memcpy(&ppu.vram[byte_addr1], src, first_bytes);
        }

        /* Copy second part: boundary + 0x100 (skips lower half region) */
        uint16_t second_dest = boundary + 0x0100;
        uint16_t second_bytes = size - first_bytes;
        uint32_t byte_addr2 = (uint32_t)second_dest * 2;
        if (byte_addr2 + second_bytes <= sizeof(ppu.vram)) {
            memcpy(&ppu.vram[byte_addr2], src + first_bytes, second_bytes);
        }
    } else {
        /* No split needed — contiguous copy */
        uint32_t byte_addr = (uint32_t)dest * 2;
        if (byte_addr + size <= sizeof(ppu.vram)) {
            memcpy(&ppu.vram[byte_addr], src, size);
        }
    }

    /* Advance dest for next row (ASM lines 54-83) */
    if (!(dest & 0x0100)) {
        /* Upper half → move to lower half */
        dest += 0x0100;
    } else {
        /* Lower half → compute next tile group */
        uint16_t advance = ((size + 0x20) & 0xFFC0) >> 1;
        uint16_t next = dest + advance;
        if ((next ^ dest) & 0x0100) {
            dest = next;  /* Crossed bit-8 → keep */
        } else {
            dest = next - 0x0100;  /* Didn't cross → adjust */
        }
    }

    return dest;
}

/* All-zero ert.buffer for blank tile fill (port of BLANK_TILE_DATA). */
static const uint8_t blank_tile_row[512] = {0};

/*
 * RENDER_ENTITY_SPRITE (C0A443)
 *
 * Uploads the current animation frame's tile graphics to ppu.vram[].
 * This is called once per frame for each entity whose appearance has changed.
 *
 * The entity's graphics_ptr_lo/hi point into the sprite_grouping data,
 * specifically at the spritepointerarray. Each direction has 4 bytes
 * (2 frame pointers × 2 bytes). Each frame pointer is a within-bank
 * ROM offset pointing to the actual tile data in the sprite graphics banks.
 *
 * Flow:
 *   1. Determine direction → graphics offset (direction * 4 bytes)
 *   2. Determine frame → +0 or +2 within direction
 *   3. Read the 2-byte within-bank pointer at that position
 *   4. Check surface flags and pre-fill blank rows if in water
 *   5. Use entity's sprite bank to find the correct bank data
 *   6. Copy tile data from bank data to ppu.vram[] via vram_copy_row_safe
 */
void render_entity_sprite(int16_t ent) {
    uint16_t tile_height = entities.tile_heights[ent];
    uint16_t byte_width = entities.byte_widths[ent];
    uint16_t vram_addr = entities.vram_address[ent];
    uint16_t gfx_ptr_lo = entities.graphics_ptr_lo[ent];
    uint16_t bank_num = entities.graphics_sprite_bank[ent];

    if (tile_height == 0 || byte_width == 0)
        return;

    /* gfx_ptr_lo is a ert.buffer offset into sprite_grouping_data_buf, pointing
     * to the start of the spritepointerarray (offset 9 in sprite_grouping).
     * The bank byte is at offset 8 (read separately into graphics_sprite_bank).
     * The array is: [word0][word1][word2]... where each word is a within-bank
     * offset into the sprite graphics bank. */

    /* Determine the direction mapping.
     * Entities with use_8dir_sprites flag use SPRITE_DIRECTION_MAPPING_8_DIRECTION
     * (preserves diagonal directions → separate diagonal sprite frames).
     * Other entities use SPRITE_DIRECTION_MAPPING_4_DIRECTION
     * (diagonals collapse to cardinal directions). */
    uint16_t direction = (uint16_t)entities.directions[ent];
    uint16_t dir_offset = 0;
    if (entities.use_8dir_sprites[ent] && direction < 8) {
        dir_offset = sprite_direction_mapping_8dir[direction];
    } else if (direction < 12) {
        dir_offset = sprite_direction_mapping_4dir[direction];
    }

    /* The spritepointerarray: each direction occupies 4 bytes (2 frame ptrs × 2 bytes).
     * gfx_ptr_lo already points to the first word (no bank byte to skip). */
    uint32_t frame_ptr_off = (uint32_t)gfx_ptr_lo + (uint32_t)dir_offset * 4;

    /* Select frame: animation_frame chooses which 2-byte pointer within the direction */
    int16_t frame = entities.animation_frame[ent];
    if (frame > 0) {
        frame_ptr_off += 2;  /* Frame 1 */
    }

    if (frame_ptr_off + 2 > sprite_grouping_data_size)
        return;

    /* Read the 2-byte within-bank offset pointing to actual tile data.
     * ASM: LDA [$02] → STA ENTITY_CURRENT_DISPLAYED_SPRITES,Y
     * Low bits are flags: bit 0 = spritemap direction chunk selector,
     * bit 1 = surface flag indicator. */
    uint16_t tile_data_offset = read_u16_le(sprite_grouping_data_buf + frame_ptr_off);

    uint16_t vram_dest = vram_addr;  /* word address */

    /* Surface flag handling (ASM lines 108-131):
     * If tile_data_offset bit 1 is NOT set AND surface_flags has SHALLOW_WATER
     * (bit 3), pre-fill 1-2 rows of blank tiles to hide lower body in water.
     * This uses fixed-source DMA (mode 3) from BLANK_TILE_DATA (all zeros).
     *
     * SHALLOW_WATER (0x08): 1 blank row — lower body hidden (wading)
     * DEEP_WATER (0x0C = SHALLOW_WATER | CAUSES_SUNSTROKE): 2 blank rows —
     *   only head visible (e.g. Deep Darkness deep water) */
    if (!(tile_data_offset & 0x0002)) {
        uint16_t sf = entities.surface_flags[ent];
        if (sf & 0x0008) {  /* SHALLOW_WATER (bit 3) */
            /* Upload one blank row */
            vram_dest = vram_copy_row_safe(vram_dest, blank_tile_row, byte_width);
            tile_height--;
            if (tile_height == 0)
                return;

            if (sf & 0x0004) {  /* CAUSES_SUNSTROKE (bit 2) — present in DEEP_WATER */
                /* Upload another blank row */
                vram_dest = vram_copy_row_safe(vram_dest, blank_tile_row, byte_width);
                tile_height--;
                if (tile_height == 0)
                    return;
            }
        }
    }

    /* Assembly writes ENTITY_CURRENT_DISPLAYED_SPRITES here (after water handling) */
    entities.current_displayed_sprites[ent] = tile_data_offset;

    /* Find the sprite graphics bank.
     * ROM stores HiROM bank bytes ($D1-$D5); mask with 0x3F to get $11-$15. */
    int bank_idx = (int)(bank_num & 0x3F) - SPRITE_BANK_FIRST;
    if (bank_idx < 0 || bank_idx >= SPRITE_BANK_COUNT)
        return;
    if (!sprite_banks[bank_idx])
        return;

    const uint8_t *bank_data = sprite_banks[bank_idx];
    uint32_t bank_size = sprite_bank_sizes[bank_idx];

    /* Mask flag bits from tile_data_offset to get the data address.
     * 4-dir sprites (render_entity_sprite.asm): AND #$FFF0 (low 4 bits are flags)
     * 8-dir sprites (C0A794.asm): AND #$FFFE (only bit 0 is a flag) */
    uint16_t data_off = entities.use_8dir_sprites[ent]
        ? (tile_data_offset & 0xFFFE)
        : (tile_data_offset & 0xFFF0);

    if (data_off >= bank_size)
        return;

    LOG_TRACE("render_entity_sprite: ent=%d bank_idx=%d data_off=0x%04X vram=0x%04X "
              "w=%u h=%u tile_data_off=0x%04X\n",
              ent, bank_idx, data_off, vram_addr, byte_width, tile_height, tile_data_offset);

    /* Copy tile data rows to ppu.vram[] via vram_copy_row_safe,
     * which handles SNES OBJ tile VRAM layout and boundary splits.
     * ASM: @UNKNOWN15 loop (lines 144-152). */
    for (uint16_t row = 0; row < tile_height; row++) {
        uint32_t src_off = (uint32_t)data_off + (uint32_t)row * byte_width;
        if (src_off + byte_width > bank_size)
            break;

        vram_dest = vram_copy_row_safe(vram_dest, bank_data + src_off, byte_width);
    }
}

/* UPDATE_ENTITY_SPRITE (asm/overworld/update_entity_sprite.asm)
 * Far wrapper that sets use_8dir_sprites and calls the 8-direction render path.
 * Used for party member sprites which have 8-directional walking animations. */
void update_entity_sprite(int16_t ent) {
    entities.use_8dir_sprites[ent] = 1;
    render_entity_sprite(ent);
}
