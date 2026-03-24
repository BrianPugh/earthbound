/*
 * Overworld spawn and enemy encounter functions.
 *
 * Ported from:
 *   SPAWN_BUZZ_BUZZ               — asm/overworld/spawn_buzz_buzz.asm
 *   SPAWN_DELIVERY_ENTITIES       — asm/overworld/spawn_delivery_entities.asm
 *   GET_DELIVERY_SPRITE_AND_PLACEHOLDER — asm/overworld/get_delivery_sprite_and_placeholder.asm
 *   SAVE_PHOTO_STATE              — asm/misc/save_photo_state.asm
 *   ENCOUNTER_TRAVELLING_PHOTOGRAPHER — asm/misc/encounter_travelling_photographer.asm
 *   GET_MAP_ENEMY_PLACEMENT       — asm/overworld/get_map_enemy_placement.asm
 *   CAN_ENEMY_RUN_IN_DIRECTION    — asm/overworld/can_enemy_run_in_direction.asm
 *   ATTEMPT_ENEMY_SPAWN           — asm/overworld/attempt_enemy_spawn.asm
 *   SPAWN_HORIZONTAL              — asm/overworld/spawn_horizontal.asm
 *   SPAWN_VERTICAL                — asm/overworld/spawn_vertical.asm
 *   INITIATE_ENEMY_ENCOUNTER      — asm/overworld/initiate_enemy_encounter.asm
 */

#include "game/overworld_internal.h"
#include "game/game_state.h"
#include "game/battle.h"
#include "game/audio.h"
#include "game/fade.h"
#include "game/map_loader.h"
#include "game/inventory.h"
#include "game/display_text.h"
#include "game/window.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "entity/pathfinding.h"
#include "data/assets.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/constants.h"
#include "core/memory.h"
#include "game_main.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- Delivery System ---- */

#define EVENT_SCRIPT_499 499
#define EVENT_SCRIPT_500 500

/* MSG_EVT_PHOTOGRAPHER ROM address ($C7AB3F from earthbound.yml text block 0x0848) */
#define MSG_EVT_PHOTOGRAPHER 0xC7AB3Fu

/* Shared delivery table data (lazy-loaded by either function) */
static const DeliveryEntry *delivery_table = NULL;
static const uint8_t *for_sale_signs = NULL;
static size_t dt_size = 0, fs_size = 0;

bool ensure_delivery_tables(void) {
    if (!delivery_table) {
        delivery_table = (const DeliveryEntry *)ASSET_DATA(ASSET_DATA_TIMED_DELIVERY_TABLE_BIN);
        dt_size = ASSET_SIZE(ASSET_DATA_TIMED_DELIVERY_TABLE_BIN);
        for_sale_signs = ASSET_DATA(ASSET_DATA_FOR_SALE_SIGN_SPRITE_TABLE_BIN);
        fs_size = ASSET_SIZE(ASSET_DATA_FOR_SALE_SIGN_SPRITE_TABLE_BIN);
        if (!delivery_table || !for_sale_signs) {
            fprintf(stderr, "overworld: delivery table data not available\n");
            return false;
        }
    }
    return true;
}

const DeliveryEntry *get_delivery_table(void) {
    if (!ensure_delivery_tables()) return NULL;
    return delivery_table;
}

/* ---- SPAWN_BUZZ_BUZZ (port of asm/overworld/spawn_buzz_buzz.asm) ----
 * Displays MSG_EVT_BUNBUNBUN (0xC5EA35) which contains entity-spawn CC codes
 * gated by event flags, then spawns delivery entities.
 * Called after every door transition and at overworld init. */
void spawn_buzz_buzz(void) {
    display_text_from_snes_addr(0xC5EA35u);  /* MSG_EVT_BUNBUNBUN */
    spawn_delivery_entities();
}

void spawn_delivery_entities(void) {
    if (!ensure_delivery_tables()) return;

    for (int i = 0; i < DELIVERY_TABLE_COUNT; i++) {
        if (!event_flag_get(delivery_table[i].event_flag))
            continue;

        /* Set var0 = delivery index */
        ert.new_entity_var[0] = (int16_t)i;

        uint16_t sprite_id = delivery_table[i].sprite_id;

        /* If sprite == 0, pick a random for-sale sign sprite */
        if (sprite_id == 0) {
            int r = rand() % 4;  /* RAND & 3 */
            sprite_id = read_u16_le(&for_sale_signs[r * 2]);
        }

        /* Create entity at (0, 0) with EVENT_SCRIPT_500, auto slot */
        create_entity(sprite_id, EVENT_SCRIPT_500, -1, 0, 0);
    }
}

/* ---- GET_DELIVERY_SPRITE_AND_PLACEHOLDER (port of EF0EAD) ----
 *
 * Port of asm/overworld/get_delivery_sprite_and_placeholder.asm.
 * Takes a 1-based delivery ID, looks up the sprite in TIMED_DELIVERY_TABLE
 * (or picks a random for-sale sign if sprite == 0), and creates an entity
 * at (0,0) with EVENT_SCRIPT_499. The 0-based delivery index is stored
 * in ert.new_entity_var[0] for the script to use. */
void get_delivery_sprite_and_placeholder(uint16_t delivery_id) {
    if (!ensure_delivery_tables()) return;
    if (delivery_id == 0 || delivery_id > DELIVERY_TABLE_COUNT) return;

    /* Store 0-based index in ert.new_entity_var[0] */
    uint16_t index = delivery_id - 1;
    ert.new_entity_var[0] = (int16_t)index;

    uint16_t sprite_id = delivery_table[index].sprite_id;

    /* If sprite == 0 (bicycle), pick a random for-sale sign sprite */
    if (sprite_id == 0) {
        int r = rand() % 4;  /* RAND & 3 */
        sprite_id = read_u16_le(&for_sale_signs[r * 2]);
    }

    /* Create entity at (0, 0) with EVENT_SCRIPT_499, auto slot */
    create_entity(sprite_id, EVENT_SCRIPT_499, -1, 0, 0);
}

