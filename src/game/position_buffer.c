/*
 * PLAYER_POSITION_BUFFER — circular history buffer for follow-the-leader.
 *
 * See position_buffer.h for overview.
 *
 * Ports of:
 *   UPDATE_LEADER_MOVEMENT          (asm/overworld/update_leader_movement.asm)
 *   UPDATE_FOLLOWER_STATE           (asm/overworld/update_follower_state.asm)
 *   ADJUST_PARTY_MEMBER_VISIBILITY  (asm/overworld/party/adjust_party_member_visibility.asm)
 *   GET_DISTANCE_TO_PARTY_MEMBER    (asm/overworld/party/get_distance_to_party_member.asm)
 *   GET_PREVIOUS_POSITION_INDEX     (asm/overworld/get_previous_position_index.asm)
 *   SYNC_CAMERA_TO_ENTITY           (asm/overworld/camera/sync_camera_to_entity.asm)
 *   UPDATE_SPECIAL_CAMERA_MODE      (asm/overworld/camera/update_special_camera_mode.asm)
 *   UPDATE_FOLLOWER_VISUALS         (asm/overworld/party/update_follower_visuals.asm)
 *   UPDATE_PARTY_ENTITY_GRAPHICS    (asm/overworld/party/update_party_entity_graphics.asm)
 *   GET_PARTY_MEMBER_SPRITE_ID      (asm/overworld/party/get_party_member_sprite_id.asm)
 */
#include "game/position_buffer.h"
#include "game/battle.h"
#include "game/door.h"
#include "include/binary.h"
#include "game/game_state.h"
#include "game/overworld.h"
#include "game/map_loader.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "snes/ppu.h"
#include "core/memory.h"
#include "include/constants.h"
#include "include/pad.h"
#include "game/audio.h"
#include "data/assets.h"
#include <string.h>

PositionBufferState pb;

/* pb.camera_mode_backup now in PositionBufferState pb. */

/* Forward declarations (defined later in file) */
int16_t map_input_to_direction(uint16_t walking_style);
static int32_t adjust_position(int16_t direction, uint16_t surface_flags,
                               int32_t pos, const int32_t *speeds);

/* CHARACTER_SIZES — loaded from ROM via asset pipeline.
 * 17 entries × 2 bytes (uint16_t LE), indexed by char_id. */
#define CHARACTER_SIZES_COUNT 17

/* PLAYABLE_CHAR_GFX_TABLE — loaded from ROM via asset pipeline.
 * 17 rows × 8 columns × 2 bytes (uint16_t LE).
 * Maps (char_id, mode) to OVERWORLD_SPRITE ID. */
#define PLAYABLE_CHAR_ROW_COUNT 17
#define PLAYABLE_CHAR_COL_COUNT 8

/* Pure table lookup for PLAYABLE_CHAR_GFX_TABLE.
 * Port of GET_SPRITE_VARIANT_FROM_FLAGS table access without side effects. */
uint16_t lookup_playable_char_sprite(uint16_t char_id, uint16_t mode) {
    if (char_id >= PLAYABLE_CHAR_ROW_COUNT) return 0xFFFF;
    if (mode >= PLAYABLE_CHAR_COL_COUNT) return 0xFFFF;
    const uint8_t *data = ASSET_DATA(ASSET_DATA_PLAYABLE_CHARACTER_GRAPHICS_TABLE_BIN);
    size_t offset = ((size_t)char_id * PLAYABLE_CHAR_COL_COUNT + mode) * 2;
    return read_u16_le(data + offset);
}

/* Status constants: use enums from battle.h (StatusGroup, Status0, Status1) */

/*
 * get_party_member_sprite_id — determine which sprite to show for a party member.
 *
 * Port of GET_PARTY_MEMBER_SPRITE_ID (C0780F).
 * Returns the OVERWORLD_SPRITE ID, or -1 (0xFFFF) to hide the entity.
 * Side effect: sets entity var3 (animation speed) based on terrain and status.
 */
int16_t get_party_member_sprite_id(
    int16_t char_id, uint16_t walking_style,
    int16_t entity_offset, int16_t party_idx)
{
    uint16_t mode = 0;  /* Y register in assembly — visual mode index */

    /* Pajamas check (assembly lines 16-26).
     * If char_id == 0 (Ness), DISABLED_TRANSITIONS == 0, PAJAMA_FLAG != 0:
     * return NESS_IN_PJS (437). */
    if (char_id == CHARACTER_NESS && ow.disabled_transitions == 0 && ow.pajama_flag != 0)
        return 437;  /* OVERWORLD_SPRITE::NESS_IN_PJS */

    /* Clear overlay flags (assembly lines 28-34). */
    if (entity_offset >= 0) {
        entities.overlay_flags[entity_offset] = 0;
    }

    /* Party status: charred/burned (assembly @UNKNOWN1-2) */
    if ((game_state.party_status & 0xFF) == 1) {
        if (game_state.character_mode == CHARACTER_MODE_SMALL) return 37;  /* LIL_CHARRED_GUY */
        return 13;  /* HUMAN_CHARRED */
    }

    /* Status affliction checks (assembly @UNKNOWN3-10) */
    if (party_idx >= 0 && party_idx < TOTAL_PARTY_COUNT) {
        uint8_t status0 = party_characters[party_idx].afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
        uint8_t status1 = party_characters[party_idx].afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL];

        if (status0 == STATUS_0_UNCONSCIOUS) {
            mode = 1;  /* angel/unconscious visual */
        } else if (status0 == STATUS_0_DIAMONDIZED) {
            if (game_state.character_mode == CHARACTER_MODE_SMALL) return 36;  /* LIL_DIAMONDIZED_GUY */
            return 12;  /* HUMAN_DIAMONDIZED */
        } else if (status0 == STATUS_0_NAUSEOUS) {
            if (entity_offset >= 0)
                entities.overlay_flags[entity_offset] |= 0x02;
        }

        if (status1 == STATUS_1_MUSHROOMIZED) {
            if (entity_offset >= 0)
                entities.overlay_flags[entity_offset] |= 0x01;
        } else if (status1 == STATUS_1_POSSESSED) {
            ow.possessed_player_count++;
        }
    }

    /* Vehicle/movement mode (assembly @UNKNOWN11-13) */
    if (game_state.character_mode == CHARACTER_MODE_BICYCLE) return 7;   /* NESS_BICYCLE */
    if (game_state.character_mode == CHARACTER_MODE_GHOST) {
        /* Ghost mode: if char_struct.unknown53 == 0, return special sprite */
        if (party_idx >= 0 && party_idx < TOTAL_PARTY_COUNT &&
            party_characters[party_idx].unknown53 == 0) {
            return 6;  /* Assembly returns OVERWORLD_SPRITE::NESS_PAJAMAS for this mode */
        }
    }

    /* Walking style → visual mode mapping (assembly @UNKNOWN14-18).
     * Only applied when mode == 0 (not already set to unconscious). */
    if (mode == 0) {
        switch (walking_style) {
        case WALKING_STYLE_NORMAL:
        case WALKING_STYLE_ESCALATOR:
        case WALKING_STYLE_STAIRS:
            mode = 0; break;
        case WALKING_STYLE_GHOST:
            mode = 1; break;
        case WALKING_STYLE_LADDER:
            mode = 2; break;
        case WALKING_STYLE_ROPE:
            mode = 3; break;
        default:
            mode = 0; break;
        }
    }

    /* Small party mode adjustments (assembly @UNKNOWN19-20) */
    if (game_state.character_mode == CHARACTER_MODE_SMALL) {
        mode += 4;  /* Small variants (columns 4-7) */
        /* Assembly clears overlay flags in small mode (lines 158-161) */
        if (entity_offset >= 0)
            entities.overlay_flags[entity_offset] = 0;
    } else if (game_state.character_mode == CHARACTER_MODE_ROBOT && mode == 0) {
        mode += 6;  /* Robot variants (column 6) */
    }

    /* Set var3 animation speed (assembly @UNKNOWN21-28).
     * Controls how many frames between animation frame changes.
     * Lower = faster animation. */
    if (entity_offset >= 0) {
        uint16_t anim_speed;

        if ((game_state.party_status & 0xFF) == 3) {
            anim_speed = 5;    /* Special party status → fast */
        } else if (party_idx >= 0 && party_idx < TOTAL_PARTY_COUNT &&
                   party_characters[party_idx].afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL]
                       == STATUS_0_UNCONSCIOUS) {
            anim_speed = 16;   /* Unconscious → slow */
        } else {
            /* Terrain-based speed from surface_flags */
            uint16_t sf = entities.surface_flags[entity_offset];
            if ((sf & 0x0C) == 12) {
                anim_speed = 24;  /* Deep water/swamp */
            } else if (sf & 0x08) {
                anim_speed = 16;  /* Shallow water */
            } else {
                anim_speed = 8;   /* Normal terrain */
            }
        }

        /* Paralyzed override — very slow animation (assembly @UNKNOWN26-28) */
        if (party_idx >= 0 && party_idx < TOTAL_PARTY_COUNT &&
            party_characters[party_idx].afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL]
                == STATUS_0_PARALYZED) {
            anim_speed = 56;
        }

        entities.var[3][entity_offset] = (int16_t)anim_speed;
    }

    /* Table lookup (assembly @UNKNOWN27-28) */
    if (char_id < 0 || char_id >= PLAYABLE_CHAR_ROW_COUNT)
        return (int16_t)0xFFFF;
    if (mode >= PLAYABLE_CHAR_COL_COUNT)
        return (int16_t)0xFFFF;
    return (int16_t)lookup_playable_char_sprite((uint16_t)char_id, mode);
}

/*
 * update_party_entity_graphics — update sprite graphics and flags for a party entity.
 *
 * Port of UPDATE_PARTY_ENTITY_GRAPHICS (C07A56).
 * Determines the correct sprite for the character based on status, walking style,
 * and game mode, then updates entity graphics pointers, walking_style, and var7 flags.
 */
