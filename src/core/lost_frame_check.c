/* Lost-frame check: see core/lost_frame_check.h. Compiled only with
 * EB_LOST_FRAME_CHECK; otherwise this file is empty. */
#include "core/lost_frame_check.h"

#ifdef EB_LOST_FRAME_CHECK

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/memory.h"
#include "core/mode_stack.h"

/* The frame being accounted. `events` lists what happened in order:
 * T = an inline tick owed, V = a bare wait owed, C<n> = n claimed,
 * D<n>@file:line = n dropped by a clear, W = window/render tick work,
 * P = a BATTLE_WAIT present frame. The LOST-FRAME line prints the mode stack
 * as GameMode.byte0 (byte 0 of the level's ModeState: the phase for most
 * modes), root first, as it was at the frame's first owed tick. */
static struct {
    uint16_t frame;
    uint16_t owed, claimed, work;
    int open;
    char events[512];
    char modes[MODE_STACK_MAX * 12 + 1]; /* at the frame's first owed tick */
} cur;
static unsigned long total_events;
static int exit_hooked;

static void report_at_exit(void) {
    fprintf(stderr, "lost-frame check: %lu events\n", total_events);
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    size_t len = strlen(cur.events);
    if (len + 1 >= sizeof cur.events)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cur.events + len, sizeof cur.events - len, fmt, ap);
    va_end(ap);
}

/* Start accounting the current frame (the first event since the last end). */
static void touch(void) {
    if (!exit_hooked) {
        exit_hooked = 1;
        atexit(report_at_exit);
    }
    if (!cur.open) {
        cur.open = 1;
        cur.frame = core.frame_counter;
    }
}

static void snapshot_modes(void) {
    size_t o = 0;
    for (int i = 0; i < g_mode_stack.depth && o + 12 < sizeof cur.modes; i++)
        o += (size_t)snprintf(cur.modes + o, sizeof cur.modes - o, "%s%u.%u", i ? ">" : "",
                              g_mode_stack.mode[i], g_mode_stack.state[i]._raw[0]);
}

void lost_frame_owed(uint16_t n, int bare) {
    touch();
    if (!cur.owed)
        snapshot_modes();
    cur.owed += n;
    for (uint16_t i = 0; i < n; i++)
        note("%c", bare ? 'V' : 'T');
}

void lost_frame_claimed(uint16_t n) {
    if (!n)
        return;
    touch();
    cur.claimed += n;
    note(" C%u ", n);
}

void lost_frame_dropped(uint16_t n, const char *file, int line) {
    if (!n)
        return;
    touch();
    const char *base = strrchr(file, '/');
    note(" D%u@%s:%d ", n, base ? base + 1 : file, line);
}

void lost_frame_work(char what) {
    touch();
    cur.work++;
    note("%c", what);
}

void lost_frame_end_frame(void) {
    if (!cur.open)
        return;
    uint16_t unclaimed = (uint16_t)(cur.owed - cur.claimed);
    /* One frame is presented: it can be the frame of one unclaimed tick when
     * nothing else in the frame needed it. */
    if (unclaimed && unclaimed + cur.work > 1) {
        total_events++;
        fprintf(stderr,
                "LOST-FRAME frame=%u lost=%u owed=%u claimed=%u work=%u modes=%s events=[%s]\n",
                cur.frame, (unsigned)(unclaimed + cur.work - 1), cur.owed, cur.claimed, cur.work,
                cur.modes, cur.events);
    }
    memset(&cur, 0, sizeof cur);
}

#endif /* EB_LOST_FRAME_CHECK */
