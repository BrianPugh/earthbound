#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_main.h"
#include "core/types.h"
#include "core/log.h"
#include "core/memory.h"
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

/* Verbosity level (0=errors, 1=warnings, 2=trace) */
int verbose_level = 0;

/* Auto-dump flag: set to non-zero to trigger a screenshot + VRAM dump */
int debug_auto_dump_requested = 0;

static bool show_fps = false;
static bool fast_forward_active = false;
static bool debug_menu_requested = false;
static uint16_t aux_prev = 0;

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
#define FPS_OVERLAY_LINES 11
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
    int ox = VIEWPORT_WIDTH - overlay_w - 1;
    pixel_t color = fps_overlay.colors[text_row];

    /* Black background */
    for (int x = ox - 1; x < VIEWPORT_WIDTH; x++)
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
                    if (px >= 0 && px < VIEWPORT_WIDTH)
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
    if (aux_new & AUX_FAST_FORWARD)
        fast_forward_active = !fast_forward_active;
    if (aux_new & AUX_STATE_DUMP)
        platform_debug_dump_state();
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

            /* Sleep if ahead of schedule */
            if (!platform_headless)
                platform_timer_sleep_until(frame_deadline);

            /* Update FPS IIR filter — only on rendered frames so the
             * counter shows actual display refresh rate, not game logic rate. */
            if (do_render)
                platform_timer_update_fps();
        }
        if (do_render)
            platform_timer_frame_start();
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
 * debug_y_button_flag — Port of DEBUG_Y_BUTTON_FLAG (asm/overworld/debug/y_button_flag.asm).
 *
 * Interactive event flag editor. Shows current flag index and ON/OFF state.
 * D-pad navigates (up/down by 1, left/right by 10), A toggles, B exits.
 */
static void debug_y_button_flag(void) {
    uint16_t flag_index = 1;  /* Start at FLG_TEMP_0 (index 1) */

    for (;;) {
        set_instant_printing();
        create_window(WINDOW_FILE_SELECT_MENU);
        set_window_number_padding(3);

        /* Print flag index */
        print_number((int)flag_index, 1);

        /* Print space */
        print_char_with_sound(0x0020);
        advance_vwf_tile();

        /* Print ON/OFF status */
        bool is_set = event_flag_get(flag_index);
        if (is_set)
            print_string("ON");
        else
            print_string("OFF");

        clear_instant_printing();
        window_tick();

        uint16_t new_index = flag_index;

        /* Wait for input */
        for (;;) {
            wait_for_vblank();

            if (core.pad1_held & PAD_UP) {
                new_index = flag_index + 1;
                break;
            }
            if (core.pad1_held & PAD_DOWN) {
                new_index = flag_index - 1;
                break;
            }
            if (core.pad1_held & PAD_RIGHT) {
                new_index = flag_index + 10;
                break;
            }
            if (core.pad1_held & PAD_LEFT) {
                new_index = flag_index - 10;
                break;
            }
            if (core.pad1_pressed & PAD_CONFIRM) {
                /* Toggle flag */
                if (event_flag_get(flag_index))
                    event_flag_clear(flag_index);
                else
                    event_flag_set(flag_index);
                break;
            }
            if (core.pad1_pressed & PAD_CANCEL) {
                close_window(WINDOW_FILE_SELECT_MENU);
                return;
            }
        }

        /* Validate new index: must be 1-1999 (assembly: >= 2000 or == 0 → keep old) */
        if (new_index > 0 && new_index < 2000)
            flag_index = new_index;
    }
}

/*
 * debug_y_button_goods — Port of DEBUG_Y_BUTTON_GOODS (asm/overworld/debug/y_button_goods.asm).
 *
 * Interactive item browser/giver. Shows item ID and name.
 * D-pad navigates (up/down by 1, left/right by 10), A gives item to a
 * selected character (auto-equips weapons), B exits.
 */
