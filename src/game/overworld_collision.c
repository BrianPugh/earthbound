/*
 * Overworld collision detection functions.
 *
 * Ported from:
 *   CHECK_ENTITY_ENEMY_COLLISION         — asm/overworld/collision/check_entity_enemy_collision.asm
 *   CHECK_CURRENT_ENTITY_OBSTACLES       — asm/overworld/collision/check_current_entity_obstacles.asm
 *   CHECK_ENTITY_COLLISION_AT_POSITION   — asm/overworld/collision/check_entity_collision_at_position.asm
 *   CHECK_PROSPECTIVE_ENTITY_COLLISION   — asm/overworld/collision/check_prospective_entity_collision.asm
 *   CHECK_ENTITY_AND_NPC_COLLISION       — asm/overworld/collision/check_entity_and_npc_collision.asm
 *   CHECK_PROSPECTIVE_NPC_COLLISION      — asm/overworld/collision/check_prospective_npc_collision.asm
 *   CHECK_ENEMY_MOVEMENT_OBSTACLES       — asm/overworld/collision/check_enemy_movement_obstacles.asm
 *   CHECK_NPC_PLAYER_OBSTACLES           — asm/overworld/collision/check_npc_player_obstacles.asm
 *   NPC_COLLISION_CHECK                  — asm/overworld/npc_collision_check.asm
 *   CLEAR_ENTITY_DELTA_MOTION            — C09907
 *   CHECK_COLLISION_IN_DIRECTION         — C042EF
 *   CHECK_DIRECTIONAL_NPC_COLLISION      — C04116
 *   FIND_CLEAR_DIRECTION_FOR_LEADER      — C043BC
 */

#include "game/overworld_internal.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "game/door.h"
#include "game/audio.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "snes/ppu.h"
#include "include/constants.h"
#include "core/math.h"
#include "game_main.h"

/* ===== NPC INTERACTION CHAIN =====
 *
 * Direction offset tables used by CHECK_COLLISION_IN_DIRECTION and
 * CHECK_DIRECTIONAL_NPC_COLLISION to probe positions near the leader.
 *
 * INTERACTION_X_OFFSETS: X pixel offsets indexed by direction (0-7).
 * INTERACTION_Y_OFFSETS: Y pixel offsets indexed by direction (0-7).
 * DIRECTION_FACING_TABLE: Direction-to-facing table (makes NPC face player).
 *
 * From asm/data/unknown/C3E148.asm, C3E158.asm, C3E168.asm.
 */

/* X offsets: UP=0, UP_RIGHT=0, RIGHT=10, DOWN_RIGHT=0,
 *            DOWN=0, DOWN_LEFT=0, LEFT=-10, UP_LEFT=0 */
const int16_t interaction_x_offsets[8] = {
    0, 0, 10, 0, 0, 0, -10, 0
};

/* Y offsets: UP=-5, UP_RIGHT=-5, RIGHT=0, DOWN_RIGHT=5,
 *            DOWN=5, DOWN_LEFT=5, LEFT=0, UP_LEFT=-5 */
const int16_t interaction_y_offsets[8] = {
    -5, -5, 0, 5, 5, 5, 0, -5
};

/* Maps leader direction to the direction an NPC should face toward leader.
 * Diagonal directions map to the nearest cardinal opposite. */
const int16_t direction_facing_table[8] = {
    4, 4, 6, 0, 0, 0, 2, 4
};

/* ---- CHECK_ENTITY_ENEMY_COLLISION (asm/overworld/collision/check_entity_enemy_collision.asm) ----
 * Checks if the current entity has collided with an enemy entity.
 * Returns:
 *   0 if player is in special movement state (bit 1 of ow.player_movement_flags)
 *  -1 if leader entity (slot 23) collided with this entity
 *  -1 if entity's collision object is in NPC/enemy range (>= 0x17, < 0x8000)
 *   0 otherwise */
