/*
 * Overworld teleport functions.
 *
 * Ported from:
 *   SETUP_TELEPORT_ENTITY_FLAGS       — asm/overworld/teleport/setup_teleport_entity_flags.asm
 *   INIT_PSI_TELEPORT_BETA            — asm/overworld/teleport/init_psi_teleport_beta.asm
 *   RESET_ENTITIES_AFTER_TELEPORT     — asm/overworld/teleport/reset_entities_after_teleport.asm
 *   RUN_TELEPORT_FAILURE_SEQUENCE     — asm/overworld/teleport/run_teleport_failure_sequence.asm
 *   UPDATE_PSI_TELEPORT_SPEED         — asm/overworld/teleport/update_psi_teleport_speed.asm
 *   RECORD_PLAYER_POSITION            — asm/overworld/record_player_position.asm
 *   SET_TELEPORT_ENTITY_SPEED_VARS    — asm/overworld/teleport/set_teleport_entity_speed_vars.asm
 *   SET_TELEPORT_VELOCITY_BY_DIRECTION — asm/overworld/teleport/set_teleport_velocity_by_direction.asm
 *   UPDATE_TELEPORT_BETA_INPUT        — asm/overworld/teleport/update_teleport_beta_input.asm
 *   CALCULATE_VELOCITY_COMPONENTS     — asm/misc/calculate_velocity_components.asm
 *   UPDATE_TELEPORT_PARTY_VISIBILITY  — asm/overworld/teleport/update_teleport_party_visibility.asm
 *   UPDATE_PARTY_ENTITY_FROM_BUFFER   — asm/overworld/party/update_party_entity_from_buffer.asm
 *   PSI_TELEPORT_ALPHA_TICK           — asm/overworld/teleport/psi_teleport_alpha_tick.asm
 *   PSI_TELEPORT_BETA_TICK            — asm/overworld/teleport/psi_teleport_beta_tick.asm
 *   PSI_TELEPORT_DECELERATE_TICK      — asm/overworld/teleport/psi_teleport_decelerate_tick.asm
 *   PSI_TELEPORT_SUCCESS_TICK         — asm/overworld/teleport/psi_teleport_success_tick.asm
 *   INIT_TELEPORT_ARRIVAL             — asm/overworld/teleport/init_teleport_arrival.asm
 *   LOAD_TELEPORT_DESTINATION         — asm/overworld/teleport/load_teleport_destination.asm
 *   INIT_TELEPORT_DEPARTURE           — asm/overworld/teleport/init_teleport_departure.asm
 *   TELEPORT_MAINLOOP                 — asm/misc/teleport_mainloop.asm
 *   GET_DIRECTION_FROM_PLAYER_TO_ENTITY — asm/overworld/get_direction_from_player_to_entity.asm
 *   GET_OPPOSITE_DIRECTION_FROM_PLAYER_TO_ENTITY — asm/overworld/get_opposite_direction_from_player_to_entity.asm
 *   CHOOSE_ENTITY_DIRECTION_TO_PLAYER — asm/overworld/entity/choose_entity_direction_to_player.asm
 *   GET_OFF_BICYCLE                   — asm/overworld/get_off_bicycle.asm
 */

#include "game/overworld_internal.h"
#include "game/game_state.h"
#include "game/audio.h"
#include "game/fade.h"
#include "game/position_buffer.h"
#include "game/map_loader.h"
#include "game/window.h"
#include "game/door.h"
#include "game/display_text.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "snes/ppu.h"
#include "core/math.h"
#include "core/memory.h"
#include "include/constants.h"
#include "include/pad.h"
#include "include/binary.h"
#include "data/assets.h"
#include "game_main.h"
#include <math.h>
#include <stdlib.h>
#include "data/text_refs.h"

/* ---- SETUP_TELEPORT_ENTITY_FLAGS (port of asm/overworld/teleport/setup_teleport_entity_flags.asm) ----
 *
 * For entities 24 to MAX_ENTITIES-1:
 *   - Set var3 = 8 (animation speed)
 *   - Set TELEPORTING flag (bit 11) in var7 */
static void setup_teleport_entity_flags(void) {
    for (int slot = PARTY_LEADER_ENTITY_INDEX; slot < MAX_ENTITIES; slot++) {
        entities.var[3][slot] = 8;
        entities.var[7][slot] |= (1 << 11);  /* SPRITE_TABLE_10_FLAGS::TELEPORTING */
    }
}

/* ---- INIT_PSI_TELEPORT_BETA (port of asm/overworld/teleport/init_psi_teleport_beta.asm) ----
 *
 * Called at teleport start. Sets up entity flags and teleport beta state. */
static void init_psi_teleport_beta(void) {
    setup_teleport_entity_flags();

    /* Randomize beta angle (assembly: RAND → XBA → AND #$FF00) */
    ow.psi_teleport_beta_angle = (rand() % 256) << 8;

    if (ow.psi_teleport_style == TELEPORT_STYLE_PSI_BETA) {
        ow.psi_teleport_beta_progress = 4;
    } else {
        ow.psi_teleport_beta_progress = 8;
        ow.psi_teleport_better_progress = 0;
    }

    ow.psi_teleport_beta_x_adjustment = game_state.leader_x_coord;
    ow.psi_teleport_beta_y_adjustment = game_state.leader_y_coord;
}

/* ---- RESET_ENTITIES_AFTER_TELEPORT (port of asm/overworld/teleport/reset_entities_after_teleport.asm) ----
 *
 * For entities 24 to MAX_ENTITIES-1:
 *   - Set var3 = 8
 *   - Clear TELEPORTING flag (bit 11) in var7
 *   - Clear bit 15 of collided_objects
 * Also sets previous_walking_style = -1 for all party character structs,
 * then calls change_music(ml.next_map_music_track). */
