/*
 * SNES video platform implementation.
 *
 * On SNES, the PPU renders directly from VRAM/OAM/CGRAM.  The game logic
 * writes to the ppu struct mirrors (ppu.vram[], ppu.cgram[], ppu.oam[]),
 * and this layer DMAs dirty regions to hardware during VBlank.
 *
 * The software renderer (ppu_render_frame) is not used on SNES hardware.
 */

#include "platform/platform.h"

bool platform_video_init(void) {
    /*
     * TODO: Configure SNES PPU registers:
     *   INIDISP ($2100) = 0x80 (force blank during setup)
     *   BGMODE  ($2105) = mode 1
     *   OBSEL   ($2101) = sprite tile base / size
     *   BG1SC-BG4SC ($2107-$210A) = tilemap addresses
     *   BG12NBA/BG34NBA ($210B-$210C) = character data addresses
     *   TM ($212C) = main screen designation
     *   INIDISP ($2100) = 0x0F (full brightness)
     */
    return true;
}

void platform_video_shutdown(void) {
    /* No cleanup needed on SNES hardware. */
}

void platform_video_begin_frame(void) {
    /* No-op on SNES — PPU renders directly from VRAM. */
}

void platform_video_send_scanline(int y, const pixel_t *pixels) {
    /* Not used on SNES — PPU renders directly from VRAM. */
    (void)y;
    (void)pixels;
}

pixel_t *platform_video_get_framebuffer(void) {
    return NULL;  /* No software framebuffer on SNES. */
}

void platform_video_end_frame(void) {
    /*
     * TODO: DMA the software mirrors to hardware during VBlank.
     *   - Track dirty regions of ppu.vram[], ppu.cgram[], ppu.oam[]
     *   - During VBlank (after NMI fires):
     *     - DMA dirty VRAM regions via channels $4300-$437F to $2118/$2119
     *     - DMA OAM via $2104
     *     - DMA CGRAM via $2122
     *     - Update scroll registers ($210D-$2114)
     *     - Update HDMA tables if needed
     */
}

void platform_video_set_vsync(bool enabled) {
    /* No-op on SNES — display is always VSync'd to the CRT. */
    (void)enabled;
}