int16_t check_entity_enemy_collision(void) {
    /* If player is in special movement state, no collision */
    if (ow.player_movement_flags & 0x0002)
        return 0;

    /* Check if init entity (slot 23) collided with current entity.
     * Slot 23 is the invisible "init entity" that runs the overworld tick and
     * performs NPC collision detection — NOT the party leader (slot 24). */
    if (entities.collided_objects[ENT(INIT_ENTITY_SLOT)] == ert.current_entity_slot)
        return -1;

    /* Check current entity's collision object */
    int16_t offset = ENT(ert.current_entity_slot);
    int8_t collided = entities.collided_objects[offset];

    /* Assembly: CMP #$7FFF; BGT (unsigned >); CMP #$0017; BCS (unsigned >=).
     * Collision valid only if 0x0017 <= collided <= MAX_ENTITIES-1.
     * Negative values are sentinels (DISABLED=-128, NO_OBJECT=-1). */
    if (collided < 0 || collided < 0x17)
        return 0;

    /* Collision with an NPC/enemy entity (slot 0x17 or higher) */
    return -1;
}

/* ---- CHECK_CURRENT_ENTITY_OBSTACLES (asm/overworld/collision/check_current_entity_obstacles.asm) ----
 * Wraps CHECK_ENTITY_OBSTACLE_FLAGS for the current entity slot.
 * Assembly: LDA CURRENT_ENTITY_SLOT; JSR CHECK_ENTITY_OBSTACLE_FLAGS; AND #$00FF */
int16_t check_current_entity_obstacles(void) {
    /* check_entity_obstacle_flags is static in map_loader.c;
     * replicate the thin wrapper here. */
    int result = calculate_prospective_position(ert.current_entity_slot);
    if (result == 0) {
        return (int16_t)0xFF00;
    }

    int16_t offset = ENT(ert.current_entity_slot);
    int16_t direction = entities.directions[offset];

    uint16_t flags = check_entity_collision(
        ow.entity_movement_prospective_x,
        ow.entity_movement_prospective_y,
        ert.current_entity_slot, direction);

    flags &= 0x00D0;
    entities.obstacle_flags[offset] = flags;

    return (int16_t)(flags & 0x00FF);
}

/*
 * check_entity_collision_at_position — entity-vs-entity AABB collision.
 *
 * Port of CHECK_ENTITY_COLLISION_AT_POSITION (asm/overworld/collision/check_entity_collision_at_position.asm).
 *
 * Tests the given entity's hitbox at position (x, y) against all other
 * active entities. Hitbox dimensions depend on facing direction (LR vs UD).
 * Returns the colliding entity slot (0-29), or -1 (no collision).
 * Stores result in entities.collided_objects[entity_slot].
 */
int16_t check_entity_collision_at_position(int16_t x, int16_t y,
                                                   int16_t entity_slot) {
    int16_t result = -1;  /* ENTITY_COLLISION_NO_OBJECT */
    int16_t offset = entity_slot;

    /* Skip if hitbox not enabled for self */
    if (!entities.hitbox_enabled[offset])
        goto done;

    /* Get self hitbox dimensions based on facing direction */
    uint16_t self_width, self_height;
    int16_t dir = entities.directions[offset];
    if (dir == 2 /* RIGHT */ || dir == 6 /* LEFT */) {
        self_width = entities.hitbox_lr_widths[offset];
        self_height = entities.hitbox_lr_heights[offset];
    } else {
        self_width = entities.hitbox_ud_widths[offset];
        self_height = entities.hitbox_ud_heights[offset];
    }

    /* Compute self bounding box */
    int16_t self_left = x - (int16_t)self_width;
    int16_t self_box_width = (int16_t)(self_width * 2);
    int16_t self_top = y - (int16_t)self_height;

    /* Check against all entities */
    for (int16_t i = 0; i < 30; i++) {
        if (i == entity_slot) continue;
        if (i == 23) continue;  /* skip init entity */

        int16_t ioff = i;

        /* Skip inactive entities */
        if (entities.script_table[ioff] == -1) continue;
        if (entities.collided_objects[ioff] == ENTITY_COLLISION_DISABLED) continue;
        if (!entities.hitbox_enabled[ioff]) continue;

        /* Get other hitbox dimensions */
        uint16_t other_width, other_height;
        int16_t other_dir = entities.directions[ioff];
        if (other_dir == 2 /* RIGHT */ || other_dir == 6 /* LEFT */) {
            other_width = entities.hitbox_lr_widths[ioff];
            other_height = entities.hitbox_lr_heights[ioff];
        } else {
            other_width = entities.hitbox_ud_widths[ioff];
            other_height = entities.hitbox_ud_heights[ioff];
        }

        /* Y-axis overlap test */
        int16_t other_top = entities.abs_y[ioff] - (int16_t)other_height;
        /* If other_top - self_height >= self_top (unsigned): no overlap */
        if ((uint16_t)(other_top - (int16_t)self_height) >= (uint16_t)self_top)
            continue;
        /* If other_top + other_height <= self_top: no overlap */
        int16_t other_bottom_ish = other_top + (int16_t)other_height;
        if (other_bottom_ish <= self_top)
            continue;

        /* X-axis overlap test */
        int16_t other_left = entities.abs_x[ioff] - (int16_t)other_width;
        /* If other_left - self_box_width >= self_left (unsigned): no overlap */
        if ((uint16_t)(other_left - self_box_width) >= (uint16_t)self_left)
            continue;
        /* If other_left + other_width*2 <= self_left: no overlap */
        int16_t other_right_edge = other_left + (int16_t)(other_width * 2);
        if (other_right_edge <= self_left)
            continue;

        /* Collision detected */
        result = i;
        break;
    }

done:
    entities.collided_objects[offset] = result;
    return result;
}