static void reset_entities_after_teleport(void) {
    for (int slot = PARTY_LEADER_ENTITY_INDEX; slot < MAX_ENTITIES; slot++) {
        entities.var[3][slot] = 8;
        entities.var[7][slot] &= ~(1 << 11);  /* clear TELEPORTING */
        entities.collided_objects[slot] &= 0x7FFF;  /* clear bit 15 */
    }

    /* Set previous_walking_style = -1 for all party characters */
    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        party_characters[i].previous_walking_style = (uint16_t)-1;
    }

    /* CHANGE_MUSIC_5DD6: port of asm/overworld/change_music_5DD6.asm */
    change_music(ml.next_map_music_track);
}

/* ---- RUN_TELEPORT_FAILURE_SEQUENCE (port of asm/overworld/teleport/run_teleport_failure_sequence.asm) ----
 *
 * Called when PSI Teleport fails (bumps into obstacle).
 * Plays failure music, sets party status to "charred", runs 180 frames
 * with surface/graphics update tick callbacks, then clears status. */
static void run_teleport_failure_sequence(void) {
    ow.disabled_transitions = 1;

    /* Play teleport failure music (MUSIC::TELEPORT_FAIL = 14) */
    change_music(14);

    /* Set var7 bit 15 (WALKING_STYLE_CHANGED) for all party entities */
    for (int slot = PARTY_LEADER_ENTITY_INDEX; slot < MAX_ENTITIES; slot++) {
        entities.var[7][slot] |= SPRITE_TABLE_10_WALKING_STYLE_CHANGED;
    }

    /* Set tick callbacks: leader = NOP, followers = UPDATE_ENTITY_SURFACE_AND_GRAPHICS */
    set_party_tick_callbacks(INIT_ENTITY_SLOT,
                             0xC0E979,  /* TICK_CALLBACK_NOP */
                             0xC0E97C); /* UPDATE_ENTITY_SURFACE_AND_GRAPHICS */

    /* Set party_status = 1 (charred appearance) */
    game_state.party_status = 1;

    /* Wait 180 frames.
     * Assembly: OAM_CLEAR + RUN_ACTIONSCRIPT_FRAME + UPDATE_SCREEN +
     * WAIT_UNTIL_NEXT_FRAME. render_frame_tick() does all four. */
    for (int i = 0; i < 180; i++) {
        render_frame_tick();
    }

    /* Clear party_status and re-enable transitions */
    game_state.party_status = 0;
    ow.disabled_transitions = 0;
}

/* ---- UPDATE_PSI_TELEPORT_SPEED (port of asm/overworld/teleport/update_psi_teleport_speed.asm) ----
 *
 * Updates ow.psi_teleport_speed based on state (accelerate/decelerate/spin),
 * then computes ow.psi_teleport_speed_x/y from direction.
 * Uses 16.16 fixed-point: low 16 = fraction, high 16 = integer. */
static void update_psi_teleport_speed(uint16_t direction) {
    /* Acceleration constants (16.16 fixed-point fractional increments):
     * state=1 (accelerating): normal=0x3333 (~0.20), small=0x051E (~0.02)
     * state=3 (decelerating): normal=0x1999 (~0.10), small=0x1999
     * state=0 (spinning):     normal=0x1851 (~0.095), small=0x29FB (~0.16) */
    int32_t speed = ow.psi_teleport_speed;
    int is_small = (game_state.character_mode == CHARACTER_MODE_SMALL);

    if (ow.psi_teleport_state == 1) {
        /* Accelerating */
        speed += is_small ? 0x051E : 0x3333;
    } else if (ow.psi_teleport_state == 3) {
        /* Decelerating */
        speed -= 0x1999;
    } else {
        /* Spinning (default state 0) */
        speed += is_small ? 0x29FB : 0x1851;
    }
    ow.psi_teleport_speed = speed;
    ow.psi_teleport_speed_int = (uint16_t)(speed >> 16);

    /* Compute X/Y components from direction.
     * Diagonal directions (odd) scale by 1/sqrt(2) ≈ 0xB505/0x10000. */
    if (direction & 1) {
        /* Diagonal: multiply speed by 0xB505, shift right by 16 (two ASR8s) */
        int64_t diag = (int64_t)speed * 0xB505;
        int32_t component = (int32_t)(diag >> 16);
        ow.psi_teleport_speed_x = component;
        ow.psi_teleport_speed_y = component;
    } else {
        /* Cardinal: full speed on one axis */
        ow.psi_teleport_speed_x = speed;
        ow.psi_teleport_speed_y = speed;
    }

    /* Apply direction signs.
     * Convention: UP=0, UP_RIGHT=1, RIGHT=2, DOWN_RIGHT=3,
     *            DOWN=4, DOWN_LEFT=5, LEFT=6, UP_LEFT=7 */
    switch (direction) {
    case 0: /* UP: Y=-speed, X=0 */
        ow.psi_teleport_speed_y = -ow.psi_teleport_speed_y;
        ow.psi_teleport_speed_x = 0;
        break;
    case 4: /* DOWN: Y=+speed, X=0 */
        ow.psi_teleport_speed_x = 0;
        break;
    case 6: /* LEFT: X=-speed, Y=0 */
        ow.psi_teleport_speed_x = -ow.psi_teleport_speed_x;
        ow.psi_teleport_speed_y = 0;
        break;
    case 2: /* RIGHT: X=+speed, Y=0 */
        ow.psi_teleport_speed_y = 0;
        break;
    case 1: /* UP_RIGHT: Y=-speed, X=+speed */
        ow.psi_teleport_speed_y = -ow.psi_teleport_speed_y;
        break;
    case 7: /* UP_LEFT: Y=-speed, X=-speed */
        ow.psi_teleport_speed_y = -ow.psi_teleport_speed_y;
        /* fall through to also negate X */
    case 5: /* DOWN_LEFT: Y=+speed, X=-speed */
        ow.psi_teleport_speed_x = -ow.psi_teleport_speed_x;
        break;
    case 3: /* DOWN_RIGHT: Y=+speed, X=+speed — no changes needed */
        break;
    }
}

