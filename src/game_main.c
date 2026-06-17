#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_main.h"
#include "core/types.h"
#include "core/log.h"
#include "core/mode_stack.h"
#include "core/memory.h"
#include "core/state_dump.h"
#include "core/math.h"
#include "core/decomp.h"
#include "data/assets.h"
#include "platform/platform.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "game/game_state.h"
#include "game/fade.h"
#include "game/audio.h"
#include "intro/init_intro.h"
#include "game/overworld.h"
#include "game/ending.h"
#include "include/pad.h"
#include "include/constants.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "game/display_text.h"
#include "game/text.h"
#include "data/event_script_data.h"
#include "game/door.h"
#include "game/window.h"
#include "game/battle.h"
#include "game/town_map.h"
#include "game/flyover.h"
#include "game/oval_window.h"
#include "game/map_loader.h"
#include "game/position_buffer.h"
#include "game/inventory.h"
#include "game/display_text_internal.h"
#include "data/text_refs.h"

/* Verbosity level (0=errors, 1=warnings, 2=trace).
 * Defaults to 1 so warnings (asset-load failures, etc.) print without -v;
 * matches the pre-existing behavior of the raw fprintf(stderr, ...) calls
 * that were folded into LOG_WARN. */
int verbose_level = 1;

/* Auto-dump flag: set to non-zero to trigger a screenshot + VRAM dump */
int debug_auto_dump_requested = 0;

static bool show_fps = false;
static bool fast_forward_active = false;
static bool debug_menu_requested = false;
static uint16_t aux_prev = 0;

/* Capture-safety gate — see host_request_capture()/host_request_load()/
 * host_root_boundary() in game_main.h. g_pending_root_action is non-NONE while a
 * savestate save OR load is pending; while set, host_process_frame() free-runs and
 * counts unwind frames so the C stack returns to the root boundary (both saving and
 * loading require it: a save must be torn-safe, and a load replaces the mode stack
 * wholesale, which is only coherent with no nested pump frames on the C stack). */
typedef enum {
    ROOT_ACTION_NONE = 0,
    ROOT_ACTION_SAVE,   /* F6 / power-off: write a savestate */
    ROOT_ACTION_LOAD,   /* F7: restore the last savestate */
} RootAction;
static RootAction g_pending_root_action = ROOT_ACTION_NONE;
static int g_capture_unwind_frames = 0;

/* Free-run unwind safety valve: while a capture is pending, host_process_frame()
 * skips render + pacing and runs the per-frame logic at CPU speed, so finite blocking
 * helpers unwind to the root boundary in a handful of frames. If the game is stuck in
 * an indefinite input-wait (an unconverted blocking surface, e.g. display_text at a
 * "▼" prompt) it never reaches the boundary; after this many free-run frames we
 * abandon the request rather than write a torn snapshot. Free-run is CPU-bound, so
 * this is reached in well under a second. Tunable: large enough to cover every normal
 * finite blocker (fades, teleport, battle actions); long finite sequences such as the
 * end credits intentionally exceed it (no one needs to resume into the credits). */
#define CAPTURE_UNWIND_FRAME_CAP 600

/* Desktop savestate path for an F6 / power-off capture. */
#define EB_SAVESTATE_PATH "savestate.bin"

/* Dynamic frame-skipping state.
 * When the system falls behind real-time, skip PPU rendering (but keep
 * running game logic + audio) to catch up. Max 2 consecutive skips
 * (visual floor ~20fps). */
#ifndef MAX_FRAME_SKIP
#define MAX_FRAME_SKIP 0
#endif
static uint64_t frame_deadline;
static int consecutive_skips;
static int display_skip_run;    /* last skip run length for FPS overlay */
static bool frame_skip_initialized;

/* Debug timing for per-section profiling.
 * All values stored as IIR accumulators in tenths-of-ms, shifted by 4
 * for fractional precision.  No floating point. */
#define DEBUG_IIR_SHIFT 4
static uint32_t debug_logic_acc;
static uint32_t debug_render_acc;

/* Convert a tick delta to tenths-of-milliseconds using integer math. */
static uint32_t debug_ticks_to_tenths_ms(uint64_t delta) {
    return (uint32_t)(delta * 10000 / platform_timer_ticks_per_sec());
}

/* Format tenths value (e.g. 605 → "60.5") into buf without snprintf.
 * Returns number of chars written (not counting NUL). */
static int debug_format_tenths(char *buf, int size, uint32_t tenths) {
    if (tenths > 9999) tenths = 9999;
    uint32_t whole = tenths / 10;
    uint32_t frac = tenths % 10;

    /* Write whole part (variable length) */
    char tmp[8];
    int n = 0;
    if (whole == 0) {
        tmp[n++] = '0';
    } else {
        /* Extract digits in reverse */
        int start = n;
        uint32_t w = whole;
        while (w > 0) {
            tmp[n++] = '0' + (char)(w % 10);
            w /= 10;
        }
        /* Reverse */
        for (int i = start, j = n - 1; i < j; i++, j--) {
            char t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
        }
    }
    tmp[n++] = '.';
    tmp[n++] = '0' + (char)frac;
    tmp[n] = '\0';

    /* Copy to output */
    int i;
    for (i = 0; i < n && i < size - 1; i++)
        buf[i] = tmp[i];
    buf[i] = '\0';
    return i;
}

/* Scanline-based FPS overlay using the EB TINY font.
 * Stamps FPS text directly into pixel scanlines as they pass through
 * ppu_render_frame. Works on all platforms regardless of BG/tilemap state. */

/* Cached overlay text, computed once at frame start */
#ifdef PPU_PROFILE
#define FPS_OVERLAY_LINES 13
#else
#define FPS_OVERLAY_LINES 5
#endif

static struct {
    char lines[FPS_OVERLAY_LINES][16];
    pixel_t colors[FPS_OVERLAY_LINES];
    int total_h;       /* total overlay height in pixels */
    int n_lines;
} fps_overlay;

static void fps_overlay_prepare(void) {
    uint32_t fps10 = platform_timer_get_fps_tenths();
    if (fps10 > 999) fps10 = 999;
    uint32_t logic10 = debug_logic_acc >> DEBUG_IIR_SHIFT;
    uint32_t render10 = debug_render_acc >> DEBUG_IIR_SHIFT;
    uint32_t frame_budget = 10000 / TARGET_FPS;
    uint32_t work = logic10 + render10;
    uint32_t idle10 = (work < frame_budget) ? frame_budget - work : 0;

    char vbuf[16];
    int n = 0;
    debug_format_tenths(vbuf, sizeof(vbuf), fps10);
    snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]), "FPS %s", vbuf);
    fps_overlay.colors[n++] = PIXEL_RGB(0x00, 0xFF, 0x00);
    debug_format_tenths(vbuf, sizeof(vbuf), logic10);
    snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]), "LOG %s", vbuf);
    fps_overlay.colors[n++] = PIXEL_RGB(0xFF, 0xFF, 0x00);
    debug_format_tenths(vbuf, sizeof(vbuf), render10);
    snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]), "PPU %s", vbuf);
    fps_overlay.colors[n++] = PIXEL_RGB(0xFF, 0x88, 0x00);
    debug_format_tenths(vbuf, sizeof(vbuf), idle10);
    snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]), "IDL %s", vbuf);
    fps_overlay.colors[n++] = PIXEL_RGB(0x88, 0x88, 0x88);

    /* Show frame-skip indicator when dynamic frame-skipping is enabled */
#if MAX_FRAME_SKIP > 0
    snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]),
             "SKP %d", display_skip_run);
    fps_overlay.colors[n++] = display_skip_run > 0
        ? PIXEL_RGB(0xFF, 0x44, 0x44)
        : PIXEL_RGB(0x88, 0x88, 0x88);
#endif

