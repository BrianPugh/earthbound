#ifndef GAME_MAIN_H
#define GAME_MAIN_H

#include "core/types.h"

/* Initialize core game systems (memory, PPU, RNG, game state, door/sprite tables).
 * Call once before game_logic_entry(). */
void game_init(void);

/* Advance the game by one frame's worth of pre-yield work, then return.
 * The host's single top-level loop calls this once per frame and performs the
 * one host_process_frame() yield afterward. Restart-on-game-over is handled
 * internally (transition back to the BOOT state), so no restart wrapper is
 * needed. See docs/plans/savestate-unified-loop.md. */
void game_loop_step(void);

/* Legacy per-frame entry point: one game_loop_step() plus one
 * host_process_frame() yield. Retained for ports that drive the game with
 * `for (;;) game_logic_entry();` (snes, waveshare, gw_retro_go). */
void game_logic_entry(void);

/* Host-side per-frame processing (rendering, input, audio, timing).
 * Called by the host main loop after each fiber yield. */
void host_process_frame(void);

/* Wait for one frame (NMI equivalent). Yields the game fiber. */
void wait_for_vblank(void);

/* Wait for N frames or until a button is pressed. Returns true if pressed. */
bool wait_frames_or_button(uint16_t count, uint16_t button_mask);

/* Returns true when turbo/fast-forward mode is active.
 * Toggled by AUX_FAST_FORWARD; used by timer and audio backends. */
bool game_is_fast_forward(void);

/* ── Capture-safety gate (savestate-anywhere; docs/plans/savestate-unified-loop.md,
 * "recommended build order" item #1) ─────────────────────────────────────────────
 * A snapshot is restorable only when taken at a root-loop boundary, where all logical
 * state lives in the serialized structures (mode stack + RAM) and no game logic is
 * suspended mid-C-stack inside a pump_mode or a blocking helper. So a capture request
 * does NOT snapshot immediately: it sets a pending flag, host_process_frame() then
 * free-runs (skips render + vblank pacing but keeps the per-frame logic running) so
 * any finite blocking helper unwinds to the root in CPU time, and host_root_boundary()
 * — called only from the outermost loop — performs the actual capture. An indefinite
 * input-wait (e.g. display_text at a "▼" prompt) never unwinds; the unwind is capped
 * and a capture failure is logged rather than writing a torn snapshot. This is the
 * `g_shutdown_requested` mechanism the embedded power-off handler will reuse. */

/* Request a torn-safe savestate capture at the next root-loop boundary. Idempotent:
 * a request made while one is already pending is ignored (the original unwind budget
 * is kept). The embedded power-off handler calls this from its shutdown ISR. */
void host_request_capture(void);

/* Perform a pending capture if one was requested, then clear the request. MUST be
 * called ONLY from the outermost host loop (the root boundary) — never from the
 * nested host_process_frame() inside pump_mode or a blocking helper, or the snapshot
 * would be torn. A no-op when nothing is pending. */
void host_root_boundary(void);

#endif /* GAME_MAIN_H */
