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
#include "core/log.h"
#include "core/mode_stack.h"
#include "game/game_state.h"
#include "game/display_text_internal.h"
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

/* ---- callroutine yield requests (GAME_MODE_ACTIONSCRIPT_FRAME) -----------
 *
 * Transient request channel: a callroutine that needs a child modal context
 * records a request here; the interpreter loops abort and the frame position
 * is parked in GAME_MODE_ACTIONSCRIPT_FRAME. The request is set and consumed
 * within one frame's work — it never crosses a yield, so it is deliberately
 * NOT serialized (the parked resume state on the mode stack is). */
typedef struct {
    uint8_t  child_kind;       /* AsChildKind; AS_CHILD_NONE = no request */
    uint8_t  epilogue;         /* AsEpilogue */
    int16_t  script_offset;    /* filled at the abort point (ems_run) */
    int16_t  entity_offset;    /* filled at the abort point (phase-1 loops) */
    uint16_t pc;               /* filled at the abort point (ems_run) */
    uint32_t text_addr;
    uint8_t  mf_step, mf_mosaic_bgs;
    uint16_t mf_delay;
    uint16_t fo_id, fo_saved_tick_hi;
    uint32_t fo_script_size;
} AsYieldRequest;

static AsYieldRequest as_req;  /* .child_kind == AS_CHILD_NONE when idle */

void actionscript_request_text(uint32_t text_addr) {
    as_req.child_kind = AS_CHILD_TEXT;
    as_req.epilogue = AS_EPI_TEXT_FLAG;
    as_req.text_addr = text_addr;
}

void actionscript_request_mosaic_fade(uint8_t step, uint16_t delay,
                                      uint8_t mosaic_bgs) {
    as_req.child_kind = AS_CHILD_MOSAIC;
    as_req.epilogue = AS_EPI_NONE;
    as_req.mf_step = step;
    as_req.mf_delay = delay;
    as_req.mf_mosaic_bgs = mosaic_bgs;
}

void actionscript_request_flyover(uint16_t id, uint16_t saved_ent23_tick_hi,
                                  uint32_t script_size) {
    as_req.child_kind = AS_CHILD_FLYOVER;
    as_req.epilogue = AS_EPI_NONE;
    as_req.fo_id = id;
    as_req.fo_saved_tick_hi = saved_ent23_tick_hi;
    as_req.fo_script_size = script_size;
}

void actionscript_request_pp_recovery(void) {
    as_req.child_kind = AS_CHILD_PP_RECOVERY;
    as_req.epilogue = AS_EPI_NONE;
}

/* Move a completed request into the mode state and clear the channel. */
static void as_state_from_request(ActionscriptFrameState *st) {
    st->phase            = ASF_PUSH;
    st->child_kind       = as_req.child_kind;
    st->epilogue         = as_req.epilogue;
    st->script_offset    = as_req.script_offset;
    st->entity_offset    = as_req.entity_offset;
    st->pc               = as_req.pc;
    st->text_addr        = as_req.text_addr;
    st->mf_step          = as_req.mf_step;
    st->mf_mosaic_bgs    = as_req.mf_mosaic_bgs;
    st->mf_delay         = as_req.mf_delay;
    st->fo_id            = as_req.fo_id;
    st->fo_saved_tick_hi = as_req.fo_saved_tick_hi;
    st->fo_script_size   = as_req.fo_script_size;
    memset(&as_req, 0, sizeof(as_req));
}

/* Interpreter sub-step result: EMS_YIELD = a callroutine requested a child
 * mode; the frame must be parked and finished after the child pops. */
typedef enum { EMS_DONE = 0, EMS_YIELD } EmsResult;

/*
 * EXECUTE_MOVEMENT_SCRIPT (C09506)
 *
 * The bytecode VM loop:
 *   1. If sleep_frames > 0, decrement and return. (ems_step)
 *   2. Otherwise, fetch opcodes and dispatch until sleep_frames becomes non-zero.
 *   3. Save PC, decrement sleep_frames, return.
 *
 * Opcodes < 0x70 are dispatched through the main opcode table.
 * Opcodes >= 0x70 are "extended" opcodes where:
 *   bits 0-3 = sleep frame count
 *   bits 4-6 = extended handler index
 *   Extended handler maps: 0→0x3B, 1→0x3C, ..., 6→0x42 (SET_ANIMATION..CALLROUTINE)
 *
 * `pc` stays a local across the loop (and, via ActionscriptFrameState.pc,
 * across a child-mode yield) exactly like the blocking form: scripts.pc[] is
 * only written at loop exit, so a child-side rewrite of scripts.pc[] is
 * overwritten by the continuation, matching the original's C-local behavior.
 * `resuming` re-enters after the interrupted opcode: the post-dispatch sleep
 * check runs first (an extended opcode's sleep count set before the dispatch
 * still applies), then the loop continues at the next opcode.
 */