#ifdef PPU_PROFILE
    if (ppu_profile.ready) {
        /* Convert ticks to tenths-of-ms for display.
         * Displayed values are in 0.1ms units: "BG 310" = 31.0ms. */
        uint64_t ticks_per_sec = platform_timer_ticks_per_sec();
        uint32_t div = (uint32_t)(ticks_per_sec / 10000); /* ticks per 0.1ms */
        if (div == 0) div = 1;
        pixel_t pc = PIXEL_RGB(0x00, 0xCC, 0xFF);
        snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]),
                 "CLR %lu", (unsigned long)(ppu_profile.clear / div));
        fps_overlay.colors[n++] = pc;
        snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]),
                 "BG  %lu", (unsigned long)(ppu_profile.bg / div));
        fps_overlay.colors[n++] = pc;
        snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]),
                 "OBJ %lu", (unsigned long)(ppu_profile.obj / div));
        fps_overlay.colors[n++] = pc;
        snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]),
                 "WIN %lu", (unsigned long)(ppu_profile.win / div));
        fps_overlay.colors[n++] = pc;
        snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]),
                 "CMP %lu", (unsigned long)(ppu_profile.composite / div));
        fps_overlay.colors[n++] = pc;
        snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]),
                 "SND %lu", (unsigned long)(ppu_profile.send / div));
        fps_overlay.colors[n++] = pc;
        /* Derived: SETUP = total - iter (frame setup before scanline loop);
         *          GLUE  = iter - sum_of_phases (per-iteration inter-phase work). */
        uint32_t phases_sum = ppu_profile.clear + ppu_profile.bg + ppu_profile.obj +
                              ppu_profile.win + ppu_profile.composite + ppu_profile.send;
        uint32_t setup_ticks = (ppu_profile.total > ppu_profile.iter)
                              ? (ppu_profile.total - ppu_profile.iter) : 0;
        uint32_t glue_ticks  = (ppu_profile.iter  > phases_sum)
                              ? (ppu_profile.iter - phases_sum) : 0;
        snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]),
                 "SET %lu", (unsigned long)(setup_ticks / div));
        fps_overlay.colors[n++] = pc;
        snprintf(fps_overlay.lines[n], sizeof(fps_overlay.lines[0]),
                 "GLU %lu", (unsigned long)(glue_ticks / div));
        fps_overlay.colors[n++] = pc;
    }
#endif

    fps_overlay.n_lines = n;
    uint8_t h = font_get_height(FONT_ID_TINY);
    fps_overlay.total_h = (h + 1) * n;
}

/* Stamp FPS overlay text into a scanline pixel buffer.
 * Used as scanline_stamp_cb_t by platform_render_frame(). */
static void fps_overlay_stamp_scanline(int y, pixel_t *pixels) {
    if (y >= fps_overlay.total_h) return;

    uint8_t h = font_get_height(FONT_ID_TINY);
    uint8_t row_h = h + 1;
    int text_row = y / row_h;
    int glyph_y = y % row_h;
    if (text_row >= fps_overlay.n_lines || glyph_y >= h) return;

    int overlay_w = 46;
    int ox = EB_VIEWPORT_WIDTH - overlay_w - 1;
    pixel_t color = fps_overlay.colors[text_row];

    /* Black background */
    for (int x = ox - 1; x < EB_VIEWPORT_WIDTH; x++)
        if (x >= 0) pixels[x] = PIXEL_RGB(0, 0, 0);

    /* Stamp glyphs */
    int cx = ox;
    for (const char *s = fps_overlay.lines[text_row]; *s; s++) {
        uint8_t eb = ascii_to_eb_char(*s);
        uint8_t idx = eb - 0x50;
        const uint8_t *glyph = font_get_glyph(FONT_ID_TINY, idx);
        uint8_t w = font_get_width(FONT_ID_TINY, idx);
        if (glyph) {
            uint8_t bits = glyph[glyph_y];
            for (int col = 0; col < w && col < 8; col++) {
                if (!(bits & (0x80 >> col))) { /* 0-bit = drawn */
                    int px = cx + col;
                    if (px >= 0 && px < EB_VIEWPORT_WIDTH)
                        pixels[px] = color;
                }
            }
        }
        cx += w;
    }
}


/* Host-side per-frame processing (rendering, input, audio, timing).
 * Called by the host main loop after each fiber yield. This contains
 * the work that the SNES NMI handler + main loop timing would do. */
