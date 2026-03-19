#ifndef DOOR_H
#define DOOR_H

#include "core/types.h"
#include "include/binary.h"

/* Screen transition config entry (12 bytes, packed to match ROM binary layout).
 * 34 entries in the table. */
#define SCREEN_TRANSITION_CONFIG_COUNT 34

PACKED_STRUCT
typedef struct {
    uint8_t duration;              /*  0: frames (0xFF = 900) */
    uint8_t animation_id;          /*  1 */
    uint8_t animation_flags;       /*  2 */
    uint8_t fade_style;            /*  3 */
    uint8_t direction;             /*  4 */
    uint8_t scroll_speed;          /*  5 */
    uint8_t slide_speed;           /*  6 */
    uint8_t start_sound_effect;    /*  7 */
    uint8_t secondary_duration;    /*  8 */
    uint8_t secondary_animation_id;    /*  9 */
    uint8_t secondary_animation_flags; /* 10 */
    uint8_t ending_sound_effect;   /* 11 */
} ScreenTransitionConfig;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(ScreenTransitionConfig, 12);

/* Door type constants (from include/enums.asm DOOR_TYPE enum). */
#define DOOR_TYPE_0  0  /* Standard door: check event flag, then show text */
#define DOOR_TYPE_1  1  /* Stairs: set walking style stairs */
#define DOOR_TYPE_2  2  /* Activate door: queue door transition */
#define DOOR_TYPE_3  3  /* Escalator: handle escalator movement */
#define DOOR_TYPE_4  4  /* Stairs movement: handle stairs movement */
#define DOOR_TYPE_5  5  /* No-op door */
#define DOOR_TYPE_6  6  /* No-op door (type 6) */
#define DOOR_TYPE_7  7  /* Same handler as type 5 */

/* Max saved door interactions across transitions. */
#define MAX_DOOR_INTERACTIONS 5

/* Consolidated door system runtime state. */
typedef struct {
    /* Set by find_door_at_position():
     * door_found: 16-bit offset into DOOR_DATA (bank-local pointer).
     * door_found_type: door type (0-7). */
    uint16_t door_found;
    uint16_t door_found_type;

    /* Non-zero when a door interaction is queued and being processed. */
    uint16_t using_door;

    /* Non-zero when exit transition faded to white (palette wipe needed). */
    uint16_t wipe_palettes_on_map_load;

    /* Transition scroll state (RAM vars $7E3C22-$7E3C30).
     * 32-bit fixed-point (16.16): high=pixel, low=fraction. */
    int32_t transition_x_velocity;
    int32_t transition_y_velocity;
    int32_t transition_x_accum;
    int32_t transition_y_accum;

    /* Saved interactions across door transitions (32-bit pointers). */
    uint32_t door_interactions[MAX_DOOR_INTERACTIONS];
} DoorState;

extern DoorState dr;

/* Search for a door at the given tile coordinates.
 * Port of FIND_DOOR_AT_POSITION (asm/overworld/door/find_door_at_position.asm).
 * Returns door_type (0-7) if found, 0xFF if no door at that position.
 * Sets door_found and door_found_type globals on success. */
uint8_t find_door_at_position(uint16_t x_tile, uint16_t y_tile);

/* Process a door found at a tile position.
 * Port of PROCESS_DOOR_AT_TILE (asm/overworld/door/process_door_at_tile.asm).
 * Dispatches to the appropriate door type handler.
 * Returns 1 if the door set stairs walking style, 0 otherwise. */
uint16_t process_door_at_tile(uint16_t x_tile, uint16_t y_tile);

/* Check door in the direction the player is facing.
 * Port of CHECK_DOOR_IN_DIRECTION (asm/overworld/door/check_door_in_direction.asm).
 * Called from check_collision_in_direction when no NPC found.
 * Sets interacting_npc_id = 0xFFFE and map_object_text if door found. */
void check_door_in_direction(uint16_t direction);

/* Check door near the leader position.
 * Port of CHECK_DOOR_NEAR_LEADER (asm/misc/check_door_near_leader.asm).
 * Called from check_directional_npc_collision when no NPC found.
 * Sets interacting_npc_id = 0xFFFE and map_object_text if door found. */
void check_door_near_leader(uint16_t direction);

/* Full door transition sequence.
 * Port of DOOR_TRANSITION (asm/overworld/door_transition.asm).
 * Handles text display, event flag checks, screen transition,
 * map loading, music resolution, and party placement. */
void door_transition(uint32_t door_data_ptr);

/* Save/restore door interactions across a transition.
 * Port of PROCESS_DOOR_INTERACTIONS (asm/overworld/door/process_door_interactions.asm). */
void process_door_interactions(void);

/* Screen transition effect for doors and teleports.
 * Port of SCREEN_TRANSITION (asm/overworld/screen_transition.asm).
 * transition_type: index into screen transition config table (0-33).
 * mode: 1 = exit (fade out), 0 = enter (fade in). */
void screen_transition(uint8_t transition_type, uint8_t mode);

/* Get the sound effect for a screen transition.
 * Port of GET_SCREEN_TRANSITION_SOUND_EFFECT.
 * get_start: 1 = start sound, 0 = ending sound.
 * Returns SFX ID. */
uint16_t get_screen_transition_sound_effect(uint16_t transition_id,
                                             uint16_t get_start);

/* Get a pointer into the door_data buffer for a given door_found offset.
 * Returns NULL if door data is not loaded. */
const uint8_t *get_door_data_entry(uint16_t door_found_offset);

#endif /* DOOR_H */