/* ---- SAVE_PHOTO_STATE (port of C4343E) ----
 *
 * Port of asm/misc/save_photo_state.asm.
 * Saves play time and party affliction status for a photo slot.
 * photo_id is 1-based (from the CC code argument). */
void save_photo_state(uint16_t photo_id) {
    if (photo_id == 0 || photo_id > NUM_PHOTOS) return;
    uint16_t slot = photo_id - 1;

    /* Assembly lines 13-28: play_time = TIMER / 3600, capped at 59999 */
    uint32_t play_time = core.play_timer / 3600;
    if (play_time >= 60000)
        play_time = 59999;

    /* Assembly lines 30-44: store play_time in saved_photo_states[slot].unknown */
    game_state.saved_photo_states[slot].unknown = (uint16_t)play_time;

    /* Assembly lines 45-162: loop 6 party slots, store affliction bits */
    for (int y = 0; y < 6; y++) {
        uint8_t party_member_id = game_state.party_order[y];
        if (party_member_id == 0) {
            /* No party member in this slot */
            game_state.saved_photo_states[slot].party[y] = 0;
            continue;
        }

        /* Assembly lines 82-98: get char_struct for this party member */
        uint8_t char_index = game_state.player_controlled_party_members[y];
        CharStruct *ch = &party_characters[char_index];

        /* Assembly lines 95-98: start with party_order value (character ID) */
        uint8_t status_byte = party_member_id;

        /* Assembly lines 101-109: check UNCONSCIOUS (afflictions[0] == 1) → bit 5 */
        if (ch->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS) {
            status_byte |= 0x20;
        }
        /* Assembly lines 111-118: check DIAMONDIZED (afflictions[0] == 2) → bit 6 */
        else if (ch->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_DIAMONDIZED) {
            status_byte |= 0x40;
        }

        /* Assembly lines 119-129: check MUSHROOMIZED (afflictions[1] == 1) → bit 7 */
        if (ch->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] == STATUS_1_MUSHROOMIZED) {
            status_byte |= 0x80;
        }

        /* Assembly lines 130-156: store status byte */
        game_state.saved_photo_states[slot].party[y] = status_byte;
    }
}

/* ---- ENCOUNTER_TRAVELLING_PHOTOGRAPHER (port of C466C1) ----
 *
 * Port of asm/misc/encounter_travelling_photographer.asm.
 * Called from CC_1F_D2 to trigger a photographer encounter. */
void encounter_travelling_photographer(uint16_t photo_id) {
    /* Assembly line 10: JSL CLEAR_PARTY_SPRITE_HIDE_FLAGS */
    clear_party_sprite_hide_flags();

    /* Assembly line 11: STZ PLAYER_INTANGIBILITY_FRAMES */
    ow.player_intangibility_frames = 0;

    /* Assembly lines 13-14: DEC, STA SPAWNING_TRAVELLING_PHOTOGRAPHER_ID */
    ow.spawning_travelling_photographer_id = photo_id - 1;

    /* Assembly line 15: DISPLAY_TEXT_PTR MSG_EVT_PHOTOGRAPHER */
    display_text_from_snes_addr(MSG_EVT_PHOTOGRAPHER);

    /* Assembly lines 16-17: LDA @LOCAL01; JSL SAVE_PHOTO_STATE */
    save_photo_state(photo_id);
}

/* ---- Enemy Spawn System ----
 * Ported from:
 *   GET_MAP_ENEMY_PLACEMENT  — asm/overworld/get_map_enemy_placement.asm
 *   CAN_ENEMY_RUN_IN_DIRECTION — asm/overworld/can_enemy_run_in_direction.asm
 *   ATTEMPT_ENEMY_SPAWN      — asm/overworld/attempt_enemy_spawn.asm
 *   SPAWN_HORIZONTAL         — asm/overworld/spawn_horizontal.asm
 *   SPAWN_VERTICAL           — asm/overworld/spawn_vertical.asm
 */

/* Enemy placement data tables loaded from ROM assets */
static const uint8_t *map_enemy_placement_data = NULL;
static const uint8_t *enemy_placement_groups_ptr_table = NULL;
static const uint8_t *enemy_placement_groups_data = NULL;

/* ROM base address of ENEMY_PLACEMENT_GROUPS data (for pointer conversion) */
#define ENEMY_PLACEMENT_GROUPS_ROM_BASE 0xD0BBAC

/* Map grid dimensions for enemy placement (128 columns x 160 rows) */
#define ENEMY_MAP_COLS 128
#define ENEMY_MAP_ROWS 160

/* Magic butterfly constants */
#define MAGIC_BUTTERFLY_BATTLEGROUP 481

/* Default movement script for enemies without one */
#define DEFAULT_ENEMY_MOVEMENT_STYLE 19

/* Map dimensions in 8px tile units (from include/enums.asm) */
#define MAP_WIDTH_TILES8  1024
#define MAP_HEIGHT_TILES8 1280

/* ENEMY_BATTLE_GROUPS_TABLE ROM base address (for pointer conversion) */
#define ENEMY_BATTLE_GROUPS_ROM_BASE 0xD0D52D

static bool enemy_spawn_data_loaded = false;

