/*
 * Port of GAS_STATION (asm/intro/gas_station.asm),
 * GAS_STATION_LOAD (asm/intro/gas_station_load.asm),
 * and RUN_GAS_STATION_CREDITS (asm/overworld/run_gas_station_credits.asm).
 *
 * ROM flow:
 *   1. GAS_STATION_LOAD — load BG1 graphics + BG2 battle BG 295
 *   2. FADE_IN (step=1, delay=11) — NMI-driven brightness fade
 *   3. RUN_GAS_STATION_CREDITS:
 *      Phase 1: 236 frames (battle BG effects + brightness fade)
 *      Phase 2: 480 frames (palette interpolation + battle BG animation)
 *      Phase 3: FINALIZE_PALETTE_FADE, disable color math, wait 120 frames
 *      Phase 4: Music change + EVENT_860 flash palette sequence
 *      Phase 5: PREPARE_PALETTE_FADE_FROM_CURRENT(330) — fade to WHITE
 *   4. 330-frame loop with UPDATE_MAP_PALETTE_ANIMATION
 *   5. Clear ert.palettes, wait 30 frames
 */
#include "intro/gas_station.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "data/event_script_data.h"
#include "game/battle_bg.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "core/decomp.h"
#include "core/memory.h"
#include "game/fade.h"
#include "game/audio.h"
#include "data/assets.h"
#include "include/constants.h"
#include "platform/platform.h"
#include "core/mode_stack.h"
#include <string.h>

/* Forward declarations from main.c */
#include "game_main.h"


/* MAP_PALETTE_BACKUP — saves battle BG palette during palette fade.
 * The ROM saves/restores palette group 2 (colors 32-47) around
 * UPDATE_MAP_PALETTE_ANIMATION to prevent the fade from touching
 * the battle BG's separately-managed palette. */
static uint16_t map_palette_backup[16];

/*
 * Sync palettes to CGRAM, then force the screen backdrop (cgram[0]) black.
 *
 * The gas station never shows cgram[0] as visible content — BG1 covers the
 * centered SNES area opaquely. In a wide viewport, though, the letterbox
 * gutters inherit cgram[0], which the palette fade drives from black up to
 * the gas-station background colour, tinting the gutters. Pinning cgram[0]
 * to black keeps the gutters a clean black letterbox without the global
 * compositor needing a gutter-forcing special case. Self-heals on scene
 * exit: the next scene's palette sync overwrites cgram[0].
 */
static void gas_station_sync_palettes(void) {
    sync_palettes_to_cgram();
    ppu.cgram[0] = 0;
}

/*
 * Port of GAS_STATION_LOAD from asm/intro/gas_station_load.asm.
 *
 * Sets up BG1 (gas station image) and BG2 (battle BG 295 noise effect):
 * - BG1 graphics -> VRAM $0000, tilemap -> VRAM $7800
 * - BG2 battle BG 295 -> VRAM $6000/$7C00 (via battle_bg_load_at)
 * - Palette groups 0-1: gas station palette → fade target (BUFFER)
 * - Palette group 2: battle BG palette → fade from current to black
 * - Color math: CGWSEL=$02, CGADSUB=$03 (add sub-screen to main)
 */
