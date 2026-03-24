/*
 * Entity system core — init, allocation, linked list management.
 *
 * Ports of:
 *   INIT_ENTITY_SYSTEM      (asm/overworld/init_entity_system.asm)
 *   INIT_ENTITY_WIPE        (asm/overworld/init_entity.asm)
 *   INIT_ENTITY             (asm/overworld/init_entity.asm)
 *   ALLOCATE_ENTITY_SLOT    (asm/overworld/allocate_entity_slot.asm)
 *   LINK_ENTITY_TO_LIST     (asm/overworld/link_entity_to_list.asm)
 *   UNLINK_ENTITY_FROM_LIST (asm/overworld/unlink_entity_from_list.asm)
 *   APPEND_ENTITY_TO_LIST   (asm/overworld/append_entity_to_list.asm)
 *   ALLOCATE_ENTITY_SCRIPT_SLOT (asm/overworld/allocate_entity_script_slot.asm)
 *   FREE_ENTITY_SCRIPTS     (asm/overworld/free_entity_scripts.asm)
 *   DEACTIVATE_ENTITY       (asm/overworld/deactivate_entity.asm)
 */
#include "entity/entity.h"
#include "entity/sprite.h"
#include "entity/script.h"
#include "data/event_script_data.h"
#include "data/assets.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "game/overworld.h"
#include "include/binary.h"
#include "snes/ppu.h"
#include "core/log.h"
#include <math.h>
#include <string.h>

/* Global entity system state (large SoA structs — kept separate) */
EntitySystem entities;
ScriptSystem scripts;
SpritePriorityQueue sprite_priority[4];

/* Consolidated entity runtime state */
EntityRuntimeState ert;

/* Forward declarations for callbacks (defined in callbacks.c) */
extern void build_entity_draw_list(void);

/*
 * ALLOCATE_ENTITY_SLOT (C09C02)
 *
 * Walks the free entity list (linked through LAST_ENTITY → next_entity chain)
 * to find a slot within [alloc_min_slot, alloc_max_slot).
 * Returns the allocated slot offset in *out_offset.
 * Returns true on success, false if no slot available.
 *
 * The ROM's free list is threaded through the same next_entity array.
 * LAST_ENTITY points to the head of the free list.
 */
static bool allocate_entity_slot(int16_t *out_offset) {
    if (ert.last_allocated_script < 0)
        return false;

    int16_t prev = ENTITY_NONE;
    int16_t cur = entities.last_entity;
    if (cur < 0)
        return false;

    int16_t min_slot = (int16_t)entities.alloc_min_slot;
    int16_t max_slot = (int16_t)entities.alloc_max_slot;

    while (cur >= 0) {
        if (cur >= min_slot && cur < max_slot) {
            /* Found a usable slot — unlink it from the free list */
            if (prev < 0) {
                /* Removing from head of free list */
                entities.last_entity = entities.next_entity[cur];
            } else {
                entities.next_entity[prev] = entities.next_entity[cur];
            }
            *out_offset = cur;
            return true;
        }
        prev = cur;
        cur = entities.next_entity[cur];
    }
    return false;
}

/*
 * ALLOCATE_ENTITY_SCRIPT_SLOT (C09D03)
 *
 * Pops a script slot from the free script list.
 * Returns the script offset in *out_offset.
 */
static bool allocate_script_slot(int16_t *out_offset) {
    int16_t slot = ert.last_allocated_script;
    if (slot < 0)
        return false;
    ert.last_allocated_script = scripts.next_script[slot];
    *out_offset = slot;
    return true;
}

/*
 * FREE_ENTITY_SCRIPTS (C09C99)
 *
 * Returns all scripts chained from an entity back to the free list.
 */
static void free_entity_scripts(int16_t entity_offset) {
    int16_t script_idx = entities.script_index[entity_offset];
    if (script_idx < 0)
        return;

    /* Walk to end of script chain */
    int16_t old_free = ert.last_allocated_script;
    ert.last_allocated_script = script_idx;
    int16_t cur = script_idx;
    while (scripts.next_script[cur] >= 0) {
        cur = scripts.next_script[cur];
    }
    scripts.next_script[cur] = old_free;
}

/*
 * CLEAR_SPRITE_TICK_CALLBACK (clear_sprite_tick_callback.asm)
 *
 * Sets the tick callback to MOVEMENT_NOP — a harmless RTL.
 * Port of clear_sprite_tick_callback.asm:
 *   LDA #.LOWORD(MOVEMENT_NOP) / STA ENTITY_TICK_CALLBACK_LOW,X
 *   LDA #.HIWORD(MOVEMENT_NOP) / STA ENTITY_TICK_CALLBACK_HIGH,X
 *
 * CRITICAL: tick_callback_hi high byte must be $00 (bits 14-15 clear).
 *   bit 15 clear = tick callback ENABLED (dispatched each frame)
 *   bit 14 clear = entity NOT frozen (movement callbacks run)
 * When entity scripts later set a real callback via opcode 0x08
 * (SET_TICK_CALLBACK), they only write the low byte (bank) of
 * tick_callback_hi, preserving the high byte. If the high byte
 * has bit 15 set, the callback stays permanently disabled. */
static void clear_sprite_tick_callback(int16_t entity_offset) {
    entities.tick_callback_lo[entity_offset] = 0;
    entities.tick_callback_hi[entity_offset] = 0;  /* flags=0: bit 15 clear = enabled */
}

/*
 * FIND_ENTITY_PREDECESSOR — find the entity before 'target' in the active list.
 * Returns ENTITY_NONE if target is first.
 */
