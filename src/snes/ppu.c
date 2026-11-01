#include "snes/ppu.h"
#include "include/binary.h"
#include <string.h>

/* 4-byte aligned: the renderer reads VRAM tiles as aligned words. */
PPUState ppu __attribute__((aligned(4)));

void ppu_init(void) {
    memset(&ppu, 0, sizeof(ppu));
    ppu.inidisp = 0x80; /* force blank on */

    /* The game's OAM_CLEAR runs early in boot to hide all sprites
       off-screen (Y=EB_VIEWPORT_HEIGHT, below the visible display). Since the port
       skips the boot sequence, pre-apply OAM_CLEAR state here. */
    for (int i = 0; i < 128; i++) {
        ppu.oam[i].y = EB_VIEWPORT_HEIGHT;
        ppu.oam_full_y[i] = EB_VIEWPORT_HEIGHT;
    }
    ppu.oam_hi[0] = 0x80;
}

void ppu_clear_effects(void) {
    ppu.cgwsel = 0;
    ppu.cgadsub = 0;
    ppu.ts = 0;
    ppu.coldata_r = 0;
    ppu.coldata_g = 0;
    ppu.coldata_b = 0;
    ppu.mosaic = 0;
    /* Window configuration registers (tmw, tsw, w12sel, w34sel, wobjsel,
     * wbglog, wobjlog) are NOT cleared here.  On real hardware, the
     * equivalent of this function runs during force-blank, but window
     * config registers persist — HDMA re-programs WH0/WH1 per-scanline
     * when the screen comes back.  Systems that use windowing (oval window,
     * battle swirl) manage their own lifecycle via stop_oval_window(),
     * stop_battle_swirl(), and update_swirl_effect() auto-restore.
     * Clearing them here would destroy active oval window masking during
     * attract mode teleports (overworld_setup_vram → ppu_clear_effects). */

    /* Clear per-scanline HDMA overrides for TM (layer enable per scanline).
     * Window HDMA flags are preserved — the oval window / battle swirl
     * systems manage those.  On real hardware, HDMAEN=0 during force blank
     * prevents HDMA from running, so stale TM tables are harmless.  The C
     * port's software renderer reads these flags every frame — if left set,
     * stale per-scanline TM values cause visible stripe artifacts
     * (e.g. on the game-over screen after a battle). */
    ppu.tm_hdma_active = false;
}

void ppu_cgram_write(uint8_t index, uint16_t color) {
    ppu.cgram[index] = color;
}

void ppu_vram_dma(const uint8_t *src, uint16_t vram_word_addr, uint16_t byte_count) {
    /* SNES DMA: size 0 means 65536 bytes (full 64KB transfer) */
    uint32_t count = byte_count ? byte_count : 0x10000;
    /* The VRAM word address is 15 bits, so a transfer past the end wraps to 0. */
    uint32_t byte_addr = ((uint32_t)vram_word_addr * 2) % VRAM_SIZE;
    while (count > 0) {
        uint32_t chunk = VRAM_SIZE - byte_addr;
        if (chunk > count) chunk = count;
        memcpy(&ppu.vram[byte_addr], src, chunk);
        src += chunk;
        count -= chunk;
        byte_addr = 0;
    }
}

void ppu_cgram_dma(const uint8_t *src, uint8_t start_color, uint16_t byte_count) {
    /* SNES DMA: size 0 means 65536 bytes */
    uint32_t count = byte_count ? byte_count : 0x10000;
    /* The CGRAM byte address is 9 bits, so a transfer past the end wraps to 0. */
    uint32_t offset = (uint32_t)start_color * 2;
    while (count > 0) {
        uint32_t chunk = CGRAM_SIZE - offset;
        if (chunk > count) chunk = count;
        memcpy((uint8_t *)ppu.cgram + offset, src, chunk);
        src += chunk;
        count -= chunk;
        offset = 0;
    }
}

void ppu_oam_dma(const uint8_t *src, uint16_t byte_count) {
    /* SNES DMA: size 0 means 65536 bytes */
    uint32_t count = byte_count ? byte_count : 0x10000;
    if (count <= OAM_SIZE) {
        memcpy(ppu.oam, src, count > 512 ? 512 : count);
        if (count > 512) {
            memcpy(ppu.oam_hi, src + 512, count - 512);
        }
    }
}
