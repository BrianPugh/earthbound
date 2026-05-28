/*
 * Game & Watch debug platform stub.
 *
 * Real implementation lives in the firmware repo under
 * Core/Src/porting/earthbound/ — but on this target the debug dumps
 * (PPU snapshots, VRAM-as-BMP, full state dumps) are no-ops. There's
 * no filesystem path on the device to write to, and the SD card driver
 * is busy serving asset loads.
 *
 * If a developer ever wants on-device dumps, the launcher's
 * make_dump_screenshot target (Makefile.common) and the SD card's
 * /debug/ directory are the natural integration points.
 */

#include "platform/platform.h"

void platform_debug_dump_ppu(const pixel_t *framebuffer) {
    (void)framebuffer;
}

void platform_debug_dump_vram_image(void) {
}

void platform_debug_dump_state(void) {
}