void update_party_entity_graphics(
    int16_t char_id, uint16_t walking_style,
    int16_t entity_offset, int16_t party_idx)
{
    /* Determine sprite ID — may return -1 to hide entity */
    int16_t sprite_id = get_party_member_sprite_id(
        char_id, walking_style, entity_offset, party_idx);

    if (sprite_id == (int16_t)0xFFFF || sprite_id < 0) {
        /* Sprite ID == -1: hide entity (assembly @HIDE_ENTITY branch).
         * Assembly: stores -1 to animation_frame, then JMPs to @CHECK_CAMERA_MODE
         * (does NOT return early — still checks camera_mode for var7 bit 12). */
        entities.animation_frame[entity_offset] = -1;
        goto check_camera_mode;
    }

    /* Look up sprite_grouping for this sprite_id and update graphics pointers.
     * Assembly lines 33-55: dereference SPRITE_GROUPING_PTR_TABLE[sprite_id],
     * set ENTITY_GRAPHICS_PTR_HIGH/LOW and ENTITY_GRAPHICS_SPRITE_BANK.
     * Only graphics pointers change — VRAM allocation, dimensions, and hitboxes
     * remain from the original CREATE_ENTITY call. */
    uint32_t grouping_offset = 0;
    load_sprite_group_properties((uint16_t)sprite_id, &grouping_offset);

    if (sprite_grouping_data_buf &&
        grouping_offset + SPRITE_GROUPING_HEADER_SIZE <= sprite_grouping_data_size) {
        const uint8_t *sg = sprite_grouping_data_buf + grouping_offset;
        entities.graphics_ptr_hi[entity_offset] = 0;  /* ert.buffer offset marker */
        entities.graphics_ptr_lo[entity_offset] =
            (uint16_t)(grouping_offset + SPRITE_GROUPING_HEADER_SIZE);
        entities.graphics_sprite_bank[entity_offset] = sg[8] & 0xFF;
    }

    /* Set entity walking_style (assembly line 56) */
    entities.walking_styles[entity_offset] = walking_style;

    /* Compare with previous walking_style stored in char_struct.previous_walking_style.
     * If changed, update char_struct and set var7 bit 15 (UNKNOWN15)
     * to signal a walking style change (assembly lines 57-79). */
    if (party_idx >= 0 && party_idx < TOTAL_PARTY_COUNT) {
        uint16_t prev_ws = party_characters[party_idx].previous_walking_style;
        if (prev_ws != walking_style) {
            party_characters[party_idx].previous_walking_style = walking_style;
            entities.var[7][entity_offset] |= (int16_t)(1 << 15);
        }
    }

    /* var7 flag management (assembly @CHECK_WALKING_STYLE_CHANGED through @STATIC_POSE).
     *
     * Assembly checks game_state.leader_moved:
     * - If leader_moved != 0 AND walking_style != ESCALATOR(12):
     *     CLEAR var7 bits 13, 14, 15 — normal animation cycling
     * - Else:
     *     SET var7 bits 13, 14 — static pose (animation_frame forced to 0) */
    if (game_state.leader_moved && walking_style != WALKING_STYLE_ESCALATOR) {
        entities.var[7][entity_offset] &=
            (int16_t)(~(uint16_t)((1 << 15) | (1 << 14) | (1 << 13)));
    } else {
        entities.var[7][entity_offset] |=
            (int16_t)((1 << 14) | (1 << 13));
    }

check_camera_mode:
    /* Assembly @CHECK_CAMERA_MODE: if camera_mode == 2 (camera follow mode),
     * SET var7 bit 12 — entity hide/show controlled by spacing logic.
     * This runs even when sprite_id == -1 (entity hidden). */
    if (game_state.camera_mode == 2) {
        entities.var[7][entity_offset] |= (int16_t)(1 << 12);
    }
}

/*
 * update_follower_visuals — initialize follower entity visuals from position ert.buffer.
 *
 * Port of UPDATE_FOLLOWER_VISUALS (C04EF0).
 * Called once during EVENT_002 (party follower) initialization (not per-frame).
 * Reads the entity's current position ert.buffer entry and sets direction,
 * surface_flags, then calls update_party_entity_graphics to configure
 * the sprite graphics and var7 flags.
 */
void update_follower_visuals(int16_t entity_offset) {
    int16_t party_idx = entities.var[1][entity_offset];
    if (party_idx < 0 || party_idx >= TOTAL_PARTY_COUNT) return;

    /* Read position ert.buffer entry at char_struct's current position_index
     * (assembly lines 16-21) */
    uint16_t read_idx = party_characters[party_idx].position_index;
    PositionBufferEntry *entry = &pb.player_position_buffer[read_idx & 0xFF];

    /* Set entity direction from ert.buffer (assembly lines 22-23) */
    entities.directions[entity_offset] = (int16_t)entry->direction;

    /* Set entity surface_flags from ert.buffer (assembly lines 24-29) */
    entities.surface_flags[entity_offset] = entry->tile_flags;

    /* Get walking_style from ert.buffer and char_id from var0 (assembly lines 30-40) */
    uint16_t walking_style = entry->walking_style;
    int16_t char_id = entities.var[0][entity_offset];

    /* Update sprite graphics, walking_style, and var7 flags */
    update_party_entity_graphics(char_id, walking_style, entity_offset, party_idx);
}

/*
 * get_previous_position_index — Port of GET_PREVIOUS_POSITION_INDEX (C03E5A).
 *
 * Finds char_id+1 in game_state.party_order[] (party ordering table).
 * If found at index 0, this char IS the leader — returns -1 (0xFFFF).
 * Otherwise, looks up the previous member's entity via party_entity_slots and returns
 * their position_index from their char_struct.
 *
 * In practice, this is never called for the leader because update_follower_state
 * has an early-return check for char_id+1 == party_order[0].
 */
static uint16_t get_previous_position_index(int16_t char_id) {
    /* Search party_order[] for (char_id + 1).
     * NOTE: Assembly has no bounds check (loops forever if not found).
     * We add idx >= 6 as a safety guard; callers always pass valid char_ids. */
    int idx = 0;
    uint16_t target = (uint16_t)(char_id + 1);
    while ((game_state.party_order[idx] & 0xFF) != target) {
        idx++;
        if (idx >= 6) return game_state.position_buffer_index;
    }

    /* idx == 0 means this char is the first follower → leader is previous */
    if (idx == 0) {
        return (uint16_t)(-1);  /* Assembly returns #$FFFF */
    }

    /* Look up the previous member's entity slot via party_entity_slots[idx-1] */
    int prev_idx = idx - 1;
    uint16_t entity_slot = read_u16_le(&game_state.party_entity_slots[prev_idx * 2]);
    int16_t ent_off = (int16_t)entity_slot;
    if (ent_off < 0 || ent_off >= MAX_ENTITIES) {
        return game_state.position_buffer_index;
    }

    /* Get var1 (party index) from entity, look up char_struct position_index */
    int16_t party_idx = entities.var[1][ent_off];
    if (party_idx < 0 || party_idx >= TOTAL_PARTY_COUNT) {
        return game_state.position_buffer_index;
    }
    return party_characters[party_idx].position_index;
}

/*
 * get_distance_to_party_member — Port of GET_DISTANCE_TO_PARTY_MEMBER (C03E9D).
 *
 * Returns circular distance (in ert.buffer entries) between the previous
 * party member's position_index and this member's position_index.
 */
uint16_t get_distance_to_party_member(int16_t char_id,
                                      uint16_t current_position_index) {
    /* NOTE: Assembly reads current_position_index from CURRENT_PARTY_MEMBER_TICK's
     * char_struct::position_index (a global). We take it as a parameter instead,
     * which is equivalent because the caller passes the same value. Verified. */
    uint16_t prev_idx = get_previous_position_index(char_id);

    /* Assembly: if prev < current, add 256 (wrapping) */
    uint16_t distance;
    if (prev_idx >= current_position_index) {
        distance = prev_idx - current_position_index;
    } else {
        distance = (prev_idx + 256) - current_position_index;
    }
    return distance;
}

/*
 * adjust_party_member_visibility — Port of ADJUST_PARTY_MEMBER_VISIBILITY (C03EC3).
 *
 * Adjusts the follower's position_index based on distance to the previous
 * party member compared to the desired spacing.
 *
 * - distance == spacing → advance by 1, clear hide flag (normal progress)
 * - distance > spacing  → advance by 2 to catch up, set hide flag (too far behind)
 * - distance < spacing  → don't advance (too close, let gap increase)
 *
 * Returns the new position_index.
 */
uint16_t adjust_party_member_visibility(int16_t entity_offset,
                                        int16_t char_id,
                                        uint16_t position_index,
                                        uint16_t spacing) {
    /* NOTE: Assembly uses global CURRENT_ENTITY_SLOT; we pass entity_offset
     * explicitly. The caller provides the correct value. Assembly also takes
     * advance_amount via @PARAM03 (always 2), which we hardcode below. */
    uint16_t distance = get_distance_to_party_member(char_id, position_index);

    if (distance == spacing) {
        /* Normal: advance by 1, clear hide flag */
        position_index++;
        entities.var[7][entity_offset] &=
            (int16_t)(~(uint16_t)(1 << 12));  /* clear UNKNOWN12 */
    } else if (distance > spacing) {
        /* Too far behind: advance by 2 to catch up, set hide flag.
         * Assembly uses @PARAM03 = caller's @LOCAL00 = 2. */
        position_index += 2;
        entities.var[7][entity_offset] |=
            (int16_t)(1 << 12);  /* set UNKNOWN12 */
    }
    /* distance < spacing: too close, don't advance (let gap increase) */

    return position_index;
}

/*
 * init_party_position_buffer — Port of C03F1E.
 *
 * Fill all 256 entries with the leader's current state, then set each
 * party member's position_index to 0 and place their entity at the
 * leader's position.
 */
