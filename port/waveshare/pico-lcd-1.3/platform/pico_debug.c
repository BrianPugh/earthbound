#include "platform/platform.h"

/* No filesystem on embedded — debug dumps are no-ops */
void platform_debug_dump_ppu(const pixel_t *framebuffer) { (void)framebuffer; }
void platform_debug_dump_vram_image(void) {}
void platform_debug_dump_state(void) {}
