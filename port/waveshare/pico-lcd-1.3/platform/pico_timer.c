#include "platform/platform.h"
#include "pico/stdlib.h"
#include "hardware/timer.h"

static uint64_t frame_start_us;

#define IIR_SHIFT 4
static uint32_t fps_tenths_acc;

bool platform_timer_init(void) {
    fps_tenths_acc = (uint32_t)TARGET_FPS * 10 << IIR_SHIFT;
    return true;
}

void platform_timer_shutdown(void) {}

uint64_t platform_timer_ticks(void) {
    return time_us_64();
}

uint64_t platform_timer_ticks_per_sec(void) {
    return 1000000;  /* microseconds */
}

void platform_timer_frame_start(void) {
    frame_start_us = time_us_64();
}

void platform_timer_frame_end(void) {
    if (platform_headless)
        return;

    uint64_t target_us = 1000000 / TARGET_FPS;
    uint64_t deadline = frame_start_us + target_us;
    platform_timer_sleep_until(deadline);

    platform_timer_update_fps();
}

void platform_timer_update_fps(void) {
    uint64_t elapsed = time_us_64() - frame_start_us;
    if (elapsed > 0) {
        uint32_t instant_fps_tenths = (uint32_t)(10000000ULL / elapsed);
        fps_tenths_acc = fps_tenths_acc - (fps_tenths_acc >> IIR_SHIFT) + instant_fps_tenths;
    }
}

void platform_timer_sleep_until(uint64_t deadline) {
    uint64_t now = time_us_64();
    if ((int64_t)(deadline - now) > 0)
        sleep_us(deadline - now);
}

uint32_t platform_timer_get_fps_tenths(void) {
    return fps_tenths_acc >> IIR_SHIFT;
}
