/*
 * Overworld interaction and dialogue functions.
 *
 * Ported from:
 *   DISPLAY_TEXT_AND_WAIT_FOR_FADE       — asm/overworld/display_text_and_wait_for_fade.asm
 *   PROCESS_QUEUED_INTERACTIONS          — asm/overworld/process_queued_interactions.asm
 *   RELOAD_HOTSPOTS                      — asm/overworld/reload_hotspots.asm
 *   ACTIVATE_HOTSPOT                     — asm/overworld/activate_hotspot.asm
 *   CREATE_PREPARED_ENTITY_NPC           — asm/overworld/create_prepared_entity_npc.asm
 *   SET_ENTITY_DIRECTION_FROM_LEADER     — C042C2
 *   FIND_ADJACENT_NPC_INTERACTION        — C041E3
 *   FIND_NEARBY_TALKABLE_TPT_ENTRY       — find_nearby_talkable_tpt_entry.asm
 *   FIND_NEARBY_CHECKABLE_TPT_ENTRY      — find_nearby_checkable_tpt_entry.asm
 *   TALK_TO                              — asm/overworld/talk_to.asm
 *   CHECK                                — asm/overworld/check.asm
 */

#include "game/overworld_internal.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "game/display_text.h"
#include "game/window.h"
#include "game/door.h"
#include "game/audio.h"
#include "game/fade.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/constants.h"
#include "core/math.h"
#include "data/assets.h"
#include "platform/platform.h"
#include "game_main.h"

#include <stdio.h>
#include "data/text_refs.h"


/* NPC type constants (from include/enums.asm NPC_TYPE enum) */
#define NPC_TYPE_PERSON   1
#define NPC_TYPE_ITEM_BOX 2
#define NPC_TYPE_OBJECT   3

void display_text_and_wait_for_fade(uint32_t text_snes_addr) {
    disable_all_entities();
    display_text_from_addr(text_snes_addr);
    /* Assembly: loop calling WINDOW_TICK until ow.entity_fade_entity == -1.
     * Entity fade animation callroutines are ported (callbacks.c).
     * The fade entity (EVENT_ENTITY_WIPE, script 859) drives the animation via its script. */
    while (ow.entity_fade_entity != -1) {
        window_tick();
        if (platform_input_quit_requested()) break;
    }
    enable_all_entities();
}

/* ---- PROCESS_QUEUED_INTERACTIONS (port of process_queued_interactions.asm) ----
 *
 * Port of asm/overworld/process_queued_interactions.asm.
 * Dequeues the next interaction from the circular queue and dispatches:
 *   Type 0, 8, 9: NPC/text interaction → display_text_and_wait_for_fade
 *   Type 2:       Door interaction → door_transition
 *   Type 10:      Screen reload / dad phone text → display_text_and_wait_for_fade
 *
 * Called from the main loop when interactions are pending and no
 * battle/swirl is in progress. */
void process_queued_interactions(void) {
    /* Assembly lines 11-20: dequeue from circular ert.buffer */
    uint16_t idx = ow.current_queued_interaction;
    uint16_t type = ow.queued_interactions[idx].type;
    uint32_t data_ptr = ow.queued_interactions[idx].data_ptr;

    /* Assembly line 25: store current type for duplicate prevention */
    ow.current_queued_interaction_type = type;

    /* Assembly lines 26-29: advance read index (circular, mask 0x03) */
    ow.current_queued_interaction = (idx + 1) & (INTERACTION_QUEUE_SIZE - 1);

    /* Assembly lines 30-32: clear intangibility low bit */
    ow.player_intangibility_frames &= 0xFFFE;

    /* Assembly line 33: CLEAR_PARTY_SPRITE_HIDE_FLAGS */
    clear_party_sprite_hide_flags();

    /* Assembly lines 34-71: dispatch by interaction type */
    switch (type) {
    case 2:
        /* Door interaction.
         * Assembly: JSR DOOR_TRANSITION with data_ptr. */
        door_transition(data_ptr);
        break;
    case 10:
        /* Screen reload / dad phone text.
         * Assembly: DISPLAY_TEXT_AND_WAIT_FOR_FADE, then check if this was
         * the dad phone message (MSG_SYS_PAPA_2H). If so, set timer. */
        display_text_and_wait_for_fade(data_ptr);
        if (data_ptr == MSG_SYS_PHONE_DAD) {
            ow.dad_phone_timer = 1687;
            ow.dad_phone_queued = 0;
        }
        break;
    case 0:   /* Text interaction */
    case 8:   /* NPC interaction */
    case 9:   /* Text interaction (variant) */
        display_text_and_wait_for_fade(data_ptr);
        break;
    default:
        break;
    }

    /* Assembly lines 72-81: update pending flag and clear current type.
     * Check if more interactions are queued. */
    ow.pending_interactions = (ow.current_queued_interaction != ow.next_queued_interaction) ? 1 : 0;
    ow.current_queued_interaction_type = 0xFFFF;
}

