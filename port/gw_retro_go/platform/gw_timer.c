/*
 * Game & Watch timer platform stub.
 *
 * Real implementation lives in the firmware repo under
 * Core/Src/porting/earthbound/. Frame pacing piggybacks on the launcher's
 * common_emu_frame_loop() machinery, which already handles 30/60 fps mode
 * switching, watchdog refresh, and vblank synchronization.
 *
 * Ticks are exposed in microseconds via HAL_GetTick()-based math, which is
 * enough resolution for the IIR FPS filter and platform_timer_sleep_until.
 */

#include "platform/platform.h"

static uint64_t frame_counter;
static uint32_t fps_tenths = 600;  /* assume locked 60.0 fps until measured */

bool platform_timer_init(void) {
    frame_counter = 0;
    return true;
}

void platform_timer_shutdown(void) {
    /* Nothing to clean up. */
}

void platform_timer_frame_start(void) {
    /* TODO: capture HAL_GetTick() into a frame-start timestamp. */
}

void platform_timer_frame_end(void) {
    /*
     * TODO: refresh the watchdog (wdog_refresh()), wait for the LCD
     * vblank (lcd_wait_for_vblank()), and yield to common_emu_sound_sync()
     * so audio DMA can keep up.
     */
    frame_counter++;
}

void platform_timer_update_fps(void) {
    /*
     * TODO: maintain an exponential moving average of the last frame's
     * duration; convert to tenths-of-fps and store in fps_tenths. The
     * launcher already exposes an FPS counter — feed that the same value
     * to keep both overlays consistent.
     */
}

void platform_timer_sleep_until(uint64_t deadline) {
    /*
     * TODO: busy-wait or HAL_Delay until platform_timer_ticks() reaches
     * deadline. Refresh the watchdog inside the wait loop. On embedded
     * targets that lack a "sleep until N microseconds" primitive, polling
     * HAL_GetTick() (1 ms resolution) is acceptable.
     */
    (void)deadline;
}

uint64_t platform_timer_ticks(void) {
    /* TODO: return microsecond-resolution monotonic ticks. */
    return frame_counter;
}

uint64_t platform_timer_ticks_per_sec(void) {
    return 1000000;  /* microsecond resolution */
}

uint32_t platform_timer_get_fps_tenths(void) {
    return fps_tenths;
}