static int16_t find_entity_predecessor(int16_t target) {
    int16_t cur = entities.first_entity;
    if (cur == target)
        return ENTITY_NONE;
    while (cur >= 0) {
        if (entities.next_entity[cur] == target)
            return cur;
        cur = entities.next_entity[cur];
    }
    return ENTITY_NONE;
}

/*
 * UNLINK_ENTITY_FROM_LIST (C09C73)
 */
static void unlink_entity_from_list(int16_t entity_offset) {
    int16_t pred = find_entity_predecessor(entity_offset);
    int16_t next = entities.next_entity[entity_offset];

    if (pred < 0) {
        entities.first_entity = next;
    } else {
        entities.next_entity[pred] = next;
    }

    if (entity_offset == ert.next_active_entity) {
        ert.next_active_entity = next;
    }
}

/*
 * APPEND_ENTITY_TO_LIST (C09C8F)
 *
 * Appends entity to the free list tail (LAST_ENTITY chain).
 */
static void append_entity_to_list(int16_t entity_offset) {
    entities.next_entity[entity_offset] = entities.last_entity;
    entities.last_entity = entity_offset;
}

/*
 * LINK_ENTITY_TO_LIST (C09C57)
 *
 * Appends entity to the END of the active list (FIRST_ENTITY chain).
 */
static void link_entity_to_list(int16_t entity_offset) {
    entities.next_entity[entity_offset] = ENTITY_NONE;

    if (entities.first_entity < 0) {
        entities.first_entity = entity_offset;
    } else {
        int16_t cur = entities.first_entity;
        while (entities.next_entity[cur] >= 0) {
            cur = entities.next_entity[cur];
        }
        entities.next_entity[cur] = entity_offset;
    }
}

/*
 * INIT_ENTITY_SYSTEM (C0927C)
 */
void entity_system_init(void) {
    entities.first_entity = ENTITY_NONE;

    /* Build entity free list: 0 → 1 → 2 → ... → (MAX_ENTITIES-1) → -1 */
    for (int i = 0; i < MAX_ENTITIES - 1; i++) {
        entities.next_entity[i] = (int16_t)(i + 1);
    }
    entities.next_entity[MAX_ENTITIES - 1] = ENTITY_NONE;
    entities.last_entity = 0;
    ert.last_allocated_script = 0;

    /* Build script free list: 0 → 1 → 2 → ... → (MAX_SCRIPTS-1) → -1 */
    for (int i = 0; i < MAX_SCRIPTS - 1; i++) {
        scripts.next_script[i] = (int16_t)(i + 1);
    }
    scripts.next_script[MAX_SCRIPTS - 1] = ENTITY_NONE;

    /* Clear script table (all entities unassigned) */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entities.script_table[i] = ENTITY_NONE;
        entities.use_8dir_sprites[i] = 0;
    }

    /* Clear spritemap and tick callback high words (hidden / disabled) */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entities.spritemap_ptr_hi[i] = 0;
        entities.tick_callback_hi[i] = 0;
    }

    /* Clear draw priorities */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entities.draw_priority[i] = 0;
    }

    /* Clear sprite priority queues */
    for (int i = 0; i < 4; i++) {
        sprite_priority[i].offset = 0;
    }

    /* Clear BG scroll tables (assembly: @CLEAR_BG_OFFSETS loop) */
    memset(ert.entity_bg_h_offset_lo, 0, sizeof(ert.entity_bg_h_offset_lo));
    memset(ert.entity_bg_v_offset_lo, 0, sizeof(ert.entity_bg_v_offset_lo));
    memset(ert.entity_bg_h_offset_hi, 0, sizeof(ert.entity_bg_h_offset_hi));
    memset(ert.entity_bg_v_offset_hi, 0, sizeof(ert.entity_bg_v_offset_hi));
    memset(ert.entity_bg_h_velocity_lo, 0, sizeof(ert.entity_bg_h_velocity_lo));
    memset(ert.entity_bg_v_velocity_lo, 0, sizeof(ert.entity_bg_v_velocity_lo));
    memset(ert.entity_bg_h_velocity_hi, 0, sizeof(ert.entity_bg_h_velocity_hi));
    memset(ert.entity_bg_v_velocity_hi, 0, sizeof(ert.entity_bg_v_velocity_hi));

    ert.disable_actionscript = 0;
    ert.actionscript_state = 0;
}

/*
 * REBUILD_ENTITY_FREE_LIST (asm/overworld/entity/rebuild_entity_free_list.asm)
 *
 * Rebuilds the free entity list by:
 * 1. Walking the current free list, marking each entry with a sentinel (0x8000)
 * 2. Scanning all entity offsets from 58 down to 0, re-chaining any slot
 *    that was marked (i.e. was free, not in the active list)
 *
 * This produces a properly ordered free list (ascending by offset).
 * Called after operations that directly assign entity slots (e.g. ADD_PARTY_MEMBER).
 */
void rebuild_entity_free_list(void) {
    /* Phase 1: Walk current free list, mark each free slot with sentinel */
    int16_t cur = entities.last_entity;
    while (cur >= 0) {
        int16_t next = entities.next_entity[cur];
        entities.next_entity[cur] = (int8_t)-128;  /* sentinel: free but not yet re-linked */
        cur = next;
    }

    /* Phase 2: Scan from highest slot down to 0, rebuild free list */
    int16_t new_head = ENTITY_NONE;
    for (int16_t x = MAX_ENTITIES - 1; x >= 0; x--) {
        if (entities.next_entity[x] == (int8_t)-128) {
            entities.next_entity[x] = new_head;
            new_head = x;
        }
    }
    entities.last_entity = new_head;
}

