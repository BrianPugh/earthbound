/*
 * Game & Watch video platform stub.
 *
 * Real implementation lives in the firmware repo under
 * Core/Src/porting/earthbound/. It targets a 320x240 RGB565 LCD with the
 * 256-px-wide SNES viewport centered (32 px black bars left/right, 8 px
 * top/bottom for a 224-line frame).
 *
 * The PPU renders one scanline at a time into the framebuffer returned by
 * lcd_get_active_buffer(); platform_video_end_frame() swaps buffers via
 * lcd_swap().
 */

#include "platform/platform.h"

bool platform_video_init(void) {
    /*
     * TODO: nothing to do here — the launcher has already configured the
     * LCD peripheral and DMA before app_main_earthbound is entered.
     */
    return true;
}

void platform_video_shutdown(void) {
    /* No cleanup needed; RAM_EMU is torn down wholesale on game exit. */
}

void platform_video_begin_frame(void) {
    /* No-op — scanlines write directly into the active LCD buffer. */
}

void platform_video_send_scanline(int y, const pixel_t *pixels) {
    /*
     * TODO: copy pixels into lcd_get_active_buffer() at row (y + Y_OFFSET),
     * starting at column X_OFFSET. Handle the three retro-go scaling modes
     * (OFF / FIT / FULL) by adjusting the row stride and X_OFFSET.
     */
    (void)y;
    (void)pixels;
}

pixel_t *platform_video_get_framebuffer(void) {
    /*
     * Return NULL — this port streams scanlines directly. Returning the
     * active LCD buffer would allow the FPS overlay to draw on top, but
     * is not needed for first boot.
     */
    return NULL;
}

void platform_video_end_frame(void) {
    /*
     * TODO: lcd_swap() — swap the front and back buffers and arm DMA on
     * the new front buffer. Wait for VBlank elsewhere (in timer_frame_end).
     */
}

void platform_video_set_vsync(bool enabled) {
    /* No-op — LCD refresh is hardware-locked to 60 Hz. */
    (void)enabled;
}
