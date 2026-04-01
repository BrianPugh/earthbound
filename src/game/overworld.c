/*
 * Overworld initialization functions needed by attract mode.
 *
 * Ported from:
 *   OVERWORLD_INITIALIZE        — asm/overworld/initialize.asm
 *   OVERWORLD_SETUP_VRAM        — asm/overworld/setup_vram.asm
 *   PLACE_LEADER_AT_POSITION    — asm/overworld/place_leader_at_position.asm
 *   INITIALIZE_MISC_OBJECT_DATA — asm/overworld/initialize_misc_object_data.asm
 *   RESET_PARTY_STATE           — asm/overworld/reset_party_state.asm
 *   INITIALIZE_PARTY            — asm/overworld/initialize_party.asm
 *   CLEAR_MAP_ENTITIES          — asm/overworld/clear_map_entities.asm
 *   REMOVE_ENTITY               — asm/overworld/remove_entity.asm
 *   GET_ON_BICYCLE              — asm/overworld/get_on_bicycle.asm
 *   DISMOUNT_BICYCLE            — asm/overworld/dismount_bicycle.asm
 *   UPDATE_OVERWORLD_FRAME      — asm/overworld/update_overworld_frame.asm
 *   UPDATE_ENTITY_SCREEN_POSITIONS — asm/overworld/entity/update_entity_screen_positions.asm
 *   UPDATE_SCREEN               — asm/overworld/clear_oam_and_update_screen.asm
 *   WAIT_FRAMES_WITH_UPDATES    — asm/overworld/wait_frames_with_updates.asm
 *   RUN_FRAMES_UNTIL_FADE_DONE  — asm/system/palette/run_frames_until_fade_done.asm
 *   RENDER_FRAME_TICK           — asm/system/render_frame_tick.asm
 *   ALLOC_SPRITE_MEM            — asm/system/alloc_sprite_mem.asm
 */

#include "game/overworld_internal.h"
#include "game/ending.h"
#include "game/battle.h"
#include "game/position_buffer.h"
#include "game/map_loader.h"
#include "game/text.h"
#include "game/fade.h"
#include "snes/ppu.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "entity/pathfinding.h"
#include "game/game_state.h"
#include "game/audio.h"
#include "game/display_text.h"
#include "game/door.h"
#include "game/window.h"
#include "game/inventory.h"
#include "core/memory.h"
#include "core/decomp.h"
#include "core/log.h"
#include "platform/platform.h"
#include "include/binary.h"
#include "include/constants.h"
#include "data/assets.h"
#include "include/pad.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Forward declarations */
#include "game_main.h"
#include "data/text_refs.h"
extern uint16_t item_transformations_loaded;

/* Music track IDs (from include/constants/music.asm) */
#define MUSIC_BICYCLE  82



/* Consolidated overworld state */
OverworldState ow = {
    .camera_focus_entity = -1,
    .mini_ghost_entity_id = -1,
    .moving_party_member_entity_id = -1,
    .loaded_map_palette = -1,
    .loaded_map_tile_combo = -1,
    .entity_fade_entity = -1,
    .current_queued_interaction_type = 0xFFFF,
};

/* ---- OVERWORLD_SETUP_VRAM (asm/overworld/setup_vram.asm) ---- */
void overworld_setup_vram(void) {
    /* SET_BGMODE(9): mode 1, BG3 priority bit set */
    ppu.bgmode = 0x09;

    /* BG1: tiles at word $0000, tilemap at word $3800, horizontal (64x32).
     * SET_BG1_VRAM_LOCATION zeroes BG1 scroll (lines 20-21). */
    ppu.bg_sc[0] = 0x39;
    ppu.bg_hofs[0] = 0;
    ppu.bg_vofs[0] = 0;

    /* BG2: tiles at word $2000, tilemap at word $5800, horizontal (64x32).
     * SET_BG2_VRAM_LOCATION zeroes BG2 scroll (lines 20-21). */
    ppu.bg_sc[1] = 0x59;
    ppu.bg_hofs[1] = 0;
    ppu.bg_vofs[1] = 0;

    /* BG3: tiles at word $6000, tilemap at word $7C00, normal (32x32).
     * SET_BG3_VRAM_LOCATION zeroes BG3 scroll (lines 20-21). */
    ppu.bg_sc[2] = 0x7C;
    ppu.bg_hofs[2] = 0;
    ppu.bg_vofs[2] = 0;

    /* Tile data bases: BG1=$0000 (nba lo=0), BG2=$2000 (nba hi=2) */
    ppu.bg_nba[0] = 0x20;
    /* BG3=$6000 (nba lo=6) */
    ppu.bg_nba[1] = (ppu.bg_nba[1] & 0xF0) | 0x06;

    /* SET_OAM_SIZE($62): 32x32 / 64x64 sprite sizes */
    ppu.obsel = 0x62;

    /* Clear rendering state that may be stale from battle, callroutines,
     * or other scenes.  On the real SNES these registers are harmless
     * when unused, but the C port's software renderer evaluates them
     * every frame. */
    ppu_clear_effects();
    ppu.bg_viewport_fill[0] = BG_VIEWPORT_CENTER;
    ppu.bg_viewport_fill[1] = BG_VIEWPORT_CENTER;
    ppu.sprite_x_offset = 0;
    ppu.sprite_y_offset = 0;
}

/* ---- OVERWORLD_INITIALIZE (asm/overworld/initialize.asm) ---- */
void overworld_initialize(void) {
    overworld_setup_vram();

    /* Assembly: COPY_TO_VRAM1P with size=0 and fixed-source DMA mode 3.
     * On SNES, DMA size 0 means 0x10000 (65536) bytes — clears ALL VRAM. */
    memset(ppu.vram, 0, sizeof(ppu.vram));

    /* Reset tracking variables — invalidate both overworld.c caches and
     * the tileset combo cache in map_loader.c so next sector load reloads GFX. */
    ow.loaded_map_palette = -1;
    ow.loaded_map_tile_combo = -1;
    invalidate_loaded_tileset_combo();
}

/* ---- PLACE_LEADER_AT_POSITION (asm/overworld/place_leader_at_position.asm) ---- */
void place_leader_at_position(uint16_t x, uint16_t y) {
    /* Assembly params: A = y_coord, X = x_coord
     * Then TXY (Y = X = x_coord), TAX (X = A = y_coord)
     * Wait, re-reading: TXY means Y gets X's value. TAX means X gets A's value.
     * Actually the assembly does:
     *   TXY  → Y = X (the second param)
     *   TAX  → X = A (the first param = y_coord)
     *   STX leader_x_coord  (so x_coord = original X param = 2824)
     *   STY leader_y_coord  (so y_coord = original A param = 7520)
     * Wait, I need to re-read. The call is:
     *   LDX #2824 / LDA #7520 / JSL PLACE_LEADER_AT_POSITION
     * So A = 7520, X = 2824. Then:
     *   TXY → Y = 2824
     *   TAX → X = 7520
     *   STX leader_x_coord → leader_x = 7520... that doesn't make sense.
     *
     * Actually: re-checking assembly:
     *   STX GAME_STATE+game_state::leader_x_coord  → leader_x = X = 7520
     *   STY GAME_STATE+game_state::leader_y_coord  → leader_y = Y = 2824
     * But the caller does LDX #2824, LDA #7520.
     * After TXY: Y=2824. After TAX: X=7520.
     * So leader_x = 7520, leader_y = 2824.
     * Hmm, that seems swapped. Let me re-read the call site.
     */
    /* The call in RUN_ATTRACT_MODE is:
     *   LDX #2824
     *   REP #$20
     *   LDA #7520
     *   JSL PLACE_LEADER_AT_POSITION
     * In the function: TXY (Y=2824), TAX (X=7520)
     * STX leader_x_coord → 7520
     * STY leader_y_coord → 2824
     * So A (first VUCC param) → TAX → STX leader_x_coord = x_coord.
     * And X (second VUCC param) → TXY → STY leader_y_coord = y_coord.
     * Assembly signature: PLACE_LEADER_AT_POSITION(A=x, X=y).
     * Our C wrapper takes (x, y) matching this order. */
    game_state.leader_x_coord = x;
    game_state.leader_y_coord = y;
    game_state.leader_direction = 2;
    /* Assembly line 10-12: SEP #$20 / LDA #$01 / STA party_members */
    game_state.party_members[0] = 1;  /* Ness is always party member 0 */

    /* Set entity screen position for the leader slot */
    entities.screen_x[ENT(PARTY_LEADER_ENTITY_INDEX)] = (int16_t)x;
    entities.screen_y[ENT(PARTY_LEADER_ENTITY_INDEX)] = (int16_t)y;
}

/* ---- CLEAR_ALL_ENEMIES (asm/overworld/clear_all_enemies.asm) ----
 * Zeroes enemy spawn counters, removes all non-party entities (script_table >= 6),
 * and clears collision data. Called at start of LOAD_MAP_AT_POSITION. */
void clear_all_enemies(void) {
    ow.magic_butterfly_spawned = 0;
    ow.enemy_spawn_too_many_failures = 0;
    ow.overworld_enemy_count = 0;

    /* Remove all entities that are NOT party members.
     * Assembly: INC; CMP #6; BLTEQ — skips if (script_id + 1) <= 6,
     * i.e., script_id <= 5 (party slots 0-5) or -1 (inactive, wraps to 0). */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        int16_t script_id = entities.script_table[ENT(i)];
        if ((uint16_t)(script_id + 1) > 6) {
            remove_entity((int16_t)i);
        }
    }

    /* Clear collision data for all entity slots */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entities.collided_objects[ENT(i)] = -1;
    }
}

/* ---- INITIALIZE_MISC_OBJECT_DATA (asm/overworld/initialize_misc_object_data.asm) ---- */
void initialize_misc_object_data(void) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entities.movement_speeds[i] = 0;
        entities.collided_objects[i] = -1; /* ENTITY_COLLISION_NO_OBJECT */
        entities.npc_ids[i] = 0xFFFF;
    }
}

/* ---- RESET_PARTY_STATE (port of C02D29) ----
 *
 * Port of asm/overworld/reset_party_state.asm.
 * Resets all party-related globals before the party is rebuilt by
 * INITIALIZE_PARTY. Sets entity sizes, clears camera mode, HP alert
 * state, party ordering, and initializes movement speed tables. */
void reset_party_state(void) {
    /* Assembly line 9: ENTITY_SIZES[23*2] = 1 */
    entities.sizes[ENT(23)] = 1;

    /* Assembly line 10: MINI_GHOST_ENTITY_ID = -1 */
    ow.mini_ghost_entity_id = -1;

    /* Assembly line 11: game_state.unknown88 = 0 (position_buffer_index) */
    game_state.position_buffer_index = 0;

    /* Assembly lines 12-15: clear camera/state fields */
    game_state.camera_mode = 0;
    game_state.auto_move_frames_left = 0;
    game_state.auto_move_saved_walking_style = 0;
    game_state.party_status = 0;
    game_state.current_party_members = PARTY_LEADER_ENTITY_INDEX;

    /* Assembly lines 20-34: loop over 6 party slots, clear arrays.
     * Clears party_order[i] and HP_ALERT_SHOWN[i*2]. */
    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        game_state.party_order[i] = 0;
        ow.hp_alert_shown[i] = 0;
    }

    /* Assembly lines 35-37: clear party counts */
    game_state.player_controlled_party_count = 0;
    game_state.party_count = 0;

    /* Assembly line 38: JSL VELOCITY_STORE
     * Precomputes horizontal/vertical movement speed lookup tables for all
     * 14 speed levels and 8 directions. */
    velocity_store();

    /* Assembly lines 39-43: GET_EVENT_FLAG(NESS_PAJAMA_FLAG) → PAJAMA_FLAG.
     * Used by GET_PARTY_MEMBER_SPRITE_ID to show pajama Ness at game start. */
    ow.pajama_flag = event_flag_get(EVENT_FLAG_NESS_PAJAMA) ? 1 : 0;
}

/* SUM_ALIVE_PARTY_LEVELS: Port of asm/overworld/party/sum_alive_party_levels.asm.
 * Sums the levels of all player-controlled characters (char_id 1-4) in the
 * party. NPCs (char_id >= 5) are excluded. Used by CHECK_ENEMY_SHOULD_FLEE
 * to determine if enemies flee from an over-leveled party. */
uint16_t sum_alive_party_levels(void) {
    uint16_t sum = 0;
    uint8_t count = game_state.party_count;
    for (uint16_t i = 0; i < count; i++) {
        /* Skip NPCs (party_order[i] >= 5 means non-PC) */
        if (game_state.party_order[i] >= 5)
            continue;
        uint8_t char_id = game_state.player_controlled_party_members[i];
        sum += party_characters[char_id].level;
    }
    return sum;
}