static void debug_y_button_goods(void) {
    uint16_t item_id = 0;

    for (;;) {
        set_instant_printing();
        create_window(WINDOW_FILE_SELECT_MENU);
        set_window_number_padding(2);

        /* US: set padding to 130, cursor to (0,0), print number, cursor to (3,0) */
        set_window_number_padding(130);
        set_focus_text_cursor(0, 0);
        print_number((int)item_id, 1);
        set_focus_text_cursor(3, 0);

        /* Print item name (EB-encoded, 25 chars max) */
        const ItemConfig *item = get_item_entry(item_id);
        if (item)
            print_text_with_word_splitting(item->name, ITEM_NAME_LEN);

        clear_instant_printing();
        window_tick();

        uint16_t new_id = item_id;

        for (;;) {
            wait_for_vblank();

            if (core.pad1_held & PAD_UP) {
                new_id = item_id + 1;
                break;
            }
            if (core.pad1_held & PAD_DOWN) {
                new_id = item_id - 1;
                break;
            }
            if (core.pad1_held & PAD_RIGHT) {
                new_id = item_id + 10;
                break;
            }
            if (core.pad1_held & PAD_LEFT) {
                new_id = item_id - 10;
                break;
            }
            if (core.pad1_pressed & PAD_CONFIRM) {
                /* Give item to selected character */
                uint16_t char_id = char_select_prompt(1, 1, NULL, NULL);
                if (char_id == 0)
                    break;  /* Cancelled */
                if (find_inventory_space2(char_id) == 0)
                    break;  /* No room */
                give_item_to_character(char_id, item_id);
                /* Auto-equip if it's a weapon/armor type (type == 2) */
                if (check_item_usable_by(char_id, item_id) == 0)
                    goto exit;
                if (get_item_type(item_id) != 2)
                    goto exit;
                uint16_t slot = find_empty_inventory_slot(char_id);
                equip_item(char_id, slot);
                goto exit;
            }
            if (core.pad1_pressed & PAD_CANCEL) {
                goto exit;
            }
        }

        /* Validate: assembly uses CMP #$0100 (256), unsigned */
        if (new_id < 256)
            item_id = new_id;
    }

exit:
    close_window(WINDOW_FILE_SELECT_MENU);
}

/*
 * debug_y_button_guide — Port of DEBUG_Y_BUTTON_GUIDE (asm/overworld/debug/y_button_guide.asm).
 *
 * Counts entities with active scripts (script_table != -1) and displays
 * the count. Press B to dismiss.
 */
static void debug_y_button_guide(void) {
    /* Count entities with active scripts */
    int count = 0;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (entities.script_table[ENT(i)] != -1)
            count++;
    }

    set_instant_printing();
    create_window(WINDOW_FILE_SELECT_MENU);
    set_window_number_padding(3);
    print_number(count, 1);
    clear_instant_printing();
    window_tick();

    /* Wait for B/SELECT to exit */
    for (;;) {
        if (core.pad1_pressed & PAD_CANCEL)
            break;
        wait_for_vblank();
    }

    close_window(WINDOW_FILE_SELECT_MENU);
}

/*
 * debug_teleport — Port of TELEPORT (asm/overworld/teleport.asm).
 *
 * Full teleport to a destination table entry. Used after CAST and CREDITS
 * to return the player to the overworld. The assembly calls this as a near
 * function (JSR TELEPORT) with the destination index in A.
 *
 * This is extracted from cc_1f_teleport_to() in display_text_cc.c which
 * implements the same logic but reads dest_id from a script stream.
 */
