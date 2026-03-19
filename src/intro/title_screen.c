/*
 * Port of SHOW_TITLE_SCREEN (asm/intro/show_title_screen.asm)
 * and supporting functions.
 *
 * ROM call graph:
 *   SHOW_TITLE_SCREEN
 *     ├── FORCE_BLANK_AND_WAIT_VBLANK
 *     ├── INIT_ENTITY_SYSTEM
 *     ├── SET_BGMODE(11)                    → ppu.bgmode = 0x0B
 *     ├── SET_OAM_SIZE(3)                   → ppu.obsel = 0x03
 *     ├── SET_BG1_VRAM_LOCATION(0,$5800,0)  → ppu.bg_sc/bg_nba
 *     ├── LOAD_TITLE_SCREEN_GRAPHICS        → load_title_screen_graphics()
 *     ├── OAM_CLEAR                         → memset oam/oam_hi
 *     ├── INIT_ENTITY_WIPE(TITLE_SCREEN_1)  → entity_init_wipe()
 *     │     └── Entity events drive:
 *     │         ├── LOAD_TITLE_SCREEN_PALETTE (C0ECB7)
 *     │         └── Letter sprite animations (E1CE08)
 *     ├── (non-quick) sprite palette fade + 60-frame entity loop
 *     ├── Input wait loop (with entity system running)
 *     └── FADE_OUT_WITH_MOSAIC
 */
#include "intro/title_screen.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "game/overworld.h"
#include "data/event_script_data.h"
#include "include/binary.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "core/decomp.h"
#include "core/memory.h"
#include "game/fade.h"
#include "game/audio.h"
#include "data/assets.h"
#include "platform/platform.h"
#include "include/pad.h"
#include <string.h>

/* Forward declarations from main.c */
#include "game_main.h"


/*
 * Port of LOAD_TITLE_SCREEN_GRAPHICS (asm/intro/load_title_screen_graphics.asm).
 *
 * Decompresses and DMA-transfers tile/tilemap/OBJ data to VRAM.
 * Does NOT load ert.palettes — the entity system handles those.
 *
 * VRAM layout (word addresses, from include/enums.asm):
 *   VRAM::LOGO_TILES              = $0000  ($B000 bytes, BG1 8bpp tile data)
 *   VRAM::TITLE_SCREEN_TILEMAP_EB = $5800  ($1000 bytes, BG1 tilemap)
 *   VRAM::TITLE_SCREEN_OBJ_EB    = $6000  ($4000 bytes, OBJ 4bpp tile data)
 */
void load_title_screen_graphics(void) {
    size_t comp_size;
    const uint8_t *comp_data;

    /* ROM: DECOMP TITLE_SCREEN_GRAPHICS → BUFFER
     *      COPY_TO_VRAM1P BUFFER, VRAM::LOGO_TILES, $B000, 0
     * Decompress directly to VRAM — no intermediate buffer needed. */
    comp_size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_GFX_LZHAL);
    comp_data = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_GFX_LZHAL);
    if (comp_data) {
        uint8_t *vram_dst = &ppu.vram[0x0000 * 2];
        memset(vram_dst, 0, 0xB000);
        decomp(comp_data, comp_size, vram_dst, 0xB000);
    }

    /* ROM: DECOMP TITLE_SCREEN_ARRANGEMENT → BUFFER
     *      COPY_TO_VRAM1P BUFFER, VRAM::TITLE_SCREEN_TILEMAP_EB, $1000, 0 */
    comp_size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_ARR_LZHAL);
    comp_data = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_ARR_LZHAL);
    if (comp_data) {
        uint8_t *vram_dst = &ppu.vram[0x5800 * 2];
        memset(vram_dst, 0, 0x1000);
        decomp(comp_data, comp_size, vram_dst, 0x1000);
    }

    /* ROM: DECOMP TITLE_SCREEN_LETTER_GFX → BUFFER
     *      COPY_TO_VRAM1P BUFFER, VRAM::TITLE_SCREEN_OBJ_EB, $4000, 0 */
    comp_size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_LETTERS_GFX_LZHAL);
    comp_data = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_LETTERS_GFX_LZHAL);
    if (comp_data) {
        uint8_t *vram_dst = &ppu.vram[0x6000 * 2];
        memset(vram_dst, 0, 0x4000);
        decomp(comp_data, comp_size, vram_dst, 0x4000);
    }
}

