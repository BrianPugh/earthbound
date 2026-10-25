/* Generic RP2040 dual-core PPU rendering.
 *
 * Band split: core 0 renders the top band of the screen and core 1 the
 * bottom band, so each core keeps the renderer's vertical reuse (BG row
 * plans, tile cache) within its band. Each core gathers CHUNK_LINES lines,
 * and the SPI bus takes the chunks in alternation (top 0, bottom 0, top 1,
 * ...); each chunk first moves the LCD's write pointer to its first row.
 * The renderer draws straight into the DMA buffers (EB_PPU_EXTERNAL_LINE_OUT),
 * so no line is copied.
 * No board-specific dependencies — uses only Pico SDK multicore APIs and the
 * st7789_rp2040 DMA interface.
 */

#ifdef EB_ENABLE_DUAL_CORE_PPU

#include "platform/platform.h"
#include "snes/ppu.h"
#include "display/st7789/rp2040/st7789_rp2040.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"

/* First scanline of core 1's band. */
#define BAND_SPLIT_Y (EB_VIEWPORT_HEIGHT / 2)

/* Scanlines per DMA transfer. Each transfer pays one row-address command
 * sequence, so bigger chunks cost less bus time and more buffer RAM. */
#define CHUNK_LINES 1
_Static_assert(BAND_SPLIT_Y % CHUNK_LINES == 0 &&
               (EB_VIEWPORT_HEIGHT - BAND_SPLIT_Y) % CHUNK_LINES == 0,
               "each band must be a whole number of chunks");

/* Per-core double buffers for chunk DMA, which the renderer draws into.
 * Each core fills one buffer while the previous DMA runs from the other.
 * Core 1's sit in its own scratch bank, below its stack (PICO_CORE1_STACK_SIZE
 * leaves them room), so they cost no main SRAM. */
typedef pixel_t ChunkBuf[CHUNK_LINES * EB_VIEWPORT_WIDTH];
static ChunkBuf core0_bufs[2];
static ChunkBuf __scratch_x("core1_line_bufs") core1_bufs[2];
static ChunkBuf *const dma_buf[2] = { core0_bufs, core1_bufs };  /* [core][buf] */
static int fill_buf[2];  /* per-core buffer being drawn into */

/* Line `line` of core_id's buffer `buf`. */
static inline pixel_t *chunk_line(int core_id, int buf, int line) {
    return &dma_buf[core_id][buf][line * EB_VIEWPORT_WIDTH];
}

/* Shared DMA channel + turn counter. Turn 2*i is the top band's chunk i,
 * turn 2*i+1 the bottom band's chunk i. Each core waits for its turn,
 * sends via DMA, then advances the counter. */
static int worker_dma_chan = -1;
static volatile int next_turn;

/* Frame handshake. Not the inter-core FIFO: multicore_lockout_victim_init()
 * gives core 1 a FIFO IRQ handler that drains every word while looking for
 * lockout requests, so a FIFO "start" message would never reach core1_entry.
 * Core 0 bumps frame_start_seq; core 1 echoes it into frame_done_seq. */
static volatile uint32_t frame_start_seq;
static volatile uint32_t frame_done_seq;

/* FPS overlay */
static volatile scanline_stamp_cb_t fps_stamp_cb;
static volatile bool fps_active;

/* Per-core send callback — `pixels` is already in this core's DMA buffer.
 * Once a chunk is full, waits for its turn, DMA sends it, advances the
 * counter and points the renderer at the other buffer. */
static void __not_in_flash_func(core_send_scanline)(int core_id, int y,
                                                     const pixel_t *pixels) {
    if (fps_active)
        fps_stamp_cb(y, (pixel_t *)pixels);

    int buf = fill_buf[core_id];
    int band_y = core_id == 0 ? y : y - BAND_SPLIT_Y;
    int line = band_y % CHUNK_LINES;
    if (line != CHUNK_LINES - 1) {
        ppu_set_line_out(core_id, chunk_line(core_id, buf, line + 1));
        return;
    }

    /* Wait for our turn in the output sequence */
    int turn = 2 * (band_y / CHUNK_LINES) + core_id;
    while (next_turn != turn)
        tight_loop_contents();

    /* Drain the previous chunk off the wire, then point the LCD at this one */
    st7789_rp2040_dma_finish(worker_dma_chan);
    st7789_rp2040_set_row((uint16_t)(y - (CHUNK_LINES - 1)), EB_VIEWPORT_HEIGHT - 1);

    /* Start DMA from our buffer */
    st7789_rp2040_dma_start(worker_dma_chan,
        dma_buf[core_id][buf], CHUNK_LINES * EB_VIEWPORT_WIDTH);

    /* Draw the next chunk into the other buffer: its DMA, this core's previous
     * chunk, finished before the one dma_finish just waited on. */
    fill_buf[core_id] = buf ^ 1;
    ppu_set_line_out(core_id, chunk_line(core_id, buf ^ 1, 0));

    /* Signal the other band's chunk can send */
    next_turn = turn + 1;
}

/* Scanline callbacks — thin wrappers that pass core_id */
static void __not_in_flash_func(core0_send)(int y, const pixel_t *pixels) {
    core_send_scanline(0, y, pixels);
}

static void __not_in_flash_func(core1_send)(int y, const pixel_t *pixels) {
    core_send_scanline(1, y, pixels);
}

/* Core 1 entry point — renders the bottom band */
static void core1_entry(void) {
    /* Enable multicore lockout so flash writes can pause this core. */
    multicore_lockout_victim_init();

    uint32_t seen = 0;
    for (;;) {
        while (frame_start_seq == seen)
            __wfe();
        seen = frame_start_seq;
        __dmb();

        ppu_set_line_out(1, chunk_line(1, fill_buf[1], 0));
        ppu_render_frame_ex(1, BAND_SPLIT_Y, EB_VIEWPORT_HEIGHT, 1, core1_send);

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
    next_turn = 0;
    fps_stamp_cb = fps_overlay_cb;
    fps_active = (fps_overlay_cb != NULL);

    /* Build the shared per-frame state before signaling core 1 */
    ppu_prepare_frame();

    /* Signal core 1 to start rendering the bottom band */
    __dmb();
    uint32_t seq = frame_start_seq + 1;
    frame_start_seq = seq;
    __sev();

    /* Set up LCD window (while core 1 starts rendering) */
    platform_video_begin_frame();

    /* Core 0 renders the top band */
    ppu_set_line_out(0, chunk_line(0, fill_buf[0], 0));
    ppu_render_frame_ex(0, 0, BAND_SPLIT_Y, 1, core0_send);

    /* Wait for core 1 to finish */
    while (frame_done_seq != seq)
        __wfe();
    __dmb();

    /* Wait for final DMA + SPI drain */
    st7789_rp2040_dma_finish(worker_dma_chan);

    platform_video_end_frame();
}

#endif /* EB_ENABLE_DUAL_CORE_PPU */