/* ---- CHECK_PROSPECTIVE_ENTITY_COLLISION (asm/overworld/collision/check_prospective_entity_collision.asm) ----
 * Checks if the entity's next-frame position would collide with another entity.
 * Calls CALCULATE_PROSPECTIVE_POSITION then CHECK_ENTITY_COLLISION_AT_POSITION. */
void check_prospective_entity_collision(void) {
    int16_t offset = ENT(ert.current_entity_slot);

    /* Skip if collision disabled for this entity */
    if (entities.collided_objects[offset] == ENTITY_COLLISION_DISABLED)
        return;

    /* Calculate where entity will be next frame */
    calculate_prospective_position(ert.current_entity_slot);

    /* Check entity-vs-entity collision at the prospective position */
    check_entity_collision_at_position(
        ow.entity_movement_prospective_x,
        ow.entity_movement_prospective_y,
        ert.current_entity_slot);
}

/* CHECK_ENTITY_AND_NPC_COLLISION (asm/overworld/collision/check_entity_and_npc_collision.asm).
 * Like check_entity_collision_at_position but checks two specific entity ranges:
 *   1. Party entities (slots 24-29) — skipped if player_intangibility_frames != 0
 *   2. NPC entities (slots 0-22) — only those with npc_id < 0x1000
 * The NPC overlap test is 1 pixel tighter (DEC) than the party test.
 * Stores result in entities.collided_objects[entity_slot]. */