/*
 * Port of SHOW_TITLE_SCREEN (asm/intro/show_title_screen.asm).
 *
 * Now uses the entity system for:
 *   - Palette loading and fade (via entity event scripts)
 *   - Letter sprite fly-in animation (via entity movement + draw callbacks)
 *   - State management (ACTIONSCRIPT_STATE signals when animation completes)
 *
 * Returns: 0 = timeout (attract mode), 1 = button pressed
 */
uint16_t show_title_screen(uint16_t quick_mode) {
    /* ROM: JSL FORCE_BLANK_AND_WAIT_VBLANK */
    ppu.inidisp = 0x80;

    /* Script data is loaded in init_intro() — available for both
     * gas station (EVENT_860) and title screen (EVENT_BATTLE_FX through TITLE_SCREEN_11, 787-798). */

    /* ROM: JSL INIT_ENTITY_SYSTEM */
    entity_system_init();

    /* Clear color math registers — may have residual values from
     * setup_entity_color_math() at previous title screen exit or
     * from attract mode entity scripts. The assembly relies on
     * SNES hardware prevent_mode handling; the C port must clear. */
    ppu.cgadsub = 0;
    ppu.cgwsel = 0;
    ppu.coldata_r = 0;
    ppu.coldata_g = 0;
    ppu.coldata_b = 0;

    /* ROM: LDA #11 / JSL SET_BGMODE
     * $0B = mode 3 (8bpp BG1, 4bpp BG2) + BG3 priority bit */
    ppu.bgmode = 0x0B;

    /* ROM: LDA #3 / JSL SET_OAM_SIZE
     * $03: OBJ name base at VRAM word $6000, size mode 0 (8x8/16x16) */
    ppu.obsel = 0x03;

    /* ROM: SET_BG1_VRAM_LOCATION(0,$5800,0)
     * BG1 tile data at word $0000, tilemap at word $5800, 32x32 screen */
    ppu.bg_sc[0] = 0x58;
    ppu.bg_nba[0] = (ppu.bg_nba[0] & 0xF0) | 0x00;

    /* ROM: STZ BG1_X_POS / STZ BG1_Y_POS / etc. */
    ppu.bg_hofs[0] = 0; ppu.bg_vofs[0] = 0;
    ppu.bg_hofs[1] = 0; ppu.bg_vofs[1] = 0;
    ppu.bg_hofs[2] = 0; ppu.bg_vofs[2] = 0;

    /* ROM: JSL LOAD_TITLE_SCREEN_GRAPHICS */
    load_title_screen_graphics();

    /* ROM: LDA #$11 / STA TM_MIRROR — enable BG1 + OBJ on main screen */
    ppu.tm = 0x11;
    ppu.ts = 0x00;

    /* Extend BG1 edges into the border area (clamp, no tilemap wrapping).
     * This avoids glow/letter wraparound while filling the borders with
     * the dark space color from the image edges. */
    ppu.bg_viewport_fill[0] = BG_VIEWPORT_CLAMP;

    /* Shift sprites to match the centered BG.  Non-filling BGs are rendered
     * at SNES_WIDTH and then centered in the viewport (temp_nf path), so
     * sprite OAM X values (designed for 256px) need the same offset. */
    ppu.sprite_x_offset = VIEWPORT_PAD_LEFT;

    /* ROM: JSL OAM_CLEAR */
    memset(ppu.oam, 0, sizeof(ppu.oam));
    memset(ppu.oam_hi, 0, sizeof(ppu.oam_hi));
    for (int i = 0; i < 128; i++) {
        ppu.oam[i].y = VIEWPORT_HEIGHT;
        ppu.oam_full_y[i] = VIEWPORT_HEIGHT;
    }

    /* Store quick_mode flag for entity scripts to read */
    ert.title_screen_quick_mode = quick_mode;

    /* ROM (show_title_screen.asm:61-64): JSL INIT_ENTITY_WIPE with TITLE_SCREEN_1
     * This spawns the title screen sequencer entity (TITLE_SCREEN_1, script 788),
     * which in turn spawns palette loader (TITLE_SCREEN_2, 789) and letter entities (790-798).
     * NOTE: EVENT_BATTLE_FX (787) is spawned by FILE_SELECT_INIT, NOT here. */
    entity_init_wipe(EVENT_SCRIPT_TITLE_SCREEN_1);

    /* ROM line 65: STZ ACTIONSCRIPT_STATE — clear after entity wipe.
     * entity_system_init already set it to 0, but the assembly explicitly
     * clears it again here as a safety measure. */
    ert.actionscript_state = 0;

    /* Non-quick path: load sprite palette and set up initial fade */
    if (!quick_mode) {
        /* ROM (lines 68-95):
         *   1. Zero all PALETTES
         *   2. Set INIDISP to $0F (full brightness, screen on)
         *   3. Decompress sprite palette (E1AE7C) → PALETTES+256
         *   4. COPY_FADE_BUFFER_TO_PALETTES (save to BUFFER)
         *   5. Zero PALETTES again
         *   6. PREPARE_PALETTE_FADE(60, $0100) — 60-frame sprite palette fade
         *   7. 60-frame loop: UPDATE_MAP_PALETTE_ANIMATION + RUN_ACTIONSCRIPT_FRAME */

        /* Zero all ert.palettes */
        memset(ert.palettes, 0, sizeof(ert.palettes));

        ppu.inidisp = 0x0F;

        /* Load sprite palette to upper half (colors 128-255) */
        size_t comp_size;
        comp_size = ASSET_SIZE(ASSET_E1AE7C_BIN_LZHAL);
        const uint8_t *comp_data = ASSET_DATA(ASSET_E1AE7C_BIN_LZHAL);
        if (comp_data) {
            /* Decompress directly to ert.palettes[128..255] (upper half) */
            decomp(comp_data, comp_size, (uint8_t *)&ert.palettes[128], 0x100);
        }

        /* Save current palette to BUFFER as fade target */
        PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
        memcpy(fade->target, ert.palettes, sizeof(fade->target));

        /* Zero ert.palettes again for fade-from-black */
        memset(ert.palettes, 0, sizeof(ert.palettes));

        /* PREPARE_PALETTE_FADE(60, $0100) — fade sprite palette group 8 */
        /* $0100 = bit 8 set = palette group 8 (colors 128-143) */
        /* Actually, let's just fade all sprite palette entries for correctness */
        /* In the ROM: mask=$0100 means only palette group 8.
         * But entity events will also set up the BG palette fade.
         * For now, we set up the sprite palette fade. */

        /* The entity system will handle BG palette fade via LOAD_TITLE_SCREEN_PALETTE.
         * We do a simple 60-frame sprite palette fade here. */

        /* Copy fade target to ert.buffer[0..511], set up slopes */
        /* The fade is from black (current=0) to the saved sprite palette */
        /* Use the full fade engine for groups that have data */
        {
            /* Re-save: fade->target already has the target from above */
            /* Accumulators start at 0 (current palette is 0) */
            memset(fade->slope_r, 0, sizeof(fade->slope_r) + sizeof(fade->slope_g)
                   + sizeof(fade->slope_b) + sizeof(fade->accum_r)
                   + sizeof(fade->accum_g) + sizeof(fade->accum_b));

            /* Compute slopes for palette group 8 (colors 128-143) */
            for (int i = 128; i < 144; i++) {
                uint16_t target = fade->target[i];
                int16_t t_r = target & 0x1F;
                int16_t t_g = (target >> 5) & 0x1F;
                int16_t t_b = (target >> 10) & 0x1F;

                /* slope = (target << 8) / 60 (current is 0) */
                fade->slope_r[i] = (int16_t)((t_r << 8) / 60);
                fade->slope_g[i] = (int16_t)((t_g << 8) / 60);
                fade->slope_b[i] = (int16_t)((t_b << 8) / 60);
            }
        }

        /* 60-frame loop: update palette animation + run entity system */
        for (int frame = 0; frame < 60; frame++) {
            /* UPDATE_MAP_PALETTE_ANIMATION — handled by callroutine but we
             * also need it here for the sprite palette pre-fade */
            /* Actually, let's keep it simple: the entity system's C42235
             * subroutine handles the full palette animation including the
             * 150-frame wait. During these first 60 frames, the entity scripts
             * are waiting (PAUSE $3C in EVENT_788, then PAUSE $96 in C42235).
             *
             * The sprite palette fade from show_title_screen.asm is separate.
             * We just do a simple lerp here. */
            for (int c = 128; c < 144; c++) {
                uint16_t target = fade->target[c];
                uint8_t tr = target & 0x1F;
                uint8_t tg = (target >> 5) & 0x1F;
                uint8_t tb = (target >> 10) & 0x1F;
                uint8_t cr = (uint8_t)((tr * (frame + 1)) / 60);
                uint8_t cg = (uint8_t)((tg * (frame + 1)) / 60);
                uint8_t cb = (uint8_t)((tb * (frame + 1)) / 60);
                ert.palettes[c] = (uint16_t)cr | ((uint16_t)cg << 5) |
                              ((uint16_t)cb << 10);
            }
            ert.palette_upload_mode = PALETTE_UPLOAD_FULL;

            /* Assembly: UPDATE_MAP_PALETTE_ANIMATION then RENDER_FRAME_TICK */
            update_map_palette_animation();
            render_frame_tick();
            if (platform_input_quit_requested()) {
                return 1;
            }
        }
    } else {
        /* Quick mode (asm @QUICK_MODE): NMI-driven brightness fade runs
         * concurrently with a 60-frame RENDER_FRAME_TICK warm-up loop.
         * Entity scripts load the correct palette via LOAD_FILE_SELECT_PALETTES
         * during this warm-up.  The brightness fade (starting from force blank)
         * keeps the screen dark until entity scripts have loaded the palette. */

        /* ROM: LDX #1 / LDA #4 / JSL FADE_IN
         * On the SNES, FADE_IN is NMI-driven so it runs concurrently with
         * the 60-frame RENDER_FRAME_TICK loop below. We merge both into
         * a single loop to match this behavior. */
        fade_in(4, 1);

        /* ROM: 60-frame RENDER_FRAME_TICK loop (@QUICK_WARMUP_LOOP) */
        for (int frame = 0; frame < 60; frame++) {
            if (fade_active()) fade_update();
            render_frame_tick();
            if (platform_input_quit_requested()) {
                return 1;
            }
        }
    }

    /* Main loop — faithful port of @INPUT_LOOP / @CHECK_ACTIONSCRIPT.
     *
     * ACTIONSCRIPT_STATE transitions:
     *   0 → entities still running (keep looping)
     *   2 → paused / palette done (keep looping, accept input)
     *   1 → EVENT_YIELD_TO_TEXT fired, all pauses complete (exit → attract mode)
     *
     * EVENT_788 fires EVENT_YIELD_TO_TEXT after ~765 frames of EVENT_PAUSEs,
     * which sets ert.actionscript_state = 1 via SET_ACTIONSCRIPT_STATE_RUNNING.
     * That's the signal to exit to attract mode (~12.75 seconds idle).
     *
     * Buttons are checked every frame (assembly checks PAD_PRESS unconditionally).
     */
    uint16_t result = 0;
    /* Note: ert.actionscript_state was zeroed by entity_system_init() and may have
     * been set to 2 (paused) by entity scripts during the warmup loop above.
     * The ROM does NOT re-zero it here — @MAIN_LOOP_INIT only clears @VIRTUAL02.
     * We must NOT reset ert.actionscript_state here or we'd lose valid state. */

    /* Jump to @CHECK_ACTIONSCRIPT first (assembly: BRA @CHECK_ACTIONSCRIPT) */
    goto check_actionscript;

input_loop:
    if (platform_input_quit_requested()) {
        return 1;
    }

    /* ROM: @INPUT_LOOP — check buttons every frame */
    {
        uint16_t pressed = platform_input_get_pad_new();
        if (pressed & PAD_ANY_BUTTON) {
            result = 1;
            goto post_input_loop;
        }
    }

    /* ROM: @NO_BUTTON — run one frame */
    render_frame_tick();

check_actionscript:
    /* ROM: @CHECK_ACTIONSCRIPT
     * State 0 or 2 → keep looping.  State 1 → exit to attract mode. */
    if (ert.actionscript_state == 0)
        goto input_loop;
    if (ert.actionscript_state == 2)
        goto input_loop;

post_input_loop:
    /* ROM: @POST_INPUT_LOOP → @FADE_OUT
     * (quick_mode and state!=0 both branch to @FADE_OUT;
     *  the RUN_TITLE_SEQUENCE path is only reachable from the overworld
     *  call path which we don't use here.) */

    /* FADE_OUT_WITH_MOSAIC(1, 4, 0) */
    for (uint8_t b = 0x0F; b > 0; b--) {
        ppu.inidisp = b;
        for (int d = 0; d < 4; d++)
            wait_for_vblank();
    }
    ppu.inidisp = 0x80;

    /* Clear viewport extension and sprite offset before leaving */
    ppu.bg_viewport_fill[0] = BG_VIEWPORT_CENTER;
    ppu.sprite_x_offset = 0;

    /* ROM: STZ ACTIONSCRIPT_STATE / JSL SETUP_ENTITY_COLOR_MATH / JSL INIT_ENTITY_SYSTEM */
    ert.actionscript_state = 0;
    setup_entity_color_math();
    entity_system_init();

    return result;
}
