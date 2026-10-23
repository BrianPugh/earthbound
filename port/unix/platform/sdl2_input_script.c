/*
 * Scripted input for automated / AI-driven playtests.
 *
 *   --input FILE     batch: play FILE, quit the game when it runs out
 *   --session FIFO   interactive: read commands from FIFO forever. When no
 *                    command is pending the game blocks (no frame advances),
 *                    so a driver (playtest/pt.py) can step it deterministically
 *                    across separate invocations.
 *
 * One command per line, '#' starts a comment. Frame-taking commands:
 *
 *   N BUTTONS        hold BUTTONS for N frames. BUTTONS is '-' (nothing) or a
 *                    '+'-joined list: A B X Y L R START SELECT UP DOWN LEFT RIGHT
 *   wait N           N frames with nothing held
 *   press BUTTONS [COUNT]
 *                    tap BUTTONS COUNT times (2 frames down, 4 up each)
 *   until COND [MAX] [PATTERN]
 *                    play PATTERN until COND holds or MAX frames pass (default
 *                    3600); in/out/idle/input must hold 3 frames in a row
 *                    (buttons released meanwhile) so the next press lands.
 *                    PATTERN is '|'-separated BUTTONS cycled every 48
 *                    frames; a trailing '*' on a segment taps it (pressed every
 *                    other frame). Default PATTERN is '-'. PATTERN 'advance'
 *                    taps A only while a text prompt waits for a button (it
 *                    never opens a menu or picks a menu item). COND is one of
 *                      in:M / out:M  mode M (name or number) is / isn't on the
 *                                    mode stack
 *                      idle          player can walk: mode stack is just
 *                                    OVERWORLD, no battle/interaction/fade
 *                      input         idle, or a menu / text prompt / target
 *                                    select is waiting for a button
 *                      text          any window text was printed
 *                      said:WORD     printed text contained WORD
 *                      battle        an enemy was touched / a battle started
 *   end | quit       stop playback and quit the game
 *
 * Instant commands (take no frame; they act on the state after all earlier
 * input):
 *
 *   shot [--crop W H] [--zoom N] [PATH|LABEL]
 *                    write the current frame as a PNG (PATH if it ends in
 *                    .png, else shots/NNNN[-LABEL].png). Prints SHOT path.
 *                    --crop: a WxH box centred on the leader; --zoom: scale up
 *   state [LABEL]    print STATE/PARTY/FOLLOW(/NEAR/ENEMIES) lines
 *   text             print the text in every open window (WIN) and the
 *                    active menu (MENU)
 *   sync ID          print "SYNC ID" (the session driver waits for it)
 *   note TEXT        print "> TEXT" verbatim (pt.py logs each command batch)
 *   mark TEXT        print "MARK n=FRAMES frame=F TEXT": FRAMES is the number
 *                    of frames recorded so far (pt.py video chapters)
 *   record on|off    resume / pause --record-video/--record-audio
 *   flag N [0|1]     read / write event flag N
 *   give ITEM [CHAR] give item id to party member CHAR (1-6; default first
 *                    with room); prints who got it, or FAILED if full
 *   status CHAR GROUP VALUE
 *                    set afflictions[GROUP-1] = VALUE (GROUP 1-7, numbered as
 *                    the text CCs; 0 clears), or party_status if GROUP is 8.
 *                    In a battle, also sets the member's battler
 *   debug on|off     set ow.debug_flag (no debug menu, unlike aux F5)
 *   money N          set money on hand
 *   hp CHAR N | pp CHAR N   set current (and rolling target) HP/PP; in a
 *                    battle, also the member's battler HP/PP (and its target)
 *   heal             restore every party member's HP/PP to max (as hp/pp)
 *   aux NAME         pulse an aux key for one frame: F1 F5 F6 F7
 *
 * Root-boundary commands (applied between frames, at the main loop root):
 *
 *   teleport X Y [DIR]  move the party to world pixel (X,Y), reloading the
 *                    map/NPCs/music (initialize_map, as the debug teleport),
 *                    then respawn Buzz Buzz and pending deliveries as the PSI
 *                    Teleport arrival does. DIR 0-7 (0=up, clockwise; default
 *                    4=down). Needs idle (prints ERR otherwise).
 *   warp NAME|N      teleport to a PSI Teleport destination (town), by index
 *                    or name prefix; 'warps' lists them
 *   snap NAME | restore NAME
 *                    save / load an in-process savestate at snaps/NAME.*
 *
 * Window text is captured as it is printed (text_trace_hook) and echoed as
 * "TEXT w<id>| ..." lines when each line completes.
 */
#include "platform/platform.h"
#include "include/pad.h"
#include "include/constants.h"
#include "core/memory.h"
#include "core/mode_stack.h"
#include "entity/entity.h"
#include "game/battle.h"
#include "game/display_text.h"
#include "game/display_text_internal.h"
#include "game/game_state.h"
#include "game/inventory.h"
#include "game/map_loader.h"
#include "game/overworld.h"
#include "game/text.h"
#include "game/window.h"
#include "game_main.h"
#include "snes/ppu.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* sdl2_video.c */
extern int video_frames_rendered;
const pixel_t *video_last_frame(void);
/* sdl2_savestate.c */
void platform_savestate_set_path(const char *prefix);
/* sdl2_record.c */
void record_set_paused(bool paused);
uint32_t record_frames(void);

static FILE *script_file;
static int script_line_no;
static uint16_t hold_pad;
static int hold_frames;
static int shot_counter;
static int last_poll_render_count = -1;
static bool frame_was_rendered;  /* this poll's frame reached the framebuffer */

/* press: remaining taps and the frame within the current tap */
static uint16_t press_pad;
static int press_left;
static int press_phase;

/* Active 'until' command */
enum { UC_IN, UC_OUT, UC_IDLE, UC_INPUT, UC_TEXT, UC_SAID, UC_BATTLE };
static bool until_active;
static int until_kind;
static int until_mode;
static int until_left;
static int until_elapsed;
static char until_pattern[128];
static char until_word[64];

/* Deferred work */
static bool capture_waiting;     /* snap/restore issued, waiting for the root boundary */
static bool capture_is_load;
static char capture_name[64];
static bool teleport_pending;
static uint16_t teleport_x, teleport_y, teleport_dir;
static bool in_root_action;      /* nested frames from a root action: don't read commands */