static void gas_station_load(void) {
    /* Reset scroll positions */
    ppu.bg_hofs[0] = 0; ppu.bg_vofs[0] = 0;
    ppu.bg_hofs[1] = 0; ppu.bg_vofs[1] = 0;

    size_t comp_size;
    const uint8_t *comp_data;

    /* Graphics -> VRAM $0000
       ROM: COPY_TO_VRAM1P BUFFER, $0000, $C000, 0
       Decompress directly to VRAM — no intermediate buffer needed. */
    comp_size = ASSET_SIZE(ASSET_INTRO_GAS_STATION_GFX_LZHAL);
    comp_data = ASSET_DATA(ASSET_INTRO_GAS_STATION_GFX_LZHAL);
    if (comp_data) {
        uint8_t *vram_dst = &ppu.vram[VRAM_GAS_STATION_L1_TILES * 2];
        memset(vram_dst, 0, 0xC000);
        decomp(comp_data, comp_size, vram_dst, 0xC000);
    }

    /* Arrangement -> VRAM $7800
       ROM: COPY_TO_VRAM1P BUFFER, $7800, $800, 0 */
    comp_size = ASSET_SIZE(ASSET_INTRO_GAS_STATION_ARR_LZHAL);
    comp_data = ASSET_DATA(ASSET_INTRO_GAS_STATION_ARR_LZHAL);
    if (comp_data) {
        uint8_t *vram_dst = &ppu.vram[VRAM_GAS_STATION_L1_TILEMAP * 2];
        decomp(comp_data, comp_size, vram_dst, 0x800);
    }

    /* Palette -> PALETTES (groups 0-1 = gas station image palette)
       ROM: DECOMP GAS_STATION_PALETTE → PALETTES */
    comp_size = ASSET_SIZE(ASSET_INTRO_GAS_STATION_PAL_LZHAL);
    comp_data = ASSET_DATA(ASSET_INTRO_GAS_STATION_PAL_LZHAL);
    if (comp_data) {
        decomp(comp_data, comp_size, (uint8_t *)ert.palettes, sizeof(ert.palettes));
    }

    /* Set BG mode 3, configure BG1 and BG2 VRAM locations.
       ROM: SET_BGMODE(3), SET_BG1_VRAM_LOCATION, SET_BG2_VRAM_LOCATION
       (done inside SETUP_GAS_STATION_BACKGROUND) */
    ppu.bgmode = 0x03;
    ppu.bg_sc[0] = (uint8_t)(VRAM_GAS_STATION_L1_TILEMAP >> 8);
    ppu.bg_nba[0] = 0x00;

    /* Load battle BG 295 to BG2 (VRAM $6000/$7C00, palette group 2).
       This is the port of SETUP_GAS_STATION_BACKGROUND. It loads graphics, arrangement,
       and palette for the noise/static effect. */
    battle_bg_load_at(BATTLEBG_GAS_STATION,
                      VRAM_GAS_STATION_L2_TILES,
                      VRAM_GAS_STATION_L2_TILEMAP,
                      32);  /* CGRAM color 32 = palette group 2 */

    /* battle_bg_load_at already wrote the BG295 palette to both
       ppu.cgram[32..47] and ert.palettes[32..47], plus called
       generate_battlebg_frame for the initial frame (step=0 = no-op).
       No additional copy is needed here — matches assembly flow where
       SETUP_GAS_STATION_BACKGROUND sets PALETTES directly. */

    /* ROM: COPY_FADE_BUFFER_TO_PALETTES — save PALETTES → BUFFER as fade target.
       At this point:
         ert.palettes[0..31] = gas station colors
         ert.palettes[32..47] = battle BG 295 palette (set by battle_bg_load_at)
         ert.palettes[48..255] = 0 */
    copy_fade_buffer_to_palettes();

    /* Zero the battle BG palette in BUFFER (fade target → black for group 2).
       ROM: MEMSET24 BUFFER+BPP4PALETTE_SIZE*2, 0, BPP4PALETTE_SIZE */
    memset(ert.buffer + BPP4PALETTE_SIZE * 2, 0, BPP4PALETTE_SIZE); /* BUF_FADE_TARGET */

    /* Zero PALETTES groups 0-1 (gas station palette starts black).
       ROM: MEMSET16 PALETTES, 0, BPP4PALETTE_SIZE*2 */
    memset(ert.palettes, 0, BPP4PALETTE_SIZE * 2);

    /* Zero PALETTES groups 3-15.
       ROM: MEMSET16 PALETTES+3*BPP4PALETTE_SIZE, 0, 13*BPP4PALETTE_SIZE */
    memset((uint8_t *)ert.palettes + 3 * BPP4PALETTE_SIZE, 0, 13 * BPP4PALETTE_SIZE);

    /* PREPARE_PALETTE_FADE(480, $FFFF) — fade all palette groups over 480 frames.
       From black → gas station palette (groups 0-1)
       From battle BG palette → black (group 2)
       Groups 3-15: 0 → 0 (no change) */
    prepare_palette_fade_slopes(480, 0xFFFF);

    /* Sync initial (zeroed) ert.palettes to CGRAM */
    gas_station_sync_palettes();

    /* ROM: TM_MIRROR=$01 (BG1 main), TD_MIRROR=$02 (BG2 sub-screen) */
    ppu.tm = 0x01;
    ppu.ts = 0x02;

    /* Color math: add sub-screen (BG2) to main screen (BG1).
     * Assembly: TM=$01, TS=$02, CGWSEL=$02, CGADSUB=$03.
     * BG1 is the only main-screen layer; BG2 is sub-screen only.
     * Color math adds sub-screen (BG2 static) to main-screen (BG1).
     *
     * Using assembly-matching values is critical for correct crossfade:
     * BG1 transparent pixels (palette entry 0) must show the backdrop
     * color (which fades from black to the gas station background),
     * NOT the BG2 static. With BG2 on main screen, transparent BG1
     * areas would show raw static instead of the fading backdrop. */
    ppu.tm = 0x01;
    ppu.cgwsel = 0x02;
    ppu.cgadsub = 0x03;

    /* Mark BG2 (the red Giygas static) as a viewport-filling layer so its
     * 32-tile noise tilemap wraps across the full viewport width and height,
     * putting a sub-screen static pixel in the gutters. The compositor's
     * wide-mode gutter fill then blends that sub layer over the black backdrop
     * gutters (cgram[0] is pinned black by gas_station_sync_palettes()), so the
     * static covers the entire screen, not just the centered 256x224 band. BG1
     * (the gas station image) stays centered. In a native 256x224 viewport
     * these flags are inert. Matches the file-select starfield. */
    ppu.bg_viewport_fill[0] = BG_VIEWPORT_CENTER;
    ppu.bg_viewport_fill[1] = BG_VIEWPORT_FILL;

    /* Vertically center the SNES-space content in the (taller) viewport while
     * BG2's fill keeps wide mode active. Without this the wide-mode path uses
     * a stale sprite_y_offset (top-aligned), then the image jumps down by
     * EB_VIEWPORT_PAD_TOP when the static is removed and rendering falls back
     * to the centered non-wide path. Matches file_select.c / battle_ui.c. */
    ppu.sprite_y_offset = EB_VIEWPORT_PAD_TOP;

    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
}

