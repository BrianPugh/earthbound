/*
 * Callroutine movement functions — speed, direction, velocity, movement loops.
 * Extracted from callroutine.c.
 */
#include "entity/callroutine_internal.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "game/display_text.h"
#include "game/audio.h"
#include "game/overworld.h"
#include "game/overworld_internal.h"
#include "game/position_buffer.h"
#include "game/window.h"
#include "game/fade.h"
#include "game/ending.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/constants.h"
#include "core/math.h"
#include "core/memory.h"
#include "data/assets.h"
#include "game_main.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* FOOTSTEP_SOUND_TABLE — 10 entries x 2 bytes (uint16_t), maps terrain type to SFX ID.
 * Loaded from data/footstep_sound_table.bin (extracted by ebtools from ROM C40BD4).
 * Indexed by ow.footstep_sound_id/2 (ow.footstep_sound_id is pre-multiplied by 2). */
static const uint8_t *footstep_sound_table;

/* ---- Movement helpers ---- */

/*
 * Set entity velocity from 8-way direction and movement speed.
 *
 * Port of SET_ENTITY_VELOCITY_FROM_DIRECTION (C47044).
 * The ROM uses CALCULATE_VELOCITY_COMPONENTS which does a sin/cos table lookup and
 * multiplies by speed. We approximate with fixed-point sin/cos.
 *
 * Speed is in 8.8 fixed-point (e.g., 0x0140 = 1.25 pixels/frame).
 * The velocity is split into integer (delta_x) and fractional (delta_frac_x).
 */
void set_velocity_from_direction(int16_t ent, int16_t direction) {
    int32_t speed = entities.movement_speeds[ent];
    uint16_t fine_dir;

    /* Accept both 8-way (0-7) and fine (0x0000-0xFFFF) directions.
     * Values 0-7 are converted to fine direction (0x2000 per step). */
    if ((uint16_t)direction <= 7)
        fine_dir = (uint16_t)direction * 0x2000;
    else
        fine_dir = (uint16_t)direction;

    /* Fine direction: 0=UP, CW, 0x2000 per 45°.
     * vx = speed * sin(angle), vy = -speed * cos(angle)
     * where angle = fine_dir * 2π / 65536 */
    double angle = (double)fine_dir * (2.0 * M_PI / 65536.0);
    int32_t vx = (int32_t)(sin(angle) * (double)speed);
    int32_t vy = (int32_t)(-cos(angle) * (double)speed);

    /* Split into integer (delta) and fractional (delta_frac) parts.
     * Assembly's CALCULATE_VELOCITY_COMPONENTS returns 16-bit where
     * high byte = integer, low byte = fraction. SET_ENTITY_VELOCITY_FROM_DIRECTION
     * does ASR 8 for integer part and ASL16 8 for fraction. */
    entities.delta_x[ent] = (int16_t)(vx >> 8);
    entities.delta_frac_x[ent] = (uint16_t)((vx & 0xFF) << 8);
    entities.delta_y[ent] = (int16_t)(vy >> 8);
    entities.delta_frac_y[ent] = (uint16_t)((vy & 0xFF) << 8);
}

/*
 * Quantize a direction to 8-way (0-7).
 * Accepts both 8-way (0-7) and fine 16-bit directions.
 */
int16_t quantize_direction(int16_t raw_dir) {
    if ((uint16_t)raw_dir <= 7)
        return raw_dir;
    return (int16_t)(((uint16_t)raw_dir + 0x1000) / 0x2000) & 7;
}

/*
 * Quantize a fine direction (0x0000-0xFFFF, 0x2000 per step) to 8-way SNES direction.
 * Port of QUANTIZE_DIRECTION (C46B51).
 *
 * Fine direction convention: 0x0000=RIGHT, counter-clockwise in 0x2000 steps.
 * SNES direction convention: 0=UP, 1=UP_RIGHT, 2=RIGHT, etc.
 * The QUANTIZED_ANGLE_TO_DIRECTION_TABLE (C46B41) maps between them.
 */
int16_t quantize_fine_direction(uint16_t fine_dir) {
    static const int16_t angle_to_dir[8] = {2, 3, 4, 5, 6, 7, 7, 1};
    uint16_t index = (uint16_t)(fine_dir + 0x1000) / 0x2000;
    return angle_to_dir[index & 7];
}

/*
 * Quantize a fine direction to raw index (0-7) and store to moving_directions.
 * Port of QUANTIZE_ENTITY_DIRECTION (C46B0A).
 * Unlike quantize_fine_direction(), this does NOT apply the angle-to-SNES table.
 */
int16_t quantize_entity_direction(int16_t entity_offset, uint16_t fine_dir) {
    int16_t raw = (int16_t)((uint16_t)(fine_dir + 0x1000) / 0x2000);
    entities.moving_directions[entity_offset] = raw;
    return raw;
}

/* Clamp a signed value to [0, max]. Port of CLAMP_VALUE_TO_RANGE. */
int16_t clamp_to_range(int16_t val, int16_t max) {
    if (val < 0) return 0;
    if (val > max) return max;
    return val;
}

/* update_bubble_monkey_mode — Port of UPDATE_BUBBLE_MONKEY_MODE
 * (asm/misc/update_bubble_monkey_mode.asm).
 * Transitions bubble monkey between behavioral modes:
 *   0 = normal following, 1 = hiding, 2 = returning, 3 = distracted.
 * Called when movement_change_timer reaches 0. */
void update_bubble_monkey_mode(int16_t char_id, uint16_t position_index) {
    uint16_t timer_mode = 0;  /* used for timer calculation */

    if (ow.bubble_monkey_mode == 3 || ow.bubble_monkey_mode == 1) {
        /* Returning from distracted or hide → mode 2 (returning) */
        ow.bubble_monkey_mode = 2;
        timer_mode = 2;
    } else {
        /* Check distance to party member */
        uint16_t distance = get_distance_to_party_member(char_id, position_index);
        if (distance > 40) {
            ow.bubble_monkey_mode = 2;
            timer_mode = 2;
        } else {
            /* Random mode 0-3 */
            timer_mode = rng_next_byte() & 0x03;
            ow.bubble_monkey_mode = timer_mode;
        }
    }

    /* Timer = mode * 3 + 4 */
    ow.bubble_monkey_movement_change_timer = (timer_mode & 0x03) * 3 + 4;
}

/* ---- cr_* callroutine functions ---- */

