/*
 * Callroutine action script functions — entity preparation, fades, teleport.
 * Extracted from callroutine.c.
 */
#include "entity/callroutine_internal.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "game/game_state.h"
#include "game/overworld.h"
#include "game/fade.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/constants.h"
#include "core/memory.h"
#include "game_main.h"
#include <string.h>

/* ---- Inline switch case wrappers ---- */

int16_t cr_actionscript_prepare_entity(int16_t entity_offset, int16_t script_offset,
                                       uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A912 ACTIONSCRIPT_PREPARE_NEW_ENTITY.
     * Reads 5 bytes: X coord (16-bit), Y coord (16-bit), direction (8-bit).
     * Sets ENTITY_PREPARED_X/Y/DIRECTION for subsequent GENERATE_ACTIVE_SPRITE. */
    prepare_new_entity(sw(pc), sw(pc + 2), sb(pc + 4));
    *out_pc = pc + 5;
    return 0;
}

int16_t cr_actionscript_prepare_at_leader(int16_t entity_offset, int16_t script_offset,
                                          uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A8FF ACTIONSCRIPT_PREPARE_NEW_ENTITY_AT_PARTY_LEADER.
     * A=1 means use the party leader entity. */
    prepare_new_entity_at_existing_entity_location(1);
    *out_pc = pc;
    return 0;
}

int16_t cr_actionscript_prepare_at_self(int16_t entity_offset, int16_t script_offset,
                                        uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A8F7 ACTIONSCRIPT_PREPARE_NEW_ENTITY_AT_SELF.
     * A=0 means use CURRENT_ENTITY_SLOT. */
    prepare_new_entity_at_existing_entity_location(0);
    *out_pc = pc;
    return 0;
}

int16_t cr_actionscript_fade_out(int16_t entity_offset, int16_t script_offset,
                                 uint16_t pc, uint16_t *out_pc) {
    /* Port of C09FBB ACTIONSCRIPT_FADE_OUT.
     * Reads 2 bytes: low byte = step, high byte = delay.
     * Assembly: XBA/TAX/XBA decodes A_lo=step, X_lo=delay.
     * FADE_OUT negates step and stores into FADE_PARAMETERS.
     * Non-blocking — NMI handler applies per-frame brightness changes. */
    uint16_t param = sw(pc);
    *out_pc = pc + 2;
    uint8_t step = param & 0xFF;
    uint8_t delay = (param >> 8) & 0xFF;
    fade_to_brightness(0, step, delay);
    return 0;
}

int16_t cr_actionscript_fade_in(int16_t entity_offset, int16_t script_offset,
                                uint16_t pc, uint16_t *out_pc) {
    /* Port of C0886C ACTIONSCRIPT_FADE_IN.
     * Reads 2 bytes: low byte = step, high byte = delay.
     * Assembly: XBA/TAX/XBA decodes A_lo=step, X_lo=delay.
     * FADE_IN stores step (positive) into FADE_PARAMETERS.
     * Non-blocking — NMI handler applies per-frame brightness changes. */
    uint16_t param = sw(pc);
    *out_pc = pc + 2;
    uint8_t step = param & 0xFF;
    uint8_t delay = (param >> 8) & 0xFF;
    fade_to_brightness(0x0F, step, delay);
    return 0;
}

int16_t cr_actionscript_get_party_member_pos(int16_t entity_offset, int16_t script_offset,
                                             uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A943 ACTIONSCRIPT_GET_POSITION_OF_PARTY_MEMBER.
     * Reads 1 byte: party member index (0xFE = last party member).
     * Copies target entity's abs_x/abs_y to current entity's var6/var7. */
    uint8_t pm_param = sb(pc);
    *out_pc = pc + 1;
    int16_t pm_slot;
    if (pm_param == 0xFE) {
        /* Last party member: assembly uses party_count - 1 as index
         * into party_entity_slots, with fallback to party_count - 2
         * if the entity's abs_x == 0. */
        int idx = game_state.party_count - 1;
        if (idx < 0) idx = 0;
        pm_slot = (int16_t)read_u16_le(&game_state.party_entity_slots[idx * 2]);
        if (entities.abs_x[pm_slot] == 0 && idx > 0) {
            idx = game_state.party_count - 2;
            pm_slot = (int16_t)read_u16_le(&game_state.party_entity_slots[idx * 2]);
        }
    } else if (pm_param == 0xFF) {
        pm_slot = (int16_t)game_state.current_party_members;
    } else {
        pm_slot = find_entity_for_character(pm_param);
        if (pm_slot < 0)
            pm_slot = (int16_t)game_state.current_party_members;
    }
    int16_t ent_off = ENT(ert.current_entity_slot);
    entities.var[6][ent_off] = entities.abs_x[pm_slot];
    entities.var[7][ent_off] = entities.abs_y[pm_slot];
    return 0;
}

int16_t cr_prepare_entity_at_teleport_dest(int16_t entity_offset, int16_t script_offset,
                                           uint16_t pc, uint16_t *out_pc) {
    /* Port of C46DE5 PREPARE_NEW_ENTITY_AT_TELEPORT_DESTINATION.
     * Reads 1 byte: teleport destination index. */
    prepare_new_entity_at_teleport_destination(sb(pc));
    *out_pc = pc + 1;
    return 0;
}

int16_t cr_actionscript_fade_out_with_mosaic(int16_t entity_offset, int16_t script_offset,
                                             uint16_t pc, uint16_t *out_pc) {
    /* Port of C0AA07 ACTIONSCRIPT_FADE_OUT_WITH_MOSAIC.
     * Reads 6 bytes: three 16-bit params (step, delay, mosaic_enable).
     * Assembly: MOVEMENT_DATA_READ16 x3, then JSL FADE_OUT_WITH_MOSAIC.
     * Identical behavior to ROM_ADDR_FADE_OUT_WITH_MOSAIC but at a
     * different ROM address (actionscript wrapper). */
    uint16_t step = sw(pc);
    uint16_t delay = sw(pc + 2);
    uint16_t mosaic_bgs = sw(pc + 4);
    *out_pc = pc + 6;
    fade_out_with_mosaic(step, delay, mosaic_bgs);
    return 0;
}

int16_t cr_actionscript_prepare_at_teleport(int16_t entity_offset, int16_t script_offset,
                                            uint16_t pc, uint16_t *out_pc) {
    /* Port of C0A907 ACTIONSCRIPT_PREPARE_NEW_ENTITY_AT_TELEPORT_DESTINATION.
     * Reads 1 byte: teleport destination index.
     * Assembly: MOVEMENT_DATA_READ8, then JSL PREPARE_NEW_ENTITY_AT_TELEPORT_DESTINATION. */
    uint8_t dest_id = sb(pc);
    *out_pc = pc + 1;
    prepare_new_entity_at_teleport_destination(dest_id);
    return 0;
}
