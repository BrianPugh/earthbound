/*
 * Callroutine movement functions — speed, direction, velocity, movement loops.
 * Extracted from callroutine.c.
 */
#include "entity/callroutine_internal.h"
#include "entity/entity.h"
#include "entity/script.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "game/display_text.h"
#include "game/display_text_internal.h"
#include "core/mode_stack.h"
#include "core/log.h"
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
#include <stdlib.h>
#include <string.h>

/* FOOTSTEP_SOUND_TABLE — 10 entries x 2 bytes (uint16_t), maps terrain type to SFX ID.
 * Loaded from data/footstep_sound_table.bin (extracted by ebtools from ROM C40BD4).
 * Indexed by ow.footstep_sound_id/2 (ow.footstep_sound_id is pre-multiplied by 2). */
static const uint8_t *footstep_sound_table;

/* ---- Movement helpers ---- */

/*
 * SET_ENTITY_VELOCITY_FROM_DIRECTION (C47044)
 *
 * Port of asm/overworld/entity/set_entity_velocity_from_direction.asm.
 * Splits the entity's movement speed along a fine 16-bit angle
 * (0x0000 = up, clockwise) with CALCULATE_VELOCITY_COMPONENTS. Each
 * component is in the speed's 8.8 units: its high byte becomes the integer
 * delta word (sign-extended) and its low byte the high byte of the fraction
 * word. For a negative component the asm ORs $00FF into the fraction (the
 * ORA #$00FF it uses to mirror the integer word's ORA #$FF00), so the
 * fraction's low byte is $FF instead of $00.
 *
 * Returns the direction, like the asm (LDA @VIRTUAL04).
 */
uint16_t set_entity_velocity_from_direction(int16_t ent, uint16_t direction) {
    int16_t comp_x, comp_y;
    calculate_velocity_components(direction, (int16_t)entities.movement_speeds[ent],
                                  &comp_x, &comp_y);

    uint16_t ux = (uint16_t)comp_x;
    uint16_t uy = (uint16_t)comp_y;
    if (ux & 0x8000) {
        entities.delta_x[ent] = (int16_t)((ux >> 8) | 0xFF00);
        entities.delta_frac_x[ent] = (uint16_t)((ux << 8) | 0x00FF);
    } else {
        entities.delta_x[ent] = (int16_t)((ux >> 8) & 0x00FF);
        entities.delta_frac_x[ent] = (uint16_t)((ux << 8) & 0xFF00);
    }
    if (uy & 0x8000) {
        entities.delta_y[ent] = (int16_t)((uy >> 8) | 0xFF00);
        entities.delta_frac_y[ent] = (uint16_t)((uy << 8) | 0x00FF);
    } else {
        entities.delta_y[ent] = (int16_t)((uy >> 8) & 0x00FF);
        entities.delta_frac_y[ent] = (uint16_t)((uy << 8) & 0xFF00);
    }
    return direction;
}

/*
 * SET_ENTITY_DIRECTION_VELOCITY (C0C83B)
 *
 * Port of asm/overworld/entity/set_entity_direction_velocity.asm.
 * Stores an 8-way direction (0 = up, clockwise) to ENTITY_MOVING_DIRECTIONS
 * and sets the 16.16 velocity: speed * scale >> 8, where scale is $10000
 * for cardinal (even) directions and $B505 (1/sqrt 2) for diagonals.
 * MULT32 keeps the low 32 bits and ASR8_INT is an arithmetic shift.
 *
 * Returns the Y delta's integer word, the last value the asm loads into A.
 * Directions above 7 fall out of the asm's compare chain and store its
 * uninitialised locals; the port leaves the velocity unchanged for them.
 */