int16_t check_entity_and_npc_collision(int16_t x, int16_t y,
                                               int16_t entity_slot) {
    int16_t result = -1;  /* ENTITY_COLLISION_NO_OBJECT */
    int16_t offset = entity_slot;

    if (!entities.hitbox_enabled[offset])
        goto store_result;

    /* Get self hitbox dimensions based on facing direction */
    uint16_t self_width, self_height;
    int16_t dir = entities.directions[offset];
    if (dir == 2 /* RIGHT */ || dir == 6 /* LEFT */) {
        self_width = entities.hitbox_lr_widths[offset];
        self_height = entities.hitbox_lr_heights[offset];
    } else {
        self_width = entities.hitbox_ud_widths[offset];
        self_height = entities.hitbox_ud_heights[offset];
    }

    /* Compute self bounding box */
    int16_t self_left = x - (int16_t)self_width;
    int16_t self_box_width = (int16_t)(self_width * 2);
    int16_t self_top = y - (int16_t)self_height;

    /* Party entity loop (slots 24-29) — skip if player is intangible */
    if (ow.player_intangibility_frames == 0) {
        for (int16_t i = 24; i < MAX_ENTITIES; i++) {
            int16_t ioff = i;

            if (entities.script_table[ioff] == -1) continue;
            if (entities.collided_objects[ioff] == ENTITY_COLLISION_DISABLED) continue;
            if (!entities.hitbox_enabled[ioff]) continue;

            uint16_t other_width, other_height;
            int16_t other_dir = entities.directions[ioff];
            if (other_dir == 2 || other_dir == 6) {
                other_width = entities.hitbox_lr_widths[ioff];
                other_height = entities.hitbox_lr_heights[ioff];
            } else {
                other_width = entities.hitbox_ud_widths[ioff];
                other_height = entities.hitbox_ud_heights[ioff];
            }

            /* Y-axis overlap */
            int16_t other_top = entities.abs_y[ioff] - (int16_t)other_height;
            if ((uint16_t)(other_top - (int16_t)self_height) >= (uint16_t)self_top)
                continue;
            int16_t other_bottom = other_top + (int16_t)other_height;
            if ((uint16_t)self_top >= (uint16_t)other_bottom)
                continue;

            /* X-axis overlap */
            int16_t other_left = entities.abs_x[ioff] - (int16_t)other_width;
            int16_t other_box_width = (int16_t)(other_width * 2);
            if ((uint16_t)(other_left - self_box_width) >= (uint16_t)self_left)
                continue;
            int16_t other_right = other_left + other_box_width;
            if ((uint16_t)self_left >= (uint16_t)other_right)
                continue;

            result = i;
            goto store_result;
        }
    }

    /* NPC entity loop (slots 0-22) — with 1-pixel-tighter overlap tests (DEC) */
    for (int16_t i = 0; i < 23; i++) {
        if (i == entity_slot) continue;

        int16_t ioff = i;

        if (entities.script_table[ioff] == -1) continue;
        if (entities.npc_ids[ioff] >= 0x1000) continue;
        if (entities.collided_objects[ioff] == ENTITY_COLLISION_DISABLED) continue;
        if (!entities.hitbox_enabled[ioff]) continue;

        uint16_t other_width, other_height;
        int16_t other_dir = entities.directions[ioff];
        if (other_dir == 2 || other_dir == 6) {
            other_width = entities.hitbox_lr_widths[ioff];
            other_height = entities.hitbox_lr_heights[ioff];
        } else {
            other_width = entities.hitbox_ud_widths[ioff];
            other_height = entities.hitbox_ud_heights[ioff];
        }

        /* Y-axis overlap (DEC = 1 pixel tighter on bottom) */
        int16_t other_top = entities.abs_y[ioff] - (int16_t)other_height;
        if ((uint16_t)(other_top - (int16_t)self_height) >= (uint16_t)self_top)
            continue;
        int16_t other_bottom = other_top + (int16_t)other_height - 1;
        if ((uint16_t)self_top >= (uint16_t)other_bottom)
            continue;

        /* X-axis overlap (DEC = 1 pixel tighter on right) */
        int16_t other_left = entities.abs_x[ioff] - (int16_t)other_width;
        int16_t other_box_width = (int16_t)(other_width * 2);
        if ((uint16_t)(other_left - self_box_width) >= (uint16_t)self_left)
            continue;
        int16_t other_right = other_left + other_box_width - 1;
        if ((uint16_t)self_left >= (uint16_t)other_right)
            continue;

        result = i;
        goto store_result;
    }

store_result:
    entities.collided_objects[offset] = result;
    return result;
}

/* CHECK_PROSPECTIVE_NPC_COLLISION (asm/overworld/collision/check_prospective_npc_collision.asm).
 * Calculates entity's next-frame position and checks for collision with
 * party entities and NPCs. Updates ENTITY_COLLIDED_OBJECTS. */
void check_prospective_npc_collision(void) {
    int16_t entity_slot = ert.current_entity_slot;
    int16_t offset = entity_slot;

    if (entities.collided_objects[offset] == ENTITY_COLLISION_DISABLED)
        return;

    calculate_prospective_position(entity_slot);
    check_entity_and_npc_collision(
        ow.entity_movement_prospective_x,
        ow.entity_movement_prospective_y,
        entity_slot);
}

/* CHECK_ENEMY_MOVEMENT_OBSTACLES (asm/overworld/collision/check_enemy_movement_obstacles.asm).
 * Checks tile collision at prospective position, then enemy-specific run restrictions.
 * Called from enemy collision check loop. */
int16_t check_enemy_movement_obstacles(void) {
    int16_t entity_slot = ert.current_entity_slot;

    int16_t result = check_current_entity_obstacles();
    if (result == (int16_t)0xFF00)
        return 0;  /* invalid position */
    if (result != 0)
        return 0;  /* has map obstacle (already stored in obstacle_flags) */

    /* No map obstacle — check enemy-specific run restrictions */
    int16_t offset = entity_slot;
    int16_t enemy_id = entities.enemy_ids[offset];
    int16_t run_result = can_enemy_run_in_direction(0, enemy_id);
    entities.obstacle_flags[offset] |= run_result;
    return 0;
}

