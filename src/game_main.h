#ifndef GAME_MAIN_H
#define GAME_MAIN_H

#include "core/types.h"

/* Initialize core game systems (memory, PPU, RNG, game state, door/sprite tables).
 * Call once before game_logic_entry(). */
void game_init(void);

/* Game logic entry point — called from main().
 * Contains intro, initialization, and the main game loop.
 * Returns on game-over Continue (main re-calls for restart). */
void game_logic_entry(void);

/* Host-side per-frame processing (rendering, input, audio, timing).
 * Called by the host main loop after each fiber yield. */
void host_process_frame(void);

/* Wait for one frame (NMI equivalent). Yields the game fiber. */
void wait_for_vblank(void);

/* Wait for N frames or until a button is pressed. Returns true if pressed. */
bool wait_frames_or_button(uint16_t count, uint16_t button_mask);

/* Returns true when turbo/fast-forward mode is active.
 * Toggled by AUX_FAST_FORWARD; used by timer and audio backends. */
bool game_is_fast_forward(void);

#endif /* GAME_MAIN_H */
