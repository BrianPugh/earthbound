#ifdef ENABLE_DUAL_CORE_PPU

#include "platform/platform.h"
#include "snes/ppu.h"
#include "display/st7789/rp2040/st7789_rp2040.h"

#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"

/* Per-core double buffers for scanline DMA.
 * Each core renders into one buffer while the previous DMA runs from the other. */
static pixel_t dma_buf[2][2][VIEWPORT_WIDTH];  /* [core][active_buf][pixels] */

/* Shared DMA channel + sequence counter.
 * next_dma_y enforces strict ordering: y=0,1,2,...239.
 * Each core waits for its turn, sends via DMA, then advances the counter. */
static int worker_dma_chan = -1;
static volatile int next_dma_y;

/* FPS overlay */
static volatile scanline_stamp_cb_t fps_stamp_cb;
static volatile bool fps_active;

/* Per-core send callback — waits for turn, DMA sends, advances counter. */
static void __not_in_flash_func(core_send_scanline)(int core_id, int y,
                                                     const pixel_t *pixels) {
    if (fps_active)
        fps_stamp_cb(y, (pixel_t *)pixels);

    /* Copy to this core's idle DMA buffer */
    static int active[2];  /* per-core active buffer index */
    int buf = active[core_id] ^ 1;
    memcpy(dma_buf[core_id][buf], pixels, VIEWPORT_WIDTH * sizeof(pixel_t));

    /* Wait for our turn in the output sequence */
    while (next_dma_y != y)
        tight_loop_contents();

    /* Wait for previous DMA transfer to complete */
    st7789_rp2040_dma_wait(worker_dma_chan);

    /* Start DMA from our buffer */
    st7789_rp2040_dma_start(worker_dma_chan,
        dma_buf[core_id][buf], VIEWPORT_WIDTH);
    active[core_id] = buf;

    /* Signal next scanline can send */
    next_dma_y = y + 1;
}

/* Scanline callbacks — thin wrappers that pass core_id */
static void __not_in_flash_func(core0_send)(int y, const pixel_t *pixels) {
    core_send_scanline(0, y, pixels);
}

static void __not_in_flash_func(core1_send)(int y, const pixel_t *pixels) {
    core_send_scanline(1, y, pixels);
}

/* Core 1 entry point — renders odd scanlines */
static void core1_entry(void) {
    /* Enable multicore lockout so flash writes can pause this core. */
    multicore_lockout_victim_init();

    for (;;) {
        multicore_fifo_pop_blocking();

        ppu_render_frame_ex(1, 1, VIEWPORT_HEIGHT, 2, core1_send);

        multicore_fifo_push_blocking(1);
    }
}

/* Initialize dual-core worker */
void platform_worker_init(void) {
    worker_dma_chan = st7789_rp2040_claim_dma();
    multicore_launch_core1(core1_entry);
}

/* Render a frame using both cores. */
void platform_render_frame(scanline_stamp_cb_t fps_overlay_cb) {
    next_dma_y = 0;
    fps_stamp_cb = fps_overlay_cb;
    fps_active = (fps_overlay_cb != NULL);

    /* Build shadow palette before signaling core 1 */
    ppu_prepare_palette();

    /* Signal core 1 to start rendering odd scanlines */
    multicore_fifo_push_blocking(1);

    /* Set up LCD window (while core 1 starts rendering) */
    platform_video_begin_frame();

    /* Core 0 renders even scanlines */
    ppu_render_frame_ex(0, 0, VIEWPORT_HEIGHT, 2, core0_send);

    /* Wait for core 1 to finish */
    multicore_fifo_pop_blocking();

    /* Wait for final DMA + SPI drain */
    st7789_rp2040_dma_finish(worker_dma_chan);

    platform_video_end_frame();
}

#endif /* ENABLE_DUAL_CORE_PPU */
