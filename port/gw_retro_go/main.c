/*
 * Game & Watch (Retro-Go SD) port entry point.
 *
 * This is scaffolding — it will not link standalone. The host firmware
 * (game-and-watch-retro-go-sd) renames this main() to earthbound_main()
 * via objcopy --redefine-syms and invokes it from app_main_earthbound()
 * after caching the rodata blob and patching the rodata pointer table.
 *
 * The actual platform_* implementations live in the firmware repo under
 * Core/Src/porting/earthbound/. The stubs in platform/ here exist so that
 * the platform.h interface is statically satisfied and embedded-only
 * cleanups shared with port/snes/ have a place to be tracked.
 */

#include "platform/platform.h"
#include "game_main.h"

int main(void) {
    platform_video_init();
    platform_input_init();
    platform_timer_init();
    platform_audio_init();

    game_init();

    /*
     * game_logic_entry() runs the main game loop. It calls wait_for_vblank()
     * which calls host_process_frame() -> ppu_render_frame() and the
     * platform_video/input/audio/timer hooks each frame.
     *
     * It returns on Game Over → Continue; loop to restart.
     */
    for (;;) {
        game_logic_entry();
    }

    /* unreachable */
    return 0;
}
