/*
 * SNES port entry point.
 *
 * This is scaffolding -- it will not compile until a 65816 C compiler
 * toolchain is set up and the src/ library is adapted for SNES constraints
 * (no malloc, no stdio).
 */

#include "platform/platform.h"
#include "game_main.h"

int main(void) {
    platform_video_init();
    platform_input_init();
    platform_timer_init();

    game_init();

    platform_audio_init();

    /*
     * game_logic_entry() contains the main game loop. It calls
     * wait_for_vblank() which calls host_process_frame() directly,
     * handling rendering, input, audio, and timing inline.
     *
     * Returns on game-over Continue; loop to restart.
     */
    for (;;) {
        game_logic_entry();
    }

    /* unreachable */
    return 0;
}
