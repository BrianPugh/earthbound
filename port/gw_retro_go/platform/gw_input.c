/*
 * Game & Watch input platform stub.
 *
 * Real implementation lives in the firmware repo under
 * Core/Src/porting/earthbound/. It reads the joystick state from
 * odroid_input_read_gamepad() and translates physical Game & Watch buttons
 * to SNES PAD_* bits.
 *
 * Two console form factors exist (detected at runtime via
 * get_ofw_is_mario()):
 *
 *   Mario unit  (6 buttons): UP/DOWN/LEFT/RIGHT + A + B
 *                            no X/Y/L/R/SELECT/START — chord with GAME/TIME
 *
 *   Zelda unit (12 buttons): UP/DOWN/LEFT/RIGHT + A + B + X + Y +
 *                            START + SELECT + GAME (mode) + TIME (pause)
 *
 * PAD_CONFIRM, PAD_CANCEL, etc. handle button-group abstraction in pad.h
 * so the Mario unit only loses access to PAD_X (town map) and PAD_Y
 * (unused) in normal gameplay.
 */

#include "platform/platform.h"

/* Global mode flags — fixed on G&W (no command line) */
bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;
static bool quit_requested;

bool platform_input_init(void) {
    pad_state = 0;
    pad_prev = 0;
    aux_state = 0;
    quit_requested = false;
    return true;
}

void platform_input_shutdown(void) {
    /* Nothing to clean up. */
}

void platform_input_poll(void) {
    /*
     * TODO: Read joystick via odroid_input_read_gamepad(), then translate
     * physical buttons to PAD_* bits according to the active mapping
     * profile (Mario vs Zelda form factor — detect via get_ofw_is_mario()).
     *
     * Reserved modifier keys:
     *   GAME  (ODROID_INPUT_START)  — pause menu / brightness chord
     *   TIME  (ODROID_INPUT_VOLUME) — volume / save-state chord
     * Do not bind these directly to PAD_* — they multiplex with other
     * buttons via common_emu_input_loop().
     */
    pad_prev = pad_state;
    /* pad_state = translate(odroid_gamepad_state); */
}

uint16_t platform_input_get_pad(void) {
    return pad_state;
}

uint16_t platform_input_get_pad_new(void) {
    return pad_state & ~pad_prev;
}

uint16_t platform_input_get_aux(void) {
    return aux_state;
}

bool platform_input_quit_requested(void) {
    return quit_requested;
}

void platform_request_quit(void) {
    /*
     * TODO: trigger a clean return to the launcher. The retro-go shell
     * normally takes over via the GAME + B "exit" chord; this is the
     * programmatic equivalent (e.g. from an in-game menu).
     */
    quit_requested = true;
}