/*
 * SET_ENTITY_DIRECTION_VELOCITY (C0C83B)
 *
 * Port of SET_ENTITY_DIRECTION_VELOCITY / C0C83B.
 * Sets velocity from movement speed and direction (cardinal or diagonal).
 * Diagonal directions (odd) scale by 1/√2 = $B505/$10000.
 * Math: result = (speed * scale) >> 8, giving 16.16 fixed-point velocity.
 */
int16_t cr_set_entity_direction_velocity(int16_t ent, int16_t scr,
                                                 uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    int16_t direction = scripts.tempvar[scr];
    int16_t offset = ENT(ert.current_entity_slot);

    /* Store moving direction (ENTITY_MOVING_DIRECTIONS, NOT ENTITY_DIRECTIONS).
     * Assembly: STA ENTITY_MOVING_DIRECTIONS,X — this is the movement direction
     * used for velocity computation and battle initiative calculation.
     * ENTITY_DIRECTIONS (visual/sprite direction) is set separately by
     * UPDATE_ENTITY_ANIMATION or callroutines like SET_DIRECTION. */
    entities.moving_directions[offset] = direction;

    /* Compute scale: diagonal (odd dir) = 1/√2, cardinal (even) = 1.0 */
    uint32_t scale = (direction & 1) ? 0xB505 : 0x10000;

    /* speed × scale >> 8 → 16.16 fixed-point */
    uint32_t speed = (uint32_t)entities.movement_speeds[offset];
    uint32_t result = (speed * scale) >> 8;

    /* Direction-to-velocity switch (SNES convention, from assembly):
     * 0=UP: dx=0, dy=-result; 1=UP_RIGHT: dx=+result, dy=-result
     * 2=RIGHT: dx=+result, dy=0; 3=DOWN_RIGHT: dx=+result, dy=+result
     * 4=DOWN: dx=0, dy=+result; 5=DOWN_LEFT: dx=-result, dy=+result
     * 6=LEFT: dx=-result, dy=0; 7=UP_LEFT: dx=-result, dy=-result */
    int32_t dx32 = 0, dy32 = 0;
    switch (direction & 7) {
    case 0: dy32 = -(int32_t)result; break;
    case 1: dx32 = (int32_t)result; dy32 = -(int32_t)result; break;
    case 2: dx32 = (int32_t)result; break;
    case 3: dx32 = (int32_t)result; dy32 = (int32_t)result; break;
    case 4: dy32 = (int32_t)result; break;
    case 5: dx32 = -(int32_t)result; dy32 = (int32_t)result; break;
    case 6: dx32 = -(int32_t)result; break;
    case 7: dx32 = -(int32_t)result; dy32 = -(int32_t)result; break;
    }

    /* Store as 16.16 fixed-point (integer + fraction) */
    entities.delta_x[offset] = (int16_t)(dx32 >> 16);
    entities.delta_frac_x[offset] = (uint16_t)(dx32 & 0xFFFF);
    entities.delta_y[offset] = (int16_t)(dy32 >> 16);
    entities.delta_frac_y[offset] = (uint16_t)(dy32 & 0xFFFF);
    return 0;
}

/*
 * UPDATE_ENTITY_ANIMATION (C0A6E3)
 *
 * Port of UPDATE_ENTITY_ANIMATION tick callback from C0A6E3.asm.
 * Checks animation fingerprint (walking_style << 8 | direction) for changes,
 * then runs walk animation countdown (var2/var3 → toggle frame 0↔2).
 */
int16_t cr_update_entity_animation(int16_t ent, int16_t scr,
                                           uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;

    /* Mark entity as using 8-directional sprite mapping.
     * In the assembly, UPDATE_ENTITY_ANIMATION always calls
     * RENDER_ENTITY_SPRITE_8DIR (C0A794) which uses the 8-direction
     * mapping table. The draw callback reads this flag. */
    entities.use_8dir_sprites[ent] = 1;

    /* Fingerprint check: if direction or walking_style changed, re-render.
     * Assembly lines 2-12: early return (RTL), no intangibility check. */
    uint16_t fingerprint = (entities.walking_styles[ent] << 8) |
                           (entities.directions[ent] & 0xFF);
    if (fingerprint != entities.animation_fingerprints[ent]) {
        entities.animation_fingerprints[ent] = fingerprint;
        render_entity_sprite(ent);
        return 0;
    }

    /* Check var7 bit 15 (WALKING_STYLE_CHANGED): clear and re-render.
     * Assembly lines 14-18. Falls through to intangibility check. */
    if (entities.var[7][ent] & 0x8000) {
        entities.var[7][ent] &= ~0x8000;
        render_entity_sprite(ent);
        goto check_intangibility;
    }

    /* Check var7 bit 13 (FORCE_STATIC): reset frame to 0 if needed.
     * Assembly lines 19-25. */
    if (entities.var[7][ent] & 0x2000) {
        if (entities.animation_frame[ent] != 0) {
            entities.animation_frame[ent] = 0;
            render_entity_sprite(ent);
        }
        goto check_intangibility;
    }

    /* Assembly lines 27-28: skip animation tick during battle swirl */
    if (ow.battle_swirl_countdown != 0)
        goto check_intangibility;

    /* Animation countdown: decrement var2, toggle frame on underflow/zero.
     * Assembly lines 29-37. */
    entities.var[2][ent]--;
    if (entities.var[2][ent] <= 0) {
        entities.var[2][ent] = entities.var[3][ent];  /* reload period */
        entities.animation_frame[ent] ^= 2;           /* toggle frame 0↔2 */

        /* Render unconditionally after frame toggle (assembly calls RENDER_ENTITY_SPRITE_8DIR
         * regardless of whether new frame is 0 or 2; BNE only skips the footstep sound). */
        render_entity_sprite(ent);

        /* Footstep sound: play when frame toggles to 0 and entity is the leader.
         * Assembly lines 38-49. */
        if (entities.animation_frame[ent] == 0 &&
            ent == ow.footstep_sound_ignore_entity) {
            uint16_t idx = ow.footstep_sound_id_override ? ow.footstep_sound_id_override
                                                      : ow.footstep_sound_id;
            /* Lazy-load footstep sound table */
            if (!footstep_sound_table)
                footstep_sound_table = ASSET_DATA(ASSET_DATA_FOOTSTEP_SOUND_TABLE_BIN);
            if (footstep_sound_table && idx < 20) {
                uint16_t sfx = read_u16_le(&footstep_sound_table[idx]);
                if (sfx != 0 && ow.disabled_transitions == 0)
                    play_sfx(sfx);
            }
        }
    }

check_intangibility:
    /* Intangibility flicker: blink party sprites when invulnerable.
     * Assembly lines 52-73. Toggles bit 15 of spritemap_ptr_hi. */
    if (ow.psi_teleport_destination != 0)
        return 0;
    if (ow.player_intangibility_frames == 0)
        return 0;

    if (ow.player_intangibility_frames >= 0x2D) {
        /* Fast flicker (>= 45 frames remaining): visible every other frame */
        if (ow.player_intangibility_frames & 1)
            entities.spritemap_ptr_hi[ent] &= 0x7FFF;  /* show */
        else
            entities.spritemap_ptr_hi[ent] |= 0x8000;  /* hide */
    } else {
        /* Slow flicker (< 45 frames): hidden every 4th frame */
        if (ow.player_intangibility_frames & 3)
            entities.spritemap_ptr_hi[ent] &= 0x7FFF;  /* show */
        else
            entities.spritemap_ptr_hi[ent] |= 0x8000;  /* hide */
    }

    return 0;
}