/* CHECK_NPC_PLAYER_OBSTACLES (asm/overworld/collision/check_npc_player_obstacles.asm).
 * Checks tile collision at prospective position using player-style (both horizontal
 * edges), then enemy-specific run restrictions. Called from NPC collision check loop. */
int16_t check_npc_player_obstacles(void) {
    int16_t entity_slot = ert.current_entity_slot;

    int result = calculate_prospective_position(entity_slot);
    if (result == 0)
        return 0;  /* entity didn't move */

    uint16_t flags = check_player_collision_at_position(
        ow.entity_movement_prospective_x,
        ow.entity_movement_prospective_y,
        entity_slot);
    flags &= 0x00D0;

    int16_t offset = entity_slot;
    entities.obstacle_flags[offset] = flags;

    if (flags != 0)
        return 0;  /* has tile obstacle, already stored */

    /* No tile obstacle — check enemy-specific run restrictions */
    int16_t enemy_id = entities.enemy_ids[offset];
    int16_t run_result = can_enemy_run_in_direction(0, enemy_id);
    entities.obstacle_flags[offset] |= run_result;
    return 0;
}

/*
 * npc_collision_check — check leader hitbox against all NPC entities.
 *
 * Port of NPC_COLLISION_CHECK (asm/overworld/npc_collision_check.asm).
 * Performs AABB overlap test between the leader entity's hitbox at the
 * given (x, y) position and each active NPC entity (slots 0-22).
 *
 * The hitbox dimensions depend on the entity's current facing direction:
 * LEFT/RIGHT use the LR hitbox, UP/DOWN use the UD hitbox. The hitbox
 * extends "width" pixels left and right of center, and "height" pixels
 * above center (so the center is at the entity's feet).
 *
 * During intangibility frames, enemy NPCs (NPC ID >= 0x8000) are skipped
 * but regular NPCs still collide.
 *
 * Result is stored in entities.collided_objects[23] (init entity, slot 23).
 */
int16_t npc_collision_check(int16_t x, int16_t y, int16_t leader_slot) {
    int16_t result = -1;  /* ENTITY_COLLISION_NO_OBJECT */

    /* Lines 21-25: Check leader hitbox enabled */
    int16_t leader_offset = ENT(leader_slot);
    if (!entities.hitbox_enabled[leader_offset])
        goto done;

    /* Lines 26-28: Skip during special movement states */
    if (ow.player_movement_flags & 0x0002)
        goto done;

    /* Lines 29-31: Skip on escalator */
    if (game_state.walking_style == WALKING_STYLE_ESCALATOR)
        goto done;

    /* Lines 32-33: Skip in demo mode */
    if (ow.demo_frames_left)
        goto done;

    /* Lines 34-52: Get leader hitbox dimensions based on direction.
     * LEFT/RIGHT use LR hitbox, everything else uses UD hitbox. */
    uint16_t leader_width, leader_height;
    uint16_t leader_dir = entities.directions[leader_offset];
    if (leader_dir == 2 || leader_dir == 6) {  /* RIGHT or LEFT */
        leader_width = entities.hitbox_lr_widths[leader_offset];
        leader_height = entities.hitbox_lr_heights[leader_offset];
    } else {
        leader_width = entities.hitbox_ud_widths[leader_offset];
        leader_height = entities.hitbox_ud_heights[leader_offset];
    }

    /* Lines 53-68: Compute leader collision bounds.
     * left_edge = x - width; width_2x = width * 2; top_edge = y - height */
    int16_t leader_left = x - (int16_t)leader_width;
    int16_t leader_width_2x = (int16_t)(leader_width * 2);
    int16_t leader_top = y - (int16_t)leader_height;

    /* Lines 73-164: Loop through entities 0-22 */
    for (int slot = 0; slot < 23; slot++) {
        int16_t offset = slot;

        /* Line 77-79: Skip inactive entities (no script assigned) */
        if (entities.script_table[offset] == -1)
            continue;

        /* Lines 80-82: Skip collision-disabled entities */
        if (entities.collided_objects[offset] == ENTITY_COLLISION_DISABLED)
            continue;

        /* Lines 83-89: During intangibility, skip enemies (NPC ID >= 0x8000) */
        if (ow.player_intangibility_frames) {
            uint16_t npc_id = entities.npc_ids[offset];
            if ((uint16_t)(npc_id + 1) >= 0x8001) {
                /* NPC ID >= 0x8000 means enemy — skip during intangibility */
                continue;
            }
        }

        /* Lines 94-95: Skip entities with hitbox disabled */
        if (!entities.hitbox_enabled[offset])
            continue;

        /* Lines 96-112: Get NPC hitbox dimensions based on direction */
        uint16_t npc_width, npc_height;
        uint16_t npc_dir = entities.directions[offset];
        if (npc_dir == 2 || npc_dir == 6) {  /* RIGHT or LEFT */
            npc_width = entities.hitbox_lr_widths[offset];
            npc_height = entities.hitbox_lr_heights[offset];
        } else {
            npc_width = entities.hitbox_ud_widths[offset];
            npc_height = entities.hitbox_ud_heights[offset];
        }

        /* Lines 117-131: Y overlap check.
         * NPC top = NPC_Y - NPC_height
         * Check: (NPC_top - leader_height) >= leader_top → no overlap (NPC too low)
         * Check: NPC_Y <= leader_top → no overlap (NPC too high) */
        int16_t npc_y = entities.abs_y[offset];
        int16_t npc_top = npc_y - (int16_t)npc_height;
        if ((uint16_t)(npc_top - (int16_t)leader_height) >= (uint16_t)leader_top)
            continue;
        if ((npc_top + (int16_t)npc_height) <= leader_top)
            continue;

        /* Lines 132-150: X overlap check.
         * NPC left = NPC_X - NPC_width
         * Check: (NPC_left - leader_width*2) >= leader_left → no overlap
         * Check: (NPC_left + NPC_width*2) <= leader_left → no overlap */
        int16_t npc_x = entities.abs_x[offset];
        int16_t npc_left = npc_x - (int16_t)npc_width;
        int16_t npc_width_2x = (int16_t)(npc_width * 2);
        if ((uint16_t)(npc_left - leader_width_2x) >= (uint16_t)leader_left)
            continue;
        if ((npc_left + npc_width_2x) <= leader_left)
            continue;

        /* Lines 151-154: Collision found! */
        result = (int16_t)slot;
        break;
    }

done:
    /* Line 167: Store in init entity's collision slot */
    entities.collided_objects[23] = result;
    return result;
}

