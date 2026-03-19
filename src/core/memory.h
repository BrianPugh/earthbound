#ifndef CORE_MEMORY_H
#define CORE_MEMORY_H

#include "core/types.h"

/* Game RAM - mirrors key BSS sections from the SNES memory map */

typedef struct {
    /* Joypad state — port of PAD_* variables from ram.asm.
     * See UPDATE_JOYPAD_STATE (asm/overworld/update_joypad_state.asm).
     * pad1_held = PAD_STATE: currently held buttons (physical state)
     * pad1_pressed = PAD_PRESS: edge-detected newly pressed buttons
     * pad1_autorepeat = PAD_HELD: auto-repeat (pressed + repeated after timer) */
    uint16_t pad1_raw;        /* raw joypad 1 state (before masking) */
    uint16_t pad1_pressed;    /* newly pressed buttons (PAD_PRESS) */
    uint16_t pad1_held;       /* currently held buttons (PAD_STATE) */
    uint16_t pad1_autorepeat; /* auto-repeat buttons (PAD_HELD) */
    uint16_t pad_timer;       /* auto-repeat timer (was file-static) */

    /* NMI/frame counter */
    uint16_t nmi_count;
    uint16_t frame_counter;

    /* Play timer — 32-bit frame counter incremented every NMI.
     * Port of TIMER at $7E00A7 (asm/bankconfig/common/ram.asm).
     * Copied to game_state.timer on save, restored on load. */
    uint32_t play_timer;

    /* Screen state */
    uint8_t force_blank;       /* non-zero = screen off */
    uint8_t screen_brightness; /* 0-15 */
    uint8_t mosaic_register;

    /* Various game state flags */
    uint16_t game_mode;
    uint16_t game_submode;

    /* Wait-for-vblank flag */
    uint8_t wait_for_nmi;
} CoreState;
extern CoreState core;

/* Initialize game RAM to zero */
void memory_init(void);

/* Update joypad state from platform layer */
void memory_update_joypad(uint16_t raw_pad);

#endif /* CORE_MEMORY_H */
