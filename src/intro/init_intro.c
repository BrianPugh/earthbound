#include "intro/init_intro.h"
#include "intro/logo_screen.h"
#include "intro/gas_station.h"
#include "intro/title_screen.h"
#include "intro/attract_mode.h"
#include "intro/file_select.h"
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
#include "core/mode_stack.h"

/* Forward declarations */
#include "game_main.h"

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
     * MUST be before the file menu so music changes work during file select. */
    ow.disable_music_changes = 0;
}

/* Attract-mode scene table (init_intro.asm states 3-10): scenes run back-to-back
 * (no yield between), looping back to the title screen after the last. */
static const uint16_t init_intro_attract_scenes[] = { 0, 2, 3, 4, 5, 6, 7, 9 };
#define INIT_INTRO_ATTRACT_COUNT \
    ((int)(sizeof(init_intro_attract_scenes) / sizeof(init_intro_attract_scenes[0])))

/* Initial state for a pushed intro child (INTRO_LOGO / GAS_STATION / TITLE_SCREEN).
 * Must outlive the dispatch turn (STEP_RESULT_PUSH_INIT copies it immediately);
 * only one child is ever pending at a time. */
static ModeState ii_child_init;

/*
 * GAME_MODE_INIT_INTRO step — run-to-completion port of INIT_INTRO's state
 * machine (asm/intro/init_intro.asm, US retail path). See InitIntroState in
 * mode_stack.h. Every child stage — the intro leaves (logos, gas station, title
 * screen), the attract scenes (GAME_MODE_ATTRACT), and the file menu
 * (GAME_MODE_FILE_MENU) — is STEP_PUSHed as its own mode after its yield-free
 * setup runs inline; the result each hands back (button skip vs normal
 * completion) is read via mode_child_result() in the matching *_RESULT phase.
 * The intro is now STEP_PUSH end-to-end. The brief fade_out_if_visible() /
 * exit-cleanup transitions still run inline (they yield via direct
 * wait_for_vblank, only on input-driven skip paths). The pump owns the yield.
 */