static void debug_teleport(uint8_t dest_id) {
    const TeleportDestination *dest = get_teleport_dest(dest_id);
    if (!dest) return;

    uint8_t saved_suppression = ow.overworld_status_suppression;
    ow.overworld_status_suppression = 1;

    /* Clear temp event flags 1-10 */
    for (int i = 1; i <= 10; i++)
        event_flag_clear((uint16_t)i);

    process_door_interactions();

    /* Screen transition out */
    uint16_t sfx = get_screen_transition_sound_effect(dest->screen_transition, 1);
    play_sfx(sfx);
    if (ow.disabled_transitions)
        fade_out(1, 1);
    else
        screen_transition(dest->screen_transition, 1);

    uint16_t x_pixels = dest->x_coord * 8;
    uint16_t y_pixels = dest->y_coord * 8;
    uint16_t direction_param = (dest->direction & 0x7F) - 1;

    load_map_at_position(x_pixels, y_pixels);
    ow.player_has_moved_since_map_load = 0;
    set_leader_position_and_load_party(x_pixels, y_pixels, direction_param);

    if (dest->direction & 0x80)
        fill_party_position_buffer(direction_param);

    resolve_map_sector_music(x_pixels, y_pixels);
    apply_next_map_music();

    if (ow.post_teleport_callback) {
        ow.post_teleport_callback();
        ow.post_teleport_callback = NULL;
    }

    flush_entity_creation_queue();

    /* Screen transition in */
    sfx = get_screen_transition_sound_effect(dest->screen_transition, 0);
    play_sfx(sfx);
    if (ow.disabled_transitions)
        fade_in(1, 1);
    else
        screen_transition(dest->screen_transition, 0);

    ow.stairs_direction = (uint16_t)-1;
    spawn_buzz_buzz();
    ow.overworld_status_suppression = saved_suppression;
}

/*
 * DEBUG_Y_BUTTON_MENU — Port of asm/system/debug/y_button_menu.asm.
 *
 * Debug menu triggered by holding B or SELECT + pressing R in the overworld
 * when ow.debug_flag is set. Opens a scrollable menu with debug commands.
 *
 * Menu items match DEBUG_MENU_TEXT in asm/data/debug/menu_text.asm (US):
 *  1=Flag  2=Goods  3=Save  4=Apple  5=Banana  6=TV  7=Event  8=Warp
 *  9=Tea  10=Teleport  11=Star~  12=Star^  13=Player0  14=Player1
 *  15=GUIDE  16=TRACK  17=CAST  18=STONE  19=STAFF  20=Meter
 *  21=REPLAY  22=TEST1  23=TEST2  24=(disable replay)
 */