static const char *const mode_names[GAME_MODE_COUNT] = {
    [GAME_MODE_NONE] = "NONE", [GAME_MODE_FADE_WAIT] = "FADE_WAIT",
    [GAME_MODE_NUMBER_SELECT] = "NUMBER_SELECT", [GAME_MODE_CHAR_SELECT] = "CHAR_SELECT",
    [GAME_MODE_TEXT_DELAY] = "TEXT_DELAY", [GAME_MODE_ACTIONSCRIPT_WAIT] = "ACTIONSCRIPT_WAIT",
    [GAME_MODE_TEXT_PROMPT] = "TEXT_PROMPT", [GAME_MODE_SELECTION_MENU] = "SELECTION_MENU",
    [GAME_MODE_TOWN_MAP] = "TOWN_MAP", [GAME_MODE_SOUND_STONE] = "SOUND_STONE",
    [GAME_MODE_DEBUG_YMENU] = "DEBUG_YMENU", [GAME_MODE_BATTLE_WAIT] = "BATTLE_WAIT",
    [GAME_MODE_BATTLE_ROW_SELECT] = "BATTLE_ROW_SELECT",
    [GAME_MODE_BATTLE_ENEMY_SELECT] = "BATTLE_ENEMY_SELECT",
    [GAME_MODE_NAMING_EVENTS] = "NAMING_EVENTS", [GAME_MODE_TEXT_INPUT] = "TEXT_INPUT",
    [GAME_MODE_NAMING_PROMPT] = "NAMING_PROMPT", [GAME_MODE_SCREEN_TRANSITION] = "SCREEN_TRANSITION",
    [GAME_MODE_PALETTE_FADE] = "PALETTE_FADE", [GAME_MODE_MAP_PALETTE_FADE] = "MAP_PALETTE_FADE",
    [GAME_MODE_MOSAIC_FADE] = "MOSAIC_FADE", [GAME_MODE_FLYOVER] = "FLYOVER",
    [GAME_MODE_INTRO_LOGO] = "INTRO_LOGO", [GAME_MODE_GAS_STATION] = "GAS_STATION",
    [GAME_MODE_TITLE_SCREEN] = "TITLE_SCREEN", [GAME_MODE_ATTRACT] = "ATTRACT",
    [GAME_MODE_FILE_MENU] = "FILE_MENU", [GAME_MODE_INIT_INTRO] = "INIT_INTRO",
    [GAME_MODE_DISPLAY_TEXT] = "DISPLAY_TEXT", [GAME_MODE_ENTITY_FADE_WAIT] = "ENTITY_FADE_WAIT",
    [GAME_MODE_TEXT_WAIT_FADE] = "TEXT_WAIT_FADE",
    [GAME_MODE_PROCESS_INTERACTION] = "PROCESS_INTERACTION",
    [GAME_MODE_DOOR_TRANSITION] = "DOOR_TRANSITION", [GAME_MODE_QUICK_CHECKTALK] = "QUICK_CHECKTALK",
    [GAME_MODE_PAUSE_MENU] = "PAUSE_MENU", [GAME_MODE_EQUIP_MENU] = "EQUIP_MENU",
    [GAME_MODE_STATUS_MENU] = "STATUS_MENU", [GAME_MODE_HPPP_DISPLAY] = "HPPP_DISPLAY",
    [GAME_MODE_PSI_MENU] = "PSI_MENU", [GAME_MODE_USE_ITEM] = "USE_ITEM",
    [GAME_MODE_TELEPORT_MENU] = "TELEPORT_MENU",
    [GAME_MODE_DETERMINE_TARGETING] = "DETERMINE_TARGETING", [GAME_MODE_LEVEL_UP] = "LEVEL_UP",
    [GAME_MODE_BATTLE_PSI_MENU] = "BATTLE_PSI_MENU", [GAME_MODE_BATTLE_MENU] = "BATTLE_MENU",
    [GAME_MODE_BATTLE] = "BATTLE", [GAME_MODE_INSTANT_WIN] = "INSTANT_WIN",
    [GAME_MODE_BATTLE_ENTRY] = "BATTLE_ENTRY", [GAME_MODE_BATTLE_SCRIPTED] = "BATTLE_SCRIPTED",
    [GAME_MODE_BATTLE_ACTION] = "BATTLE_ACTION", [GAME_MODE_BATTLE_CALC] = "BATTLE_CALC",
    [GAME_MODE_BATTLE_REVIVE] = "BATTLE_REVIVE", [GAME_MODE_BATTLE_APPLY] = "BATTLE_APPLY",
    [GAME_MODE_BATTLE_KO] = "BATTLE_KO", [GAME_MODE_CHECK_DEAD_PLAYERS] = "CHECK_DEAD_PLAYERS",
    [GAME_MODE_ACTIONSCRIPT_FRAME] = "ACTIONSCRIPT_FRAME",
    [GAME_MODE_PP_RECOVERY_FLASH] = "PP_RECOVERY_FLASH", [GAME_MODE_TELEPORT] = "TELEPORT",
    [GAME_MODE_BICYCLE_DISMOUNT] = "BICYCLE_DISMOUNT", [GAME_MODE_HP_ALERT] = "HP_ALERT",
    [GAME_MODE_GAME_OVER] = "GAME_OVER", [GAME_MODE_OVERWORLD] = "OVERWORLD",
    [GAME_MODE_DEBUG_GOODS] = "DEBUG_GOODS", [GAME_MODE_DEBUG_MENU] = "DEBUG_MENU",
    [GAME_MODE_TELEPORT_TO] = "TELEPORT_TO", [GAME_MODE_NEW_GAME_NAMING] = "NEW_GAME_NAMING",
    [GAME_MODE_SPECIAL_EVENT] = "SPECIAL_EVENT", [GAME_MODE_ENTER_NAME] = "ENTER_NAME",
    [GAME_MODE_LOAD_BATTLE_SCENE] = "LOAD_BATTLE_SCENE", [GAME_MODE_STORE_MENU] = "STORE_MENU",
    [GAME_MODE_ESCARGO_MENU] = "ESCARGO_MENU", [GAME_MODE_TELEPHONE_MENU] = "TELEPHONE_MENU",
    [GAME_MODE_WAIT_FRAMES] = "WAIT_FRAMES", [GAME_MODE_ENDING] = "ENDING",
    [GAME_MODE_WINDOW_BORDER_ANIM] = "WINDOW_BORDER_ANIM",
};

static const char *const dir_names[8] = {
    "UP", "UP_RIGHT", "RIGHT", "DOWN_RIGHT", "DOWN", "DOWN_LEFT", "LEFT", "UP_LEFT",
};

static void script_error(const char *fmt, const char *arg) {
    printf("ERR line %d: ", script_line_no);
    printf(fmt, arg);
    printf("\n");
    fflush(stdout);
}

static const char *mode_name(int mode) {
    if (mode >= 0 && mode < GAME_MODE_COUNT && mode_names[mode])
        return mode_names[mode];
    return "?";
}

static int parse_mode(const char *s) {
    if (isdigit((unsigned char)s[0]))
        return atoi(s);
    for (int i = 0; i < GAME_MODE_COUNT; i++)
        if (mode_names[i] && strcmp(mode_names[i], s) == 0)
            return i;
    return -1;
}

static bool mode_on_stack(int mode) {
    for (int i = 0; i < g_mode_stack.depth; i++)
        if (g_mode_stack.mode[i] == mode)
            return true;
    return false;
}

static int top_mode(void) {
    return g_mode_stack.depth ? g_mode_stack.mode[g_mode_stack.depth - 1] : GAME_MODE_NONE;
}

/* ---- Window text capture (text_trace_hook) ---- */

#define TT_LINES  16
#define TT_GLYPHS 72

typedef struct {
    uint16_t x;
    char c;
} Glyph;

typedef struct {
    uint8_t n;
    Glyph g[TT_GLYPHS];
} TextLine;

static TextLine tt_lines[MAX_WINDOWS][TT_LINES];

/* Per window: where the previous character went. After a character the game
 * sets cursor_pixel_x = vwf_x (VWF-buffer relative); an explicit reposition
 * sets it window-relative. So "cursor_pixel_x == vwf_x" means "continues the
 * previous character", advanced by however far vwf_x moved. */
