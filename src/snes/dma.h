#ifndef SNES_DMA_H
#define SNES_DMA_H

#include "core/types.h"
#include "snes/ppu.h"

/* DMA transfer modes */
#define DMA_CPU_TO_IO  0x00
#define DMA_IO_TO_CPU  0x80

/* DMA destination registers (B-bus addresses, $21xx) */
#define DMA_DEST_VRAM_LO   0x18  /* VMDATAL */
#define DMA_DEST_VRAM_HI   0x19  /* VMDATAH */
#define DMA_DEST_CGRAM     0x22  /* CGDATA */
#define DMA_DEST_OAM       0x04  /* OAMDATA */

/* Add a DMA transfer (executed immediately in the C port) */
void dma_queue_add(uint8_t mode, const uint8_t *src, uint16_t dest_vram_addr, uint16_t size);

/* Execute all queued DMA transfers */
void dma_queue_flush(void);

/* Direct DMA helpers */
void dma_vram_transfer(const uint8_t *src, uint16_t vram_word_addr, uint16_t byte_count);
void dma_cgram_transfer(const uint8_t *src, uint8_t start_color, uint16_t byte_count);
void dma_oam_transfer(const uint8_t *src, uint16_t byte_count);

#endif /* SNES_DMA_H */
