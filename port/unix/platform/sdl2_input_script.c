/*
 * Scripted input playback for automated playtests (--input FILE).
 *
 * The script replaces keyboard input frame by frame. One command per line,
 * '#' starts a comment:
 *
 *   N BUTTONS     hold BUTTONS for N frames. BUTTONS is '-' (nothing) or a
 *                 '+'-joined list: A B X Y L R START SELECT UP DOWN LEFT RIGHT
 *   shot LABEL    dump the PPU on this frame (debug/screenshot_NNN.bmp)
 *   state LABEL   print a one-line game-state summary to stdout
 *   aux NAME      pulse an aux key for one frame: F1 F5 F6 F7
 *   until COND MAX PATTERN
 *                 play PATTERN until COND holds or MAX frames pass. COND is
 *                 in:M / out:M (mode M is / isn't on the mode stack). PATTERN
 *                 is '|'-separated BUTTONS cycled every 48 frames; a trailing
 *                 '*' on a segment taps it (pressed every other frame)
 *   end           quit (also implied at end of file)
 *
 * shot/state/aux take no frames of their own; they apply to the next held
 * frame. Playback quits the game when the script runs out.
 */
#include "platform/platform.h"
#include "include/pad.h"
#include "core/memory.h"
#include "core/mode_stack.h"
#include "game/game_state.h"
#include "game/overworld.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int debug_auto_dump_requested;

static FILE *script_file;
static int script_line_no;
static uint16_t hold_pad;
static int hold_frames;
static int shot_counter;

/* Active 'until' command */
static bool until_active;
static bool until_want_in;
static int until_mode;
static int until_left;
static int until_elapsed;
static char until_pattern[128];

static bool mode_on_stack(int mode) {
    for (int i = 0; i < g_mode_stack.depth; i++)
        if (g_mode_stack.mode[i] == mode)
            return true;
    return false;
}

bool input_script_open(const char *path) {
    script_file = fopen(path, "r");
    if (!script_file) {
        fprintf(stderr, "input script: cannot open %s\n", path);
        return false;
    }
    return true;
}

bool input_script_active(void) {
    return script_file != NULL || hold_frames > 0 || until_active;
}

static uint16_t parse_buttons(char *s) {
    static const struct { const char *name; uint16_t bit; } names[] = {
        {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y},
        {"L", PAD_L}, {"R", PAD_R}, {"START", PAD_START}, {"SELECT", PAD_SELECT},
        {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT},
    };
    uint16_t pad = 0;
    if (strcmp(s, "-") == 0)
        return 0;
    for (char *tok = strtok(s, "+"); tok; tok = strtok(NULL, "+")) {
        size_t i;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (strcmp(tok, names[i].name) == 0) {
                pad |= names[i].bit;
                break;
            }
        }
        if (i == sizeof(names) / sizeof(names[0]))
            fprintf(stderr, "input script:%d: unknown button '%s'\n", script_line_no, tok);
    }
    return pad;
}

static void print_state(const char *label) {
    printf("STATE %-24s frame=%u modes=[", label, (unsigned)core.frame_counter);
    for (int i = 0; i < g_mode_stack.depth; i++)
        printf(i ? ",%u" : "%u", g_mode_stack.mode[i]);
    printf("] pos=(%u,%u) dir=%u battle=%u party=",
           game_state.leader_x_coord, game_state.leader_y_coord,
           game_state.leader_direction, ow.battle_mode);
    for (int i = 0; i < game_state.party_count && i < 6; i++) {
        uint8_t id = game_state.party_members[i];
        if (id >= 1 && id <= TOTAL_PARTY_COUNT)
            printf("%s%u:%uhp", i ? "," : "", id, party_characters[id - 1].current_hp);
    }
    printf("\n");
    fflush(stdout);
}

/* One frame of an 'until' command. Returns false once it has finished. */
static bool until_step(uint16_t *pad) {
    if (mode_on_stack(until_mode) == until_want_in || until_left <= 0) {
        printf("UNTIL %s:%d %s frame=%u\n", until_want_in ? "in" : "out", until_mode,
               until_left <= 0 ? "TIMEOUT" : "met", (unsigned)core.frame_counter);
        until_active = false;
        return false;
    }
    char buf[128];
    int nseg = 1;
    for (const char *c = until_pattern; *c; c++)
        nseg += (*c == '|');
    int seg = (until_elapsed / 48) % nseg;
    const char *start = until_pattern;
    for (int i = 0; i < seg; i++)
        start = strchr(start, '|') + 1;
    size_t len = strcspn(start, "|");
    memcpy(buf, start, len);
    buf[len] = '\0';
    bool tap = len > 0 && buf[len - 1] == '*';
    if (tap)
        buf[len - 1] = '\0';
    *pad = parse_buttons(buf);
    if (tap && (until_elapsed & 1))
        *pad = 0;
    until_elapsed++;
    until_left--;
    return true;
}

/* Advance the script by one frame. Returns the pad state for this frame and
 * ORs any one-frame aux pulses into *aux. */
uint16_t input_script_next(uint16_t *aux) {
    char line[256];
    uint16_t pad;
    if (until_active && until_step(&pad))
        return pad;
    while (hold_frames == 0) {
        if (!script_file || !fgets(line, sizeof(line), script_file)) {
            if (script_file) {
                fclose(script_file);
                script_file = NULL;
            }
            platform_request_quit();
            return 0;
        }
        script_line_no++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char cmd[64] = "", arg[128] = "";
        if (sscanf(line, "%63s %127s", cmd, arg) < 1)
            continue;

        if (isdigit((unsigned char)cmd[0])) {
            hold_frames = atoi(cmd);
            hold_pad = parse_buttons(arg[0] ? arg : "-");
        } else if (strcmp(cmd, "shot") == 0) {
            printf("SHOT %03d %s frame=%u\n", shot_counter++, arg, (unsigned)core.frame_counter);
            debug_auto_dump_requested = 1;
        } else if (strcmp(cmd, "state") == 0) {
            print_state(arg);
        } else if (strcmp(cmd, "aux") == 0) {
            if (strcmp(arg, "F1") == 0) *aux |= AUX_DEBUG_DUMP;
            else if (strcmp(arg, "F5") == 0) *aux |= AUX_DEBUG_TOGGLE;
            else if (strcmp(arg, "F6") == 0) *aux |= AUX_SAVESTATE;
            else if (strcmp(arg, "F7") == 0) *aux |= AUX_LOAD_STATE;
            else fprintf(stderr, "input script:%d: unknown aux '%s'\n", script_line_no, arg);
        } else if (strcmp(cmd, "until") == 0) {
            char cond[32] = "", pattern[128] = "-";
            int max = 0;
            if (sscanf(line, "%*s %31s %d %127s", cond, &max, pattern) < 2 ||
                (strncmp(cond, "in:", 3) != 0 && strncmp(cond, "out:", 4) != 0)) {
                fprintf(stderr, "input script:%d: bad until\n", script_line_no);
                continue;
            }
            until_want_in = cond[0] == 'i';
            until_mode = atoi(strchr(cond, ':') + 1);
            until_left = max;
            until_elapsed = 0;
            strcpy(until_pattern, pattern);
            until_active = true;
            if (until_step(&pad))
                return pad;
        } else if (strcmp(cmd, "end") == 0) {
            fclose(script_file);
            script_file = NULL;
        } else {
            fprintf(stderr, "input script:%d: unknown command '%s'\n", script_line_no, cmd);
        }
    }
    hold_frames--;
    return hold_pad;
}