static struct {
    int y;
    int x;          /* window-relative pixel x of the previous character */
    int vwf;        /* vwf_x when it was printed */
    bool valid;
} tt_pos[MAX_WINDOWS];

/* Transcript: the line currently being printed, echoed as TEXT when done */
static char said_line[160];
static int said_len;
static int said_slot = -1, said_y = -1, said_last_x;
static uint8_t said_window_id;
static unsigned text_chars_printed;
#define UNTIL_SAID_KEEP 512
static char until_said[2048];
static int until_said_len;

static int window_slot(uint16_t window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (win.windows[i].active && win.windows[i].id == window_id)
            return i;
    return -1;
}

static void said_flush(void) {
    while (said_len > 0 && said_line[said_len - 1] == ' ')
        said_len--;
    if (said_len > 0) {
        said_line[said_len] = '\0';
        printf("TEXT w%u| %s\n", said_window_id, said_line);
        fflush(stdout);
    }
    said_len = 0;
    said_slot = -1;
    said_y = -1;
}

static void tt_clear_slot(int slot) {
    memset(tt_lines[slot], 0, sizeof(tt_lines[slot]));
    tt_pos[slot].valid = false;
}

static void text_trace(int event, uint16_t window_id, uint8_t eb_char) {
    int slot = window_slot(window_id);
    if (slot < 0)
        return;
    WindowInfo *w = &win.windows[slot];

    if (event != TEXT_TRACE_CHAR) {
        if (slot == said_slot)
            said_flush();
        if (event == TEXT_TRACE_SCROLL) {
            memmove(&tt_lines[slot][0], &tt_lines[slot][1], sizeof(TextLine) * (TT_LINES - 1));
            memset(&tt_lines[slot][TT_LINES - 1], 0, sizeof(TextLine));
            tt_pos[slot].y--;
        } else {
            tt_clear_slot(slot);
        }
        return;
    }

    char c = (eb_char >= 0x50 && eb_char <= 0xAF) ? eb_char_to_ascii(eb_char) : 0;
    if (!c)
        return;
    int y = w->text_y;
    int x = w->cursor_pixel_x;
    int span_from = -1; /* the previous character's pixels: (span_from, x) */
    if (tt_pos[slot].valid && tt_pos[slot].y == y && w->cursor_pixel_x == vwf_x) {
        int adv = (int)vwf_x - tt_pos[slot].vwf;
        if (adv < 0)
            adv += VWF_BUFFER_TILES * 8;
        x = tt_pos[slot].x + adv;
        span_from = tt_pos[slot].x;
    }
    tt_pos[slot].y = y;
    tt_pos[slot].x = x;
    tt_pos[slot].vwf = vwf_x;
    tt_pos[slot].valid = true;

    /* Grid copy: overwrite whatever glyph sat at this x, keep sorted by x.
     * Glyphs inside the previous character's span were drawn over too: when
     * a menu row is blanked without a trace event and dialogue prints there
     * ("Yes  No" then "@Ready!"), the old glyphs fall between the new ones. */
    if (y >= 0 && y < TT_LINES) {
        TextLine *ln = &tt_lines[slot][y];
        int j = 0;
        for (int i = 0; i < ln->n; i++) {
            int gx = ln->g[i].x;
            bool covered = (gx + 1 >= x && gx <= x + 1) || (span_from >= 0 && gx > span_from && gx < x);
            if (!covered)
                ln->g[j++] = ln->g[i];
        }
        ln->n = (uint8_t)j;
        if (ln->n < TT_GLYPHS) {
            int at = ln->n;
            while (at > 0 && ln->g[at - 1].x > x) {
                ln->g[at] = ln->g[at - 1];
                at--;
            }
            ln->g[at] = (Glyph){ (uint16_t)x, c };
            ln->n++;
        }
    }

    /* Transcript, in print order; a '@' bullet starts a new statement */
    if (slot != said_slot || y != said_y || x < said_last_x || c == '@')
        said_flush();
    if (said_len > 0 && x > said_last_x + 14 && said_len < (int)sizeof(said_line) - 3) {
        said_line[said_len++] = ' ';
        said_line[said_len++] = ' ';
    }
    if (said_len < (int)sizeof(said_line) - 1)
        said_line[said_len++] = c;
    said_slot = slot;
    said_y = y;
    said_window_id = (uint8_t)window_id;
    said_last_x = x;
    text_chars_printed++;
    if (until_active) {
        /* A sliding window over the printed text: said:WORD is checked every
         * frame, so when it fills, only the most recent text is kept */
        if (until_said_len >= (int)sizeof(until_said) - 1) {
            memmove(until_said, until_said + until_said_len - UNTIL_SAID_KEEP, UNTIL_SAID_KEEP);
            until_said_len = UNTIL_SAID_KEEP;
        }
        until_said[until_said_len++] = c;
        until_said[until_said_len] = '\0';
    }
}

/* Whether the window still shows something where a captured glyph went.
 * Some game paths blank a window's tilemap (or a row of it) without a trace
 * event; a glyph whose tile cells are all blank is no longer on screen. A text
 * line is 2 tile rows; a glyph (<= 8 px wide) touches tile columns x/8 and
 * (x+7)/8. */
static bool glyph_visible(const WindowInfo *w, int y, int x) {
    if (!w->content_tilemap || w->width < 3)
        return false;
    int cw = w->width - 2;
    for (int col = x / 8; col <= (x + 7) / 8 && col < cw; col++) {
        int upper = y * cw * 2 + col, lower = upper + cw;
        if (lower < w->content_tilemap_size && (w->content_tilemap[upper] || w->content_tilemap[lower]))
            return true;
    }
    return false;
}

/* Render one captured line as ASCII, spacing out column gaps. Glyphs whose
 * tiles have since been blanked are skipped. */