void host_process_frame(void) {
    uint64_t t0, t1, t2;

    t0 = platform_timer_ticks();

    /* Capture-safety free-run: while a snapshot is pending, this frame is a pure
     * unwind step — keep the per-frame logic (fade/timers/RNG/tasks) so blocking
     * helpers progress, but skip render + vblank pacing (set below) so they unwind
     * at CPU speed back to the root boundary, where host_root_boundary() captures.
     * Bail (abandon the request) past the unwind budget — an indefinite input-wait
     * that will never reach the boundary; abandoning beats writing a torn snapshot. */
    bool capture_unwinding = (g_pending_root_action != ROOT_ACTION_NONE);
    if (capture_unwinding && ++g_capture_unwind_frames > CAPTURE_UNWIND_FRAME_CAP) {
        LOG_WARN("savestate: %s abandoned — game did not reach a root boundary "
                 "within %d unwind frames (stuck in an indefinite input-wait?)\n",
                 g_pending_root_action == ROOT_ACTION_LOAD ? "load" : "capture",
                 CAPTURE_UNWIND_FRAME_CAP);
        g_pending_root_action = ROOT_ACTION_NONE;
        g_capture_unwind_frames = 0;
        capture_unwinding = false;
    }

    /* Reset per-frame fade guard so fade_update runs exactly once this frame,
     * matching the assembly NMI handler which updates fade once per vblank. */
    fade_new_frame();

    /* NMI handler: update fade brightness (irq_nmi.asm lines 93-118).
     * Assembly updates INIDISP_MIRROR from fade parameters every vblank.
     * Must run before ppu_render_frame so the renderer sees the new brightness. */
    fade_update();

    /* NMI handler: sync palette mirror to CGRAM if upload pending.
     * On the SNES, the NMI handler always checks PALETTE_UPLOAD_MODE
     * and copies PALETTES → CGRAM when set. */
    sync_palettes_to_cgram();

    t1 = platform_timer_ticks();

    /* Initialize frame-skip deadline on first call */
    if (!frame_skip_initialized) {
        frame_deadline = platform_timer_ticks();
        consecutive_skips = 0;
        frame_skip_initialized = true;
    }

    /* Frame skipping for turbo mode: skip rendering 2 out of every 3 frames
     * so that vsync blocking doesn't negate the speed-up. */
    static int frame_skip_counter;
    bool do_render;
    if (fast_forward_active) {
        do_render = (frame_skip_counter == 0);
        frame_skip_counter = (frame_skip_counter + 1) % FAST_FORWARD_MULTIPLIER;
    } else {
#ifdef PLATFORM_HOST_PACED_FRAMESKIP
        /* The platform paces the loop against an external clock and owns the
         * skip decision (e.g. G&W locks to its audio DMA). should_render()
         * records this frame's skip state for the matching sleep_until() and
         * returns whether to render. The deadline-based logic below is bypassed
         * — the deadline computed in the timing block is then ignored by the
         * platform's sleep_until(). */
        do_render = platform_timer_should_render();
        frame_skip_counter = 0;
#else
        /* Dynamic frame-skipping: when behind schedule, skip renders to
         * keep game logic at real-time speed. */
        uint64_t now = platform_timer_ticks();
        uint64_t tps = platform_timer_ticks_per_sec();
        uint64_t frame_period = tps / TARGET_FPS;
        int64_t time_debt = (int64_t)(now - frame_deadline);

        if (time_debt > (int64_t)frame_period && consecutive_skips < MAX_FRAME_SKIP) {
            do_render = false;
            consecutive_skips++;
        } else {
            do_render = true;
            display_skip_run = consecutive_skips;
            consecutive_skips = 0;
        }
        frame_skip_counter = 0;
#endif
    }

    /* Compute aux button edges (newly pressed this frame) */
    uint16_t aux = platform_input_get_aux();
    uint16_t aux_new = aux & ~aux_prev;
    aux_prev = aux;

    /* Force render if a debug dump is pending (even during frame skip) */
    bool debug_dump = (aux_new & AUX_DEBUG_DUMP) || debug_auto_dump_requested;
    bool vram_dump = (aux_new & AUX_VRAM_DUMP) != 0;
    if (debug_dump || vram_dump)
        do_render = true;

    /* Capture-safety: never render during a free-run unwind. */
    if (capture_unwinding)
        do_render = false;

    if (do_render) {
        /* Render the PPU state via the platform's render path.
         * Single-core platforms do begin/render/end sequentially.
         * Dual-core platforms distribute scanlines across cores. */
        {
            scanline_stamp_cb_t fps_cb = NULL;
            if (show_fps && font_get_glyph(FONT_ID_TINY, 0)) {
                fps_overlay_prepare();
                fps_cb = fps_overlay_stamp_scanline;
            }
            platform_render_frame(fps_cb);
        }

        t2 = platform_timer_ticks();

        pixel_t *fb = platform_video_get_framebuffer();

        /* Debug dump — must happen while texture is locked */
        if (fb && debug_dump) {
            platform_debug_dump_ppu(fb);
            platform_debug_dump_vram_image();
            debug_auto_dump_requested = 0;
        }
        if (fb && vram_dump) {
            platform_debug_dump_ppu(fb);
            platform_debug_dump_vram_image();
        }
    } else {
        t2 = t1;
    }

    /* Update profiling timers (integer IIR, no floating point) */
    if (show_fps) {
        uint32_t logic10  = debug_ticks_to_tenths_ms(t1 - t0);
        uint32_t render10 = debug_ticks_to_tenths_ms(t2 - t1);
        debug_logic_acc  = debug_logic_acc  - (debug_logic_acc  >> DEBUG_IIR_SHIFT) + logic10;
        debug_render_acc = debug_render_acc - (debug_render_acc >> DEBUG_IIR_SHIFT) + render10;
    }

    /* Poll input */
    platform_input_poll();
    uint16_t current_pad = platform_input_get_pad();

    /* Demo playback: override pad input when playing back auto-movement.
     * Port of READ_JOYPAD demo section — demo_playback_tick() may
     * overwrite core.pad1_raw, so we call memory_update_joypad with the
     * demo-overridden value when in playback mode. */
    demo_playback_tick();
    if (ow.demo_recording_flags & 0x4000) {
        current_pad = core.pad1_raw;
    }
    memory_update_joypad(current_pad);

#ifdef ENABLE_VERIFY
    verify_frame(current_pad);
#endif

    if (aux_new & AUX_FPS_TOGGLE)
        show_fps = !show_fps;
    if (aux_new & AUX_FAST_FORWARD) {
        fast_forward_active = !fast_forward_active;
        platform_video_set_vsync(!fast_forward_active);
    }
    /* F6 (save) / F7 (load): request the action rather than performing it here —
     * this host_process_frame() may be a nested yield inside pump_mode or a blocking
     * helper, where a save would be torn and a load (replacing the mode stack) would
     * corrupt the suspended parent. host_root_boundary() services it at the root loop. */
    if (aux_new & AUX_SAVESTATE)
        host_request_capture();
    if (aux_new & AUX_LOAD_STATE)
        host_request_load();
    if (aux_new & AUX_DEBUG_TOGGLE) {
        ow.debug_flag = 1;
        debug_menu_requested = true;
    }

    /* Process sound effect queue (once per frame, matching NMI handler) */
    audio_process_sfx_queue();

    /* Execute per-frame IRQ callback (port of EXECUTE_IRQ_CALLBACK).
     * Normally process_overworld_tasks; credits swaps to credits_scroll_frame.
     * Only ONE runs per frame, matching the assembly's JMP (IRQ_CALLBACK). */
    if (frame_callback) {
        frame_callback();
    } else {
        process_overworld_tasks();
    }

    /* Increment frame counter */
    core.frame_counter++;
    core.nmi_count++;
    core.play_timer++;

    /* Check frame limit */
    if (platform_max_frames > 0 && core.frame_counter >= (uint32_t)platform_max_frames)
        platform_request_quit();

    /* Frame timing: advance deadline by one frame period, clamp debt,
     * sleep if ahead, and update FPS filter. */
    {
        uint64_t tps = platform_timer_ticks_per_sec();
        uint64_t frame_period = tps / TARGET_FPS;

        if (fast_forward_active) {
            /* Fast-forward: use the original simple timing */
            platform_timer_frame_end();
        } else {
            /* Advance deadline by one frame */
            frame_deadline += frame_period;

            /* Clamp: if debt exceeds MAX_FRAME_SKIP frames, reset deadline
             * to prevent runaway catch-up after pause/breakpoint */
            uint64_t now = platform_timer_ticks();
            if ((int64_t)(now - frame_deadline) > (int64_t)(MAX_FRAME_SKIP * frame_period))
                frame_deadline = now;

            /* Sleep if ahead of schedule (skipped during a capture free-run so
             * finite blockers unwind at CPU speed, not wall-clock frames). */
            if (!platform_headless && !capture_unwinding)
                platform_timer_sleep_until(frame_deadline);

            /* Update FPS IIR filter — only on rendered frames so the
             * counter shows actual display refresh rate, not game logic rate. */
            if (do_render)
                platform_timer_update_fps();
        }
        /* Reset the per-frame timer.  Must run every frame (not just
         * rendered ones) so that platform_timer_frame_end() computes its
         * sleep deadline from the current frame, not a stale one. */
        platform_timer_frame_start();
    }
}

/* Request a torn-safe capture at the next root-loop boundary. See game_main.h. */
void host_request_capture(void) {
    if (g_pending_root_action != ROOT_ACTION_NONE)
        return; /* already pending — keep the original action + unwind budget */
    g_pending_root_action = ROOT_ACTION_SAVE;
    g_capture_unwind_frames = 0;
}

/* Request a savestate restore at the next root-loop boundary. See game_main.h. */
void host_request_load(void) {
    if (g_pending_root_action != ROOT_ACTION_NONE)
        return; /* already pending — keep the original action + unwind budget */
    g_pending_root_action = ROOT_ACTION_LOAD;
    g_capture_unwind_frames = 0;
}

/* Perform a pending save/load, if any. MUST be called only from the outermost host
 * loop (the root boundary) — never from a nested host_process_frame(). See game_main.h. */
void host_root_boundary(void) {
    RootAction action = g_pending_root_action;
    if (action == ROOT_ACTION_NONE)
        return;

    g_pending_root_action = ROOT_ACTION_NONE;
    g_capture_unwind_frames = 0;

    if (action == ROOT_ACTION_SAVE) {
        /* Crash-safe ping-pong write: alternates between EB_SAVESTATE_PATH ".0"/".1"
         * so a power-loss mid-write leaves the prior slot intact. */
        if (state_dump_save_slots(EB_SAVESTATE_PATH))
            LOG_WARN("savestate: wrote %s.[01]\n", EB_SAVESTATE_PATH);
        else
            LOG_WARN("savestate: failed to write %s\n", EB_SAVESTATE_PATH);
    } else { /* ROOT_ACTION_LOAD */
        if (state_dump_load_slots(EB_SAVESTATE_PATH)) {
            /* The snapshot doesn't include the live SPC700/DSP, so restart the music
             * named by the restored audio_state — otherwise the pre-load track keeps
             * playing over the loaded game. */
            audio_resync_after_load();
            LOG_WARN("savestate: loaded %s\n", EB_SAVESTATE_PATH);
        } else {
            LOG_WARN("savestate: failed to load %s\n", EB_SAVESTATE_PATH);
        }
    }
}

/* Wait for one frame (NMI equivalent).
 * Yields the game fiber back to the host, which runs host_process_frame()
 * (rendering, input, audio, timing) before resuming game logic. */
void wait_for_vblank(void) {
    host_process_frame();
    /* Check quit after resuming — game logic may be deep in nested calls
     * (battle, text, menu) where the top-level loop check never runs. */
    if (platform_input_quit_requested())
        exit(0);
}

/* Wait for N frames or until a button is pressed.
   Returns true if a button was pressed. */
bool wait_frames_or_button(uint16_t count, uint16_t button_mask) {
    for (uint16_t i = 0; i < count; i++) {
        if (platform_input_quit_requested()) return false;
        wait_for_vblank();
        if (platform_input_get_pad_new() & button_mask)
            return true;
    }
    return false;
}

/*
 * mode_step_debug_ymenu — run-to-completion driver for the clean-leaf debug
 * Y-button menus (flag editor, guide counter). See DebugYMenuState in
 * mode_stack.h. The single yield is owned by the pump; this body never calls
 * wait_for_vblank(). Input is read post-yield (the established pattern): DY_DRAW
 * renders + window_tick_work + yields, DY_INPUT acts on the latched input.
 */