/* ---- GET_COMBINED_SURFACE_FLAGS (port of asm/overworld/collision/get_combined_surface_flags.asm) ----
 *
 * Looks up surface flags at both old position (old_x, old_y) and new
 * position (new_x, new_y), ORs the results. Returns 0 during teleport. */
static uint16_t get_combined_surface_flags(
    uint16_t old_x, uint16_t old_y,
    uint16_t new_x, uint16_t new_y, uint16_t direction)
{
    (void)direction;
    if (ow.psi_teleport_state != 0) return 0;

    uint16_t leader_size = entities.sizes[ENT(game_state.current_party_members)];
    uint16_t flags_old = lookup_surface_flags(
        (int16_t)old_x, (int16_t)old_y, leader_size);
    uint16_t flags_new = lookup_surface_flags(
        (int16_t)new_x, (int16_t)new_y, leader_size);
    return flags_old | flags_new;
}

/* ---- RECORD_PLAYER_POSITION (port of asm/overworld/record_player_position.asm) ----
 *
 * Records current leader position to the position buffer at current index,
 * then increments the buffer index (wrapping at 256). */
static void record_player_position(void) {
    uint8_t idx = game_state.position_buffer_index;

    /* player_position_buffer_entry layout:
     * x_coord, y_coord, tile_flags, walking_style, direction */
    PositionBufferEntry *entry = &pb.player_position_buffer[idx];
    entry->x_coord = game_state.leader_x_coord;
    entry->y_coord = game_state.leader_y_coord;
    entry->tile_flags = lookup_surface_flags(
        (int16_t)game_state.leader_x_coord,
        (int16_t)game_state.leader_y_coord,
        entities.sizes[ENT(game_state.current_party_members)]);
    entry->walking_style = 0;
    entry->direction = game_state.leader_direction;

    /* Increment and wrap index */
    game_state.position_buffer_index = (uint8_t)((idx + 1) & 0xFF);
}

/* ---- SET_TELEPORT_ENTITY_SPEED_VARS (port of asm/overworld/teleport/set_teleport_entity_speed_vars.asm) ----
 *
 * Sets entity var3 (animation speed) for party entities 24-28 based on
 * teleport speed. Higher speed → lower var3 (faster animation). */
static void set_teleport_entity_speed_vars(void) {
    int16_t anim_speed = 12 - (int16_t)(ow.psi_teleport_speed >> 16);
    if (anim_speed <= 0) anim_speed = 1;

    for (int slot = PARTY_LEADER_ENTITY_INDEX; slot < 29; slot++) {
        entities.var[3][slot] = anim_speed;
    }
}

/* ---- SET_TELEPORT_VELOCITY_BY_DIRECTION (port of asm/overworld/teleport/set_teleport_velocity_by_direction.asm) ----
 *
 * Sets ow.psi_teleport_speed_x/y integer parts to ±5 based on leader direction.
 * Used when beta/better teleport completes to set initial departure velocity. */
static void set_teleport_velocity_by_direction(void) {
    int16_t sx = 0, sy = 0;
    switch (game_state.leader_direction) {
    case 0: sy = -5; break;                /* UP */
    case 1: sy = -5; sx = 5; break;        /* UP_RIGHT */
    case 2: sx = 5; break;                 /* RIGHT */
    case 3: sy = 5; sx = 5; break;         /* DOWN_RIGHT */
    case 4: sy = 5; break;                 /* DOWN */
    case 5: sy = 5; sx = -5; break;        /* DOWN_LEFT */
    case 6: sx = -5; break;                /* LEFT */
    case 7: sy = -5; sx = -5; break;       /* UP_LEFT */
    }
    ow.psi_teleport_speed_x = (int32_t)sx << 16;
    ow.psi_teleport_speed_y = (int32_t)sy << 16;
}

/* ---- UPDATE_TELEPORT_BETA_INPUT (port of asm/overworld/teleport/update_teleport_beta_input.asm) ----
 *
 * For PSI_BETA style only (not BETTER): adjusts the beta center position
 * by d-pad input, letting the player steer the spiral. */
static void update_teleport_beta_input(void) {
    if (ow.psi_teleport_style == TELEPORT_STYLE_PSI_BETTER) return;

    uint16_t pad = core.pad1_held;
    if (pad & PAD_UP)    ow.psi_teleport_beta_y_adjustment--;
    if (pad & PAD_DOWN)  ow.psi_teleport_beta_y_adjustment++;
    if (pad & PAD_LEFT)  ow.psi_teleport_beta_x_adjustment--;
    if (pad & PAD_RIGHT) ow.psi_teleport_beta_x_adjustment++;
}

/* ---- CALCULATE_VELOCITY_COMPONENTS (port of asm/misc/calculate_velocity_components.asm) ----
 *
 * Computes signed X and Y velocity components from an angle and speed.
 * The assembly uses VELOCITY_X/Y_TABLE (sine/cosine LUTs scaled by 256).
 * We compute equivalent values using trig functions.
 *
 * angle: 16-bit, high byte represents 0-360 degrees in 256 steps.
 * speed: radius/progress value.
 * Returns signed pixel displacements matching the assembly's high-byte extraction. */