/*
 * INIT_ENTITY_WIPE + INIT_ENTITY (init_entity.asm)
 *
 * INIT_ENTITY_WIPE: clears new_entity vars, sets allocation range to full,
 *                   then falls through to INIT_ENTITY.
 * INIT_ENTITY: allocates slot + script, sets up entity tables, links to active list,
 *              then initializes the script PC from EVENT_SCRIPT_POINTERS.
 */
int16_t entity_init_wipe(uint16_t script_id) {
    ert.new_entity_pos_z = 0;
    memset(ert.new_entity_var, 0, sizeof(ert.new_entity_var));
    ert.new_entity_priority = 0;
    entities.alloc_min_slot = 0;
    entities.alloc_max_slot = MAX_ENTITIES;

    return entity_init(script_id, 0, 0);
}

int16_t entity_init(uint16_t script_id, int16_t x, int16_t y) {
    uint16_t saved_min = entities.alloc_min_slot;
    uint16_t saved_max = entities.alloc_max_slot;

    int16_t ent_offset;
    if (!allocate_entity_slot(&ent_offset)) {
        entities.alloc_min_slot = saved_min;
        entities.alloc_max_slot = saved_max;
        return -1;
    }

    int16_t script_offset;
    if (!allocate_script_slot(&script_offset)) {
        /* Return entity to free list */
        append_entity_to_list(ent_offset);
        entities.alloc_min_slot = saved_min;
        entities.alloc_max_slot = saved_max;
        return -1;
    }

    entities.script_index[ent_offset] = script_offset;
    scripts.next_script[script_offset] = ENTITY_NONE;

    /* Default callbacks */
    entities.move_callback[ent_offset] = 0;       /* APPLY_ENTITY_DELTA_POSITION_ENTRY2 */
    entities.screen_pos_callback[ent_offset] = 0;  /* ENTITY_SCREEN_COORDS_BG1 (default) */
    entities.draw_callback[ent_offset] = 0;        /* DRAW_ENTITY_SPRITE (default) */

    /* Copy new_entity vars */
    for (int i = 0; i < 8; i++) {
        entities.var[i][ent_offset] = ert.new_entity_var[i];
    }
    entities.draw_priority[ent_offset] = ert.new_entity_priority;

    /* Position */
    entities.frac_x[ent_offset] = 0x8000;
    entities.frac_y[ent_offset] = 0x8000;
    entities.frac_z[ent_offset] = 0x8000;
    entities.abs_x[ent_offset] = x;
    entities.screen_x[ent_offset] = x;
    entities.abs_y[ent_offset] = y;
    entities.screen_y[ent_offset] = y;
    entities.abs_z[ent_offset] = ert.new_entity_pos_z;

    /* Link to active list */
    link_entity_to_list(ent_offset);

    /* Store script ID */
    entities.script_table[ent_offset] = (int16_t)script_id;

    /* animation_frame = -1 means HIDDEN — call_entity_draw() skips
     * entities with animation_frame < 0. The entity's script or a
     * callroutine (RENDER_ENTITY_SPRITE_ME1/ME2, INITIALIZE_PARTY_MEMBER_ENTITY,
     * etc.) must set animation_frame >= 0 to make the entity visible.
     * IMPORTANT: cr_update_entity_animation() XORs frame with 2, so
     * -1 ^ 2 = -3 and -3 ^ 2 = -1 — the entity stays invisible forever
     * unless explicitly set to >= 0 first. */
    entities.animation_frame[ent_offset] = -1;
    entities.delta_frac_x[ent_offset] = 0;
    entities.delta_x[ent_offset] = 0;
    entities.delta_frac_y[ent_offset] = 0;
    entities.delta_y[ent_offset] = 0;
    entities.delta_frac_z[ent_offset] = 0;
    entities.delta_z[ent_offset] = 0;

    /* Clear tick callback */
    clear_sprite_tick_callback(ent_offset);

    /* Initialize script from event script pointer table */
    event_script_init(script_id, ent_offset, script_offset);

    /* Restore allocation range */
    entities.alloc_min_slot = saved_min;
    entities.alloc_max_slot = saved_max;

    return ent_offset;
}

/*
 * DEACTIVATE_ENTITY (C09C3B)
 */
void deactivate_entity(int16_t entity_offset) {
    if (entities.script_table[entity_offset] < 0)
        return;

    entities.script_table[entity_offset] = ENTITY_NONE;
    clear_sprite_tick_callback(entity_offset);
    free_entity_scripts(entity_offset);
    unlink_entity_from_list(entity_offset);
    append_entity_to_list(entity_offset);
}

/*
 * CREATE_ENTITY (asm/overworld/create_entity.asm)
 *
 * Creates a fully-initialized entity with sprite graphics loaded into VRAM
 * and spritemaps loaded into the OVERWORLD_SPRITEMAPS ert.buffer.
 *
 * Parameters:
 *   sprite: OVERWORLD_SPRITE enum value (index into SPRITE_GROUPING_PTR_TABLE)
 *   script: EVENT_SCRIPT enum value (event script to run)
 *   index:  -1 = auto-allocate slot (0-21), else force specific slot
 *   x, y:   initial position
 *
 * Returns: entity slot (0-21) on success, -1 on failure.
 */