StepResult mode_step_debug_ymenu(ModeState *st) {
    DebugYMenuState *s = &st->debug_ymenu;

    switch ((DebugYMenuPhase)s->phase) {
    case DY_DRAW:
        set_instant_printing();
        create_window(WINDOW_FILE_SELECT_MENU);
        set_window_number_padding(3);
        if (s->kind == DBG_YMENU_FLAG) {
            print_number((int)s->index, 1);
            print_char_with_sound(0x0020);  /* space */
            advance_vwf_tile();
            if (event_flag_get(s->index))
                print_string("ON");
            else
                print_string("OFF");
        } else { /* DBG_YMENU_GUIDE: count entities with active scripts */
            int count = 0;
            for (int i = 0; i < MAX_ENTITIES; i++) {
                if (entities.script_table[ENT(i)] != -1)
                    count++;
            }
            print_number(count, 1);
        }
        clear_instant_printing();
        if (window_tick_work_step()) {
            s->phase = DY_DRAW_FLUSH;
            return actionscript_frame_take_push();
        }
        s->phase = DY_INPUT;
        return STEP_RESULT_CONTINUE();

    case DY_DRAW_FLUSH:
        window_tick_work_flush();
        s->phase = DY_INPUT;
        return STEP_RESULT_CONTINUE();

    case DY_INPUT:
        if (s->kind == DBG_YMENU_GUIDE) {
            /* Draw once, then wait for B/SELECT (pre-yield check each frame). */
            if (core.pad1_pressed & PAD_CANCEL) {
                close_window(WINDOW_FILE_SELECT_MENU);
                return STEP_RESULT_POP(0);
            }
            return STEP_RESULT_CONTINUE();
        }

        /* DBG_YMENU_FLAG: d-pad navigates (±1 / ±10 held), A toggles, B exits.
         * Any nav/toggle returns to DY_DRAW to re-render. */
        {
            uint16_t new_index = s->index;
            bool redraw = false;
            if (core.pad1_held & PAD_UP) {
                new_index = s->index + 1; redraw = true;
            } else if (core.pad1_held & PAD_DOWN) {
                new_index = s->index - 1; redraw = true;
            } else if (core.pad1_held & PAD_RIGHT) {
                new_index = s->index + 10; redraw = true;
            } else if (core.pad1_held & PAD_LEFT) {
                new_index = s->index - 10; redraw = true;
            } else if (core.pad1_pressed & PAD_CONFIRM) {
                if (event_flag_get(s->index))
                    event_flag_clear(s->index);
                else
                    event_flag_set(s->index);
                redraw = true;  /* new_index unchanged; re-render toggled state */
            } else if (core.pad1_pressed & PAD_CANCEL) {
                close_window(WINDOW_FILE_SELECT_MENU);
                return STEP_RESULT_POP(0);
            }

            if (redraw) {
                /* Validate: must be 1-1999 (assembly: >= 2000 or == 0 → keep old) */
                if (new_index > 0 && new_index < 2000)
                    s->index = new_index;
                s->phase = DY_DRAW;
            }
            return STEP_RESULT_CONTINUE();
        }
    }

    return STEP_RESULT_POP(0);
}

/*
 * mode_step_debug_goods — run-to-completion port of DEBUG_Y_BUTTON_GOODS
 * (asm/overworld/debug/y_button_goods.asm). See DebugGoodsState in mode_stack.h.
 *
 * Interactive item browser/giver. Shows item ID and name. D-pad navigates
 * (up/down by 1, left/right by 10), A gives item to a selected character
 * (auto-equips weapons), B exits. The blocking form was a raw for(;;){ ...
 * wait_for_vblank(); ... } loop with an inline char_select_prompt(mode 1) — the
 * last non-mode debug driver. The A path now runs char_select_prompt's mode-1
 * (overworld) flow as a STEP_PUSH of SELECTION_MENU, bracketed by
 * char_select_overworld_prepare/finish (the determine-targetting ally pattern).
 * The single yield is owned by the pump/root; this body never calls
 * wait_for_vblank(). Input is read post-yield (the established pattern).
 */
StepResult mode_step_debug_goods(ModeState *mst) {
    DebugGoodsState *s = &mst->debug_goods;

    switch ((DebugGoodsPhase)s->phase) {
    case DG_DRAW: {
        set_instant_printing();
        create_window(WINDOW_FILE_SELECT_MENU);
        set_window_number_padding(2);

        /* US: set padding to 130, cursor to (0,0), print number, cursor to (3,0) */
        set_window_number_padding(130);
        set_focus_text_cursor(0, 0);
        print_number((int)s->item_id, 1);
        set_focus_text_cursor(3, 0);

        /* Print item name (EB-encoded, 25 chars max) */
        const ItemConfig *item = get_item_entry(s->item_id);
        if (item)
            print_text_with_word_splitting(item->name, ITEM_NAME_LEN);

        clear_instant_printing();
        if (window_tick_work_step()) {
            s->phase = DG_DRAW_FLUSH;
            return actionscript_frame_take_push();
        }
        s->phase = DG_INPUT;
        return STEP_RESULT_CONTINUE();
    }

    case DG_DRAW_FLUSH:
        window_tick_work_flush();
        s->phase = DG_INPUT;
        return STEP_RESULT_CONTINUE();

    case DG_INPUT: {
        /* d-pad navigates (held), A gives, B exits. Any nav returns to DG_DRAW
         * to re-render; the cadence (one step per two frames) matches the
         * blocking draw-frame + input-frame loop. */
        uint16_t new_id = s->item_id;
        bool changed = false;
        if (core.pad1_held & PAD_UP) {
            new_id = s->item_id + 1; changed = true;
        } else if (core.pad1_held & PAD_DOWN) {
            new_id = s->item_id - 1; changed = true;
        } else if (core.pad1_held & PAD_RIGHT) {
            new_id = s->item_id + 10; changed = true;
        } else if (core.pad1_held & PAD_LEFT) {
            new_id = s->item_id - 10; changed = true;
        } else if (core.pad1_pressed & PAD_CONFIRM) {
            /* Give item to selected character: char_select_prompt(1, 1, NULL,
             * NULL)'s name window, with its SELECTION_MENU STEP_PUSHed; the
             * mode-1 epilogue runs in DG_GIVE_RESULT after it pops. */
            s->saved_argument_memory = get_argument_memory();
            s->give_window_id = char_select_overworld_prepare(NULL);
            ModeState child = {0};
            child.selection_menu.phase        = SM_SETUP;
            child.selection_menu.allow_cancel = 1;
            s->phase = DG_GIVE_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_SELECTION_MENU, &child);
        } else if (core.pad1_pressed & PAD_CANCEL) {
            close_window(WINDOW_FILE_SELECT_MENU);
            return STEP_RESULT_POP(0);
        }

        if (changed) {
            /* Validate: assembly uses CMP #$0100 (256), unsigned */
            if (new_id < 256)
                s->item_id = new_id;
            s->phase = DG_DRAW;
        }
        return STEP_RESULT_CONTINUE();
    }

    case DG_GIVE_RESULT:
    default: {
        /* The char-select SELECTION_MENU popped. Run char_select_prompt's mode-1
         * epilogue (window close, attrs, pagination) + argument_memory restore,
         * then the give/equip tail. */
        uint16_t char_id = (uint16_t)mode_child_result();
        char_select_overworld_finish(s->give_window_id, false);
        set_argument_memory(s->saved_argument_memory);

        /* Cancelled (0) or no inventory room -> back to browsing (the blocking
         * form's `break` out of the inner loop, which redrew the same item). */
        if (char_id == 0 || find_inventory_space2(char_id) == 0) {
            s->phase = DG_DRAW;
            return STEP_RESULT_CONTINUE();
        }

        give_item_to_character(char_id, s->item_id);
        /* Auto-equip if it's a weapon/armor type (usable by char && type == 2). */
        if (check_item_usable_by(char_id, s->item_id) != 0 &&
            get_item_type(s->item_id) == 2) {
            uint16_t slot = find_empty_inventory_slot(char_id);
            equip_item(char_id, slot);
        }
        close_window(WINDOW_FILE_SELECT_MENU);
        return STEP_RESULT_POP(0);
    }
    }
}


