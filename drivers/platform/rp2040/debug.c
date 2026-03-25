/* Generic RP2040 debug stubs — no filesystem on bare-metal. */

#include "platform/platform.h"

void platform_debug_dump_ppu(const pixel_t *framebuffer) { (void)framebuffer; }
void platform_debug_dump_vram_image(void) {}
void platform_debug_dump_state(void) {}
