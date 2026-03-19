#include "intro/logo_screen.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "core/decomp.h"
#include "core/memory.h"
#include "entity/entity.h"
#include "game/fade.h"
#include "game/audio.h"
#include "data/assets.h"
#include "platform/platform.h"
#include <string.h>

/* Forward declarations from main.c */
#include "game_main.h"

/* Graphics and arrangements decompress directly to ppu.vram, matching the
 * ROM's BUFFER → VRAM DMA flow without needing an intermediate buffer.
 * ppu.vram persists between logo loads (BSS-zeroed once), matching the ROM. */

/* Fade in with mosaic effect.
   Ports FADE_IN_WITH_MOSAIC: A=step, X=delay_frames, Y=mosaic_enable */
static void fade_in_with_mosaic(uint16_t step, uint16_t delay_frames, uint16_t mosaic_enable) {
    ppu.inidisp = 0x00; /* brightness 0, force blank off */
    ppu.mosaic = 0;

    while (1) {
        uint8_t brightness = ppu.inidisp & 0x0F;
        uint16_t next = brightness + step;
        if (next >= 0x0F) {
            ppu.inidisp = (ppu.inidisp & 0xF0) | 0x0F;
            break;
        }
        ppu.inidisp = (ppu.inidisp & 0xF0) | (uint8_t)next;

        if (mosaic_enable) {
            /* Mosaic size = inverted brightness, shifted left 4 */
            uint8_t inv = (~(uint8_t)next) & 0x0F;
            ppu.mosaic = (inv << 4) | mosaic_enable;
        }

        for (uint16_t i = 0; i < delay_frames; i++) {
            wait_for_vblank();
        }
    }
}

/* Fade out with mosaic effect.
   Ports FADE_OUT_WITH_MOSAIC: A=step, X=delay_frames, Y=mosaic_enable */
void fade_out_with_mosaic(uint16_t step, uint16_t delay_frames, uint16_t mosaic_enable) {
    while (1) {
        ppu.mosaic = 0;
        uint8_t brightness = ppu.inidisp & 0x0F;
        /* ROM checks: if force blank already set (bit 7), exit (US only) */
        if (ppu.inidisp & 0x80) break;
        /* ROM: subtract step, if result < 0 then exit */
        int16_t next = (int16_t)brightness - (int16_t)step;
        if (next < 0) break;
        ppu.inidisp = (ppu.inidisp & 0xF0) | (uint8_t)next;

        if (mosaic_enable) {
            uint8_t inv = (~(uint8_t)next) & 0x0F;
            ppu.mosaic = (inv << 4) | mosaic_enable;
        }

        for (uint16_t i = 0; i < delay_frames; i++) {
            wait_for_vblank();
        }
    }
    ppu.inidisp = 0x80; /* force blank after fade out */
    /* ROM spins until the next NMI fires, then immediately continues to the
       next operation (logo load, fade_in, etc.) within the SAME frame.
       In the emulator, that spin-wait + subsequent code all happen in one
       runFrame call. So we do NOT call wait_for_vblank() here — the next
       wait_for_vblank() in fade_in_with_mosaic will serve as that frame. */
}

/* Wait for N frames, return true if button pressed */
static bool wait_frames_or_until_pressed(uint16_t frames) {
    return wait_frames_or_button(frames, 0xFFFF);
}

