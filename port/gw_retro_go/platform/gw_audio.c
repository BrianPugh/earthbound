/*
 * Game & Watch audio platform stub.
 *
 * Real implementation lives in the firmware repo under
 * Core/Src/porting/earthbound/. It uses the launcher's SAI peripheral
 * (16 kHz, ping-pong DMA buffers) and pumps SPC700 samples through
 * audio_get_active_buffer().
 *
 * Initial builds will set ENABLE_AUDIO=OFF in src/CMakeLists.txt to free
 * ~64 KB of BSS used by the lakesnes SPC700/DSP emulator; with audio off
 * these functions are still defined but called only on init/shutdown and
 * are effectively no-ops.
 */

#include "platform/platform.h"

bool platform_audio_init(void) {
    /*
     * TODO: When ENABLE_AUDIO=ON, start SAI DMA via audio_start_playing()
     * with SAMPLES_PER_FRAME = 534 (matching the Zelda 3/SMW ports' 16 kHz
     * @ 30 fps cadence) and arrange for platform_audio_pump() to be driven
     * from the main loop's frame end.
     *
     * The Pico port uses the same pattern (see port/waveshare/pico-lcd-1.3
     * + drivers/platform/rp2040/audio.c) — copy that single-threaded
     * model rather than the desktop port's audio-callback-thread model.
     */
    return true;
}

void platform_audio_shutdown(void) {
    /* TODO: stop SAI DMA. */
}

void platform_audio_lock(void) {
    /* No-op — G&W is single-threaded; audio is pumped from the main loop. */
}

void platform_audio_unlock(void) {
    /* No-op — see platform_audio_lock(). */
}