void calculate_velocity_components(uint16_t angle, int16_t speed,
                                   int16_t *out_x, int16_t *out_y) {
    /* Discretize to 64 steps matching the assembly table indexing:
     * angle_hi & 0xFC → clear low 2 bits, LSR → byte offset, /2 → entry */
    int entry = ((angle >> 8) & 0xFC) >> 2;  /* 0..63 */
    double angle_rad = entry * (2.0 * M_PI / 64.0);

    /* Assembly chain: table_val (|trig|*256) × speed >> 8 → 16-bit,
     * then extract high byte → signed pixel displacement.
     * Net effect: speed × |trig(angle)| / 256, truncated. */
    double abs_sin = fabs(sin(angle_rad));
    double abs_cos = fabs(cos(angle_rad));

    int16_t x_mag = (int16_t)((abs_sin * (uint16_t)speed) / 256.0);
    int16_t y_mag = (int16_t)((abs_cos * (uint16_t)speed) / 256.0);

    /* Apply quadrant signs (matching assembly byte_offset comparisons):
     * X negated for entries 33-63 (sin < 0, byte_offset >= 0x42)
     * Y negated for entries 0-15 and 49-63 (cos > 0, byte_offset < 0x20 or >= 0x62)
     * Result: dx = speed*sin(angle)/256, dy = -speed*cos(angle)/256
     * (screen coords: +Y = down) */
    *out_x = (entry >= 33) ? (int16_t)-x_mag : x_mag;
    *out_y = (entry < 16 || entry >= 49) ? (int16_t)-y_mag : y_mag;
}

/* ---- UPDATE_TELEPORT_PARTY_VISIBILITY (port of asm/overworld/teleport/update_teleport_party_visibility.asm) ----
 *
 * Updates follower entity position_index during teleport.
 * If the party only has 1 member, just increments position_index.
 * If speed is 0, returns the initial position_index unchanged.
 * Otherwise delegates to adjust_party_member_visibility with spacing=6. */
static uint16_t update_teleport_party_visibility(int16_t char_id,
                                                  uint16_t position_index,
                                                  int16_t entity_offset) {
    uint16_t next_idx = (position_index + 1) & 0xFF;

    /* If party has only this many members, just advance */
    if ((game_state.party_order[0] & 0xFF) == (uint16_t)(char_id + 1)) {
        return next_idx;
    }

    /* If speed is zero, don't advance */
    if ((ow.psi_teleport_speed >> 16) == 0) {
        return position_index;
    }

    /* Delegate to adjust_party_member_visibility with spacing=6 */
    return adjust_party_member_visibility(entity_offset, char_id,
                                          position_index, 6);
}

/* ---- UPDATE_PARTY_ENTITY_FROM_BUFFER (port of asm/overworld/party/update_party_entity_from_buffer.asm) ----
 *
 * Follower tick callback during teleport. Reads position from the position
 * buffer at the entity's char_struct::position_index, updates entity
 * position/direction/surface, and adjusts visibility spacing. */
void update_party_entity_from_buffer(int16_t entity_offset) {
    int16_t char_id = entities.var[1][entity_offset];

    /* Get char_struct pointer */
    CharStruct *cs = &party_characters[char_id];

    /* Read position_index from char_struct */
    uint16_t pos_idx = cs->position_index;

    /* Read var0 (used for update_party_entity_graphics walking_style param) */
    int16_t var0 = entities.var[0][entity_offset];

    /* Look up position buffer entry */
    PositionBufferEntry *entry = &pb.player_position_buffer[pos_idx];

    /* Update party entity graphics from buffer entry.
     * Assembly passes CURRENT_ENTITY_SLOT as Y to UPDATE_PARTY_ENTITY_GRAPHICS,
     * but sets CURRENT_PARTY_MEMBER_TICK = char_id * sizeof(CharStruct) before.
     * The C version uses char_id directly as party_idx. */
    update_party_entity_graphics(var0, entry->walking_style, entity_offset,
                                 char_id);

    /* Copy buffer data to entity */
    entities.abs_x[entity_offset] = entry->x_coord;
    entities.abs_y[entity_offset] = entry->y_coord;
    entities.directions[entity_offset] = entry->direction;
    entities.surface_flags[entity_offset] = entry->tile_flags;

    /* Update visibility/spacing */
    uint16_t new_pos = update_teleport_party_visibility(char_id, pos_idx,
                                                         entity_offset);
    cs->position_index = (uint8_t)(new_pos & 0xFF);
}

/* ---- PSI_TELEPORT_ALPHA_TICK (port of asm/overworld/teleport/psi_teleport_alpha_tick.asm) ----
 *
 * Leader tick callback for PSI Teleport Alpha (straight-line run).
 * Reads d-pad input, updates speed/direction, checks collisions,
 * moves leader, updates camera. Sets state=2 on collision, state=1 when
 * speed reaches 9+. */
