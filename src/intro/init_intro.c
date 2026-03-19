#include "intro/init_intro.h"
#include "intro/logo_screen.h"
#include "intro/gas_station.h"
#include "intro/title_screen.h"
#include "intro/attract_mode.h"
#include "data/event_script_data.h"
#include "snes/ppu.h"
#include "core/memory.h"
#include "entity/entity.h"
#include "game/audio.h"
#include "game/fade.h"
#include "game/game_state.h"
#include "game/overworld.h"
#include "game/battle.h"
#include "game/display_text.h"
#include "game/text.h"
#include "platform/platform.h"

/* Forward declarations */
#include "game_main.h"

/* Forward declaration for file select (Phase 7) */
extern uint16_t file_menu_loop(void);

/* Blocking fade-out if the screen is currently visible.
 * Port of FADE_OUT_WITH_MOSAIC(step=4, delay=1, mosaic=0) used in the
 * exit cleanup at init_intro.asm:233-236 and the skip-to-title paths
 * at lines 86-87 and 118-121. */
static void fade_out_if_visible(void) {
    if (ppu.inidisp & 0x80) return; /* already force-blanked */
    while (1) {
        int16_t next = (int16_t)(ppu.inidisp & 0x0F) - 4;
        if (next < 0) break;
        ppu.inidisp = (ppu.inidisp & 0xF0) | (uint8_t)next;
        wait_for_vblank();
    }
    ppu.inidisp = 0x80;
}

/* Exit cleanup from INIT_INTRO.
 * Port of init_intro.asm lines 222-247 (@UNKNOWN27 exit path).
 * Called when any state returns non-zero (button pressed). */
static void init_intro_exit_cleanup(void) {
    write_apu_port1(2);
    fade_out_if_visible();
    /* Assembly lines 238-244: clear color math, enable BG1 only */
    ppu.cgadsub = 0;
    ppu.cgwsel = 0;
    ppu.tm = 1;  /* BG1 only */
    ppu.ts = 0;
    /* Assembly line 246: re-enable music changes for normal gameplay.
     * MUST be before file_menu_loop so music changes work during file select. */
    ow.disable_music_changes = 0;
}

/*
 * Port of INIT_INTRO from asm/intro/init_intro.asm (US retail path).
 *
 * The original is a state machine that cycles through:
 *   State 0: Logo screens (Nintendo, APE, HAL)
 *   State 1: Gas station prologue
 *   State 2: Title screen
 *   State 3+: Attract mode demos (loop back to state 2)
 *
 * When the user presses Start/A/B on the title screen OR during attract
 * mode, we proceed to the file select menu. Pressing during logos or gas
 * station skips to the title screen (quick mode). From file select,
 * loading or starting a new game exits the intro.
 */