/*
 * mode_step_debug_menu — run-to-completion port of DEBUG_Y_BUTTON_MENU
 * (asm/system/debug/y_button_menu.asm), the debug Y-button parent menu. See
 * DebugMenuState in mode_stack.h. Triggered by holding B/SELECT + R in the
 * overworld with ow.debug_flag set. The blocking form was a `display_menu:`-goto
 * loop; each phase here is an asm sequence point, every blocking child driver
 * becomes a STEP_PUSH, and the single yield is owned by the root.
 *
 * Commands that block only via wait_for_vblank/host_process_frame but never
 * pump_mode (Warp, and the cutscenes of CAST/STAFF), and the synchronous ones
 * (Save/learn_special_psi/Meter), run inline within DM_DISPATCH — host_process_frame
 * does not re-enter the mode stack, so this is safe (and matches the blocking form's
 * behaviour). CAST/STAFF's teleport-back is GAME_MODE_TELEPORT_TO (STEP_PUSHed, D4b).
 * Player 0/1 naming is GAME_MODE_ENTER_NAME (STEP_PUSHed, D4b).
 *
 * Menu items match DEBUG_MENU_TEXT in asm/data/debug/menu_text.asm (US):
 *  1=Flag  2=Goods  3=Save  4=Apple  5=Banana  6=TV  7=Event  8=Warp
 *  9=Tea  10=Teleport  11=Star~  12=Star^  13=Player0  14=Player1
 *  15=GUIDE  16=TRACK  17=CAST  18=STONE  19=STAFF  20=Meter
 *  21=REPLAY  22=TEST1  23=TEST2  24=(disable replay)
 */
StepResult mode_step_debug_menu(ModeState *mst) {
    DebugMenuState *s = &mst->debug_menu;
    static ModeState child; /* child init: pump copies it immediately */

    switch ((DebugMenuPhase)s->phase) {
    case DM_ENTER:
        /* Assembly entry (lines 15-19): disable entities, SFX, show HP/PP. */
        disable_all_entities();
        play_sfx(1);  /* SFX::CURSOR1 */
        show_hppp_windows();
        s->phase = DM_BUILD;
        return STEP_RESULT_CONTINUE();

    case DM_BUILD:
        /* Assembly @DISPLAY_MENU: (re)build the 23-item menu window, then push the
         * selection. Assembly uses @LOCAL04 as an @AFTER_COMMAND message pointer
         * (here DebugMenuState.message_addr), reset before each selection. */
        s->message_addr = 0;
        create_window(WINDOW_PHONE_MENU);
        add_menu_item_no_position("Flag",      1);
        add_menu_item_no_position("Goods",     2);
        add_menu_item_no_position("Save",      3);
        add_menu_item_no_position("Apple",     4);
        add_menu_item_no_position("Banana",    5);
        add_menu_item_no_position("TV",        6);
        add_menu_item_no_position("Event",     7);
        add_menu_item_no_position("Warp",      8);
        add_menu_item_no_position("Tea",       9);
        add_menu_item_no_position("Teleport", 10);
        add_menu_item_no_position("Star ~",   11);
        add_menu_item_no_position("Star ^",   12);
        add_menu_item_no_position("Player 0", 13);
        add_menu_item_no_position("Player 1", 14);
        add_menu_item_no_position("GUIDE",    15);
        add_menu_item_no_position("TRACK",    16);
        add_menu_item_no_position("CAST",     17);
        add_menu_item_no_position("STONE",    18);
        add_menu_item_no_position("STAFF",    19);
        add_menu_item_no_position("Meter",    20);
        add_menu_item_no_position("REPLAY",   21);
        add_menu_item_no_position("TEST1",    22);
        add_menu_item_no_position("TEST2",    23);
        open_window_and_print_menu(1, 0);
        child = (ModeState){0};
        child.selection_menu.phase        = SM_SETUP;
        child.selection_menu.allow_cancel = 1;
        s->phase = DM_DISPATCH;
        return STEP_RESULT_PUSH_INIT(GAME_MODE_SELECTION_MENU, &child);

    case DM_DISPATCH: {
        /* The selection menu popped: dispatch the chosen command. Pushes set their
         * resume to DM_AFTER (where the optional message text shows, then the menu
         * rebuilds); TEST2/REPLAY/default end the menu via DM_CLEANUP. */
        uint16_t result = (uint16_t)mode_child_result();
        s->message_addr = 0;
        switch (result) {
        case 1:
            /* FLAGS — assembly @CMD_FLAGS: JSL DEBUG_Y_BUTTON_FLAG. */
            child = (ModeState){0};
            child.debug_ymenu.phase = DY_DRAW;
            child.debug_ymenu.kind  = DBG_YMENU_FLAG;
            child.debug_ymenu.index = 1;  /* Start at FLG_TEMP_0 (index 1) */
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DEBUG_YMENU, &child);
        case 2:
            /* GOODS — assembly @CMD_GOODS: JSL DEBUG_Y_BUTTON_GOODS. */
            child = (ModeState){0};
            child.debug_goods.phase   = DG_DRAW;
            child.debug_goods.item_id = 0;
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DEBUG_GOODS, &child);
        case 3:
            /* SAVE — assembly @CMD_SAVE: save + update respawn coordinates. */
            save_game(current_save_slot - 1);
            ow.respawn_x = game_state.leader_x_coord;
            ow.respawn_y = game_state.leader_y_coord;
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 4:
            /* Apple (MSG_DEBUG_00) — assembly @CMD_MSG_00. */
            s->message_addr = MSG_DBG_MAIN_MENU;
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 5:
            /* Banana (MSG_DEBUG_01) — assembly @CMD_MSG_01. */
            s->message_addr = MSG_DBG_EVENT_SCENE_SELECT;
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 6:
            /* TV (MSG_DEBUG_02) — assembly @CMD_MSG_02. */
            s->message_addr = MSG_DBG_MONSTER_TOGGLE;
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 7:
            /* Event — assembly @CMD_MSG_UNKNOWN. */
            s->message_addr = MSG_DBGTXT_DEBUG_MENU_CAMERA_MOVE;
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 8: {
            /* WARP — assembly @CMD_WARP. Flash HP/PP windows 30 times, then
             * teleport to Onett center (7696, 2280). Inline-blocking but pump-free
             * (update_hppp_meter_and_render/fade_* block via host_process_frame,
             * not the mode stack) — a savestate cannot land mid-warp, accepted. */
            for (int i = 0; i < 30; i++) {
                undraw_hp_pp_window(0);
                update_hppp_meter_and_render();
                update_hppp_meter_and_render();
                draw_and_mark_hppp_window(0);
                update_hppp_meter_and_render();
                update_hppp_meter_and_render();
            }
            fade_out(1, 0);
            load_map_at_position(7696, 2280);
            set_leader_position_and_load_party(7696, 2280, 0);
            fade_in(1, 0);
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        }
        case 9: {
            /* Tea (COFFEE/TEA) — assembly @CMD_COFFEE_TEA: random coffee/tea scene.
             * Push GAME_MODE_FLYOVER (FO_COFFEETEA), the coffeetea_scene() init. */
            uint16_t type = rng_next_byte() & 1;
            child = (ModeState){0};
            child.flyover.kind        = FO_COFFEETEA;
            child.flyover.phase       = FOP_CT_FADEOUT1;
            child.flyover.id          = type;
            child.flyover.pos         = 0;
            child.flyover.script_size =
                (uint32_t)ASSET_SIZE(type == 0 ? ASSET_COFFEE_BIN : ASSET_TEA_BIN);
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_FLYOVER, &child);
        }
        case 10:
            /* Teleport (PSI_1) — assembly @CMD_PSI_1: learn special PSI type 1. */
            learn_special_psi(1);
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 11:
            /* Star ~ (PSI_2) — assembly @CMD_PSI_2: learn special PSI type 2. */
            learn_special_psi(2);
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 12:
            /* Star ^ (PSI_3_4) — assembly @CMD_PSI_3_4: learn PSI types 3 and 4. */
            learn_special_psi(3);
            learn_special_psi(4);
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 13:
            /* Player 0 (NAME_0) — assembly @CMD_NAME_0: name character 0 (Ness).
             * The M2/EB name prompt is GAME_MODE_ENTER_NAME (param 0). */
            child = (ModeState){0};
            child.enter_name.phase = EN_ENTER;
            child.enter_name.param = 0;
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_ENTER_NAME, &child);
        case 14:
            /* Player 1 (NAME_1) — assembly @CMD_NAME_1: name character 1 (Paula). */
            child = (ModeState){0};
            child.enter_name.phase = EN_ENTER;
            child.enter_name.param = 1;
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_ENTER_NAME, &child);
        case 15:
            /* GUIDE — assembly @CMD_TOWN_MAP: JSL RUN_TOWN_MAP_MENU. (The label
             * says "GUIDE" but the assembly dispatches to the town map.) */
            run_town_map_menu_prepare(&child);
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_TOWN_MAP, &child);
        case 16:
            /* TRACK — assembly @CMD_GUIDE: JSL DEBUG_Y_BUTTON_GUIDE (script counter). */
            child = (ModeState){0};
            child.debug_ymenu.phase = DY_DRAW;
            child.debug_ymenu.kind  = DBG_YMENU_GUIDE;
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DEBUG_YMENU, &child);
        case 17:
            /* CAST — assembly @CMD_CAST: play cast scene, then teleport to dest 1.
             * play_cast_scene blocks via host_process_frame only (pump-free); the
             * teleport-back is GAME_MODE_TELEPORT_TO (D4b: the screen_transition
             * pump bridge is gone), STEP_PUSHed with resume DM_AFTER. */
            play_cast_scene();
            child = (ModeState){0};
            child.teleport_to.phase   = TT_BEGIN;
            child.teleport_to.dest_id = 1;
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_TELEPORT_TO, &child);
        case 18:
            /* STONE — assembly @CMD_SOUND_STONE: sound stone melody (cancellable). */
            child = (ModeState){0};
            child.sound_stone.phase       = SS_SETUP1;
            child.sound_stone.cancellable = 1;
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_SOUND_STONE, &child);
        case 19:
            /* STAFF (CREDITS) — assembly @CMD_CREDITS: play credits, then dest 1.
             * Teleport-back is GAME_MODE_TELEPORT_TO, STEP_PUSHed (resume DM_AFTER). */
            play_credits();
            child = (ModeState){0};
            child.teleport_to.phase   = TT_BEGIN;
            child.teleport_to.dest_id = 1;
            s->phase = DM_AFTER;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_TELEPORT_TO, &child);
        case 20:
            /* Meter (FLIPOUT) — assembly @CMD_FLIPOUT: toggle HP/PP flipout mode. */
            toggle_hppp_flipout_mode(bt.hppp_meter_flipout_mode ? 0 : 1);
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 22:
            /* TEST1 (MSG_BTL) — assembly @CMD_MSG_BTL: show
             * MSG_BTL7_PRAY_RESPONSE_STRANGER (0xC9F70C). */
            s->message_addr = 0xC9F70C;
            s->phase = DM_AFTER;
            return STEP_RESULT_CONTINUE();
        case 23:
            /* TEST2 (TO_BE_CONTINUED) — assembly @CMD_TO_BE_CONTINUED: close
             * windows, display the "To Be Continued" text, then cleanup. */
            close_all_windows();
            hide_hppp_windows();
            s->phase = DM_CLEANUP;
            if (dt_make_child_init(&child, MSG_EVT5_NESS_WAKES_KIDNAP_AFTERMATH))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            LOG_WARN("debug menu: resolve_text_addr(0x%06X) returned NULL\n",
                     (unsigned)MSG_EVT5_NESS_WAKES_KIDNAP_AFTERMATH);
            return STEP_RESULT_CONTINUE();
        case 21:
            /* REPLAY — assembly @CMD_REPLAY: START_REPLAY_MODE. Replay recording is
             * not applicable to the C port (SNES SRAM); go straight to cleanup. */
        default:
            /* Cancelled or item 24+ → DISABLE_REPLAY_MODE (also N/A); cleanup. */
            s->phase = DM_CLEANUP;
            return STEP_RESULT_CONTINUE();
        }
    }

    case DM_AFTER:
        /* Assembly @AFTER_COMMAND: if a command set a message address, close the
         * menu window, open TEXT_STANDARD and display it; then loop back to rebuild
         * the menu. If no message, just rebuild. */
        if (s->message_addr != 0) {
            uint32_t addr = s->message_addr;
            s->message_addr = 0;
            close_focus_window();
            create_window(WINDOW_TEXT_STANDARD);
            s->phase = DM_BUILD;
            if (dt_make_child_init(&child, addr))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            LOG_WARN("debug menu: resolve_text_addr(0x%06X) returned NULL\n",
                     (unsigned)addr);
            return STEP_RESULT_CONTINUE();
        }
        s->phase = DM_BUILD;
        return STEP_RESULT_CONTINUE();

    case DM_CLEANUP:
        /* Assembly @CLEANUP: close windows, hide HP/PP, wait for entity fade. */
        close_all_windows();
        hide_hppp_windows();
        s->phase = DM_DONE;
        return STEP_RESULT_PUSH(GAME_MODE_ENTITY_FADE_WAIT);

    case DM_DONE:
    default:
        enable_all_entities();
        return STEP_RESULT_POP(0);
    }
}