static void line_text(const WindowInfo *w, int y, const TextLine *ln, char *out, size_t cap) {
    size_t n = 0;
    int prev_x = -1;
    for (int i = 0; i < ln->n && n + 3 < cap; i++) {
        if (!glyph_visible(w, y, ln->g[i].x))
            continue;
        if (prev_x >= 0 && ln->g[i].x > prev_x + 14)
            out[n++] = ' ', out[n++] = ' ';
        out[n++] = ln->g[i].c;
        prev_x = ln->g[i].x;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

static void print_windows(void) {
    said_flush();
    /* Walk the draw-order list bottom to top */
    for (int s = win.window_head; s >= 0 && s < MAX_WINDOWS; s = win.windows[s].next) {
        WindowInfo *w = &win.windows[s];
        if (!w->active)
            continue;
        bool any = false;
        for (int y = 0; y < TT_LINES; y++) {
            char buf[256];
            line_text(w, y, &tt_lines[s][y], buf, sizeof(buf));
            if (!buf[0])
                continue;
            printf("WIN w%u%s%s%s%s| %s\n", w->id, w->id == win.current_focus_window ? "*" : "",
                   w->title[0] ? " [" : "", w->title, w->title[0] ? "]" : "", buf);
            any = true;
        }
        if (!any && w->title[0])
            printf("WIN w%u| [%s]\n", w->id, w->title);
    }
    /* The focused menu (if a menu is being driven) */
    WindowInfo *f = win.current_focus_window != WINDOW_ID_NONE ? get_window(win.current_focus_window) : NULL;
    if (f && f->menu_count > 0) {
        printf("MENU w%u|", f->id);
        for (int i = 0; i < f->menu_count && i < 24; i++)
            printf(" %s%s%s", i == f->current_option ? ">[" : "[", f->menu_items[i].label, "]");
        printf("\n");
    }
    fflush(stdout);
}

/* ---- State queries ---- */

/* Why the player can't walk right now, or NULL if they can. */
static const char *not_idle_reason(void) {
    if (g_mode_stack.depth != 1 || g_mode_stack.mode[0] != GAME_MODE_OVERWORLD)
        return "modes";
    if (ow.battle_mode || ow.battle_swirl_countdown || ow.enemy_has_been_touched)
        return "battle";
    if (ow.pending_interactions)
        return "interaction";
    if (ow.input_disable_frame_counter)
        return "input-disabled";
    if (entities.tick_callback_hi[PARTY_LEADER_ENTITY_INDEX] & (OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED))
        return "leader-frozen";
    /* The leader moves by itself: an escalator ride, a stairs walk, a
     * camera_mode auto-move, or a replayed auto-movement (door/escalator
     * approach). UPDATE_LEADER_MOVEMENT ignores the pad meanwhile. */
    if (game_state.walking_style == WALKING_STYLE_ESCALATOR)
        return "escalator";
    if (game_state.walking_style == WALKING_STYLE_STAIRS)
        return "stairs";
    if (game_state.camera_mode)
        return "auto-move";
    if (ow.demo_frames_left || (ow.player_movement_flags & 0x02))
        return "auto-walk";
    if ((ppu.inidisp & 0x80) || (ppu.inidisp & 0x0F) != 0x0F)
        return "fade";
    return NULL;
}

static bool player_idle(void) {
    return not_idle_reason() == NULL;
}

/* A text prompt is waiting for a button (TP_TEXTSPEED advances by itself) */
static bool text_prompt_waiting(void) {
    if (top_mode() != GAME_MODE_TEXT_PROMPT)
        return false;
    uint8_t phase = g_mode_stack.state[g_mode_stack.depth - 1].text_prompt.phase;
    return phase == TP_WAIT_BUTTON || phase == TP_TRIANGLE;
}

static bool waiting_for_input(void) {
    if (player_idle() || text_prompt_waiting())
        return true;
    switch (top_mode()) {
    case GAME_MODE_SELECTION_MENU:
    case GAME_MODE_CHAR_SELECT:
    case GAME_MODE_NUMBER_SELECT:
    case GAME_MODE_TEXT_INPUT:
    case GAME_MODE_NAMING_PROMPT:
    case GAME_MODE_BATTLE_ENEMY_SELECT:
    case GAME_MODE_BATTLE_ROW_SELECT:
        return true;
    default:
        return false;
    }
}

static void print_eb(const uint8_t *s, int max) {
    for (int i = 0; i < max && s[i]; i++)
        putchar(eb_char_to_ascii(s[i]));
}

/* Affliction names by group (0-based) and value; NULL = print the number */
static const char *const status_names[7][8] = {
    { NULL, "unconscious", "diamondized", "paralyzed", "nauseous", "poisoned", "sunstroke", "cold" },
    { NULL, "mushroomized", "possessed" },
    { NULL, "asleep", "crying", "immobilized", "solidified" },
    { NULL, "strange" },
    { NULL, "cant-concentrate", NULL, NULL, "cant-concentrate4" },
    { NULL, "homesick" },
    { NULL, "psi-shield-power", "psi-shield", "shield-power", "shield" },
};

/* " st=G:NAME,..." for every nonzero affliction group (1-based, as `status`) */
static void print_afflictions(const CharStruct *c) {
    bool any = false;
    for (int g = 0; g < 7; g++) {
        uint8_t v = c->afflictions[g];
        if (!v)
            continue;
        printf("%s%d:", any ? "," : " st=", g + 1);
        if (v < 8 && status_names[g][v])
            printf("%s", status_names[g][v]);
        else
            printf("%u", v);
        any = true;
    }
}

/* Surface flags (SURFACE_FLAGS in include/enums.asm) as words */
static void print_surface(uint16_t sf) {
    printf(" surface=0x%02X", sf & 0xFF);
    if (!(sf & 0xFF))
        return;
    printf("(");
    const char *sep = "";
    if ((sf & 0x0C) == 0x0C)
        printf("%sdeep-water", sep), sep = ",";
    else if (sf & 0x08)
        printf("%sshallow-water", sep), sep = ",";
    else if (sf & 0x04)
        printf("%ssunstroke", sep), sep = ",";
    if (sf & 0x10)
        printf("%sladder-stairs", sep), sep = ",";
    if (sf & 0x01)
        printf("%shide-lower", sep), sep = ",";
    if (sf & 0x02)
        printf("%shide-upper", sep), sep = ",";
    if (sf & 0x80)
        printf("%ssolid", sep), sep = ",";
    printf(")");
}

static void print_state(const char *label) {
    said_flush();
    printf("STATE %s frame=%u modes=[", label[0] ? label : "-", (unsigned)core.frame_counter);
    for (int i = 0; i < g_mode_stack.depth; i++)
        printf("%s%s", i ? ">" : "", mode_name(g_mode_stack.mode[i]));
    uint16_t dir = game_state.leader_direction;
    const char *why = not_idle_reason();
    printf("] idle=%s input=%d pos=(%u,%u) dir=%s music=%u money=%u battle=%u ow_enemies=%u",
           why ? why : "yes", waiting_for_input(), game_state.leader_x_coord, game_state.leader_y_coord,
           dir < 8 ? dir_names[dir] : "?", ml.current_map_music_track, (unsigned)game_state.money_carried,
           ow.battle_mode, ow.overworld_enemy_count);
    /* Post-battle invulnerability: the party sprites blink every other frame */
    if (ow.player_intangibility_frames)
        printf(" blink=%u", ow.player_intangibility_frames);
    print_surface(game_state.trodden_tile_type);
    if (game_state.walking_style)
        printf(" walk=%u", game_state.walking_style);
    if (game_state.party_status)
        printf(" party_status=%u", game_state.party_status);
    if (ow.debug_flag)
        printf(" debug=1");
    printf("\n");

    printf("PARTY");
    for (int i = 0; i < game_state.party_count && i < 6; i++) {
        uint8_t id = game_state.party_members[i];
        if (id < 1 || id > TOTAL_PARTY_COUNT)
            continue;
        const CharStruct *c = &party_characters[id - 1];
        printf("%s %u:", i ? " |" : "", id);
        print_eb(c->name, 5);
        printf(" L%u HP %u/%u PP %u/%u", c->level, c->current_hp, c->max_hp, c->current_pp, c->max_pp);
        print_afflictions(c);
    }
    printf("\n");

    /* Followers: (dx,dy) of each party entity from the leader entity, in
     * party order. A hidden follower (spacing / no sprite) is marked. */
    if (!mode_on_stack(GAME_MODE_BATTLE) && (game_state.party_count > 1 || ow.mini_ghost_entity_id >= 0)) {
        int lead = game_state.current_party_members;
        printf("FOLLOW");
        for (int i = 0; i < game_state.party_count && i < TOTAL_PARTY_COUNT; i++) {
            int e = read_u16_le(&game_state.party_entity_slots[i * 2]);
            if (e == lead || e < 0 || e >= MAX_ENTITIES || entities.script_table[e] == ENTITY_NONE)
                continue;
            printf(" %u:(%+d,%+d)%s", game_state.party_order[i], entities.abs_x[e] - entities.abs_x[lead],
                   entities.abs_y[e] - entities.abs_y[lead], entities.animation_frame[e] == -1 ? "hidden" : "");
        }
        if (ow.mini_ghost_entity_id >= 0 && ow.mini_ghost_entity_id < MAX_ENTITIES) {
            int g = ow.mini_ghost_entity_id;
            printf(" ghost:(%+d,%+d)%s", entities.abs_x[g] - entities.abs_x[lead],
                   entities.abs_y[g] - entities.abs_y[lead], entities.animation_frame[g] == -1 ? "hidden" : "");
        }
        printf("\n");
    }

    /* Nearby overworld entities (not the party), relative to the leader */
    if (!mode_on_stack(GAME_MODE_BATTLE)) {
        bool any = false;
        for (int e = entities.first_entity; e >= 0 && e < MAX_ENTITIES; e = entities.next_entity[e]) {
            if (e >= PARTY_LEADER_ENTITY_INDEX || entities.script_table[e] == ENTITY_NONE ||
                entities.npc_ids[e] == 0xFFFF)
                continue;
            int dx = entities.abs_x[e] - (int)game_state.leader_x_coord;
            int dy = entities.abs_y[e] - (int)game_state.leader_y_coord;
            bool enemy = (entities.npc_ids[e] & 0x8000) != 0;
            if (!enemy && (dx < -160 || dx > 160 || dy < -128 || dy > 128))
                continue;
            printf(any ? " |" : "NEAR");
            any = true;
            if (enemy && enemy_config_table && entities.enemy_ids[e] >= 0) {
                printf(" enemy ");
                print_eb(enemy_config_table[entities.enemy_ids[e]].name, ENEMY_NAME_SIZE);
            } else {
                printf(" npc%u", entities.npc_ids[e]);
            }
            printf(" (%+d,%+d)", dx, dy);
        }
        if (any)
            printf("\n");
    }

    if (mode_on_stack(GAME_MODE_BATTLE) && enemy_config_table) {
        printf("ENEMIES");
        for (int i = FIRST_ENEMY_INDEX; i < BATTLER_COUNT; i++) {
            const Battler *b = &bt.battlers_table[i];
            if (!b->consciousness)
                continue;
            printf(" [%d] ", i - FIRST_ENEMY_INDEX);
            print_eb(enemy_config_table[b->id].name, ENEMY_NAME_SIZE);
            printf(" hp=%u/%u", b->hp, b->hp_max);
        }
        printf("\n");
    }
    fflush(stdout);
}

/* ---- Screenshot (uncompressed PNG) ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);
    uint32_t crc = crc32_update(0, (const uint8_t *)type, 4);
    crc = crc32_update(crc, data, len);
    put_be32(hdr, crc);
    fwrite(hdr, 1, 4, f);
}

static bool write_png(const char *path, const pixel_t *fb, int w, int h) {
    size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h;
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *raw = malloc(raw_len), *z = malloc(z_len);
    FILE *f = fopen(path, "wb");
    if (!raw || !z || !f) {
        free(raw);
        free(z);
        if (f)
            fclose(f);
        return false;
    }
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + row * (size_t)y;
        *r++ = 0; /* filter: none */
        for (int x = 0; x < w; x++) {
            uint32_t px = pixel_to_rgb888(fb[y * w + x]);
            *r++ = (uint8_t)(px >> 16);
            *r++ = (uint8_t)(px >> 8);
            *r++ = (uint8_t)px;
        }
    }
    /* zlib stream of stored deflate blocks */
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i += 65535) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        z[o++] = (i + n == raw_len);
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + i, n);
        o += n;
    }
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(z + o, (b << 16) | a);
    o += 4;

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t ihdr[13] = { 0 };
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 2; /* RGB */
    fwrite(sig, 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)o);
    png_chunk(f, "IEND", NULL, 0);
    free(raw);
    free(z);
    return fclose(f) == 0;
}