/*
 * CLEAR_CURRENT_ENTITY_COLLISION (C0A6DA)
 *
 * Clears the collided_objects for the current entity.
 * Assembly: LDX $88 / LDA #ENTITY_COLLISION_NO_OBJECT / STA ENTITY_COLLIDED_OBJECTS,X / RTL
 */
int16_t cr_clear_current_entity_collision(int16_t ent, int16_t scr,
                                                  uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    entities.collided_objects[ent] = -1;  /* ENTITY_COLLISION_NO_OBJECT = $FFFF */
    return 0;
}

/* ---- Inline movement cases extracted from dispatch switch ---- */

int16_t cr_set_entity_movement_speed(int16_t entity_offset, int16_t script_offset,
                                      uint16_t pc, uint16_t *out_pc) {
    uint16_t speed = sw(pc);
    *out_pc = pc + 2;
    entities.movement_speeds[entity_offset] = speed;
    return 0;
}

int16_t cr_set_entity_movement_speed_entry2(int16_t entity_offset, int16_t script_offset,
                                             uint16_t pc, uint16_t *out_pc) {
    /* Entry2: uses tempvar (A register) as speed instead of reading from stream */
    *out_pc = pc;
    entities.movement_speeds[entity_offset] = (uint16_t)scripts.tempvar[script_offset];
    return 0;
}

int16_t cr_get_entity_movement_speed(int16_t entity_offset, int16_t script_offset,
                                      uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    return (int16_t)entities.movement_speeds[entity_offset];
}

int16_t cr_set_direction8(int16_t entity_offset, int16_t script_offset,
                           uint16_t pc, uint16_t *out_pc) {
    /* Port of SET_DIRECTION8 (set_direction8.asm).
     * Assembly: calls SET_DIRECTION (writes to ENTITY_DIRECTIONS) then
     * additionally stores to ENTITY_MOVING_DIRECTIONS. */
    uint8_t dir = sb(pc);
    *out_pc = pc + 1;
    if (entities.pathfinding_states[entity_offset] >= 0)
        entities.directions[entity_offset] = (int8_t)dir;
    entities.moving_directions[entity_offset] = (int8_t)dir;
    return 0;
}

int16_t cr_set_direction(int16_t entity_offset, int16_t script_offset,
                          uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    /* Uses tempvar as direction. Assembly (set_direction.asm) preserves
     * the direction in A via TAY/TYA, so return value = input direction.
     * If pathfinding_states bit 15 is set, skip storing direction. */
    if (entities.pathfinding_states[entity_offset] >= 0) {
        entities.directions[entity_offset] = (int8_t)scripts.tempvar[script_offset];
    }
    return scripts.tempvar[script_offset];
}

int16_t cr_get_entity_direction(int16_t entity_offset, int16_t script_offset,
                                 uint16_t pc, uint16_t *out_pc) {
    /* Port of GET_ENTITY_DIRECTION (C0A673.asm).
     * Returns the current facing direction of this entity. */
    *out_pc = pc;
    return (int16_t)entities.directions[entity_offset];
}

int16_t cr_rand_high_byte(int16_t entity_offset, int16_t script_offset,
                           uint16_t pc, uint16_t *out_pc) {
    /* Port of RAND_HIGH_BYTE (C09FA8.asm).
     * Returns random 0-255 in high byte (<<8). Used for random direction
     * selection: script divides by speed to get 8-way direction. */
    *out_pc = pc;
    return (int16_t)((uint16_t)rng_next_byte() << 8);
}

int16_t cr_movement_cmd_set_dir_velocity(int16_t entity_offset, int16_t script_offset,
                                          uint16_t pc, uint16_t *out_pc) {
    /* Port of MOVEMENT_CMD_SET_DIRECTION_VELOCITY (C0A697.asm).
     * Reads 1 byte (direction) from script stream, then calls
     * SET_ENTITY_DIRECTION_VELOCITY (C0C83B) to set moving_directions
     * and velocity from the entity's movement_speed.
     * Assembly: STY $94 / JSL SET_ENTITY_DIRECTION_VELOCITY.
     * SET_ENTITY_DIRECTION_VELOCITY stores direction to
     * ENTITY_MOVING_DIRECTIONS before computing velocity. */
    uint8_t dir = sb(pc);
    *out_pc = pc + 1;
    entities.moving_directions[entity_offset] = (int16_t)dir;
    set_velocity_from_direction(entity_offset, (int16_t)dir);
    return 0;
}

int16_t cr_get_entity_obstacle_flags(int16_t entity_offset, int16_t script_offset,
                                      uint16_t pc, uint16_t *out_pc) {
    /* Port of GET_ENTITY_OBSTACLE_FLAGS (C0A6C5.asm).
     * Returns obstacle/collision flags for this entity. */
    *out_pc = pc;
    return (int16_t)entities.obstacle_flags[entity_offset];
}

int16_t cr_get_entity_pathfinding_state(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* Port of GET_ENTITY_PATHFINDING_STATE (C0A6CB.asm).
     * Returns pathfinding state word for this entity.
     * Bit 15 = locked direction flag. */
    *out_pc = pc;
    return entities.pathfinding_states[entity_offset];
}

int16_t cr_set_surface_flags(int16_t entity_offset, int16_t script_offset,
                              uint16_t pc, uint16_t *out_pc) {
    uint8_t flags = sb(pc);
    *out_pc = pc + 1;
    entities.surface_flags[entity_offset] = flags;
    return 0;
}

int16_t cr_set_entity_velocity_from_dir(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* Port of C47044: set velocity from direction (in A/tempvar) and speed.
     * Direction is passed as the return value from CALCULATE_DIRECTION_TO_TARGET. */
    *out_pc = pc;
    int16_t dir = scripts.tempvar[script_offset];
    set_velocity_from_direction(entity_offset, dir);
    return dir;
}

