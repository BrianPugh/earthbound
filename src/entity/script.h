/*
 * Event script bytecode interpreter API.
 *
 * Ports of:
 *   EXECUTE_MOVEMENT_SCRIPT     (asm/overworld/execute_movement_script.asm)
 *   RUN_ENTITY_SCRIPTS_AND_TICK (asm/overworld/entity/run_entity_scripts_and_tick.asm)
 *   RUN_ACTIONSCRIPT_FRAME      (asm/overworld/actionscript/run_actionscript_frame.asm)
 */
#ifndef ENTITY_SCRIPT_H
#define ENTITY_SCRIPT_H

#include "core/types.h"

/* Initialize a script's PC from the event script pointer table.
 * Called after allocating a script slot during entity_init. */
void event_script_init(uint16_t script_id, int16_t entity_offset,
                       int16_t script_offset);

/* Execute one frame of a movement script (EXECUTE_MOVEMENT_SCRIPT). */
void execute_movement_script(int16_t script_offset);

/* ---- callroutine yield requests (GAME_MODE_ACTIONSCRIPT_FRAME) -----------
 *
 * A callroutine that needs a child modal context (text box, mosaic fade,
 * flyover) records a request instead of running it inline-blocking. The
 * interpreter loops abort, run_actionscript_frame() parks the frame position
 * in GAME_MODE_ACTIONSCRIPT_FRAME, the requested child mode is pushed, and
 * after it pops the frame finishes from where it stopped. The request is
 * transient (consumed before any yield) — only valid from a callroutine
 * dispatched inside the interpreter. */

/* Display a text box (GAME_MODE_DISPLAY_TEXT child); after it pops,
 * event_flag_set(2) runs at the callroutine's original sequence point. */
void actionscript_request_text(uint32_t text_addr);

/* FADE_OUT_WITH_MOSAIC (GAME_MODE_MOSAIC_FADE child, MF_OUT + final_hdma). */
void actionscript_request_mosaic_fade(uint8_t step, uint16_t delay,
                                      uint8_t mosaic_bgs);

/* PLAY_FLYOVER_SCRIPT (GAME_MODE_FLYOVER child, FO_SCRIPT). The synchronous
 * prologue (entity-23 tick disable, screen init, asset lookup) must already
 * have run — see play_flyover_script_prepare() (flyover.h). */
void actionscript_request_flyover(uint16_t id, uint16_t saved_ent23_tick_hi,
                                  uint32_t script_size);

/* INSTANT_WIN_PP_RECOVERY (GAME_MODE_PP_RECOVERY_FLASH child). The recovery
 * SFX must already have been played at the request point. */
void actionscript_request_pp_recovery(void);

#endif /* ENTITY_SCRIPT_H */
