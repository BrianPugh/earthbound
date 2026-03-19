/*
 * Event script bytecode interpreter.
 *
 * Ports of:
 *   EXECUTE_MOVEMENT_SCRIPT     (asm/overworld/execute_movement_script.asm)
 *   RUN_ENTITY_SCRIPTS_AND_TICK (asm/overworld/entity/run_entity_scripts_and_tick.asm)
 *   RUN_ACTIONSCRIPT_FRAME      (asm/overworld/actionscript/run_actionscript_frame.asm)
 */
#include "entity/entity.h"
#include "entity/script.h"
#include "data/event_script_data.h"
#include <string.h>
#include <stdio.h>

/* Script bank identifier for the title screen script data */
#define SCRIPT_BANK_TITLE 0

/* Forward declarations for opcode handlers (opcodes.c) */
extern uint16_t opcode_dispatch(uint8_t opcode, int16_t script_offset,
                                int16_t entity_offset, uint16_t pc);

/* Forward declaration for tick callback dispatch (callroutine.c) */
extern void dispatch_tick_callback(uint32_t rom_addr, int16_t entity_offset);

/* Forward declarations for callbacks (callbacks.c) */
extern void call_move_callback(int16_t entity_offset);
extern void call_screen_pos_callback(int16_t entity_offset);
extern void build_entity_draw_list(void);

/*
 * event_script_init — Initialize a script's PC from the event script pointer table.
 *
 * All script IDs are resolved through the global EVENT_SCRIPT_POINTERS table
 * and the registered script bank system. Title screen scripts (787-798) are
 * resolved the same way as any other script — via their registered bank.
 */
void event_script_init(uint16_t script_id, int16_t entity_offset,
                       int16_t script_offset) {
    int bank_idx;
    uint16_t offset;
    if (resolve_script_id(script_id, &bank_idx, &offset)) {
        scripts.pc[script_offset] = offset;
        scripts.pc_bank[script_offset] = (uint8_t)bank_idx;
        scripts.sleep_frames[script_offset] = 0;
        scripts.stack_offset[script_offset] = 0;
        LOG_TRACE("event_script_init: script=%u -> bank=%d offset=%u (ent=%d scr=%d)\n",
                  script_id, bank_idx, offset, entity_offset, script_offset);
        return;
    }

    /* Unknown script — halt immediately */
    LOG_WARN("WARN: unknown script ID %u (ent=%d)\n", script_id, entity_offset);
    scripts.pc[script_offset] = 0;
    scripts.pc_bank[script_offset] = 0xFF;
    scripts.sleep_frames[script_offset] = -1;
    scripts.stack_offset[script_offset] = 0;
}

/*
 * Read a byte from the script bank at the given PC offset.
 * Uses the generalized script bank system.
 */
static inline uint8_t read_script_byte(uint16_t bank, uint16_t offset) {
    return script_bank_read_byte((int)bank, offset);
}

/*
 * EXECUTE_MOVEMENT_SCRIPT (C09506)
 *
 * The bytecode VM loop:
 *   1. If sleep_frames > 0, decrement and return.
 *   2. Otherwise, fetch opcodes and dispatch until sleep_frames becomes non-zero.
 *   3. Save PC, decrement sleep_frames, return.
 *
 * Opcodes < 0x70 are dispatched through the main opcode table.
 * Opcodes >= 0x70 are "extended" opcodes where:
 *   bits 0-3 = sleep frame count
 *   bits 4-6 = extended handler index
 *   Extended handler maps: 0→0x3B, 1→0x3C, ..., 6→0x42 (SET_ANIMATION..CALLROUTINE)
 */