void init_party_position_buffer(void) {
    /* NOTE: position_buffer_index is our name for game_state::unknown88 (offset 136).
     * It's the write index into the 256-entry circular position ert.buffer. */
    game_state.position_buffer_index = 0;

    /* Fill all 256 entries with leader's current state */
    PositionBufferEntry fill = {
        .x_coord       = (int16_t)game_state.leader_x_coord,
        .y_coord       = (int16_t)game_state.leader_y_coord,
        .tile_flags    = game_state.trodden_tile_type,
        .walking_style = game_state.walking_style,
        .direction     = game_state.leader_direction,
    };
    for (int i = 0; i < POSITION_BUFFER_SIZE; i++) {
        pb.player_position_buffer[i] = fill;
    }

    /* Clear player movement flags (assembly line 20) */
    ow.player_movement_flags = 0;

    /* Set each party member's position_index = 0 and place entity at leader.
     * Assembly lines 28-54: uses party_entity_slots[i] to look up entity slot. */
    for (int i = 0; i < game_state.party_count && i < TOTAL_PARTY_COUNT; i++) {
        uint8_t char_id = game_state.player_controlled_party_members[i];
        party_characters[char_id].position_index = 0;
        party_characters[char_id].buffer_walking_style = 0xFFFF;   /* assembly line 39 */
        party_characters[char_id].previous_walking_style = 0xFFFF; /* assembly line 40 */

        uint16_t entity_slot = read_u16_le(&game_state.party_entity_slots[i * 2]);
        int16_t ent_off = (int16_t)entity_slot;
        if (ent_off < 0 || ent_off >= MAX_ENTITIES) continue;

        entities.abs_x[ent_off]        = (int16_t)game_state.leader_x_coord;
        entities.abs_y[ent_off]        = (int16_t)game_state.leader_y_coord;
        entities.directions[ent_off]   = (int16_t)game_state.leader_direction;
        entities.surface_flags[ent_off] = game_state.trodden_tile_type;
    }
}

/*
 * fill_party_position_buffer — Port of FILL_PARTY_POSITION_BUFFER
 * (asm/overworld/party/fill_party_position_buffer.asm).
 *
 * Fills all 256 position ert.buffer entries with positions trailing behind
 * the leader in the opposite direction, creating a spread-out formation.
 * Then assigns each party member a position_index 16 entries apart and
 * updates their entity positions from the ert.buffer.
 *
 * Called from TELEPORT when bit 7 of the direction byte is set.
 */
void fill_party_position_buffer(uint16_t direction) {
    /* Set position_buffer_index = 0xFF (assembly lines 19-21) */
    game_state.position_buffer_index = 0xFF;

    /* Cache leader state (lines 22-30) */
    int16_t current_x = (int16_t)game_state.leader_x_coord;
    int16_t current_y = (int16_t)game_state.leader_y_coord;
    uint16_t surface_flags = game_state.trodden_tile_type;
    uint16_t walking_style_val = game_state.walking_style;

    /* Compute opposite direction: (direction + 4) & 7 (lines 31-37) */
    uint16_t opposite_dir = (direction + 4) & 7;

    /* Build 32-bit fixed-point positions (lines 38-41, 50-53) */
    int32_t pos_x = (int32_t)(((uint32_t)game_state.leader_x_coord << 16) |
                               game_state.leader_x_frac);
    int32_t pos_y = (int32_t)(((uint32_t)game_state.leader_y_coord << 16) |
                               game_state.leader_y_frac);

    /* Compute adjusted positions in opposite direction (lines 42-44, 54-56) */
    int32_t adj_x = adjust_position(opposite_dir, surface_flags, pos_x, pb.h_speeds);
    int32_t adj_y = adjust_position(opposite_dir, surface_flags, pos_y, pb.v_speeds);

    /* Delta = adjusted - original (lines 47-48, 59-61) */
    int32_t delta_x = adj_x - pos_x;
    int32_t delta_y = adj_y - pos_y;
    int16_t delta_x_int = (int16_t)(delta_x >> 16);
    int16_t delta_y_int = (int16_t)(delta_y >> 16);

    /* Fill 256 entries from index 255 down to 0 (lines 62-95).
     * Entry 255 = leader position, each subsequent entry steps backward. */
    for (int i = 255; i >= 0; i--) {
        pb.player_position_buffer[i].x_coord = current_x;
        pb.player_position_buffer[i].y_coord = current_y;
        pb.player_position_buffer[i].tile_flags = surface_flags;
        pb.player_position_buffer[i].walking_style = walking_style_val;
        pb.player_position_buffer[i].direction = direction;
        current_x += delta_x_int;
        current_y += delta_y_int;
    }

    /* Assign each party member a position_index and update entity (lines 96-183).
     * Position indices are spaced 16 apart: 255, 239, 223, ... */
    uint16_t pos_idx = 0xFF;
    int buf_idx = 255;

    for (int i = 0; i < (game_state.party_count & 0xFF); i++) {
        uint8_t char_id = game_state.player_controlled_party_members[i];

        party_characters[char_id].position_index = pos_idx;
        party_characters[char_id].buffer_walking_style = 0xFFFF;
        party_characters[char_id].previous_walking_style = 0xFFFF;

        /* Get entity slot for this party member */
        uint16_t entity_slot = read_u16_le(&game_state.party_entity_slots[i * 2]);
        int16_t ent_off = (int16_t)entity_slot;

        PositionBufferEntry *entry = &pb.player_position_buffer[buf_idx];
        entities.abs_x[ent_off] = entry->x_coord;
        entities.abs_y[ent_off] = entry->y_coord;

        entities.directions[ent_off] = entry->direction;
        entities.surface_flags[ent_off] = entry->tile_flags;

        pos_idx -= 16;
        buf_idx -= 16;
    }
}

/*
 * position_buffer_write — write leader state to the circular ert.buffer.
 *
 * Port of UPDATE_LEADER_MOVEMENT's ert.buffer write logic (lines 61-119).
 * Must be called AFTER game_state leader_x/y_coord, leader_direction,
 * trodden_tile_type, and walking_style are up to date for this frame.
 */
void position_buffer_write(int16_t source_entity) {
    (void)source_entity;  /* No longer used — reads from game_state */
    uint16_t idx = game_state.position_buffer_index;
    PositionBufferEntry *entry = &pb.player_position_buffer[idx];

    /* X/Y and index advance only when leader has moved.
     * Assembly reads from game_state.leader_x/y_coord (not entity abs position).
     * In normal gameplay, UPDATE_PARTY_SPRITES keeps these in sync. */
    if (game_state.leader_moved) {
        entry->x_coord = (int16_t)game_state.leader_x_coord;
        entry->y_coord = (int16_t)game_state.leader_y_coord;
        game_state.position_buffer_index = (idx + 1) & 0xFF;
    }

    /* tile_flags, walking_style, direction are always written
     * (even when the leader hasn't moved) — assembly lines 110-119 */
    entry->tile_flags    = game_state.trodden_tile_type;
    entry->walking_style = game_state.walking_style;
    entry->direction     = game_state.leader_direction;
}

/*
 * sync_camera_to_entity — Port of SYNC_CAMERA_TO_ENTITY (C0476D).
 *
 * Copies the camera focus entity's position and direction to game_state
 * leader fields. Sets leader_moved = 1 if position changed (including
 * sub-pixel fractional parts).
 *
 * Called from update_special_camera_mode when camera_mode == 2.
 * This is what makes party followers trail the camera entity in attract mode.
 */
static void sync_camera_to_entity(void) {
    if (ow.camera_focus_entity < 0) return;

    /* In the assembly, CAMERA_FOCUS_ENTITY is a slot; ASL converts to offset.
     * In the C port, ow.camera_focus_entity is already the entity slot. */
    int16_t ent = ow.camera_focus_entity;
    int16_t new_x = entities.abs_x[ent];
    int16_t new_y = entities.abs_y[ent];
    uint16_t new_frac_x = (uint16_t)entities.frac_x[ent];
    uint16_t new_frac_y = (uint16_t)entities.frac_y[ent];

    /* Check if position changed (including sub-pixel fraction) */
    uint16_t moved = 0;
    if ((uint16_t)new_x != game_state.leader_x_coord ||
        (uint16_t)new_y != game_state.leader_y_coord ||
        new_frac_x != game_state.leader_x_frac ||
        new_frac_y != game_state.leader_y_frac) {
        moved = 1;
    }

    /* Copy entity position to leader game_state fields */
    game_state.leader_x_coord = (uint16_t)new_x;
    game_state.leader_y_coord = (uint16_t)new_y;
    game_state.leader_x_frac = new_frac_x;
    game_state.leader_y_frac = new_frac_y;

    /* Copy direction */
    game_state.leader_direction = (uint16_t)entities.directions[ent];

    /* Set leader_moved if position changed */
    game_state.leader_moved = moved;
}

/*
 * restore_camera_mode — Port of RESTORE_CAMERA_MODE (C04A7B).
 *
 * Restores camera_mode from backup saved by start_camera_shake().
 * Then calls INITIATE_ENEMY_ENCOUNTER to begin the battle transition.
 */
static void restore_camera_mode(void) {
    game_state.camera_mode = pb.camera_mode_backup;
    initiate_enemy_encounter();
}

/*
 * start_camera_shake — Port of START_CAMERA_SHAKE (C04A88).
 *
 * Called when an enemy entity touches the party. Saves the current camera_mode,
 * switches to mode 3 with a 12-frame timer, plays a collision SFX via APU
 * port 1, and suppresses overworld status updates.
 */
void start_camera_shake(void) {
    pb.camera_mode_3_frames_left = 12;
    pb.camera_mode_backup = game_state.camera_mode;
    game_state.camera_mode = 3;
    write_apu_port1(2);
    ow.overworld_status_suppression = 1;
}

/*
 * update_camera_mode_3 — Port of UPDATE_CAMERA_MODE_3 (C04AAD).
 *
 * Called each frame while camera_mode == 3 (screen shake on enemy contact).
 * Decrements the frame counter; when it reaches 0, restores camera mode.
 * While active, reads player d-pad input and updates party entity directions
 * to face the input direction (lets the player turn during the shake).
 * Only updates entities that:
 *   - Have an active entity script (not -1)
 *   - Are not currently facing the input direction
 *   - Are not on a ROPE or LADDER walking style (from position ert.buffer)
 */
