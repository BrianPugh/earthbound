/*
 * SNES input platform implementation.
 *
 * Reads auto-joypad registers $4218/$4219 which are already in the same
 * bit format as the PAD_* constants used by the game logic.
 */

#include "platform/platform.h"

/* Global mode flags -- on SNES these are always fixed */
bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static uint16_t pad_state;
static uint16_t pad_prev;

bool platform_input_init(void) {
    /*
     * TODO: Enable auto-joypad read:
     *   NMITIMEN ($4200) |= 0x01
     *
     * The SNES reads joypad state automatically each VBlank and
     * places results in $4218-$421F.
     */
    pad_state = 0;
    pad_prev = 0;
    return true;
}

void platform_input_shutdown(void) {
    /* Nothing to clean up. */
}

void platform_input_poll(void) {
    /*
     * TODO: Read auto-joypad result registers:
     *   pad_prev = pad_state;
     *   while ($4212 & 0x01) {}  // Wait for auto-read to complete
     *   pad_state = *(volatile uint16_t *)0x4218;
     *
     * The register format matches PAD_* constants directly:
     *   Bit 15: B      Bit 14: Y      Bit 13: Select  Bit 12: Start
     *   Bit 11: Up     Bit 10: Down   Bit 9:  Left    Bit 8:  Right
     *   Bit 7:  A      Bit 6:  X      Bit 5:  L       Bit 4:  R
     */
    pad_prev = pad_state;
    /* pad_state = *(volatile uint16_t *)0x4218; */
}

uint16_t platform_input_get_pad(void) {
    return pad_state;
}

uint16_t platform_input_get_pad_new(void) {
    return pad_state & ~pad_prev;
}

bool platform_input_quit_requested(void) {
    return false;  /* No quit concept on SNES */
}

void platform_request_quit(void) {
    /* No-op on SNES */
}

uint16_t platform_input_get_aux(void) {
    return 0;  /* No aux buttons on SNES */
}
