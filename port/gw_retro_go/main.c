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

/*
 * Optional host hook, called once per iteration at the root-loop boundary —
 * the port-side counterpart to the engine's host_root_boundary(), and the only
 * quiescent point where deferred / torn-unsafe host work is safe (no mode-step
 * or blocking helper is mid-execution). Declared weak so ports that don't need
 * it (desktop) link without providing one. May not return.
 *
 * Current use: STANDBY power-down. An embedded port (Game & Watch retro-go
 * firmware) requests a torn-safe savestate from its per-frame
 * platform_input_poll() via host_request_capture() when POWER is pressed —
 * issuing it from there lets in-flight blocking helpers (battle/text/fade, which
 * yield through wait_for_vblank deep in the stack) unwind to this boundary,
 * where host_root_boundary() writes the slot. This hook then powers the device
 * down once that capture has committed.
 */
void platform_root_boundary(void) __attribute__((weak));

int main(void) {
    platform_video_init();
    platform_input_init();
    platform_timer_init();
    platform_audio_init();

    game_init();

    /*
     * game_logic_entry() runs the main game loop. It calls wait_for_vblank()
     * which calls host_process_frame() -> ppu_render_frame() and the
     * platform_video/input/audio/timer hooks each frame, then runs
     * host_root_boundary() — the safe point for deferred host actions.
     *
     * It returns on Game Over → Continue; loop to restart.
     */
    for (;;) {
        game_logic_entry();
        if (platform_root_boundary)
            platform_root_boundary();   /* deferred host work; may not return */
    }

    /* unreachable */
    return 0;
}
