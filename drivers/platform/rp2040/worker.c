/* Generic RP2040 dual-core PPU rendering.
 *
 * Distributes even/odd scanlines across both RP2040 cores with strict
 * y-ordering for DMA output. No board-specific dependencies — uses only
 * Pico SDK multicore APIs and the st7789_rp2040 DMA interface.
 */

#ifdef EB_ENABLE_DUAL_CORE_PPU

#include "platform/platform.h"
#include "snes/ppu.h"
#include "display/st7789/rp2040/st7789_rp2040.h"

#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"

/* Per-core double buffers for scanline DMA.
 * Each core renders into one buffer while the previous DMA runs from the other. */
static pixel_t dma_buf[2][2][EB_VIEWPORT_WIDTH];  /* [core][active_buf][pixels] */

/* Shared DMA channel + sequence counter.
 * next_dma_y enforces strict ordering: y=0,1,2,...height-1.
 * Each core waits for its turn, sends via DMA, then advances the counter. */
static int worker_dma_chan = -1;
static volatile int next_dma_y;

/* Frame handshake. Not the inter-core FIFO: multicore_lockout_victim_init()
 * gives core 1 a FIFO IRQ handler that drains every word while looking for
 * lockout requests, so a FIFO "start" message would never reach core1_entry.
 * Core 0 bumps frame_start_seq; core 1 echoes it into frame_done_seq. */
static volatile uint32_t frame_start_seq;
static volatile uint32_t frame_done_seq;

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
    memcpy(dma_buf[core_id][buf], pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));

    /* Wait for our turn in the output sequence */
    while (next_dma_y != y)
        tight_loop_contents();

    /* Wait for previous DMA transfer to complete */
    st7789_rp2040_dma_wait(worker_dma_chan);

    /* Start DMA from our buffer */
    st7789_rp2040_dma_start(worker_dma_chan,
        dma_buf[core_id][buf], EB_VIEWPORT_WIDTH);
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

    uint32_t seen = 0;
    for (;;) {
        while (frame_start_seq == seen)
            __wfe();
        seen = frame_start_seq;
        __dmb();

        ppu_render_frame_ex(1, 1, EB_VIEWPORT_HEIGHT, 2, core1_send);

        __dmb();
        frame_done_seq = seen;
        __sev();
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
    __dmb();
    uint32_t seq = frame_start_seq + 1;
    frame_start_seq = seq;
    __sev();

    /* Set up LCD window (while core 1 starts rendering) */
    platform_video_begin_frame();

    /* Core 0 renders even scanlines */
    ppu_render_frame_ex(0, 0, EB_VIEWPORT_HEIGHT, 2, core0_send);

    /* Wait for core 1 to finish */
    while (frame_done_seq != seq)
        __wfe();
    __dmb();

    /* Wait for final DMA + SPI drain */
    st7789_rp2040_dma_finish(worker_dma_chan);

    platform_video_end_frame();
}

#endif /* EB_ENABLE_DUAL_CORE_PPU */