static void update_camera_mode_3(void) {
    pb.camera_mode_3_frames_left--;
    if (pb.camera_mode_3_frames_left == 0) {
        restore_camera_mode();
        return;
    }

    /* Get player input direction (assembly lines 12-18) */
    int16_t input_dir = map_input_to_direction(game_state.walking_style);
    if (input_dir == -1) return;

    /* Loop over party entities 24..28 (assembly lines 22-67) */
    for (int ent_idx = 24; ent_idx <= 28; ent_idx++) {
        int16_t ent_off = (int16_t)ent_idx;

        /* Skip if entity has no active script (assembly lines 26-28) */
        if (entities.script_table[ent_off] == -1) continue;

        /* Skip if already facing the input direction (assembly lines 37-38) */
        if (entities.directions[ent_off] == input_dir) continue;

        /* Get char_id from entity var1 (party index), look up position ert.buffer
         * entry to check walking_style (assembly lines 39-55) */
        int16_t party_idx = entities.var[1][ent_off];
        if (party_idx < 0 || party_idx >= TOTAL_PARTY_COUNT) continue;

        uint16_t read_idx = party_characters[party_idx].position_index;
        PositionBufferEntry *entry = &pb.player_position_buffer[read_idx & 0xFF];

        /* Skip if on ROPE or LADDER (assembly lines 52-55) */
        if (entry->walking_style == WALKING_STYLE_ROPE ||
            entry->walking_style == WALKING_STYLE_LADDER) {
            continue;
        }

        /* Update entity direction to input direction (assembly lines 57-59) */
        entities.directions[ent_off] = input_dir;

        /* Assembly calls JSL UPDATE_ENTITY_SPRITE to DMA new sprite tiles.
         * In the C port, the software renderer reads direction at draw time
         * from entities.directions[], so no explicit sprite update needed. */
    }

    /* Store input direction as leader direction (assembly lines 68-70) */
    game_state.leader_direction = (uint16_t)input_dir;
}

#define NUM_WALKING_STYLES 14
#define NUM_DIRECTIONS 8

/*
 * update_special_camera_mode — Port of UPDATE_SPECIAL_CAMERA_MODE (C04B53).
 *
 * Dispatches on game_state.camera_mode:
 *   Mode 1: Auto-movement using speed tables (camera dolly along a direction).
 *   Mode 2: SYNC_CAMERA_TO_ENTITY (follow ow.camera_focus_entity).
 *   Mode 3: UPDATE_CAMERA_MODE_3 (camera shake / restore).
 */
static void update_special_camera_mode(void) {
    uint16_t mode = game_state.camera_mode & 0xFF;
    switch (mode) {
    case 1: {
        /* Mode 1: Auto-movement using speed tables (camera dolly).
         * Uses leader_direction, or ow.auto_movement_direction if on stairs.
         * Decrements auto_move_frames_left; when 0, restores walking_style
         * and clears camera_mode. */
        uint16_t ws = game_state.walking_style;
        uint16_t dir;
        if (ws == WALKING_STYLE_STAIRS) {
            dir = ow.auto_movement_direction;
        } else {
            dir = game_state.leader_direction;
        }
        int idx = ws * NUM_DIRECTIONS + dir;

        /* Add horizontal speed to 32-bit fixed-point leader X. */
        int32_t pos_x = (int32_t)(((uint32_t)game_state.leader_x_coord << 16)
                       | game_state.leader_x_frac);
        pos_x += pb.h_speeds[idx];
        game_state.leader_x_frac = (uint16_t)(pos_x & 0xFFFF);
        game_state.leader_x_coord = (uint16_t)((uint32_t)pos_x >> 16);

        /* Add vertical speed to 32-bit fixed-point leader Y. */
        int32_t pos_y = (int32_t)(((uint32_t)game_state.leader_y_coord << 16)
                       | game_state.leader_y_frac);
        pos_y += pb.v_speeds[idx];
        game_state.leader_y_frac = (uint16_t)(pos_y & 0xFFFF);
        game_state.leader_y_coord = (uint16_t)((uint32_t)pos_y >> 16);

        /* Decrement frame counter; clear mode when done. */
        game_state.auto_move_frames_left--;
        if (game_state.auto_move_frames_left == 0) {
            game_state.camera_mode = 0;
            game_state.walking_style = game_state.auto_move_saved_walking_style;
        }
        game_state.leader_moved = 1;
        break;
    }
    case 2:
        /* Mode 2: Follow camera focus entity.
         * This is the mode set by SET_CAMERA_FOCUS_BY_SPRITE_ID (CC 1F EF). */
        sync_camera_to_entity();
        break;
    case 3:
        /* Mode 3: Camera shake on enemy contact.
         * Port of UPDATE_CAMERA_MODE_3 (C04AAD). */
        update_camera_mode_3();
        break;
    default:
        break;
    }
}

/* ====================================================================
 * Movement System
 *
 * Ports of:
 *   VELOCITY_STORE                (asm/overworld/velocity_store.asm)
 *   MAP_INPUT_TO_DIRECTION        (asm/overworld/map_input_to_direction.asm)
 *   ADJUST_POSITION_HORIZONTAL    (asm/overworld/adjust_position_horizontal.asm)
 *   ADJUST_POSITION_VERTICAL      (asm/overworld/adjust_position_vertical.asm)
 *   MUSHROOMIZATION_MOVEMENT_SWAP (asm/overworld/mushroomization_movement_swap.asm)
 *   UPDATE_OVERWORLD_PLAYER_INPUT (asm/overworld/update_overworld_player_input.asm)
 * ==================================================================== */

/* MOVEMENT_SPEEDS — cardinal speed per walking style (16.16 fixed-point).
 * From asm/data/map/movement_speeds.asm. */
static const int32_t movement_speeds_cardinal[NUM_WALKING_STYLES] = {
    0x00016000, /* 0  NORMAL:   1.375 */
    0x00016000, /* 1:           1.375 */
    0x00016000, /* 2:           1.375 */
    0x0001CCCC, /* 3  BICYCLE:  1.8   */
    0x00010000, /* 4  GHOST:    1.0   */
    0x00000000, /* 5:           0.0   */
    0x00010000, /* 6  SLOWER:   1.0   */
    0x0000CCCC, /* 7  LADDER:   0.8   */
    0x0000CCCC, /* 8  ROPE:     0.8   */
    0x00000000, /* 9:           0.0   */
    0x00008000, /* 10 SLOWEST:  0.5   */
    0x00000000, /* 11:          0.0   */
    0x0000CCCC, /* 12 ESCALATOR:0.8   */
    0x0000CCCC, /* 13 STAIRS:   0.8   */
};

/* MOVEMENT_SPEEDS_DIAGONAL — diagonal speed (cardinal * sqrt(2)/2).
 * From asm/data/map/movement_speeds.asm. */
static const int32_t movement_speeds_diag[NUM_WALKING_STYLES] = {
    0x0000F8E6, /* 0  NORMAL:   0.972 */
    0x0000F8E6, /* 1:           0.972 */
    0x0000F8E6, /* 2:           0.972 */
    0x000145D5, /* 3  BICYCLE:  1.273 */
    0x0000B505, /* 4  GHOST:    0.707 */
    0x00000000, /* 5:           0.0   */
    0x0000B505, /* 6  SLOWER:   0.707 */
    0x000090D0, /* 7  LADDER:   0.566 */
    0x000090D0, /* 8  ROPE:     0.566 */
    0x00000000, /* 9:           0.0   */
    0x00005A82, /* 10 SLOWEST:  0.354 */
    0x00000000, /* 11:          0.0   */
    0x000090D0, /* 12 ESCALATOR:0.566 */
    0x000090D0, /* 13 STAIRS:   0.566 */
};

/* Speed modifier constants (from include/config.asm, 16.16 fixed-point). */
#define SHALLOW_WATER_SPEED  0x00008000  /* 0.5x */
#define DEEP_WATER_SPEED     0x0000547A  /* 0.33x */
#define SKIP_SANDWICH_SPEED  0x00018000  /* 1.5x */

/* Surface flags masks (from include/enums.asm SURFACE_FLAGS) */
#define SURFACE_SHALLOW_WATER 0x08
#define SURFACE_DEEP_WATER    0x0C  /* SHALLOW_WATER | CAUSES_SUNSTROKE */

/* ALLOWED_INPUT_DIRECTIONS — bitmask of allowed directions per walking style.
 * From asm/data/map/allowed_input_directions.asm. */
static const uint16_t allowed_input_directions[NUM_WALKING_STYLES] = {
    0xFF, /* 0  NORMAL:   all */
    0xFF, /* 1:           all */
    0xFF, /* 2:           all */
    0xFF, /* 3  BICYCLE:  all */
    0xFF, /* 4  GHOST:    all */
    0xFF, /* 5:           all */
    0xFF, /* 6  SLOWER:   all */
    0x11, /* 7  LADDER:   UP and DOWN only */
    0x11, /* 8  ROPE:     UP and DOWN only */
    0xFF, /* 9:           all */
    0xFF, /* 10 SLOWEST:  all */
    0xFF, /* 11:          all */
    0x00, /* 12 ESCALATOR:none (handled specially) */
    0xFF, /* 13 STAIRS:   all */
};

/* MUSHROOMIZATION_DIRECTION_REMAP_TABLES — 3 tables x 16 entries.
 * From asm/data/map/mushroomization_direction_remap_tables.asm.
 * Each table remaps the 4-bit d-pad combination (UP|DOWN|LEFT|RIGHT in
 * bits 8-11 of PAD_STATE) to a rotated version.
 * Table 0 (modifier=1): 90° CW, Table 1 (modifier=2): 180°,
 * Table 2 (modifier=3): 270° CW. */