/* shot [--crop W H] [--zoom N] [LABEL|PATH.png]
 * --crop cuts a WxH box centred on the party leader (clamped to the frame);
 * --zoom scales the result up N times (nearest neighbour). */
static void take_shot(const char *line) {
    /* The frame just rendered shows the state after all earlier input. If
     * frame-skip dropped this frame's render, render it now. */
    if (!frame_was_rendered) {
        platform_render_frame(NULL);
        frame_was_rendered = true;
    }
    char buf[256], arg[128] = "";
    int cw = 0, ch = 0, zoom = 1;
    snprintf(buf, sizeof(buf), "%s", line);
    char *save = NULL;
    strtok_r(buf, " \t\r\n", &save); /* "shot" */
    for (char *tok = strtok_r(NULL, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (strcmp(tok, "--crop") == 0) {
            char *w = strtok_r(NULL, " \t\r\n", &save), *h = w ? strtok_r(NULL, " \t\r\n", &save) : NULL;
            cw = w ? atoi(w) : 0;
            ch = h ? atoi(h) : 0;
            if (cw <= 0 || ch <= 0) {
                script_error("usage: shot --crop W H [--zoom N] [LABEL]%s", "");
                return;
            }
        } else if (strcmp(tok, "--zoom") == 0) {
            char *z = strtok_r(NULL, " \t\r\n", &save);
            zoom = z ? atoi(z) : 0;
            if (zoom < 1 || zoom > 16) {
                script_error("usage: shot ... --zoom N (1-16)%s", "");
                return;
            }
        } else {
            snprintf(arg, sizeof(arg), "%s", tok);
        }
    }

    const pixel_t *fb = video_last_frame();
    char path[256];
    size_t len = strlen(arg);
    if (len > 4 && strcmp(arg + len - 4, ".png") == 0) {
        snprintf(path, sizeof(path), "%s", arg);
    } else {
        mkdir("shots", 0755);
        snprintf(path, sizeof(path), "shots/%04d%s%s.png", shot_counter, arg[0] ? "-" : "", arg);
    }
    shot_counter++;

    int x0 = 0, y0 = 0, w = EB_VIEWPORT_WIDTH, h = EB_VIEWPORT_HEIGHT;
    if (cw > 0) {
        int lead = game_state.current_party_members;
        /* screen_x/y are already viewport pixels (BG scroll includes the pad) */
        int cx = entities.screen_x[lead];
        int cy = entities.screen_y[lead] - 8; /* feet -> body */
        w = cw < EB_VIEWPORT_WIDTH ? cw : EB_VIEWPORT_WIDTH;
        h = ch < EB_VIEWPORT_HEIGHT ? ch : EB_VIEWPORT_HEIGHT;
        x0 = cx - w / 2;
        y0 = cy - h / 2;
        x0 = x0 < 0 ? 0 : (x0 > EB_VIEWPORT_WIDTH - w ? EB_VIEWPORT_WIDTH - w : x0);
        y0 = y0 < 0 ? 0 : (y0 > EB_VIEWPORT_HEIGHT - h ? EB_VIEWPORT_HEIGHT - h : y0);
    }
    bool ok = false;
    if (fb && x0 == 0 && y0 == 0 && w == EB_VIEWPORT_WIDTH && h == EB_VIEWPORT_HEIGHT && zoom == 1) {
        ok = write_png(path, fb, w, h);
    } else if (fb) {
        int ow_ = w * zoom, oh = h * zoom;
        pixel_t *out = malloc(sizeof(pixel_t) * (size_t)ow_ * (size_t)oh);
        if (out) {
            for (int y = 0; y < oh; y++)
                for (int x = 0; x < ow_; x++)
                    out[y * ow_ + x] = fb[(y0 + y / zoom) * EB_VIEWPORT_WIDTH + x0 + x / zoom];
            ok = write_png(path, out, ow_, oh);
            free(out);
        }
    }
    if (ok) {
        printf("SHOT %s frame=%u", path, (unsigned)core.frame_counter);
        if (cw > 0 || zoom > 1)
            printf(" box=%d,%d,%dx%d zoom=%d", x0, y0, w, h, zoom);
        printf("\n");
    } else {
        script_error("cannot write screenshot %s", path);
    }
    fflush(stdout);
}

/* ---- Script input ---- */

static bool open_script(const char *path, bool session) {
    if (session) {
        /* O_RDWR keeps a writer open ourselves, so the FIFO never reports EOF
         * between driver writes; reads simply block until a command arrives. */
        int fd = open(path, O_RDWR);
        script_file = fd >= 0 ? fdopen(fd, "r") : NULL;
    } else {
        script_file = fopen(path, "r");
    }
    if (!script_file) {
        fprintf(stderr, "input script: cannot open %s\n", path);
        return false;
    }
    text_trace_hook = text_trace;
    return true;
}

bool input_script_open(const char *path) {
    return open_script(path, false);
}

bool input_script_open_session(const char *path) {
    return open_script(path, true);
}

bool input_script_active(void) {
    return script_file != NULL || hold_frames > 0 || until_active || press_left > 0;
}

static uint16_t parse_buttons(char *s) {
    static const struct { const char *name; uint16_t bit; } names[] = {
        {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y},
        {"L", PAD_L}, {"R", PAD_R}, {"START", PAD_START}, {"SELECT", PAD_SELECT},
        {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT},
    };
    uint16_t pad = 0;
    if (strcmp(s, "-") == 0 || !s[0])
        return 0;
    char *save = NULL;
    for (char *tok = strtok_r(s, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        for (char *p = tok; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        size_t i;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (strcmp(tok, names[i].name) == 0) {
                pad |= names[i].bit;
                break;
            }
        }
        if (i == sizeof(names) / sizeof(names[0]))
            script_error("unknown button '%s'", tok);
    }
    return pad;
}

/* in/out/idle/input must hold this many frames in a row (with buttons
 * released meanwhile): a menu or prompt ignores input on its first frame, so
 * a press sent at once would be lost */
#define UNTIL_STABLE_FRAMES 3
static int until_stable;

static bool until_settled(bool cond) {
    until_stable = cond ? until_stable + 1 : 0;
    return until_stable >= UNTIL_STABLE_FRAMES;
}

static bool until_met(void) {
    switch (until_kind) {
    case UC_IN: return until_settled(mode_on_stack(until_mode));
    case UC_OUT: return until_settled(!mode_on_stack(until_mode));
    case UC_IDLE: return until_settled(player_idle());
    case UC_INPUT: return until_settled(waiting_for_input());
    case UC_TEXT: return until_said_len > 0;
    case UC_SAID: return strstr(until_said, until_word) != NULL;
    case UC_BATTLE:
        return ow.battle_mode || ow.battle_swirl_countdown || ow.enemy_has_been_touched ||
               mode_on_stack(GAME_MODE_BATTLE);
    default: return true;
    }
}

/* One frame of an 'until' command. Returns false once it has finished. */
static bool until_step(uint16_t *pad) {
    bool met = until_met();
    if (met || until_left <= 0) {

        printf("UNTIL %s after %d frames (frame=%u)\n", met ? "met" : "TIMEOUT", until_elapsed,
               (unsigned)core.frame_counter);
        fflush(stdout);
        until_active = false;
        return false;
    }
    if (strcmp(until_pattern, "advance") == 0) {
        /* Tap A only while a text prompt waits for it */
        *pad = (text_prompt_waiting() && !(until_elapsed & 1)) ? PAD_A : 0;
        until_elapsed++;
        until_left--;
        return true;
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
    if ((tap && (until_elapsed & 1)) || until_stable > 0)
        *pad = 0; /* release while an idle/input condition settles */
    until_elapsed++;
    until_left--;
    return true;
}

static bool start_until(const char *line) {
    char cond[96] = "", a2[128] = "", a3[128] = "";
    int n = sscanf(line, "%*s %95s %127s %127s", cond, a2, a3);
    if (n < 1) {
        script_error("bad until%s", "");
        return false;
    }
    int max = 3600;
    const char *pattern = "-";
    if (n >= 2 && isdigit((unsigned char)a2[0])) {
        max = atoi(a2);
        if (n >= 3)
            pattern = a3;
    } else if (n >= 2) {
        pattern = a2;
    }
    if (strncmp(cond, "in:", 3) == 0 || strncmp(cond, "out:", 4) == 0) {
        until_kind = cond[0] == 'i' ? UC_IN : UC_OUT;
        until_mode = parse_mode(strchr(cond, ':') + 1);
        if (until_mode < 0) {
            script_error("unknown mode in '%s'", cond);
            return false;
        }
    } else if (strncmp(cond, "said:", 5) == 0) {
        until_kind = UC_SAID;
        snprintf(until_word, sizeof(until_word), "%s", cond + 5);
    } else if (strcmp(cond, "idle") == 0) {
        until_kind = UC_IDLE;
    } else if (strcmp(cond, "input") == 0) {
        until_kind = UC_INPUT;
    } else if (strcmp(cond, "text") == 0) {
        until_kind = UC_TEXT;
    } else if (strcmp(cond, "battle") == 0) {
        until_kind = UC_BATTLE;
    } else {
        script_error("unknown until condition '%s'", cond);
        return false;
    }
    until_left = max;
    until_elapsed = 0;
    until_stable = 0;
    until_said_len = 0;
    until_said[0] = '\0';
    snprintf(until_pattern, sizeof(until_pattern), "%s", pattern);
    until_active = true;
    return true;
}

/* PSI Teleport destination table entry i (from 1; entry 0 is a sentinel),
 * or NULL past the end */
static const uint8_t *psi_teleport_entry(int i) {
    size_t off = (size_t)i * PSI_TELEPORT_DEST_ENTRY_SIZE;
    if (i < 1 || off + PSI_TELEPORT_DEST_ENTRY_SIZE > psi_teleport_dest_size)
        return NULL;
    const uint8_t *e = psi_teleport_dest_data + off;
    return e[0] ? e : NULL;
}

static CharStruct *party_char(int id) {
    if (id < 1 || id > TOTAL_PARTY_COUNT) {
        script_error("bad character id%s", "");
        return NULL;
    }
    return &party_characters[id - 1];
}

/* During a battle, the battler for party member CHAR_INDEX (0-based), or
 * NULL. The battler keeps its own HP/PP targets and afflictions: damage is
 * taken from battler.hp_target (REDUCE_HP), which SET_HP then copies to the
 * char_struct's current_hp_target, and the afflictions are copied back to the
 * char_struct after each action. So a poke to the char_struct alone is
 * overwritten (or undone by the next hit) until the battle ends. */
static Battler *party_battler(int char_index) {
    if (!mode_on_stack(GAME_MODE_BATTLE))
        return NULL;
    for (int i = 0; i < FIRST_ENEMY_INDEX; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness && b->ally_or_enemy == 0 && b->npc_id == 0 && b->row == char_index)
            return b;
    }
    return NULL;
}

/* Set a party member's HP / PP (current and roller target), and its battler's */
static void poke_hp(CharStruct *c, uint16_t hp) {
    c->current_hp = c->current_hp_target = hp;
    Battler *b = party_battler((int)(c - party_characters));
    if (b)
        b->hp = b->hp_target = hp;
}

static void poke_pp(CharStruct *c, uint16_t pp) {
    c->current_pp = c->current_pp_target = pp;
    Battler *b = party_battler((int)(c - party_characters));
    if (b)
        b->pp = b->pp_target = pp;
}

/* Run one instant/deferred command. Returns false if the command must end
 * this frame's command processing (it waits on the root boundary). */
static bool run_command(const char *cmd, const char *arg, const char *line, uint16_t *aux) {
    int v1 = 0, v2 = 0, v3 = 0;
    int nv = sscanf(line, "%*s %d %d %d", &v1, &v2, &v3);

    if (strcmp(cmd, "shot") == 0) {
        take_shot(line);
    } else if (strcmp(cmd, "state") == 0) {
        print_state(arg);
    } else if (strcmp(cmd, "text") == 0) {
        print_windows();
    } else if (strcmp(cmd, "sync") == 0) {
        said_flush();
        printf("SYNC %s frame=%u\n", arg, (unsigned)core.frame_counter);
        fflush(stdout);
    } else if (strcmp(cmd, "flag") == 0) {
        if (nv < 1 || v1 < 1 || v1 >= EVENT_FLAG_COUNT) {
            script_error("usage: flag N [0|1]%s", "");
        } else {
            if (nv >= 2) {
                if (v2)
                    event_flag_set((uint16_t)v1);
                else
                    event_flag_clear((uint16_t)v1);
            }
            printf("FLAG %d = %d\n", v1, event_flag_get((uint16_t)v1));
        }
    } else if (strcmp(cmd, "give") == 0) {
        const ItemConfig *item = nv >= 1 && v1 >= 1 && v1 <= 0xFF ? get_item_entry((uint16_t)v1) : NULL;
        if (!item) {
            script_error("usage: give ITEM [CHAR] (ITEM = item id 1-253)%s", "");
        } else if (nv >= 2 && !party_char(v2)) {
            /* party_char printed the error */
        } else {
            uint16_t who = nv >= 2 ? (uint16_t)v2 : 0xFF;
            uint16_t got = give_item_to_character(who, (uint16_t)v1);
            printf("GIVE item %d (", v1);
            print_eb(item->name, ITEM_NAME_LEN);
            if (got) {
                printf(") -> char %u ", got);
                print_eb(party_characters[got - 1].name, 5);
                printf("\n");
            } else if (nv >= 2) {
                printf(") FAILED: char %d's inventory is full\n", v2);
            } else {
                printf(") FAILED: every party inventory is full\n");
            }
        }
    } else if (strcmp(cmd, "money") == 0) {
        game_state.money_carried = (uint32_t)v1;
        printf("MONEY %u\n", (unsigned)game_state.money_carried);
    } else if (strcmp(cmd, "hp") == 0 || strcmp(cmd, "pp") == 0) {
        CharStruct *c = nv >= 2 ? party_char(v1) : NULL;
        if (c && cmd[0] == 'h')
            poke_hp(c, (uint16_t)v2);
        else if (c)
            poke_pp(c, (uint16_t)v2);
    } else if (strcmp(cmd, "heal") == 0) {
        for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
            CharStruct *c = &party_characters[i];
            poke_hp(c, c->max_hp);
            poke_pp(c, c->max_pp);
        }
    } else if (strcmp(cmd, "status") == 0) {
        /* GROUP 1-7 = afflictions[GROUP-1] (as the text CCs number them),
         * 8 = party_status. VALUE is the stored value (0 clears). */
        CharStruct *c = nv >= 3 ? party_char(v1) : NULL;
        if (nv < 3 || v2 < 1 || v2 > 8 || v3 < 0 || v3 > 255) {
            script_error("usage: status CHAR GROUP VALUE (GROUP 1-7 affliction, 8 party_status)%s", "");
        } else if (v2 == 8) {
            game_state.party_status = (uint8_t)v3;
            printf("STATUS party_status=%d\n", v3);
        } else if (c) {
            c->afflictions[v2 - 1] = (uint8_t)v3;
            Battler *b = party_battler(v1 - 1);
            if (b)
                b->afflictions[v2 - 1] = (uint8_t)v3;
            printf("STATUS char %d group %d = %d", v1, v2, v3);
            if (v3 < 8 && status_names[v2 - 1][v3])
                printf(" (%s)", status_names[v2 - 1][v3]);
            printf("\n");
        }
    } else if (strcmp(cmd, "record") == 0) {
        /* Pause / resume --record-video/--record-audio (no-op otherwise) */
        if (strcmp(arg, "on") == 0)
            record_set_paused(false);
        else if (strcmp(arg, "off") == 0)
            record_set_paused(true);
        else
            script_error("usage: record on|off%s", "");
    } else if (strcmp(cmd, "debug") == 0) {
        /* Set ow.debug_flag directly. Unlike `aux F5` (AUX_DEBUG_TOGGLE in
         * game_main.c), this doesn't also request the debug menu. */
        if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0)
            ow.debug_flag = 1;
        else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0)
            ow.debug_flag = 0;
        else if (arg[0])
            script_error("usage: debug [on|off]%s", "");
        printf("DEBUG %s\n", ow.debug_flag ? "on" : "off");
    } else if (strcmp(cmd, "aux") == 0) {
        if (strcmp(arg, "F1") == 0) *aux |= AUX_DEBUG_DUMP;
        else if (strcmp(arg, "F5") == 0) *aux |= AUX_DEBUG_TOGGLE;
        else if (strcmp(arg, "F6") == 0) *aux |= AUX_SAVESTATE;
        else if (strcmp(arg, "F7") == 0) *aux |= AUX_LOAD_STATE;
        else script_error("unknown aux '%s'", arg);
    } else if (strcmp(cmd, "warps") == 0) {
        for (int i = 1; psi_teleport_entry(i); i++) {
            const uint8_t *e = psi_teleport_entry(i);
            printf("WARP %d ", i);
            print_eb(e, PSI_TELEPORT_DEST_NAME_LEN);
            printf(" (%u,%u)\n", read_u16_le(e + PSI_TELEPORT_DEST_NAME_LEN + 2) * 8,
                   read_u16_le(e + PSI_TELEPORT_DEST_NAME_LEN + 4) * 8);
        }
    } else if (strcmp(cmd, "teleport") == 0 || strcmp(cmd, "warp") == 0) {
        if (cmd[0] == 'w') {
            /* PSI Teleport destination by index or (case-insensitive) name prefix */
            const uint8_t *e = NULL;
            for (int i = 1; arg[0] && psi_teleport_entry(i); i++) {
                char name[PSI_TELEPORT_DEST_NAME_LEN + 1];
                eb_to_ascii_buf(psi_teleport_entry(i), PSI_TELEPORT_DEST_NAME_LEN, name);
                if ((isdigit((unsigned char)arg[0]) && atoi(arg) == i) ||
                    strncasecmp(name, arg, strlen(arg)) == 0) {
                    e = psi_teleport_entry(i);
                    break;
                }
            }
            if (!e) {
                script_error("unknown warp '%s' (see 'warps')", arg);
                return true;
            }
            teleport_x = read_u16_le(e + PSI_TELEPORT_DEST_NAME_LEN + 2) * 8;
            teleport_y = read_u16_le(e + PSI_TELEPORT_DEST_NAME_LEN + 4) * 8;
            teleport_dir = 4;
        } else {
            if (nv < 2) {
                script_error("usage: teleport X Y [DIR]%s", "");
                return true;
            }
            teleport_x = (uint16_t)v1;
            teleport_y = (uint16_t)v2;
            teleport_dir = nv >= 3 ? (uint16_t)(v3 & 7) : 4;
        }
        if (!player_idle()) {
            printf("ERR %s needs the player idle on the overworld (not idle: %s)\n", cmd, not_idle_reason());
            fflush(stdout);
            return true;
        }
        teleport_pending = true;
        return false;
    } else if (strcmp(cmd, "snap") == 0 || strcmp(cmd, "restore") == 0) {
        if (!arg[0] || strchr(arg, '/')) {
            script_error("usage: snap|restore NAME%s", "");
            return true;
        }
        char prefix[128];
        mkdir("snaps", 0755);
        snprintf(prefix, sizeof(prefix), "snaps/%s", arg);
        snprintf(capture_name, sizeof(capture_name), "%s", arg);
        capture_is_load = cmd[0] == 'r';
        platform_savestate_set_path(prefix);
        if (capture_is_load)
            host_request_load();
        else
            host_request_capture();
        capture_waiting = true;
        return false;
    } else if (strcmp(cmd, "end") == 0 || strcmp(cmd, "quit") == 0) {
        fclose(script_file);
        script_file = NULL;
        platform_request_quit();
        return false;
    } else {
        script_error("unknown command '%s'", cmd);
    }
    fflush(stdout);
    return true;
}