int16_t create_entity(uint16_t sprite, uint16_t script, int16_t index,
                      int16_t x, int16_t y) {
    /* ASM line 38-44: debug mode check (skip in port) */

    /* ASM lines 46-55: Load sprite group properties */
    uint32_t grouping_offset;
    uint8_t sprite_size = load_sprite_group_properties(sprite, &grouping_offset);

    /* ASM lines 57-61: Upload sprite tiles to VRAM (clears VRAM region).
     * Third param = hint (Y register) = entity slot index, or -1 for auto-allocate. */
    uint16_t vram_slot = upload_sprite_to_vram(
        new_sprite_tile_width, new_sprite_tile_height, (uint16_t)index);
    /* ASM lines 62-65: wait loop — in C, just check result */
    if (vram_slot >= 0x7FFF) {
        return -1;
    }

    /* ASM lines 66-76: Look up spritemap config and allocate spritemap ert.buffer space.
     * sprite_size indexes into SPRITEMAP_CONFIG_POINTERS (spritemap config pointer table).
     * The pointed-to config entry has [0]=sprite_count, then spritemap data. */
    const uint8_t *config_entry = NULL;
    uint8_t sprites_per_frame = 0;
    if (spritemap_config && spritemap_config_size > SMCFG_CONFIG_PTRS_OFF + (uint32_t)sprite_size * 4 + 4) {
        /* Read the 4-byte far pointer from SPRITEMAP_CONFIG_POINTERS[sprite_size] */
        const uint8_t *ptrs = spritemap_config + SMCFG_CONFIG_PTRS_OFF;
        uint32_t config_rom_addr = read_u32_le(ptrs + (uint32_t)sprite_size * 4);
        /* Convert ROM address to offset within spritemap_config blob.
         * SPRITEMAP_CONFIG_POINTERS data has far pointers into the range C42B51+.
         * Within-bank offset = (config_rom_addr & 0xFFFF) - 0x2AEB (blob base). */
        uint16_t config_within = (uint16_t)(config_rom_addr & 0xFFFF);
        uint32_t config_off = config_within - 0x2AEB;
        if (config_off < spritemap_config_size) {
            config_entry = spritemap_config + config_off;
            sprites_per_frame = config_entry[0];
        }
    }

    /* Allocate space in OVERWORLD_SPRITEMAPS: sprites_per_frame × sizeof(spritemap) × 2 dirs */
    uint16_t smap_bytes = (uint16_t)sprites_per_frame * SPRITEMAP_ENTRY_SIZE * 2;
    uint16_t smap_offset = find_free_spritemap_space(smap_bytes);
    if (smap_offset >= 0x7F00) {
        return -1;
    }

    /* ASM lines 82-83: Set draw priority */
    ert.new_entity_priority = 1;

    /* ASM lines 90-95: Read palette from sprite_grouping.palette (byte offset 3) */
    uint8_t palette = 0;
    if (sprite_grouping_data_buf && grouping_offset + 4 <= sprite_grouping_data_size) {
        palette = sprite_grouping_data_buf[grouping_offset + 3];
    }

    /* ASM lines 96-99: Load spritemaps into OVERWORLD_SPRITEMAPS ert.buffer */
    if (config_entry) {
        load_overworld_spritemaps(smap_offset, vram_slot, palette,
                                 config_entry, sprites_per_frame);
    }

    /* ASM lines 100-123: Set allocation range and create entity */
    if (index == -1) {
        entities.alloc_min_slot = 0;
        entities.alloc_max_slot = 22;
    } else {
        entities.alloc_min_slot = (uint16_t)index;
        entities.alloc_max_slot = (uint16_t)(index + 1);
    }

    int16_t ent = entity_init(script, x, y);
    if (ent < 0)
        return -1;

    /* ASM lines 123-127: For auto-allocate path, re-tag VRAM table entries.
     * upload_sprite_to_vram used hint=-1 (0xFFFF), so allocate_sprite_vram marked
     * slots with 0xFF. Now that we know the actual entity slot, re-tag them. */
    if (index == -1) {
        alloc_sprite_mem(0xFFFF, (uint16_t)(ent | 0x80));
    }

    /* ASM lines 129-138: Set spritemap pointers.
     * In ROM, these point into WRAM at OVERWORLD_SPRITEMAPS + smap_offset.
     * In C, we store the byte offset into our overworld_spritemaps[] ert.buffer
     * in spritemap_ptr_lo, and use a flag in spritemap_ptr_hi to indicate
     * it's an overworld spritemap (not a title screen spritemap). */
    entities.spritemap_ptr_lo[ent] = smap_offset;
    entities.spritemap_ptr_hi[ent] = 0;  /* Visible (bit 15 clear) */

    /* ASM lines 139-144: Set spritemap sizes and beginning indices */
    entities.spritemap_sizes[ent] = (uint16_t)sprites_per_frame * SPRITEMAP_ENTRY_SIZE;
    entities.spritemap_begin_idx[ent] = vram_slot;

    /* ASM lines 145-157: Compute and store VRAM address.
     * VRAM address = SPRITE_VRAM_SLOT_TABLE[vram_slot] + VRAM_OBJ_BASE ($4000). */
    uint16_t vram_addr = VRAM_OBJ_BASE;
    if (spritemap_config && spritemap_config_size > SMCFG_VRAM_SLOT_TABLE_OFF + vram_slot * 2 + 2) {
        const uint8_t *vram_table = spritemap_config + SMCFG_VRAM_SLOT_TABLE_OFF;
        vram_addr = read_u16_le(vram_table + vram_slot * 2) + VRAM_OBJ_BASE;
    }

    /* ASM lines 194-200: Adjust VRAM address for odd tile heights */
    if (new_sprite_tile_height & 1) {
        vram_addr += 0x0100;
    }
    entities.vram_address[ent] = vram_addr;

    /* ASM lines 158-168: Set byte widths and tile heights from sprite_grouping */
    if (sprite_grouping_data_buf && grouping_offset + 2 <= sprite_grouping_data_size) {
        const uint8_t *sg = sprite_grouping_data_buf + grouping_offset;
        entities.byte_widths[ent] = (uint16_t)(sg[1] & 0xFF) * 2;  /* width byte × 2 */
        entities.tile_heights[ent] = sg[0];  /* height byte */
    }

    /* ASM lines 169-193: Set graphics pointers from sprite_grouping.
     * ENTITY_GRAPHICS_SPRITE_BANK = sg[8] (spritebank)
     * ENTITY_GRAPHICS_PTR_LOW = ROM ptr to sprite_grouping + offset of spritepointerarray
     * ENTITY_GRAPHICS_PTR_HIGH = bank byte of the ROM ptr */
    if (sprite_grouping_data_buf && grouping_offset + SPRITE_GROUPING_HEADER_SIZE <= sprite_grouping_data_size) {
        const uint8_t *sg = sprite_grouping_data_buf + grouping_offset;
        entities.graphics_sprite_bank[ent] = sg[8];

        /* The spritepointerarray starts at offset 9 in the struct.
         * We store the ert.buffer offset into our sprite_grouping_data so
         * render_entity_sprite can find the tile data pointers. */
        entities.graphics_ptr_lo[ent] = (uint16_t)(grouping_offset + SPRITE_GROUPING_HEADER_SIZE);
        entities.graphics_ptr_hi[ent] = 0;  /* Marks as ert.buffer offset, not ROM address */

        /* Debug: dump first few pointer values from spritepointerarray */
        uint32_t spa_off = grouping_offset + SPRITE_GROUPING_HEADER_SIZE;
        if (spa_off + 8 <= sprite_grouping_data_size) {
            LOG_TRACE("  gfx_ptr_lo=0x%04X bank=0x%02X spa=[%04X %04X %04X %04X]\n",
                      entities.graphics_ptr_lo[ent], sg[8],
                      read_u16_le(sprite_grouping_data_buf + spa_off + 0),
                      read_u16_le(sprite_grouping_data_buf + spa_off + 2),
                      read_u16_le(sprite_grouping_data_buf + spa_off + 4),
                      read_u16_le(sprite_grouping_data_buf + spa_off + 6));
        }
    }

    /* ASM lines 201-212: Store sprite ID and size */
    entities.sprite_ids[ent] = (int16_t)sprite;
    if (sprite_grouping_data_buf && grouping_offset + 3 <= sprite_grouping_data_size) {
        entities.sizes[ent] = sprite_grouping_data_buf[grouping_offset + 2];
    }

    /* ASM lines 213-261: Set hitbox data from sprite_grouping */
    if (sprite_grouping_data_buf && grouping_offset + 8 <= sprite_grouping_data_size) {
        const uint8_t *sg = sprite_grouping_data_buf + grouping_offset;
        entities.hitbox_ud_widths[ent] = sg[4];
        entities.hitbox_ud_heights[ent] = sg[5];
        entities.hitbox_lr_widths[ent] = sg[6];
        entities.hitbox_lr_heights[ent] = sg[7];
    }

    /* Hitbox enabled from SPRITE_HITBOX_ENABLE_TABLE[size] */
    if (spritemap_config && spritemap_config_size > SMCFG_HITBOX_ENABLE_OFF + (uint32_t)sprite_size * 2 + 2) {
        entities.hitbox_enabled[ent] = read_u16_le(spritemap_config + SMCFG_HITBOX_ENABLE_OFF + (uint32_t)sprite_size * 2);
    }

    /* Upper/lower body divide from spritemap config */
    if (config_entry) {
        uint8_t cfg_width = config_entry[1];  /* width field from config */
        uint8_t cfg_height = config_entry[0]; /* height field from config */
        uint16_t divide = (uint16_t)(cfg_width << 8) | (uint16_t)(cfg_height - cfg_width);
        entities.upper_lower_body_divides[ent] = divide;
    } else {
        entities.upper_lower_body_divides[ent] = 0;
    }

    /* ASM lines 262-273: Initialize remaining fields */
    entities.enemy_spawn_tiles[ent] = -1;
    entities.enemy_ids[ent] = -1;
    entities.npc_ids[ent] = 0xFFFF;
    entities.collided_objects[ent] = -1;
    entities.surface_flags[ent] = 0;
    entities.obstacle_flags[ent] = 0;
    entities.pathfinding_states[ent] = 0;
    entities.movement_speeds[ent] = 0;
    entities.directions[ent] = 0;
    entities.walking_styles[ent] = 0;
    entities.butterfly_orbit_direction[ent] = 0;  /* ASM line 268 */

    return ent;  /* Return slot index (ASM line 274: STA @VIRTUAL02) */
}

