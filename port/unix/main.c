#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "game_main.h"
#include "game/audio.h"
#include "core/log.h"
#include "core/state_dump.h"

/* Unix-specific: set save file path (defined in sdl2_save.c) */
void platform_save_set_path(const char *path);
#include "verify/verify.h"

/* Cleanup handler registered with atexit() — runs on any exit() call. */
static void platform_cleanup(void) {
    platform_timer_shutdown();
    platform_audio_shutdown();
    platform_input_shutdown();
    platform_video_shutdown();
    SDL_Quit();
}

int main(int argc, char *argv[]) {
    const char *verify_rom_path = NULL;
    bool savestate_selftest = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0 && i + 1 < argc) {
            verify_rom_path = argv[++i];
        } else if (strcmp(argv[i], "--selftest-savestate") == 0) {
            /* Run the savestate save->load->save round-trip self-test and exit.
             * Implies headless so it neither opens a window nor plays the game. */
            savestate_selftest = true;
            platform_headless = true;
        } else if (strcmp(argv[i], "--headless") == 0) {
            platform_headless = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            platform_max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            platform_save_set_path(argv[++i]);
        } else if (strcmp(argv[i], "--skip-intro") == 0) {
            platform_skip_intro = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose_level++;
        } else if (argv[i][0] == '-' && argv[i][1] == 'v' && argv[i][1] != '-') {
            /* Count v's: -v = 1, -vv = 2, -vvv = 3 */
            for (const char *p = &argv[i][1]; *p == 'v'; p++)
                verbose_level++;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Usage: %s [--save FILE] [--headless] [--frames N] [--verbose] [--verify ROM] [--selftest-savestate]\n",
                    argv[0]);
            return 1;
        }
    }

    /* Initialize SDL */
    uint32_t sdl_flags = SDL_INIT_TIMER;
    if (!platform_headless) {
        sdl_flags |= SDL_INIT_VIDEO;
#ifdef ENABLE_AUDIO
        sdl_flags |= SDL_INIT_AUDIO;
#endif
    }
    if (SDL_Init(sdl_flags) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Initialize subsystems */
    if (!platform_video_init()) {
        fprintf(stderr, "Video init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    platform_input_init();
    platform_save_init();
    atexit(platform_cleanup);
    platform_timer_init();

    /* Initialize game systems */
    game_init();

    /* Savestate round-trip self-test: prove state_dump_load() reads back exactly
     * what state_dump_save() wrote (byte-identical), then exit. Runs against the
     * post-game_init boot state. */
    if (savestate_selftest) {
        bool ok = state_dump_roundtrip_test();
        fprintf(stderr, "savestate round-trip self-test: %s\n", ok ? "PASS" : "FAIL");
        exit(ok ? 0 : 1);
    }

    /* Initialize audio (loads audio packs from embedded assets) */
    if (!platform_headless)
        platform_audio_init();

#ifdef ENABLE_VERIFY
    if (verify_rom_path) {
        if (!verify_init(verify_rom_path)) {
            fprintf(stderr, "Failed to initialize verification with ROM: %s\n", verify_rom_path);
        }
    }
#else
    (void)verify_rom_path;
#endif

    /* Start frame timer, then run the single top-level game loop:
     * advance the game one frame (game_loop_step), then perform the one
     * per-frame host yield (host_process_frame). Restart-on-game-over is
     * handled inside the step machine, so there is no restart wrapper.
     * See docs/plans/savestate-unified-loop.md. */
    platform_timer_frame_start();

    while (!platform_input_quit_requested()) {
        game_loop_step();
        host_process_frame();
        host_root_boundary(); /* root boundary: perform any pending torn-safe capture */
    }

#ifdef ENABLE_VERIFY
    verify_shutdown();
#endif

    return 0;
}