static EmsResult ems_run(int16_t script_offset, uint16_t pc, bool resuming) {
    int iterations = 0;

    if (resuming && scripts.sleep_frames[script_offset] != 0)
        goto store_and_sleep;

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

        if (as_req.child_kind != AS_CHILD_NONE) {
            /* A callroutine requested a child mode: park the continuation
             * point and abort the frame. scripts.pc[] is NOT written here
             * (see the function comment). */
            as_req.script_offset = script_offset;
            as_req.pc = pc;
            return EMS_YIELD;
        }

        if (scripts.sleep_frames[script_offset] != 0)
            break;

        if (++iterations > 50000) {
            LOG_WARN("SCRIPT HANG: entity=%d script=%d pc=%u bank=%u opcode=0x%02X after %d iters\n",
                    ert.current_entity_offset, script_offset, pc, bank, opcode, iterations);
            scripts.sleep_frames[script_offset] = 1;
            break;
        }
    }

store_and_sleep:
    scripts.pc[script_offset] = pc;
    scripts.sleep_frames[script_offset]--;
    return EMS_DONE;
}

/* One frame of one script: sleep countdown or run the VM loop. */
static EmsResult ems_step(int16_t script_offset) {
    if (scripts.sleep_frames[script_offset] != 0) {
        scripts.sleep_frames[script_offset]--;
        return EMS_DONE;
    }
    return ems_run(script_offset, scripts.pc[script_offset], false);
}

void execute_movement_script(int16_t script_offset) {
    ems_step(script_offset);
}

static void entity_tick(int16_t entity_offset);

/*
 * RUN_ENTITY_SCRIPTS_AND_TICK (C094D0)
 *
 * For a single entity: execute all its scripts, then call tick callback.
 */
static EmsResult run_entity_scripts_and_tick(int16_t entity_offset) {
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
            if (ems_step(script_idx) == EMS_YIELD) {
                as_req.entity_offset = entity_offset;
                return EMS_YIELD;
            }
            script_idx = ert.actionscript_current_script;
        }
    }

    entity_tick(entity_offset);
    return EMS_DONE;
}

/* Tick callback (if not disabled: bit 15 clear in tick_callback_hi).
 * Port of RUN_ENTITY_SCRIPTS_AND_TICK's JUMP_TO_LOADED_MOVEMENT_PTR call.
 * The tick callback runs every frame after all scripts for this entity.
 * Tick callbacks never request a child mode (dispatch_tick_callback's
 * handlers are yield-free, except update_overworld_frame's documented
 * one-frame sector-music wait, which stays inline-blocking — deferred
 * sequential-N-frame-helper class). */
static void entity_tick(int16_t entity_offset) {
    if (!(entities.tick_callback_hi[entity_offset] & 0x8000)) {
        uint32_t tick_addr = ((uint32_t)(entities.tick_callback_hi[entity_offset] & 0xFF) << 16)
                           | entities.tick_callback_lo[entity_offset];
        if (tick_addr != 0) {
            dispatch_tick_callback(tick_addr, entity_offset);
        }
    }
}

/* Phase 1 (scripts + ticks) from entity `ent` onward. The iteration cursor is
 * the serialized global ert.next_active_entity, deliberately re-read after each
 * entity exactly like the original — entity frees during a script (or during a
 * child mode) retarget it. */
static EmsResult as_run_phase1_from(int16_t ent) {
    while (ent >= 0) {
        ert.current_entity_offset = ent;
        ert.current_entity_slot = ent;
        ert.next_active_entity = entities.next_entity[ent];
        if (run_entity_scripts_and_tick(ent) == EMS_YIELD)
            return EMS_YIELD;
        ent = ert.next_active_entity;
    }
    return EMS_DONE;
}

/* Phases 2 + 3: movement/screen-pos callbacks, then the draw list. Ends the
 * frame (clears the reentrancy guard). */