static const uint16_t mushroom_remap[3][16] = {
    /* Modifier 1: UP→RIGHT, RIGHT→DOWN, DOWN→LEFT, LEFT→UP */
    {
        0,
        PAD_DOWN,                           /* RIGHT → DOWN */
        PAD_UP,                             /* LEFT → UP */
        PAD_UP | PAD_DOWN,
        PAD_LEFT,                           /* DOWN → LEFT */
        PAD_LEFT | PAD_DOWN,
        PAD_LEFT | PAD_UP,
        PAD_LEFT | PAD_UP | PAD_DOWN,
        PAD_RIGHT,                          /* UP → RIGHT */
        PAD_RIGHT | PAD_DOWN,
        PAD_RIGHT | PAD_UP,
        PAD_RIGHT | PAD_UP | PAD_DOWN,
        PAD_RIGHT | PAD_LEFT,
        PAD_RIGHT | PAD_LEFT | PAD_DOWN,
        PAD_RIGHT | PAD_LEFT | PAD_UP,
        PAD_RIGHT | PAD_LEFT | PAD_UP | PAD_DOWN,
    },
    /* Modifier 2: UP↔DOWN, LEFT↔RIGHT */
    {
        0,
        PAD_LEFT,                           /* RIGHT → LEFT */
        PAD_RIGHT,                          /* LEFT → RIGHT */
        PAD_RIGHT | PAD_LEFT,
        PAD_UP,                             /* DOWN → UP */
        PAD_LEFT | PAD_UP,
        PAD_RIGHT | PAD_UP,
        PAD_RIGHT | PAD_LEFT | PAD_UP,
        PAD_DOWN,                           /* UP → DOWN */
        PAD_LEFT | PAD_DOWN,
        PAD_RIGHT | PAD_DOWN,
        PAD_RIGHT | PAD_LEFT | PAD_DOWN,
        PAD_UP | PAD_DOWN,
        PAD_LEFT | PAD_UP | PAD_DOWN,
        PAD_RIGHT | PAD_UP | PAD_DOWN,
        PAD_RIGHT | PAD_LEFT | PAD_UP | PAD_DOWN,
    },
    /* Modifier 3: UP→LEFT, RIGHT→UP, DOWN→RIGHT, LEFT→DOWN */
    {
        0,
        PAD_UP,                             /* RIGHT → UP */
        PAD_DOWN,                           /* LEFT → DOWN */
        PAD_UP | PAD_DOWN,
        PAD_RIGHT,                          /* DOWN → RIGHT */
        PAD_RIGHT | PAD_UP,
        PAD_RIGHT | PAD_DOWN,
        PAD_RIGHT | PAD_UP | PAD_DOWN,
        PAD_LEFT,                           /* UP → LEFT */
        PAD_LEFT | PAD_UP,
        PAD_LEFT | PAD_DOWN,
        PAD_LEFT | PAD_UP | PAD_DOWN,
        PAD_RIGHT | PAD_LEFT,
        PAD_RIGHT | PAD_LEFT | PAD_UP,
        PAD_RIGHT | PAD_LEFT | PAD_DOWN,
        PAD_RIGHT | PAD_LEFT | PAD_UP | PAD_DOWN,
    },
};

/* Bicycle diagonal turn delay counter (BSS, from ram.asm:865).
 * When a diagonal direction is pressed, this is set to 4.
 * While non-zero, cardinal directions are delayed (the bicycle
 * maintains its diagonal direction briefly). */
/* pb.bicycle_diagonal_turn_counter now in PositionBufferState pb. */

/* SFX ID for bicycle bell (from include/constants/sfx.asm) */
#define SFX_BICYCLE_BELL 23

/* Walking style enum (from include/enums.asm) */
#define WALKING_STYLE_NORMAL     0
#define WALKING_STYLE_BICYCLE    3
#define WALKING_STYLE_GHOST      4
#define WALKING_STYLE_SLOWER     6
#define WALKING_STYLE_LADDER     7
#define WALKING_STYLE_ROPE       8
#define WALKING_STYLE_SLOWEST   10
#define WALKING_STYLE_ESCALATOR 12
#define WALKING_STYLE_STAIRS    13

/* Mushroomization timer constant (from include/config.asm).
 * TIME_BETWEEN_DIRECTION_SWAPS = 30 * 60 = 1800 frames. */
#define TIME_BETWEEN_DIRECTION_SWAPS 1800

/*
 * velocity_store — precompute horizontal/vertical speed lookup tables.
 *
 * Port of VELOCITY_STORE (asm/overworld/velocity_store.asm).
 * For each of 14 walking styles, distributes the cardinal and diagonal
 * speed values into the 8-direction pb.h_speeds[] and pb.v_speeds[] tables.
 */
void velocity_store(void) {
    for (int style = 0; style < NUM_WALKING_STYLES; style++) {
        int32_t cardinal = movement_speeds_cardinal[style];
        int32_t diagonal = movement_speeds_diag[style];
        int base = style * NUM_DIRECTIONS;

        /* Horizontal speed table:
         * UP/DOWN = 0, RIGHT = +cardinal, LEFT = -cardinal,
         * diagonals = ±diagonal. */
        pb.h_speeds[base + 0] = 0;           /* UP */
        pb.h_speeds[base + 1] = diagonal;    /* UP_RIGHT */
        pb.h_speeds[base + 2] = cardinal;    /* RIGHT */
        pb.h_speeds[base + 3] = diagonal;    /* DOWN_RIGHT */
        pb.h_speeds[base + 4] = 0;           /* DOWN */
        pb.h_speeds[base + 5] = -diagonal;   /* DOWN_LEFT */
        pb.h_speeds[base + 6] = -cardinal;   /* LEFT */
        pb.h_speeds[base + 7] = -diagonal;   /* UP_LEFT */

        /* Vertical speed table:
         * LEFT/RIGHT = 0, DOWN = +cardinal, UP = -cardinal,
         * diagonals = ±diagonal. */
        pb.v_speeds[base + 0] = -cardinal;   /* UP */
        pb.v_speeds[base + 1] = -diagonal;   /* UP_RIGHT */
        pb.v_speeds[base + 2] = 0;           /* RIGHT */
        pb.v_speeds[base + 3] = diagonal;    /* DOWN_RIGHT */
        pb.v_speeds[base + 4] = cardinal;    /* DOWN */
        pb.v_speeds[base + 5] = diagonal;    /* DOWN_LEFT */
        pb.v_speeds[base + 6] = 0;           /* LEFT */
        pb.v_speeds[base + 7] = -diagonal;   /* UP_LEFT */
    }
}

/*
 * map_input_to_direction — convert d-pad input to direction 0-7.
 *
 * Port of MAP_INPUT_TO_DIRECTION (asm/overworld/map_input_to_direction.asm).
 * Checks the d-pad state against the allowed directions for the current
 * walking style. Returns direction (0=UP .. 7=UP_LEFT) or -1 if no input
 * or direction not allowed.
 */
int16_t map_input_to_direction(uint16_t walking_style) {
    if (ow.pending_interactions)
        return -1;

    uint16_t allowed = (walking_style < NUM_WALKING_STYLES)
        ? allowed_input_directions[walking_style] : 0xFF;
    uint16_t pad = core.pad1_held & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);

    /* Check each direction combination (assembly lines 25-96) */
    if (pad == PAD_UP            && (allowed & 0x01)) return 0;
    if (pad == (PAD_UP|PAD_RIGHT)&& (allowed & 0x02)) return 1;
    if (pad == PAD_RIGHT         && (allowed & 0x04)) return 2;
    if (pad == (PAD_DOWN|PAD_RIGHT)&&(allowed & 0x08)) return 3;
    if (pad == PAD_DOWN          && (allowed & 0x10)) return 4;
    if (pad == (PAD_DOWN|PAD_LEFT)&&(allowed & 0x20)) return 5;
    if (pad == PAD_LEFT          && (allowed & 0x40)) return 6;
    if (pad == (PAD_UP|PAD_LEFT) && (allowed & 0x80)) return 7;

    return -1;
}

/*
 * adjust_position — apply movement speed to a position component.
 *
 * Port of ADJUST_POSITION_HORIZONTAL / ADJUST_POSITION_VERTICAL
 * (asm/overworld/adjust_position_horizontal.asm, adjust_position_vertical.asm).
 *
 * Looks up the speed from the precomputed table, applies water/skip-sandwich
 * modifiers, and returns the adjusted 32-bit fixed-point position.
 *
 * speeds: pointer to pb.h_speeds or pb.v_speeds array.
 */
static int32_t adjust_position(int16_t direction, uint16_t surface_flags,
                               int32_t pos, const int32_t *speeds) {
    uint16_t ws = game_state.walking_style;
    if (ws >= NUM_WALKING_STYLES) ws = 0;
    int32_t speed = speeds[ws * NUM_DIRECTIONS + direction];

    uint16_t water = surface_flags & 0x0C;

    if (water == SURFACE_SHALLOW_WATER) {
        /* Shallow water: speed * 0.5 (assembly lines 22-55) */
        int32_t delta = (int32_t)(((int64_t)speed * SHALLOW_WATER_SPEED) >> 16);
        return pos + delta;
    } else if (water == SURFACE_DEEP_WATER) {
        /* Deep water: speed * 0.33 (assembly lines 56-89) */
        int32_t delta = (int32_t)(((int64_t)speed * DEEP_WATER_SPEED) >> 16);
        return pos + delta;
    } else if (ow.demo_frames_left) {
        /* Demo mode: raw speed, no modifiers (assembly lines 91-112) */
        return pos + speed;
    } else if ((game_state.party_status & 0xFF) == 3 && ws == 0) {
        /* Skip sandwich active + NORMAL walking style: speed * 1.5
         * (assembly @UNKNOWN8, lines 113-153) */
        int32_t delta = (int32_t)(((int64_t)speed * SKIP_SANDWICH_SPEED) >> 16);
        return pos + delta;
    } else {
        /* Normal case: raw speed (assembly @UNKNOWN13, lines 154-173) */
        return pos + speed;
    }
}

/*
 * mushroomization_movement_swap — confuse d-pad input when mushroomized.
 *
 * Port of MUSHROOMIZATION_MOVEMENT_SWAP
 * (asm/overworld/mushroomization_movement_swap.asm).
 * Cycles through 4 rotation states (0=none, 1=90°CW, 2=180°, 3=270°CW)
 * every TIME_BETWEEN_DIRECTION_SWAPS frames. Remaps the d-pad bits in
 * core.pad1_pressed and core.pad1_held.
 */