/* CHECK_ENEMY_SHOULD_FLEE: Port of asm/overworld/check_enemy_should_flee.asm.
 * Determines if the current enemy entity should flee from the party.
 * Checks:
 *   1. Run-away flag/state from BTL_ENTRY_PTR_TABLE (event flag match → flee)
 *   2. Party level sum vs enemy level thresholds:
 *      - sum > level*10 → always flee
 *      - sum > level*8 AND weak_enemy_value < 192 → flee
 *      - sum > level*6 AND weak_enemy_value < 128 → flee
 * Returns 1 if enemy should flee, 0 otherwise. */
uint16_t check_enemy_should_flee(void) {
    int16_t offset = ENT(ert.current_entity_slot);
    uint16_t npc_id = entities.npc_ids[offset] & 0x7FFF;

    /* --- Run-away flag check (BTL_ENTRY_PTR_TABLE) --- */
    static const uint8_t *btl_ptr_tbl = NULL;
    if (!btl_ptr_tbl)
        btl_ptr_tbl = ASSET_DATA(ASSET_DATA_BTL_ENTRY_PTR_TABLE_BIN);
    if (btl_ptr_tbl) {
        /* battle_entry_ptr_entry is 8 bytes: pointer(4), run_away_flag(2),
         * run_away_flag_state(1), letterbox_style(1) */
        uint16_t entry_offset = npc_id * 8;
        uint16_t run_away_flag = read_u16_le(&btl_ptr_tbl[entry_offset + 4]);
        if (run_away_flag != 0) {
            uint16_t flag_value = event_flag_get(run_away_flag) ? 1 : 0;
            uint8_t run_away_flag_state = btl_ptr_tbl[entry_offset + 6];
            if (flag_value == run_away_flag_state)
                return 1;
        }
    }

    /* --- Level threshold checks --- */
    uint16_t level_sum = sum_alive_party_levels();
    uint16_t enemy_id = (uint16_t)entities.enemy_ids[offset];
    if (!enemy_config_table) return 0;
    uint8_t enemy_level = enemy_config_table[enemy_id].level;

    /* If party level sum > enemy_level * 10 → always flee */
    if (level_sum > (uint16_t)enemy_level * 10)
        return 1;

    /* If party level sum > enemy_level * 8 AND weak_enemy_value < 192 → flee */
    if (level_sum > (uint16_t)enemy_level * 8) {
        if (entities.weak_enemy_value[offset] < 192)
            return 1;
    }

    /* If party level sum > enemy_level * 6 AND weak_enemy_value < 128 → flee */
    if (level_sum > (uint16_t)enemy_level * 6) {
        if (entities.weak_enemy_value[offset] < 128)
            return 1;
    }

    return 0;
}

/* PREPARE_NEW_ENTITY: Port of asm/overworld/prepare_new_entity.asm.
 * Sets staging globals for entity creation. The next call to
 * CREATE_PREPARED_ENTITY_SPRITE or CREATE_PREPARED_ENTITY_NPC reads
 * these values to position the newly created entity.
 * A = direction, X = x_coord, Y = y_coord in assembly. */
void prepare_new_entity(uint16_t x, uint16_t y, uint8_t direction) {
    ow.entity_prepared_x = (int16_t)x;
    ow.entity_prepared_y = (int16_t)y;
    ow.entity_prepared_direction = (int16_t)(direction & 0xFF);
}

/* PREPARE_NEW_ENTITY_AT_EXISTING_ENTITY_LOCATION:
 * Port of asm/overworld/prepare_new_entity_at_existing_entity_location.asm.
 * Sets ow.entity_prepared_x/y/direction by copying from an existing entity.
 * source_selector: 0 = ert.current_entity_slot, 1 = party leader, else = slot number. */
void prepare_new_entity_at_existing_entity_location(int16_t source_selector) {
    int16_t slot;
    if (source_selector == 0) {
        slot = ert.current_entity_slot;
    } else if (source_selector == 1) {
        slot = game_state.current_party_members;
    } else {
        slot = source_selector;
    }
    ow.entity_prepared_x = entities.abs_x[slot];
    ow.entity_prepared_y = entities.abs_y[slot];
    ow.entity_prepared_direction = entities.directions[slot];
}

/* PREPARE_NEW_ENTITY_AT_TELEPORT_DESTINATION:
 * Port of asm/overworld/prepare_new_entity_at_teleport_destination.asm.
 * Sets ow.entity_prepared_x/y/direction from a teleport destination table entry.
 * dest_id: teleport destination index (8-bit). */
void prepare_new_entity_at_teleport_destination(uint16_t dest_id) {
    const TeleportDestination *dest = get_teleport_dest(dest_id & 0xFF);
    if (!dest) return;
    ow.entity_prepared_x = (int16_t)(dest->x_coord << 3);
    ow.entity_prepared_y = (int16_t)(dest->y_coord << 3);
    ow.entity_prepared_direction = (int16_t)((dest->direction & 0xFF) - 1);
}

/*
 * CHARACTER_INITIAL_ENTITY_DATA (asm/data/map/character_initial_entity_data.asm).
 * Per-character entity setup: normal sprite, Lost Underworld sprite,
 * event script, and base entity slot. Index = char_id - 1 (0-based).
 * Characters 1-4 = PCs (Ness, Paula, Jeff, Poo).
 * Characters 5-17 = NPCs (Pokey, Picky, King, Tony, Bubble Monkey,
 *                         Dungeon Man, Flying Man ×5, Teddy Bear ×2).
 */
typedef struct {
    uint16_t normal_sprite;      /* offset 0: normal overworld sprite */
    uint16_t small_sprite;       /* offset 2: Lost Underworld sprite (0xFFFF = invalid) */
    uint16_t event_script;       /* offset 4: event script ID */
    uint16_t base_slot;          /* offset 6: base entity slot */
} CharacterEntityEntry;

