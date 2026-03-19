/*
 * Callroutine entity sprite functions — direction, frame, animation, collision.
 * Extracted from callroutine.c.
 */
#include "entity/callroutine_internal.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "game/game_state.h"
#include "game/overworld.h"
#include "game/audio.h"
#include "game/battle.h"
#include "game/map_loader.h"
#include "game/position_buffer.h"
#include "core/math.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/constants.h"
#include "core/memory.h"
#include "game_main.h"
#include <stdlib.h>
#include <string.h>

/* ---- Static cr_* functions moved from callroutine.c ---- */

/*
 * CR_SET_ENTITY_DIRECTION_AND_FRAME (C0AA6E)
 *
 * Reads 2 parameter bytes from the script: direction and frame.
 * Sets the entity's direction and animation frame.
 */
int16_t cr_set_entity_direction_and_frame(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc) {
    uint8_t direction = sb(pc);
    uint8_t frame = sb((uint16_t)(pc + 1));
    *out_pc = pc + 2;

    entities.directions[ent] = (int16_t)direction;

    /* Assembly: checks ENTITY_SCRIPT_VAR0_TABLE,X for 4-dir vs 8-dir.
     * 8-dir path does ASL on frame (0→0, 1→2) before storing.
     * Both paths call RENDER_ENTITY_SPRITE to update visuals immediately. */
    if (entities.var[0][ent] != 0) {
        /* 8-dir: ASL frame before storing */
        entities.animation_frame[ent] = (int16_t)(frame << 1);
    } else {
        entities.animation_frame[ent] = (int16_t)frame;
    }
    render_entity_sprite(ent);
    return 0;
}

/*
 * CR_DEALLOCATE_ENTITY_SPRITE (C020F1)
 *
 * Faithful port of DEALLOCATE_ENTITY_SPRITE:
 *   1. CLEAR_SPRITEMAP_SLOTS — free overworld spritemap ert.buffer entries
 *   2. ALLOC_SPRITE_MEM(0) — free sprite VRAM allocation
 *   3. Set ENTITY_SPRITE_IDS = -1  (prevents stale sprite_id matches)
 *   4. Set ENTITY_NPC_IDS = -1
 *
 * Assembly uses CURRENT_ENTITY_SLOT (not a parameter).
 */
int16_t cr_deallocate_entity_sprite(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    /* Assembly: LDA CURRENT_ENTITY_SLOT / ASL / TAY */
    int16_t offset = ENT(ert.current_entity_slot);

    /* CLEAR_SPRITEMAP_SLOTS: mark spritemap ert.buffer entries as free (0xFF).
     * Two direction chunks at spritemap_ptr_lo, each spritemap_sizes bytes. */
    {
        uint16_t smap_ptr = entities.spritemap_ptr_lo[offset];
        uint16_t smap_size = entities.spritemap_sizes[offset];
        uint16_t total = smap_size * 2;
        if (total > 0 && smap_ptr + total <= OVERWORLD_SPRITEMAPS_SIZE) {
            memset(overworld_spritemaps + smap_ptr, 0xFF, total);
        }
    }

    /* ASM line 14-16: ALLOC_SPRITE_MEM(ert.current_entity_slot, 0) — free VRAM slots
     * by searching for entries tagged with this entity's ID. */
    alloc_sprite_mem((uint16_t)ert.current_entity_slot, 0);

    /* Assembly lines 17-22: if NPC ID upper bits == 0x8000, it's an enemy —
     * decrement ow.overworld_enemy_count. */
    if ((entities.npc_ids[offset] & 0xF000) == 0x8000) {
        ow.overworld_enemy_count--;
    }

    /* Assembly lines 24-31: if enemy ID == MAGIC_BUTTERFLY (225),
     * clear the ow.magic_butterfly_spawned flag. */
    if (entities.enemy_ids[offset] == ENEMY_MAGIC_BUTTERFLY) {
        ow.magic_butterfly_spawned = 0;
    }

    /* Assembly: LDA #-1 / STA ENTITY_SPRITE_IDS,X / STA ENTITY_NPC_IDS,X */
    entities.sprite_ids[offset] = -1;
    entities.npc_ids[offset] = -1;

    return 0;
}