void psi_teleport_alpha_tick(int16_t entity_offset) {
    (void)entity_offset;
    game_state.leader_moved = 1;

    /* Read d-pad with walking_style=0 */
    int16_t input_dir = map_input_to_direction(0);

    /* Don't allow reversing direction */
    int16_t old_dir = (int16_t)game_state.leader_direction;
    int16_t reverse = input_dir ^ 4;
    if (old_dir == reverse) {
        input_dir = old_dir;
    }

    /* No input: keep current direction */
    if (input_dir == -1) {
        input_dir = old_dir;
    }

    game_state.leader_direction = (uint16_t)input_dir;

    /* Check for battle swirl */
    if (ow.battle_swirl_countdown != 0) {
        ow.psi_teleport_state = 2;
        ow.battle_mode = 1;
    }

    /* Update speed and velocity components */
    update_psi_teleport_speed((uint16_t)input_dir);

    /* Compute predicted next position (16.16 fixed-point) */
    int32_t next_x = (int32_t)(((uint32_t)game_state.leader_x_coord << 16) |
                     (uint16_t)game_state.leader_x_frac);
    next_x += ow.psi_teleport_speed_x;
    ow.psi_teleport_next_x = next_x;

    int32_t next_y = (int32_t)(((uint32_t)game_state.leader_y_coord << 16) |
                     (uint16_t)game_state.leader_y_frac);
    next_y += ow.psi_teleport_speed_y;
    ow.psi_teleport_next_y = next_y;

    /* NPC collision check */
    int16_t next_x_int = (int16_t)(ow.psi_teleport_next_x >> 16);
    int16_t next_y_int = (int16_t)(ow.psi_teleport_next_y >> 16);
    if (npc_collision_check(next_x_int, next_y_int,
                            game_state.current_party_members) != -1) {
        ow.psi_teleport_state = 2;
    }

    /* Surface collision check */
    uint16_t surface = get_combined_surface_flags(
        game_state.leader_x_coord, game_state.leader_y_coord,
        (uint16_t)next_x_int, (uint16_t)next_y_int,
        game_state.leader_direction);
    if (surface & 0x00C0) {
        ow.psi_teleport_state = 2;
    }

    /* If no collision, update leader position */
    if (ow.psi_teleport_state != 2) {
        game_state.leader_x_frac = (uint16_t)(ow.psi_teleport_next_x & 0xFFFF);
        game_state.leader_x_coord = (uint16_t)(ow.psi_teleport_next_x >> 16);
        game_state.leader_y_frac = (uint16_t)(ow.psi_teleport_next_y & 0xFFFF);
        game_state.leader_y_coord = (uint16_t)(ow.psi_teleport_next_y >> 16);
    }

    /* Center camera on leader */
    center_screen(game_state.leader_x_coord, game_state.leader_y_coord);
    record_player_position();
    set_teleport_entity_speed_vars();

    /* Transition to STATE_ARRIVED when speed >= 9 */
    if ((int16_t)(ow.psi_teleport_speed >> 16) > 9) {
        ow.psi_teleport_state = 1;
    }
}

/* ---- PSI_TELEPORT_BETA_TICK (port of asm/overworld/teleport/psi_teleport_beta_tick.asm) ----
 *
 * Leader tick callback for PSI Teleport Beta/Better (spiral motion).
 * Computes position from angle+progress using velocity tables,
 * checks collisions, updates direction from angle, updates speed/angle. */
void psi_teleport_beta_tick(int16_t entity_offset) {
    (void)entity_offset;
    game_state.leader_moved = 1;

    /* For BETA: allow d-pad adjustment of spiral center */
    update_teleport_beta_input();

    /* Calculate velocity components from angle and progress */
    int16_t vel_x, vel_y;
    calculate_velocity_components(ow.psi_teleport_beta_angle,
                                  (int16_t)ow.psi_teleport_beta_progress,
                                  &vel_x, &vel_y);

    /* Set predicted next position from components + adjustment */
    int16_t next_x = vel_x + (int16_t)ow.psi_teleport_beta_x_adjustment;
    ow.psi_teleport_next_x = (int32_t)next_x << 16;

    int16_t next_y = vel_y + (int16_t)ow.psi_teleport_beta_y_adjustment;
    ow.psi_teleport_next_y = (int32_t)next_y << 16;

    /* For non-BETTER: check surface and NPC collisions */
    if (ow.psi_teleport_style != TELEPORT_STYLE_PSI_BETTER) {
        uint16_t surface = get_combined_surface_flags(
            game_state.leader_x_coord, game_state.leader_y_coord,
            (uint16_t)next_x, (uint16_t)next_y,
            game_state.leader_direction);
        if (surface & 0x00C0) {
            ow.psi_teleport_state = 2;
        }

        if (ow.battle_swirl_countdown != 0) {
            ow.psi_teleport_state = 2;
            ow.battle_mode = 1;
        }

        if (npc_collision_check(next_x, next_y,
                                game_state.current_party_members) != -1) {
            ow.psi_teleport_state = 2;
        }
    }

    /* If no collision, update leader position */
    if (ow.psi_teleport_state != 2) {
        game_state.leader_x_coord = (uint16_t)next_x;
        game_state.leader_y_coord = (uint16_t)next_y;
    }

    /* Derive direction from angle (top 3 bits of angle, +2, masked to 0-7) */
    game_state.leader_direction = (uint16_t)(((ow.psi_teleport_beta_angle >> 13) + 2) & 7);

    /* Increment speed (16.16 fixed-point: add ~0.095 per frame) */
    ow.psi_teleport_speed += 0x1851;

    /* Update angle and progress based on style */
    if (ow.psi_teleport_style == TELEPORT_STYLE_PSI_BETA) {
        ow.psi_teleport_beta_angle += 0x0A00;
        ow.psi_teleport_beta_progress += 12;
    } else {
        /* PSI_BETTER: variable angle increment */
        ow.psi_teleport_better_progress += 32;
        ow.psi_teleport_beta_angle += ow.psi_teleport_better_progress;
        ow.psi_teleport_beta_progress += 16;
    }

    /* Center camera and update position buffer */
    center_screen(game_state.leader_x_coord, game_state.leader_y_coord);
    record_player_position();
    set_teleport_entity_speed_vars();

    /* Check completion thresholds */
    if (ow.psi_teleport_style == TELEPORT_STYLE_PSI_BETA) {
        if ((int16_t)ow.psi_teleport_beta_progress > 0x1000) {
            ow.psi_teleport_state = 1;
            set_teleport_velocity_by_direction();
        }
    } else {
        if ((int16_t)ow.psi_teleport_better_progress > 0x1800) {
            ow.psi_teleport_state = 1;
            set_teleport_velocity_by_direction();
        }
    }
}

/* ---- PSI_TELEPORT_DECELERATE_TICK (port of asm/overworld/teleport/psi_teleport_decelerate_tick.asm) ----
 *
 * Leader tick callback for arrival deceleration. Updates speed (decreasing),
 * applies velocity to position, centers camera with offset. */