/*
 * DISPLAY_ANIMATED_NAMING_SPRITE (display_animated_naming_sprite.asm)
 *
 * Creates entities from the NAMING_SCREEN_ENTITIES table.
 * Each entry is a list of (sprite_id, script_id) pairs terminated by 0.
 *
 * Parameters:
 *   start_entry: index into NAMING_SCREEN_ENTITIES (0-13)
 *                0-6 = initial animations, 7-13 = return animations
 */
void display_animated_naming_sprite(uint16_t start_entry) {
    if (!naming_entities_data ||
        start_entry >= NAMING_SCREEN_ENTITY_COUNT)
        return;

    /* Read the DWORD pointer for this entry from the pointer table.
     * The binary layout is: entity lists (228 bytes) then pointer table (56 bytes).
     * The pointer table starts at ert.buffer offset NAMING_ENTITIES_PTR_TABLE_OFF. */
    uint32_t off = NAMING_ENTITIES_PTR_TABLE_OFF + (uint32_t)start_entry * 4;
    if (off + 4 > naming_entities_data_size)
        return;

    uint32_t ptr = read_u32_le(&naming_entities_data[off]);

    /* Convert ROM address to ert.buffer offset.
     * Entity lists start at $C3FC49, so ert.buffer offset = within_bank - 0xFC49. */
    uint16_t within_bank = (uint16_t)(ptr & 0xFFFF);
    uint16_t buf_off = within_bank - NAMING_ENTITIES_ROM_BASE;

    /* Walk the entity list: each entry is .WORD sprite, .WORD script */
    while (buf_off + 2 <= naming_entities_data_size) {
        uint16_t sprite_id = read_u16_le(&naming_entities_data[buf_off]);
        if (sprite_id == 0)
            break;  /* Terminator */

        uint16_t script_id = read_u16_le(&naming_entities_data[buf_off + 2]);

        create_entity(sprite_id, script_id, -1, 0, 0);
        buf_off += 4;
    }

    /* Clear the wait flag (assembly line 47) */
    ert.wait_for_naming_screen_actionscript = 0;
}