/*
 * CR_IS_X_LESS_THAN_ENTITY (C468B5)
 *
 * Parameter comes via A register (= tempvar), NOT from the script stream.
 * Assembly: STACK_RESERVE_PARAM_INT16, STA @LOCAL01, CMP ENTITY_ABS_X_TABLE,X
 * Returns 1 if param < entity.abs_x, 0 otherwise.
 */
int16_t cr_is_x_less_than_entity(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;  /* no extra bytes consumed */
    uint16_t x_val = (uint16_t)scripts.tempvar[scr];

    /* Assembly uses CMP + BCS = unsigned comparison */
    return (x_val < (uint16_t)entities.abs_x[ent]) ? 1 : 0;
}

/*
 * CR_IS_Y_LESS_THAN_ENTITY (C468DC)
 *
 * Parameter comes via A register (= tempvar), NOT from the script stream.
 * Assembly: STACK_RESERVE_PARAM_INT16, STA @LOCAL01, CMP ENTITY_ABS_Y_TABLE,X
 * Returns 1 if param < entity.abs_y, 0 otherwise.
 */
int16_t cr_is_y_less_than_entity(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;  /* no extra bytes consumed */
    uint16_t y_val = (uint16_t)scripts.tempvar[scr];

    /* Assembly uses CMP + BCS = unsigned comparison */
    return (y_val < (uint16_t)entities.abs_y[ent]) ? 1 : 0;
}

/*
 * CR_CLEAR_LOOP_COUNTER (C0AAFD) — MOVEMENT_CMD_CLEAR_LOOP_COUNTER
 *
 * Stores 0 at script data offset $0C (the loop counter used by var[6]).
 * Assembly: LDA #$0000 / LDY #$000C / STA ($84),Y / RTL
 */
int16_t cr_clear_loop_counter(int16_t ent, int16_t scr,
                              uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    /* Assembly: LDA #$0000 / LDY #$000C / STA ($84),Y
     * Clears the loop counter at script stack offset $0C.
     * (NOT entities.var[6] — that's the movement target X coordinate.) */
    scripts.stack[scr][12] = 0;
    scripts.stack[scr][13] = 0;
    return 0;
}

/*
 * CR_MOVEMENT_LOOP (C0AAD5) — MOVEMENT_CMD_LOOP
 *
 * Reads 3 parameter bytes: 1-byte loop count + 2-byte target address.
 * Uses entity var[6] as the loop counter.
 * First call: initializes var[6] = count + 1.
 * Each call: decrements var[6]. If non-zero, jumps to target. If zero, falls through.
 */
int16_t cr_movement_loop(int16_t ent, int16_t scr,
                         uint16_t pc, uint16_t *out_pc) {
    uint8_t count = sb(pc);
    uint16_t target_rom = (uint16_t)(sb((uint16_t)(pc + 1)) |
                                     (sb((uint16_t)(pc + 2)) << 8));
    *out_pc = pc + 3;

    int16_t init_val = (int16_t)(count + 1);

    /* If loop counter is 0 (first call), initialize it.
     * Loop counter is at script stack offset $0C (16-bit). */
    int16_t loop_ctr = (int16_t)read_u16_le(&scripts.stack[scr][12]);
    if (loop_ctr == 0) {
        loop_ctr = init_val;
    }

    /* Decrement and check */
    loop_ctr--;
    scripts.stack[scr][12] = (uint8_t)(loop_ctr & 0xFF);
    scripts.stack[scr][13] = (uint8_t)((loop_ctr >> 8) & 0xFF);
    if (loop_ctr != 0) {
        /* Loop back: set PC to target address */
        *out_pc = script_bank_rom_to_offset(cr_bank_idx, target_rom);
    }
    /* else: fall through (counter reached 0) */

    return 0;
}