/* One frame of a 'press': 2 frames down, 4 up per tap. */
static uint16_t press_step(void) {
    uint16_t pad = press_phase < 2 ? press_pad : 0;
    if (++press_phase == 6) {
        press_phase = 0;
        press_left--;
    }
    return pad;
}

/* Advance the script by one frame. Returns the pad state for this frame and
 * ORs any one-frame aux pulses into *aux. */
uint16_t input_script_next(uint16_t *aux) {
    char line[256];
    uint16_t pad = 0;
    frame_was_rendered = video_frames_rendered != last_poll_render_count;
    last_poll_render_count = video_frames_rendered;

    if (in_root_action || teleport_pending)
        return 0;
    if (capture_waiting) {
        HostCaptureStatus st = host_capture_status();
        if (st == HOST_CAPTURE_PENDING)
            return 0;
        capture_waiting = false;
        platform_savestate_set_path(NULL);
        printf("%s %s %s\n", capture_is_load ? "RESTORE" : "SNAP", capture_name,
               st == HOST_CAPTURE_COMMITTED ? "ok" : "FAILED");
        fflush(stdout);
        if (capture_is_load) {
            /* The loaded game has other windows; forget the captured text */
            said_len = 0;
            said_slot = -1;
            memset(tt_lines, 0, sizeof(tt_lines));
            memset(tt_pos, 0, sizeof(tt_pos));
        }
    }
    if (until_active && until_step(&pad))
        return pad;
    if (press_left > 0)
        return press_step();
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
        if (strncmp(line, "mark ", 5) == 0) {
            /* Chapter mark: n = frames recorded so far (the next frame's
             * index in the recording), frame = core.frame_counter */
            said_flush();
            printf("MARK n=%u frame=%u %s", (unsigned)record_frames(), (unsigned)core.frame_counter,
                   line + 5);
            if (!strchr(line + 5, '\n'))
                printf("\n");
            fflush(stdout);
            continue;
        }
        if (strncmp(line, "note ", 5) == 0) {
            /* Echo a driver note verbatim ('#' included) into the output log */
            said_flush();
            printf("> %s", line + 5);
            if (!strchr(line + 5, '\n'))
                printf("\n");
            fflush(stdout);
            continue;
        }
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char cmd[64] = "", arg[128] = "";
        if (sscanf(line, "%63s %127s", cmd, arg) < 1)
            continue;

        if (isdigit((unsigned char)cmd[0])) {
            hold_frames = atoi(cmd);
            hold_pad = parse_buttons(arg[0] ? arg : "-");
        } else if (strcmp(cmd, "wait") == 0) {
            hold_frames = atoi(arg);
            hold_pad = 0;
        } else if (strcmp(cmd, "press") == 0) {
            int count = 1;
            sscanf(line, "%*s %*s %d", &count);
            press_pad = parse_buttons(arg);
            press_left = count > 0 ? count : 1;
            press_phase = 0;
            return press_step();
        } else if (strcmp(cmd, "until") == 0) {
            if (start_until(line) && until_step(&pad))
                return pad;
        } else if (!run_command(cmd, arg, line, aux)) {
            return 0;
        }
    }
    hold_frames--;
    return hold_pad;
}

