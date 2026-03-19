#ifndef INTRO_ATTRACT_MODE_H
#define INTRO_ATTRACT_MODE_H

#include "core/types.h"

/* Run one attract mode scene.
 * Port of RUN_ATTRACT_MODE (asm/misc/run_attract_mode.asm).
 * scene_index: selects which text/location to show.
 * Returns 1 if user pressed a button (proceed to file select),
 * 0 if the scene completed normally (continue attract loop). */
uint16_t run_attract_mode(uint16_t scene_index);

#endif /* INTRO_ATTRACT_MODE_H */