int16_t cr_move_entity_distance(int16_t entity_offset, int16_t script_offset,
                                 uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A6AD + C0CBD3 (CALCULATE_MOVEMENT_DURATION).
     * Reads 16-bit distance, computes sleep_frames = (distance << 8) / speed.
     * Entity continues moving in current direction for that many frames. */
    uint16_t distance = sw(pc);
    *out_pc = pc + 2;
    uint32_t speed = (uint32_t)entities.movement_speeds[entity_offset];
    if (speed > 0) {
        uint32_t frames = ((uint32_t)distance << 8) / speed;
        scripts.sleep_frames[script_offset] = (int16_t)frames;
    } else {
        scripts.sleep_frames[script_offset] = 0;
    }
    return 0;
}

int16_t cr_halve_entity_delta_y(int16_t entity_offset, int16_t script_offset,
                                 uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    /* Assembly: extract sign bit before shifting, then OR back after.
     * Original delta_y sign must be preserved in both halved values. */
    int16_t orig_dy = entities.delta_y[entity_offset];
    uint16_t sign_bit = (uint16_t)orig_dy & 0x8000;
    entities.delta_y[entity_offset] = (int16_t)(((uint16_t)orig_dy >> 1) | sign_bit);
    /* Assembly only halves delta_y (the integer part); delta_frac_y is not modified. */
    return 0;
}

int16_t cr_load_current_map_block_events(int16_t entity_offset, int16_t script_offset,
                                          uint16_t pc, uint16_t *out_pc) {
    /* Port of LOAD_CURRENT_MAP_BLOCK_EVENTS (C4733C.asm).
     * Re-applies event-triggered tile changes after flag changes. */
    *out_pc = pc;
    load_current_map_block_events();
    return 0;
}

int16_t cr_reverse_direction_8(int16_t entity_offset, int16_t script_offset,
                                uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    /* Reverse 8-way direction: (dir + 4) % 8 */
    entities.directions[entity_offset] =
        (entities.directions[entity_offset] + 4) & 7;
    return 0;
}

int16_t cr_quantize_entity_direction(int16_t entity_offset, int16_t script_offset,
                                      uint16_t pc, uint16_t *out_pc) {
    /* Port of C46B0A QUANTIZE_ENTITY_DIRECTION.
     * Quantizes fine direction to 8-way and stores in ENTITY_MOVING_DIRECTIONS. */
    *out_pc = pc;
    int16_t qoff = ENT(ert.current_entity_slot);
    int16_t qdir = quantize_entity_direction(qoff,
        (uint16_t)scripts.tempvar[script_offset]);
    return qdir;
}

int16_t cr_calculate_direction_to_target(int16_t entity_offset, int16_t script_offset,
                                          uint16_t pc, uint16_t *out_pc) {
    /* Port of C46ADB: compute fine direction from entity to target (V6,V7).
     * V6 = target X, V7 = target Y (set by script SET_VAR opcodes).
     * Returns fine 16-bit direction (used by SET_ENTITY_VELOCITY_FROM_DIRECTION). */
    *out_pc = pc;
    int16_t target_x = entities.var[6][entity_offset];  /* V6 = target X */
    int16_t target_y = entities.var[7][entity_offset];  /* V7 = target Y */
    uint16_t dir = calculate_direction_fine(
        entities.abs_x[entity_offset], entities.abs_y[entity_offset],
        target_x, target_y);
    return (int16_t)dir;
}

int16_t cr_disable_entity_collision2(int16_t entity_offset, int16_t script_offset,
                                      uint16_t pc, uint16_t *out_pc) {
    /* Port of DISABLE_CURRENT_ENTITY_COLLISION2 (C0A82F).
     * Assembly: LDA #ENTITY_COLLISION_DISABLED; LDX $88; STA ENTITY_COLLIDED_OBJECTS,X */
    *out_pc = pc;
    entities.collided_objects[entity_offset] = ENTITY_COLLISION_DISABLED;
    return 0;
}

int16_t cr_movement_display_text(int16_t entity_offset, int16_t script_offset,
                                  uint16_t pc, uint16_t *out_pc) {
    /* 4 extra bytes: bank(1), 0(1), text_lo(1), text_hi(1).
     * Port of asm/overworld/movement_display_text.asm — reads a 24-bit text pointer
     * and calls DISPLAY_TEXT_FROM_PARAMS to run the text bytecode.
     *
     * The 4 bytes encode a ROM address from EVENT_DISPLAY_TEXT_IMMEDIATELY:
     *   .BANKBYTE(label), 0, .LOBYTE(label), .HIBYTE(label)
     * For attract mode, these point into EEVENT0 (bank $CB, base $E5BC).
     * Convert to ert.buffer offset and call display_text(). */
    uint8_t bank = sb(pc);
    uint8_t text_lo = sb(pc + 2);
    uint8_t text_hi = sb(pc + 3);
    *out_pc = pc + 4;

    uint32_t full_snes_addr = ((uint32_t)bank << 16) | ((uint16_t)text_hi << 8) | text_lo;

    /* SNES address from ROM script bytes — remapped to dialogue blob at runtime */
    display_text_from_addr(full_snes_addr);

    /* Set FLG_TEMP_1 (flag 2) to unblock script spin loops.
     * Assembly: text bytecode sets this when text completes. */
    event_flag_set(2);
    return 0;
}

int16_t cr_movement_cmd_play_sound(int16_t entity_offset, int16_t script_offset,
                                    uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A841: read 16-bit sound ID and play it. */
    uint16_t sfx_id = sw(pc);
    *out_pc = pc + 2;
    play_sfx(sfx_id);
    return 0;
}

int16_t cr_movement_cmd_copy_sprite_pos(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A86F → C46CC7: find entity by sprite ID and copy its
     * abs_x/abs_y to the current entity.
     * Assembly: FIND_ENTITY_BY_SPRITE_ID returns slot; ASL to get offset. */
    uint16_t sprite_id = sw(pc);
    *out_pc = pc + 2;
    int16_t found = find_entity_by_sprite_id(sprite_id);
    if (found >= 0) {
        int16_t found_off = ENT(found);
        entities.abs_x[entity_offset] = entities.abs_x[found_off];
        entities.abs_y[entity_offset] = entities.abs_y[found_off];
    }
    return 0;
}

int16_t cr_movement_cmd_copy_leader_pos(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* 1 extra byte: param (unused in most contexts).
     * Copy the party leader's position to this entity. */
    (void)sb(pc);
    *out_pc = pc + 1;
    entities.abs_x[entity_offset] = (int16_t)game_state.leader_x_coord;
    entities.abs_y[entity_offset] = (int16_t)game_state.leader_y_coord;
    return 0;
}

