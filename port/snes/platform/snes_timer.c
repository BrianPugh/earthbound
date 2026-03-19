/*
 * SNES timer platform implementation.
 *
 * On SNES, frame timing is hardware-locked to VBlank (60Hz NTSC / 50Hz PAL).
 * There is no high-resolution timer; we use a frame counter instead.
 */

#include "platform/platform.h"

static uint32_t frame_counter;

bool platform_timer_init(void) {
    frame_counter = 0;
    return true;
}

void platform_timer_shutdown(void) {
    /* Nothing to clean up. */
}

void platform_timer_frame_start(void) {
    /* Nothing to do -- frame timing is VBlank-driven. */
}

void platform_timer_frame_end(void) {
    /*
     * TODO: Wait for VBlank NMI.
     *
     * In 65816 assembly this is:
     *   WAI  ; halt CPU until next interrupt (NMI)
     *
     * The NMI handler sets a flag; we spin until it's set:
     *   while (!vblank_flag) { __asm("WAI"); }
     *   vblank_flag = 0;
     *
     * This is where DMA transfers to VRAM/OAM/CGRAM happen
     * (called from or coordinated with platform_video_present).
     */
    frame_counter++;
}

uint64_t platform_timer_ticks(void) {
    return frame_counter;
}

uint64_t platform_timer_ticks_per_sec(void) {
    return 60;  /* NTSC frame rate */
}

uint32_t platform_timer_get_fps_tenths(void) {
    return 600;  /* Always 60.0 FPS -- hardware-locked */
}