StepResult mode_step_init_intro(ModeState *ms) {
    InitIntroState *st = &ms->init_intro;

    for (;;) {
        switch ((InitIntroPhase)st->phase) {
        case II_LOGO:
            /* Logo screens (Nintendo, APE, HAL). No yield-free setup needed. */
            ii_child_init = (ModeState){0};
            ii_child_init.intro_logo.phase    = LG_LOAD;
            ii_child_init.intro_logo.logo_idx = 0;
            st->phase = II_LOGO_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_INTRO_LOGO, &ii_child_init);

        case II_LOGO_RESULT:
            if (mode_child_result() != 0) {
                /* Button during logos — WRITE_APU_PORT1(2) + FADE_OUT_WITH_MOSAIC
                 * then skip directly to the title (quick mode). asm:78-97. */
                write_apu_port1(2);
                fade_out_if_visible();
                st->title_quick_mode = 1;
                st->phase = II_TITLE;
            } else {
                st->phase = II_GAS;
            }
            continue;

        case II_GAS:
            /* Gas station prologue. */
            change_music(1); /* MUSIC::GAS_STATION */
            gas_station_setup();
            ii_child_init = (ModeState){0};
            ii_child_init.gas_station.phase            = GS_PH1;
            ii_child_init.gas_station.fade_delay_left  = 11;
            ii_child_init.gas_station.brightness_fading = 1;
            ii_child_init.gas_station.remaining        = 236;
            st->phase = II_GAS_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_GAS_STATION, &ii_child_init);

        case II_GAS_RESULT:
            if (mode_child_result() != 0) {
                /* Button during gas station — WRITE_APU_PORT1(2) +
                 * FADE_OUT_WITH_MOSAIC + PPU cleanup, skip to title (quick).
                 * asm:110-138. */
                write_apu_port1(2);
                fade_out_if_visible();
                ppu.cgadsub = 0;
                ppu.cgwsel = 0;
                ppu.tm = 1;
                ppu.ts = 0;
                st->title_quick_mode = 1;
            }
            st->phase = II_TITLE;
            continue;

        case II_TITLE:
            /* Title screen. */
            change_music(175); /* MUSIC::TITLE_SCREEN */
            title_screen_setup(st->title_quick_mode);
            ii_child_init = (ModeState){0};
            ii_child_init.title_screen.phase      = TS_WARMUP;
            ii_child_init.title_screen.quick_mode = st->title_quick_mode;
            st->title_quick_mode = 0;   /* consumed (asm clears title_quick_mode) */
            st->phase = II_TITLE_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_TITLE_SCREEN, &ii_child_init);

        case II_TITLE_RESULT:
            if (mode_child_result() != 0) {
                /* Button — go to file select. */
                st->phase = II_FILE_MENU;
            } else {
                /* Timed out — start the attract sequence. */
                st->attract_index = 0;
                st->phase = II_ATTRACT;
            }
            continue;

        case II_ATTRACT: {
            /* Attract scenes run back-to-back. The yield-free per-scene setup runs
             * inline (run_attract_mode_prepare); GAME_MODE_ATTRACT then drives the
             * scene (its AT_SCRIPT phase) and pops 1 if a button was pressed during
             * ANY scene -> exit directly to file select (asm @UNKNOWN27,
             * init_intro.asm:212-247). */
            if (st->attract_index == 0)
                change_music(157); /* MUSIC::ATTRACT_MODE (first scene only) */
            uint16_t scene = init_intro_attract_scenes[st->attract_index];
            run_attract_mode_prepare(scene);
            ii_child_init = (ModeState){0};
            ii_child_init.attract.phase       = AT_SCRIPT;
            ii_child_init.attract.scene_index = scene;
            st->phase = II_ATTRACT_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_ATTRACT, &ii_child_init);
        }

        case II_ATTRACT_RESULT:
            if (mode_child_result() != 0) {
                /* Button during a scene — exit to file select. */
                st->phase = II_FILE_MENU;
            } else {
                st->attract_index++;
                if (st->attract_index >= INIT_INTRO_ATTRACT_COUNT) {
                    /* After all attract scenes, loop back to the title screen. */
                    st->title_quick_mode = 0;
                    st->phase = II_TITLE;
                } else {
                    /* Run the next scene immediately. */
                    st->phase = II_ATTRACT;
                }
            }
            continue;

        case II_FILE_MENU:
            /* Exit cleanup + file select. RUN_FILE_MENU (run_file_menu.asm).
             * The yield-free file-menu setup runs inline (file_menu_setup); the
             * cascade runs as GAME_MODE_FILE_MENU. */
            init_intro_exit_cleanup();
            change_music(3); /* MUSIC::SETUP_SCREEN */
            file_menu_setup();
            ii_child_init = (ModeState){0};
            ii_child_init.file_menu.phase = FM_FADEIN_WAIT;
            st->phase = II_FILE_MENU_POST;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_FILE_MENU, &ii_child_init);

        case II_FILE_MENU_POST:
            /* RUN_FILE_MENU post-cleanup (run_file_menu.asm:13-17). The single
             * window_tick() becomes window_tick_work() + the CONTINUE yield. */
            clear_instant_printing();
            window_tick_work();
            st->phase = II_FILE_MENU_DONE;
            return STEP_RESULT_CONTINUE();

        case II_FILE_MENU_DONE:
            /* Critical: ow.disabled_transitions must be cleared or the overworld
             * text/palette/pajama systems won't function. event_script_data is
             * NOT freed — it's needed by the main game loop. */
            ow.disabled_transitions = 0;
            free_title_screen_script_data();
            return STEP_RESULT_POP(0);
        }

        /* Unreachable: every phase returns or continues. */
        return STEP_RESULT_POP(0);
    }
}

/*
 * Port of INIT_INTRO from asm/intro/init_intro.asm (US retail path) — one-shot
 * boot setup that ends by PUSHing GAME_MODE_INIT_INTRO onto the mode stack.
 *
 * The one-shot initialization (asm:20-42, including the two startup vblanks)
 * runs here, then the state machine that cycles logos -> gas station -> title ->
 * attract (and exits to the file select on a button press) is left on the stack
 * for the root loop to drive one step per frame (game_main.c's LOOP_BOOT). This
 * no longer pump_mode()s the intro to completion itself, so a savestate taken
 * mid-intro lands on serializable mode-stack state rather than a blocked C stack.
 */
void init_intro(void) {
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

    ModeState init = {0};
    init.init_intro.phase = II_LOGO;
    mode_push(GAME_MODE_INIT_INTRO, &init);
}