/*
 * FIND_ENTITY_BY_SPRITE_ID (C46028.asm)
 *
 * Searches all entity slots for one matching the given sprite_id.
 * Returns slot number (0 to MAX_ENTITIES-1) on success, or -1 if not found.
 */
int16_t find_entity_by_sprite_id(uint16_t sprite_id) {
    for (int slot = 0; slot < MAX_ENTITIES; slot++) {
        int16_t ent_offset = ENT(slot);
        if (entities.sprite_ids[ent_offset] == (int16_t)sprite_id)
            return (int16_t)slot;
    }
    return -1;
}

/*
 * FIND_ENTITY_BY_NPC_ID_LINKED (asm/overworld/npc/spawn_npcs_at_sector.asm call at line 123)
 *
 * Walks the active entity linked list and checks npc_ids.
 * Returns entity offset (ENT(slot)) on match, or -1 if not found.
 * Assembly returns X = entity offset; callers use X directly to index arrays.
 */
int16_t find_entity_by_npc_id(uint16_t npc_id) {
    int16_t ent = entities.first_entity;
    while (ent >= 0) {
        if (entities.npc_ids[ent] == npc_id)
            return ent;
        ent = entities.next_entity[ent];
    }
    return -1;
}

/*
 * INIT_ENTITY_UNKNOWN1 + INIT_ENTITY_UNKNOWN2 (init_entity.asm)
 *
 * Reassigns a new event script to an existing entity.
 * Frees old scripts, allocates a new script slot, initializes PC
 * from EVENT_SCRIPT_POINTERS, and clears the tick callback.
 *
 * Does NOT modify animation_frame or velocity (unlike entity_init).
 *
 * Parameters:
 *   entity_slot: slot number (0-based, NOT entity offset)
 *   script_id:   new event script ID
 */
void reassign_entity_script(int16_t entity_slot, uint16_t script_id) {
    int16_t ent_offset = ENT(entity_slot);

    /* Assembly debug trap: infinite loop if script_table is negative.
     * In port, just return. */
    if (entities.script_table[ent_offset] < 0)
        return;

    /* Free old scripts */
    free_entity_scripts(ent_offset);

    /* Allocate new script slot */
    int16_t script_offset;
    if (!allocate_script_slot(&script_offset))
        return;

    entities.script_index[ent_offset] = script_offset;
    scripts.next_script[script_offset] = ENTITY_NONE;

    /* Clear tick callback (CLEAR_SPRITE_TICK_CALLBACK) */
    clear_sprite_tick_callback(ent_offset);

    /* Initialize script PC/bank from event script pointer table */
    event_script_init(script_id, ent_offset, script_offset);
}

/*
 * Sync ert.palettes[] mirror to ppu.cgram.
 * In the ROM, the NMI handler does this. We call it from wait_for_vblank.
 */
void sync_palettes_to_cgram(void) {
    if (ert.palette_upload_mode != PALETTE_UPLOAD_NONE) {
        memcpy(ppu.cgram, ert.palettes, sizeof(ert.palettes));
        ert.palette_upload_mode = PALETTE_UPLOAD_NONE;
    }
}

/*
 * FIND_ENTITY_FOR_CHARACTER (C4608C)
 *
 * Given a character ID, returns the entity slot for that party member.
 * char_id 0xFF = leader entity (game_state.current_party_members).
 * Otherwise searches game_state.party_order[] for the character and
 * returns the corresponding party_entity_slots[] entry.
 * Returns -1 if not found.
 */
int16_t find_entity_for_character(uint8_t char_id) {
    if (char_id == 0xFF)
        return (int16_t)game_state.current_party_members;
    for (int i = 0; i < 6; i++) {
        if ((uint8_t)game_state.party_order[i] == char_id)
            /* party_entity_slots is uint8_t[12] storing 6 x 16-bit words */
            return (int16_t)read_u16_le(&game_state.party_entity_slots[i * 2]);
    }
    return -1;
}

/*
 * remove_associated_entities — Remove floating sprites for a parent entity.
 *
 * Port of REMOVE_ASSOCIATED_ENTITIES (asm/misc/remove_associated_entities.asm).
 * Floating sprites are marked with draw_priority = parent_slot | 0xC000.
 * This function searches all entities for that marker and removes matches.
 */