/* ---- RELOAD_HOTSPOTS (port of asm/overworld/reload_hotspots.asm) ----
 *
 * Reloads the 2 active hotspot boundaries from saved game_state data.
 * Called when continuing a saved game from file select.
 * Reads active_hotspot_modes/ids from game_state, looks up coordinates
 * in MAP_HOTSPOTS data table, multiplies by 8 for pixel coords, and
 * copies the saved pointer from game_state.active_hotspot_pointers. */
void reload_hotspots(void) {
    static const HotspotCoords *hotspot_data = NULL;

    if (!hotspot_data) {
        hotspot_data = (const HotspotCoords *)ASSET_DATA(ASSET_DATA_HOTSPOT_COORDINATES_BIN);
        if (!hotspot_data) {
            fprintf(stderr, "overworld: hotspot_coordinates data not available\n");
            return;
        }
    }

    for (int i = 0; i < NUM_ACTIVE_HOTSPOTS; i++) {
        uint8_t mode = game_state.active_hotspot_modes[i];
        if (mode == 0)
            continue;

        /* Set mode in active_hotspot struct */
        ow.active_hotspots[i].mode = mode;

        /* Look up predefined hotspot by ID */
        uint8_t hotspot_id = game_state.active_hotspot_ids[i];
        if (hotspot_id >= HOTSPOT_COORD_COUNT)
            continue;
        const HotspotCoords *hc = &hotspot_data[hotspot_id];

        /* Multiply by 8 (tile-to-pixel) */
        ow.active_hotspots[i].x1 = hc->x1 * 8;
        ow.active_hotspots[i].y1 = hc->y1 * 8;
        ow.active_hotspots[i].x2 = hc->x2 * 8;
        ow.active_hotspots[i].y2 = hc->y2 * 8;

        /* Copy the 32-bit pointer from game_state.active_hotspot_pointers */
        uint32_t ptr = read_u32_le(&game_state.active_hotspot_pointers[i * 4]);
        ow.active_hotspots[i].pointer = ptr;
    }
}

/* ---- CREATE_PREPARED_ENTITY_NPC (port of asm/overworld/create_prepared_entity_npc.asm) ----
 *
 * Creates an NPC entity at ow.entity_prepared_x/y/direction.
 * Looks up the sprite ID from NPC_CONFIG_TABLE[npc_id] offset 1,
 * calls create_entity(), then sets direction and NPC ID on the entity.
 * Returns the entity slot number. */
int16_t create_prepared_entity_npc(uint16_t npc_id, uint16_t action_script) {
    /* Look up sprite ID from NPC_CONFIG_TABLE */
    uint16_t sprite_id = get_npc_config_sprite(npc_id);

    /* Create entity at prepared coordinates */
    int16_t slot = create_entity(sprite_id, action_script, -1,
                                 ow.entity_prepared_x, ow.entity_prepared_y);
    if (slot >= 0) {
        entities.directions[slot] = ow.entity_prepared_direction;
        entities.npc_ids[slot] = npc_id;
    }
    return slot;
}

/* ---- ACTIVATE_HOTSPOT (port of asm/overworld/activate_hotspot.asm) ----
 *
 * Sets up an active hotspot boundary for player position tracking.
 * Reads predefined coordinates from MAP_HOTSPOTS[hotspot_id], multiplies
 * by 8 for pixel coords, determines if leader is inside or outside the
 * boundary, and stores the result in ow.active_hotspots[slot-1] and game_state. */