/* ---- GAME_MODE_GAS_STATION ------------------------------------------------
 * Run-to-completion port of the blocking gas_station() below. See the
 * GasStationState comment in mode_stack.h. The single yield is owned by the
 * pump; this body never calls wait_for_vblank(). Each phase does its frame's
 * work then decrements `remaining`; on the frame `remaining` reaches 0 it runs
 * the (yield-free) transition into the next phase, so frame counts match the
 * blocking loops. Every phase but GS_PH6 pops 1 on any button (post-yield read,
 * checked at the top of the step — the same <=1-frame placement the other
 * conversions accept). */
StepResult mode_step_gas_station(ModeState *st) {
    GasStationState *s = &st->gas_station;

    switch ((GasStationPhase)s->phase) {
    case GS_PH1:
        /* 236-frame red-static intro with the NMI brightness fade-in. */
        if (platform_input_get_pad_new())
            return STEP_RESULT_POP(1);
        battle_bg_update();
        if (s->brightness_fading) {
            if (--s->fade_delay_left < 0) {
                s->fade_delay_left = 11;
                uint8_t brightness = ppu.inidisp & 0x0F;
                int next = brightness + 1;
                if (next >= 0x10) {
                    ppu.inidisp = 0x0F;
                    s->brightness_fading = 0;
                } else {
                    ppu.inidisp = (uint8_t)next;
                }
            }
        }
        gas_station_sync_palettes();
        if (--s->remaining == 0) {
            s->phase = GS_PH2;
            s->remaining = 480;
        }
        return STEP_RESULT_CONTINUE();

    case GS_PH2:
        /* 480-frame palette interpolation + battle BG animation. The group-2
         * save/restore keeps UPDATE_MAP_PALETTE_ANIMATION from disturbing the
         * battle BG's own palette cycling. */
        if (platform_input_get_pad_new())
            return STEP_RESULT_POP(1);
        memcpy(map_palette_backup, &ert.palettes[32], sizeof(map_palette_backup));
        update_map_palette_animation();
        memcpy(loaded_bg_data_layer1.palette,
               &ert.palettes[loaded_bg_data_layer1.palette_index],
               sizeof(loaded_bg_data_layer1.palette));
        memcpy(&ert.palettes[32], map_palette_backup, sizeof(map_palette_backup));
        battle_bg_update();
        gas_station_sync_palettes();
        if (--s->remaining == 0) {
            /* FINALIZE_PALETTE_FADE + disable color math (yield-free). */
            PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
            memcpy(ert.palettes, fade->target, sizeof(fade->target));
            ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
            gas_station_sync_palettes();
            ppu.cgadsub = 0x00;
            ppu.cgwsel = 0x00;
            ppu.tm = 0x01;
            /* Keep BG2 in TS to hold wide mode (see the blocking notes below). */
            ppu.ts = 0x02;
            s->phase = GS_PH3;
            s->remaining = 120;
        }
        return STEP_RESULT_CONTINUE();

    case GS_PH3:
        /* 120-frame hold at full brightness (no per-frame work). */
        if (platform_input_get_pad_new())
            return STEP_RESULT_POP(1);
        if (--s->remaining == 0) {
            /* Music change + EVENT_860 flash entity (yield-free). */
            change_music(174); /* MUSIC::GAS_STATION_2 */
            s->entity_offset = entity_init_wipe(860);
            s->phase = GS_PH4;
        }
        return STEP_RESULT_CONTINUE();

    case GS_PH4:
        /* Run EVENT_860 until its script clears. A button skip deactivates the
         * entity first (the blocking post-yield check). */
        if (platform_input_get_pad_new()) {
            deactivate_entity(s->entity_offset);
            return STEP_RESULT_POP(1);
        }
        if (!(s->entity_offset >= 0 &&
              entities.script_table[s->entity_offset] != ENTITY_NONE)) {
            /* Script finished: set up the 330-frame fade to white (yield-free).
             * Reset backdrop index 0 to black so the wide-mode gutters fade
             * cleanly from black to white. */
            ert.palettes[0] = 0;
            PaletteFadeBuffer *wfade = buf_palette_fade(ert.buffer);
            for (int i = 0; i < BUF_FADE_COLOR_COUNT; i++)
                wfade->target[i] = 0x7FFF;
            prepare_palette_fade_slopes(330, 0xFFFF);
            s->phase = GS_PH5;
            s->remaining = 330;
            return STEP_RESULT_CONTINUE();
        }
        run_actionscript_frame();
        gas_station_sync_palettes();
        return STEP_RESULT_CONTINUE();

    case GS_PH5:
        /* 330-frame fade to white. Plain sync (not gas_station_sync_palettes)
         * so the backdrop cgram[0] whitens along with the gutters. */
        if (platform_input_get_pad_new())
            return STEP_RESULT_POP(1);
        update_map_palette_animation();
        sync_palettes_to_cgram();
        if (--s->remaining == 0) {
            /* Clear the screen + palettes, restore BG2 centering (yield-free). */
            ppu.tm = 0;
            memset(ert.palettes, 0, sizeof(ert.palettes));
            ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
            sync_palettes_to_cgram();
            ppu.bg_viewport_fill[1] = BG_VIEWPORT_CENTER;
            ppu.sprite_y_offset = 0;
            ppu.ts = 0x00;
            s->phase = GS_PH6;
            s->remaining = 30;
        }
        return STEP_RESULT_CONTINUE();

    case GS_PH6:
        /* 30-frame final wait — NOT button-skippable (WAIT_FRAMES_OR_UNTIL_
         * PRESSED(30) with mask 0). Check-at-top so it yields exactly 30 frames
         * before the terminal pop (which does not yield). */
        if (s->remaining == 0)
            return STEP_RESULT_POP(0);
        s->remaining--;
        return STEP_RESULT_CONTINUE();
    }
    return STEP_RESULT_POP(0);
}

/*
 * Port of GAS_STATION from asm/intro/gas_station.asm
 * + RUN_GAS_STATION_CREDITS from asm/overworld/run_gas_station_credits.asm.
 *
 * Returns: 0 = timed out, 1 = button pressed
 */
void gas_station_setup(void) {
    /* One-shot setup (yield-free): ROM JSL INIT_ENTITY_SYSTEM + GAS_STATION_LOAD. */
    entity_system_init();
    gas_station_load();
}

/* The blocking gas_station() pump bridge was deleted (D4b); init_intro
 * (GAME_MODE_INIT_INTRO) runs gas_station_setup() then STEP_PUSHes
 * GAME_MODE_GAS_STATION (phase GS_PH1, fade_delay_left 11, brightness_fading 1,
 * remaining 236) directly. */
