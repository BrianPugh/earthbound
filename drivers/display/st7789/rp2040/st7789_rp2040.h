#ifndef DRIVERS_DISPLAY_ST7789_RP2040_H
#define DRIVERS_DISPLAY_ST7789_RP2040_H

#include "../st7789.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Claim and configure a DMA channel for 16-bit SPI pixel streaming.
 * Returns the DMA channel number. Can be called multiple times for
 * separate channels (e.g. single-core vs dual-core paths). */
int st7789_rp2040_claim_dma(void);

/* Release a DMA channel previously claimed by st7789_rp2040_claim_dma. */
void st7789_rp2040_release_dma(int dma_chan);

/* Begin a frame: switch SPI to 8-bit for commands, set the drawing window,
 * then switch back to 16-bit for pixel DMA. */
void st7789_rp2040_begin_frame(const st7789_t *lcd);

/* End a frame: release CS (call after DMA is fully drained). */
void st7789_rp2040_end_frame(void);

/* Start a DMA transfer of `count` 16-bit pixels from `buf`.
 * Non-blocking — returns immediately. Use st7789_rp2040_dma_wait to block. */
void st7789_rp2040_dma_start(int dma_chan, const uint16_t *buf, uint16_t count);

/* Wait for a DMA transfer to complete. */
void st7789_rp2040_dma_wait(int dma_chan);

/* Wait for DMA to complete AND for the SPI TX FIFO to fully drain
 * (all bits clocked out on the wire). */
void st7789_rp2040_dma_finish(int dma_chan);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_DISPLAY_ST7789_RP2040_H */