static void mushroomization_movement_swap(void) {
    /* Timer management (assembly lines 7-17) */
    if (ow.mushroomization_timer == 0) {
        ow.mushroomization_timer = TIME_BETWEEN_DIRECTION_SWAPS;
        ow.mushroomization_modifier = (ow.mushroomization_modifier + 1) & 3;
    }
    ow.mushroomization_timer--;

    /* Skip if no rotation or demo mode (assembly lines 19-23) */
    if (ow.mushroomization_modifier == 0) return;
    if (ow.demo_frames_left) return;

    uint16_t table_idx = ow.mushroomization_modifier - 1;

    /* Remap core.pad1_pressed d-pad bits (assembly lines 24-54) */
    uint16_t press_dpad = (core.pad1_pressed >> 8) & 0x0F;
    uint16_t new_press_dpad = mushroom_remap[table_idx][press_dpad];
    core.pad1_pressed = (core.pad1_pressed & 0xF0FF) | new_press_dpad;

    /* Remap core.pad1_held d-pad bits (assembly lines 55-69) */
    uint16_t held_dpad = (core.pad1_held >> 8) & 0x0F;
    uint16_t new_held_dpad = mushroom_remap[table_idx][held_dpad];
    core.pad1_held = (core.pad1_held & 0xF0FF) | new_held_dpad;
}

/*
 * update_escalator_movement — per-frame escalator movement handler.
 *
 * Port of UPDATE_ESCALATOR_MOVEMENT (asm/overworld/door/update_escalator_movement.asm).
 * Called from update_leader_movement() when walking_style == ESCALATOR.
 *
 * Moves the player diagonally based on ow.escalator_entrance_direction:
 *   bits 8-9: 0x0000=UP_LEFT, 0x0100=UP_RIGHT, 0x0200=DOWN_LEFT, 0x0300=DOWN_RIGHT.
 * Checks for door tiles at the destination (escalator exit).
 * Always applies movement (no wall collision blocking).
 */
static void update_escalator_movement(void) {
    /* Lines 10-15: Early exit for enemy encounter */
    if (ow.enemy_has_been_touched) return;
    if (ow.battle_swirl_countdown) {
        ow.battle_swirl_countdown--;
        return;
    }

    /* Lines 17-45: Map ow.escalator_entrance_direction bits 8-9 to diagonal direction */
    uint16_t dir_bits = ow.escalator_entrance_direction & 0x0300;
    int16_t direction;
    switch (dir_bits) {
    case 0x0000: direction = 7; break;  /* UP_LEFT */
    case 0x0200: direction = 5; break;  /* DOWN_LEFT */
    case 0x0100: direction = 1; break;  /* UP_RIGHT */
    case 0x0300: direction = 3; break;  /* DOWN_RIGHT */
    default:     direction = 7; break;
    }

    /* Lines 47-48: Reset ladder tile sentinel */
    ow.ladder_stairs_tile_x = -1;

    /* Lines 52-55: Directional collision check (for door detection) */
    check_directional_collision(
        (int16_t)game_state.leader_x_coord,
        (int16_t)game_state.leader_y_coord,
        direction);

    /* Lines 56-61: If a door tile was found, process it */
    if (ow.ladder_stairs_tile_x != -1) {
        process_door_at_tile((uint16_t)ow.ladder_stairs_tile_x,
                             (uint16_t)ow.ladder_stairs_tile_y);
    }

    /* Lines 69-107: Apply escalator movement speed.
     * Uses walking_style 12 (ESCALATOR) speed tables.
     * Movement is always applied — escalator doesn't check wall collision. */
    int32_t pos_x = (int32_t)(((uint32_t)game_state.leader_x_coord << 16) |
                               game_state.leader_x_frac);
    int32_t pos_y = (int32_t)(((uint32_t)game_state.leader_y_coord << 16) |
                               game_state.leader_y_frac);

    int idx = WALKING_STYLE_ESCALATOR * NUM_DIRECTIONS + direction;
    pos_x += pb.h_speeds[idx];
    pos_y += pb.v_speeds[idx];

    game_state.leader_x_frac = (uint16_t)(pos_x & 0xFFFF);
    game_state.leader_x_coord = (uint16_t)((uint32_t)pos_x >> 16);
    game_state.leader_y_frac = (uint16_t)(pos_y & 0xFFFF);
    game_state.leader_y_coord = (uint16_t)((uint32_t)pos_y >> 16);

    /* Line 109-110: Always set leader_moved */
    game_state.leader_moved = 1;
}

/*
 * update_bicycle_movement — per-frame bicycle movement handler.
 *
 * Port of UPDATE_BICYCLE_MOVEMENT (asm/overworld/update_bicycle_movement.asm).
 * Called from update_leader_movement() when walking_style == BICYCLE.
 *
 * Handles:
 * - R button plays bicycle bell sound
 * - Diagonal turn delay (keeps diagonal direction for 4 frames before
 *   allowing a cardinal turn, giving the bicycle a smooth feel)
 * - Direct speed lookup from bicycle walking style tables (no water/sandwich modifiers)
 * - Entity and NPC collision checking
 * - Battle swirl countdown
 *
 * Parameter: prev_leader_moved (1 = leader moved last frame, 0 = stopped).
 * When no input and prev_leader_moved != 0, the bicycle coasts in the
 * current direction (momentum).
 */
static void update_bicycle_movement(uint16_t prev_leader_moved) {
    /* Line 17-18: Map input to direction using BICYCLE walking style */
    int16_t input_direction = map_input_to_direction(game_state.walking_style);
    int16_t direction = input_direction;

    /* Lines 22-36: Battle swirl countdown */
    if (ow.battle_swirl_countdown) {
        ow.battle_swirl_countdown--;
        if (ow.battle_swirl_countdown == 0) {
            ow.battle_mode = 0xFFFF;  /* -1: trigger battle */
            return;
        }
        npc_collision_check(game_state.leader_x_coord,
                           game_state.leader_y_coord,
                           game_state.current_party_members);
        return;
    }

    /* Lines 38-42: R button plays bicycle bell */
    if (core.pad1_pressed & PAD_R) {
        play_sfx(SFX_BICYCLE_BELL);
    }

    /* Lines 43-57: Handle no input.
     * If no direction pressed but bicycle was moving last frame (prev_leader_moved),
     * continue in current facing direction (momentum).
     * If stopped (prev_leader_moved == 0), just do NPC collision check and return. */
    if (direction == -1) {
        if (prev_leader_moved) {
            direction = game_state.leader_direction;
        } else {
            npc_collision_check(game_state.leader_x_coord,
                               game_state.leader_y_coord,
                               game_state.current_party_members);
            return;
        }
    }

    /* Lines 58-80: Diagonal turn delay.
     * When a diagonal direction is input, set counter to 4.
     * When a cardinal direction is input while counter > 0:
     *   - Decrement counter, keep old diagonal direction
     *   - When counter reaches 0: use raw input direction if available,
     *     else keep current facing */
    if (direction & 1) {
        /* Diagonal direction — reset counter */
        pb.bicycle_diagonal_turn_counter = 4;
    } else {
        /* Cardinal direction — check delay counter */
        if (pb.bicycle_diagonal_turn_counter) {
            pb.bicycle_diagonal_turn_counter--;
            if (pb.bicycle_diagonal_turn_counter != 0) {
                /* Still in delay — keep current facing direction */
                direction = game_state.leader_direction;
            } else {
                /* Counter just expired — if no input, keep facing */
                if (input_direction == -1) {
                    direction = game_state.leader_direction;
                }
            }
        }
    }

    /* Line 82: Update leader direction */
    game_state.leader_direction = (uint16_t)direction;

    /* Lines 83-109: Calculate new position.
     * Bicycle uses walking_style 3 speed tables directly (no modifiers).
     * Assembly: direction * 4 indexes into 32-bit speed table entries. */
    int32_t pos_x = (int32_t)(((uint32_t)game_state.leader_x_coord << 16) |
                               game_state.leader_x_frac);
    int32_t pos_y = (int32_t)(((uint32_t)game_state.leader_y_coord << 16) |
                               game_state.leader_y_frac);

    int32_t new_x = pos_x + pb.h_speeds[WALKING_STYLE_BICYCLE * NUM_DIRECTIONS + direction];
    int32_t new_y = pos_y + pb.v_speeds[WALKING_STYLE_BICYCLE * NUM_DIRECTIONS + direction];

    /* Line 111: Reset ladder tile sentinel */
    ow.ladder_stairs_tile_x = -1;

    /* Lines 112-130: Entity collision check + NPC collision check.
     * CHECK_ENTITY_COLLISION returns surface flags; bits 6-7 = wall collision.
     * Assembly uses entity slot 24 (leader/first party member). */
    int16_t new_xi = (int16_t)((uint32_t)new_x >> 16);
    int16_t new_yi = (int16_t)((uint32_t)new_y >> 16);
    uint16_t collision_flags = check_entity_collision(new_xi, new_yi, 24, direction);
    npc_collision_check(new_xi, new_yi, game_state.current_party_members);

    /* Lines 141-143: If NPC collision detected, skip position update.
     * Assembly: CMP #ENTITY_COLLISION_NO_OBJECT; BNE @UNKNOWN9 */
    if (entities.collided_objects[23] != (int16_t)0xFFFF) {
        return;
    }

    /* Lines 144-148: Set leader_moved */
    game_state.leader_moved++;
    ow.player_has_moved_since_map_load++;

    /* Lines 149-154: If wall collision (bits 6-7 set), clear leader_moved */
    if (collision_flags & 0x00C0) {
        game_state.leader_moved = 0;
        return;
    }

    /* Lines 155-161: Write new position to game_state */
    game_state.leader_x_frac = (uint16_t)(new_x & 0xFFFF);
    game_state.leader_x_coord = (uint16_t)((uint32_t)new_x >> 16);
    game_state.leader_y_frac = (uint16_t)(new_y & 0xFFFF);
    game_state.leader_y_coord = (uint16_t)((uint32_t)new_y >> 16);
}

/*
 * check_hotspot_exit — check if the leader has left a hotspot boundary.
 *
 * Port of CHECK_HOTSPOT_EXIT (asm/overworld/check_hotspot_exit.asm).
 * Called twice per pair of frames from update_overworld_player_input
 * (alternating hotspot 0 and 1 each frame).
 *
 * When mode == 1 (inside check): if player is inside bounds, do nothing.
 *   If player is OUTSIDE bounds, deactivate hotspot, queue interaction type 9.
 * When mode == 2 (outside check): if player is outside bounds, do nothing.
 *   If player is INSIDE bounds (strictly), deactivate hotspot, queue interaction.
 * Mode == 0: inactive, do nothing.
 */