/* ---- CLEAR_ENTITY_DELTA_MOTION (port of C09907) ----
 *
 * Zeros all 6 delta motion components (X/Y/Z integer + fraction) for a slot. */
void clear_entity_delta_motion(int16_t slot) {
    int16_t offset = slot;
    entities.delta_x[offset] = 0;
    entities.delta_y[offset] = 0;
    entities.delta_z[offset] = 0;
}

/* ---- CHECK_COLLISION_IN_DIRECTION (port of C042EF) ----
 *
 * Checks for NPC collision at a position offset from the leader in the
 * given direction. Uses interaction_x/y_offsets tables for probe position.
 * Temporarily sets ow.player_intangibility_frames = 1 to skip enemy NPCs.
 *
 * If NPC found: sets ow.interacting_npc_id and ow.interacting_npc_entity.
 * If entity collision (surface flags 0x82) found: nudges by ±8 and retries.
 * If nothing found: calls CHECK_DOOR_IN_DIRECTION.
 *
 * Returns ow.interacting_npc_id. */
int16_t check_collision_in_direction(int16_t direction) {
    int16_t dir_x = interaction_x_offsets[direction & 7];
    int16_t dir_y = interaction_y_offsets[direction & 7];

    int16_t check_x = (int16_t)game_state.leader_x_coord + dir_x;
    int16_t check_y = (int16_t)game_state.leader_y_coord + dir_y;

    /* Save and temporarily override intangibility */
    uint16_t saved_intangibility = ow.player_intangibility_frames;
    ow.player_intangibility_frames = 1;

    for (;;) {
        /* Try NPC collision at check position */
        int16_t result = npc_collision_check(check_x, check_y,
                                             game_state.current_party_members);
        if ((uint16_t)result < 0x8000) {
            /* NPC found! Store ID and entity slot */
            ow.interacting_npc_id = entities.npc_ids[result];
            ow.interacting_npc_entity = result;
            break;
        }

        /* No NPC — check entity/terrain collision */
        uint16_t surface = check_entity_collision(
            check_x, check_y,
            game_state.current_party_members, direction);

        if ((surface & 0x0082) != 0x0082) {
            /* No wall/entity collision — stop searching */
            break;
        }

        /* Surface collision 0x82 found — nudge check position by ±8 pixels
         * in the direction's axis and retry NPC check */
        if (dir_x != 0) {
            check_x += (dir_x < 0) ? -8 : 8;
        }
        if (dir_y == 0) {
            continue;  /* retry with adjusted X */
        }
        check_y += (dir_y < 0) ? -8 : 8;
    }

    /* Restore intangibility */
    ow.player_intangibility_frames = saved_intangibility;

    /* If no NPC found (0 or -1), check for doors */
    if (ow.interacting_npc_id == 0 || ow.interacting_npc_id == 0xFFFF) {
        check_door_in_direction(direction);
    }

    return (int16_t)ow.interacting_npc_id;
}