void load_enemy_spawn_data(void) {
    if (enemy_spawn_data_loaded) return;
    map_enemy_placement_data = ASSET_DATA(ASSET_DATA_MAP_ENEMY_PLACEMENT_BIN);
    enemy_placement_groups_ptr_table = ASSET_DATA(ASSET_DATA_ENEMY_PLACEMENT_GROUPS_PTR_TABLE_BIN);
    enemy_placement_groups_data = ASSET_DATA(ASSET_DATA_ENEMY_PLACEMENT_GROUPS_BIN);
    if (!enemy_config_table)
        enemy_config_table = (const EnemyData *)ASSET_DATA(ASSET_DATA_ENEMY_CONFIGURATION_TABLE_BIN);
    if (!battle_action_table)
        battle_action_table = (const BattleAction *)ASSET_DATA(ASSET_DATA_BATTLE_ACTION_TABLE_BIN);
    enemy_spawn_data_loaded = true;
}

/* GET_MAP_ENEMY_PLACEMENT (asm/overworld/get_map_enemy_placement.asm).
 * Returns encounter_id for tile at (x, y) in the 128x160 grid.
 * x: 0-127, y: 0-159 (64px tile coordinates). */
uint16_t get_map_enemy_placement(uint16_t x, uint16_t y) {
    load_enemy_spawn_data();
    if (!map_enemy_placement_data) return 0;
    if (x >= ENEMY_MAP_COLS || y >= ENEMY_MAP_ROWS) return 0;

    /* Assembly: index = y * 256 + x * 2 (each row = 256 bytes = 128 words) */
    size_t offset = (size_t)y * 256 + (size_t)x * 2;
    return read_u16_le(&map_enemy_placement_data[offset]);
}

/* CAN_ENEMY_RUN_IN_DIRECTION (asm/overworld/can_enemy_run_in_direction.asm).
 * Checks if enemy can move in a given direction based on surface flags and run_flag.
 * surface_flags: terrain flags from lookup_surface_flags.
 * enemy_id: enemy configuration index.
 * Returns 0x80 if enemy can run in this direction, 0 if blocked. */
uint16_t can_enemy_run_in_direction(uint16_t surface_flags, uint16_t enemy_id) {
    /* Assembly extracts direction from surface flags bits 2-3 */
    uint16_t dir_bits = surface_flags & 0x000C;
    uint16_t check_mask;
    if (dir_bits == 0x0000)      check_mask = 0x0004;  /* up */
    else if (dir_bits == 0x0004) check_mask = 0x0002;  /* right */
    else if (dir_bits >= 0x0008) check_mask = 0x0001;  /* down or left */
    else                         check_mask = 0x0000;

    /* Read run_flag from enemy configuration table */
    if (!enemy_config_table) return 0x0080;
    uint8_t run_flag = enemy_config_table[enemy_id].run_flag;

    /* If run_flag has the direction bit set, enemy is blocked */
    if ((run_flag & check_mask) != 0)
        return 0x0000;
    return 0x0080;
}

/* ATTEMPT_ENEMY_SPAWN (asm/overworld/attempt_enemy_spawn.asm).
 * Core enemy spawn logic — tries to create an enemy entity at a random position.
 * x, y: 64px grid coordinates (0-127, 0-159).
 * encounter_id: from MAP_ENEMY_PLACEMENT table. */