void check_hotspot_exit(uint16_t hotspot_idx) {
    /* Lines 12-14: Assembly does LDA NEXT_QUEUED_INTERACTION; EOR NEXT_QUEUED_INTERACTION
     * which is value XOR value = always 0, so BNEL never branches.
     * This is dead code in the original — possibly an intended
     * "skip if queue non-empty" check that was incorrectly implemented
     * (should have been CURRENT EOR NEXT). Match the assembly: no-op. */

    /* Lines 15-16: Skip during PSI teleport */
    if (ow.psi_teleport_destination != 0)
        return;

    /* Lines 17-23: Get hotspot entry and mode */
    ActiveHotspot *hs = &ow.active_hotspots[hotspot_idx];
    uint16_t mode = hs->mode;

    uint16_t player_x = game_state.leader_x_coord;
    uint16_t player_y = game_state.leader_y_coord;

    /* Lines 28-42: Mode 1 — inside check.
     * Player must be inside bounds to stay; if outside, trigger exit. */
    if (mode == 1) {
        if (player_x < hs->x1 || player_x > hs->x2 ||
            player_y < hs->y1 || player_y > hs->y2) {
            /* Player has left the hotspot — fall through to deactivate */
        } else {
            return;  /* Still inside, do nothing */
        }
    }
    /* Lines 43-55: Mode 2 — outside check.
     * Player must be outside bounds to stay; if inside (strictly), trigger. */
    else if (mode == 2) {
        if (player_x <= hs->x1 || player_x >= hs->x2 ||
            player_y <= hs->y1 || player_y >= hs->y2) {
            return;  /* Still outside, do nothing */
        }
        /* Player entered the zone — fall through to deactivate */
    }
    else {
        /* Mode 0 or other: inactive */
        return;
    }

    /* Lines 56-79: Deactivate hotspot and queue interaction */
    hs->mode = 0;

    /* Queue interaction type 9 (text trigger) with the hotspot's pointer */
    queue_interaction(9, hs->pointer);

    /* Clear the game_state active_hotspot_modes byte for this index */
    game_state.active_hotspot_modes[hotspot_idx] = 0;
}

/*
 * update_overworld_player_input — handle d-pad movement for normal walking.
 *
 * Port of UPDATE_OVERWORLD_PLAYER_INPUT (asm/overworld/update_overworld_player_input.asm).
 * Reads d-pad input, converts to direction, applies speed tables,
 * checks collision, and updates leader position in game_state.
 *
 * Called from update_leader_movement() for non-bicycle, non-escalator
 * walking styles.
 */
static void update_overworld_player_input(void) {
    /* Line 14: Clear leader_moved */
    game_state.leader_moved = 0;

    /* Lines 15-17: Mushroomization d-pad swap */
    if (ow.mushroomized_walking_flag)
        mushroomization_movement_swap();

    /* Lines 19-22: Map d-pad to direction using walking_style */
    int16_t direction = map_input_to_direction(game_state.walking_style);

    /* Lines 24-38: Battle swirl countdown */
    if (ow.battle_swirl_countdown) {
        ow.battle_swirl_countdown--;
        if (ow.battle_swirl_countdown == 0) {
            ow.battle_mode = 0xFFFF; /* -1: trigger battle */
        }
        npc_collision_check(game_state.leader_x_coord,
                           game_state.leader_y_coord,
                           game_state.current_party_members);
        return;
    }

    /* Lines 40-47: No direction input → return */
    if (direction == -1) {
        npc_collision_check(game_state.leader_x_coord,
                           game_state.leader_y_coord,
                           game_state.current_party_members);
        return;
    }

    /* Lines 48-98: Direction forcing for STAIRS walking style.
     * Assembly compares walking_style == 13 (STAIRS). When on stairs,
     * the input direction is forced to diagonal and facing is set.
     * STAIRS_DIRECTION 0x0100 and 0x0200 both use the "up/right" path;
     * other values use the "down/left" path. */
    if (game_state.walking_style == WALKING_STYLE_STAIRS) {
        if (ow.stairs_direction == 0x0100 || ow.stairs_direction == 0x0200) {
            /* Force UP_RIGHT (1) or DOWN_LEFT (5) */
            if (direction > 3)
                direction = 5;
            else
                direction = 1;
        } else {
            /* Force DOWN_RIGHT (3) or UP_LEFT (7) */
            if (((direction - 1) & 7) > 3)
                direction = 7;
            else
                direction = 3;
        }
        /* Set facing based on forced direction (assembly lines 82-92) */
        if ((uint16_t)direction >= 4)
            game_state.leader_direction = 6;  /* LEFT */
        else
            game_state.leader_direction = 2;  /* RIGHT */
    } else if (!(ow.player_movement_flags & 0x0001)) {
        /* Normal direction update (assembly lines 93-98) */
        game_state.leader_direction = (uint16_t)direction;
    }

    /* Lines 99-104: Set leader_moved and PLAYER_HAS_MOVED_SINCE_MAP_LOAD */
    ow.player_has_moved_since_map_load = 1;
    game_state.leader_moved = 1;

    /* Lines 105-128: Load current position and apply speed adjustments.
     * game_state.leader_x_frac + leader_x_coord form a 32-bit fixed-point X.
     * game_state.leader_y_frac + leader_y_coord form a 32-bit fixed-point Y. */
    uint16_t tile_type = game_state.trodden_tile_type;
    int32_t pos_x = (int32_t)(((uint32_t)game_state.leader_x_coord << 16) |
                               game_state.leader_x_frac);
    int32_t pos_y = (int32_t)(((uint32_t)game_state.leader_y_coord << 16) |
                               game_state.leader_y_frac);

    int32_t new_x = adjust_position(direction, tile_type, pos_x, pb.h_speeds);
    int32_t new_y = adjust_position(direction, tile_type, pos_y, pb.v_speeds);

    /* Lines 130-131: Reset ladder tile sentinel */
    ow.ladder_stairs_tile_x = -1;

    /* Lines 132-172: Collision checking.
     * If movement_locked (bit 1), skip directional collision → use GET_COLLISION_AT_PIXEL.
     * Otherwise, CHECK_DIRECTIONAL_COLLISION samples tile collision at the new position.
     * Returns surface flags — bits 6-7 (0xC0) = wall blocking.
     * Also sets ow.final_movement_direction (may change for wall sliding). */
    uint16_t collision_flags;
    if (ow.player_movement_flags & 0x0002) {
        /* Lines 160-172: Movement locked — use GET_COLLISION_AT_PIXEL. */
        if (ow.demo_frames_left) {
            collision_flags = 0;
        } else {
            collision_flags = get_collision_at_pixel(
                (int16_t)((uint32_t)new_x >> 16),
                (int16_t)((uint32_t)new_y >> 16)) & 0x003F;
        }
    } else {
        /* Normal path: directional collision check */
        collision_flags = check_directional_collision(
            (int16_t)((uint32_t)new_x >> 16),
            (int16_t)((uint32_t)new_y >> 16),
            direction);

        /* Lines 142-158: If direction was changed by collision (wall slide),
         * recalculate position using original coords + new direction. */
        if (ow.final_movement_direction != direction) {
            new_x = adjust_position(ow.final_movement_direction, tile_type, pos_x, pb.h_speeds);
            new_y = adjust_position(ow.final_movement_direction, tile_type, pos_y, pb.v_speeds);
        }
    }

    /* Lines 173-175: Store tile type */
    game_state.trodden_tile_type = collision_flags;

    /* Lines 176-192: NPC collision check.
     * Check if leader collides with any NPC at the new position. */
    int16_t new_xi = (int16_t)((uint32_t)new_x >> 16);
    int16_t new_yi = (int16_t)((uint32_t)new_y >> 16);
    npc_collision_check(new_xi, new_yi, game_state.current_party_members);
    int can_move = (entities.collided_objects[23] == (int16_t)0xFFFF) ? 1 : 0;

    /* Lines 188-192: Check tile collision bits 6-7 (0xC0) */
    if (collision_flags & 0x00C0)
        can_move = 0;

    /* Lines 194-209: Ladder tile door check. */
    if (ow.ladder_stairs_tile_x != -1) {
        can_move = (int)process_door_at_tile((uint16_t)ow.ladder_stairs_tile_x,
                                             (uint16_t)ow.ladder_stairs_tile_y);
    } else {
        /* Clear ladder/rope walking style if no ladder tile found
         * (assembly lines 203-209) */
        if (game_state.walking_style == WALKING_STYLE_LADDER ||
            game_state.walking_style == WALKING_STYLE_ROPE) {
            game_state.walking_style = 0;
        }
    }

    /* Lines 210-225: Write new position if movement allowed */
    if (can_move) {
        game_state.leader_x_frac = (uint16_t)(new_x & 0xFFFF);
        game_state.leader_x_coord = (uint16_t)((uint32_t)new_x >> 16);
        game_state.leader_y_frac = (uint16_t)(new_y & 0xFFFF);
        game_state.leader_y_coord = (uint16_t)((uint32_t)new_y >> 16);
    } else {
        game_state.leader_moved = 0;
    }

    /* Lines 226-243: Hotspot exit checks.
     * Alternates between hotspot 0 (even frames) and hotspot 1 (odd frames).
     * Only checks if the hotspot is active (mode != 0). */
    if (!(core.frame_counter & 1)) {
        if (ow.active_hotspots[0].mode)
            check_hotspot_exit(0);
    } else {
        if (ow.active_hotspots[1].mode)
            check_hotspot_exit(1);
    }

    /* Lines 244-255: Ladder alignment.
     * When on a ladder/rope, snap X to tile grid (assembly lines 250-255). */
    if (game_state.walking_style == WALKING_STYLE_LADDER ||
        game_state.walking_style == WALKING_STYLE_ROPE) {
        if (ow.ladder_stairs_tile_x != -1) {
            game_state.leader_x_coord =
                (uint16_t)(ow.ladder_stairs_tile_x * 8 + 8);
        }
    }

    /* Lines 256-269: Debug snap to grid (X button + DEBUG flag).
     * Snaps position to 8-pixel grid. */
    if (ow.debug_flag && (core.pad1_held & PAD_X)) {
        game_state.leader_x_coord &= 0xFFF8;
        game_state.leader_y_coord &= 0xFFF8;
    }
}

