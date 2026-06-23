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
#include "core/mode_stack.h"
#include <string.h>

/* Forward declarations from main.c */
#include "game_main.h"

/* Graphics and arrangements decompress directly to ppu.vram, matching the
 * ROM's BUFFER → VRAM DMA flow without needing an intermediate buffer.
 * ppu.vram persists between logo loads (BSS-zeroed once), matching the ROM. */

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

    /* Fill viewport — the dark background tile wraps seamlessly. The renderer's
     * FILL path now centers the native 256px content at viewport x [PAD_LEFT,
     * PAD_LEFT+256) for every explicit-FILL layer (see render_bg_scanline's
     * fill_pad), so no manual hofs compensation is needed — doing so would
     * double-shift the logo. */
    ppu.bg_viewport_fill[2] = BG_VIEWPORT_FILL;
    ppu.bg_hofs[2] = 0;

    /* Vertically center the logo: an explicit-FILL layer's vertical position is
     * driven by sprite_y_offset (snes_scanline = scanline - sprite_y_offset),
     * so without this the logo renders top-aligned. The dark background tile
     * still wraps to fill the top/bottom gutters. Horizontal centering is
     * automatic via the FILL fill_pad (see render_bg_scanline). */
    ppu.sprite_y_offset = EB_VIEWPORT_PAD_TOP;

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

/* ---- GAME_MODE_INTRO_LOGO -------------------------------------------------
 * Run-to-completion port of the blocking logo_screen() below. See the
 * IntroLogoState comment in mode_stack.h. The single yield is owned by the pump;
 * this body never calls wait_for_vblank(). The brightness ramps are pushed as
 * GAME_MODE_MOSAIC_FADE children (MF_IN/MF_OUT, no mosaic), exactly matching the
 * former fade_in_with_mosaic() / fade_out_with_mosaic(step,delay,0) calls. */

/* Logo IDs in display order, indexed by IntroLogoState.logo_idx. */
static const uint16_t intro_logo_ids[3] = { LOGO_NINTENDO, LOGO_APE, LOGO_HALKEN };

/* Build a STEP_PUSH of a MOSAIC_FADE child. The pump copies *push_init into the
 * child's stack level synchronously right after this step returns (before any
 * other step runs), so a function-local static is a safe home for the init. */
static StepResult intro_logo_push_fade(MosaicFadeKind kind, uint8_t step, uint16_t delay) {
    static ModeState child;
    memset(&child, 0, sizeof(child));
    child.mosaic_fade.kind  = (uint8_t)kind;
    child.mosaic_fade.step  = step;
    child.mosaic_fade.delay = delay;
    /* mosaic_bgs = 0 (logos use no mosaic); final_hdma = 0; phase/delay_left = 0 */
    return STEP_RESULT_PUSH_INIT(GAME_MODE_MOSAIC_FADE, &child);
}

/* Load logo[idx] and begin its fade-in (shared by initial entry + inter-logo
 * transition). Primes INIDISP=0x00 / MOSAIC=0 (FADE_IN_WITH_MOSAIC's first act)
 * and pushes MF_IN(step 1, delay 2, no mosaic). Resumes at LG_HOLD. */
static StepResult intro_logo_begin(IntroLogoState *s) {
    logo_screen_load(intro_logo_ids[s->logo_idx]);
    ppu.inidisp = 0x00; /* brightness 0, force blank off */
    ppu.mosaic = 0;
    s->hold_remaining = (s->logo_idx == 0) ? 180 : 120;
    s->skipping = 0;
    s->phase = LG_HOLD;
    return intro_logo_push_fade(MF_IN, 1, 2);
}

StepResult mode_step_intro_logo(ModeState *st) {
    IntroLogoState *s = &st->intro_logo;

    switch ((IntroLogoPhase)s->phase) {
    case LG_LOAD:
        return intro_logo_begin(s);

    case LG_HOLD:
        /* APE/HAL (idx > 0) skip on any button (post-yield read), exactly like
         * the former wait_frames_or_until_pressed(); Nintendo (idx 0) is a plain
         * 180-frame hold with no button check. */
        if (s->logo_idx > 0 && platform_input_get_pad_new()) {
            s->skipping = 1;
            s->phase = LG_DONE_FADE;
            return intro_logo_push_fade(MF_OUT, 2, 1); /* fast skip fade-out */
        }
        if (s->hold_remaining == 0) {
            s->phase = LG_DONE_FADE;
            return intro_logo_push_fade(MF_OUT, 1, 2); /* normal fade-out */
        }
        s->hold_remaining--;
        return STEP_RESULT_CONTINUE();

    case LG_DONE_FADE:
        if (s->skipping)
            return STEP_RESULT_POP(1);
        if (s->logo_idx < 2) {
            s->logo_idx++;
            return intro_logo_begin(s);
        }
        return STEP_RESULT_POP(0);
    }
    return STEP_RESULT_POP(0);
}

/* The blocking logo_screen() pump bridge was deleted (D4b); init_intro
 * (GAME_MODE_INIT_INTRO) STEP_PUSHes GAME_MODE_INTRO_LOGO (phase LG_LOAD,
 * logo_idx 0) directly. */