int16_t cr_movement_store_offset_position(int16_t entity_offset, int16_t script_offset,
                                           uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A8B3 → C46C5E: STORE_ENTITY_OFFSET_POSITION.
     * Reads x_offset(16) and y_offset(16), stores:
     *   var0 = abs_x + x_offset
     *   var1 = abs_y + y_offset */
    int16_t x_off = (int16_t)sw(pc);
    int16_t y_off = (int16_t)sw(pc + 2);
    *out_pc = pc + 4;
    entities.var[0][entity_offset] =
        entities.abs_x[entity_offset] + x_off;
    entities.var[1][entity_offset] =
        entities.abs_y[entity_offset] + y_off;
    return 0;
}

int16_t cr_movement_cmd_get_event_flag(int16_t entity_offset, int16_t script_offset,
                                        uint16_t pc, uint16_t *out_pc) {
    /* 2 extra bytes: flag ID (word) — return flag state */
    uint16_t flag_id = sw(pc);
    *out_pc = pc + 2;
    return event_flag_get(flag_id) ? 1 : 0;
}

int16_t cr_movement_cmd_set_event_flag(int16_t entity_offset, int16_t script_offset,
                                        uint16_t pc, uint16_t *out_pc) {
    /* 2 extra bytes: flag ID (word) — set the flag */
    uint16_t flag_id = sw(pc);
    *out_pc = pc + 2;
    event_flag_set(flag_id);
    return 0;
}

int16_t cr_movement_queue_interaction(int16_t entity_offset, int16_t script_offset,
                                       uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A88D MOVEMENT_QUEUE_INTERACTION.
     * Reads two 16-bit values from the movement script stream.
     * Assembly reads first word → PHA, second word → A, PLX.
     * QUEUE_INTERACTION_FROM_PARAMS(A=second, X=first) builds
     * a 32-bit pointer: low=A(second), high=X(first), type=8. */
    uint16_t first_word = sw(pc);      /* high word of pointer */
    uint16_t second_word = sw(pc + 2); /* low word of pointer */
    uint32_t data_ptr = (uint32_t)second_word | ((uint32_t)first_word << 16);
    queue_interaction(8, data_ptr);
    *out_pc = pc + 4;
    return 0;
}

int16_t cr_movement_cmd_store_npc_pos(int16_t entity_offset, int16_t script_offset,
                                       uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A92D → C46B8D: STORE_NPC_POSITION_TO_SCRIPT_VARS.
     * Finds entity by NPC ID and stores its abs_x/abs_y to
     * the current entity's var6/var7 (target position registers). */
    uint16_t npc_id = sw(pc);
    *out_pc = pc + 2;
    int16_t found = find_entity_by_npc_id(npc_id);
    if (found >= 0) {
        entities.var[6][entity_offset] = entities.abs_x[found];
        entities.var[7][entity_offset] = entities.abs_y[found];
    }
    return 0;
}

int16_t cr_movement_cmd_store_sprite_pos(int16_t entity_offset, int16_t script_offset,
                                          uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A938 → C46BBB: SAVE_SPRITE_POSITION_TO_VARS.
     * Same as STORE_NPC_POS but looks up by sprite ID.
     * Assembly: FIND_ENTITY_BY_SPRITE_ID returns slot; ASL to get offset. */
    uint16_t sprite_id = sw(pc);
    *out_pc = pc + 2;
    int16_t found = find_entity_by_sprite_id(sprite_id);
    if (found >= 0) {
        int16_t found_off = ENT(found);
        entities.var[6][entity_offset] = entities.abs_x[found_off];
        entities.var[7][entity_offset] = entities.abs_y[found_off];
    }
    return 0;
}

int16_t cr_movement_cmd_face_toward_npc(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A94E → C46984: FACE_ENTITY_TOWARD_NPC.
     * Finds the entity by NPC ID and sets that entity's direction
     * to face toward the current entity. */
    uint16_t npc_id = sw(pc);
    *out_pc = pc + 2;
    int16_t found = find_entity_by_npc_id(npc_id);
    if (found >= 0) {
        int16_t dir = calculate_direction_8(
            entities.abs_x[found], entities.abs_y[found],
            entities.abs_x[entity_offset], entities.abs_y[entity_offset]);
        entities.directions[found] = dir;
    }
    return 0;
}

int16_t cr_movement_cmd_face_toward_sprite(int16_t entity_offset, int16_t script_offset,
                                            uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A959 → C469F1: FACE_ENTITY_TOWARD_SPRITE.
     * Same as FACE_TOWARD_NPC but looks up by sprite ID.
     * Assembly: FIND_ENTITY_BY_SPRITE_ID returns slot; ASL to get offset. */
    uint16_t sprite_id = sw(pc);
    *out_pc = pc + 2;
    int16_t found = find_entity_by_sprite_id(sprite_id);
    if (found >= 0) {
        int16_t found_off = ENT(found);
        int16_t dir = calculate_direction_8(
            entities.abs_x[found_off], entities.abs_y[found_off],
            entities.abs_x[entity_offset], entities.abs_y[entity_offset]);
        entities.directions[found_off] = dir;
    }
    return 0;
}

int16_t cr_movement_set_bounding_box(int16_t entity_offset, int16_t script_offset,
                                      uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A964 → C47225: SET_ENTITY_BOUNDING_BOX.
     * Reads half_width(16) and half_height(16), sets:
     *   var0 = abs_x - half_width   (left)
     *   var1 = abs_x + half_width   (right)
     *   var2 = abs_y - half_height  (top)
     *   var3 = abs_y + half_height  (bottom) */
    int16_t half_w = (int16_t)sw(pc);
    int16_t half_h = (int16_t)sw(pc + 2);
    *out_pc = pc + 4;
    int16_t ax = entities.abs_x[entity_offset];
    int16_t ay = entities.abs_y[entity_offset];
    entities.var[0][entity_offset] = ax - half_w;
    entities.var[1][entity_offset] = ax + half_w;
    entities.var[2][entity_offset] = ay - half_h;
    entities.var[3][entity_offset] = ay + half_h;
    return 0;
}

