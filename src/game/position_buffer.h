/*
 * PLAYER_POSITION_BUFFER — circular history buffer for follow-the-leader.
 *
 * Port of the position buffer system used by:
 *   INIT_PARTY_POSITION_BUFFER  (asm/overworld/party/init_party_position_buffer.asm)
 *   UPDATE_LEADER_MOVEMENT      (asm/overworld/update_leader_movement.asm)
 *   UPDATE_FOLLOWER_STATE       (asm/overworld/update_follower_state.asm)
 *
 * The leader entity writes its position each frame to a 256-entry circular
 * buffer. Follower party members read from the buffer at a time delay,
 * creating a trail effect where each follower walks the same path the
 * leader walked N frames ago.
 */
#ifndef GAME_POSITION_BUFFER_H
#define GAME_POSITION_BUFFER_H

#include "core/types.h"

#define POSITION_BUFFER_SIZE 256

/* Packed version of assembly player_position_buffer_entry (include/structs.asm).
 * Assembly uses 12 bytes (6 words) per entry, but tile_flags, walking_style,
 * and direction all fit in uint8_t and unknown10 is dead (write-only, always 0).
 * 8 bytes per entry, 256 entries = 2,048 bytes (saves 1,024 bytes vs assembly layout). */
typedef struct {
    int16_t  x_coord;       /* 0 */
    int16_t  y_coord;       /* 2 */
    uint8_t  tile_flags;    /* 4 — surface flags, only low byte used */
    uint8_t  walking_style; /* 5 — 0–13 */
    uint8_t  direction;     /* 6 — 0–7 */
    uint8_t  pad;           /* 7 — alignment padding */
} PositionBufferEntry;

/* Speed table dimensions. */
#define NUM_WALKING_STYLES_EXTERN 14
#define NUM_DIRECTIONS_EXTERN     8

/* Consolidated position buffer runtime state. */
typedef struct {
    PositionBufferEntry player_position_buffer[POSITION_BUFFER_SIZE];

    /* Movement speed BSS tables, 14 styles x 8 directions x 32-bit fixed-point.
     * Indexed as [style * 8 + direction].  Populated by velocity_store(). */
    int32_t h_speeds[NUM_WALKING_STYLES_EXTERN * NUM_DIRECTIONS_EXTERN];
    int32_t v_speeds[NUM_WALKING_STYLES_EXTERN * NUM_DIRECTIONS_EXTERN];

    /* BSS: frame counter for camera mode 3 shake effect. */
    uint16_t camera_mode_3_frames_left;

    /* Camera mode backup for mode 3 shake (from ram.asm:893-895). */
    uint16_t camera_mode_backup;

    /* Bicycle diagonal turn animation counter. */
    uint16_t bicycle_diagonal_turn_counter;
} PositionBufferState;

extern PositionBufferState pb;

/* Fill all 256 entries with leader's current state, reset write index.
 * Port of INIT_PARTY_POSITION_BUFFER (C03F1E). */
void init_party_position_buffer(void);

/* Fill all 256 entries with positions trailing behind the leader,
 * creating a spread-out formation for followers after teleport.
 * Port of FILL_PARTY_POSITION_BUFFER (asm/overworld/party/fill_party_position_buffer.asm).
 * Called from TELEPORT when bit 7 of the direction byte is set. */
void fill_party_position_buffer(uint16_t direction);

/* Write the source entity's position to the buffer.
 * Reads position_buffer_index, leader_moved, leader_direction, etc. from
 * game_state.  Must be called AFTER leader position/direction/terrain are
 * updated in game_state for the current frame.
 * Port of UPDATE_LEADER_MOVEMENT buffer-write logic (lines 61-119). */
void position_buffer_write(int16_t source_entity);

/* Per-frame follower tick: read position from buffer, update entity.
 * Port of UPDATE_FOLLOWER_STATE (C04D78). */
void update_follower_state(int16_t entity_offset);

/* Leader entity tick callback.
 * Port of UPDATE_LEADER_MOVEMENT (C04236). */
void update_leader_movement(int16_t entity_offset);

/* Initialize horizontal/vertical movement speed lookup tables.
 * Port of VELOCITY_STORE (asm/overworld/velocity_store.asm).
 * Called from RESET_PARTY_STATE during party initialization. */
void velocity_store(void);

/* Initialize follower visuals from position buffer (one-time setup).
 * Port of UPDATE_FOLLOWER_VISUALS (C04EF0). Called during EVENT_002 (party follower) init. */
void update_follower_visuals(int16_t entity_offset);

/* Update sprite graphics, walking_style, animation speed, and var7 flags.
 * Port of UPDATE_PARTY_ENTITY_GRAPHICS (C07A56). Called per-frame from
 * update_follower_state and once from update_follower_visuals. */
void update_party_entity_graphics(int16_t char_id, uint16_t walking_style,
                                   int16_t entity_offset, int16_t party_idx);

/* Camera shake (enemy contact) — screen shake + direction control.
 * Port of START_CAMERA_SHAKE (C04A88).
 * Sets camera_mode=3 with a 12-frame timer, plays collision SFX. */
void start_camera_shake(void);

/* Convert d-pad input to 8-way direction.
 * Port of MAP_INPUT_TO_DIRECTION (asm/overworld/map_input_to_direction.asm).
 * Returns direction 0-7, or -1 if no input or direction not allowed. */
int16_t map_input_to_direction(uint16_t walking_style);

/* Circular distance (buffer entries) between prev member and this member.
 * Port of GET_DISTANCE_TO_PARTY_MEMBER (C03E9D). */
uint16_t get_distance_to_party_member(int16_t char_id,
                                      uint16_t current_position_index);

/* Adjust follower position_index based on spacing to previous member.
 * Port of ADJUST_PARTY_MEMBER_VISIBILITY (C03EC3). */
uint16_t adjust_party_member_visibility(int16_t entity_offset,
                                        int16_t char_id,
                                        uint16_t position_index,
                                        uint16_t spacing);

/* Lookup sprite ID from the PLAYABLE_CHAR_GFX_TABLE.
 * Pure table lookup with no side effects (used by photo rendering).
 * char_id: 0-based row index, mode: column index. */
uint16_t lookup_playable_char_sprite(uint16_t char_id, uint16_t mode);

#endif /* GAME_POSITION_BUFFER_H */