static void debug_y_button_menu(void) {
    disable_all_entities();
    play_sfx(1);  /* SFX::CURSOR1 */
    show_hppp_windows();

    /* Assembly uses @LOCAL04 (US) / @VIRTUAL06 (JPN) as a message pointer.
     * When non-NULL after a command, it opens TEXT_STANDARD and displays that text.
     * This implements the @AFTER_COMMAND message display loop. */
    uint32_t message_addr = 0;

display_menu:
    create_window(WINDOW_PHONE_MENU);

    /* Build menu items matching DEBUG_MENU_TEXT order */
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
    uint16_t result = selection_menu(1);  /* allow_cancel=1 */
    message_addr = 0;

    switch (result) {
    case 1:
        /* FLAGS — assembly @CMD_FLAGS (line 107): JSL DEBUG_Y_BUTTON_FLAG. */
        debug_y_button_flag();
        break;
    case 2:
        /* GOODS — assembly @CMD_GOODS (line 110): JSL DEBUG_Y_BUTTON_GOODS. */
        debug_y_button_goods();
        break;
    case 3:
        /* SAVE — assembly @CMD_SAVE (lines 113-118).
         * Save current game and update respawn coordinates. */
        save_game(current_save_slot - 1);
        ow.respawn_x = game_state.leader_x_coord;
        ow.respawn_y = game_state.leader_y_coord;
        break;
    case 4:
        /* Apple (MSG_DEBUG_00) — assembly @CMD_MSG_00 (lines 119-124).
         * Sets message pointer to MSG_DEBUG_00 (0xC58000). */
        message_addr = 0xC58000;
        break;
    case 5:
        /* Banana (MSG_DEBUG_01) — assembly @CMD_MSG_01 (lines 125-130). */
        message_addr = 0xC58ED1;
        break;
    case 6:
        /* TV (MSG_DEBUG_02) — assembly @CMD_MSG_02 (lines 131-136). */
        message_addr = 0xC58DBF;
        break;
    case 7:
        /* Event (TEXT_DEBUG_UNKNOWN_MENU_2) — assembly @CMD_MSG_UNKNOWN (lines 137-142). */
        message_addr = 0xEFA6EC;
        break;
    case 8: {
        /* WARP — assembly @CMD_WARP (lines 143-179).
         * Flash HP/PP windows 30 times, then teleport to Onett center (7696, 2280). */
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
        break;
    }
    case 9: {
        /* Tea (COFFEE/TEA) — assembly @CMD_COFFEE_TEA (lines 180-184).
         * Random coffee or tea scene. */
        uint16_t type = rng_next_byte() & 1;
        coffeetea_scene(type);
        break;
    }
    case 10:
        /* Teleport (PSI_1) — assembly @CMD_PSI_1 (lines 185-188).
         * Learn special PSI type 1. */
        learn_special_psi(1);
        break;
    case 11:
        /* Star ~ (PSI_2) — assembly @CMD_PSI_2 (lines 189-191).
         * Learn special PSI type 2. */
        learn_special_psi(2);
        break;
    case 12:
        /* Star ^ (PSI_3_4) — assembly @CMD_PSI_3_4 (lines 193-198).
         * Learn special PSI types 3 and 4. */
        learn_special_psi(3);
        learn_special_psi(4);
        break;
    case 13:
        /* Player 0 (NAME_0) — assembly @CMD_NAME_0 (lines 199-202).
         * Open naming screen for character 0 (Ness). */
        enter_your_name_please(0);
        break;
    case 14:
        /* Player 1 (NAME_1) — assembly @CMD_NAME_1 (lines 203-210).
         * Open naming screen for character 1 (Paula). */
        enter_your_name_please(1);
        break;
    case 15:
        /* GUIDE — assembly @CMD_TOWN_MAP (lines 211-213): JSL RUN_TOWN_MAP_MENU.
         * Note: menu label says "GUIDE" but assembly dispatches to town map. */
        run_town_map_menu();
        break;
    case 16:
        /* TRACK — assembly @CMD_GUIDE (lines 214-216): JSL DEBUG_Y_BUTTON_GUIDE. */
        debug_y_button_guide();
        break;
    case 17:
        /* CAST — assembly @CMD_CAST (lines 217-221).
         * Play cast scene, then teleport to destination 1 to return to overworld. */
        play_cast_scene();
        debug_teleport(1);
        break;
    case 18:
        /* STONE — assembly @CMD_SOUND_STONE (lines 222-225).
         * Play sound stone melody (cancellable=1). */
        use_sound_stone(1);
        break;
    case 19:
        /* STAFF (CREDITS) — assembly @CMD_CREDITS (lines 226-230).
         * Play credits, then teleport to destination 1. */
        play_credits();
        debug_teleport(1);
        break;
    case 20:
        /* Meter (FLIPOUT) — assembly @CMD_FLIPOUT (lines 231-239).
         * Toggle HP/PP meter flipout mode. */
        toggle_hppp_flipout_mode(bt.hppp_meter_flipout_mode ? 0 : 1);
        break;
    case 21:
        /* REPLAY — assembly @CMD_REPLAY (lines 240-242): JSL START_REPLAY_MODE.
         * Replay recording system not applicable to C port (requires SNES SRAM).
         * Assembly goes directly to cleanup. */
        goto cleanup;
    case 22:
        /* TEST1 (MSG_BTL) — assembly @CMD_MSG_BTL (lines 243-248).
         * Sets message pointer to MSG_BTL_INORU_BACK_TO_PC_9 (0xC9F70C). */
        message_addr = 0xC9F70C;
        break;
    case 23: {
        /* TEST2 (TO_BE_CONTINUED) — assembly @CMD_TO_BE_CONTINUED (lines 249-255).
         * Close windows, display "To Be Continued" text, then cleanup. */
        close_all_windows();
        hide_hppp_windows();
        display_text_from_snes_addr(0xC9C7FA);  /* MSG_EVT_TO_BE_CONTINUED */
        goto cleanup;
    }
    default:
        /* Cancelled or item 24+ → DISABLE_REPLAY_MODE.
         * Assembly @CMD_DISABLE_REPLAY (lines 256-258).
         * Replay system not applicable to C port. Goes directly to cleanup. */
        goto cleanup;
    }

    /* Assembly @AFTER_COMMAND (lines 259-275):
     * If a message address was set, close the menu window, open TEXT_STANDARD,
     * display the message text, then loop back to show the menu again.
     * If no message, just redisplay the menu. */
    if (message_addr != 0) {
        close_focus_window();
        create_window(WINDOW_TEXT_STANDARD);
        display_text_from_snes_addr(message_addr);
    }
    goto display_menu;

cleanup:
    /* Assembly @CLEANUP (lines 276-285) */
    close_all_windows();
    hide_hppp_windows();

    /* Wait for entity fade to complete (assembly @WAIT_FADE, lines 279-283) */
    while (ow.entity_fade_entity != (int16_t)-1) {
        window_tick();
    }

    enable_all_entities();
}