void activate_hotspot(uint16_t slot, uint16_t hotspot_id, uint32_t pointer) {
    static const HotspotCoords *hotspot_data = NULL;

    if (!hotspot_data) {
        hotspot_data = (const HotspotCoords *)ASSET_DATA(ASSET_DATA_HOTSPOT_COORDINATES_BIN);
        if (!hotspot_data) return;
    }

    int idx = slot - 1;  /* Convert 1-indexed to 0-indexed */
    if (idx < 0 || idx >= 2) return;  /* Only slots 1 and 2 are valid */

    /* Read predefined hotspot coordinates and multiply by 8 */
    if (hotspot_id >= HOTSPOT_COORD_COUNT) return;
    const HotspotCoords *hc = &hotspot_data[hotspot_id];
    uint16_t x1 = hc->x1 * 8;
    uint16_t y1 = hc->y1 * 8;
    uint16_t x2 = hc->x2 * 8;
    uint16_t y2 = hc->y2 * 8;

    /* Determine if leader is inside or outside the bounding box.
     * Assembly uses signed BLTEQ for <=, unsigned BCS for >=.
     * Inside: x1 < leader_x < x2 AND y1 < leader_y < y2 → mode=1
     * Outside: otherwise → mode=2 */
    uint16_t leader_x = game_state.leader_x_coord;
    uint16_t leader_y = game_state.leader_y_coord;
    uint16_t mode;
    if (leader_x > x1 && leader_x < x2 && leader_y > y1 && leader_y < y2) {
        mode = 1;  /* Player is inside the hotspot */
    } else {
        mode = 2;  /* Player is outside the hotspot */
    }

    /* Store into ow.active_hotspots[slot-1] */
    ow.active_hotspots[idx].mode = mode;
    ow.active_hotspots[idx].x1 = x1;
    ow.active_hotspots[idx].y1 = y1;
    ow.active_hotspots[idx].x2 = x2;
    ow.active_hotspots[idx].y2 = y2;
    ow.active_hotspots[idx].pointer = pointer;

    /* Persist into game_state for save/load */
    game_state.active_hotspot_modes[idx] = (uint8_t)mode;
    game_state.active_hotspot_ids[idx] = (uint8_t)hotspot_id;
    game_state.active_hotspot_pointers[idx * 4 + 0] = (uint8_t)(pointer);
    game_state.active_hotspot_pointers[idx * 4 + 1] = (uint8_t)(pointer >> 8);
    game_state.active_hotspot_pointers[idx * 4 + 2] = (uint8_t)(pointer >> 16);
    game_state.active_hotspot_pointers[idx * 4 + 3] = (uint8_t)(pointer >> 24);
}

/* ---- SET_ENTITY_DIRECTION_FROM_LEADER (port of C042C2) ----
 *
 * Makes an NPC entity face toward the leader by looking up the leader's
 * direction in direction_facing_table, setting the NPC's direction,
 * clearing its motion, and re-rendering the sprite. */
void set_entity_direction_from_leader(int16_t entity_slot) {
    /* Look up the direction the NPC should face */
    uint16_t leader_dir = game_state.leader_direction;
    int16_t npc_dir = direction_facing_table[leader_dir & 7];

    /* Set entity direction */
    entities.directions[entity_slot] = npc_dir;

    /* Clear motion */
    clear_entity_delta_motion(entity_slot);

    /* Re-render sprite (port of JSL RENDER_ENTITY_SPRITE_ENTRY2) */
    render_entity_sprite(entity_slot);
}

/* ---- FIND_ADJACENT_NPC_INTERACTION (port of C041E3) ----
 *
 * Same rotation pattern as find_clear_direction_for_leader, but uses
 * check_directional_npc_collision (extended door search). */
int16_t find_adjacent_npc_interaction(void) {
    int16_t orig_dir = game_state.leader_direction & 0xFFFE;
    int16_t dir = orig_dir;

    /* Try current direction */
    int16_t result = check_directional_npc_collision(dir);
    if (result != -1 && result != 0)
        return dir;

    /* Try clockwise 90° */
    dir = (dir + 2) & 7;
    game_state.leader_direction = dir;
    result = check_directional_npc_collision(dir);
    if (result != -1 && result != 0)
        return dir;

    /* Try counterclockwise 90° */
    dir = (dir + 4) & 7;
    game_state.leader_direction = dir;
    result = check_directional_npc_collision(dir);
    if (result != -1 && result != 0)
        return dir;

    /* Try opposite */
    dir = (dir - 2) & 7;
    game_state.leader_direction = dir;
    result = check_directional_npc_collision(dir);
    if (result != -1 && result != 0)
        return dir;

    /* Nothing found */
    game_state.leader_direction = orig_dir;
    return -1;
}

/* ---- FIND_NEARBY_TALKABLE_TPT_ENTRY (port of find_nearby_talkable_tpt_entry.asm) ----
 *
 * Initializes interaction state, calls find_clear_direction_for_leader,
 * and if a direction is found, rotates the leader entity to face that way.
 * Returns ow.interacting_npc_id. */
int16_t find_nearby_talkable_tpt_entry(void) {
    ow.interacting_npc_id = 0xFFFF;
    ow.interacting_npc_entity = 0xFFFF;

    int16_t found_dir = find_clear_direction_for_leader();
    if (found_dir == -1)
        return (int16_t)ow.interacting_npc_id;

    /* If direction changed, update leader entity to face the new direction */
    int16_t leader_slot = game_state.current_party_members;
    int16_t leader_off = ENT(leader_slot);

    if (found_dir != (int16_t)entities.directions[leader_off]) {
        game_state.leader_direction = found_dir;
        entities.directions[leader_off] = found_dir;

        /* Re-render leader sprite (port of JSL UPDATE_ENTITY_SPRITE = 8-dir) */
        update_entity_sprite(leader_off);
    }

    return (int16_t)ow.interacting_npc_id;
}