/* boot_begin — boot stage 1: one-shot setup, run once from OWP_BOOT_SETUP.
 *
 * Normal path: init_intro() does its one-shot init and PUSHes GAME_MODE_INIT_INTRO
 * as a child of the overworld root; the generic pump then drives that intro state
 * machine one step per frame (the root loop owns the single yield). Once the intro
 * pops back, the root's OWP_BOOT_AWAIT runs overworld_boot(). See
 * docs/plans/savestate-unified-loop.md.
 *
 * --skip-intro path: do the minimal init the intro would otherwise perform and push
 * nothing, so OWP_BOOT_SETUP flows straight into OWP_BOOT_AWAIT/overworld_boot(). */
static void boot_begin(void) {
    if (platform_skip_intro) {
        /* Debug: skip intro/file select, go directly to overworld.
         * Minimal init that the intro would normally do. */
        load_event_script_data();
        text_system_init();
        text_setup_bg3();
        text_upload_font_tiles();
        text_load_window_gfx();
        window_system_init();
        /* Set up a proper starting position for debugging */
        game_state.party_members[0] = 1;
        game_state.party_count = 1;
        game_state.player_controlled_party_count = 1;
        game_state.current_party_members = 1;
        /* Default starting position for skip-intro debugging */
        game_state.leader_x_coord = 265 * 8;
        game_state.leader_y_coord = 15 * 8;
        game_state.leader_direction = 4; /* facing DOWN */
    } else {
        /* One-shot intro setup + mode_push(GAME_MODE_INIT_INTRO). Returns
         * immediately with the intro state machine on the stack. */
        init_intro();
    }
}

/* overworld_boot — stage 3 of boot: overworld initialization, run once the intro
 * has finished (or was skipped). In the assembly, MAIN_LOOP calls INIT_INTRO →
 * FILE_SELECT_INIT → INITIALIZE_OVERWORLD_STATE; this is the work after the intro.
 *
 * IMPORTANT: this must NOT yield before the first overworld render — the assembly
 * has no WAIT between init and @LOOP_BEGIN (see the fade_in note below). The caller
 * (OWP_BOOT_AWAIT) runs this and the first OWP_RENDER in the same step (its
 * `continue` into OWP_RENDER does not yield). */
/* The post-boot-frame tail (main.asm lines 15-23): fade-in + first palette sync +
 * BG2 tilemap init. Run inline by overworld_boot_step() on the no-park path, or at
 * OWP_BOOT_FLUSH if the boot actionscript frame parked. */
static void overworld_boot_flush(void) {
    /* Assembly main.asm lines 15-18: FADE_IN(1,1) then UPDATE_SCREEN.
     * FADE_IN is non-blocking (just sets fade parameters).
     * UPDATE_SCREEN syncs ert.palettes and builds entity draw list.
     *
     * IMPORTANT: The assembly has NO WAIT_UNTIL_NEXT_FRAME between here
     * and @LOOP_BEGIN (line 24). All pre-loop code (lines 14-22) runs
     * within a single frame. The first vblank occurs inside the main
     * loop at line 29. Inserting an extra wait_for_vblank() here would
     * consume one fade delay tick without advancing entity scripts,
     * causing the screen to become visible one script frame too early
     * (e.g., Ness flashing on screen before the intro script hides him). */
    fade_in(1, 1);
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    sync_palettes_to_cgram();

    /* Assembly: JSL INIT_USED_BG2_TILE_MAP (main.asm line 23, US only). */
    init_used_bg2_tile_map();
}