int16_t cr_movement_set_pos_from_screen(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A87A → C46CF5: ENTITY_SCREEN_TO_WORLD.
     * Reads screen_x(16) and screen_y(16), converts to world coords:
     *   abs_x = screen_x + BG1_X_POS
     *   abs_y = screen_y + BG1_Y_POS
     *   frac_x = frac_y = 0x8000 */
    int16_t scr_x = (int16_t)sw(pc);
    int16_t scr_y = (int16_t)sw(pc + 2);
    *out_pc = pc + 4;
    entities.abs_x[entity_offset] = scr_x + (int16_t)ppu.bg_hofs[0];
    entities.abs_y[entity_offset] = scr_y + (int16_t)ppu.bg_vofs[0];
    entities.frac_x[entity_offset] = 0x8000;
    entities.frac_y[entity_offset] = 0x8000;
    return 0;
}

int16_t cr_movement_cmd_set_npc_id(int16_t entity_offset, int16_t script_offset,
                                    uint16_t pc, uint16_t *out_pc) {
    uint16_t npc_id = sw(pc);
    *out_pc = pc + 2;
    entities.npc_ids[entity_offset] = npc_id;
    return 0;
}

int16_t cr_movement_cmd_animate_pal_fade(int16_t entity_offset, int16_t script_offset,
                                          uint16_t pc, uint16_t *out_pc) {
    /* Port of C0AAB5 MOVEMENT_CMD_ANIMATE_PALETTE_FADE.
     * Reads 4 bytes: 16-bit palette mask, 8-bit fade_type, 8-bit frames.
     * Calls ANIMATE_PALETTE_FADE_TO_MAP which:
     *   1. LOAD_PALETTE_TO_FADE_BUFFER — applies brightness transform
     *      (fade_type: 0=black, 1-49=dim, 50=normal, 51+=white) to
     *      MAP_PALETTE_BACKUP and stores result in ert.buffer[].
     *   2. PREPARE_PALETTE_FADE — computes per-color fade slopes from
     *      current ert.palettes[] to ert.buffer[] target.
     *   3. Loops `frames` times calling UPDATE_MAP_PALETTE_ANIMATION.
     *   4. FINALIZE_PALETTE_FADE — copies ert.buffer[] to ert.palettes[].
     *
     * Since this is blocking in assembly (internal WAIT_UNTIL_NEXT_FRAME
     * loop), we fast-forward the entire fade: load target to ert.buffer,
     * run the slope iterations, then finalize. */
    uint16_t mask = sw(pc);
    uint8_t fade_type = sb(pc + 2);
    uint8_t frames = sb(pc + 3);
    *out_pc = pc + 4;

    /* Step 1: LOAD_PALETTE_TO_FADE_BUFFER — transform MAP_PALETTE_BACKUP
     * by fade_type brightness and store into ert.buffer[].
     * Port of FADE_PALETTE_COLOR (C49496):
     *   type < 50: channel_out = min((channel * type * 5) >> 8, 31)
     *   type == 50: no change
     *   type > 50: white (0x7FFF) */
    for (int i = 0; i < 256; i++) {
        uint16_t color = ml.map_palette_backup[i];
        uint16_t out_color;
        if (fade_type == 50) {
            out_color = color;
        } else if (fade_type > 50) {
            out_color = 0x7FFF;
        } else {
            uint16_t mult = (uint16_t)fade_type * 5;
            uint16_t r = (((color & 0x1F) * mult) >> 8);
            uint16_t g = ((((color >> 5) & 0x1F) * mult) >> 8);
            uint16_t b = ((((color >> 10) & 0x1F) * mult) >> 8);
            if (r > 31) r = 31;
            if (g > 31) g = 31;
            if (b > 31) b = 31;
            out_color = r | (g << 5) | (b << 10);
        }
        ert.buffer[i * 2]     = (uint8_t)(out_color & 0xFF);
        ert.buffer[i * 2 + 1] = (uint8_t)(out_color >> 8);
    }

    /* Step 2: PREPARE_PALETTE_FADE — compute slopes from ert.palettes[] to
     * ert.buffer[] target, then fast-forward by running the animation. */
    prepare_palette_fade_slopes((int16_t)frames, mask);

    /* Step 3: Fast-forward the fade — run update_map_palette_animation
     * `frames` times to reach the target palette state. */
    if (frames > 1) {
        for (int f = 0; f < (int)frames; f++) {
            update_map_palette_animation();
        }
    }

    /* Step 4: FINALIZE_PALETTE_FADE — copy ert.buffer[] (target) to ert.palettes[]. */
    memcpy(ert.palettes, ert.buffer, 512);
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    return 0;
}

int16_t cr_movement_cmd_setup_spotlight(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* Port of C0AA23 → C47765: SETUP_SPOTLIGHT_HDMA_WINDOW.
     * Reads 6 bytes: three 16-bit params from movement data.
     * Builds per-scanline WH0/WH1 window tables to create a spotlight
     * cone effect. Used by ending credits spotlight scenes (365, 398, 411).
     *
     * Assembly wrapper reads: left_x, y_pos, right_x (world coords).
     * SETUP_SPOTLIGHT_HDMA_WINDOW converts to screen coords and builds
     * an HDMA table: dark region (WH0=0,WH1=0xFF) above the cone,
     * 16-scanline cone narrowing downward, then empty window below. */
    int16_t left_x  = (int16_t)sw(pc);
    int16_t y_pos   = (int16_t)sw(pc + 2);
    int16_t right_x = (int16_t)sw(pc + 4);
    *out_pc = pc + 6;

    /* Convert world coordinates to screen coordinates */
    int screen_y     = (int)(int16_t)((uint16_t)y_pos - ppu.bg_vofs[0]);
    int screen_left  = (int)(int16_t)((uint16_t)left_x - ppu.bg_hofs[0]);
    int screen_right = (int)(int16_t)((uint16_t)right_x - ppu.bg_hofs[0]);

    /* Phase 1: scanlines before cone — full window (dark area) */
    for (int s = 0; s < VIEWPORT_HEIGHT && s < screen_y; s++) {
        ppu.wh0_table[s] = 0;
        ppu.wh1_table[s] = 0xFF;
    }

    /* Phase 2: 16 scanlines of spotlight cone (narrows downward) */
    for (int i = 0; i < 16; i++) {
        int s = screen_y + i;
        if (s < 0) continue;
        if (s >= VIEWPORT_HEIGHT) break;
        ppu.wh0_table[s] = (uint8_t)(screen_left + i);
        ppu.wh1_table[s] = (uint8_t)(screen_right - i);
    }

    /* Phase 3: remaining scanlines — empty window (no masking) */
    for (int s = screen_y + 16; s < VIEWPORT_HEIGHT; s++) {
        if (s < 0) continue;
        ppu.wh0_table[s] = 128;
        ppu.wh1_table[s] = 127;
    }

    ppu.window_hdma_active = true;
    return 0;
}