int16_t set_entity_direction_velocity(int16_t ent, int16_t direction) {
    entities.moving_directions[ent] = direction;

    uint32_t scale = (direction & 1) ? 0xB505u : 0x10000u;
    /* STORE_INT1632 zero-extends the speed. */
    int32_t v = (int32_t)((uint32_t)entities.movement_speeds[ent] * scale) >> 8;

    int32_t vx, vy;
    switch (direction) {
    case 0: vx = 0;  vy = -v; break;
    case 1: vx = v;  vy = -v; break;
    case 2: vx = v;  vy = 0;  break;
    case 3: vx = v;  vy = v;  break;
    case 4: vx = 0;  vy = v;  break;
    case 5: vx = -v; vy = v;  break;
    case 6: vx = -v; vy = 0;  break;
    case 7: vx = -v; vy = -v; break;
    default:
        return entities.delta_y[ent];
    }

    entities.delta_x[ent] = (int16_t)((uint32_t)vx >> 16);
    entities.delta_frac_x[ent] = (uint16_t)vx;
    entities.delta_y[ent] = (int16_t)((uint32_t)vy >> 16);
    entities.delta_frac_y[ent] = (uint16_t)vy;
    return entities.delta_y[ent];
}

/*
 * QUANTIZE_DIRECTION (C46B51)
 *
 * Port of asm/misc/quantize_direction.asm. (fine_dir + $1000) / $2000 through
 * QUANTIZED_ANGLE_TO_DIRECTION_TABLE (C46B41). The asm enters the divide at
 * DIVISION16S_DIVISOR_POSITIVE, past DIVISION16S's sign fix-ups, so it is an
 * unsigned 16-bit divide and the index is always 0-7.
 */
int16_t quantize_fine_direction(uint16_t fine_dir) {
    static const int16_t quantized_angle_to_direction[8] = {2, 3, 4, 5, 6, 7, 7, 1};
    uint16_t index = (uint16_t)(fine_dir + 0x1000) / 0x2000;
    return quantized_angle_to_direction[index];
}

/*
 * QUANTIZE_ENTITY_DIRECTION (C46B0A)
 *
 * Port of asm/overworld/entity/quantize_entity_direction.asm. The same
 * unsigned (fine_dir + $1000) / $2000 without the table (0 = up, clockwise),
 * stored to ENTITY_MOVING_DIRECTIONS of CURRENT_ENTITY_SLOT and returned.
 */