/*
 * update_leader_movement — leader entity tick callback.
 *
 * Port of UPDATE_LEADER_MOVEMENT (C04236, asm/overworld/update_leader_movement.asm).
 * Per-frame: saves/clears leader_moved, syncs position_index, dispatches movement
 * by walking_style, checks tile collision, writes position ert.buffer.
 */
void update_leader_movement(int16_t entity_offset) {
    (void)entity_offset;  /* Assembly reads leader from game_state, not caller's entity */

    /* 1. Save and clear leader_moved (assembly lines 10-14).
     * @LOCAL03 = game_state.leader_moved; game_state.leader_moved = 0.
     * Movement dispatchers (step 4) re-set it to 1 if the leader moves.
     * prev_leader_moved is passed to UPDATE_BICYCLE_MOVEMENT (assembly line 55). */
    uint16_t prev_leader_moved = game_state.leader_moved;
    game_state.leader_moved = 0;

    /* 2. Decrement intangibility frames if active (assembly lines 15-18) */
    if (ow.player_intangibility_frames != 0) {
        clear_party_sprite_hide_flags();
        ow.player_intangibility_frames--;
    }

    /* 3. Sync leader's char_struct.position_index with write head.
     * Assembly lines 30-39: reads current_party_members → entity var1 →
     * CHOSEN_FOUR_PTRS → char_struct.position_index = position_buffer_index.
     * CRITICAL: uses game_state.current_party_members (the leader's entity
     * slot) to find the leader entity, NOT the caller's entity_offset (which
     * is the init entity at slot 23 in attract mode). */
    int16_t leader_ent_offset = (int16_t)game_state.current_party_members;
    int16_t party_idx = entities.var[1][leader_ent_offset];
    if (party_idx >= 0 && party_idx < TOTAL_PARTY_COUNT) {
        party_characters[party_idx].position_index =
            game_state.position_buffer_index;
    }

    /* 4. Dispatch movement (assembly lines 40-59).
     * If camera_mode != 0, use UPDATE_SPECIAL_CAMERA_MODE instead of
     * the normal walking_style dispatch. */
    if (game_state.camera_mode != 0) {
        update_special_camera_mode();
    } else {
        /* Walking_style dispatch (assembly lines 44-59).
         * ESCALATOR dispatches to UPDATE_ESCALATOR_MOVEMENT.
         * BICYCLE dispatches to UPDATE_BICYCLE_MOVEMENT (with prev_leader_moved).
         * All others dispatch to UPDATE_OVERWORLD_PLAYER_INPUT. */
        if (game_state.walking_style == WALKING_STYLE_ESCALATOR) {
            update_escalator_movement();
            (void)prev_leader_moved;
        } else if (game_state.walking_style == WALKING_STYLE_BICYCLE) {
            update_bicycle_movement(prev_leader_moved);
        } else {
            update_overworld_player_input();
            (void)prev_leader_moved;
        }
    }

    /* 5. Tile collision check (assembly lines 61-82).
     * Assembly calls CHECK_PLAYER_COLLISION_AT_POSITION which reads the
     * entity's size from ENTITY_SIZES[slot*2]. */
    game_state.trodden_tile_type = lookup_surface_flags(
        (int16_t)game_state.leader_x_coord,
        (int16_t)game_state.leader_y_coord,
        entities.sizes[leader_ent_offset]);

    /* 6-7. Write to position ert.buffer (assembly lines 83-119).
     * position_buffer_write reads from game_state (matching assembly).
     * The x/y write and index advance are gated by leader_moved inside
     * position_buffer_write; tile_flags/walking_style/direction always written. */
    position_buffer_write(0);

    /* 7b. CENTER_SCREEN (assembly line 103) — update BG scroll registers
     * AND stream new tilemap rows/columns as camera scrolls.
     * Only called when leader_moved is set (assembly: BEQ @LEADER_NOT_MOVED).
     * CRITICAL: This MUST happen during Phase 1 (tick callback), BEFORE Phase 2
     * computes entity screen positions via entity_screen_coords_bg1. If scroll
     * is updated AFTER Phase 2 (as was previously done in render_frame_tick),
     * entity screen positions are computed from stale scroll values, causing a
     * 1-frame sprite-background offset that makes sprites appear to "slide"
     * relative to the background.
     *
     * Assembly: JSL CENTER_SCREEN → JSR REFRESH_MAP_AT_POSITION.
     * center_screen() calls map_refresh_tilemaps() which updates scroll
     * registers AND incrementally streams new map tile rows/columns. */
    if (game_state.leader_moved) {
        center_screen(game_state.leader_x_coord, game_state.leader_y_coord);
    }

    /* 8. Footstep sound override (assembly lines 120-136).
     * Tile type bit 3 = special surface; bit 2 distinguishes outdoor (16) vs indoor (18). */
    ow.footstep_sound_id_override = 0;
    if (game_state.trodden_tile_type & 0x0008) {
        if (game_state.trodden_tile_type & 0x0004) {
            ow.footstep_sound_id_override = 16;
        } else {
            ow.footstep_sound_id_override = 18;
        }
    }
}

/*
 * update_follower_state — per-frame follower tick callback.
 *
 * Port of UPDATE_FOLLOWER_STATE (C04D78, asm/overworld/update_follower_state.asm).
 * Reads the follower's historical position from the circular ert.buffer and
 * updates its entity state. Uses ADJUST_PARTY_MEMBER_VISIBILITY for
 * spacing-aware advancement.
 */
void update_follower_state(int16_t entity_offset) {
    /* Early exit checks (assembly lines 14-22) */
    if (game_state.camera_mode == 3) return;
    if (ow.battle_swirl_countdown) return;
    if (ow.enemy_has_been_touched) return;
    if (ow.battle_mode) return;

    int16_t char_id = entities.var[0][entity_offset];
    int16_t party_idx = entities.var[1][entity_offset];
    if (party_idx < 0 || party_idx >= TOTAL_PARTY_COUNT) return;

    uint16_t read_idx = party_characters[party_idx].position_index;
    PositionBufferEntry *entry = &pb.player_position_buffer[read_idx & 0xFF];

    /* Always: copy direction and surface_flags from ert.buffer (assembly lines 43-49) */
    entities.directions[entity_offset] = (int16_t)entry->direction;
    entities.surface_flags[entity_offset] = entry->tile_flags;

    /* Call UPDATE_PARTY_ENTITY_GRAPHICS each frame (assembly line 59).
     * CRITICAL: this is BEFORE the leader_moved gate (assembly lines 60-65).
     * Updates sprite graphics, walking_style, var3 (animation speed), var7 flags. */
    update_party_entity_graphics(char_id, entry->walking_style,
                                 entity_offset, party_idx);

    /* Position update gated on leader_moved or escalator (assembly lines 60-65) */
    if (!game_state.leader_moved &&
        entry->walking_style != WALKING_STYLE_ESCALATOR) {
        return;
    }

    entities.abs_x[entity_offset] = entry->x_coord;
    entities.abs_y[entity_offset] = entry->y_coord;

    /* Check if this char_id+1 equals party_order[0] — if so, this is the
     * "first" party member and gets simpler handling (assembly @UNKNOWN11/12).
     * Assembly: if (char_id + 1) == party_order[0] (first byte), skip spacing
     * and just increment by 1. */
    int hide_entity = 0;
    uint16_t target = (uint16_t)(char_id + 1);
    if ((game_state.party_order[0] & 0xFF) == target) {
        /* First follower — just advance by 1 (assembly @SIMPLE_ADVANCE path).
         * NOTE: Assembly stores buffer_walking_style once BEFORE branching (line 139).
         * We store it in each branch instead — functionally equivalent since the
         * value (entry->walking_style) is the same regardless of path taken. */
        party_characters[party_idx].buffer_walking_style = entry->walking_style;
        uint16_t new_idx = (read_idx + 1) & 0xFF;
        entities.var[7][entity_offset] &=
            (int16_t)(~(uint16_t)(1 << 12));  /* clear bit 12 (spacing hide) */
        party_characters[party_idx].position_index = new_idx;
        return;
    }

    /* Calculate spacing based on walking_style from ert.buffer (assembly lines 85-124) */
    uint16_t mode_spacing = 0;
    uint16_t walking_style = entry->walking_style & 0xFF;

    if (walking_style == WALKING_STYLE_LADDER ||
        walking_style == WALKING_STYLE_ROPE) {
        mode_spacing = 30;
    } else if (walking_style == WALKING_STYLE_ESCALATOR) {
        if (game_state.walking_style == 0) {
            /* Escalator with no walking style → hide entity */
            hide_entity = 1;
        } else {
            mode_spacing = 30;
        }
    } else if (walking_style == WALKING_STYLE_STAIRS) {
        mode_spacing = 24;
    } else if (game_state.character_mode == CHARACTER_MODE_SMALL) {
        mode_spacing = 8;
    } else {
        mode_spacing = 12;
    }

    /* Add character size (assembly: CHARACTER_SIZES[char_id]) */
    uint16_t char_size = 0;
    if (char_id >= 0 && (uint16_t)char_id < CHARACTER_SIZES_COUNT) {
        const uint8_t *sizes_data = ASSET_DATA(ASSET_DATA_CHARACTER_SIZES_BIN);
        char_size = read_u16_le(sizes_data + (uint16_t)char_id * 2);
    }
    uint16_t spacing = char_size + mode_spacing;

    /* Store walking_style to char_struct.buffer_walking_style (assembly lines 136-139) */
    party_characters[party_idx].buffer_walking_style = entry->walking_style;

    if (hide_entity) {
        /* Escalator with walking_style==0 → just advance by 1, skip spacing */
        uint16_t new_idx = (read_idx + 1) & 0xFF;
        entities.var[7][entity_offset] &=
            (int16_t)(~(uint16_t)(1 << 12));
        party_characters[party_idx].position_index = new_idx;
        return;
    }

    /* Use full spacing logic via ADJUST_PARTY_MEMBER_VISIBILITY */
    uint16_t new_idx = adjust_party_member_visibility(
        entity_offset, char_id, read_idx, spacing);
    party_characters[party_idx].position_index = new_idx & 0xFF;
}