int16_t cr_movement_cmd_apply_color_math(int16_t entity_offset, int16_t script_offset,
                                          uint16_t pc, uint16_t *out_pc) {
    /* Port of C0AA3F → C42439: MOVEMENT_CMD_APPLY_COLOR_MATH_FIXED.
     * Reads 3 × 8-bit values (B, G, R) from script data.
     * Previous tempvar selects add/subtract mode:
     *   tempvar == 1 → cgadsub = $B3 (subtract from BG1+BG2+OBJ+backdrop)
     *   tempvar != 1 → cgadsub = $33 (add to BG1+BG2+OBJ+backdrop)
     * Then writes CGADSUB and FIXED_COLOR_DATA (5-bit RGB). */
    int16_t prev_temp = scripts.tempvar[script_offset];
    uint8_t mode = (prev_temp == 1) ? 0xB3 : 0x33;

    uint8_t blue  = sb(pc);
    uint8_t green = sb(pc + 1);
    uint8_t red   = sb(pc + 2);
    *out_pc = pc + 3;

    ppu.cgadsub = mode;
    ppu.coldata_b = blue & 0x1F;
    ppu.coldata_g = green & 0x1F;
    ppu.coldata_r = red & 0x1F;
    return 0;
}

int16_t cr_movement_cmd_print_cast_name(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* C0A9B3: MOVEMENT_CMD_PRINT_CAST_NAME.
     * Reads 3 x 16-bit params: cast_index, x_col, y_row.
     * Calls PRINT_CAST_NAME to render a character's name on BG3. */
    uint16_t pcn_index = sw(pc);
    uint16_t pcn_x = sw(pc + 2);
    uint16_t pcn_y = sw(pc + 4);
    *out_pc = pc + 6;
    print_cast_name(pcn_index, pcn_x, pcn_y);
    return 0;
}

int16_t cr_movement_cmd_print_cast_var0(int16_t entity_offset, int16_t script_offset,
                                         uint16_t pc, uint16_t *out_pc) {
    /* C0A9EB: MOVEMENT_CMD_PRINT_CAST_VAR0.
     * Reads 3 x 16-bit params: cast_index, x_col, y_row.
     * Calls PRINT_CAST_NAME_ENTITY_VAR0 with entity's var0. */
    uint16_t pcv_index = sw(pc);
    uint16_t pcv_x = sw(pc + 2);
    uint16_t pcv_y = sw(pc + 4);
    *out_pc = pc + 6;
    print_cast_name_entity_var0(pcv_index, pcv_x, pcv_y, ert.current_entity_slot);
    return 0;
}

int16_t cr_movement_cmd_print_cast_party(int16_t entity_offset, int16_t script_offset,
                                          uint16_t pc, uint16_t *out_pc) {
    /* C0A9CF: MOVEMENT_CMD_PRINT_CAST_PARTY.
     * Reads 3 x 16-bit params: char_id, x_col, y_row.
     * Calls PRINT_CAST_NAME_PARTY to render party member name on BG3. */
    uint16_t pcp_id = sw(pc);
    uint16_t pcp_x = sw(pc + 2);
    uint16_t pcp_y = sw(pc + 4);
    *out_pc = pc + 6;
    print_cast_name_party(pcp_id, pcp_x, pcp_y);
    return 0;
}

int16_t cr_move_toward_no_sprite_cb(int16_t entity_offset, int16_t script_offset,
                                     uint16_t pc, uint16_t *out_pc) {
    /* Port of MOVE_ENTITY_TOWARD_TARGET (C47143) with no-sprite-callback flag.
     * Checks if entity is close enough to target (V6,V7).
     * Returns 0 if still moving (loop continues), non-zero if arrived.
     * V5 = proximity threshold, V6 = target X, V7 = target Y. */
    *out_pc = pc;
    int16_t target_x = entities.var[6][entity_offset];  /* V6 = target X */
    int16_t target_y = entities.var[7][entity_offset];  /* V7 = target Y */
    int16_t threshold = entities.var[5][entity_offset];  /* V5 = threshold */
    int dx = abs((int)entities.abs_x[entity_offset] - (int)target_x);
    int dy = abs((int)entities.abs_y[entity_offset] - (int)target_y);
    if (dx < threshold && dy < threshold) {
        /* Reached target — stop movement */
        entities.delta_x[entity_offset] = 0;
        entities.delta_frac_x[entity_offset] = 0;
        entities.delta_y[entity_offset] = 0;
        entities.delta_frac_y[entity_offset] = 0;
        return 1;  /* TRUE = arrived */
    }
    /* Still moving — update direction and velocity toward target */
    uint16_t mt_fine = calculate_direction_fine(
        entities.abs_x[entity_offset], entities.abs_y[entity_offset],
        target_x, target_y);
    set_velocity_from_direction(entity_offset, (int16_t)mt_fine);
    return 0;  /* FALSE = still moving */
}

int16_t cr_move_toward_reversed_cb(int16_t entity_offset, int16_t script_offset,
                                    uint16_t pc, uint16_t *out_pc) {
    /* Port of MOVE_ENTITY_TOWARD_TARGET(A=1 reverse, X=0 update sprite).
     * Moves entity TOWARD target (velocity = normal direction),
     * but SPRITE faces the opposite direction (walking backward).
     * Also updates sprite if sprite group changes. */
    static const int16_t rev_dir_to_sg[8] = {0,0,2,4,4,4,6,0};
    *out_pc = pc;
    int16_t tx = entities.var[6][entity_offset];
    int16_t ty = entities.var[7][entity_offset];
    int16_t thr = entities.var[5][entity_offset];
    int rdx = abs((int)entities.abs_x[entity_offset] - (int)tx);
    int rdy = abs((int)entities.abs_y[entity_offset] - (int)ty);
    if (rdx < thr && rdy < thr) {
        entities.delta_x[entity_offset] = 0;
        entities.delta_frac_x[entity_offset] = 0;
        entities.delta_y[entity_offset] = 0;
        entities.delta_frac_y[entity_offset] = 0;
        return 1;
    }
    uint16_t rdir_fine = calculate_direction_fine(
        entities.abs_x[entity_offset], entities.abs_y[entity_offset], tx, ty);
    /* Velocity is toward target (fine direction for smooth movement) */
    set_velocity_from_direction(entity_offset, (int16_t)rdir_fine);
    /* Sprite direction is reversed (entity faces away from target) */
    {
        int16_t qdir = quantize_direction((int16_t)rdir_fine);
        int16_t rev = (qdir + 4) & 7;
        int16_t old_dir = entities.directions[entity_offset];
        int16_t old_group = rev_dir_to_sg[old_dir & 7];
        int16_t new_group = rev_dir_to_sg[rev & 7];
        entities.directions[entity_offset] = rev;
        if (old_group != new_group) {
            render_entity_sprite(entity_offset);
        }
    }
    return 0;
}

