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
#include <string.h>

/* Forward declarations from main.c */
#include "game_main.h"


/* MAP_PALETTE_BACKUP — saves battle BG palette during palette fade.
 * The ROM saves/restores palette group 2 (colors 32-47) around
 * UPDATE_MAP_PALETTE_ANIMATION to prevent the fade from touching
 * the battle BG's separately-managed palette. */
static uint16_t map_palette_backup[16];

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
    sync_palettes_to_cgram();

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

    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
}

/*
 * Port of GAS_STATION from asm/intro/gas_station.asm
 * + RUN_GAS_STATION_CREDITS from asm/overworld/run_gas_station_credits.asm.
 *
 * Returns: 0 = timed out, 1 = button pressed
 */
uint16_t gas_station(void) {
    /* ROM: JSL INIT_ENTITY_SYSTEM */
    entity_system_init();

    gas_station_load();

    /*
     * NMI-driven brightness fade.
     * ROM: FADE_IN stores step=1, delay=11.
     * Brightness goes: $80 -> $01 -> $02 -> ... -> $0F over ~180 frames.
     */
    int fade_delay_left = 11;
    bool brightness_fading = true;

    /*
     * Phase 1: 236 frames.
     * ROM: UPDATE_BATTLE_SCREEN_EFFECTS + WAIT_UNTIL_NEXT_FRAME.
     * Battle BG animates (palette cycling), brightness fades in.
     * CGRAM groups 0-1 still all-zero so gas station image is invisible.
     */
    for (int i = 0; i < 236; i++) {
        if (platform_input_quit_requested()) return 1;
        if (platform_input_get_pad_new()) return 1;

        /* UPDATE_BATTLE_SCREEN_EFFECTS — just the battle BG animation part */
        battle_bg_update();

        /* Process NMI brightness fade */
        if (brightness_fading) {
            fade_delay_left--;
            if (fade_delay_left < 0) {
                fade_delay_left = 11;
                uint8_t brightness = ppu.inidisp & 0x0F;
                int next = brightness + 1;
                if (next >= 0x10) {
                    ppu.inidisp = 0x0F;
                    brightness_fading = false;
                } else {
                    ppu.inidisp = (uint8_t)next;
                }
            }
        }

        sync_palettes_to_cgram();
        wait_for_vblank();
    }

    /*
     * Phase 2: 480 frames — palette interpolation + battle BG animation.
     *
     * ROM loop (RUN_GAS_STATION_CREDITS @UNKNOWN3):
     *   1. Save palette group 2 → MAP_PALETTE_BACKUP
     *   2. UPDATE_MAP_PALETTE_ANIMATION (advances all 256 palette entries)
     *   3. Clear PALETTE_UPLOAD_MODE (prevent mid-loop upload)
     *   4. COPY_BG_PALETTE_FROM_POINTER (restore LOADED_BG_DATA palette from PALETTES)
     *   5. Restore palette group 2 from MAP_PALETTE_BACKUP
     *   6. UPDATE_BATTLE_SCREEN_EFFECTS (battle BG animation + palette cycling)
     *   7. Set PALETTE_UPLOAD_MODE = FULL
     *   8. WAIT_UNTIL_NEXT_FRAME
     *
     * The save/restore of group 2 prevents UPDATE_MAP_PALETTE_ANIMATION from
     * interfering with the battle BG's own palette cycling.
     */
    for (int i = 0; i < 480; i++) {
        if (platform_input_quit_requested()) return 1;
        if (platform_input_get_pad_new()) return 1;

        /* Save battle BG palette (group 2, colors 32-47) */
        memcpy(map_palette_backup, &ert.palettes[32], sizeof(map_palette_backup));

        /* Advance palette fade (gas station fades in, battle BG fades out) */
        update_map_palette_animation();

        /* COPY_BG_PALETTE_FROM_POINTER (asm/battle/effects/copy_bg_palette_from_pointer.asm):
         * Copy the now-faded palette from ert.palettes[] into loaded_bg_data_layer1.palette
         * so battle_bg_update() palette cycling uses the gradually-dimming palette. */
        memcpy(loaded_bg_data_layer1.palette,
               &ert.palettes[loaded_bg_data_layer1.palette_index],
               sizeof(loaded_bg_data_layer1.palette));

        /* Restore battle BG palette — undo what UPDATE_MAP_PALETTE_ANIMATION did */
        memcpy(&ert.palettes[32], map_palette_backup, sizeof(map_palette_backup));

        /* UPDATE_BATTLE_SCREEN_EFFECTS — animates battle BG (palette cycling) */
        battle_bg_update();

        sync_palettes_to_cgram();
        wait_for_vblank();
    }

    /*
     * FINALIZE_PALETTE_FADE: copy fade target (ert.buffer[0..511]) to ert.palettes[].
     * Then disable color math (gas station fully visible, no more BG2 overlay).
     *
     * ROM: CGADSUB=0, CGWSEL=0, TM=BG1, TS=0
     */
    PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
    memcpy(ert.palettes, fade->target, sizeof(fade->target));
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    sync_palettes_to_cgram();

    ppu.cgadsub = 0x00;
    ppu.cgwsel = 0x00;
    ppu.tm = 0x01;
    ppu.ts = 0x00;

    /*
     * Phase 3: Wait 120 frames (gas station visible at full brightness).
     * ROM: WAIT_FRAMES_OR_UNTIL_PRESSED(120)
     */
    for (int i = 0; i < 120; i++) {
        if (platform_input_quit_requested()) return 1;
        if (platform_input_get_pad_new()) return 1;
        wait_for_vblank();
    }

    /*
     * Phase 4: Music change + EVENT_860 flash palette sequence.
     *
     * ROM: CHANGE_MUSIC(GAS_STATION_2=174), INIT_ENTITY_WIPE(EVENT_860).
     * EVENT_860 is an entity script that rapidly alternates between
     * the normal gas station palette and a "flash" palette with
     * accelerating timing, creating a strobe effect.
     */
    change_music(174);  /* MUSIC::GAS_STATION_2 */

    int16_t entity_offset = entity_init_wipe(860);

    /* Run entity script until it finishes.
     * ROM: loop while ENTITY_SCRIPT_TABLE[entity_offset] != -1 */
    while (entity_offset >= 0 &&
           entities.script_table[entity_offset] != ENTITY_NONE) {
        run_actionscript_frame();
        sync_palettes_to_cgram();
        wait_for_vblank();

        if (platform_input_quit_requested()) {
            deactivate_entity(entity_offset);
            return 1;
        }
        if (platform_input_get_pad_new()) {
            deactivate_entity(entity_offset);
            return 1;
        }
    }

    /*
     * Phase 5: 330-frame palette fade to WHITE.
     *
     * ROM: PREPARE_PALETTE_FADE_FROM_CURRENT(330)
     * This calls LOAD_PALETTE_TO_FADE_BUFFER with level=100,
     * which produces all-white ($7FFF) target for every color.
     * Then PREPARE_PALETTE_FADE(330, $FFFF) sets up the fade slopes.
     */
    {
        /* Set fade target to all-white (0x7FFF) */
        PaletteFadeBuffer *wfade = buf_palette_fade(ert.buffer);
        for (int i = 0; i < BUF_FADE_COLOR_COUNT; i++) {
            wfade->target[i] = 0x7FFF;
        }
        prepare_palette_fade_slopes(330, 0xFFFF);
    }

    for (int i = 0; i < 330; i++) {
        if (platform_input_quit_requested()) return 1;
        if (platform_input_get_pad_new()) return 1;

        update_map_palette_animation();
        sync_palettes_to_cgram();
        wait_for_vblank();
    }

    /*
     * Phase 6: Clear screen and wait.
     * ROM: STZ TM_MIRROR, zeroes all PALETTES, PALETTE_UPLOAD_MODE=FULL,
     * then WAIT_FRAMES_OR_UNTIL_PRESSED(30).
     */
    ppu.tm = 0;
    memset(ert.palettes, 0, sizeof(ert.palettes));
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    sync_palettes_to_cgram();

    wait_frames_or_button(30, 0);

    return 0;
}
