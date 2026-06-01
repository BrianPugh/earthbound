#include "snes/ppu.h"
#include "include/binary.h"
#include <string.h>

PPUState ppu;

/* ---- VRAM write barrier (see ppu.h) --------------------------------------
 * vram_gen[b] bumps whenever any byte in 64-byte block b is written.  The
 * renderer records the generation at decode time and re-decodes only when it
 * no longer matches — so the decode cache survives across frames for the
 * (overwhelming) majority of tiles whose graphics never change. */
uint32_t vram_gen[VRAM_GEN_BLOCKS];

void vram_dirty(uint32_t byte_off, uint32_t n) {
    if (n == 0) return;
    uint32_t end = byte_off + n - 1;
    if (end >= VRAM_SIZE) end = VRAM_SIZE - 1;          /* defensive clamp */
    for (uint32_t b = byte_off >> VRAM_GEN_SHIFT; b <= (end >> VRAM_GEN_SHIFT); b++)
        vram_gen[b]++;
}

void vram_dirty_all(void) {
    for (uint32_t b = 0; b < VRAM_GEN_BLOCKS; b++) vram_gen[b]++;
}

/* These mirror the bare memcpy/memset the call sites used before, plus the
 * dirty stamp.  No bounds check is added (matching prior semantics); callers
 * that wrote in-range still do, and vram_dirty() clamps its block range. */
void vram_write(uint32_t byte_off, const void *src, uint32_t n) {
    memcpy(&ppu.vram[byte_off], src, n);
    vram_dirty(byte_off, n);
}

void vram_memset(uint32_t byte_off, uint8_t val, uint32_t n) {
    memset(&ppu.vram[byte_off], val, n);
    vram_dirty(byte_off, n);
}

void vram_write_u16(uint32_t byte_off, uint16_t val) {
    ppu.vram[byte_off]     = (uint8_t)val;
    ppu.vram[byte_off + 1] = (uint8_t)(val >> 8);
    vram_dirty(byte_off, 2);
}

#ifdef PPU_VRAM_VERIFY
#include <stdio.h>
/* Per-block FNV hash of last frame's VRAM + the generation we last saw, so we
 * can detect a block that changed content without its generation bumping —
 * i.e. a write that bypassed the barrier.  8 KB of state, debug builds only. */
static uint32_t vram_hash_prev[VRAM_GEN_BLOCKS];
static uint32_t vram_gen_prev[VRAM_GEN_BLOCKS];
static bool     vram_verify_primed;
void vram_verify_frame(void) {
    for (uint32_t b = 0; b < VRAM_GEN_BLOCKS; b++) {
        const uint8_t *p = &ppu.vram[b << VRAM_GEN_SHIFT];
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < (1u << VRAM_GEN_SHIFT); i++) h = (h ^ p[i]) * 16777619u;
        bool changed = (h != vram_hash_prev[b]);
        bool bumped  = (vram_gen[b] != vram_gen_prev[b]);
        if (vram_verify_primed && changed && !bumped) {
            printf("VRAM-BARRIER MISS: block %lu (byte 0x%lx) changed without vram_dirty()\n",
                   (unsigned long)b, (unsigned long)(b << VRAM_GEN_SHIFT));
        }
        vram_hash_prev[b] = h;
        vram_gen_prev[b]  = vram_gen[b];
    }
    vram_verify_primed = true;
}
#endif

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

void ppu_reset(void) {
    /* Zero all registers but keep memory */
    ppu.inidisp = 0x80;
    ppu.obsel = 0;
    ppu.bgmode = 0;
    ppu.mosaic = 0;
    memset(ppu.bg_sc, 0, sizeof(ppu.bg_sc));
    memset(ppu.bg_nba, 0, sizeof(ppu.bg_nba));
    memset(ppu.bg_hofs, 0, sizeof(ppu.bg_hofs));
    memset(ppu.bg_vofs, 0, sizeof(ppu.bg_vofs));
    ppu.tm = 0;
    ppu.ts = 0;
    ppu.tmw = 0;
    ppu.tsw = 0;
    ppu.cgwsel = 0;
    ppu.cgadsub = 0;
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

void ppu_set_vram_addr(uint16_t addr) {
    ppu.vram_addr = addr;
}

void ppu_vram_write(uint16_t data) {
    uint16_t addr = ppu.vram_addr;
    if (addr < VRAM_SIZE / 2) {
        write_u16_le(&ppu.vram[addr * 2], data);
        vram_dirty((uint32_t)addr * 2, 2);
    }
    /* Increment based on vmain setting */
    uint8_t inc = ppu.vmain & 0x03;
    switch (inc) {
    case 0: ppu.vram_addr += 1; break;
    case 1: ppu.vram_addr += 32; break;
    case 2: case 3: ppu.vram_addr += 128; break;
    }
}

void ppu_cgram_write(uint8_t index, uint16_t color) {
    ppu.cgram[index] = color;
}

void ppu_vram_dma(const uint8_t *src, uint16_t vram_word_addr, uint16_t byte_count) {
    /* SNES DMA: size 0 means 65536 bytes (full 64KB transfer) */
    uint32_t count = byte_count ? byte_count : 0x10000;
    uint32_t byte_addr = (uint32_t)vram_word_addr * 2;
    if (byte_addr + count <= VRAM_SIZE) {
        memcpy(&ppu.vram[byte_addr], src, count);
        vram_dirty(byte_addr, count);
    }
}

void ppu_cgram_dma(const uint8_t *src, uint8_t start_color, uint16_t byte_count) {
    /* SNES DMA: size 0 means 65536 bytes */
    uint32_t count = byte_count ? byte_count : 0x10000;
    uint16_t offset = (uint16_t)start_color * 2;
    if (offset + count <= CGRAM_SIZE) {
        memcpy((uint8_t *)ppu.cgram + offset, src, count);
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
