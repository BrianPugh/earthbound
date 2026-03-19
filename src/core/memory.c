#include "core/memory.h"
#include "game/overworld.h"

CoreState core;

/* Auto-repeat timer constants (from ram.asm PAD_TIMER).
 * Initial delay = 20 frames, repeat interval = 3 frames. */
#define PAD_INITIAL_DELAY  0x0014
#define PAD_REPEAT_DELAY   0x0003

void memory_init(void) {
    core.pad1_raw = 0;
    core.pad1_pressed = 0;
    core.pad1_held = 0;
    core.pad1_autorepeat = 0;
    core.pad_timer = 0;
    core.nmi_count = 0;
    core.frame_counter = 0;
    core.play_timer = 0;
    core.force_blank = 1;
    core.screen_brightness = 0;
    core.mosaic_register = 0;
    core.game_mode = 0;
    core.game_submode = 0;
    core.wait_for_nmi = 0;
}

/* Port of UPDATE_JOYPAD_STATE (asm/overworld/update_joypad_state.asm).
 * The assembly processes 2 pads and ORs them in non-debug mode.
 * The C port only supports one pad, so we process pad 1 only.
 *
 * Assembly variables:
 *   PAD_STATE → pad1_held  (physical button state, masked)
 *   PAD_PRESS → pad1_pressed  (newly pressed, edge-detected)
 *   PAD_HELD  → pad1_autorepeat  (auto-repeat: pressed + repeated after timer)
 *   PAD_TIMER → pad_timer (countdown for auto-repeat)
 */
void memory_update_joypad(uint16_t raw_pad) {
    /* Assembly: AND #$FFF0 masks off unused low 4 bits */
    uint16_t current = raw_pad & 0xFFF0;

    /* Edge-detect: newly pressed = current & ~previous */
    core.pad1_pressed = current & ~core.pad1_held;

    if (current != core.pad1_held) {
        /* State changed: reset auto-repeat timer */
        core.pad1_autorepeat = core.pad1_pressed;
        core.pad_timer = PAD_INITIAL_DELAY;
    } else {
        /* Same state held */
        if (core.pad_timer != 0) {
            /* Timer counting down — no auto-repeat yet */
            core.pad_timer--;
            core.pad1_autorepeat = 0;
        } else {
            /* Timer expired — fire auto-repeat with current state */
            core.pad1_autorepeat = current;
            core.pad_timer = PAD_REPEAT_DELAY;
        }
    }

    /* Assembly lines 55-58: increment activity counter on any button press */
    if (core.pad1_pressed) {
        ow.player_has_done_something_this_frame++;
    }

    core.pad1_held = current;
    core.pad1_raw = raw_pad;
}