void attempt_enemy_spawn(uint16_t x, uint16_t y, uint16_t encounter_id) {
    load_enemy_spawn_data();
    uint16_t battle_group_id = 0;

    /* Assembly lines 25-35: Debug mode path — skip in C port (debug disabled) */

    /* Assembly lines 36-41: Increment spawn counter, check low 4 bits.
     * Every 16th call -> try magic butterfly (check sector encounter rate).
     * Other calls -> go to normal enemy group selection. */
    ow.enemy_spawn_counter++;
    if ((ow.enemy_spawn_counter & 0x000F) != 0)
        goto select_enemy_group;

    /* --- Magic butterfly path (every 16th frame) --- */
    {
        /* Assembly lines 42-60: Compute sector index from 64px grid coordinates.
         * sector_x = (x * 8) / 32 = x / 4
         * sector_y = (y * 8) / 16 = y / 2
         * Table index (bytes) = sector_y * 64 + sector_x * 2 */
        uint16_t sector_x = (x * 8) >> 5;  /* (x_grid * 8) / 32 */
        uint16_t sector_y = (y * 8) >> 4;  /* (y_grid * 8) / 16 */

        /* Read MAP_DATA_PER_SECTOR_ATTRIBUTES_TABLE entry */
        uint16_t sector_offset = sector_y * 64 + sector_x * 2;
        static const uint8_t *sector_data = NULL;
        if (!sector_data)
            sector_data = ASSET_DATA(ASSET_DATA_PER_SECTOR_ATTRIBUTES_BIN);
        if (!sector_data) return;
        uint16_t attrs = read_u16_le(&sector_data[sector_offset]);

        /* Assembly lines 62-103: Low 3 bits = encounter rate category -> spawn_rate.
         * 0->2, 1->0, 2->1, 3->0, 4->5, 5->1, else->unchanged */
        uint16_t rate_category = attrs & 0x0007;
        uint16_t spawn_rate;
        switch (rate_category) {
            case 0: spawn_rate = 2; break;
            case 1: spawn_rate = 0; break;
            case 2: spawn_rate = 1; break;
            case 3: spawn_rate = 0; break;
            case 4: spawn_rate = 5; break;
            case 5: spawn_rate = 1; break;
            default: spawn_rate = rate_category; break;
        }

        /* Assembly lines 104-112: Roll random 0-99, compare with spawn_rate.
         * If rand < rate, try magic butterfly. */
        uint16_t roll = (uint16_t)(rand() % 100);  /* 0-99 */
        if (roll >= spawn_rate)
            return;

        /* Spawn magic butterfly (assembly lines 113-118) */
        battle_group_id = MAGIC_BUTTERFLY_BATTLEGROUP;
        ow.spawning_enemy_group = battle_group_id;
        goto read_group_entry_count;
    }

select_enemy_group:
    /* Assembly lines 119-121: If encounter_id == 0, return */
    if (encounter_id == 0)
        return;

    {
        /* Assembly lines 122-146: Check GLOBAL_MAP_TILESETPALETTE_DATA.
         * Verify the spawning tile's tileset_combo matches LOADED_MAP_TILE_COMBO. */
        uint16_t sector_x = (x * 8) >> 5;
        uint16_t sector_y = (y * 8) >> 4;
        uint16_t tsp_index = sector_y * 32 + sector_x;

        static const uint8_t *tsp_data = NULL;
        if (!tsp_data)
            tsp_data = ASSET_DATA(ASSET_DATA_GLOBAL_MAP_TILESETPALETTE_DATA_BIN);
        if (!tsp_data) return;

        uint16_t tileset_combo = ((uint16_t)tsp_data[tsp_index] & 0xFF) >> 3;
        if (tileset_combo != (uint16_t)(int16_t)ow.loaded_map_tile_combo)
            return;

        /* Assembly lines 147-173: Load enemy_placement data from groups table */
        ow.enemy_spawn_encounter_id = encounter_id;
        if (!enemy_placement_groups_ptr_table || !enemy_placement_groups_data)
            return;

        /* Read 4-byte ROM pointer from PTR_TABLE[encounter_id] */
        uint32_t ptr_off = (uint32_t)encounter_id * 4;
        uint32_t placement_rom_ptr = (uint32_t)enemy_placement_groups_ptr_table[ptr_off]
                                   | ((uint32_t)enemy_placement_groups_ptr_table[ptr_off + 1] << 8)
                                   | ((uint32_t)enemy_placement_groups_ptr_table[ptr_off + 2] << 16);
        uint32_t placement_offset = placement_rom_ptr - ENEMY_PLACEMENT_GROUPS_ROM_BASE;
        const uint8_t *placement = enemy_placement_groups_data + placement_offset;

        /* enemy_placement struct:
         *   [0-1] event_flag (word)
         *   [2]   spawn_chance (byte)
         *   [3]   spawn_chance_alt (byte)
         *   [4+]  enemy_group entries: { slots(1), group(2) } */
        uint16_t event_flag = read_u16_le(&placement[0]);
        uint8_t spawn_chance_val = placement[2];
        uint8_t spawn_chance_alt = placement[3];
        const uint8_t *groups_start = placement + 4;  /* enemy_placement::groups */

        /* Assembly lines 170-173: Store spawn_chance */
        ow.enemy_spawn_chance = spawn_chance_val;

        /* Assembly lines 174-191: Check event_flag and possibly use alt spawn chance */
        uint16_t group_offset_delta = 0;  /* Assembly @LOCAL06: offset into alternate group set */
        if (event_flag != 0 && event_flag_get(event_flag)) {
            ow.enemy_spawn_chance = spawn_chance_alt;
            if (spawn_chance_val != 0)
                group_offset_delta = 8;
        }

        /* Assembly lines 192-206: Roll spawn chance (PIRACY_FLAG check).
         * PIRACY_FLAG is always 0 in legitimate copies, so always roll. */
        {
            uint16_t roll = (uint16_t)(rand() % 100);  /* 0-99 */
            if (roll >= ow.enemy_spawn_chance)
                return;
        }

        /* Assembly lines 207-243: Pick random slot and find matching group.
         * Random value 0-7 + group_offset_delta -> cumulative slot search. */
        uint16_t random_slot = (rand() % 256) & 0x0007;
        uint16_t target_slot = group_offset_delta + random_slot;

        /* Walk through enemy_group entries: { slots(1), group(2) }
         * Accumulate slot counts until target_slot falls within range. */
        const uint8_t *grp_entry = groups_start;
        uint16_t accumulated = 0;
        while (1) {
            uint8_t slots = grp_entry[0];
            accumulated += slots;
            if (target_slot < accumulated)
                break;
            grp_entry += 3;  /* sizeof(enemy_group) = 3 */
        }

        /* Assembly lines 239-250: Read group ID */
        battle_group_id = read_u16_le(&grp_entry[1]);
        ow.spawning_enemy_group = battle_group_id;

        /* Assembly lines 251-256: Check for duplicate enemy at same spawn tile.
         * spawn_tile = y * 128 + x. If an active entity with same group+0x8000
         * is already at this spawn_tile, don't spawn again. */
        uint16_t spawn_tile = y * 128 + x;
        for (int i = 0; i < 23; i++) {
            if (entities.script_table[i] == -1) continue;
            if (entities.npc_ids[i] != (battle_group_id + 0x8000)) continue;
            if (entities.enemy_spawn_tiles[i] == (int16_t)spawn_tile)
                return;
        }

        goto read_group_entry_count;
    }

read_group_entry_count:
    {
        /* Assembly lines 430-437: Read battle_group_entry::count (first byte).
         * Format: { count(1), enemy_id(2) }, terminated by count == 0xFF. */
        static const uint8_t *btl_ptr_table_local = NULL;
        static const uint8_t *battle_groups_local = NULL;
        if (!btl_ptr_table_local)
            btl_ptr_table_local = ASSET_DATA(ASSET_DATA_BTL_ENTRY_PTR_TABLE_BIN);
        if (!battle_groups_local)
            battle_groups_local = ASSET_DATA(ASSET_DATA_ENEMY_BATTLE_GROUPS_TABLE_BIN);

        /* We need the grp_ptr — but we jumped here from two different paths.
         * Re-derive it from ow.spawning_enemy_group. */
        if (!btl_ptr_table_local || !battle_groups_local) return;
        uint32_t entry_off = (uint32_t)ow.spawning_enemy_group * 8;
        uint32_t rom_ptr = (uint32_t)btl_ptr_table_local[entry_off]
                         | ((uint32_t)btl_ptr_table_local[entry_off + 1] << 8)
                         | ((uint32_t)btl_ptr_table_local[entry_off + 2] << 16);
        uint32_t grp_offset = rom_ptr - ENEMY_BATTLE_GROUPS_ROM_BASE;
        const uint8_t *grp_ptr = battle_groups_local + grp_offset;

        /* Iterate through battle group entries */
        while (1) {
            uint8_t count = grp_ptr[0];
            if (count == 0xFF)
                break;
            uint16_t enemy_id = read_u16_le(&grp_ptr[1]);
            ow.enemy_spawn_remaining_count = count;

            /* Assembly lines 279-319: Load enemy configuration data */
            if (!enemy_config_table) break;
            ow.spawning_enemy_name = enemy_config_table[enemy_id].the_flag;
            uint16_t overworld_sprite = enemy_config_table[enemy_id].overworld_sprite;
            ow.spawning_enemy_sprite = overworld_sprite;
            uint16_t event_script = enemy_config_table[enemy_id].event_script;
            if (event_script == 0)
                event_script = DEFAULT_ENEMY_MOVEMENT_STYLE;

            /* Assembly lines 321-332: Per-enemy spawn loop.
             * For each count, check butterfly dupe, check enemy limit, create entity. */
            for (uint16_t c = 0; c < count; c++) {
                /* Check magic butterfly already spawned */
                if (enemy_id == ENEMY_MAGIC_BUTTERFLY) {
                    if (ow.magic_butterfly_spawned)
                        continue;
                }

                /* Check overworld enemy count limit */
                if (ow.overworld_enemy_count >= ow.overworld_enemy_maximum) {
                    ow.enemy_spawn_too_many_failures++;
                    continue;
                }

                /* Assembly lines 333-344: Create entity */
                ow.enemy_spawn_too_many_failures = 0;
                int16_t entity_slot = create_entity(overworld_sprite, event_script,
                                                     -1, 0, 0);
                if (entity_slot < 0) continue;

                /* Assembly lines 346-390: Try up to 20 random positions */
                bool position_found = false;
                for (int attempt = 0; attempt < 20; attempt++) {
                    /* Random x within spawn range */
                    uint16_t rand_x_off = rand() % ow.enemy_spawn_range_width;
                    int16_t pixel_x = (int16_t)((x * 8 + rand_x_off) * 8);

                    /* Random y within spawn range */
                    uint16_t rand_y_off = rand() % ow.enemy_spawn_range_height;
                    int16_t pixel_y = (int16_t)((y * 8 + rand_y_off) * 8);

                    /* Check surface flags at position */
                    uint16_t sf = lookup_surface_flags(pixel_x, pixel_y,
                                                       entities.sizes[entity_slot]);

                    /* Assembly line 374: Check for impassable terrain (bits 4,6,7) */
                    if (sf & 0x00D0)
                        continue;

                    /* Assembly lines 376-381: result==0 (can't flee) -> position valid.
                     * result!=0 (can flee this direction) -> skip (position invalid). */
                    uint16_t can_run = can_enemy_run_in_direction(sf, enemy_id);
                    if (can_run != 0)
                        continue;

                    /* Position is valid — set entity coordinates */
                    int ent = entity_slot;
                    entities.abs_x[ent] = pixel_x;
                    entities.abs_y[ent] = pixel_y;

                    /* Set NPC ID = group_id + 0x8000 */
                    entities.npc_ids[ent] = battle_group_id + 0x8000;

                    /* Set enemy ID */
                    entities.enemy_ids[ent] = (int16_t)enemy_id;

                    /* Set spawn tile for dedup */
                    entities.enemy_spawn_tiles[ent] = (int16_t)(y * 128 + x);

                    /* Clear pathfinding state */
                    entities.pathfinding_states[ent] = 0;

                    /* Set random weak_enemy_value */
                    entities.weak_enemy_value[ent] = rand() % 256;

                    ow.overworld_enemy_count++;

                    /* Track magic butterfly */
                    if (enemy_id == ENEMY_MAGIC_BUTTERFLY)
                        ow.magic_butterfly_spawned = 1;

                    position_found = true;
                    break;
                }

                /* If no valid position found in 20 attempts, remove entity */
                if (!position_found)
                    remove_entity(entity_slot);
            }

            /* Advance to next battle_group_entry */
            grp_ptr += 3;
        }
    }
}