int16_t cr_update_dir_velocity_cb(int16_t entity_offset, int16_t script_offset,
                                   uint16_t pc, uint16_t *out_pc) {
    /* Port of UPDATE_DIRECTION_VELOCITY_CALLBACK (C0A8E7).
     * Calls UPDATE_ENTITY_DIRECTION_AND_VELOCITY(A=0).
     * Reads var0 as fine direction, sets velocity, then quantizes to 8-way for sprite.
     * Assembly: C472A8 reads ENTITY_SCRIPT_VAR0 as fine direction. */
    static const int16_t udv_dir_to_sg[8] = {0,0,2,4,4,4,6,0};
    *out_pc = pc;
    uint16_t fine_dir = (uint16_t)entities.var[0][entity_offset];
    /* Set velocity from fine direction FIRST (smooth movement) */
    set_velocity_from_direction(entity_offset, (int16_t)fine_dir);
    /* THEN quantize to 8-way for sprite direction */
    int16_t snes_dir = quantize_fine_direction(fine_dir);
    int16_t old_dir = entities.directions[entity_offset];
    int16_t old_group = udv_dir_to_sg[old_dir & 7];
    int16_t new_group = udv_dir_to_sg[snes_dir & 7];
    entities.directions[entity_offset] = snes_dir;
    if (old_group != new_group) {
        render_entity_sprite(entity_offset);
    }
    return 0;
}

int16_t cr_update_dir_velocity_reversed_cb(int16_t entity_offset, int16_t script_offset,
                                            uint16_t pc, uint16_t *out_pc) {
    /* Port of UPDATE_DIRECTION_VELOCITY_REVERSED_CALLBACK (C0A8EF).
     * Calls UPDATE_ENTITY_DIRECTION_AND_VELOCITY(A=1).
     * Same as above but reverses the sprite direction (entity faces away). */
    static const int16_t udv_rev_sg[8] = {0,0,2,4,4,4,6,0};
    *out_pc = pc;
    uint16_t fine_dir = (uint16_t)entities.var[0][entity_offset];
    /* Set velocity from fine direction FIRST */
    set_velocity_from_direction(entity_offset, (int16_t)fine_dir);
    /* THEN quantize and reverse for sprite */
    int16_t snes_dir = quantize_fine_direction(fine_dir);
    int16_t rev = (snes_dir + 4) & 7;
    int16_t old_dir = entities.directions[entity_offset];
    int16_t old_group = udv_rev_sg[old_dir & 7];
    int16_t new_group = udv_rev_sg[rev & 7];
    entities.directions[entity_offset] = rev;
    if (old_group != new_group) {
        render_entity_sprite(entity_offset);
    }
    return 0;
}

int16_t cr_move_toward_target_cb(int16_t entity_offset, int16_t script_offset,
                                  uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A8C6 MOVE_TOWARD_TARGET_CALLBACK.
     * Calls MOVE_ENTITY_TOWARD_TARGET(A=0, X=0):
     *   reverse=0, skip_sprite_update=0.
     * Same proximity check as NO_SPRITE but also updates sprite
     * when direction sprite group changes. */
    static const int16_t dir_to_sprite_group[8] = {0,0,2,4,4,4,6,0};
    *out_pc = pc;
    int16_t tx = entities.var[6][entity_offset];
    int16_t ty = entities.var[7][entity_offset];
    int16_t thr = entities.var[5][entity_offset];
    int dx = abs((int)entities.abs_x[entity_offset] - (int)tx);
    int dy = abs((int)entities.abs_y[entity_offset] - (int)ty);
    if (dx < thr && dy < thr) {
        /* Assembly (MOVE_ENTITY_TOWARD_TARGET line 73): LDA #TRUE; BRA @RETURN
         * — returns TRUE without zeroing deltas. */
        return 1;
    }
    uint16_t met_fine = calculate_direction_fine(
        entities.abs_x[entity_offset], entities.abs_y[entity_offset],
        tx, ty);
    set_velocity_from_direction(entity_offset, (int16_t)met_fine);
    /* Update sprite if direction sprite group changed */
    {
        int16_t qdir = quantize_direction((int16_t)met_fine);
        int16_t old_dir = entities.directions[entity_offset];
        int16_t old_group = dir_to_sprite_group[old_dir & 7];
        int16_t new_group = dir_to_sprite_group[qdir & 7];
        entities.directions[entity_offset] = qdir;
        if (old_group != new_group) {
            render_entity_sprite(entity_offset);
        }
    }
    return 0;
}

/*
 * UPDATE_MINI_GHOST_POSITION (C0778A)
 *
 * Port of asm/overworld/update_mini_ghost_position.asm.
 * Orbits the mini ghost entity around the party leader at radius 0x3000.
 * If the current party member is disabled, hides the sprite instead.
 */
int16_t cr_update_mini_ghost_position(int16_t entity_offset, int16_t script_offset,
                                      uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;

    /* Check if current party member is disabled */
    int16_t party_ent = ENT(game_state.current_party_members);
    if (entities.tick_callback_hi[party_ent] &
        (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED)) {
        /* Hide sprite */
        entities.animation_frame[entity_offset] = -1;
        return 0;
    }

    /* Calculate orbit position using angle and radius 0x3000 */
    int16_t vel_x, vel_y;
    calculate_velocity_components(ow.mini_ghost_angle, 0x3000, &vel_x, &vel_y);

    /* X position: high byte of vel_y (sign-extended) + leader_x
     * Assembly: LDA @LOCAL00+2; AND #$FF00; XBA; sign-extend; ADC leader_x */
    int16_t x_offset = vel_y >> 8;
    entities.abs_x[entity_offset] =
        (int16_t)(game_state.leader_x_coord + (uint16_t)x_offset);

    /* Y position: vel_x >> 10 (ASR16 with Y=10) + leader_y - 8
     * Assembly: LDA @LOCAL00; JSL ASR16(Y=10); ADC leader_y - 8 */
    int16_t y_offset = vel_x >> 10;
    entities.abs_y[entity_offset] =
        (int16_t)(game_state.leader_y_coord - 8 + (uint16_t)y_offset);

    /* Advance angle by 0x0300 per frame */
    ow.mini_ghost_angle += 0x0300;

    /* Show sprite (animation frame 0) */
    entities.animation_frame[entity_offset] = 0;

    return 0;
}
