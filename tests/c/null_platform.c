/*
 * No-op platform.h backend for host unit tests. The tests call game routines
 * directly and never run a frame, but libgame references the whole platform
 * interface, so every symbol it pulls in needs a definition.
 */
#include "platform/platform.h"

bool platform_headless = true;
bool platform_skip_intro = false;
int platform_max_frames = 0;

void platform_video_begin_frame(void) {}
void platform_video_send_scanline(int y, const pixel_t *pixels) {
    (void)y;
    (void)pixels;
}
pixel_t *platform_video_get_framebuffer(void) { return NULL; }
void platform_video_end_frame(void) {}
void platform_video_set_vsync(bool enabled) { (void)enabled; }

void platform_input_poll(void) {}
uint16_t platform_input_get_pad(void) { return 0; }
uint16_t platform_input_get_pad_new(void) { return 0; }
uint16_t platform_input_get_aux(void) { return 0; }
bool platform_input_quit_requested(void) { return false; }
void platform_request_quit(void) {}

void platform_audio_lock(void) {}
void platform_audio_unlock(void) {}

size_t platform_save_read(void *dst, size_t offset, size_t size) {
    (void)dst;
    (void)offset;
    (void)size;
    return 0;
}
bool platform_save_write(const void *src, size_t offset, size_t size) {
    (void)src;
    (void)offset;
    (void)size;
    return false;
}

bool platform_savestate_begin(int slot) {
    (void)slot;
    return false;
}
bool platform_savestate_write(int slot, size_t offset, const void *src, size_t size) {
    (void)slot;
    (void)offset;
    (void)src;
    (void)size;
    return false;
}
bool platform_savestate_commit(int slot) {
    (void)slot;
    return false;
}
size_t platform_savestate_read(int slot, size_t offset, void *dst, size_t size) {
    (void)slot;
    (void)offset;
    (void)dst;
    (void)size;
    return 0;
}
void *platform_savestate_scratch(size_t *out_bytes) {
    if (out_bytes)
        *out_bytes = 0;
    return NULL;
}

void platform_debug_dump_ppu(const pixel_t *framebuffer) { (void)framebuffer; }
void platform_debug_dump_vram_image(void) {}

void platform_timer_frame_start(void) {}
void platform_timer_frame_end(void) {}
void platform_timer_update_fps(void) {}
void platform_timer_sleep_until(uint64_t deadline) { (void)deadline; }
uint64_t platform_timer_ticks(void) { return 0; }
uint64_t platform_timer_ticks_per_sec(void) { return 1; }
uint32_t platform_timer_get_fps_tenths(void) { return 0; }