void psi_teleport_decelerate_tick(int16_t entity_offset) {
    (void)entity_offset;

    /* Update speed (decelerating: state=3 subtracts 0x1999 per frame) */
    update_psi_teleport_speed(game_state.leader_direction);

    /* Apply velocity to leader position (16.16 fixed-point) */
    int32_t x_pos = (int32_t)(((uint32_t)game_state.leader_x_coord << 16) |
                    (uint16_t)game_state.leader_x_frac);
    x_pos += ow.psi_teleport_speed_x;
    game_state.leader_x_frac = (uint16_t)(x_pos & 0xFFFF);
    game_state.leader_x_coord = (uint16_t)(x_pos >> 16);

    int32_t y_pos = (int32_t)(((uint32_t)game_state.leader_y_coord << 16) |
                    (uint16_t)game_state.leader_y_frac);
    y_pos += ow.psi_teleport_speed_y;
    game_state.leader_y_frac = (uint16_t)(y_pos & 0xFFFF);
    game_state.leader_y_coord = (uint16_t)(y_pos >> 16);

    /* Camera offset: center_screen(x - speed*2, y)
     * Assembly: MULT32(speed, 2) → subtract integer part from x */
    int32_t doubled_speed = ow.psi_teleport_speed * 2;
    int16_t speed_offset = (int16_t)(doubled_speed >> 16);
    center_screen((uint16_t)((int16_t)game_state.leader_x_coord - speed_offset),
                  game_state.leader_y_coord);

    record_player_position();
    set_teleport_entity_speed_vars();
}

/* ---- PSI_TELEPORT_SUCCESS_TICK (port of asm/overworld/teleport/psi_teleport_success_tick.asm) ----
 *
 * Leader tick callback for success screen animation (after arrival confirmed).
 * Applies velocity to position, scrolls screen via success_screen vars. */
void psi_teleport_success_tick(int16_t entity_offset) {
    (void)entity_offset;

    /* Update speed */
    update_psi_teleport_speed(game_state.leader_direction);

    /* Apply velocity to leader position */
    int32_t x_pos = (int32_t)(((uint32_t)game_state.leader_x_coord << 16) |
                    (uint16_t)game_state.leader_x_frac);
    x_pos += ow.psi_teleport_speed_x;
    game_state.leader_x_frac = (uint16_t)(x_pos & 0xFFFF);
    game_state.leader_x_coord = (uint16_t)(x_pos >> 16);

    int32_t y_pos = (int32_t)(((uint32_t)game_state.leader_y_coord << 16) |
                    (uint16_t)game_state.leader_y_frac);
    y_pos += ow.psi_teleport_speed_y;
    game_state.leader_y_frac = (uint16_t)(y_pos & 0xFFFF);
    game_state.leader_y_coord = (uint16_t)(y_pos >> 16);

    /* Update success screen position */
    ow.psi_teleport_success_screen_x += ow.psi_teleport_success_screen_speed_x;
    ow.psi_teleport_success_screen_y += ow.psi_teleport_success_screen_speed_y;

    /* Center camera on success screen position (not leader position) */
    center_screen((uint16_t)ow.psi_teleport_success_screen_x,
                  (uint16_t)ow.psi_teleport_success_screen_y);

    record_player_position();
}

/* ---- INIT_TELEPORT_ARRIVAL (port of asm/overworld/teleport/init_teleport_arrival.asm) ----
 *
 * For INSTANT style: complete no-op (early return).
 * For other styles: disables collision on entities 24+, zeroes speed vars,
 * sets PSI_TELEPORT_SUCCESS_TICK callback, fades out. */
static void init_teleport_arrival(void) {
    if (ow.psi_teleport_style == TELEPORT_STYLE_INSTANT)
        return;

    /* Non-INSTANT: disable collision for entities 24+ */
    for (int slot = PARTY_LEADER_ENTITY_INDEX; slot < MAX_ENTITIES; slot++) {
        entities.collided_objects[slot] = ENTITY_COLLISION_DISABLED;
    }

    /* Zero speed X/Y integer parts (assembly lines 27-33) */
    ow.psi_teleport_speed_x &= 0x0000FFFF;  /* clear integer, keep fraction */
    ow.psi_teleport_speed_y &= 0x0000FFFF;

    /* Set SUCCESS_TICK callback for leader, UPDATE_PARTY_ENTITY_FROM_BUFFER
     * for followers (assembly lines 34-37) */
    set_party_tick_callbacks(INIT_ENTITY_SLOT,
                             0xC0E674,  /* PSI_TELEPORT_SUCCESS_TICK */
                             0xC0E3C1); /* UPDATE_PARTY_ENTITY_FROM_BUFFER */

    /* Store current speed/position for success screen animation (lines 38-47) */
    ow.psi_teleport_success_screen_speed_x = (int16_t)(ow.psi_teleport_speed_x >> 16);
    ow.psi_teleport_success_screen_x = (int16_t)game_state.leader_x_coord;
    ow.psi_teleport_success_screen_speed_y = (int16_t)(ow.psi_teleport_speed_y >> 16);
    ow.psi_teleport_success_screen_y = (int16_t)game_state.leader_y_coord;

    fade_out(1, 4);
    run_frames_until_fade_done();
}

/* ---- LOAD_TELEPORT_DESTINATION (port of asm/overworld/teleport/load_teleport_destination.asm) ----
 *
 * Clears event flags 1-10, reads destination coords from PSI_TELEPORT_DEST_TABLE,
 * resets map state, and calls initialize_map. */