void logo_screen_load(uint16_t logo_id) {
    /* Set BG Mode 1 */
    ppu.bgmode = 0x01;

    /* BG3SC = $40: tilemap at VRAM word $4000, 32x32 screen size */
    ppu.bg_sc[2] = 0x40;

    /* BG3 tile data base: NBA register bits 7-4 for BG3
       For tile data at word address $0000: $0000 / $1000 = $0, so bits = 0 */
    ppu.bg_nba[1] = 0x00;

    /* Enable BG3 on main screen only (TM bit 2) */
    ppu.tm = 0x04;
    ppu.ts = 0x00;

    /* Fill viewport — the dark background tile wraps seamlessly.
     * Scroll to center the 256px content in the wider viewport. */
    ppu.bg_viewport_fill[2] = BG_VIEWPORT_FILL;
    ppu.bg_hofs[2] = (256 - VIEWPORT_PAD_LEFT) & 0xFF;

    /* Asset IDs for each logo.
     * Nintendo gfx/arr are locale-specific (US/ or JP/) — locale aliases
     * in asset_ids.h resolve transparently via locale aliases.
     * APE and HAL are locale-independent.
     * All palettes are locale-independent. */
    AssetId gfx_id, arr_id, pal_id;

    switch (logo_id) {
    case LOGO_NINTENDO:
        gfx_id = ASSET_INTRO_LOGOS_NINTENDO_GFX_LZHAL;
        arr_id = ASSET_INTRO_LOGOS_NINTENDO_ARR_LZHAL;
        pal_id = ASSET_INTRO_LOGOS_NINTENDO_PAL_LZHAL;
        break;
    case LOGO_APE:
        gfx_id = ASSET_INTRO_LOGOS_APE_GFX_LZHAL;
        arr_id = ASSET_INTRO_LOGOS_APE_ARR_LZHAL;
        pal_id = ASSET_INTRO_LOGOS_APE_PAL_LZHAL;
        break;
    case LOGO_HALKEN:
        gfx_id = ASSET_INTRO_LOGOS_HALKEN_GFX_LZHAL;
        arr_id = ASSET_INTRO_LOGOS_HALKEN_ARR_LZHAL;
        pal_id = ASSET_INTRO_LOGOS_HALKEN_PAL_LZHAL;
        break;
    default:
        return;
    }

    /* Load and decompress graphics */
    size_t comp_size;
    const uint8_t *comp_data;

    /* Graphics -> VRAM $0000 (tile data)
       ROM: COPY_TO_VRAM1 BUFFER, $0000, $8000, 0
       Decompress directly to VRAM — no intermediate buffer needed.
       NOTE: The ROM's BUFFER is BSS-zeroed once at boot, NOT between logo
       loads. Subsequent logos decompress on top of previous data, so leftover
       bytes from earlier logos persist beyond the new decompression output.
       ppu.vram is also zeroed at init, so the same retention applies. */
    comp_size = ASSET_SIZE(gfx_id);
    comp_data = ASSET_DATA(gfx_id);
    if (comp_data) {
        decomp(comp_data, comp_size, &ppu.vram[0x0000 * 2], 0x8000);
    }

    /* Arrangement (tilemap) -> VRAM $4000
       ROM always DMAs $800 bytes (full 32x32 tilemap).
       Same BSS-retention logic applies: don't zero between loads. */
    comp_size = ASSET_SIZE(arr_id);
    comp_data = ASSET_DATA(arr_id);
    if (comp_data) {
        decomp(comp_data, comp_size, &ppu.vram[0x4000 * 2], 0x800);
    }

    /* Palette -> PALETTES -> CGRAM (via NMI sync)
       ROM: decompresses to PALETTES ert.buffer, then sets PALETTE_UPLOAD_MODE=FULL
       which causes the NMI to copy the entire 512-byte PALETTES ert.buffer to CGRAM.
       This includes BSS-zeroed bytes beyond the decompressed palette data. */
    comp_size = ASSET_SIZE(pal_id);
    comp_data = ASSET_DATA(pal_id);
    if (comp_data) {
        decomp(comp_data, comp_size, (uint8_t *)ert.palettes, sizeof(ert.palettes));
        ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    }
}

uint16_t logo_screen(void) {
    /* Screen 0: Nintendo logo - show for 180 frames (3 seconds) */
    logo_screen_load(LOGO_NINTENDO);
    fade_in_with_mosaic(1, 2, 0); /* step=1, delay=2, no mosaic */

    /* Wait 180 frames (3 seconds) */
    for (int i = 0; i < 180; i++) {
        if (platform_input_quit_requested()) return 1;
        wait_for_vblank();
    }

    fade_out_with_mosaic(1, 2, 0);

    /* Screen 1: APE logo - show for 120 frames (2 seconds) */
    logo_screen_load(LOGO_APE);
    fade_in_with_mosaic(1, 2, 0);

    if (wait_frames_or_until_pressed(120)) {
        /* Button pressed - skip remaining logos */
        fade_out_with_mosaic(2, 1, 0);
        return 1;
    }

    fade_out_with_mosaic(1, 2, 0);

    /* Screen 2: HAL Laboratory logo */
    logo_screen_load(LOGO_HALKEN);
    fade_in_with_mosaic(1, 2, 0);

    if (wait_frames_or_until_pressed(120)) {
        fade_out_with_mosaic(2, 1, 0);
        return 1;
    }

    fade_out_with_mosaic(1, 2, 0);
    return 0;
}
