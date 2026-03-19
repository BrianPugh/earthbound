/* Default single-core implementation of platform_render_frame().
 *
 * Platforms with multi-core rendering (e.g., RP2040, ESP32) should
 * provide their own implementation and exclude this file from the build,
 * or override via linker precedence (platform .o linked after libgame.a).
 *
 * This default calls begin_frame, ppu_render_frame, end_frame in sequence
 * on the calling core. */

#include "platform/platform.h"
#include "snes/ppu.h"

/* Stamp callback + real send, stored for the wrapper below */
static scanline_stamp_cb_t current_stamp_cb;

static void send_scanline_with_stamp(int y, const pixel_t *pixels) {
    current_stamp_cb(y, (pixel_t *)pixels);
    platform_video_send_scanline(y, pixels);
}

void platform_render_frame(scanline_stamp_cb_t fps_overlay_cb) {
    platform_video_begin_frame();

    if (fps_overlay_cb) {
        current_stamp_cb = fps_overlay_cb;
        ppu_render_frame(send_scanline_with_stamp);
    } else {
        ppu_render_frame(platform_video_send_scanline);
    }

    platform_video_end_frame();
}
