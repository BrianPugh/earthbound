#ifndef GAME_FADE_H
#define GAME_FADE_H

#include "core/types.h"

/* Fade directions */
#define FADE_IN  0
#define FADE_OUT 1

typedef struct {
    uint8_t target;
    int8_t  step;
    uint8_t delay;
    uint8_t counter;
    bool    fading;
    bool    updated_this_frame;
} FadeState;
extern FadeState fade_state;

/* Fade the screen brightness from current to target over duration frames.
   brightness is 0-15 (INIDISP value). */
void fade_to_brightness(uint8_t target, uint8_t step, uint8_t delay);

/* Start a fade in (0 -> 15) */
void fade_in(uint8_t step, uint8_t delay);

/* Start a fade out (15 -> 0) */
void fade_out(uint8_t step, uint8_t delay);

/* Process one frame of fading. Returns true while still fading.
 * Has a per-frame guard to prevent double-updates when called from both
 * explicit code and wait_for_vblank (NMI equivalent). */
bool fade_update(void);

/* Reset the per-frame fade guard. Called by wait_for_vblank at start of
 * each frame, matching the assembly NMI handler which updates fade exactly
 * once per vblank. */
void fade_new_frame(void);

/* Is a fade currently in progress? */
bool fade_active(void);

/* WAIT_FOR_FADE_COMPLETE: Port of asm/system/palette/wait_for_fade_complete.asm.
 * Loops until fade finishes, calling oam_clear + update_screen + wait each frame. */
void wait_for_fade_complete(void);

/* FADE_OUT_WITH_MOSAIC — fade out with optional mosaic effect.
 * step: brightness decrement per frame, delay_frames: wait between steps,
 * mosaic_enable: BG enable mask for mosaic (0 = no mosaic). */
void fade_out_with_mosaic(uint16_t step, uint16_t delay_frames, uint16_t mosaic_enable);

/* Force blank on/off */
void set_force_blank(bool blank);

#endif /* GAME_FADE_H */