/* ---- CHECK_DIRECTIONAL_NPC_COLLISION (port of C04116) ----
 *
 * Similar to check_collision_in_direction but uses the direction offset
 * tables for nudge adjustments (re-reads from table each iteration)
 * and calls CHECK_DOOR_NEAR_LEADER instead of CHECK_DOOR_IN_DIRECTION. */
int16_t check_directional_npc_collision(int16_t direction) {
    int16_t check_x = (int16_t)game_state.leader_x_coord
                     + interaction_x_offsets[direction & 7];
    int16_t check_y = (int16_t)game_state.leader_y_coord
                     + interaction_y_offsets[direction & 7];

    /* Save and temporarily override intangibility */
    uint16_t saved_intangibility = ow.player_intangibility_frames;
    ow.player_intangibility_frames = 1;

    for (;;) {
        /* Try NPC collision at check position */
        int16_t result = npc_collision_check(check_x, check_y,
                                             game_state.current_party_members);
        if ((uint16_t)result < 0x8000) {
            /* NPC found */
            ow.interacting_npc_id = entities.npc_ids[result];
            ow.interacting_npc_entity = result;
            break;
        }

        /* No NPC — check entity/terrain collision */
        uint16_t surface = check_entity_collision(
            check_x, check_y,
            game_state.current_party_members, direction);

        if ((surface & 0x0082) != 0x0082) {
            break;
        }

        /* Nudge by ±8 using direction table signs (re-read from table) */
        int16_t tbl_x = interaction_x_offsets[direction & 7];
        if (tbl_x != 0) {
            check_x += (tbl_x < 0) ? -8 : 8;
        }
        int16_t tbl_y = interaction_y_offsets[direction & 7];
        if (tbl_y == 0) {
            continue;
        }
        check_y += (tbl_y < 0) ? -8 : 8;
    }

    /* Restore intangibility */
    ow.player_intangibility_frames = saved_intangibility;

    /* If no NPC found, check for doors near leader */
    if (ow.interacting_npc_id == 0xFFFF || ow.interacting_npc_id == 0) {
        check_door_near_leader(direction);
    }

    return (int16_t)ow.interacting_npc_id;
}

/* ---- FIND_CLEAR_DIRECTION_FOR_LEADER (port of C043BC) ----
 *
 * Tries 4 directions (current cardinal, CW+90, CCW+90, opposite) looking
 * for an NPC via check_collision_in_direction. Updates leader_direction
 * to face the found NPC. Returns the direction found, or -1 if none. */
int16_t find_clear_direction_for_leader(void) {
    int16_t orig_dir = game_state.leader_direction & 0xFFFE;  /* force even */
    int16_t dir = orig_dir;

    /* Try current direction */
    int16_t result = check_collision_in_direction(dir);
    if (result != -1 && result != 0)
        return dir;

    /* Try clockwise 90° */
    dir = (dir + 2) & 7;
    game_state.leader_direction = dir;
    result = check_collision_in_direction(dir);
    if (result != -1 && result != 0)
        return dir;

    /* Try counterclockwise 90° (CW+180 from last = CW+90 from CW+90) */
    dir = (dir + 4) & 7;
    game_state.leader_direction = dir;
    result = check_collision_in_direction(dir);
    if (result != -1 && result != 0)
        return dir;

    /* Try opposite (CCW 90° from CCW = back 2 from current) */
    dir = (dir - 2) & 7;
    game_state.leader_direction = dir;
    result = check_collision_in_direction(dir);
    if (result != -1 && result != 0)
        return dir;

    /* Nothing found — restore original direction */
    game_state.leader_direction = orig_dir;
    return -1;
}
