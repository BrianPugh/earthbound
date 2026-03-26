#ifndef PLATFORM_H
#define PLATFORM_H

#include "core/types.h"

/*
 * Porting Guide: Input Mapping
 *
 * EarthBound uses 8 SNES action buttons, but most have redundant functions.
 * The game checks button *groups* for most actions (see PAD_CONFIRM, PAD_CANCEL,
 * etc. in pad.h). Ports with fewer buttons should prioritize mapping:
 *
 * REQUIRED (minimum playable):
 *   D-pad       — Movement and menu navigation
 *   PAD_A       — Confirm selection, open command window, interact
 *   PAD_B       — Cancel, back, show HP/PP window on overworld
 *
 * RECOMMENDED:
 *   PAD_X       — Toggle town map (unique function, no equivalent)
 *   PAD_START   — Start game from title screen (only use; could be combined)
 *   PAD_L       — "Check/talk to" shortcut on overworld (like A but tries talk first)
 *
 * OPTIONAL (cosmetic or redundant):
 *   PAD_SELECT  — Duplicate of B (for one-handed play)
 *   PAD_R       — Rings bicycle bell (cosmetic sound effect only)
 *   PAD_Y       — No function in normal gameplay
 *
 * For platforms with limited buttons, consider mapping unused physical buttons
 * to PAD_L (for check/talk) rather than PAD_Y or PAD_R.
 */

/* Global mode flags (set before init, read by platform backends) */
extern bool platform_headless;    /* --headless: no window, no frame timing */
extern bool platform_skip_intro;  /* --skip-intro: skip to overworld for debugging */
extern int platform_max_frames;   /* --frames N: quit after N frames (0=unlimited) */

/*
 * Video — scanline-based rendering
 *
 * The PPU renders one scanline at a time. Each frame follows this sequence:
 *   1. platform_video_begin_frame()       — prepare for a new frame
 *   2. platform_video_send_scanline(y, p) — called for y=0..height-1 with RGBA pixels
 *   3. platform_video_end_frame()         — present the completed frame
 *
 * platform_video_get_framebuffer() returns a pointer to the full frame pixel
 * buffer (for desktop backends that texture-upload), or NULL on embedded
 * targets that stream scanlines directly to the display.
 *
 * platform_render_frame() encapsulates the full begin/render/end sequence.
 * On single-core platforms it calls begin_frame, ppu_render_frame, end_frame.
 * Dual-core platforms override this to distribute scanline rendering across
 * cores (e.g., even/odd scanline interleaving with a ring buffer).
 * The fps_overlay_cb, if non-NULL, is called for each scanline to stamp
 * the FPS overlay text before output.
 */
typedef void (*scanline_stamp_cb_t)(int y, pixel_t *pixels);
bool platform_video_init(void);
void platform_video_shutdown(void);
void platform_video_begin_frame(void);
void platform_video_send_scanline(int y, const pixel_t *pixels);
pixel_t *platform_video_get_framebuffer(void);
void platform_video_end_frame(void);
void platform_video_set_vsync(bool enabled);
void platform_render_frame(scanline_stamp_cb_t fps_overlay_cb);

/*
 * Input
 *
 * Pad state is a uint16_t bitmask of SNES joypad buttons (see pad.h).
 * Each bit corresponds to one button — e.g. PAD_A (0x0080), PAD_B (0x8000).
 * Game logic tests buttons via groups like PAD_CONFIRM (A|L) and PAD_CANCEL
 * (B|SELECT), so ports can map physical buttons to whichever SNES buttons
 * make sense for the hardware.
 *
 * Aux state is a uint16_t bitmask of non-game buttons (debug, turbo, etc.).
 * Ports report raw level state (bit set = button currently held). Edge
 * detection and toggle logic are handled centrally in game_main.c, so
 * ports never need to debounce or track previous state for aux buttons.
 *
 * platform_input_poll() is called once per frame before any pad/aux reads.
 * It must sample hardware and update internal state so that get_pad(),
 * get_pad_new(), and get_aux() return consistent values for the frame.
 */