/* SPAWN_HORIZONTAL (asm/overworld/spawn_horizontal.asm).
 * Spawns enemies along new map columns when the camera scrolls horizontally.
 * new_x: the new x position (in 8px tile coordinates).
 * y_coord: y position (in 8px tile coordinates). */
void spawn_horizontal(uint16_t new_x, uint16_t y_coord) {
    /* Assembly lines 17-26: Check event flags and spawn enable */
    if (event_flag_get(EVENT_FLAG_MONSTER_OFF)) return;
    if (event_flag_get(EVENT_FLAG_WIN_GIEGU)) return;
    if (!ow.enemy_spawns_enabled) return;

    /* Assembly lines 27-37: Validate y coordinate (must be 8-aligned, in bounds) */
    if ((y_coord & 0x0007) != 0) return;
    if (y_coord >= 0xFFF0) y_coord = 0;
    if (y_coord >= MAP_HEIGHT_TILES8) return;

    /* Assembly lines 39-73: Convert pixel-ish coords to 64px grid coords.
     * ASL -> LSR x4 = signed /8 with sign extension. */
    int16_t x_start = (int16_t)((((uint16_t)new_x << 1) >> 4) | (((new_x << 1) & 0x8000) ? 0xF000 : 0));
    int16_t y_grid = (int16_t)((((uint16_t)y_coord << 1) >> 4) | (((y_coord << 1) & 0x8000) ? 0xF000 : 0));

    /* Assembly lines 74-129: Iterate through columns */
    int16_t col = x_start;
    while (1) {
        /* Assembly line 129: BRANCHGTS: loop while x_start + 5 - col >= 0, i.e. col <= x_start + 5 */
        int16_t diff = (int16_t)(x_start + 5);
        /* CLC; SBC col -> diff - col - 1. BRANCHGTS branches if result >= 0.
         * So: diff - col - 1 >= 0 -> col <= diff - 1 -> col <= x_start + 4 */
        if ((int16_t)(diff - col - 1) < 0)
            break;

        int16_t x_tile_start = col;
        ow.enemy_spawn_range_width = 8;
        ow.enemy_spawn_range_height = 8;
        uint16_t tile_count = 1;

        /* Assembly lines 83-108: Scan contiguous tiles with same encounter_id */
        uint16_t first_id = get_map_enemy_placement((uint16_t)col, (uint16_t)y_grid);
        int16_t next_col = col + 1;

        if (first_id != 0) {
            while (tile_count < 6) {
                uint16_t next_id = get_map_enemy_placement((uint16_t)next_col, (uint16_t)y_grid);
                if (next_id != first_id) break;
                ow.enemy_spawn_range_width += 8;
                col = next_col;
                next_col = col + 1;
                tile_count++;
            }
        }

        /* Assembly lines 110-121: Attempt spawns (count down from tile_count) */
        if (first_id != 0) {
            for (uint16_t i = tile_count; i > 0; i--) {
                attempt_enemy_spawn((uint16_t)x_tile_start, (uint16_t)y_grid, first_id);
            }
        }

        col++;
    }
}

