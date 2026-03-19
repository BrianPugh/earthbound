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

#endif /* ENTITY_SCRIPT_H */