/* Run-to-completion form of overworld_boot (savestate D4b): the boot actionscript
 * frame becomes a STEP_PUSH on the (rare) park instead of a nested pump_mode.
 * Returns true iff the boot frame parked — the caller STEP_PUSHes
 * GAME_MODE_ACTIONSCRIPT_FRAME and runs overworld_boot_flush() at OWP_BOOT_FLUSH.
 * On no park (the normal case) it completes the frame + tail inline and returns
 * false, so OWP_BOOT_AWAIT flows into OWP_RENDER with no yield (no Ness-flash). */
static bool overworld_boot_step(void) {
    /* Sprite data lives in ROM on the SNES (always available). In the C port
     * it must be loaded from extracted asset files. The naming screen (new game)
     * and attract mode load it for their own use, but when loading an existing
     * save the data may not yet be present. Ensure it's loaded before the
     * overworld creates entities that need sprite graphics. */
    if (!sprite_grouping_ptr_table)
        load_sprite_data();

    /* Assembly: JSR INITIALIZE_OVERWORLD_STATE (main.asm line 12).
     * Creates init entity (slot 23) with EVENT_001 (main overworld tick),
     * initializes party, loads the map at leader's saved position. */
    initialize_overworld_state();

    /* Assembly: JSL OAM_CLEAR; JSL RUN_ACTIONSCRIPT_FRAME (main.asm lines 13-14).
     * The first actionscript frame executes EVENT_001 (main overworld tick),
     * which sets the UPDATE_OVERWORLD_FRAME tick callback on the init entity.
     * It also runs EVENT_002 (party follower) for party members, calling
     * INITIALIZE_PARTY_MEMBER_ENTITY (sets up sprite, but animation_frame
     * stays -1 = hidden until SET_ANIMATION opcode runs later). */
    oam_clear();
    if (run_actionscript_frame_step())
        return true;

    overworld_boot_flush();
    return false;
}

/* ---------------------------------------------------------------------------
 * Unified single-loop driver (savestate-anywhere migration, phase 1).
 *
 * The host's single top-level loop calls game_loop_step() once per frame, then
 * performs the one and only host_process_frame() yield. game_loop_step() advances
 * whatever context the game is in by one frame's worth of pre-yield work and
 * returns — it never blocks on vblank itself.
 *
 * After D3 there is ONE generic pump: game_loop_step() dispatches the mode-stack
 * top one step and applies its PUSH/POP. g_mode_stack[0] is the permanent
 * GAME_MODE_OVERWORLD root, which folds in boot (OWP_BOOT_* phases run the intro as
 * a child, then overworld_boot()) and never pops — a "Continue" game-over resets
 * the root to its boot phase instead of unwinding. Every modal context reached from
 * the overworld (battle, menus, dialogue, fades, teleport, intro, game-over) is a
 * mode pushed onto the stack — including the debug Y-button menu
 * (GAME_MODE_DEBUG_MENU) and the script/debug teleport (GAME_MODE_TELEPORT_TO). The
 * only blocking holdouts are the pump-bridge wrappers that still drive
 * already-converted modes for remaining inline callers (deleted as their callers
 * convert at the Phase D cutover, D4b). See docs/plans/savestate-unified-loop.md.
 * ------------------------------------------------------------------------- */

/* GAME_MODE_OVERWORLD step — the permanent root (g_mode_stack[0]). Boot stages
 * (OWP_BOOT_SETUP/AWAIT) fold in the former LOOP_BOOT machine, then it is the
 * run-to-completion port of overworld_post() + overworld_step() (port of main.asm
 * lines 25-161). See OverworldPhase in core/mode_stack.h for the phase map. Each
 * former inline pump-bridge driver is a STEP_PUSH with a resume phase; the render
 * tail is the OWP_RENDER per-frame CONTINUE; the post→render split + first-frame
 * render-only are preserved. The internal for(;;) advances phases with `continue`
 * (no yield) and returns on a PUSH/CONTINUE/POP. The root never pops. */