/*
 * CR_RENDER_ENTITY_SPRITE_ME2 (C0A4BF)
 *
 * RENDER_ENTITY_SPRITE_MOVEMENT_ENTRY_2: renders entity sprite with frame 0.
 * In the ROM: STZ USE_SECOND_SPRITE_FRAME, then falls through directly to
 * RENDER_ENTITY_SPRITE_SETUP_AND_RENDER (no on-screen check).
 * Uploads tiles to VRAM and sets ENTITY_CURRENT_DISPLAYED_SPRITES.
 */
int16_t cr_render_entity_sprite_me2(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    /* ASM: STZ USE_SECOND_SPRITE_FRAME → render with frame 0 */
    entities.animation_frame[ent] = 0;
    /* ASM: falls through to RENDER_ENTITY_SPRITE_SETUP_AND_RENDER */
    render_entity_sprite(ent);
    return 0;
}

/*
 * CR_RENDER_ENTITY_SPRITE_ME1 (C0A4B2)
 *
 * RENDER_ENTITY_SPRITE_MOVEMENT_ENTRY_1: renders entity sprite with frame 1.
 * In the ROM: sets USE_SECOND_SPRITE_FRAME=1, checks IS_ENTITY_ON_SCREEN,
 * then uploads tiles to VRAM if on screen.
 */
int16_t cr_render_entity_sprite_me1(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    /* ASM: LDA #$0001 / STA USE_SECOND_SPRITE_FRAME → render with frame 1 */
    entities.animation_frame[ent] = 1;
    /* ASM: JSL IS_ENTITY_ON_SCREEN / BNE SETUP_AND_RENDER
     * Only upload tiles if entity is on screen. */
    render_entity_sprite(ent);
    return 0;
}

/*
 * CR_RESET_ENTITY_ANIMATION (C40015)
 *
 * RESET_ENTITY_ANIMATION: resets animation frame to 0, renders sprite,
 * then checks if entity is on screen relative to leader position.
 *
 * ROM flow:
 *   1. STZ ENTITY_ANIMATION_FRAME,X — reset animation frame
 *   2. JSL RENDER_ENTITY_SPRITE_ENTRY3 — render with frame 0
 *   3. JSL IS_ENTITY_NEAR_LEADER — check if entity is near leader (on screen)
 *   4. RTL — return on-screen result
 *
 * Return value is CRITICAL — feeds into tempvar for EVENT_SHORTCALL_CONDITIONAL_NOT
 * loop. Returns -1 if entity is on screen, 0 if off screen.
 *
 * IS_ENTITY_NEAR_LEADER checks:
 *   - If PSI_TELEPORT_SPEED >= 4, always returns -1
 *   - Calculates entity position relative to (leader - screen_center)
 *   - Returns -1 if within [-64, 320) on both axes, 0 otherwise
 */
int16_t cr_reset_entity_animation(int16_t ent, int16_t scr,
                                  uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;

    /* ASM line 4: STZ ENTITY_ANIMATION_FRAME,X — reset animation frame to 0 */
    entities.animation_frame[ent] = 0;

    /* ASM line 5: JSL RENDER_ENTITY_SPRITE_ENTRY3
     * ENTRY3: STZ USE_SECOND_SPRITE_FRAME, checks IS_ENTITY_ON_SCREEN,
     * if on screen → uploads tiles to VRAM via SETUP_AND_RENDER. */
    render_entity_sprite(ent);

    /* ASM line 6: JSL IS_ENTITY_NEAR_LEADER — return on-screen result */
    /* During fast teleportation, always treat entities as on-screen */
    if (ow.psi_teleport_speed_int >= 4)
        return -1;
    int16_t leader_sx = (int16_t)game_state.leader_x_coord - VIEWPORT_CENTER_X;
    int16_t leader_sy = (int16_t)game_state.leader_y_coord - VIEWPORT_CENTER_Y;

    int16_t rel_x = entities.abs_x[ent] - leader_sx;
    int16_t rel_y = entities.abs_y[ent] - leader_sy;

    /* Entity is on screen if within [-64, VIEWPORT+64) in both axes */
    if (rel_x >= -64 && rel_x < (VIEWPORT_WIDTH + 64) && rel_y >= -64 && rel_y < (VIEWPORT_HEIGHT + 96))
        return -1;  /* on screen */
    return 0;  /* off screen */
}