void remove_associated_entities(int16_t parent_slot) {
    if (parent_slot == -1) return;

    int16_t marker = parent_slot | (int16_t)0xC000;

    for (int slot = 0; slot < MAX_ENTITIES; slot++) {
        int16_t ent_offset = ENT(slot);
        if (entities.draw_priority[ent_offset] == marker) {
            entities.draw_priority[ent_offset] = 0;
            remove_entity((int16_t)slot);
        }
    }
}

/*
 * calculate_direction_fine — Compute fine 16-bit direction between two positions.
 *
 * Port of CALCULATE_DIRECTION_FROM_POSITIONS (C41EFF).
 * Returns a 16-bit fine direction:
 *   0x0000=UP, 0x2000=UP_RIGHT, 0x4000=RIGHT, 0x6000=DOWN_RIGHT,
 *   0x8000=DOWN, 0xA000=DOWN_LEFT, 0xC000=LEFT, 0xE000=UP_LEFT
 * Full circle = 0x10000 (wraps via uint16_t).
 */
uint16_t calculate_direction_fine(int16_t from_x, int16_t from_y,
                                  int16_t to_x, int16_t to_y) {
    int dx = to_x - from_x;
    int dy = to_y - from_y;
    if (dx == 0 && dy == 0)
        return 0x8000;  /* default: down */

    /* atan2(dx, dy) in screen coords: 0=+Y(down), π/2=+X(right), CW.
     * Fine direction: 0=UP(0x0000), RIGHT(0x4000), DOWN(0x8000), LEFT(0xC000).
     * Mapping: fine = 0x8000 - atan2_scaled (reflect + offset). */
    double angle = atan2((double)dx, (double)dy);
    if (angle < 0) angle += 2.0 * M_PI;
    uint16_t fine = (uint16_t)(angle * 65536.0 / (2.0 * M_PI));
    return 0x8000 - fine;
}

/*
 * calculate_direction_8 — Compute 8-way direction between two positions.
 *
 * Port of CALCULATE_DIRECTION_FROM_POSITIONS (C41EFF) + quantization step.
 * Returns SNES direction: 0=UP, 1=UP_RIGHT, 2=RIGHT, 3=DOWN_RIGHT,
 *                         4=DOWN, 5=DOWN_LEFT, 6=LEFT, 7=UP_LEFT
 */
int16_t calculate_direction_8(int16_t from_x, int16_t from_y,
                              int16_t to_x, int16_t to_y) {
    uint16_t fine = calculate_direction_fine(from_x, from_y, to_x, to_y);
    return (int16_t)(((uint16_t)(fine + 0x1000) / 0x2000) & 7);
}

/*
 * FIND_ENTITY_BY_TYPE (C4621C)
 *
 * Dispatches to the appropriate entity find function based on type.
 * type: 0=character, 1=NPC, 2=sprite.
 * Returns entity offset on success, or -1 if not found.
 */
int16_t find_entity_by_type(int16_t type, int16_t entity_id) {
    switch (type) {
    case 0: { /* character */
        int16_t slot = find_entity_for_character((uint8_t)entity_id);
        return (slot >= 0) ? (int16_t)(ENT(slot)) : -1;
    }
    case 1: /* NPC */
        return find_entity_by_npc_id((uint16_t)entity_id);
    case 2: { /* sprite */
        int16_t slot = find_entity_by_sprite_id((uint16_t)entity_id);
        return (slot >= 0) ? ENT(slot) : -1;
    }
    default:
        return -1;
    }
}

/*
 * GET_DIRECTION_BETWEEN_ENTITIES (C46257)
 *
 * Finds two entities by type+id, reads their absolute positions,
 * and computes the 8-way direction from source to target.
 *
 * source_type/target_type: 0=character, 1=NPC, 2=sprite.
 * Returns 0-7 SNES direction, or 4 (DOWN) if either entity not found.
 */
int16_t get_direction_between_entities(int16_t source_type, int16_t source_id,
                                       int16_t target_type, int16_t target_id) {
    int16_t src_ent = find_entity_by_type(source_type, source_id);
    int16_t tgt_ent = find_entity_by_type(target_type, target_id);
    if (src_ent < 0 || tgt_ent < 0)
        return 4; /* default direction */

    return calculate_direction_8(
        entities.abs_x[src_ent], entities.abs_y[src_ent],
        entities.abs_x[tgt_ent], entities.abs_y[tgt_ent]);
}

/* ===================================================================
 * Floating sprites — SPAWN_FLOATING_SPRITE + UPDATE_FLOATING_SPRITE_OFFSET
 *
 * Port of asm/text/spawn_floating_sprite.asm (bank C4) and
 * asm/overworld/entity/update_floating_sprite_offset.asm.
 *
 * The FLOATING_SPRITE_TABLE is a binary asset (12 entries × 5 bytes each):
 *   [0-1] uint16_t sprite  (OVERWORLD_SPRITE enum)
 *   [2]   uint8_t  position_mode (1-6: placement relative to collision box)
 *   [3]   int8_t   x_offset
 *   [4]   int8_t   y_offset
 * =================================================================== */

#define FLOATING_SPRITE_TABLE_ENTRY_SIZE 5
#define FLOATING_SPRITE_TABLE_ENTRIES    12
#define FLOATING_SPRITE_SCRIPT           785  /* EVENT_PARTY_SPRITE */

static const uint8_t *floating_sprite_table_data = NULL;
static size_t   floating_sprite_table_size = 0;