void init_intro(void) {
    uint16_t state = 0;
    uint16_t result;
    uint16_t title_quick_mode = 0;

    /* Assembly lines 20-26: initialization before state machine */
    ow.disabled_transitions = 1;
    write_apu_port1(2);
    entity_system_init();
    initialize_battle_ui_state();
    clear_all_status_effects();

    /* Assembly line 26-27: LDA #1 / STA DISABLE_MUSIC_CHANGES.
     * Prevents subsystems (e.g. GET_ON_BICYCLE) from changing music
     * during the entire intro/title/attract sequence. */
    ow.disable_music_changes = 1;

    /* Load fonts early so the debug FPS overlay works during intro screens. */
    text_system_init();

    /* Initialize scrolling positions to zero (assembly lines 29-42).
     * Assembly zeroes all BG scroll positions and calls UPDATE_SCREEN twice. */
    ppu.bg_hofs[0] = 0; ppu.bg_vofs[0] = 0;
    ppu.bg_hofs[1] = 0; ppu.bg_vofs[1] = 0;
    ppu.bg_hofs[2] = 0; ppu.bg_vofs[2] = 0;
    wait_for_vblank();
    ppu.bg_hofs[0] = 0; ppu.bg_vofs[0] = 0;
    ppu.bg_hofs[1] = 0; ppu.bg_vofs[1] = 0;
    ppu.bg_hofs[2] = 0; ppu.bg_vofs[2] = 0;
    wait_for_vblank();

    /* Load script data early — needed by gas station (EVENT_860)
     * and title screen (EVENT_BATTLE_FX through TITLE_SCREEN_11, 787-798). */
    load_title_screen_script_data();
    load_event_script_data();

    while (!platform_input_quit_requested()) {
        switch (state) {
        case 0:
            /* Logo screens (Nintendo, APE, HAL) */
            result = logo_screen();
            if (result != 0) {
                /* Button pressed during logos — assembly does
                 * WRITE_APU_PORT1(2) + FADE_OUT_WITH_MOSAIC(4,1,0)
                 * then skips directly to title screen (quick mode).
                 * init_intro.asm lines 78-97. */
                write_apu_port1(2);
                fade_out_if_visible();
                title_quick_mode = 1;
                state = 2;
            } else {
                state = 1;
            }
            break;

        case 1:
            /* Gas station prologue */
            change_music(1); /* MUSIC::GAS_STATION */
            result = gas_station();
            if (result != 0) {
                /* Button pressed during gas station — assembly does
                 * WRITE_APU_PORT1(2) + FADE_OUT_WITH_MOSAIC(4,1,0)
                 * + PPU cleanup, then skips to title screen (quick mode).
                 * init_intro.asm lines 110-138. */
                write_apu_port1(2);
                fade_out_if_visible();
                ppu.cgadsub = 0;
                ppu.cgwsel = 0;
                ppu.tm = 1;
                ppu.ts = 0;
                title_quick_mode = 1;
            }
            state = 2;
            break;

        case 2:
            /* Title screen */
            change_music(175); /* MUSIC::TITLE_SCREEN */
            result = show_title_screen(title_quick_mode);
            title_quick_mode = 0;

            if (result != 0) {
                /* User pressed button — go to file select.
                 * In the assembly, INIT_INTRO returns here and the caller
                 * runs FILE_SELECT_MENU_LOOP. We embed it. */
                init_intro_exit_cleanup();
                change_music(3); /* MUSIC::SETUP_SCREEN */
                file_menu_loop();
                /* RUN_FILE_MENU post-cleanup (run_file_menu.asm lines 13-17).
                 * The assembly's RUN_FILE_MENU wrapper does this after
                 * FILE_MENU_LOOP returns. Critical: ow.disabled_transitions
                 * must be cleared or the overworld text/palette/pajama
                 * systems won't function correctly. */
                clear_instant_printing();
                window_tick();
                ow.disabled_transitions = 0;
                free_title_screen_script_data();
                /* NOTE: event_script_data is NOT freed here — it's needed by
                 * INITIALIZE_OVERWORLD_STATE and the main game loop. In the
                 * assembly, INIT_INTRO returns to MAIN_LOOP which continues
                 * using the event pointer table for the rest of the game. */
                return;
            } else {
                /* Timed out — start attract mode sequence */
                state = 3;
            }
            break;

        /* Attract mode scenes.
         * In the assembly, button press during ANY attract scene exits
         * INIT_INTRO entirely via @UNKNOWN27 (init_intro.asm:212-247).
         * It does NOT show the title screen first — it goes directly
         * to the exit cleanup. In the C port, we embed the file select. */
        case 3:
            change_music(157); /* MUSIC::ATTRACT_MODE */
            result = run_attract_mode(0);
            if (result != 0) goto exit_to_file_select;
            state = 4;
            break;

        case 4:
            result = run_attract_mode(2);
            if (result != 0) goto exit_to_file_select;
            state = 5;
            break;

        case 5:
            result = run_attract_mode(3);
            if (result != 0) goto exit_to_file_select;
            state = 6;
            break;

        case 6:
            result = run_attract_mode(4);
            if (result != 0) goto exit_to_file_select;
            state = 7;
            break;

        case 7:
            result = run_attract_mode(5);
            if (result != 0) goto exit_to_file_select;
            state = 8;
            break;

        case 8:
            result = run_attract_mode(6);
            if (result != 0) goto exit_to_file_select;
            state = 9;
            break;

        case 9:
            result = run_attract_mode(7);
            if (result != 0) goto exit_to_file_select;
            state = 10;
            break;

        case 10:
            result = run_attract_mode(9);
            if (result != 0) goto exit_to_file_select;
            /* After all attract scenes, loop back to title screen */
            state = 2;
            title_quick_mode = 0;
            break;

        default:
            state = 0;
            break;
        }
    }
    return;

exit_to_file_select:
    /* Assembly @UNKNOWN27 exit path (init_intro.asm:222-247):
     * Button press during attract mode or title screen exits directly
     * to file select — no intermediate title screen. */
    init_intro_exit_cleanup();
    change_music(3); /* MUSIC::SETUP_SCREEN */
    file_menu_loop();
    /* RUN_FILE_MENU post-cleanup (run_file_menu.asm lines 13-17). */
    clear_instant_printing();
    window_tick();
    ow.disabled_transitions = 0;
    free_title_screen_script_data();
    /* NOTE: event_script_data persists — see note above. */
}