int16_t quantize_entity_direction(uint16_t fine_dir) {
    int16_t direction = (int16_t)((uint16_t)(fine_dir + 0x1000) / 0x2000);
    entities.moving_directions[ert.current_entity_slot] = direction;
    return direction;
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

/* Callroutine entry for SET_ENTITY_DIRECTION_VELOCITY (C0C83B): A = tempvar,
 * acts on CURRENT_ENTITY_SLOT. */
int16_t cr_set_entity_direction_velocity(int16_t ent, int16_t scr,
                                                 uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    return set_entity_direction_velocity(ENT(ert.current_entity_slot),
                                         scripts.tempvar[scr]);
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
     * Assembly: STY $94 / JSL SET_ENTITY_DIRECTION_VELOCITY, whose A
     * (the Y delta's integer word) becomes the tempvar. */
    uint8_t dir = sb(pc);
    *out_pc = pc + 1;
    return set_entity_direction_velocity(entity_offset, (int16_t)dir);
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
    return (int16_t)set_entity_velocity_from_direction(
        entity_offset, (uint16_t)scripts.tempvar[script_offset]);
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
    return quantize_entity_direction((uint16_t)scripts.tempvar[script_offset]);
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
    /* 4 extra bytes encoding a text address: [hi_byte, 0, lo_byte, mid_byte].
     * Pack-time remapping converts these from SNES ROM addresses to dialogue
     * blob offsets, so the reconstructed address is a blob offset (0x1xxxxx). */
    uint8_t hi_byte = sb(pc);
    uint8_t lo_byte = sb(pc + 2);
    uint8_t mid_byte = sb(pc + 3);
    *out_pc = pc + 4;

    uint32_t text_addr = ((uint32_t)hi_byte << 16) | ((uint16_t)mid_byte << 8) | lo_byte;

    /* The text box runs as a GAME_MODE_DISPLAY_TEXT child pushed by
     * GAME_MODE_ACTIONSCRIPT_FRAME; the interpreter parks this frame and
     * resumes the script after the text. FLG_TEMP_1 (flag 2, the text-done
     * flag that unblocks script spin loops) is set by the mode's
     * AS_EPI_TEXT_FLAG epilogue at this exact sequence point — after the
     * text, before the next opcode. An unresolvable address skips the child
     * and warns there, matching display_text_from_addr(); probe it here so
     * the no-text case doesn't take a frame boundary at all. */
    ModeState probe;
    if (dt_make_child_init(&probe, text_addr)) {
        actionscript_request_text(text_addr);
    } else {
        LOG_WARN("WARNING: resolve_text_addr(0x%06X) returned NULL\n", text_addr);
        event_flag_set(2);
    }
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
    /* 2 extra bytes: flag ID (word). This is SET_EVENT_FLAG(flag, value),
     * not an unconditional set (asm/overworld/movement/movement_cmd_set_
     * event_flag.asm calls it with the tempvar/A-register as the boolean;
     * asm/text/set_event_flag.asm: LDY value / BEQ @CLEAR_FLAG). The
     * caller primes that boolean immediately beforehand via
     * EVENT_WRITE_WORD_TEMPVAR ($0000 = clear, $0001 = set), same
     * tempvar[script_offset] convention every sibling routine in this
     * file already uses (e.g. cr_movement_cmd_get_event_flag just above,
     * or the direction/speed setters below).
     *
     * Unconditionally calling event_flag_set() here meant a script could
     * never actually clear a flag through this opcode -- confirmed live
     * as the Threed hotel-zombie lure NPC (movement script EVENT_78,
     * asm/data/events/scripts/078.asm) looping her walk-into-hotel
     * animation forever, even after the ambush/dungeon event was long
     * since completed: EVENT_78's WRITE_WORD_TEMPVAR $0000 + this opcode
     * is supposed to clear FLG_THRK_BIKINIZOMBI_F_APPEAR as she hands off
     * to the next stage, but it silently stayed set instead, so her
     * NPC record (gated on that exact flag, port/assets/data/
     * npc_config.json) kept satisfying its spawn condition and re-running
     * its script on every subsequent visit. At least 11 other flag
     * transitions across the game rely on the same clear-half of this
     * opcode and were equally broken (grep the extracted movement scripts
     * for "WRITE_WORD_TEMPVAR $0000" followed by this opcode to find
     * them all), not something specific to Threed. */
    uint16_t flag_id = sw(pc);
    *out_pc = pc + 2;
    if (scripts.tempvar[script_offset] != 0) {
        event_flag_set(flag_id);
    } else {
        event_flag_clear(flag_id);
    }
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
    /* Port of MOVEMENT_CMD_ANIMATE_PALETTE_FADE (C0AAB5) ->
     * ANIMATE_PALETTE_FADE_TO_MAP (asm/system/palette/animate_palette_fade_to_map.asm).
     * Reads 4 bytes: 16-bit palette mask, 8-bit fade style, 8-bit frames.
     *   1. LOAD_PALETTE_TO_FADE_BUFFER(MAP_PALETTE_BACKUP, style) — target.
     *   2. PREPARE_PALETTE_FADE(frames, mask) — per-color slopes.
     *   3. Unless frames == 1: loop `frames` times UPDATE_MAP_PALETTE_ANIMATION
     *      + WAIT_UNTIL_NEXT_FRAME, blocking the script.
     *   4. FINALIZE_PALETTE_FADE + SET_PALETTE_UPLOAD_MODE(24).
     * Steps 1-2 run here; the per-frame loop and the finalize run as a
     * GAME_MODE_PALETTE_FADE (PF_ANIMATE_TO_MAP) child pushed by
     * GAME_MODE_ACTIONSCRIPT_FRAME, which parks this frame and resumes the
     * script after the fade. */
    (void)entity_offset;
    (void)script_offset;
    uint16_t mask = sw(pc);
    uint8_t fade_style = sb(pc + 2);
    uint8_t frames = sb(pc + 3);
    *out_pc = pc + 4;

    load_palette_to_fade_buffer_from(ml.map_palette_backup, fade_style);
    prepare_palette_fade_slopes((int16_t)frames, mask);
    actionscript_request_palette_fade(frames == 1 ? 0 : frames);
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
    for (int s = 0; s < EB_VIEWPORT_HEIGHT && s < screen_y; s++) {
        ppu.wh0_table[s] = 0;
        ppu.wh1_table[s] = 0xFF;
    }

    /* Phase 2: 16 scanlines of spotlight cone (narrows downward) */
    for (int i = 0; i < 16; i++) {
        int s = screen_y + i;
        if (s < 0) continue;
        if (s >= EB_VIEWPORT_HEIGHT) break;
        ppu.wh0_table[s] = (uint8_t)(screen_left + i);
        ppu.wh1_table[s] = (uint8_t)(screen_right - i);
    }

    /* Phase 3: remaining scanlines — empty window (no masking) */
    for (int s = screen_y + 16; s < EB_VIEWPORT_HEIGHT; s++) {
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

/* MAP_DIRECTION_TO_SPRITE_GROUP (asm/overworld/entity/map_direction_to_sprite_group.asm):
 * DIRECTION_TO_SPRITE_GROUP_TABLE lookup. */
static int16_t map_direction_to_sprite_group(int16_t direction) {
    static const int16_t direction_to_sprite_group[8] = {0, 0, 2, 4, 4, 4, 6, 0};
    return direction_to_sprite_group[direction & 7];
}

/* REVERSE_DIRECTION_8 (asm/misc/reverse_direction_8.asm). */
static int16_t reverse_direction_8(int16_t direction) {
    return (int16_t)((direction + 4) & 7);
}

/* Shared tail of MOVE_ENTITY_TOWARD_TARGET and UPDATE_ENTITY_DIRECTION_AND_VELOCITY:
 * store the new facing to ENTITY_DIRECTIONS and re-render the sprite
 * (RENDER_ENTITY_SPRITE_ENTRY2) when its sprite group changes. */
static void set_direction_and_rerender(int16_t slot, int16_t direction) {
    int16_t old_dir = entities.directions[slot];
    entities.directions[slot] = direction;
    if (map_direction_to_sprite_group(old_dir) != map_direction_to_sprite_group(direction))
        render_entity_sprite(slot);
}

/*
 * MOVE_ENTITY_TOWARD_TARGET (C47143)
 *
 * Port of asm/overworld/entity/move_entity_toward_target.asm.
 * Arrived (TRUE) when |V6 - abs_x| and |V7 - abs_y| are both below V5
 * (unsigned CMP/BCS); the deltas are left as they are. Otherwise heads for
 * (V6, V7) with SET_ENTITY_VELOCITY_FROM_DIRECTION and, unless
 * skip_sprite_update, faces the QUANTIZE_ENTITY_DIRECTION result (which also
 * stores ENTITY_MOVING_DIRECTIONS), reversed when reverse is set.
 */
static int16_t move_entity_toward_target(int16_t reverse, int16_t skip_sprite_update) {
    int16_t slot = ert.current_entity_slot;
    uint16_t dist_x = (uint16_t)(entities.var[6][slot] - entities.abs_x[slot]);
    if (dist_x & 0x8000)
        dist_x = (uint16_t)-dist_x;
    if (dist_x < (uint16_t)entities.var[5][slot]) {
        uint16_t dist_y = (uint16_t)(entities.var[7][slot] - entities.abs_y[slot]);
        if (dist_y & 0x8000)
            dist_y = (uint16_t)-dist_y;
        if (dist_y < (uint16_t)entities.var[5][slot])
            return 1;
    }

    /* CALCULATE_DIRECTION_TO_TARGET */
    uint16_t fine_dir = calculate_direction_fine(entities.abs_x[slot], entities.abs_y[slot],
                                                 entities.var[6][slot], entities.var[7][slot]);
    set_entity_velocity_from_direction(slot, fine_dir);
    if (skip_sprite_update)
        return 0;
    int16_t direction = quantize_entity_direction(fine_dir);
    if (reverse)
        direction = reverse_direction_8(direction);
    set_direction_and_rerender(slot, direction);
    return 0;
}

/*
 * UPDATE_ENTITY_DIRECTION_AND_VELOCITY (C472A8)
 *
 * Port of asm/overworld/entity/update_entity_direction_and_velocity.asm.
 * Sets the velocity from V0's fine angle, then faces QUANTIZE_DIRECTION(V0)
 * (the table-mapped direction; ENTITY_MOVING_DIRECTIONS is not written),
 * reversed when reverse is set.
 */
static void update_entity_direction_and_velocity(int16_t reverse) {
    int16_t slot = ert.current_entity_slot;
    uint16_t fine_dir = (uint16_t)entities.var[0][slot];
    set_entity_velocity_from_direction(slot, fine_dir);
    int16_t direction = quantize_fine_direction(fine_dir);
    if (reverse)
        direction = reverse_direction_8(direction);
    set_direction_and_rerender(slot, direction);
}

/* MOVE_TOWARD_TARGET_NO_SPRITE_CALLBACK (A=0, X=1) */
int16_t cr_move_toward_no_sprite_cb(int16_t entity_offset, int16_t script_offset,
                                     uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    return move_entity_toward_target(0, 1);
}

/* MOVE_TOWARD_TARGET_REVERSED_CALLBACK (A=1, X=0) */
int16_t cr_move_toward_reversed_cb(int16_t entity_offset, int16_t script_offset,
                                    uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    return move_entity_toward_target(1, 0);
}

/* UPDATE_DIRECTION_VELOCITY_CALLBACK (C0A8E7): A=0. */
int16_t cr_update_dir_velocity_cb(int16_t entity_offset, int16_t script_offset,
                                   uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    update_entity_direction_and_velocity(0);
    return 0;
}

/* UPDATE_DIRECTION_VELOCITY_REVERSED_CALLBACK (C0A8EF): A=1. */
int16_t cr_update_dir_velocity_reversed_cb(int16_t entity_offset, int16_t script_offset,
                                            uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    update_entity_direction_and_velocity(1);
    return 0;
}

/* MOVE_TOWARD_TARGET_CALLBACK (C0A8C6): A=0, X=0. */
int16_t cr_move_toward_target_cb(int16_t entity_offset, int16_t script_offset,
                                  uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    return move_entity_toward_target(0, 0);
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

    /* Orbit offset from angle at radius $3000. The asm copies VIRTUAL06
     * into LOCAL00, so LOCAL00+2 holds the X component and LOCAL00 the Y. */
    int16_t vel_x, vel_y;
    calculate_velocity_components(ow.mini_ghost_angle, 0x3000, &vel_x, &vel_y);

    /* X: LDA LOCAL00+2; AND #$FF00; XBA; sign-extend; ADC leader_x
     * (high byte of the X component, up to +-0x30 px). */
    int16_t x_offset = (int16_t)(vel_x >> 8);
    entities.abs_x[entity_offset] =
        (int16_t)(game_state.leader_x_coord + (uint16_t)x_offset);

    /* Y: LDA LOCAL00; ASR16 by 10; + leader_y - 8 (up to +-0x0C px). */
    int16_t y_offset = (int16_t)(vel_y >> 10);
    entities.abs_y[entity_offset] =
        (int16_t)(game_state.leader_y_coord - 8 + (uint16_t)y_offset);

    /* Advance angle by 0x0300 per frame */
    ow.mini_ghost_angle += 0x0300;

    /* Show sprite (animation frame 0) */
    entities.animation_frame[entity_offset] = 0;

    return 0;
}