static void load_teleport_destination(void) {
    /* Clear event flags 1-10 */
    for (uint16_t flag = 1; flag <= 10; flag++) {
        event_flag_clear(flag);
    }

    /* Load PSI teleport destination table */
    const uint8_t *table = ASSET_DATA(ASSET_DATA_PSI_TELEPORT_DEST_TABLE_BIN);
    if (!table) return;

    /* Read destination entry: name[25] + event_flag[2] + dest_x[2] + dest_y[2] = 31 bytes */
    uint16_t dest = ow.psi_teleport_destination;
    const uint8_t *entry = table + (uint32_t)dest * PSI_TELEPORT_DEST_ENTRY_SIZE;

    /* Read dest_x and dest_y (little-endian 16-bit, at offsets 27 and 29).
     * Entry layout: name[25] + event_flag[2] + dest_x[2] + dest_y[2]. */
    uint16_t dest_x = read_u16_le(entry + PSI_TELEPORT_DEST_NAME_LEN + 2);
    uint16_t dest_y = read_u16_le(entry + PSI_TELEPORT_DEST_NAME_LEN + 4);

    ow.current_teleport_destination_x = dest_x;
    ow.current_teleport_destination_y = dest_y;

    /* Convert to pixel coordinates (tile coords * 8) */
    uint16_t pixel_x = dest_x * 8;
    uint16_t pixel_y = dest_y * 8;

    /* Non-INSTANT styles add 316 to x (for off-screen arrival) */
    if (ow.psi_teleport_style != TELEPORT_STYLE_INSTANT) {
        pixel_x += 316;
    }

    /* Reset map tracking state to force full reload */
    ml.current_map_music_track = (uint16_t)-1;
    ow.loaded_map_palette = -1;
    ow.loaded_map_tile_combo = -1;

    /* Load the map at destination with direction = LEFT (6) */
    initialize_map(pixel_x, pixel_y, 6);
}

/* ---- INIT_TELEPORT_DEPARTURE (port of asm/overworld/teleport/init_teleport_departure.asm) ----
 *
 * For INSTANT style: center screen, fade in, wait for fade.
 * For other styles: updates party graphics, sets speed/direction,
 * plays TELEPORT_IN music, waits 30 frames, fades in, animates. */
static void init_teleport_departure(void) {
    if (ow.psi_teleport_style == TELEPORT_STYLE_INSTANT) {
        center_screen(game_state.leader_x_coord, game_state.leader_y_coord);
        fade_in(1, 1);
        run_frames_until_fade_done();
        return;
    }

    /* Non-INSTANT: update all party member graphics */
    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        int char_slot = i + PARTY_LEADER_ENTITY_INDEX;
        party_characters[i].previous_walking_style = (uint16_t)-1;

        uint8_t char_id = game_state.party_order[i];
        if (char_id == 0) continue;
        update_party_entity_graphics((uint16_t)(char_id - 1), 0,
                                     char_slot, 0);
    }

    /* Set speed = 0x00080000 (8.0 in 16.16 fixed-point), direction = LEFT (6), state = 3 */
    ow.psi_teleport_speed = 0x00080000;
    ow.psi_teleport_speed_int = 8;
    game_state.leader_direction = 6;
    ow.psi_teleport_state = 3;

    /* Set DECELERATE_TICK callback for leader, UPDATE_PARTY_ENTITY_FROM_BUFFER
     * for followers (assembly lines 62-65) */
    set_party_tick_callbacks(INIT_ENTITY_SLOT,
                             0xC0E776,  /* PSI_TELEPORT_DECELERATE_TICK */
                             0xC0E3C1); /* UPDATE_PARTY_ENTITY_FROM_BUFFER */
    setup_teleport_entity_flags();

    /* Play TELEPORT_IN music */
    change_music(135);  /* MUSIC::TELEPORT_IN */

    /* Wait 30 frames */
    for (int i = 0; i < 30; i++) {
        render_frame_tick();
    }

    /* Fade in */
    fade_in(1, 4);

    /* Animate until speed reaches 0 */
    while ((ow.psi_teleport_speed >> 16) != 0) {
        oam_clear();
        run_actionscript_frame();
        update_screen();
        render_frame_tick();
    }

    center_screen(game_state.leader_x_coord, game_state.leader_y_coord);
}

/* ---- TELEPORT_MAINLOOP (port of asm/misc/teleport_mainloop.asm) ----
 *
 * Main teleport event loop. Handles all teleport styles.
 * For INSTANT: stops music, freezes entities, loads destination, fades in.
 * For other styles: plays departure animation, fades, loads, plays arrival. */