/* SPAWN_VERTICAL (asm/overworld/spawn_vertical.asm).
 * Spawns enemies along new map rows when the camera scrolls vertically.
 * x_coord: x position (in 8px tile coordinates).
 * new_y: the new y position (in 8px tile coordinates). */
void spawn_vertical(uint16_t x_coord, uint16_t new_y) {
    /* Assembly lines 17-26: Check event flags and spawn enable */
    if (event_flag_get(EVENT_FLAG_MONSTER_OFF)) return;
    if (event_flag_get(EVENT_FLAG_WIN_GIEGU)) return;
    if (!ow.enemy_spawns_enabled) return;

    /* Assembly lines 28-38: Validate x coordinate (must be 8-aligned, in bounds) */
    if ((x_coord & 0x0007) != 0) return;
    if (x_coord >= 0xFFF0) x_coord = 0;
    if (x_coord >= MAP_WIDTH_TILES8) return;

    /* Assembly lines 39-73: Convert to 64px grid coords.
     * x_coord -> x_grid (fixed), new_y -> y_start (iterating). */
    int16_t x_grid = (int16_t)((((uint16_t)x_coord << 1) >> 4) | (((x_coord << 1) & 0x8000) ? 0xF000 : 0));
    int16_t y_start = (int16_t)((((uint16_t)new_y << 1) >> 4) | (((new_y << 1) & 0x8000) ? 0xF000 : 0));

    /* Assembly lines 74-129: Iterate through rows */
    int16_t row = y_start;
    while (1) {
        /* BRANCHGTS: loop while y_start + 5 - row - 1 >= 0, i.e. row <= y_start + 4 */
        int16_t diff = (int16_t)(y_start + 5);
        if ((int16_t)(diff - row - 1) < 0)
            break;

        int16_t y_tile_start = row;
        ow.enemy_spawn_range_width = 8;
        ow.enemy_spawn_range_height = 8;
        uint16_t tile_count = 1;

        /* Assembly lines 83-108: Scan contiguous tiles vertically with same encounter_id */
        uint16_t first_id = get_map_enemy_placement((uint16_t)x_grid, (uint16_t)row);
        int16_t next_row = row + 1;

        if (first_id != 0) {
            while (tile_count < 6) {
                uint16_t next_id = get_map_enemy_placement((uint16_t)x_grid, (uint16_t)next_row);
                if (next_id != first_id) break;
                ow.enemy_spawn_range_height += 8;
                row = next_row;
                next_row = row + 1;
                tile_count++;
            }
        }

        /* Assembly lines 110-121: Attempt spawns (count down from tile_count) */
        if (first_id != 0) {
            for (uint16_t i = tile_count; i > 0; i--) {
                attempt_enemy_spawn((uint16_t)x_grid, (uint16_t)y_tile_start, first_id);
            }
        }

        row++;
    }
}

/*
 * initiate_enemy_encounter — Port of INITIATE_ENEMY_ENCOUNTER (C0D19B).
 *
 * Called by restore_camera_mode() after the 12-frame camera shake completes.
 * Determines battle initiative by comparing the enemy's movement direction
 * and the player's facing direction against the approach vector.
 *
 * Assembly: asm/overworld/initiate_enemy_encounter.asm (400 lines)
 *
 * Sections ported:
 *   1. Initiative determination (lines 17-99)
 *   2. Battle group setup + swirl (lines 100-119)
 *   3. Mark enemy entities in battle group (lines 120-194)
 *   4. FIND_PATH_TO_PARTY (lines 195-211)
 *   5. Pathfinding trim loop (lines 212-354)
 *   6. Entity state update + bt.enemies_in_battle population (lines 355-400)
 */
