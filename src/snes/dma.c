#include "snes/dma.h"
#include <string.h>

void dma_queue_add(uint8_t mode, const uint8_t *src, uint16_t dest_vram_addr, uint16_t size) {
    (void)mode;
    /* In the C port, transfers happen immediately — no queue needed */
    ppu_vram_dma(src, dest_vram_addr, size);
}

void dma_queue_flush(void) {
    /* In the C port, transfers happen immediately in dma_queue_add — nothing to flush */
}

void dma_vram_transfer(const uint8_t *src, uint16_t vram_word_addr, uint16_t byte_count) {
    ppu_vram_dma(src, vram_word_addr, byte_count);
}

void dma_cgram_transfer(const uint8_t *src, uint8_t start_color, uint16_t byte_count) {
    ppu_cgram_dma(src, start_color, byte_count);
}

void dma_oam_transfer(const uint8_t *src, uint16_t byte_count) {
    ppu_oam_dma(src, byte_count);
}