/* Game logic entry point — called from main().
 * Contains intro, initialization, and the main game loop.
 * Returns on game-over Continue (main's for-loop re-calls for restart)
 * or when quit is requested (exit(0) from wait_for_vblank). */
void game_logic_entry(void) {
    /* Run the intro sequence (logo → gas station → title → file select).
     * In the assembly, MAIN_LOOP calls INIT_INTRO → FILE_SELECT_INIT →
     * INITIALIZE_OVERWORLD_STATE. The C port embeds file select inside
     * init_intro(), so after init_intro() returns, we go directly to
     * overworld initialization. */
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
        init_intro();
    }

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
    run_actionscript_frame();

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

    /* Main game loop (port of main.asm @LOOP_BEGIN, lines 24-171).
     *
     * The full assembly loop handles:
     *   - OAM_CLEAR + RUN_ACTIONSCRIPT_FRAME + UPDATE_SCREEN
     *   - UPDATE_SWIRL_EFFECT + WAIT_UNTIL_NEXT_FRAME
     *   - Queued interaction processing (NPC talk, item use, etc.)
     *   - Battle mode transitions (INIT_BATTLE_OVERWORLD)
     *   - Button input (A=talk/check, B/Select=HP display, X=town map, L=talk/check)
     *   - Bicycle dismount (A+L while walking_style==BICYCLE)
     *   - PSI Teleport processing
     *   - UPDATE_OVERWORLD_DAMAGE + SPAWN (enemy encounters) */
    while (!platform_input_quit_requested()) {
        /* Assembly lines 25-29: render frame */
        oam_clear();
        run_actionscript_frame();
        update_screen();
        /* Assembly line 28: UPDATE_SWIRL_EFFECT (advances battle swirl animation) */
        update_swirl_effect();

        wait_for_vblank();

        /* Assembly lines 30-42: process queued interactions.
         * Check if there are queued interactions (read != write index)
         * and no battle/swirl is in progress. */
        if (ow.current_queued_interaction != ow.next_queued_interaction &&
            !ow.battle_swirl_countdown &&
            !ow.enemy_has_been_touched &&
            !ow.battle_mode) {
            process_queued_interactions();
            ow.input_disable_frame_counter++;
            goto loop_end;  /* Assembly: JMP @LOOP_END (runs damage update + spawn) */
        }

        /* Assembly lines 43-52: skip input processing in special modes */
        if (game_state.camera_mode == 2 ||
            game_state.walking_style == WALKING_STYLE_ESCALATOR ||
            ow.battle_swirl_countdown)
            goto loop_end;

        /* Assembly lines 52-56: battle mode transition.
         * Port of main.asm: if BATTLE_MODE != 0, call INIT_BATTLE_OVERWORLD
         * and increment INPUT_DISABLE_FRAME_COUNTER to suppress input for 1 frame. */
        if (ow.battle_mode) {
            init_battle_overworld();
            ow.input_disable_frame_counter++;
            goto after_bicycle;  /* Assembly line 56: BRA @CHECK_DEBUG (skips bicycle) */
        }

        /* Assembly lines 57-66: bicycle dismount check.
         * If A or L pressed while walking_style == BICYCLE (3), dismount. */
        if ((core.pad1_pressed & PAD_CONFIRM) &&
            game_state.walking_style == WALKING_STYLE_BICYCLE) {
            disable_all_entities();
            get_off_bicycle_with_message();
            enable_all_entities();
            continue;  /* restart loop */
        }

after_bicycle:
        /* Assembly lines 67-88: @CHECK_DEBUG — debug menu and debug key checks.
         * When ow.debug_flag is set, special button combos trigger debug features.
         * (B or SELECT) held + R pressed → DEBUG_Y_BUTTON_MENU.
         * Full menu not ported; dispatches available debug commands. */
        if (ow.debug_flag) {
            /* (B|SELECT) held + R just pressed → debug Y button menu (lines 70-77) */
            if (debug_menu_requested ||
                ((core.pad1_held & PAD_CANCEL) && (core.pad1_pressed & PAD_R))) {
                debug_menu_requested = false;
                debug_y_button_menu();
                continue;  /* JMP @LOOP_BEGIN */
            }
        }
        debug_menu_requested = false;

        /* Assembly lines 89-92: skip remaining checks if swirl/enemy active */
        if (ow.battle_swirl_countdown || ow.enemy_has_been_touched)
            continue;  /* jump to @LOOP_BEGIN */

        /* Assembly lines 93-124: button input processing */
        if (ow.input_disable_frame_counter > 0) {
            /* Assembly line 124: DEC INPUT_DISABLE_FRAME_COUNTER */
            ow.input_disable_frame_counter--;
        } else if (!ow.pending_interactions) {
            /* Assembly lines 97-100: A → OPEN_MENU_BUTTON (full pause menu) */
            if (core.pad1_pressed & PAD_A) {
                open_menu_button();
            }
            /* Assembly lines 103-110: B/Select → OPEN_HPPP_DISPLAY */
            else if ((core.pad1_pressed & PAD_CANCEL) &&
                     game_state.walking_style != WALKING_STYLE_BICYCLE) {
                open_hppp_display();
            }
            /* Assembly lines 112-116: X → SHOW_TOWN_MAP */
            else if (core.pad1_pressed & PAD_X) {
                show_town_map();
            }
            /* Assembly lines 118-121: L → OPEN_MENU_BUTTON_CHECKTALK */
            else if (core.pad1_pressed & PAD_L) {
                open_menu_button_checktalk();
            }
        }

        /* Assembly lines 125-128: PSI teleport processing */
        if (ow.psi_teleport_destination) {
            teleport_mainloop();
        }

loop_end:
        /* Assembly lines 152-161: UPDATE_OVERWORLD_DAMAGE + SPAWN.
         * update_overworld_damage() applies poison/environmental damage each frame.
         * If it returns 0 (all party KO'd), SPAWN handles game-over/comeback.
         * If SPAWN returns non-zero (player chose Continue), restart the main loop
         * via LONGJMP(JMP_BUF1) equivalent (assembly lines 158-161).
         * Returning from game_logic_entry causes main's for-loop to
         * re-call us, restarting from the intro/init sequence. */
        if (update_overworld_damage() == 0) {
            if (spawn() != 0)
                return;
        }
    }
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
}