void execute_movement_script(int16_t script_offset) {
    if (scripts.sleep_frames[script_offset] != 0) {
        scripts.sleep_frames[script_offset]--;
        return;
    }

    uint16_t pc = scripts.pc[script_offset];

    int iterations = 0;
    while (1) {
        /* Re-read bank each iteration: opcodes 03 (LONGJUMP), 04 (LONGCALL),
         * and 05 (LONGRETURN) modify scripts.pc_bank[] directly. Using a
         * stale local copy would read opcodes from the wrong bank. */
        uint8_t bank = scripts.pc_bank[script_offset];
        uint8_t opcode = read_script_byte(bank, pc);
        pc++;

        if (opcode < 0x70) {
            /* Standard opcode dispatch */
            pc = opcode_dispatch(opcode, script_offset,
                                 ert.current_entity_offset, pc);
        } else {
            /* Extended opcode: bits 0-3 = sleep, bits 4-6 = handler */
            scripts.sleep_frames[script_offset] = opcode & 0x0F;
            uint8_t ext_idx = (opcode & 0x70) >> 4;
            /* Map extended index to opcode: 0→0x3B, 1→0x3C, ..., 6→0x42 */
            uint8_t mapped_opcode = 0x3B + ext_idx;
            pc = opcode_dispatch(mapped_opcode, script_offset,
                                 ert.current_entity_offset, pc);
        }

        if (scripts.sleep_frames[script_offset] != 0)
            break;

        if (++iterations > 50000) {
            fprintf(stderr, "SCRIPT HANG: entity=%d script=%d pc=%u bank=%u opcode=0x%02X after %d iters\n",
                    ert.current_entity_offset, script_offset, pc, bank, opcode, iterations);
            scripts.sleep_frames[script_offset] = 1;
            break;
        }
    }

    scripts.pc[script_offset] = pc;
    scripts.sleep_frames[script_offset]--;
}

/*
 * RUN_ENTITY_SCRIPTS_AND_TICK (C094D0)
 *
 * For a single entity: execute all its scripts, then call tick callback.
 */
static void run_entity_scripts_and_tick(int16_t entity_offset) {
    /* Assembly (C094D0):
     *   BIT ENTITY_TICK_CALLBACK_HIGH,X  ; N=bit15, V=bit14
     *   BVS @skip_scripts                ; bit14 → skip scripts ONLY
     *   ... execute scripts ...
     *   @skip_scripts:
     *   LDA ENTITY_TICK_CALLBACK_HIGH,X
     *   BMI @skip_tick                   ; bit15 → skip tick callback
     *   ... call tick callback ...
     *   @skip_tick:
     *   RTS
     *
     * Bit 14 (OBJECT_MOVE_DISABLED) skips entity scripts only.
     * Bit 15 (OBJECT_TICK_DISABLED) skips the tick callback only.
     * These are independent checks — bit 14 does NOT skip the tick callback. */

    /* Execute entity scripts unless bit 14 is set */
    if (!(entities.tick_callback_hi[entity_offset] & 0x4000)) {
        int16_t script_idx = entities.script_index[entity_offset];
        while (script_idx >= 0) {
            ert.current_script_offset = script_idx;
            ert.current_script_slot = script_idx;
            ert.actionscript_current_script = scripts.next_script[script_idx];
            execute_movement_script(script_idx);
            script_idx = ert.actionscript_current_script;
        }
    }

    /* Tick callback (if not disabled: bit 15 clear in tick_callback_hi).
     * Port of RUN_ENTITY_SCRIPTS_AND_TICK's JUMP_TO_LOADED_MOVEMENT_PTR call.
     * The tick callback runs every frame after all scripts for this entity. */
    if (!(entities.tick_callback_hi[entity_offset] & 0x8000)) {
        uint32_t tick_addr = ((uint32_t)(entities.tick_callback_hi[entity_offset] & 0xFF) << 16)
                           | entities.tick_callback_lo[entity_offset];
        if (tick_addr != 0) {
            dispatch_tick_callback(tick_addr, entity_offset);
        }
    }
}

/*
 * RUN_ACTIONSCRIPT_FRAME (run_actionscript_frame.asm)
 *
 * Per-frame entity system update:
 *   Phase 1: Run scripts + tick callbacks for each entity
 *   Phase 2: Movement + screen position callbacks
 *   Phase 3: Draw all entities (build draw list → render spritemaps → OAM)
 */
void run_actionscript_frame(void) {
    if (ert.disable_actionscript)
        return;

    ert.disable_actionscript = 1;

    /* Phase 1: Scripts + tick */
    int16_t ent = entities.first_entity;
    while (ent >= 0) {
        ert.current_entity_offset = ent;
        ert.current_entity_slot = ent;
        ert.next_active_entity = entities.next_entity[ent];
        run_entity_scripts_and_tick(ent);
        ent = ert.next_active_entity;
    }

    /* Phase 2: Movement + screen position callbacks */
    ent = entities.first_entity;
    while (ent >= 0) {
        ert.current_entity_slot = ent;
        ert.current_entity_offset = ent;

        /* Skip move if tick_callback_hi bit 14 is set */
        if (!(entities.tick_callback_hi[ent] & 0x4000)) {
            call_move_callback(ent);
        }
        call_screen_pos_callback(ent);

        ent = entities.next_entity[ent];
    }

    /* Phase 3: Draw entities */
    build_entity_draw_list();

    ert.disable_actionscript = 0;
}
