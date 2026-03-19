#include "pico/stdlib.h"

#include "platform/platform.h"
#include "game_main.h"

/* Pico-specific: pump audio samples into PWM ring buffer each frame */
extern void platform_audio_pump(void);

int main(void) {
    /* Clock is configured at boot by the SDK via SYS_CLK_MHZ=250
     * (set in CMakeLists.txt).  The SDK raises the regulator voltage
     * automatically and keeps clk_peri = clk_sys = 250 MHz, so
     * hardware SPI can reach 62.5 MHz (250/4) without manual clock hacks. */

    platform_video_init();
    platform_input_init();
    platform_timer_init();
    platform_audio_init();
#ifdef ENABLE_DUAL_CORE_PPU
    extern void platform_worker_init(void);
    platform_worker_init();
#endif
    game_init();

    platform_timer_frame_start();

    for (;;) {
        game_logic_entry();
        platform_audio_pump();
    }

    return 0;
}