/* ---- Inline switch case wrappers ---- */

int16_t cr_initialize_party_member_entity(int16_t entity_offset, int16_t script_offset,
                                          uint16_t pc, uint16_t *out_pc) {
    /* C03DAA: INITIALIZE_PARTY_MEMBER_ENTITY
     * Port of asm/overworld/party/initialize_party_member_entity.asm.
     * Initializes party member entity for walking animation:
     *   - animation_fingerprints = -1 (force re-render)
     *   - var3 = 8 (animation period; 16 if unconscious)
     *   - var2 = random 0-15 (staggered animation start)
     *   - Calls UPDATE_ENTITY_SPRITE (sprite re-render)
     *   - Updates char_struct fields for party tracking
     *   - Sets FOOTSTEP_SOUND_IGNORE_ENTITY */
    *out_pc = pc;
    int16_t offset = ENT(ert.current_entity_slot);

    /* Force animation re-render on next UPDATE_ENTITY_ANIMATION call */
    entities.animation_fingerprints[offset] = -1;

    /* var3 = 8 (animation period — frames between walk frame toggles) */
    entities.var[3][offset] = 8;

    /* var2 = random 0-15 (staggered start so party members don't
     * all toggle walk frames in sync) */
    entities.var[2][offset] = (int16_t)(rng_next_byte() & 0x0F);

    /* In ROM, calls UPDATE_ENTITY_SPRITE (JSL C0A780) which renders
     * the sprite for the current direction, sets use_8dir_sprites = 1,
     * and uploads initial sprite tiles to VRAM. This does NOT modify
     * ENTITY_ANIMATION_FRAME — the entity stays hidden (animation_frame=-1)
     * until a SET_ANIMATION opcode in the event script makes it visible. */
    update_entity_sprite(offset);

    /* char_struct updates: var1 = party member index (0-based),
     * var0 = character_index (0-based).
     * These track which entity belongs to which party character. */
    int16_t var1 = entities.var[1][offset];  /* party member index */
    if (var1 >= 0 && var1 < TOTAL_PARTY_COUNT) {
        CharStruct *c = &party_characters[var1];
        c->unknown59 = (uint16_t)ert.current_entity_slot;
        c->unknown53 = (uint16_t)entities.var[0][offset];
        c->unknown57 = 0;
        /* Assembly stores 16-bit -1 across unused_92+unused_93 (dead storage) */
        c->unused_92 = 0xFF;
        c->unused_93 = 0xFF;

        /* If party member is unconscious, slow animation (var3=16 vs 8) */
        if (c->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS)
            entities.var[3][offset] = 16;
    }

    /* Assembly line 50-52: FOOTSTEP_SOUND_IGNORE_ENTITY = leader entity offset.
     * The footstep sound only plays for this entity (the party leader). */
    ow.footstep_sound_ignore_entity = (int16_t)game_state.current_party_members;

    return 0;
}

int16_t cr_update_follower_visuals(int16_t entity_offset, int16_t script_offset,
                                   uint16_t pc, uint16_t *out_pc) {
    /* Port of C04EF0 UPDATE_FOLLOWER_VISUALS.
     * Called once during EVENT_002 (party follower) init to set follower entity direction,
     * surface_flags, graphics pointers, walking_style, and var7 flags
     * from the position ert.buffer. Not per-frame — update_follower_state
     * (tick callback) handles the per-frame updates. */
    update_follower_visuals(entity_offset);
    *out_pc = pc;
    return 0;
}

int16_t cr_sram_check_routine_checksum(int16_t entity_offset, int16_t script_offset,
                                       uint16_t pc, uint16_t *out_pc) {
    /* No-op in C port — SRAM checksum verification (C1FFD3).
     * The ROM verifies save data integrity on each frame. The C port
     * uses its own save system, making this permanently unnecessary. */
    *out_pc = pc;
    return 0;
}