void initiate_enemy_encounter(void) {
    int16_t enemy_slot = bt.touched_enemy;  /* @LOCAL09 */
    ow.enemy_has_been_touched = 0;          /* line 19: STZ ENEMY_HAS_BEEN_TOUCHED */

    int16_t enemy_offset = enemy_slot;  /* @LOCAL08 */

    /* ================================================================
     * Section 1: Determine initiative (lines 20-99)
     *
     * Compute approach_direction = direction FROM enemy TO its target.
     * Compare with enemy's movement direction and player's facing direction
     * to determine who gets first strike.
     *
     * Variables:
     *   enemy_approaching: 1 if enemy moving toward player (from behind)
     *   player_facing_toward: 1 if player faces toward the enemy
     * ================================================================ */
    int16_t enemy_approaching;       /* Y in assembly */
    int16_t player_facing_toward;    /* X in assembly */

    int16_t enemy_moving_dir = entities.moving_directions[enemy_offset];

    if (enemy_moving_dir == 8) {
        /* Stationary enemy — default to player gets initiative (lines 28-32) */
        enemy_approaching = 0;
        player_facing_toward = 1;
    } else {
        /* Calculate approach direction: enemy -> target entity (lines 34-54) */
        int16_t target_offset = ert.enemy_pathfinding_target_entity;
        int16_t target_x = entities.abs_x[target_offset];
        int16_t target_y = entities.abs_y[target_offset];
        int16_t enemy_x = entities.abs_x[enemy_offset];
        int16_t enemy_y = entities.abs_y[enemy_offset];

        int16_t approach_dir = calculate_direction_8(enemy_x, enemy_y, target_x, target_y);

        /* Check if enemy is approaching from behind player (lines 55-69) */
        int16_t enemy_diff = (enemy_moving_dir - approach_dir) & 7;
        if (enemy_diff == 0 || enemy_diff == 1 || enemy_diff == 7) {
            enemy_approaching = 1;  /* enemy moving toward player */
        } else {
            enemy_approaching = 0;
        }

        /* Check if player is facing toward the enemy (lines 70-84)
         * If leader_direction ~ approach_direction, player faces same direction
         * as the approach vector (enemy->player), meaning player faces AWAY.
         * Assembly labels are misleading — X=0 means "facing away". */
        int16_t player_diff = (game_state.leader_direction - approach_dir) & 7;
        if (player_diff == 0 || player_diff == 1 || player_diff == 7) {
            player_facing_toward = 0;  /* player facing away from enemy */
        } else {
            player_facing_toward = 1;  /* player facing toward enemy */
        }
    }

    /* Determine initiative (lines 85-99) */
    bt.battle_initiative = INITIATIVE_NORMAL;
    if (player_facing_toward == 1 && enemy_approaching == 0) {
        bt.battle_initiative = INITIATIVE_PARTY_FIRST;
    }
    if (enemy_approaching == 1 && player_facing_toward == 0) {
        bt.battle_initiative = INITIATIVE_ENEMIES_FIRST;
    }

    /* ================================================================
     * Section 2: Battle group setup (lines 100-119)
     * ================================================================ */
    ow.battle_swirl_countdown = 120;

    uint16_t npc_id = entities.npc_ids[enemy_offset] & 0x7FFF;
    bt.current_battle_group = npc_id;

    /* Load battle entry pointer table and groups table */
    static const uint8_t *ptr_table = NULL;
    static const uint8_t *groups_table = NULL;
    if (!ptr_table)
        ptr_table = ASSET_DATA(ASSET_DATA_BTL_ENTRY_PTR_TABLE_BIN);
    if (!groups_table)
        groups_table = ASSET_DATA(ASSET_DATA_ENEMY_BATTLE_GROUPS_TABLE_BIN);

    /* Get pointer to enemy group data (lines 110-118) */
    const uint8_t *grp_ptr = NULL;
    if (ptr_table && groups_table) {
        uint32_t entry_off = (uint32_t)npc_id * 8;
        uint32_t rom_ptr = (uint32_t)ptr_table[entry_off]
                         | ((uint32_t)ptr_table[entry_off + 1] << 8)
                         | ((uint32_t)ptr_table[entry_off + 2] << 16);
        uint32_t groups_offset = rom_ptr - 0xD0D52D;
        grp_ptr = groups_table + groups_offset;
    }

    /* Play the battle swirl animation (line 119) */
    battle_swirl_sequence();

    /* ================================================================
     * Section 3: Mark enemy entities for battle (lines 120-194)
     *
     * Iterates 4 entries from the battle group data. For each entry,
     * finds matching entities by enemy_id and marks them with
     * pathfinding_state = -1 (participating in battle).
     * Also populates ert.pathfinding_enemy_ids[] and ert.pathfinding_enemy_counts[].
     * ================================================================ */
    for (int i = 0; i < 4; i++) {
        if (!grp_ptr) {
            ert.pathfinding_enemy_ids[i] = 0;
            ert.pathfinding_enemy_counts[i] = 0;
            continue;
        }

        uint8_t count = grp_ptr[0];
        int16_t enemy_id = 0;

        if (count == 0xFF) {
            /* End marker (lines 178-181) — store 0 for both */
            ert.pathfinding_enemy_ids[i] = 0;
            ert.pathfinding_enemy_counts[i] = 0;
            grp_ptr = NULL;  /* stop reading further entries */
            continue;
        }

        if (count == 0) {
            /* Zero count — just advance pointer (lines 130-133) */
            enemy_id = 0;
        } else {
            /* Read enemy_id from group data (lines 134-136) */
            enemy_id = (int16_t)read_u16_le(&grp_ptr[1]);

            /* First check the touched enemy itself (lines 137-147) */
            if (enemy_id == entities.enemy_ids[enemy_offset]) {
                entities.pathfinding_states[enemy_offset] = -1;
                count--;
            }

            /* Then scan all other entities for matching enemy_ids (lines 148-171) */
            if (count > 0) {
                for (int slot = 0; slot < 23; slot++) {
                    if (entities.script_table[slot] == -1)
                        continue;  /* no script = inactive entity */
                    if (enemy_id == entities.enemy_ids[slot]) {
                        entities.pathfinding_states[slot] = -1;
                    }
                }
            }
        }

        /* Store entry results (lines 182-190) */
        ert.pathfinding_enemy_ids[i] = enemy_id;
        ert.pathfinding_enemy_counts[i] = count;

        /* Advance group pointer by 3 bytes (line 173-176) */
        grp_ptr += 3;
    }

    /* ================================================================
     * Section 4: FIND_PATH_TO_PARTY (line 195-200)
     * ================================================================ */
    bt.enemies_in_battle = 0;
    PathfindingState *pf_state = find_path_to_party(
        game_state.party_count & 0xFF, 64, 64);

    /* ================================================================
     * Section 5: Pathfinding trim loop (lines 201-354)
     *
     * Re-reads the battle group data. For each group entry, counts how
     * many pathfinders matched that enemy_id. If more than needed,
     * removes excess by clearing the ones with the highest path cost.
     * ================================================================ */
    {
        /* Re-load group data pointer (same as Section 2, lines 203-211) */
        const uint8_t *trim_grp = NULL;
        if (ptr_table && groups_table) {
            uint32_t entry_off = (uint32_t)npc_id * 8;
            uint32_t rom_ptr = (uint32_t)ptr_table[entry_off]
                             | ((uint32_t)ptr_table[entry_off + 1] << 8)
                             | ((uint32_t)ptr_table[entry_off + 2] << 16);
            uint32_t groups_offset = rom_ptr - 0xD0D52D;
            trim_grp = groups_table + groups_offset;
        }

        for (int gi = 0; gi < 4 && trim_grp; gi++) {
            uint8_t needed_count = trim_grp[0];
            if (needed_count == 0xFF) {
                break;  /* end marker */
            }
            if (needed_count == 0) {
                trim_grp += 3;
                continue;
            }
            int16_t target_enemy_id = (int16_t)read_u16_le(&trim_grp[1]);

            /* Count pathfinders matching this enemy_id */
            int16_t match_count = 0;
            for (int16_t pi = 0; pi < pf_state->pathfinder_count; pi++) {
                int16_t obj_slot = pf_state->pathfinders[pi].object_index;
                if (entities.enemy_ids[obj_slot] == target_enemy_id)
                    match_count++;
            }

            /* If more pathfinders than needed, remove excess (highest cost first) */
            int16_t excess = match_count - (int16_t)needed_count;
            while (excess > 0) {
                /* Find the pathfinder with highest path_cost matching this enemy_id */
                int16_t best_idx = -1;
                int16_t best_cost = -1;
                for (int16_t pi = 0; pi < pf_state->pathfinder_count; pi++) {
                    int16_t obj_slot = pf_state->pathfinders[pi].object_index;
                    if (entities.enemy_ids[obj_slot] != target_enemy_id)
                        continue;
                    int16_t cost = pf_state->pathfinders[pi].path_cost;
                    if (cost > best_cost) {
                        best_cost = cost;
                        best_idx = pi;
                    }
                }

                if (best_idx < 0)
                    break;

                /* Don't remove the touched enemy */
                if (pf_state->pathfinders[best_idx].object_index == enemy_slot) {
                    excess--;
                    continue;
                }

                /* Remove this pathfinder: clear path_cost and entity state */
                pf_state->pathfinders[best_idx].path_cost = 0;
                int16_t rem_ent = pf_state->pathfinders[best_idx].object_index;
                entities.pathfinding_states[rem_ent] = 0;
                excess--;
            }

            trim_grp += 3;
        }
    }

    /* ================================================================
     * Section 6: Entity state update (lines 355-400)
     *
     * For each entity slot (0-22), excluding the touched enemy:
     *   - If pathfinding_state == -1: entity participates in battle
     *     -> clear TICK_DISABLED and MOVE_DISABLED
     *   - Otherwise: hide entity sprite (set bit 15 of spritemap_ptr_hi)
     *
     * Then add the touched enemy itself to bt.enemies_in_battle_ids[].
     * ================================================================ */
    for (int slot = 0; slot < 23; slot++) {
        if (slot == enemy_slot)
            continue;

        if (entities.pathfinding_states[slot] == -1) {
            /* Battle participant — enable tick and movement (lines 366-373) */
            entities.tick_callback_hi[slot] &=
                (uint16_t)~(uint16_t)(OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
        } else {
            /* Non-participant — hide sprite (lines 374-381) */
            entities.spritemap_ptr_hi[slot] |= 0x8000;
        }
    }

    /* Clear touched enemy's pathfinding state (line 392) */
    entities.pathfinding_states[enemy_offset] = 0;

    /* Add touched enemy to bt.enemies_in_battle (lines 393-399) */
    bt.enemies_in_battle_ids[bt.enemies_in_battle] = entities.enemy_ids[enemy_offset];
    bt.enemies_in_battle++;
}