void teleport_mainloop(void) {
    /* Assembly line 8: stop music */
    stop_music();
    render_frame_tick();

    /* Assembly line 10: freeze NPC entities */
    teleport_freeze_entities();

    /* Assembly line 13-14: clear teleport speed and state */
    ow.psi_teleport_speed = 0;
    ow.psi_teleport_speed_int = 0;
    ow.psi_teleport_state = 0;

    /* Assembly line 15-16: clear party hide flags and init beta state */
    clear_party_sprite_hide_flags();
    init_psi_teleport_beta();

    /* Style-based tick callback setup (assembly lines 17-49) */
    switch (ow.psi_teleport_style) {
    case TELEPORT_STYLE_PSI_ALPHA:
    case TELEPORT_STYLE_STAR_MASTER:
        set_party_tick_callbacks(INIT_ENTITY_SLOT,
                                 0xC0E28F,  /* PSI_TELEPORT_ALPHA_TICK */
                                 0xC0E3C1); /* UPDATE_PARTY_ENTITY_FROM_BUFFER */
        break;
    case TELEPORT_STYLE_PSI_BETA:
        set_party_tick_callbacks(INIT_ENTITY_SLOT,
                                 0xC0E516,  /* PSI_TELEPORT_BETA_TICK */
                                 0xC0E3C1); /* UPDATE_PARTY_ENTITY_FROM_BUFFER */
        break;
    case TELEPORT_STYLE_INSTANT:
        /* Skip tick callbacks entirely — go straight to arrived state */
        ow.psi_teleport_state = 1;
        break;
    case TELEPORT_STYLE_PSI_BETTER:
        set_party_tick_callbacks(INIT_ENTITY_SLOT,
                                 0xC0E516,  /* PSI_TELEPORT_BETA_TICK */
                                 0xC0E3C1); /* UPDATE_PARTY_ENTITY_FROM_BUFFER */
        break;
    }

    /* Play teleport music (skip for INSTANT) */
    if (ow.psi_teleport_style != TELEPORT_STYLE_INSTANT) {
        change_music(13);  /* MUSIC::TELEPORT_OUT */
    }

    /* Teleport loop: run animation frames until state changes (lines 57-65) */
    while (ow.psi_teleport_state == 0) {
        oam_clear();
        run_actionscript_frame();
        teleport_freeze_entities_conditional();
        update_screen();
        render_frame_tick();
    }

    /* Dispatch based on state */
    if (ow.psi_teleport_state == 1) {
        /* STATE_ARRIVED: successful teleport */
        init_teleport_arrival();
        load_teleport_destination();
        init_teleport_departure();

        /* Style STAR_MASTER (5): display master teleport text */
        if (ow.psi_teleport_style == TELEPORT_STYLE_STAR_MASTER) {
            freeze_and_queue_text_interaction(MSG_EVT_MASTER_TLPT_SNES_ADDR);
        }
    } else if (ow.psi_teleport_state == 2) {
        /* STATE_FAILED: teleport failure sequence */
        run_teleport_failure_sequence();
        /* Assembly: LDA #10; JSL WAIT_FRAMES_WITH_UPDATES */
        wait_frames_with_updates(10);
    }

    /* CLEANUP (assembly lines 86-97):
     * Restore normal tick callbacks, reset entities, re-enable all. */
    set_party_tick_callbacks(INIT_ENTITY_SLOT,
                             0xC05200,  /* UPDATE_OVERWORLD_FRAME */
                             0xC04D78); /* UPDATE_FOLLOWER_STATE */
    reset_entities_after_teleport();
    enable_all_entities();

    /* Clear teleport state */
    ow.psi_teleport_speed = 0;
    ow.psi_teleport_speed_int = 0;
    ow.player_intangibility_frames = 0;
    ow.psi_teleport_destination = 0;
}

/* ---- GET_DIRECTION_FROM_PLAYER_TO_ENTITY ----
 * (port of asm/overworld/get_direction_from_player_to_entity.asm)
 *
 * Computes the 8-way direction from the current entity toward the leader.
 * Despite the name, the assembly passes entity coords as "from" and leader
 * coords as "to" to GET_DIRECTION_TO.
 *
 * Assembly register trace at JSL GET_DIRECTION_TO:
 *   A = entity_abs_x (x1), X = entity_abs_y (y1),
 *   Y = leader_x_coord (x2), stack(@LOCAL00) = leader_y_coord (y2).
 * GET_DIRECTION_TO params: A=x1, X=y1, Y=x2, stack=y2.
 * Result: direction from entity toward leader. */
int16_t get_direction_from_player_to_entity(void) {
    int16_t slot = ert.current_entity_slot;
    int16_t entity_x = entities.abs_x[slot];
    int16_t entity_y = entities.abs_y[slot];
    int16_t leader_x = (int16_t)game_state.leader_x_coord;
    int16_t leader_y = (int16_t)game_state.leader_y_coord;

    return calculate_direction_8(entity_x, entity_y, leader_x, leader_y);
}

/* ---- GET_OPPOSITE_DIRECTION_FROM_PLAYER_TO_ENTITY ----
 * (port of asm/overworld/get_opposite_direction_from_player_to_entity.asm)
 *
 * Calls GET_DIRECTION_FROM_PLAYER_TO_ENTITY, then looks up the opposite
 * direction from the OPPOSITE_DIRECTIONS table. */
static const uint16_t opposite_directions[] = {
    4, 5, 6, 7, 0, 1, 2, 3  /* DOWN, DOWN_LEFT, LEFT, UP_LEFT, UP, UP_RIGHT, RIGHT, DOWN_RIGHT */
};

int16_t get_opposite_direction_from_player_to_entity(void) {
    int16_t dir = get_direction_from_player_to_entity();
    if (dir < 0 || dir > 7) return 0;
    return (int16_t)opposite_directions[dir];
}

/* ---- CHOOSE_ENTITY_DIRECTION_TO_PLAYER ----
 * (port of asm/overworld/entity/choose_entity_direction_to_player.asm)
 *
 * If the enemy should flee (CHECK_ENEMY_SHOULD_FLEE returns nonzero),
 * returns the direction AWAY from the player (opposite direction).
 * Otherwise returns the direction TOWARD the player. */
int16_t choose_entity_direction_to_player(void) {
    if (check_enemy_should_flee())
        return get_opposite_direction_from_player_to_entity();
    else
        return get_direction_from_player_to_entity();
}

/* ---- GET_OFF_BICYCLE (port of asm/overworld/get_off_bicycle.asm) ----
 *
 * Displays the "got off the bicycle" message, then calls DISMOUNT_BICYCLE.
 * Assembly:
 *   CREATE_WINDOW(WINDOW::TEXT_STANDARD)
 *   SET_WORKING_MEMORY(1)
 *   DISPLAY_TEXT_PTR MSG_SYS_BICYCLE_OFF
 *   CLOSE_FOCUS_WINDOW
 *   WINDOW_TICK
 *   DISMOUNT_BICYCLE */
void get_off_bicycle_with_message(void) {
    /* Port of asm/overworld/get_off_bicycle.asm.
     * Shows "got off the bicycle" message, then dismounts. */
    create_window(0x01);  /* WINDOW::TEXT_STANDARD */
    set_working_memory(1);
    display_text_from_addr(MSG_SYS_BIKE_GOT_OFF);
    close_focus_window();
    window_tick();
    dismount_bicycle();
}