int16_t cr_inflict_sunstroke_check(int16_t entity_offset, int16_t script_offset,
                                   uint16_t pc, uint16_t *out_pc) {
    /* Port of C20000 INFLICT_SUNSTROKE_CHECK.
     * Checks if the current terrain causes sunstroke (CAUSES_SUNSTROKE bit
     * set, SHALLOW_WATER bit clear). For each party member, rolls a
     * chance based on guts: probability = ((30 - guts) * 256) / 100.
     * If the character has no persistent affliction (or has COLD), may
     * inflict SUNSTROKE. RAND returns 0-255; sunstroke if rand <= prob. */
    *out_pc = pc;
    if (ow.overworld_status_suppression != 0) return 0;
    /* Check tile: (trodden_tile_type & 0x0C) must equal 4
     * (CAUSES_SUNSTROKE set, SHALLOW_WATER clear) */
    if ((game_state.trodden_tile_type & 0x000C) != 4) return 0;
    for (int i = 0; i < 6; i++) {
        uint8_t order = game_state.party_order[i];
        if (order == 0) return 0;
        /* CLC; SBC #4 = order - 5. BRANCHGTS: if (order - 5) > 0, return */
        if (order > 4) return 0;
        uint8_t member = game_state.player_controlled_party_members[i];
        uint8_t affliction = party_characters[member].afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
        if (affliction != 0 && affliction != STATUS_0_COLD)
            continue;
        uint8_t guts = party_characters[member].guts;
        int16_t diff = 30 - (int16_t)guts;
        /* Assembly: CMP #$8000; BLTEQ (unsigned <=).
         * If diff > 0x8000 (negative wrap), clamp to 1. */
        if ((uint16_t)diff > 0x8000) diff = 1;
        /* XBA; AND #$FF00; DIV by 100 → (diff * 256) / 100 */
        uint16_t probability = (uint16_t)((uint16_t)diff << 8) / 100;
        /* RAND returns 0-255. BGT (unsigned >): skip if rand > probability */
        uint16_t rnd = (uint16_t)rng_next_byte();
        if (rnd > probability) continue;
        /* Inflict sunstroke */
        party_characters[member].afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = STATUS_0_SUNSTROKE;
    }
    return 0;
}

int16_t cr_check_entity_enemy_collision(int16_t entity_offset, int16_t script_offset,
                                        uint16_t pc, uint16_t *out_pc) {
    /* Port of C0D15C CHECK_ENTITY_ENEMY_COLLISION.
     * Checks collision state variables to determine if current entity
     * has collided with an enemy. Returns -1 if collision, 0 otherwise.
     * Checks: player movement flags, leader collision, NPC slot range. */
    *out_pc = pc;
    return check_entity_enemy_collision();
}

int16_t cr_get_overworld_status(int16_t entity_offset, int16_t script_offset,
                                uint16_t pc, uint16_t *out_pc) {
    /* Port of C0C35D GET_OVERWORLD_STATUS.
     * Simply returns game_state.leader_moved. */
    *out_pc = pc;
    return (int16_t)game_state.leader_moved;
}

int16_t cr_check_prospective_entity_collision(int16_t entity_offset, int16_t script_offset,
                                              uint16_t pc, uint16_t *out_pc) {
    /* Port of C064A6 CHECK_PROSPECTIVE_ENTITY_COLLISION.
     * Calculates entity's next-frame position and checks for collision
     * with other entities. Updates ENTITY_COLLIDED_OBJECTS. */
    *out_pc = pc;
    check_prospective_entity_collision();
    return 0;
}

int16_t cr_render_entity_sprite_me3(int16_t entity_offset, int16_t script_offset,
                                    uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A480 RENDER_ENTITY_SPRITE_MOVEMENT_ENTRY_3.
     * Assembly: LDY $88; loads ENTITY_ANIMATION_FRAME[Y] into
     * USE_SECOND_SPRITE_FRAME; then JSL RENDER_ENTITY_SPRITE_ENTRY4.
     * Uses entity's current animation_frame to select sprite frame,
     * then renders the entity sprite (uploads tiles to VRAM). */
    *out_pc = pc;
    render_entity_sprite(entity_offset);
    return 0;
}