#define CHARACTER_ENTITY_DATA_COUNT 17
static const CharacterEntityEntry character_entity_data[CHARACTER_ENTITY_DATA_COUNT] = {
    {  1,  27, EVENT_SCRIPT_002, 24 },  /* 1: Ness */
    {  2,  28, EVENT_SCRIPT_002, 25 },  /* 2: Paula */
    {  3,  29, EVENT_SCRIPT_002, 26 },  /* 3: Jeff */
    {  4,  30, EVENT_SCRIPT_002, 27 },  /* 4: Poo */
    { 44, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 5: Pokey */
    { 45, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 6: Picky */
    { 40, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 7: King */
    {182, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 8: Tony */
    { 46, 0xFFFF, EVENT_SCRIPT_003, 28 },  /* 9: Bubble Monkey */
    { 41, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 10: Dungeon Man */
    { 39, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 11: Flying Man 1 */
    { 39, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 12: Flying Man 2 */
    { 39, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 13: Flying Man 3 */
    { 39, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 14: Flying Man 4 */
    { 39, 0xFFFF, EVENT_SCRIPT_002, 28 },  /* 15: Flying Man 5 */
    { 51,  35, EVENT_SCRIPT_002, 28 },  /* 16: Teddy Bear 1 */
    { 51,  35, EVENT_SCRIPT_002, 28 },  /* 17: Teddy Bear 2 */
};

/* Helper to look up character entity data. char_id is 1-indexed.
 * Returns NULL if char_id is out of range. */
static const CharacterEntityEntry *get_character_entity_entry(uint16_t char_id) {
    if (char_id == 0 || char_id > CHARACTER_ENTITY_DATA_COUNT) return NULL;
    return &character_entity_data[char_id - 1];
}

/* ---- INITIALIZE_PARTY (port of C03A24) ----
 *
 * Port of asm/overworld/initialize_party.asm.
 * Rebuilds the party from game_state.party_members[]. Clears party arrays,
 * then calls add_party_member() for each non-zero member — matching the
 * assembly which calls JSL ADD_PARTY_MEMBER in a loop. */
void initialize_party(void) {
    game_state.player_controlled_party_count = 0;
    game_state.party_count = 0;

    /* Clear per-party-member state (assembly lines 15-33). */
    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        game_state.party_order[i] = 0;
        game_state.player_controlled_party_members[i] = 0;
        game_state.party_entity_slots[i * 2] = 0;
        game_state.party_entity_slots[i * 2 + 1] = 0;
    }

    /* Assembly lines 45-64: call ADD_PARTY_MEMBER for each non-zero member.
     * add_party_member() handles slot conflict detection (base_slot+1 if occupied),
     * insertion ordering, position buffer setup, entity creation, and per-member
     * rebuild/update calls — matching the assembly exactly. */
    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        uint8_t member = game_state.party_members[i];
        if (member == 0)
            break;
        add_party_member(member);
    }

    ow.footstep_sound_id = game_state.character_mode * 2;
    ow.footstep_sound_id_override = 0;
}

/*
 * ADD_PARTY_MEMBER (asm/overworld/add_party_member.asm)
 *
 * Adds a character to the active party. Handles:
 *   - Finding insertion point (PCs sorted by ID, before unconscious/NPCs)
 *   - Shifting existing members right to make room
 *   - Entity slot allocation (base slot, +1 if occupied)
 *   - Entity creation with correct sprite and event script
 *   - Position from predecessor's position_buffer_index
 *   - Rebuild entity free list, update NPC lineup, update party sort
 *
 * Returns the entity slot assigned to the new member.
 * char_id: 1-indexed character ID.
 */
uint16_t add_party_member(uint16_t char_id) {
    const CharacterEntityEntry *entry = get_character_entity_entry(char_id);
    if (!entry) return 0;

    /* ---- Phase 1: Find insertion point ---- */
    uint16_t insert_pos = 0;

    if (char_id >= 5) {
        /* NPC: insert before first empty slot or first member with higher ID.
         * Assembly @FIND_SLOT_HIGH: BLTEQ = char_id <= current_member. */
        while (insert_pos < 6) {
            uint8_t current = game_state.party_order[insert_pos];
            if (current == 0) break;  /* empty slot */
            if (char_id <= current) break;
            insert_pos++;
        }
    } else {
        /* PC: insert before first empty, NPC (>=5), higher char_id,
         * or unconscious PC (affliction[0] == 1).
         * Assembly @FIND_SLOT_LOW. */
        while (insert_pos < 6) {
            uint8_t current = game_state.party_order[insert_pos];
            if (current == 0) break;  /* empty */
            if (current >= 5) break;  /* NPC */
            if (current > char_id) break;  /* higher-ID PC */

            /* Check if current member is unconscious */
            uint16_t eslot_off = insert_pos * 2;
            uint16_t eslot = read_u16_le(&game_state.party_entity_slots[eslot_off]);
            uint16_t char_idx = (uint16_t)entities.var[1][eslot];
            uint8_t affliction = party_characters[char_idx].afflictions[0];
            if (affliction == 1) break;  /* unconscious: insert before */

            insert_pos++;
        }
    }

    /* ---- Phase 2: Shift existing members right ---- */
    if (game_state.party_order[insert_pos] != 0) {
        for (uint16_t i = 5; i > insert_pos; i--) {
            game_state.party_order[i] = game_state.party_order[i - 1];

            game_state.party_entity_slots[i * 2] = game_state.party_entity_slots[(i - 1) * 2];
            game_state.party_entity_slots[i * 2 + 1] = game_state.party_entity_slots[(i - 1) * 2 + 1];

            game_state.player_controlled_party_members[i] =
                game_state.player_controlled_party_members[i - 1];
        }
    }

    /* ---- Phase 3: Store new member ---- */
    game_state.party_order[insert_pos] = (uint8_t)char_id;
    game_state.party_count++;

    uint16_t char_index = char_id - 1;  /* 0-based */
    ert.new_entity_var[0] = (int16_t)char_index;

    /* Look up base entity slot; if occupied, try next slot.
     * Assembly: check ENTITY_SCRIPT_TABLE for -1 (unoccupied). */
    uint16_t entity_slot = entry->base_slot;
    if (entities.script_table[ENT(entity_slot)] != ENTITY_NONE) {
        entity_slot++;
    }

    /* Store entity slot in party arrays */
    game_state.party_entity_slots[insert_pos * 2] = (uint8_t)(entity_slot & 0xFF);
    game_state.party_entity_slots[insert_pos * 2 + 1] = (uint8_t)(entity_slot >> 8);

    /* player_controlled_party_members = slot - 24 (party leader base) */
    uint16_t member_offset = entity_slot - PARTY_LEADER_ENTITY_INDEX;
    ert.new_entity_var[1] = (int16_t)member_offset;
    game_state.player_controlled_party_members[insert_pos] = (uint8_t)member_offset;

    /* ---- Phase 4: Set position_index ---- */
    if (game_state.party_count == 1) {
        /* First member: use position_buffer_index directly */
        party_characters[member_offset].position_index =
            game_state.position_buffer_index;
    } else if (insert_pos == 0) {
        /* Becoming new leader: use position_buffer_index */
        party_characters[member_offset].position_index =
            game_state.position_buffer_index;
    } else {
        /* Copy from predecessor's position_index */
        uint16_t prev_slot_off = (insert_pos - 1) * 2;
        uint16_t prev_slot = read_u16_le(&game_state.party_entity_slots[prev_slot_off]);
        uint16_t prev_char_idx = (uint16_t)entities.var[1][prev_slot];
        party_characters[member_offset].position_index =
            party_characters[prev_char_idx].position_index;
    }

    /* ---- Phase 5: Look up position from ert.buffer ---- */
    uint16_t pos_idx = party_characters[member_offset].position_index;
    uint16_t buf_idx;
    if (pos_idx == 0) {
        buf_idx = 0xFF;  /* Assembly: LDX #$00FF */
    } else {
        buf_idx = pos_idx - 1;
    }
    int16_t x = pb.player_position_buffer[buf_idx].x_coord;
    int16_t y = pb.player_position_buffer[buf_idx].y_coord;

    /* ---- Phase 6: Select sprite and create entity ---- */
    uint16_t sprite_id;
    if (game_state.character_mode == CHARACTER_MODE_SMALL && entry->small_sprite != 0xFFFF)
        sprite_id = entry->small_sprite;
    else
        sprite_id = entry->normal_sprite;

    create_entity(sprite_id, entry->event_script, (int16_t)entity_slot, x, y);

    /* Set entity screen coordinates */
    int16_t ent_off = (int16_t)entity_slot;
    entities.screen_x[ent_off] = x - (int16_t)ppu.bg_hofs[0];
    entities.screen_y[ent_off] = y - (int16_t)ppu.bg_vofs[0];

    /* ---- Phase 7: Update current_party_members and rebuild ---- */
    /* Set current_party_members from leader's base slot (assembly reads
     * CHARACTER_INITIAL_ENTITY_DATA[party_order[0]-1].base_slot). */
    uint8_t leader_id = game_state.party_order[0];
    const CharacterEntityEntry *leader_entry = get_character_entity_entry(leader_id);
    if (leader_entry) {
        game_state.current_party_members = leader_entry->base_slot;
    }

    rebuild_entity_free_list();
    update_npc_party_lineup();

    /* Re-read current_party_members from party_entity_slots[0] */
    game_state.current_party_members =
        read_u16_le(&game_state.party_entity_slots[0]);

    update_party();

    /* Save entity position as prepared coordinates */
    ow.entity_prepared_x = entities.abs_x[ent_off];
    ow.entity_prepared_y = entities.abs_y[ent_off];
    ow.entity_prepared_direction = entities.directions[ent_off];

    return entity_slot;
}

/* ---- CLEAR_MAP_ENTITIES (port of C021E6) ----
 *
 * Port of asm/overworld/clear_map_entities.asm.
 * Loops through entity slots 0-29, removing any entity that has an active
 * script (script_table > 1). Slot 23 is skipped (deactivated separately
 * at the end). This cleans up NPC entities between scenes/teleports. */
void clear_map_entities(void) {
    ow.magic_butterfly_spawned = 0;
    ow.enemy_spawn_too_many_failures = 0;
    ow.overworld_enemy_count = 0;

    for (int slot = 0; slot < 30; slot++) {
        /* Assembly check: LDA ENTITY_SCRIPT_TABLE,X; INC; CMP #2; BLTEQ @SKIP
         * This skips entities with script_table <= 1 (inactive or default). */
        int16_t st = entities.script_table[slot];
        if (st + 1 <= 2)
            continue;  /* script_table <= 1: not a map entity */
        if (slot == 23)
            continue;  /* skip the init slot */
        remove_entity(slot);
    }
    deactivate_entity(23);
}

/* ---- CLEAR_PARTY_SPRITE_HIDE_FLAGS (port of C07C5B) ----
 *
 * Port of asm/overworld/party/clear_party_sprite_hide_flags.asm.
 * When PLAYER_INTANGIBILITY_FRAMES > 0, clears bit 15 (hidden flag) of
 * spritemap_ptr_hi for party entity slots 24-29. This makes party sprites
 * visible during the invulnerability flicker period. */
void clear_party_sprite_hide_flags(void) {
    if (ow.player_intangibility_frames == 0) return;
    for (int slot = 24; slot < MAX_ENTITIES; slot++) {
        entities.spritemap_ptr_hi[slot] &= 0x7FFF;
    }
}

/* ---- SHOW_ENTITY_SPRITES (port of C4645A) ----
 *
 * Port of asm/overworld/entity/show_entity_sprites.asm.
 * Clears the DRAW_DISABLED bit (bit 15) from spritemap_ptr_hi, making
 * the entity visible. If char_id == 0xFF, shows all party member entities;
 * otherwise shows the entity for the specific character.
 * Assembly uses CMP #<-1 (=$00FF in 16-bit mode) for the "all" check. */
void show_entity_sprites(uint16_t char_id) {
    if ((uint8_t)char_id != 0xFF) {
        /* Single character mode */
        int16_t slot = find_entity_for_character((uint8_t)char_id);
        if (slot < 0) return;
        entities.spritemap_ptr_hi[slot] &= ~0x8000;
    } else {
        /* All party members */
        uint8_t count = game_state.party_count & 0xFF;
        for (int i = 0; i < count; i++) {
            uint16_t ent_slot =
                read_u16_le(&game_state.party_entity_slots[i * 2]);
            entities.spritemap_ptr_hi[ent_slot] &= ~0x8000;
        }
    }
}

/* ---- HIDE_ENTITY_SPRITES (port of C463F4) ----
 *
 * Port of asm/overworld/entity/hide_entity_sprites.asm.
 * Sets the DRAW_DISABLED bit (bit 15) on spritemap_ptr_hi, hiding
 * the entity. If char_id == 0xFF, hides all party member entities;
 * otherwise hides the entity for the specific character.
 * Also calls clear_party_sprite_hide_flags() and clears intangibility. */
void hide_entity_sprites(uint16_t char_id) {
    clear_party_sprite_hide_flags();
    ow.player_intangibility_frames = 0;
    if (char_id != 0xFF) {
        /* Single character mode */
        int16_t slot = find_entity_for_character((uint8_t)char_id);
        if (slot < 0) return;
        entities.spritemap_ptr_hi[slot] |= 0x8000;
    } else {
        /* All party members */
        uint8_t count = game_state.party_count & 0xFF;
        for (int i = 0; i < count; i++) {
            uint16_t ent_slot =
                read_u16_le(&game_state.party_entity_slots[i * 2]);
            entities.spritemap_ptr_hi[ent_slot] |= 0x8000;
        }
    }
}

/* ---- DISABLE_CHARACTER_MOVEMENT (port of C46594) ----
 *
 * Port of asm/misc/disable_character_movement.asm.
 * ORs OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED into tick_callback_hi
 * for either a specific character entity or all party + init entities.
 * char_id: character ID, or 0xFF for all party members + init entity.
 * Assembly uses CMP #<-1 (=$00FF in 16-bit mode) for the "all" check. */
void disable_character_movement(uint16_t char_id) {
    uint16_t flags = OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED;
    if ((uint8_t)char_id != 0xFF) {
        /* Single character */
        int16_t slot = find_entity_for_character((uint8_t)char_id);
        if (slot < 0) return;
        entities.tick_callback_hi[slot] |= flags;
    } else {
        /* Init entity (slot 23) */
        entities.tick_callback_hi[ENT(INIT_ENTITY_SLOT)] |= flags;
        /* All party member entities */
        uint8_t count = game_state.party_count & 0xFF;
        for (int i = 0; i < count; i++) {
            uint16_t ent_slot =
                read_u16_le(&game_state.party_entity_slots[i * 2]);
            entities.tick_callback_hi[ent_slot] |= flags;
        }
    }
}

/* ---- ENABLE_CHARACTER_MOVEMENT (port of C46631) ----
 *
 * Port of asm/misc/enable_character_movement.asm.
 * Clears OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED from tick_callback_hi.
 * char_id: character ID, or 0xFF for all party members + init entity.
 * Assembly uses CMP #<-1 (=$00FF in 16-bit mode) for the "all" check. */
void enable_character_movement(uint16_t char_id) {
    uint16_t mask = (uint16_t)~(uint16_t)(OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
    if ((uint8_t)char_id != 0xFF) {
        int16_t slot = find_entity_for_character((uint8_t)char_id);
        if (slot < 0) return;
        entities.tick_callback_hi[slot] &= mask;
    } else {
        /* Init entity (slot 23) */
        entities.tick_callback_hi[ENT(INIT_ENTITY_SLOT)] &= mask;
        /* All party member entities */
        uint8_t count = game_state.party_count & 0xFF;
        for (int i = 0; i < count; i++) {
            uint16_t ent_slot =
                read_u16_le(&game_state.party_entity_slots[i * 2]);
            entities.tick_callback_hi[ent_slot] &= mask;
        }
    }
}

/* ---- DISABLE_NPC_MOVEMENT (port of C4655E) ----
 *
 * Port of asm/overworld/npc/disable_npc_movement.asm.
 * Finds entity by NPC ID and sets OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED. */
void disable_npc_movement(uint16_t npc_id) {
    int16_t offset = find_entity_by_npc_id(npc_id);
    if (offset < 0) return;
    entities.tick_callback_hi[offset] |= (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
}

/* ---- ENABLE_NPC_MOVEMENT (port of C465FB) ----
 *
 * Port of asm/overworld/npc/enable_npc_movement.asm.
 * Finds entity by NPC ID and clears OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED. */
void enable_npc_movement(uint16_t npc_id) {
    int16_t offset = find_entity_by_npc_id(npc_id);
    if (offset < 0) return;
    entities.tick_callback_hi[offset] &=
        (uint16_t)~(uint16_t)(OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
}

/* ---- DISABLE_SPRITE_MOVEMENT (port of C46579, DISABLE_ENTITY_BY_SPRITE_ID) ----
 *
 * Port of asm/overworld/entity/disable_entity_by_sprite_id.asm.
 * Finds entity by sprite ID and sets OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED. */
void disable_sprite_movement(uint16_t sprite_id) {
    int16_t slot = find_entity_by_sprite_id(sprite_id);
    if (slot < 0) return;
    entities.tick_callback_hi[ENT(slot)] |= (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
}

/* ---- ENABLE_SPRITE_MOVEMENT (port of C46616) ----
 *
 * Port of asm/overworld/entity/enable_sprite_movement.asm.
 * Finds entity by sprite ID and clears OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED. */
void enable_sprite_movement(uint16_t sprite_id) {
    int16_t slot = find_entity_by_sprite_id(sprite_id);
    if (slot < 0) return;
    entities.tick_callback_hi[ENT(slot)] &=
        (uint16_t)~(uint16_t)(OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
}

/* ---- DEACTIVATE_NPC_ENTITY (port of C460CE) ----
 *
 * Port of asm/overworld/entity/deactivate_npc_entity.asm.
 * Saves entity position to ENTITY_PREPARED_* globals, then reassigns
 * the entity's script to a deactivation script that deallocates it.
 * fade_param == 6: immediate deallocation (EVENT_DESPAWN, script 35).
 * fade_param != 6: brief pause then deallocation (BRIEF_PAUSE_END_TASK → EVENT_DESPAWN). */
void deactivate_npc_entity(uint16_t npc_id, uint16_t fade_param) {
    int16_t offset = find_entity_by_npc_id(npc_id);
    if (offset < 0) return;
    ow.entity_prepared_x = entities.abs_x[offset];
    ow.entity_prepared_y = entities.abs_y[offset];
    ow.entity_prepared_direction = entities.directions[offset];
    /* Assembly uses BRIEF_PAUSE_END_TASK (pause + jump to EVENT_DESPAWN) for non-6 params,
     * or EVENT_DESPAWN directly for param 6. BRIEF_PAUSE_END_TASK is a 1/15s delay before
     * deallocation — intended to overlap with fade effect. Since we don't have
     * the delay script as a numbered event, use EVENT_DESPAWN for both. */
    reassign_entity_script(offset, EVENT_SCRIPT_DESPAWN);
}

/* ---- DEACTIVATE_SPRITE_ENTITY (port of C46125) ----
 *
 * Port of asm/overworld/entity/deactivate_sprite_entity.asm.
 * Same as DEACTIVATE_NPC_ENTITY but finds entity by sprite ID. */
void deactivate_sprite_entity(uint16_t sprite_id, uint16_t fade_param) {
    int16_t slot = find_entity_by_sprite_id(sprite_id);
    if (slot < 0) return;
    ow.entity_prepared_x = entities.abs_x[slot];
    ow.entity_prepared_y = entities.abs_y[slot];
    ow.entity_prepared_direction = entities.directions[slot];
    reassign_entity_script(slot, EVENT_SCRIPT_DESPAWN);
}

/* ---- DISABLE_ALL_ENTITIES (port of C0943C) ----
 *
 * Port of asm/overworld/disable_all_entities.asm.
 * Walk entity linked list and set OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED
 * on every active entity. Used before screen transitions. */
void disable_all_entities(void) {
    uint16_t flags = OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED;
    int16_t e = entities.first_entity;
    while (e >= 0) {
        entities.tick_callback_hi[e] |= flags;
        e = entities.next_entity[e];
    }
}

/* ---- ENABLE_ALL_ENTITIES (port of C09451) ----
 *
 * Port of asm/overworld/enable_all_entities.asm.
 * Walk entity linked list and clear OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED
 * on every active entity. Used after screen transitions. */
void enable_all_entities(void) {
    uint16_t mask = (uint16_t)~(uint16_t)(OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
    int16_t e = entities.first_entity;
    while (e >= 0) {
        entities.tick_callback_hi[e] &= mask;
        e = entities.next_entity[e];
    }
}

/* ---- SET_TELEPORT_BOX_DESTINATION (port of asm/misc/set_teleport_box_destination.asm) ----
 *
 * Saves destination_id to game_state.unknownC3 and copies
 * the leader's current position to the respawn coordinates. */
void set_teleport_box_destination(uint8_t destination_id) {
    game_state.unknownC3 = destination_id;
    ow.respawn_x = game_state.leader_x_coord;
    ow.respawn_y = game_state.leader_y_coord;
}

/* ---- TELEPORT_FREEZEOBJECTS (port of asm/misc/teleport_freezeobjects.asm) ----
 *
 * Iterates entity slots 0-22 and unconditionally sets OBJECT_TICK_DISABLED |
 * OBJECT_MOVE_DISABLED on each.  Unlike disable_all_entities() which walks the
 * active linked list, this operates on raw slot indices. */
void teleport_freeze_entities(void) {
    uint16_t flags = OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED;
    for (int i = 0; i < INIT_ENTITY_SLOT; i++) {
        entities.tick_callback_hi[ENT(i)] |= flags;
    }
}

/* ---- TELEPORT_FREEZEOBJECTS2 (port of asm/misc/teleport_freezeobjects2.asm) ----
 *
 * Same as teleport_freeze_entities but only freezes entities that aren't
 * already frozen (both flags already set → skip). */
void teleport_freeze_entities_conditional(void) {
    uint16_t flags = OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED;
    for (int i = 0; i < INIT_ENTITY_SLOT; i++) {
        uint16_t val = entities.tick_callback_hi[ENT(i)];
        if ((val & flags) != flags) {
            entities.tick_callback_hi[ENT(i)] = val | flags;
        }
    }
}

/* ---- SET_ENTITY_DIRECTION_BY_SPRITE_ID (port of C46331) ----
 *
 * Port of asm/overworld/entity/set_entity_direction_by_sprite_id.asm.
 * Finds entity by sprite ID and sets its direction. Re-renders the sprite
 * if the direction actually changed. */
void set_entity_direction_by_sprite_id(uint16_t sprite_id, int16_t direction) {
    int16_t slot = find_entity_by_sprite_id(sprite_id);
    if (slot < 0) return;
    if (entities.directions[slot] != direction) {
        entities.directions[slot] = direction;
        render_entity_sprite(slot);
    }
}

/* ---- REMOVE_ENTITY (port of C02140) ----
 *
 * Port of asm/overworld/remove_entity.asm.
 * Cleans up an entity: frees spritemap ert.buffer entries and VRAM allocation,
 * clears sprite/NPC IDs, and deactivates the entity. */
void remove_entity(int16_t slot) {
    /* CLEAR_SPRITEMAP_SLOTS: mark spritemap ert.buffer entries as free (0xFF).
     * Two direction chunks, each spritemap_sizes bytes. */
    uint16_t smap_ptr = entities.spritemap_ptr_lo[slot];
    uint16_t smap_size = entities.spritemap_sizes[slot];
    uint16_t total = smap_size * 2;
    if (total > 0 && smap_ptr + total <= OVERWORLD_SPRITEMAPS_SIZE) {
        memset(overworld_spritemaps + smap_ptr, 0xFF, total);
    }

    /* ALLOC_SPRITE_MEM(slot, 0): free VRAM allocation */
    alloc_sprite_mem((uint16_t)slot, 0);

    /* Assembly lines 17-22: if NPC ID upper bits == 0x8000, it's an enemy —
     * decrement ow.overworld_enemy_count. */
    if ((entities.npc_ids[slot] & 0xF000) == 0x8000) {
        ow.overworld_enemy_count--;
    }

    /* Assembly lines 24-31: if enemy ID == MAGIC_BUTTERFLY (225),
     * clear the ow.magic_butterfly_spawned flag. */
    if (entities.enemy_ids[slot] == ENEMY_MAGIC_BUTTERFLY) {
        ow.magic_butterfly_spawned = 0;
    }

    /* Clear sprite and NPC IDs */
    entities.sprite_ids[slot] = -1;
    entities.npc_ids[slot] = -1;

    /* Deactivate the entity */
    deactivate_entity(slot);
}

/*
 * REMOVE_PARTY_MEMBER (asm/overworld/party/remove_party_member.asm)
 *
 * Removes a character from the party ordering arrays (party_order,
 * party_entity_slots, player_controlled_party_members) by finding their
 * position and shifting remaining members left.
 *
 * If the removed member was the leader (position 0), copies the removed
 * entity's position_index to the new leader's character.
 *
 * Then saves the entity's position as entity_prepared_*, removes the entity,
 * and calls update_npc_party_lineup + update_party to re-sort.
 */
void remove_party_member(uint16_t char_id) {
    /* Find char_id in party_order[] */
    uint16_t found = 0;
    while (found < 6) {
        if ((game_state.party_order[found] & 0xFF) == char_id)
            break;
        found++;
    }
    if (found >= 6) return;

    /* Save the entity slot for this member (16-bit value from byte array) */
    uint16_t entity_slot =
        read_u16_le(&game_state.party_entity_slots[found * 2]);

    /* Shift party_order, party_entity_slots, and player_controlled_party_members
     * left to fill the gap (positions found..4, copying [i+1] to [i]). */
    for (uint16_t i = found; i < 5; i++) {
        game_state.party_order[i] = game_state.party_order[i + 1];

        game_state.party_entity_slots[i * 2] = game_state.party_entity_slots[(i + 1) * 2];
        game_state.party_entity_slots[i * 2 + 1] = game_state.party_entity_slots[(i + 1) * 2 + 1];

        game_state.player_controlled_party_members[i] =
            game_state.player_controlled_party_members[i + 1];
    }

    /* If the removed member was the leader (position 0), copy position_index
     * from the removed entity's character to the new leader's character.
     * Assembly: ENTITY_SCRIPT_VAR1_TABLE → entities.var[1] = char_idx. */
    if (found == 0) {
        uint8_t new_leader_char_id = game_state.player_controlled_party_members[0];
        uint16_t removed_char_id = (uint16_t)entities.var[1][entity_slot];
        party_characters[new_leader_char_id].position_index =
            party_characters[removed_char_id].position_index;
    }

    /* Clear the last party_order slot and decrement count */
    game_state.party_order[5] = 0;
    game_state.party_count--;

    /* Save removed entity's position for potential reuse */
    ow.entity_prepared_x = entities.abs_x[entity_slot];
    ow.entity_prepared_y = entities.abs_y[entity_slot];
    ow.entity_prepared_direction = entities.directions[entity_slot];

    /* Remove the entity and re-sort party */
    remove_entity((int16_t)entity_slot);
    update_npc_party_lineup();
    update_party();
}

/* ---- CREATE_MINI_GHOST_ENTITY (port of C07716) ----
 *
 * Port of asm/overworld/entity/create_mini_ghost_entity.asm.
 * Creates a mini ghost entity when a party member is possessed.
 * Guards: leader must not be tick/move-disabled, not hidden, camera_mode != 2.
 * Places the ghost off-screen at (0xFF00, 0xFF00) with animation_frame = -1. */
void create_mini_ghost_entity(void) {
    /* Assembly lines 8-15: Guard checks on leader entity */
    uint16_t leader_offset = game_state.current_party_members;
    if (entities.tick_callback_hi[leader_offset] &
        (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED))
        return;
    if (entities.spritemap_ptr_hi[leader_offset] & 0x8000)
        return;
    if (game_state.camera_mode == 2)
        return;

    /* Assembly lines 22-28: Create entity with sprite MINI_GHOST, EVENT_MINI_GHOST */
    int16_t slot = create_entity(OVERWORLD_SPRITE_MINI_GHOST,
                                  EVENT_SCRIPT_MINI_GHOST, -1, 0, 0);
    if (slot < 0)
        return;

    ow.mini_ghost_entity_id = slot;

    /* Assembly lines 30-46: Initialize entity off-screen */
    entities.animation_frame[slot] = -1;
    entities.screen_y[slot] = (int16_t)0xFF00;
    entities.abs_y[slot] = (int16_t)0xFF00;
    entities.abs_x[slot] = (int16_t)0xFF00;
}

/* ---- DESTROY_MINI_GHOST_ENTITY (port of C0777A) ----
 *
 * Port of asm/overworld/entity/destroy_mini_ghost_entity.asm.
 * Removes the mini ghost entity and resets the tracking ID. */
void destroy_mini_ghost_entity(void) {
    remove_entity(ow.mini_ghost_entity_id);
    ow.mini_ghost_entity_id = -1;
}

/* ---- LOAD_DAD_PHONE (port of load_dad_phone.asm) ----
 *
 * Port of asm/overworld/load_dad_phone.asm.
 * Checks conditions for the "Your Dad called" phone interaction.
 * Only triggers when: no windows open, not in battle, no swirl,
 * no enemy touched, not already queued, and flag not disabled. */
void load_dad_phone(void) {
    /* Assembly lines 7-17: Guard checks */
    if (any_window_open())
        return;
    if (ow.battle_mode)
        return;
    if (ow.battle_swirl_countdown)
        return;
    if (ow.enemy_has_been_touched)
        return;
    if (ow.dad_phone_queued)
        return;

    /* Assembly lines 18-21: Check event flag */
    if (event_flag_get(EVENT_FLAG_DIS_2H_PAPA))
        return;

    /* Assembly lines 22-26: Queue dad phone interaction */
    queue_interaction(10, MSG_SYS_PHONE_DAD);
    ow.dad_phone_queued = 1;
}

/* ---- GET_ON_BICYCLE (port of C03C5E) ----
 *
 * Port of asm/overworld/get_on_bicycle.asm.
 * Mounts the bicycle: removes current leader sprite, creates bicycle sprite,
 * sets walking_style to BICYCLE, plays bicycle music. Only works when the
 * party has exactly 1 member in single-character mode (party_order[0] == 1). */
void get_on_bicycle(void) {
    /* Check party_count == 1 */
    if ((game_state.party_count & 0xFF) != 1)
        return;

    /* Check single-character mode (party_order[0] == 1) */
    if ((game_state.party_order[0] & 0xFF) != 1)
        return;

    /* Play bicycle music unless suppressed.
     * Assembly: LDA DISABLE_MUSIC_CHANGES / BNE @UNKNOWN2
     * During INIT_INTRO, DISABLE_MUSIC_CHANGES is set to 1 for the entire
     * intro sequence (logos, title screen, attract mode). This prevents
     * the Burglin Park attract scene from overriding the attract mode music
     * with bicycle music. During normal gameplay, DISABLE_MUSIC_CHANGES is 0
     * so the music changes normally. */
    if (!ow.disable_music_changes)
        change_music(MUSIC_BICYCLE);

    /* Remove current leader entity (slot 24) */
    remove_entity(PARTY_LEADER_ENTITY_INDEX);

    /* Set bicycle state */
    game_state.character_mode = CHARACTER_MODE_BICYCLE;
    game_state.walking_style = WALKING_STYLE_BICYCLE;
    party_characters[0].position_index = 0;
    game_state.position_buffer_index = 0;

    /* Create bicycle entity at leader's current position */
    ert.new_entity_var[0] = 0;
    ert.new_entity_var[1] = 0;
    int16_t x = entities.abs_x[ENT(PARTY_LEADER_ENTITY_INDEX)];
    int16_t y = entities.abs_y[ENT(PARTY_LEADER_ENTITY_INDEX)];
    create_entity(OVERWORLD_SPRITE_NESS_BICYCLE, EVENT_SCRIPT_002,
                  PARTY_LEADER_ENTITY_INDEX, x, y);
    /* Disable tick callback for bicycle entity */
    entities.tick_callback_hi[ENT(PARTY_LEADER_ENTITY_INDEX)] |= OBJECT_TICK_DISABLED;

    /* Set var7 flags (UNKNOWN12 | UNKNOWN13) */
    entities.var[7][ENT(PARTY_LEADER_ENTITY_INDEX)] |=
        (int16_t)(SPRITE_TABLE_10_SPACING_HIDE | SPRITE_TABLE_10_FORCE_STATIC_ANIM);

    /* Reset animation and set direction */
    entities.animation_frame[ENT(PARTY_LEADER_ENTITY_INDEX)] = 0;
    entities.directions[ENT(PARTY_LEADER_ENTITY_INDEX)] =
        (int16_t)game_state.leader_direction;

    ow.enable_auto_sector_music_changes = 0;

    /* Mark that player has moved */
    game_state.leader_moved = 1;

    ow.input_disable_frame_counter = 2;
}

/* ---- DISMOUNT_BICYCLE (port of C03CFD) ----
 *
 * Port of asm/overworld/dismount_bicycle.asm.
 * Gets off the bicycle: removes bicycle sprite, creates normal Ness sprite,
 * clears walking_style. Only acts if currently on bicycle. */
void dismount_bicycle(void) {
    /* Check if currently on bicycle */
    if (game_state.walking_style != WALKING_STYLE_BICYCLE)
        return;

    ow.enable_auto_sector_music_changes = 1;
    if (!ow.battle_mode && !ow.pending_interactions) {
        update_map_music_at_leader();
    }

    /* Remove bicycle entity (slot 24) */
    remove_entity(PARTY_LEADER_ENTITY_INDEX);

    /* Clear bicycle state */
    game_state.character_mode = CHARACTER_MODE_NORMAL;
    game_state.walking_style = 0;
    party_characters[0].position_index = 0;
    game_state.position_buffer_index = 0;

    /* Assembly lines 25-30: render a frame before swapping sprites */
    if (!ow.pending_interactions) {
        oam_clear();
        run_actionscript_frame();
        update_screen();
        wait_for_vblank();
    }

    /* Create normal Ness entity at leader's current position */
    ert.new_entity_var[0] = 0;
    ert.new_entity_var[1] = 0;
    int16_t x = entities.abs_x[ENT(PARTY_LEADER_ENTITY_INDEX)];
    int16_t y = entities.abs_y[ENT(PARTY_LEADER_ENTITY_INDEX)];
    create_entity(OVERWORLD_SPRITE_NESS, EVENT_SCRIPT_002,
                  PARTY_LEADER_ENTITY_INDEX, x, y);

    /* Reset animation and set direction */
    entities.animation_frame[ENT(PARTY_LEADER_ENTITY_INDEX)] = 0;
    entities.directions[ENT(PARTY_LEADER_ENTITY_INDEX)] =
        (int16_t)game_state.leader_direction;

    /* Set var7 flags (SPACING_HIDE | WALKING_STYLE_CHANGED) */
    entities.var[7][ENT(PARTY_LEADER_ENTITY_INDEX)] |=
        (int16_t)(SPRITE_TABLE_10_SPACING_HIDE | SPRITE_TABLE_10_WALKING_STYLE_CHANGED);

    /* Assembly lines 49-60: if interactions pending, disable entity and wait */
    if (ow.pending_interactions) {
        entities.tick_callback_hi[ENT(PARTY_LEADER_ENTITY_INDEX)] |=
            (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);
        wait_for_vblank();
        wait_for_vblank();
        update_entity_sprite(PARTY_LEADER_ENTITY_INDEX);
    }

    ow.input_disable_frame_counter = 2;
}

/* ---- UPDATE_OVERWORLD_FRAME (port of C05200) ----
 *
 * Per-frame overworld update — tick callback for the init entity (slot 23).
 * Set by EVENT_001 (main overworld tick) via EVENT_SET_TICK_CALLBACK.
 *
 * Assembly sequence:
 *   1. Check BATTLE_MODE → early exit
 *   2. Mini ghost entity creation/destruction (POSSESSED_PLAYER_COUNT)
 *   3. ANIMATE_TILESET (if loaded)
 *   4. ANIMATE_PALETTE (if loaded)
 *   5. PROCESS_ITEM_TRANSFORMATIONS (if loaded)
 *   6. UPDATE_LEADER_MOVEMENT ← critical
 *   7. Sector music change detection (leader position vs last sector)
 *   8. Dad phone timer check
 *   9. Clear POSSESSED_PLAYER_COUNT
 *  10. Set CURRENT_LEADER_DIRECTION, CURRENT_LEADING_PARTY_MEMBER_ENTITY
 *  11. If unknown90 (leader_moved), set PLAYER_HAS_DONE_SOMETHING_THIS_FRAME
 */
void update_overworld_frame(int16_t entity_offset) {
    /* 1. Early exit if in battle mode (assembly line 7-8) */
    if (ow.battle_mode) return;

    /* 2. Mini ghost entity management (assembly lines 9-20) */
    if (ow.possessed_player_count) {
        /* Ghost needed — create if doesn't exist yet */
        if (ow.mini_ghost_entity_id == -1)
            create_mini_ghost_entity();
    } else {
        /* No ghost needed — destroy if exists */
        if (ow.mini_ghost_entity_id != -1)
            destroy_mini_ghost_entity();
    }

    /* 3. Animated tiles (assembly lines 22-24) */
    if (ml.loaded_animated_tile_count)
        animate_tileset();

    /* 4. Palette animation (assembly lines 26-28) */
    if (ml.map_palette_animation_loaded)
        animate_palette();

    /* 5. Item transformations (assembly lines 30-32)
     * Assembly guards with: LDA ITEM_TRANSFORMATIONS_LOADED; BEQ skip. */
    if (item_transformations_loaded)
        process_item_transformations();

    /* 6. UPDATE_LEADER_MOVEMENT — the critical call (assembly line 34) */
    update_leader_movement(entity_offset);

    /* 7. Sector music change detection (assembly lines 35-55).
     * Checks if leader has moved to a new sector (high byte of x/y coords)
     * and fades to the new sector's music if ENABLE_AUTO_SECTOR_MUSIC_CHANGES. */
    {
        uint16_t sector_x = (game_state.leader_x_coord >> 8) & 0xFF;
        uint16_t sector_y = (game_state.leader_y_coord >> 8) & 0xFF;
        if (sector_x != ow.last_sector_x || sector_y != ow.last_sector_y) {
            ow.last_sector_x = sector_x;
            ow.last_sector_y = sector_y;
            if (ow.enable_auto_sector_music_changes) {
                /* FADE_TO_MAP_MUSIC_AT_LEADER (port of C03C25):
                 * Set fade flag, resolve music, wait a frame, apply. */
                ml.do_map_music_fade = 1;
                resolve_map_sector_music(game_state.leader_x_coord,
                                         game_state.leader_y_coord);
                if (ml.next_map_music_track != ml.current_map_music_track) {
                    wait_for_vblank();
                    apply_next_map_music();
                }
                ml.do_map_music_fade = 0;
            }
        }
    }

    /* 8. Dad phone timer (assembly lines 57-63) */
    if (!ow.dad_phone_timer && game_state.camera_mode != 2)
        load_dad_phone();

    /* 9. Clear possessed player count (assembly line 64) */
    ow.possessed_player_count = 0;

    /* 10. Set current leader direction and entity (assembly lines 65-69) */
    ow.current_leader_direction = game_state.leader_direction;
    ow.current_leading_party_member_entity = game_state.current_party_members;

    /* 11. If leader has done something, signal it (assembly lines 70-73) */
    if (game_state.leader_moved)
        ow.player_has_done_something_this_frame = 1;
}

/* ---- RESET_QUEUED_INTERACTIONS (port of C064D4) ----
 *
 * Port of asm/overworld/reset_queued_interactions.asm.
 * Resets the interaction queue to empty and clears the current type. */
void reset_queued_interactions(void) {
    ow.next_queued_interaction = 0;
    ow.current_queued_interaction = 0;
    ow.current_queued_interaction_type = 0xFFFF;
}

/* ---- QUEUE_INTERACTION (port of C064E3) ----
 *
 * Port of asm/overworld/queue_interaction.asm.
 * Enqueues an interaction into the circular queue. Skips if the type
 * matches the currently-processing interaction (duplicate prevention). */
void queue_interaction(uint16_t type, uint32_t data_ptr) {
    /* Don't queue if same type is currently being processed */
    if (type == ow.current_queued_interaction_type) return;

    uint16_t idx = ow.next_queued_interaction;
    ow.queued_interactions[idx].type = type;
    ow.queued_interactions[idx].data_ptr = data_ptr;

    ow.next_queued_interaction = (idx + 1) & (INTERACTION_QUEUE_SIZE - 1);
    ow.pending_interactions = 1;
}

/* FREEZE_AND_QUEUE_TEXT_INTERACTION (C46881):
 * Disable all character movement and queue a type-8 text interaction. */
void freeze_and_queue_text_interaction(uint32_t text_ptr) {
    disable_character_movement(0xFF);  /* 0xFF = freeze all (assembly: LDA #<-1) */
    queue_interaction(8, text_ptr);
}

/* ---- Entity creation queue (port of C06578 / C065A3) ---- */

/* QUEUE_ENTITY_CREATION (C06578): append sprite+script to creation queue. */
void queue_entity_creation(uint16_t sprite_id, uint16_t script_id) {
    uint16_t idx = ow.entity_creation_queue_length;
    if (idx >= ENTITY_CREATION_QUEUE_CAPACITY)
        FATAL("entity creation queue overflow (len=%u)\n", idx);
    ow.entity_creation_queue[idx].sprite = sprite_id;
    ow.entity_creation_queue[idx].script = script_id;
    ow.entity_creation_queue_length++;
}

/* FLUSH_ENTITY_CREATION_QUEUE (C065A3): pop all entries (LIFO) and create entities. */
void flush_entity_creation_queue(void) {
    while (ow.entity_creation_queue_length != 0) {
        ow.entity_creation_queue_length--;
        uint16_t idx = ow.entity_creation_queue_length;
        uint16_t sprite_id = ow.entity_creation_queue[idx].sprite;
        uint16_t script_id = ow.entity_creation_queue[idx].script;
        /* CREATE_PREPARED_ENTITY_SPRITE: create at ow.entity_prepared_x/y with auto slot */
        int16_t result = create_entity(sprite_id, script_id, -1,
                                       ow.entity_prepared_x, ow.entity_prepared_y);
        if (result >= 0) {
            entities.directions[result] = ow.entity_prepared_direction;
        }
    }
}

/* ---- OAM_CLEAR (port of asm/system/oam.asm) ---- */
void oam_clear(void) {
    /* Clear priority queue offsets (assembly lines 9-12 of oam_clear.asm) */
    for (int i = 0; i < 4; i++)
        sprite_priority[i].offset = 0;
    ert.oam_write_index = 0;

    /* Park sprites off-screen. VIEWPORT_HEIGHT (240) is used instead of
     * SNES_HEIGHT (224) so sprites are hidden in both 224px and 240px modes. */
    for (int i = 0; i < 128; i++) {
        ppu.oam[i].y = VIEWPORT_HEIGHT;
        ppu.oam_full_y[i] = VIEWPORT_HEIGHT;
    }
}

/* ---- UPDATE_ENTITY_SCREEN_POSITIONS (port of C426C7) ----
 *
 * Converts all active entities' absolute world coordinates to screen coordinates
 * by subtracting BG1 scroll position. Entities with script_table < 0 (dormant,
 * high bit set = BMI in assembly) are skipped.
 *
 * Called by SCREEN_TRANSITION to update screen positions after scroll changes. */
void update_entity_screen_positions(void) {
    for (int offset = 0; offset < MAX_ENTITIES; offset++) {
        if (entities.script_table[offset] < 0)
            continue;
        entities.screen_x[offset] = entities.abs_x[offset] - (int16_t)ppu.bg_hofs[0];
        entities.screen_y[offset] = entities.abs_y[offset] - (int16_t)ppu.bg_vofs[0];
    }
}

/* ---- UPDATE_SCREEN (port of C08B19) ----
 *
 * Flushes queued priority sprites to OAM and syncs palettes.
 * In assembly: RENDER_ALL_PRIORITY_SPRITES + palette sync + triple-buffer swap.
 * Triple-buffering is not needed in the C port (we render directly).
 *
 * Callers must call oam_clear() and build_entity_draw_list() separately
 * before this — matching the assembly's three-step pipeline:
 *   1. OAM_CLEAR (clears OAM + priority queues)
 *   2. BUILD_ENTITY_DRAW_LIST (sorts entities, queues sprites)
 *   3. UPDATE_SCREEN (flushes queues to OAM, syncs palettes) */
void update_screen(void) {
    render_all_priority_sprites();
    sync_palettes_to_cgram();
}

/* ---- WAIT_FRAMES_WITH_UPDATES (port of C0DD2C) ----
 *
 * Runs the full render loop (OAM_CLEAR → RUN_ACTIONSCRIPT_FRAME →
 * UPDATE_SCREEN → WAIT_UNTIL_NEXT_FRAME) for the specified number of frames.
 * Used by SCREEN_TRANSITION and TELEPORT_MAINLOOP for animation delays. */
void wait_frames_with_updates(uint16_t count) {
    while (count) {
        if (platform_input_quit_requested()) return;
        oam_clear();
        run_actionscript_frame();
        update_screen();
        wait_for_vblank();
        count--;
    }
}

/* ---- RUN_FRAMES_UNTIL_FADE_DONE (port of C0DD0F) ----
 *
 * Loops running the full render frame (OAM_CLEAR → RUN_ACTIONSCRIPT_FRAME →
 * UPDATE_SCREEN → WAIT_UNTIL_NEXT_FRAME) until the fade has completed
 * (fade_parameters::step == 0). Called after fade_in/fade_out to block
 * until the brightness transition is done. */
void run_frames_until_fade_done(void) {
    while (fade_active()) {
        if (platform_input_quit_requested()) return;
        oam_clear();
        run_actionscript_frame();
        update_screen();
        fade_update();
        wait_for_vblank();
    }
}

/* ---- RENDER_FRAME_TICK (port of asm/system/render_frame_tick.asm) ----
 *
 * Runs one complete render frame for the overworld path:
 *   OAM_CLEAR → RUN_ACTIONSCRIPT_FRAME → UPDATE_SCREEN → WAIT_UNTIL_NEXT_FRAME
 *
 * The assembly version also:
 *   - Checks RENDER_HPPP_WINDOWS flag → UPDATE_TEXT_WINDOW_PALETTE
 *   - Checks BATTLE_MODE_FLAG → WAIT_AND_UPDATE_BATTLE_EFFECTS_FAR */
void render_frame_tick(void) {
    if (ow.render_hppp_windows & 0xFF)
        update_text_window_palette();

    /* Assembly: if BATTLE_MODE_FLAG, JSL WAIT_AND_UPDATE_BATTLE_EFFECTS_FAR and return.
     * WAIT_AND_UPDATE_BATTLE_EFFECTS = WAIT_UNTIL_NEXT_FRAME + UPDATE_BATTLE_SCREEN_EFFECTS. */
    if (bt.battle_mode_flag) {
        wait_for_vblank();
        update_battle_screen_effects();
        return;
    }

    oam_clear();
    run_actionscript_frame();
    update_screen();
    wait_for_vblank();
}

/* ---- ALLOC_SPRITE_MEM (port of asm/system/alloc_sprite_mem.asm) ---- */
void alloc_sprite_mem(uint16_t id, uint16_t param) {
    /* When id == 0x8000, clear the entire sprite VRAM table.
     * Otherwise, mark/free specific entries based on param matching. */
    for (int x = 0; x < SPRITE_VRAM_TABLE_SIZE; x++) {
        uint8_t test_val = (id & 0xFF) | 0x80;
        uint8_t current = sprite_vram_table[x];

        if (current == test_val || id == 0x8000) {
            sprite_vram_table[x] = (uint8_t)param;
        }
    }
}

/* ---- CALCULATE_PROSPECTIVE_POSITION (asm/overworld/calculate_prospective_position.asm) ----
 * Adds delta velocity to absolute position to determine where an entity
 * will be next frame. Stores result in ow.entity_movement_prospective_x/y.
 * Returns count of changed axes (0=none, 1=one, 2=both) — assembly uses
 * INY (increment Y from 0), NOT bit flags. Callers only test zero vs nonzero.
 * Entry point CALCULATE_PROSPECTIVE_POSITION uses CURRENT_ENTITY_OFFSET;
 * Entry point CALCULATE_PROSPECTIVE_POSITION_ENTRY2 takes entity slot in A.
 * This C function implements the ENTRY2 path (takes entity_slot). */
int calculate_prospective_position(int16_t entity_slot) {
    int16_t offset = entity_slot;  /* slot == offset (identity after refactor) */
    int changed = 0;

    /* Add fractional + integer X with carry propagation.
     * Assembly: CLC; ADC frac; ADC int — carry from frac flows into int add. */
    uint32_t fx = (uint32_t)entities.frac_x[offset] +
                  (uint32_t)entities.delta_frac_x[offset];
    int16_t new_x = entities.abs_x[offset] + entities.delta_x[offset] +
                    (int16_t)(fx >> 16);  /* carry from fractional add */
    ow.entity_movement_prospective_x = new_x;
    if (new_x != entities.abs_x[offset])
        changed++;  /* Assembly uses INY (increment), not bit flags */

    /* Same for Y */
    uint32_t fy = (uint32_t)entities.frac_y[offset] +
                  (uint32_t)entities.delta_frac_y[offset];
    int16_t new_y = entities.abs_y[offset] + entities.delta_y[offset] +
                    (int16_t)(fy >> 16);
    ow.entity_movement_prospective_y = new_y;
    if (new_y != entities.abs_y[offset])
        changed++;  /* Assembly uses INY (increment), not bit flags */

    return changed;
}

/* ---- SNAP_PARTY_TO_LEADER (port of C039E5) ----
 *
 * Port of asm/overworld/party/snap_party_to_leader.asm.
 * Iterates through all 6 party slots. For each active party member,
 * copies the leader's absolute position to the entity and recomputes
 * screen coordinates. Called after map load to prevent followers from
 * being stranded at their old positions. */
void snap_party_to_leader(void) {
    int16_t leader_x = game_state.leader_x_coord;
    int16_t leader_y = game_state.leader_y_coord;

    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        /* Assembly: LDA party_order,Y; AND #$00FF; BEQ skip */
        if ((game_state.party_order[i] & 0xFF) == 0)
            continue;

        /* Assembly: TYA; ASL; TAX; LDA party_entity_slots,X
         * party_entity_slots is a uint8_t[12] storing 6 x 16-bit words. */
        uint16_t slot = read_u16_le(&game_state.party_entity_slots[i * 2]);

        /* Copy leader position to this entity */
        entities.abs_x[slot] = leader_x;
        entities.abs_y[slot] = leader_y;

        /* ENTITY_SCREEN_COORDS_BY_ID: screen = abs - BG scroll.
         * Port of asm/overworld/entity/entity_screen_coords_by_id.asm. */
        entities.screen_x[slot] = leader_x - (int16_t)ppu.bg_hofs[0];
        entities.screen_y[slot] = leader_y - (int16_t)ppu.bg_vofs[0];
    }
}

/* ---- REFRESH_PARTY_ENTITIES (port of asm/overworld/party/refresh_party_entities.asm) ----
 *
 * Iterates party entity slots (24 to MAX_ENTITIES-1), disables tick+movement,
 * updates sprite graphics, positions entities at leader coords or from the
 * position ert.buffer, computes screen coords, and re-renders sprites.
 *
 * Called after INIT_PARTY_POSITION_BUFFER (SET_LEADER_POSITION_AND_LOAD_PARTY),
 * after battle (UPDATE_OVERWORLD_DAMAGE), and from event scripts. */
void refresh_party_entities(void) {
    /* @LOCAL03: leader's position_index for comparison */
    uint16_t leader_pos_idx = party_characters[0].position_index;

    for (int slot = PARTY_LEADER_ENTITY_INDEX; slot < MAX_ENTITIES; slot++) {
        int16_t ent_off = slot;

        /* Skip empty entity slots */
        if (entities.script_table[ent_off] == ENTITY_NONE)
            continue;

        /* Disable tick and movement callbacks */
        entities.tick_callback_hi[ent_off] |=
            (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED);

        /* Get party member index from entity var1, look up char_struct */
        int16_t party_idx = entities.var[1][ent_off];
        int16_t char_id = entities.var[0][ent_off];

        /* Check if this entity is the leader or has same position_index */
        uint16_t current_party = game_state.current_party_members;
        uint16_t member_pos_idx = party_characters[party_idx].position_index;

        if (current_party == (uint16_t)slot || member_pos_idx == leader_pos_idx) {
            /* Use leader coords and walking_style */
            uint16_t ws = game_state.walking_style;
            update_party_entity_graphics(char_id, ws, ent_off, party_idx);

            entities.abs_x[ent_off] = game_state.leader_x_coord;
            entities.abs_y[ent_off] = game_state.leader_y_coord;

            /* Only set direction if party has more than 1 member */
            if ((game_state.party_count & 0xFF) != 1) {
                entities.directions[ent_off] = game_state.leader_direction;
            }
        } else {
            /* Read follower position from the circular buffer using the
             * follower's actual position_index (masked to 8 bits for the
             * 256-entry circular buffer). */
            PositionBufferEntry *entry = &pb.player_position_buffer[member_pos_idx & 0xFF];

            uint16_t ws = entry->walking_style;
            update_party_entity_graphics(char_id, ws, ent_off, party_idx);

            entities.abs_x[ent_off] = entry->x_coord;
            entities.abs_y[ent_off] = entry->y_coord;
            entities.directions[ent_off] = (int16_t)entry->direction;
        }

        /* Calculate screen coords: abs - BG1 scroll */
        entities.screen_x[ent_off] = entities.abs_x[ent_off] - (int16_t)ppu.bg_hofs[0];
        entities.screen_y[ent_off] = entities.abs_y[ent_off] - (int16_t)ppu.bg_vofs[0];

        /* Re-render entity sprite (assembly uses UPDATE_ENTITY_SPRITE = 8-dir) */
        update_entity_sprite(ent_off);
    }
}

/* ---- LOAD_PARTY_AT_MAP_POSITION (port of asm/overworld/party/load_party_at_map_position.asm) ----
 *
 * Rebuilds party member entities for the current map position.
 * Called during teleport and map transitions to:
 *  1. Load sector attributes at the destination coords
 *  2. Set character_mode, walking_style, footstep sound
 *  3. Remove old party entities and recreate them with correct sprites
 *  4. Snap party to leader and check collision for stair/door tiles
 *
 * direction: the direction the party should face after loading. */

/* ACTION_SCRIPT_ID lookup per char_id (from CHARACTER_INITIAL_ENTITY_DATA).
 * All characters use EVENT_002 (party follower) except Bubble Monkey (char_id 8) = EVENT_003.
 * These are structural interface constants, not ROM data. */
#define CHARACTER_INITIAL_ENTITY_COUNT 17
static const uint16_t character_actionscripts[CHARACTER_INITIAL_ENTITY_COUNT] = {
    EVENT_SCRIPT_002,  /* 0: Ness */
    EVENT_SCRIPT_002,  /* 1: Paula */
    EVENT_SCRIPT_002,  /* 2: Jeff */
    EVENT_SCRIPT_002,  /* 3: Poo */
    EVENT_SCRIPT_002,  /* 4: Pokey */
    EVENT_SCRIPT_002,  /* 5: Picky */
    EVENT_SCRIPT_002,  /* 6: King */
    EVENT_SCRIPT_002,  /* 7: Tony */
    EVENT_SCRIPT_003,  /* 8: Bubble Monkey */
    EVENT_SCRIPT_002,  /* 9: Dungeon Man */
    EVENT_SCRIPT_002,  /* 10: Flying Man 0 */
    EVENT_SCRIPT_002,  /* 11: Flying Man 1 */
    EVENT_SCRIPT_002,  /* 12: Flying Man 2 */
    EVENT_SCRIPT_002,  /* 13: Flying Man 3 */
    EVENT_SCRIPT_002,  /* 14: Flying Man 4 */
    EVENT_SCRIPT_002,  /* 15: Teddy Bear 0 */
    EVENT_SCRIPT_002,  /* 16: Teddy Bear 1 */
};

void load_party_at_map_position(uint16_t direction) {
    /* Assembly lines 26-41: determine coords for sector attr lookup.
     * If teleport destination is set, use those × 8; else use leader coords. */
    uint16_t attr_x, attr_y;
    if (ow.current_teleport_destination_x | ow.current_teleport_destination_y) {
        attr_x = ow.current_teleport_destination_x * 8;
        attr_y = ow.current_teleport_destination_y * 8;
    } else {
        attr_x = game_state.leader_x_coord;
        attr_y = game_state.leader_y_coord;
    }

    /* Assembly lines 42-49: load sector attrs and set character mode.
     * Low 3 bits = character_mode. Footstep sound = mode * 2 (ASL). */
    uint16_t attrs = load_sector_attrs(attr_x, attr_y);
    uint16_t char_mode = attrs & 0x0007;
    game_state.character_mode = char_mode;
    ow.footstep_sound_id = char_mode * 2;
    ow.footstep_sound_id_override = 0;

    /* Assembly lines 50-57: walking_style depends on character_mode.
     * Mode 3 (Lost Underworld / small party) → SLOWEST; else → 0. */
    if (char_mode == CHARACTER_MODE_SMALL) {
        game_state.walking_style = WALKING_STYLE_SLOWEST;
    } else {
        game_state.walking_style = 0;
    }

    /* Assembly lines 59-62: save and clear CURRENT_ENTITY_SLOT */
    int16_t saved_entity_slot = ert.current_entity_slot;
    ert.current_entity_slot = -1;

    /* Assembly lines 63-191: loop through 6 party positions */
    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        /* Get char from party_order (assembly lines 65-78) */
        uint8_t party_char = game_state.party_order[i] & 0xFF;
        if (party_char == 0)
            continue;

        int16_t char_id = (int16_t)(party_char - 1);  /* @VIRTUAL04 */

        /* Get entity slot from party_entity_slots (assembly lines 82-96) */
        uint16_t slot = read_u16_le(&game_state.party_entity_slots[i * 2]);
        int16_t ent_off = (int16_t)slot;

        /* Save entity state before removal (assembly lines 97-110) */
        ert.new_entity_var[0] = entities.var[0][ent_off];
        ert.new_entity_var[1] = entities.var[1][ent_off];
        ert.new_entity_var[5] = (int16_t)(i * 2);
        /* Clear remaining vars (assembly doesn't set them explicitly;
         * CREATE_ENTITY copies all 8 from ert.new_entity_var) */
        ert.new_entity_var[2] = 0;
        ert.new_entity_var[3] = 0;
        ert.new_entity_var[4] = 0;
        ert.new_entity_var[6] = 0;
        ert.new_entity_var[7] = 0;

        int16_t saved_spritemap_ptr_hi = entities.spritemap_ptr_hi[ent_off];
        int16_t saved_tick_callback_hi = entities.tick_callback_hi[ent_off];

        /* Remove old entity (assembly line 112) */
        remove_entity((int16_t)slot);

        /* Set MOVING_PARTY_MEMBER_ENTITY_ID (assembly line 114) */
        ow.moving_party_member_entity_id = (int16_t)slot;

        /* Determine sprite ID via GET_PARTY_MEMBER_SPRITE_ID.
         * Normal mode: walking_style=0. Small party (mode 3): walking_style=10.
         * (Assembly lines 115-156) */
        uint16_t ws = (char_mode == 3) ? WALKING_STYLE_SLOWEST : 0;
        int16_t sprite_id = get_party_member_sprite_id(
            char_id, ws, ent_off, (int16_t)i);

        /* Look up actionscript_id from table (assembly lines 134-141/163-169) */
        uint16_t script_id = EVENT_SCRIPT_002;
        if (char_id >= 0 && char_id < CHARACTER_INITIAL_ENTITY_COUNT) {
            script_id = character_actionscripts[char_id];
        }

        /* Create new entity at leader position (assembly lines 128-144/157-173) */
        int16_t new_slot = create_entity(
            sprite_id, script_id, (int16_t)slot,
            game_state.leader_x_coord, game_state.leader_y_coord);
        if (new_slot < 0)
            continue;

        /* Restore spritemap_ptr_hi and tick_callback_hi (assembly lines 174-180).
         * These preserve the entity's visual state across the remove/create. */
        int16_t new_ent_off = new_slot;
        entities.spritemap_ptr_hi[new_ent_off] = saved_spritemap_ptr_hi;
        entities.tick_callback_hi[new_ent_off] = saved_tick_callback_hi;

        /* Set direction and clear animation frame (assembly lines 181-183) */
        entities.directions[new_ent_off] = (int16_t)direction;
        entities.animation_frame[new_ent_off] = 0;

        /* Re-render entity sprite (assembly lines 184-185: UPDATE_ENTITY_SPRITE = 8-dir) */
        update_entity_sprite(new_ent_off);
    }

    /* Assembly lines 192-193: restore ert.current_entity_slot */
    ert.current_entity_slot = saved_entity_slot;

    /* Assembly line 194: snap all party entities to leader */
    snap_party_to_leader();

    /* Assembly lines 195-207: check collision at leader position for doors.
     * Save and clear ow.pending_interactions to avoid side effects. */
    ow.ladder_stairs_tile_x = -1;
    uint16_t saved_pending = ow.pending_interactions;
    ow.pending_interactions = 0;

    /* Assembly line 199: LDA #4 → STA @LOCAL00 sets direction=4 (DOWN) via
     * VUCC stack parameter. Y=current_party_members is the 3rd register
     * param (entity), unused by check_directional_collision. */
    check_directional_collision(
        (int16_t)game_state.leader_x_coord,
        (int16_t)game_state.leader_y_coord,
        4);  /* direction = DOWN */

    ow.pending_interactions = saved_pending;

    /* Assembly lines 208-213: if a door/stairway was found, process it */
    if (ow.ladder_stairs_tile_x != -1) {
        process_door_at_tile((uint16_t)ow.ladder_stairs_tile_x,
                             (uint16_t)ow.ladder_stairs_tile_y);
    }
}

/* ---- SET_LEADER_POSITION_AND_LOAD_PARTY ----
 * Port of asm/overworld/party/set_leader_position_and_load_party.asm (45 lines).
 *
 * Sets leader position/direction, calls LOAD_PARTY_AT_MAP_POSITION to
 * rebuild party entities for the new area, inits position ert.buffer,
 * clears animation fingerprints, resets mini ghost and teleport destination,
 * updates pajama flag, and refreshes party entity graphics. */
void set_leader_position_and_load_party(uint16_t x, uint16_t y,
                                         uint16_t direction) {
    /* Assembly lines 11-15: set leader position and direction */
    game_state.leader_x_coord = x;
    game_state.leader_y_coord = y;
    game_state.leader_direction = direction;

    /* Assembly lines 16-19: LOOKUP_SURFACE_FLAGS → trodden_tile_type */
    game_state.trodden_tile_type =
        lookup_surface_flags((int16_t)x, (int16_t)y,
                             entities.sizes[ENT(game_state.current_party_members)]);

    /* Assembly line 20-21: JSL LOAD_PARTY_AT_MAP_POSITION */
    load_party_at_map_position(direction);

    /* Assembly line 22: JSL INIT_PARTY_POSITION_BUFFER */
    init_party_position_buffer();

    /* Assembly lines 23-36: clear animation fingerprints for party entities */
    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        entities.animation_fingerprints[PARTY_LEADER_ENTITY_INDEX + i] =
            (uint16_t)-1;
    }

    /* Assembly lines 37-38: reset mini ghost */
    ow.mini_ghost_entity_id = -1;

    /* Assembly lines 39-40: clear teleport destination override */
    ow.current_teleport_destination_y = 0;
    ow.current_teleport_destination_x = 0;

    /* Assembly lines 41-43: GET_EVENT_FLAG(NESS_PAJAMA_FLAG) → PAJAMA_FLAG */
    ow.pajama_flag = event_flag_get(EVENT_FLAG_NESS_PAJAMA) ? 1 : 0;

    /* Assembly line 44: JSL REFRESH_PARTY_ENTITIES */
    refresh_party_entities();
}

/* ---- INITIALIZE_OVERWORLD_STATE (port of C0B67F) ----
 *
 * Port of asm/overworld/initialize_overworld_state.asm.
 * Called after file select to set up the overworld for gameplay.
 * Creates the init entity (slot 23) with EVENT_001 (main overworld tick), initializes party,
 * loads the map at the leader's saved position, and prepares all
 * overworld subsystems. */
void initialize_overworld_state(void) {
    /* Load the dialogue blob and inline string table. */
    display_text_init();

    /* Assembly line 11: JSL INIT_ENTITY_SYSTEM */
    entity_system_init();

    /* Assembly line 12: JSL CLEAR_OVERWORLD_SPRITEMAPS */
    clear_overworld_spritemaps();

    /* Assembly lines 13-15: ALLOC_SPRITE_MEM(0, $8000) — clear entire sprite VRAM table.
     * Assembly loads X=0, A=$8000. ALLOC_SPRITE_MEM(id=A, param=X). */
    alloc_sprite_mem(0x8000, 0);

    /* Assembly line 16: JSL INITIALIZE_MISC_OBJECT_DATA */
    initialize_misc_object_data();

    /* Assembly lines 17-26: set BSS globals */
    ow.battle_mode = 0;
    ow.input_disable_frame_counter = 0;
    ow.npc_spawns_enabled = 1;
    ow.enemy_spawns_enabled = 0xFF;  /* -1 (all enabled) */
    ow.overworld_enemy_count = 0;
    ow.overworld_enemy_maximum = 10;
    ow.magic_butterfly_spawned = 0;
    ow.enemy_spawn_counter = 0;
    ow.enemy_spawn_too_many_failures = 0;
    ow.battle_swirl_countdown = 0;
    ow.pending_interactions = 0;

    /* Assembly line 27-28: SET_AUTO_SECTOR_MUSIC_CHANGES(1) */
    ow.enable_auto_sector_music_changes = 1;

    /* Assembly line 29-30: DAD_PHONE_TIMER = 1687 */
    ow.dad_phone_timer = 1687;

    /* Assembly line 31-32: SET_IRQ_CALLBACK(PROCESS_OVERWORLD_TASKS) */
    frame_callback = process_overworld_tasks;

    /* Assembly lines 33-36: clear teleport state and entity fade */
    ow.psi_teleport_style = 0;
    ow.psi_teleport_destination = 0;
    ow.entity_fade_entity = -1;

    /* Assembly lines 37-44: create init entity at slot 23 with EVENT_001
     * (main overworld tick). Set allocation range to [23, 24) so INIT_ENTITY
     * allocates slot 23. The init entity has no sprite — it only runs EVENT_001
     * which sets UPDATE_OVERWORLD_FRAME as the tick callback. */
    entities.alloc_min_slot = INIT_ENTITY_SLOT;
    entities.alloc_max_slot = PARTY_LEADER_ENTITY_INDEX;
    memset(ert.new_entity_var, 0, sizeof(ert.new_entity_var));
    ert.new_entity_pos_z = 0;
    ert.new_entity_priority = 0;
    entity_init(EVENT_SCRIPT_001, 0, 0);

    /* Assembly line 45: JSL RESET_PARTY_STATE */
    reset_party_state();

    /* Assembly line 46: JSL INITIALIZE_PARTY */
    initialize_party();

    /* Assembly lines 47-52: clear all 256 palette colors to 0.
     * MEMSET16(PALETTES, 0, BPP4PALETTE_SIZE * 16).
     * BPP4PALETTE_SIZE = 32 bytes, * 16 ert.palettes = 512 bytes = 256 words. */
    memset(ert.palettes, 0, sizeof(ert.palettes));

    /* Assembly line 53: JSL LOAD_CHARACTER_WINDOW_PALETTE */
    load_character_window_palette();

    /* Assembly line 54: JSL OVERWORLD_INITIALIZE */
    overworld_initialize();

    /* C port fix: clear win.bg2_buffer to match the freshly-cleared VRAM.
     * In the assembly, file select uses direct VRAM DMA (not win.bg2_buffer),
     * so win.bg2_buffer stays zeroed (BSS). The C port routes all text
     * through win.bg2_buffer, so stale file select tilemap data remains.
     * Without this, the first upload_battle_screen_to_vram() call writes
     * stale file select entries into the cleared BG3 tilemap VRAM. */
    memset(win.bg2_buffer, 0, BG2_BUFFER_SIZE);

    /* Assembly lines 55-57: LOAD_MAP_AT_POSITION(leader_x, leader_y) */
    load_map_at_position(game_state.leader_x_coord, game_state.leader_y_coord);

    /* Assembly line 58: JSL SPAWN_BUZZ_BUZZ */
    spawn_buzz_buzz();

    /* Assembly line 59: JSL LOAD_WINDOW_GFX */
    text_load_window_gfx();

    /* Assembly lines 60-65: UPLOAD_TEXT_TILES_TO_VRAM(1)
     * In the C port, text_load_window_gfx() already handles the mode 1
     * upload path (combined LOAD_WINDOW_GFX + UPLOAD_TEXT_TILES_TO_VRAM). */

    /* Assembly line 66: JSL SNAP_PARTY_TO_LEADER */
    snap_party_to_leader();
}

/* ====================================================================
 * Overworld Task Scheduler
 *
 * Ports of:
 *   SCHEDULE_OVERWORLD_TASK    (asm/overworld/schedule_overworld_task.asm)
 *   PROCESS_OVERWORLD_TASKS   (asm/overworld/process_overworld_tasks.asm)
 *
 * 4 task slots.  Each has a countdown timer (frames_left) and a
 * callback function pointer.  When frames_left ticks to 0 the
 * callback fires.  A slot with frames_left == 0 is free.
 * ==================================================================== */

#define MAX_OVERWORLD_TASKS 4

typedef struct {
    uint16_t frames_left;   /* 0 = free */
    void (*callback)(void); /* called when timer expires */
} OverworldTask;

static OverworldTask overworld_tasks[MAX_OVERWORLD_TASKS];

int schedule_overworld_task(void (*callback)(void), uint16_t frames) {
    for (int i = 0; i < MAX_OVERWORLD_TASKS; i++) {
        if (overworld_tasks[i].frames_left == 0) {
            overworld_tasks[i].frames_left = frames;
            overworld_tasks[i].callback = callback;
            return i;
        }
    }
    return -1;  /* no free slot */
}

void process_overworld_tasks(void) {
    /* Assembly lines 7-12: decrement dad phone timer every 256 frames */
    if ((core.frame_counter & 0xFF) == 0 && ow.dad_phone_timer != 0) {
        ow.dad_phone_timer--;
    }

    /* Assembly lines 14-22: skip if window open, battle active, etc. */
    if (any_window_open()) return;
    if (bt.battle_mode_flag) return;
    if (ow.battle_swirl_countdown) return;
    if (ow.enemy_has_been_touched) return;

    /* Assembly lines 23-53: process each task slot */
    for (int i = 0; i < MAX_OVERWORLD_TASKS; i++) {
        if (overworld_tasks[i].frames_left == 0) continue;
        overworld_tasks[i].frames_left--;
        if (overworld_tasks[i].frames_left == 0) {
            if (overworld_tasks[i].callback) {
                overworld_tasks[i].callback();
            }
        }
    }
}

/* ====================================================================
 * Auto-Movement Buffer + Demo Playback
 *
 * Ports of:
 *   CLEAR_AUTO_MOVEMENT_BUFFER    (asm/misc/clear_auto_movement_buffer.asm)
 *   RECORD_AUTO_MOVEMENT_STEP     (asm/misc/record_auto_movement_step.asm)
 *   RECORD_REPEATED_AUTO_MOVEMENT (asm/misc/record_repeated_auto_movement.asm)
 *   QUEUE_AUTO_MOVEMENT_STEP      (asm/misc/queue_auto_movement_step.asm)
 *   START_DEMO_PLAYBACK           (asm/intro/start_demo_playback.asm)
 *   CALCULATE_MOVEMENT_PATH_STEPS (asm/misc/calculate_movement_path_steps.asm)
 *
 * The auto-movement ert.buffer stores RLE-encoded pad-direction records
 * (1 byte count + 2 byte direction).  When queued, it feeds into
 * the demo playback system which overrides PAD_RAW each frame.
 * ==================================================================== */

#define AUTO_MOVEMENT_BUFFER_SIZE 64

typedef struct {
    uint8_t  count;      /* number of frames to hold this direction */
    uint16_t direction;  /* pad state / direction code */
} AutoMovementEntry;

static AutoMovementEntry auto_movement_buffer[AUTO_MOVEMENT_BUFFER_SIZE];
static uint16_t auto_movement_index;  /* current write position */

/* Demo playback state (from ram.asm direct-page vars) */
#define DEMO_FLAG_PLAYBACK  0x4000
#define DEMO_FLAG_RECORDING 0x8000

static const AutoMovementEntry *demo_read_ptr;
static uint16_t demo_initial_pad_state;

/* AUTO_MOVEMENT_DIRECTION_TABLE — maps quantized 8-direction index to
 * pad/direction code used by the movement system.
 * From asm/data/unknown/C48C59.asm. */
static const uint16_t auto_movement_direction_table[8] = {
    PAD_UP,                  /* 0: UP         */
    PAD_UP | PAD_RIGHT,      /* 1: UP_RIGHT   */
    PAD_RIGHT,               /* 2: RIGHT      */
    PAD_DOWN | PAD_RIGHT,    /* 3: DOWN_RIGHT */
    PAD_DOWN,                /* 4: DOWN       */
    PAD_DOWN | PAD_LEFT,     /* 5: DOWN_LEFT  */
    PAD_LEFT,                /* 6: LEFT       */
    PAD_UP | PAD_LEFT,       /* 7: UP_LEFT    */
};

void clear_auto_movement_buffer(void) {
    auto_movement_index = 0;
    for (int i = 0; i < AUTO_MOVEMENT_BUFFER_SIZE; i++) {
        auto_movement_buffer[i].count = 0;
        auto_movement_buffer[i].direction = 0;
    }
}

static void record_auto_movement_step(uint16_t direction) {
    if (auto_movement_index == 0 &&
        auto_movement_buffer[0].count == 0) {
        /* First entry: write directly */
        auto_movement_buffer[0].direction = direction;
        auto_movement_buffer[0].count = 1;
        return;
    }

    /* Check if same direction as current entry — extend the run */
    if (auto_movement_buffer[auto_movement_index].direction == direction) {
        auto_movement_buffer[auto_movement_index].count++;
        return;
    }

    /* Different direction: advance to next entry */
    if (auto_movement_index + 1 >= AUTO_MOVEMENT_BUFFER_SIZE) {
        fprintf(stderr, "auto_movement_buffer overflow!\n");
        return;
    }
    auto_movement_index++;

    auto_movement_buffer[auto_movement_index].direction = direction;
    auto_movement_buffer[auto_movement_index].count = 1;
}

void record_repeated_auto_movement(uint16_t direction_index,
                                    uint16_t count) {
    if (direction_index >= 8) return;
    uint16_t dir = auto_movement_direction_table[direction_index];
    for (uint16_t i = 0; i < count; i++) {
        record_auto_movement_step(dir);
    }
}

/* start_demo_playback — begin playback of the auto-movement ert.buffer.
 * Port of START_DEMO_PLAYBACK (C083E3). */
static void start_demo_playback(void) {
    if (ow.demo_recording_flags & DEMO_FLAG_PLAYBACK)
        return;  /* already playing */

    if (auto_movement_buffer[0].count == 0) {
        /* Empty ert.buffer — clear flags */
        ow.demo_recording_flags = 0;
        return;
    }

    ow.demo_frames_left = auto_movement_buffer[0].count;
    demo_initial_pad_state = auto_movement_buffer[0].direction;
    demo_read_ptr = &auto_movement_buffer[0];

    /* Immediately apply first pad state */
    core.pad1_raw = auto_movement_buffer[0].direction;

    ow.demo_recording_flags |= DEMO_FLAG_PLAYBACK;
}

void queue_auto_movement_step(void) {
    /* Write terminator after last entry */
    uint16_t next = auto_movement_index + 1;
    if (next < AUTO_MOVEMENT_BUFFER_SIZE) {
        auto_movement_buffer[next].count = 0;
    }

    /* Kick off playback */
    start_demo_playback();
}

/* demo_playback_tick — per-frame demo ert.buffer consumer.
 * Called from the joypad update path.
 * Port of READ_JOYPAD demo section (asm/system/read_joypad.asm). */
void demo_playback_tick(void) {
    if (!(ow.demo_recording_flags & DEMO_FLAG_PLAYBACK))
        return;

    ow.demo_frames_left--;
    if (ow.demo_frames_left != 0)
        return;  /* current entry still active, PAD_RAW unchanged */

    /* Advance to next record */
    demo_read_ptr++;

    if (demo_read_ptr->count == 0) {
        /* End of demo — clear playback flag */
        ow.demo_recording_flags &= ~DEMO_FLAG_PLAYBACK;
        ow.demo_frames_left = 0;
        return;
    }

    ow.demo_frames_left = demo_read_ptr->count;
    core.pad1_raw = demo_read_ptr->direction;
}

/* ====================================================================
 * CALCULATE_MOVEMENT_PATH_STEPS
 *
 * Port of asm/misc/calculate_movement_path_steps.asm.
 * Simulates a path from (src_x, src_y) to (dest_x, dest_y) using
 * the movement speed tables, recording each step into the auto-
 * movement ert.buffer.  Returns the number of steps needed.
 * ==================================================================== */

uint16_t calculate_movement_path_steps(int16_t src_x, int16_t src_y,
                                        int16_t dest_x, int16_t dest_y) {
    uint16_t steps = 0;

    /* Fixed-point 16.16 positions */
    int32_t cur_x = (int32_t)src_x << 16;
    int32_t cur_y = (int32_t)src_y << 16;

    while (1) {
        /* Check if we've arrived: |dx| <= 1 and |dy| <= 1 */
        int16_t ix = (int16_t)(cur_x >> 16);
        int16_t iy = (int16_t)(cur_y >> 16);
        int16_t dx = ix - dest_x;
        int16_t dy = iy - dest_y;

        int16_t abs_dx = dx < 0 ? -dx : dx;
        int16_t abs_dy = dy < 0 ? -dy : dy;

        /* Assembly: CLC; SBC #1; BRANCHGTS for abs_dx, JUMPLTEQS for abs_dy.
         * The CLC+SBC gives abs-1-1 = abs-2, BRANCHGTS means >= 0, so abs >= 2.
         * If abs_dx < 2 (i.e. <= 1), check abs_dy.  If abs_dy < 2, arrived. */
        if (abs_dx <= 1 && abs_dy <= 1)
            break;

        /* Calculate 8-way direction from current to destination */
        int16_t dir = calculate_direction_8(ix, iy, dest_x, dest_y);
        if (dir < 0 || dir > 7) dir = 0;

        /* Record this step */
        record_auto_movement_step(auto_movement_direction_table[dir]);

        /* Advance position using speed tables (walking style 0 = NORMAL) */
        cur_x += pb.h_speeds[dir];  /* style 0, direction dir */
        cur_y += pb.v_speeds[dir];

        steps++;

        /* Safety: prevent infinite loop */
        if (steps > 500) break;
    }

    return steps;
}

/* ---- ATTEMPT_HOMESICKNESS (port of asm/overworld/attempt_homesickness.asm) ----
 *
 * Checks if Ness should become homesick. Called periodically from the overworld.
 * Ness must be alive (afflictions[0] != UNCONSCIOUS).
 * Level brackets of 15: at each bracket, checks a probability from the table.
 * HOMESICKNESS_PROBABILITY table: {0, 100, 150, 200, 250, 0} (6 entries).
 * RAND_MOD(prob) returns 0..prob; if result == 0, inflict homesick status.
 * Returns 0 always (non-homesick or failed). */
static const uint8_t homesickness_probability[] = { 0, 100, 150, 200, 250, 0 };

uint16_t attempt_homesickness(void) {
    /* Assembly line 8-11: check if Ness is alive */
    uint8_t status = party_characters[0].afflictions[0];  /* STATUS_GROUP::PERSISTENT_EASYHEAL */
    if (status == 1)  /* UNCONSCIOUS */
        return 0;

    /* Assembly lines 12-50: iterate level brackets of 15 */
    uint16_t threshold = 15;
    /* Loop count: LDA #100/15 = 6; CLC; SBC counter; BRANCHGTS → 6 iterations (counter 0..5) */
    for (int bracket = 0; bracket <= 5; bracket++) {
        /* Assembly: LDA Ness.level; CLC; SBC threshold; BRANCHGTS @NEXT_BRACKET
         * CLC+SBC = level - threshold - 1. BRANCHGTS (N xor V == 0) branches when >= 0,
         * i.e. level - threshold - 1 >= 0 → level >= threshold + 1 → level > threshold.
         * So: if level > threshold, skip to next bracket. */
        uint8_t level = party_characters[0].level;
        if (level > threshold) {
            threshold += 15;
            continue;
        }

        /* Level is in this bracket — check probability */
        uint8_t prob = homesickness_probability[bracket];
        if (prob == 0)
            return 0;

        /* RAND_MOD(prob): returns 0..prob inclusive (rand % (prob+1)); homesick if result == 0 */
        uint16_t roll = (uint16_t)(rand() % ((int)prob + 1));
        if (roll != 0)
            return 0;

        /* Inflict homesick: STATUS_GROUP::HOMESICKNESS = status_group 6 (arg+1=7),
         * STATUS_5::HOMESICK = value 1 (arg+1=2), PARTY_MEMBER::NESS = 1 */
        inflict_status_nonbattle(1, 7, 2);
        return 0;
    }

    return 0;
}


/* ---- INITIALIZE_MAP (port of asm/overworld/initialize_map.asm) ----
 *
 * Wrapper function that calls the four map initialization steps in order.
 * Assembly: A=x, X=y, Y=direction.
 * 1. RESOLVE_MAP_SECTOR_MUSIC(x, y) — determine music for map sector
 * 2. LOAD_MAP_AT_POSITION(x, y) — load map tiles, collision, palette
 * 3. SET_LEADER_POSITION_AND_LOAD_PARTY(x, y, direction) — place party
 * 4. APPLY_NEXT_MAP_MUSIC() — start playing the resolved music */
void initialize_map(uint16_t x, uint16_t y, uint16_t direction) {
    resolve_map_sector_music(x, y);
    load_map_at_position(x, y);
    set_leader_position_and_load_party(x, y, direction);
    apply_next_map_music();
}

/* ---- SET_TELEPORT_STATE (port of asm/overworld/set_teleport_state.asm) ----
 *
 * Stores teleport destination and style into global variables.
 * Assembly: A=destination (8-bit), PARAM01=style (8-bit). */
void set_teleport_state(uint8_t destination, uint8_t style) {
    ow.psi_teleport_destination = (uint16_t)destination;
    ow.psi_teleport_style = (uint16_t)style;
}

/* ---- CENTER_SCREEN (port of asm/system/center_screen.asm) ----
 *
 * Subtracts half-screen offsets from x/y and refreshes tilemaps.
 * Assembly: A=x, X=y. Calls REFRESH_MAP_AT_POSITION(A=x-128, X=y-112). */
void center_screen(uint16_t x, uint16_t y) {
    map_refresh_tilemaps(x, y);
}

/* ---- SET_PARTY_TICK_CALLBACKS (port of asm/overworld/set_party_tick_callbacks.asm) ----
 *
 * Sets tick callback for leader entity and 6 follower entities.
 * Assembly: A=leader_slot, $0E/$10=leader callback (32-bit),
 *           $12/$14=follower callback (32-bit).
 * Stores 16-bit low word in tick_callback_lo, bank byte in tick_callback_hi. */
void set_party_tick_callbacks(uint16_t leader_slot,
                              uint32_t leader_cb, uint32_t follower_cb) {
    uint16_t x = ENT(leader_slot);
    entities.tick_callback_lo[x] = leader_cb & 0xFFFF;
    entities.tick_callback_hi[x] = (leader_cb >> 16) & 0xFF;

    uint16_t follower_lo = follower_cb & 0xFFFF;
    uint16_t follower_hi = (follower_cb >> 16) & 0xFF;
    for (int i = 0; i < 6; i++) {
        x++;
        entities.tick_callback_lo[x] = follower_lo;
        entities.tick_callback_hi[x] = follower_hi;
    }
}