/* Aux button bitmask — non-game actions (debug, turbo, etc.) */
#define AUX_DEBUG_DUMP   (1 << 0)   /* F1: dump PPU state */
#define AUX_VRAM_DUMP    (1 << 1)   /* F2: dump VRAM as image */
#define AUX_FPS_TOGGLE   (1 << 2)   /* F3: toggle FPS overlay */
#define AUX_STATE_DUMP   (1 << 3)   /* F4: dump game state */
#define AUX_FAST_FORWARD (1 << 4)   /* Tab: toggle 4x speed */
#define AUX_DEBUG_TOGGLE (1 << 5)   /* `: toggle debug mode */

bool platform_input_init(void);
void platform_input_shutdown(void);
void platform_input_poll(void);                       /* sample hardware — call once per frame */
uint16_t platform_input_get_pad(void);                /* bitmask of currently held buttons (pad.h PAD_* flags) */
uint16_t platform_input_get_pad_new(void);            /* bitmask of buttons pressed this frame (not held last frame) */
uint16_t platform_input_get_aux(void);                /* bitmask of currently held aux buttons (AUX_* flags) */
bool platform_input_quit_requested(void);             /* user wants to exit (e.g. window close, Escape) */
void platform_request_quit(void);                     /* programmatic quit (e.g. from game menu) */

/*
 * Audio
 *
 * The audio subsystem runs the SPC700/DSP emulator on a separate thread
 * (desktop) or is disabled entirely (embedded without audio hardware).
 * platform_audio_lock/unlock() guard shared state between the audio
 * callback thread and the main game thread (e.g. when writing APU ports).
 */
bool platform_audio_init(void);
void platform_audio_shutdown(void);
void platform_audio_lock(void);                       /* acquire audio mutex (pairs with unlock) */
void platform_audio_unlock(void);

/*
 * Save data — persistent storage
 *
 * The game calls platform_save_read/write with byte offsets and sizes
 * into a flat SAVE_FILE_SIZE byte buffer (7680 bytes = 3 slots × 2 copies
 * × 1280 bytes). On desktop this is a file; on SNES it's SRAM; on
 * embedded it might be flash.
 *
 * platform_save_init() is called once at startup. Returns false if the
 * storage backend can't be initialized.
 *
 * platform_save_read() returns the number of bytes actually read (0 if
 * no save exists). platform_save_write() returns true on success.
 */
bool platform_save_init(void);
size_t platform_save_read(void *dst, size_t offset, size_t size);
bool platform_save_write(const void *src, size_t offset, size_t size);

/* Debug dumps (desktop: write files to debug/; embedded: no-op) */
void platform_debug_dump_ppu(const pixel_t *framebuffer);
void platform_debug_dump_vram_image(void);
void platform_debug_dump_state(void);

/*
 * Timer — frame pacing
 *
 * Call platform_timer_frame_start() at the top of the game loop and
 * platform_timer_frame_end() at the bottom. frame_end() sleeps or
 * busy-waits to maintain ~60 fps (NTSC timing).
 *
 * platform_timer_ticks() / ticks_per_sec() provide a monotonic clock
 * for measuring elapsed time. get_fps_tenths() returns a smoothed FPS
 * value multiplied by 10 (e.g. 600 = 60.0 fps) for the FPS overlay.
 */
bool platform_timer_init(void);
void platform_timer_shutdown(void);
void platform_timer_frame_start(void);
void platform_timer_frame_end(void);
void platform_timer_update_fps(void);               /* update IIR FPS filter (call every frame) */
void platform_timer_sleep_until(uint64_t deadline);  /* sleep until the given tick value */
uint64_t platform_timer_ticks(void);
uint64_t platform_timer_ticks_per_sec(void);
uint32_t platform_timer_get_fps_tenths(void);

#endif /* PLATFORM_H */