static void as_frame_tail(void) {
    /* Phase 2: Movement + screen position callbacks */
    int16_t ent = entities.first_entity;
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

/* Continue the interrupted entity after a child mode popped: finish the
 * suspended script (from the parked pc), the rest of its script chain, then
 * its tick callback. The chain's bit-14 gate is NOT re-checked — the blocking
 * loop tested it once before the chain, and the suspended chain is past it. */
static EmsResult as_continue_entity(const ActionscriptFrameState *st) {
    if (ems_run(st->script_offset, st->pc, true) == EMS_YIELD) {
        as_req.entity_offset = st->entity_offset;
        return EMS_YIELD;
    }

    int16_t script_idx = ert.actionscript_current_script;
    while (script_idx >= 0) {
        ert.current_script_offset = script_idx;
        ert.current_script_slot = script_idx;
        ert.actionscript_current_script = scripts.next_script[script_idx];
        if (ems_step(script_idx) == EMS_YIELD) {
            as_req.entity_offset = st->entity_offset;
            return EMS_YIELD;
        }
        script_idx = ert.actionscript_current_script;
    }

    entity_tick(st->entity_offset);
    return EMS_DONE;
}

/* GAME_MODE_ACTIONSCRIPT_FRAME step. ASF_PUSH pushes the requested child;
 * ASF_RESUME runs the callroutine's post-child epilogue at its original
 * sequence point, then finishes the interrupted frame. A resumed script that
 * immediately requests another child loops back to ASF_PUSH within the same
 * step (no extra yield beyond the push boundary itself). */
StepResult mode_step_actionscript_frame(ModeState *ms) {
    ActionscriptFrameState *st = &ms->actionscript_frame;

    for (;;) {
        switch (st->phase) {
        case ASF_PUSH: {
            static ModeState init;  /* outlives this dispatch (mode_push copies it) */
            memset(&init, 0, sizeof(init));
            st->phase = ASF_RESUME;
            switch (st->child_kind) {
            case AS_CHILD_TEXT:
                if (!dt_make_child_init(&init, st->text_addr)) {
                    /* Unresolvable text address: warn and skip the child,
                     * matching display_text_from_addr(); the epilogue +
                     * frame continuation run in this same step. */
                    LOG_WARN("WARNING: resolve_text_addr(0x%06X) returned NULL\n",
                             st->text_addr);
                    break;
                }
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &init);
            case AS_CHILD_MOSAIC:
                init.mosaic_fade.kind       = MF_OUT;
                init.mosaic_fade.step       = st->mf_step;
                init.mosaic_fade.delay      = st->mf_delay;
                init.mosaic_fade.mosaic_bgs = st->mf_mosaic_bgs;
                init.mosaic_fade.final_hdma = 1;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_MOSAIC_FADE, &init);
            case AS_CHILD_FLYOVER:
                init.flyover.kind                = FO_SCRIPT;
                init.flyover.phase               = FOP_S_PARSE;
                init.flyover.id                  = st->fo_id;
                init.flyover.pos                 = 0;
                init.flyover.script_size         = st->fo_script_size;
                init.flyover.saved_ent23_tick_hi = st->fo_saved_tick_hi;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_FLYOVER, &init);
            case AS_CHILD_PP_RECOVERY:
                return STEP_RESULT_PUSH_INIT(GAME_MODE_PP_RECOVERY_FLASH, &init);
            default:
                break;
            }
            break;  /* no child pushed: fall through to ASF_RESUME */
        }

        case ASF_RESUME: {
            /* Post-child epilogue at the callroutine's original sequence
             * point (blocking order: child finishes, then the epilogue,
             * then the script continues). */
            if (st->epilogue == AS_EPI_TEXT_FLAG)
                event_flag_set(2);

            EmsResult r = as_continue_entity(st);
            if (r == EMS_DONE)
                r = as_run_phase1_from(ert.next_active_entity);
            if (r == EMS_YIELD) {
                /* Another child requested: refill and push it this step. */
                as_state_from_request(st);
                continue;
            }
            as_frame_tail();
            return STEP_RESULT_POP(0);
        }

        default:
            LOG_WARN("WARNING: actionscript_frame bad phase %d\n", st->phase);
            as_frame_tail();
            return STEP_RESULT_POP(0);
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
 *
 * If a script callroutine requests a child mode (text box, mosaic fade,
 * flyover), the frame is parked and finished as GAME_MODE_ACTIONSCRIPT_FRAME.
 * Bridge era: a local pump drives it (this function's callers are still
 * blocking drivers); at the Phase-D flip the converted overworld render step
 * STEP_PUSHes it instead. ert.disable_actionscript stays 1 across the child —
 * the blocking form held the same guard across its nested pump, so script
 * execution stays frozen during script-triggered modal contexts either way.
 */
void run_actionscript_frame(void) {
    if (ert.disable_actionscript)
        return;

    ert.disable_actionscript = 1;

    if (as_run_phase1_from(entities.first_entity) == EMS_YIELD) {
        ModeState init = {0};
        as_state_from_request(&init.actionscript_frame);
        pump_mode(GAME_MODE_ACTIONSCRIPT_FRAME, &init);
        return;
    }

    as_frame_tail();
}
