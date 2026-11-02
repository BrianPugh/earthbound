#ifndef CORE_LOST_FRAME_CHECK_H
#define CORE_LOST_FRAME_CHECK_H

/*
 * Lost-frame check (debug only; build with -DEB_LOST_FRAME_CHECK=ON, the
 * default for port/unix Debug builds).
 *
 * A synchronous helper that runs a WINDOW_TICK inline (close_window,
 * window_tick_without_instant_printing, ...: render_frame_tick_no_actionscript)
 * or a bare wait (note_unpresented_vblanks) owes a presented frame: the asm's
 * WINDOW_TICK always ends in WAIT_UNTIL_NEXT_FRAME. The mode step that ran it
 * claims the count with take_unpresented_ticks() and pushes that many
 * GAME_MODE_BATTLE_WAIT frames. A count nobody claims (never taken, or thrown
 * away by clear_unpresented_ticks()) is a frame the original shows and the port
 * doesn't, unless the step's own yield is that frame.
 *
 * The check accounts per port frame (core.frame_counter): `ticks` owed, `claimed`
 * by a take, and `work` (window_tick_work_step / render_frame_tick_work_step /
 * a BATTLE_WAIT present frame: each one a frame of its own). Only one frame is
 * presented, so a frame with unclaimed ticks loses (unclaimed + work - 1) frames
 * when that is above 0. Each such frame logs one `LOST-FRAME` line to stderr
 * (game frame, the mode stack, the frame's events and the file:line of each
 * clear that dropped a count); at exit `lost-frame check: N events` is printed.
 * playtest/regress.py and autodiff.py fail a run whose output has LOST-FRAME
 * lines.
 *
 * In other builds every hook is an empty macro: no code, no static RAM.
 */

#include <stdint.h>

#ifdef EB_LOST_FRAME_CHECK

/* n frames owed by an inline tick (bare = SET_WINDOW_TITLE-style bare waits). */
void lost_frame_owed(uint16_t n, int bare);
/* take_unpresented_ticks() returned n: the caller presents them. */
void lost_frame_claimed(uint16_t n);
/* clear_unpresented_ticks() threw n away at file:line. */
void lost_frame_dropped(uint16_t n, const char *file, int line);
/* A frame's own work: a window/render tick or a BATTLE_WAIT present frame. */
void lost_frame_work(char what);
/* End of a port frame (host_process_frame, before core.frame_counter++). */
void lost_frame_end_frame(void);

#define LOST_FRAME_OWED(n, bare)            lost_frame_owed((n), (bare))
#define LOST_FRAME_CLAIMED(n)               lost_frame_claimed(n)
#define LOST_FRAME_DROPPED(n, file, line)   lost_frame_dropped((n), (file), (line))
#define LOST_FRAME_WORK(what)               lost_frame_work(what)
#define LOST_FRAME_END_FRAME()              lost_frame_end_frame()

#else

#define LOST_FRAME_OWED(n, bare)            ((void)0)
#define LOST_FRAME_CLAIMED(n)               ((void)0)
#define LOST_FRAME_DROPPED(n, file, line)   ((void)0)
#define LOST_FRAME_WORK(what)               ((void)0)
#define LOST_FRAME_END_FRAME()              ((void)0)

#endif /* EB_LOST_FRAME_CHECK */

#endif /* CORE_LOST_FRAME_CHECK_H */