bool floating_sprite_table_load(void) {
    if (floating_sprite_table_data)
        return true;
    floating_sprite_table_size = ASSET_SIZE(ASSET_TEXT_FLOATING_SPRITE_TABLE_BIN);
    floating_sprite_table_data = ASSET_DATA(ASSET_TEXT_FLOATING_SPRITE_TABLE_BIN);
    return (floating_sprite_table_data != NULL);
}

void floating_sprite_table_free(void) {
    floating_sprite_table_data = NULL;
    floating_sprite_table_size = 0;
}

/*
 * UPDATE_FLOATING_SPRITE_OFFSET (C4B329)
 *
 * Adjusts x/y position based on entity collision box and a position_mode:
 *   1 = above-left   (y -= coll_y + 8, then x -= coll_x - 8)
 *   2 = above         (y -= coll_y - 8)
 *   3 = above-right  (y -= coll_y + 8, then x -= -(coll_x + 8))
 *   4 = left          (x -= coll_x - 8)
 *   5 = none          (no adjustment)
 *   6 = right         (x -= -(coll_x + 8))
 *
 * size_index: entity size code (index into ENTITY_COLLISION_X/Y_OFFSET).
 * x, y are modified in place.
 */
static void update_floating_sprite_offset(uint8_t position_mode, uint16_t size_index,
                                           int16_t *x, int16_t *y) {
    if (size_index >= 17) return;

    switch (position_mode) {
    case 1: /* above-left: offset Y up, then fall through to left */
        *y -= entity_collision_y_offset[size_index] + 8;
        /* fall through */
    case 4: /* left */
        *x -= entity_collision_x_offset[size_index] - 8;
        break;

    case 2: /* above */
        *y -= entity_collision_y_offset[size_index] - 8;
        break;

    case 3: /* above-right: offset Y up, then fall through to right */
        *y -= entity_collision_y_offset[size_index] + 8;
        /* fall through */
    case 6: /* right */
        *x -= (entity_collision_x_offset[size_index] + 8);
        break;

    case 5: /* none */
    default:
        break;
    }
}

/*
 * SPAWN_FLOATING_SPRITE (asm/text/spawn_floating_sprite.asm)
 *
 * Creates a floating sprite entity attached to entity_slot.
 * The floating sprite uses FLOATING_SPRITE_TABLE[table_index] to determine:
 *   - which overworld sprite to use
 *   - positioning relative to the parent entity's collision box
 *   - additional x/y offsets
 *
 * The child entity is created with:
 *   - script = EVENT_PARTY_SPRITE (785) (follow parent position)
 *   - draw_priority = parent_slot | 0xC000 (marker for remove_associated_entities)
 *   - surface_flags copied from parent
 */
void spawn_floating_sprite(int16_t entity_slot, uint16_t table_index) {
    if (entity_slot < 0) return;

    int16_t entity_offset = ENT(entity_slot);

    /* Check entity is valid (has a script assigned) */
    if (entities.script_table[entity_offset] == -1) return;

    /* Validate table data */
    if (!floating_sprite_table_data) return;
    uint32_t entry_off = (uint32_t)table_index * FLOATING_SPRITE_TABLE_ENTRY_SIZE;
    if (entry_off + FLOATING_SPRITE_TABLE_ENTRY_SIZE > floating_sprite_table_size) return;

    const uint8_t *entry = floating_sprite_table_data + entry_off;
    uint16_t sprite_id     = read_u16_le(&entry[0]);
    uint8_t  position_mode = entry[2];
    int8_t   x_off         = (int8_t)entry[3];
    int8_t   y_off         = (int8_t)entry[4];

    /* Get entity size for collision box lookup */
    uint16_t size_idx = entities.sizes[entity_offset];

    /* Start with parent entity's absolute position */
    int16_t manpu_x = entities.abs_x[entity_offset];
    int16_t manpu_y = entities.abs_y[entity_offset];

    /* Apply collision-box-based offset */
    update_floating_sprite_offset(position_mode, size_idx, &manpu_x, &manpu_y);

    /* Apply table x/y offsets (sign-extended) */
    manpu_x += (int16_t)x_off;
    manpu_y += (int16_t)y_off;

    /* Create the floating sprite entity */
    int16_t new_slot = create_entity(sprite_id, FLOATING_SPRITE_SCRIPT, -1,
                                     manpu_x, manpu_y);
    if (new_slot < 0) return;

    int16_t new_offset = ENT(new_slot);

    /* Mark as child: draw_priority = parent_slot | 0xC000 */
    entities.draw_priority[new_offset] = entity_slot | (int16_t)0xC000;

    /* Copy surface flags from parent */
    entities.surface_flags[new_offset] = entities.surface_flags[entity_offset];
}

/* Convenience wrappers — port of C4B524, C4B4FE, C4B54A */

void spawn_floating_sprite_for_npc(uint16_t npc_id, uint16_t table_index) {
    int16_t entity_offset = find_entity_by_npc_id(npc_id);
    /* find_entity_by_npc_id returns entity offset (== slot now that ENT is identity) */
    spawn_floating_sprite(entity_offset, table_index);
}

void spawn_floating_sprite_for_character(uint8_t char_id, uint16_t table_index) {
    int16_t slot = find_entity_for_character(char_id);
    spawn_floating_sprite(slot, table_index);
}

void spawn_floating_sprite_for_sprite(uint16_t sprite_id, uint16_t table_index) {
    int16_t slot = find_entity_by_sprite_id(sprite_id);
    spawn_floating_sprite(slot, table_index);
}