/* ---- FIND_NEARBY_CHECKABLE_TPT_ENTRY (port of find_nearby_checkable_tpt_entry.asm) ----
 *
 * Same pattern as find_nearby_talkable_tpt_entry but uses
 * find_adjacent_npc_interaction (extended door search). */
int16_t find_nearby_checkable_tpt_entry(void) {
    ow.interacting_npc_id = 0xFFFF;
    ow.interacting_npc_entity = 0xFFFF;

    int16_t found_dir = find_adjacent_npc_interaction();
    if (found_dir == -1)
        return (int16_t)ow.interacting_npc_id;

    /* If direction changed, update leader entity */
    int16_t leader_slot = game_state.current_party_members;
    int16_t leader_off = ENT(leader_slot);

    if (found_dir != (int16_t)entities.directions[leader_off]) {
        game_state.leader_direction = found_dir;
        entities.directions[leader_off] = found_dir;
        update_entity_sprite(leader_off);
    }

    return (int16_t)ow.interacting_npc_id;
}

/* ---- TALK_TO (port of asm/overworld/talk_to.asm) ----
 *
 * Finds a nearby talkable NPC, looks up its text pointer from
 * NPC_CONFIG_TABLE, and returns the SNES ROM address of its dialogue.
 * For PERSON type NPCs, makes the NPC face the player first.
 * Returns 0 if nothing to talk to. */
uint32_t talk_to(void) {
    uint32_t text_ptr = 0;

    create_window(0x01);  /* WINDOW::TEXT_STANDARD */

    find_nearby_talkable_tpt_entry();

    if (ow.interacting_npc_id == 0)
        return 0;
    if (ow.interacting_npc_id == 0xFFFF)
        return 0;

    if (ow.interacting_npc_id == 0xFFFE) {
        /* Door — use MAP_OBJECT_TEXT */
        return ow.map_object_text;
    }

    /* Look up NPC config */
    uint8_t npc_type = get_npc_config_type(ow.interacting_npc_id);

    if (npc_type == NPC_TYPE_PERSON) {
        /* Make the NPC face the player */
        set_entity_direction_from_leader(ow.interacting_npc_entity);

        /* Return the text pointer */
        text_ptr = get_npc_config_text_pointer(ow.interacting_npc_id);
    }
    /* ITEM_BOX and OBJECT types return 0 for TALK_TO (can't talk to items) */

    return text_ptr;
}

/* ---- CHECK (port of asm/overworld/check.asm) ----
 *
 * Finds a nearby checkable NPC/object, looks up its text/item data
 * from NPC_CONFIG_TABLE. For PERSON type returns text, for ITEM_BOX
 * sets up gift data and returns text, for OBJECT returns text.
 * Returns 0 if nothing to check. */
uint32_t check_action(void) {
    uint32_t text_ptr = 0;

    create_window(0x01);  /* WINDOW::TEXT_STANDARD */

    find_nearby_checkable_tpt_entry();

    if (ow.interacting_npc_id == 0)
        return 0;
    if (ow.interacting_npc_id == 0xFFFF)
        return 0;

    if (ow.interacting_npc_id == 0xFFFE) {
        /* Door — use MAP_OBJECT_TEXT */
        return ow.map_object_text;
    }

    /* Look up NPC config */
    uint8_t npc_type = get_npc_config_type(ow.interacting_npc_id);

    switch (npc_type) {
    case NPC_TYPE_PERSON:
        /* Can't CHECK a person — assembly returns NULL */
        break;

    case NPC_TYPE_ITEM_BOX: {
        uint16_t item = get_npc_config_item(ow.interacting_npc_id);
        if (item < 256) {
            /* Item gift: working_memory = item_id.
             * Port of check.asm @ITEM_BOX (lines 52-54). */
            set_working_memory((uint32_t)item);
        } else {
            /* Money gift: working_memory = 0, argument_memory = amount (item - 256).
             * Port of check.asm @GIFT_MONEY (lines 57-77). */
            set_working_memory(0);
            set_argument_memory((uint32_t)(item - 256));
        }
        /* Store event flag for later flag-setting by text script */
        dt.current_interacting_event_flag = get_npc_config_event_flag(ow.interacting_npc_id);
        text_ptr = get_npc_config_text_pointer(ow.interacting_npc_id);
        break;
    }

    case NPC_TYPE_OBJECT:
        text_ptr = get_npc_config_text_pointer(ow.interacting_npc_id);
        break;
    }

    return text_ptr;
}