/* Called by the main loop at the root boundary, after host_root_boundary():
 * perform a pending teleport while no mode step is mid-execution. */
void input_script_root_boundary(void) {
    if (!teleport_pending)
        return;
    teleport_pending = false;
    if (!player_idle()) {
        printf("ERR teleport needs the player idle on the overworld (not idle: %s)\n",
               not_idle_reason());
        fflush(stdout);
        return;
    }
    in_root_action = true;
    /* As the PSI Teleport arrival (overworld_teleport.c): force a full map
     * reload, then map + party + music, and spawn the NPCs. */
    ml.current_map_music_track = (uint16_t)-1;
    ow.loaded_map_palette = -1;
    ow.loaded_map_tile_combo = -1;
    initialize_map(teleport_x, teleport_y, teleport_dir);
    ow.player_has_moved_since_map_load = 0;
    flush_entity_creation_queue();
    /* The party reload leaves its entities disabled; the incoming screen
     * transition re-enables them (door.c), so do the same. */
    enable_all_entities();
    /* load_map_at_position's CLEAR_ALL_ENEMIES removed every non-party entity,
     * including the Buzz Buzz follower and the pending timed-delivery entities.
     * Respawn them as the PSI Teleport arrival does (door.c TT_FINALIZE). */
    ow.stairs_direction = (uint16_t)-1;
    spawn_buzz_buzz();
    in_root_action = false;
    printf("TELEPORT (%u,%u) dir=%s\n", teleport_x, teleport_y, dir_names[teleport_dir & 7]);
    fflush(stdout);
}
