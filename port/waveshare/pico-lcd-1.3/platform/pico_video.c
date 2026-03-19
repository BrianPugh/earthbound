#include "platform/platform.h"
#include "board.h"
#include "display/st7789/st7789.h"
#include "display/st7789/rp2040/st7789_rp2040.h"
#include "hal/hal.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"

static st7789_t lcd;

#ifndef ENABLE_DUAL_CORE_PPU
/* Double-buffered scanline buffers (BGR565) — single-core only.
 * Dual-core mode has its own per-core buffers in pico_worker.c. */
static uint16_t scanline_buf[2][VIEWPORT_WIDTH];
static int dma_chan = -1;
static int active_buf;
#endif

bool platform_video_init(void) {
    if (platform_headless)
        return true;

    /* Backlight PWM */
    gpio_set_function(PIN_LCD_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_LCD_BL);
    pwm_set_wrap(slice, 255);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(PIN_LCD_BL), 77);
    pwm_set_enabled(slice, true);

    /* Initialize ST7789 display */
    st7789_config_t config = {
        .width = LCD_WIDTH,
        .height = LCD_HEIGHT,
        .rotation = ST7789_ROTATION_90,
    };
    st7789_init(&lcd, &config);

    /* Set up DMA for 16-bit pixel streaming */
    hal_spi_set_format(16);
#ifndef ENABLE_DUAL_CORE_PPU
    dma_chan = st7789_rp2040_claim_dma();
    active_buf = 0;
#endif
    return true;
}

#ifndef ENABLE_DUAL_CORE_PPU
void platform_video_shutdown(void) {
    st7789_rp2040_dma_wait(dma_chan);
    st7789_rp2040_release_dma(dma_chan);
    dma_chan = -1;
}
#else
void platform_video_shutdown(void) {}
#endif

void platform_video_begin_frame(void) {
    if (platform_headless)
        return;
    st7789_rp2040_begin_frame(&lcd);
}

#ifndef ENABLE_DUAL_CORE_PPU
void platform_video_send_scanline(int y, const pixel_t *pixels) {
    if (platform_headless)
        return;

    (void)y;

    /* Copy pixels to the idle buffer while previous DMA runs */
    int fill = active_buf ^ 1;
    memcpy(scanline_buf[fill], pixels, VIEWPORT_WIDTH * sizeof(uint16_t));

    st7789_rp2040_dma_wait(dma_chan);

    active_buf = fill;
    st7789_rp2040_dma_start(dma_chan, scanline_buf[fill], VIEWPORT_WIDTH);
}
#endif

pixel_t *platform_video_get_framebuffer(void) {
    return NULL;
}

void platform_video_end_frame(void) {
#ifndef ENABLE_DUAL_CORE_PPU
    st7789_rp2040_dma_finish(dma_chan);
#endif
    st7789_rp2040_end_frame();
}
