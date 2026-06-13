#ifndef INTRO_ATTRACT_MODE_H
#define INTRO_ATTRACT_MODE_H

#include "core/types.h"

/* One-shot setup for one attract mode scene (the yield-free front half of
 * RUN_ATTRACT_MODE, asm/misc/run_attract_mode.asm): inits the entity/overworld
 * state, oval window, and palettes, and selects the scene's bytecode script.
 * The init_intro parent calls this, then STEP_PUSHes GAME_MODE_ATTRACT, whose
 * AT_SCRIPT phase runs the scene script and the post-script loops. The mode pops
 * 1 if the user pressed a button (proceed to file select), 0 if the scene
 * completed normally (continue the attract loop).
 * scene_index: selects which text/location to show. */
void run_attract_mode_prepare(uint16_t scene_index);

#endif /* INTRO_ATTRACT_MODE_H */