StepResult mode_step_overworld(ModeState *mst) {
    OverworldModeState *st = &mst->overworld;
    static ModeState child;  /* outlives this dispatch (root/pump copies it) */

    for (;;) {
        switch ((OverworldPhase)st->phase) {
        case OWP_BOOT_SETUP:
            /* Boot stage 1 (folded in at D3): one-shot setup. boot_begin() pushes
             * GAME_MODE_INIT_INTRO as a child (normal path) or does the skip-intro
             * init and pushes nothing. If a child was pushed, yield so it runs and
             * resume at OWP_BOOT_AWAIT on its pop; otherwise (skip-intro) flow
             * straight into overworld_boot() with no yield. */
            boot_begin();
            st->phase = OWP_BOOT_AWAIT;
            if (g_mode_stack.depth > 1)
                return STEP_RESULT_CONTINUE();
            continue;

        case OWP_BOOT_AWAIT:
            /* Boot stage 3: the intro has finished (or was skipped). overworld_boot
             * and the first render run in THIS step (the `continue` into OWP_RENDER
             * does not yield) — the assembly has no WAIT between init and the first
             * loop iteration, and a yield here flashes Ness a script frame early.
             * If the (rare) boot actionscript frame parks for a child modal, the
             * STEP_PUSH yield is unavoidable; resume at OWP_BOOT_FLUSH on its pop. */
            st->phase = OWP_RENDER;
            if (overworld_boot_step()) {
                st->phase = OWP_BOOT_FLUSH;
                return actionscript_frame_take_push();
            }
            continue;

        case OWP_BOOT_FLUSH:
            overworld_boot_flush();
            st->phase = OWP_RENDER;
            continue;

        case OWP_RENDER:
            /* Assembly lines 25-29: render frame, then advance to POST next frame.
             * This is also the entry/first-frame phase (render once before the
             * first POST, matching the assembly's pre-WAIT render). If a script
             * callroutine parks the frame for a child modal, STEP_PUSH
             * GAME_MODE_ACTIONSCRIPT_FRAME and finish the render at OWP_RENDER_FLUSH
             * when it pops. */
            oam_clear();
            st->phase = OWP_RENDER_FLUSH;
            if (run_actionscript_frame_step())
                return actionscript_frame_take_push();
            continue;   /* no park: flush in the same step (no yield) */

        case OWP_RENDER_FLUSH:
            update_screen();
            update_swirl_effect();  /* advances the battle swirl animation */
            st->phase = OWP_POST_TOP;
            return STEP_RESULT_CONTINUE();

        case OWP_POST_TOP:
            /* Assembly lines 30-42: process queued interactions (read != write
             * index) when no battle/swirl is in progress -> loop_end. */
            if (ow.current_queued_interaction != ow.next_queued_interaction &&
                !ow.battle_swirl_countdown &&
                !ow.enemy_has_been_touched &&
                !ow.battle_mode) {
                child = (ModeState){0};
                child.process_interaction.phase = PI_DISPATCH;
                st->phase = OWP_RESUME_INTERACTION;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_PROCESS_INTERACTION, &child);
            }
            /* Assembly lines 43-52: skip input in special modes -> loop_end. */
            if (game_state.camera_mode == 2 ||
                game_state.walking_style == WALKING_STYLE_ESCALATOR ||
                ow.battle_swirl_countdown) {
                st->phase = OWP_LOOP_START;
                continue;
            }
            /* Assembly lines 52-56: battle-mode transition. */
            if (ow.battle_mode) {
                child = (ModeState){0};
                child.battle_entry.phase = BE_ENTER;
                st->phase = OWP_RESUME_BATTLE;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ENTRY, &child);
            }
            /* Assembly lines 57-66: bicycle dismount (A/L while on the bicycle). */
            if ((core.pad1_pressed & PAD_CONFIRM) &&
                game_state.walking_style == WALKING_STYLE_BICYCLE) {
                disable_all_entities();
                child = (ModeState){0};
                child.bicycle_dismount.phase = BD_TEXT;
                st->phase = OWP_RESUME_BICYCLE;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BICYCLE_DISMOUNT, &child);
            }
            st->phase = OWP_POST_DEBUG;
            continue;

        case OWP_RESUME_INTERACTION:
            /* Assembly line 41: after the interaction, suppress input 1 frame. */
            ow.input_disable_frame_counter++;
            st->phase = OWP_LOOP_START;
            continue;

        case OWP_RESUME_BATTLE:
            /* Assembly line 55-56: after the battle, suppress input 1 frame and
             * jump to @CHECK_DEBUG (skipping the bicycle check). */
            ow.input_disable_frame_counter++;
            st->phase = OWP_POST_DEBUG;
            continue;

        case OWP_RESUME_BICYCLE:
            /* Assembly: after the dismount message, JMP @LOOP_BEGIN (skip damage/
             * spawn) -> render and start the next frame's POST. */
            enable_all_entities();
            st->phase = OWP_RENDER;
            continue;

        case OWP_POST_DEBUG:
            /* Assembly lines 67-88: @CHECK_DEBUG. (B|SELECT) held + R just pressed
             * -> the debug Y-button menu (GAME_MODE_DEBUG_MENU). On its POP, resume
             * at OWP_RENDER (the assembly JMP @LOOP_BEGIN skips damage/spawn). */
            if (ow.debug_flag) {
                if (debug_menu_requested ||
                    ((core.pad1_held & PAD_CANCEL) && (core.pad1_pressed & PAD_R))) {
                    debug_menu_requested = false;
                    child = (ModeState){0};
                    child.debug_menu.phase = DM_ENTER;
                    st->phase = OWP_RENDER;
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DEBUG_MENU, &child);
                }
            }
            debug_menu_requested = false;

            /* Assembly lines 89-92: skip the rest if swirl/enemy active. */
            if (ow.battle_swirl_countdown || ow.enemy_has_been_touched) {
                st->phase = OWP_RENDER;
                continue;
            }
            st->phase = OWP_POST_INPUT;
            continue;

        case OWP_POST_INPUT:
            /* Assembly lines 93-124: button input processing. The four button
             * handlers all resume at the teleport check (OWP_POST_TELEPORT). */
            if (ow.input_disable_frame_counter > 0) {
                ow.input_disable_frame_counter--;
            } else if (!ow.pending_interactions) {
                if (core.pad1_pressed & PAD_A) {
                    child = (ModeState){0};
                    child.pause_menu.phase = PM_ENTER;
                    st->phase = OWP_POST_TELEPORT;
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_PAUSE_MENU, &child);
                } else if ((core.pad1_pressed & PAD_CANCEL) &&
                           game_state.walking_style != WALKING_STYLE_BICYCLE) {
                    child = (ModeState){0};
                    child.hppp_display.phase = HD_ENTER;
                    st->phase = OWP_POST_TELEPORT;
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_HPPP_DISPLAY, &child);
                } else if (core.pad1_pressed & PAD_X) {
                    /* show_town_map(): push the display variant if there is a map;
                     * OWP_RESUME_TOWNMAP re-enables entities afterward. No map ->
                     * fall through to the teleport check (no push). */
                    if (show_town_map_prepare(&child)) {
                        st->phase = OWP_RESUME_TOWNMAP;
                        return STEP_RESULT_PUSH_INIT(GAME_MODE_TOWN_MAP, &child);
                    }
                } else if (core.pad1_pressed & PAD_L) {
                    child = (ModeState){0};
                    child.quick_checktalk.phase = QCT_TEXT;
                    st->phase = OWP_POST_TELEPORT;
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_QUICK_CHECKTALK, &child);
                }
            }
            st->phase = OWP_POST_TELEPORT;
            continue;

        case OWP_RESUME_TOWNMAP:
            /* show_town_map()'s enable bracket, after the town map pops. */
            enable_all_entities();
            st->phase = OWP_POST_TELEPORT;
            continue;

        case OWP_POST_TELEPORT:
            /* Assembly lines 125-128: PSI teleport. Resume at loop_end. */
            if (ow.psi_teleport_destination) {
                st->phase = OWP_LOOP_START;
                return STEP_RESULT_PUSH(GAME_MODE_TELEPORT);
            }
            st->phase = OWP_LOOP_START;
            continue;

        case OWP_LOOP_START:
            /* Assembly lines 152-161: begin a fresh UPDATE_OVERWORLD_DAMAGE pass. */
            st->dmg = (OwDamageState){0};
            st->phase = OWP_LOOP_END;
            continue;

        case OWP_LOOP_END:
            /* Drive the damage loop. A mid-loop low-HP warning is a STEP_PUSH
             * (HP_ALERT); ow_damage_step() resumes via st->dmg on re-entry (we
             * stay in OWP_LOOP_END, which does NOT reset st->dmg). On completion,
             * a total HP of 0 pushes GAME_MODE_GAME_OVER (spawn()). */
            if (ow_damage_step(&st->dmg) == OW_DMG_ALERT) {
                child = (ModeState){0};
                child.hp_alert.phase = HA_TEXT;
                child.hp_alert.party_index = st->dmg.i;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_HP_ALERT, &child);
            }
            if (st->dmg.total_hp == 0) {
                child = (ModeState){0};
                child.game_over.phase = GO_ENTER;
                st->phase = OWP_GAMEOVER_RESULT;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_GAME_OVER, &child);
            }
            st->phase = OWP_RENDER;
            continue;

        case OWP_GAMEOVER_RESULT:
        default:
            /* GAME_OVER popped -1 ("Continue" -> reboot) or 0 ("No Continue" ->
             * the world was reinitialised; render and carry on). The root never
             * pops; a "Continue" reboot resets this root back to its boot phase
             * (replaying boot_begin/intro), mirroring the former g_loop_mode reset
             * to LOOP_BOOT. */
            if (mode_child_result() != 0) {
                *st = (OverworldModeState){ .phase = OWP_BOOT_SETUP };
                return STEP_RESULT_CONTINUE();
            }
            st->phase = OWP_RENDER;
            continue;
        }
    }
}

/* Advance the game by one frame's pre-yield work. Called once per frame by the
 * host's single top-level loop; the host performs host_process_frame() after.
 *
 * The single generic pump (D3): dispatch the mode-stack top one step, then apply
 * its PUSH/POP. The same body as pump_mode()'s loop, minus the floor and the host
 * yield (the host owns the one yield). g_mode_stack[0] is the permanent
 * GAME_MODE_OVERWORLD root (pushed in game_init); its OWP_BOOT_* phases fold in the
 * former BOOT machine and its OWP_GAMEOVER "Continue" path resets itself to boot
 * rather than popping, so the root never pops and the stack is never empty. */
void game_loop_step(void) {
    uint8_t top = (uint8_t)(g_mode_stack.depth - 1);
    StepResult r = mode_dispatch_step((GameMode)g_mode_stack.mode[top],
                                      &g_mode_stack.state[top]);
    if (r.kind == STEP_PUSH) {
        mode_push(r.push_mode, r.push_init);
    } else if (r.kind == STEP_POP) {
        mode_pop(r.pop_result);
    }
    /* STEP_CONTINUE: nothing extra; the host yields after this returns. */
}

/* Legacy entry point retained for ports that drive the game with
 * `for (;;) game_logic_entry();` (snes, waveshare, gw_retro_go). Each call now
 * advances exactly one frame: one step plus the single host yield. */
void game_logic_entry(void) {
    game_loop_step();
    host_process_frame();
    host_root_boundary(); /* root boundary: perform any pending torn-safe capture */
}

bool game_is_fast_forward(void) {
    return fast_forward_active;
}

void game_init(void) {
    memory_init();
    ppu_init();

    rng_seed(0x56781234U);
    game_state_init();
    floating_sprite_table_load();

    /* Push the permanent GAME_MODE_OVERWORLD root (g_mode_stack[0]) so the first
     * game_loop_step() dispatches its OWP_BOOT_SETUP phase (boot_begin -> intro).
     * The root is never popped; the stack is never empty after this. */
    ModeState root = { .overworld = { .phase = OWP_BOOT_SETUP } };
    mode_push(GAME_MODE_OVERWORLD, &root);
}
