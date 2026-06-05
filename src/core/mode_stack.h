#ifndef EB_CORE_MODE_STACK_H
#define EB_CORE_MODE_STACK_H

#include "core/types.h"

/* ---------------------------------------------------------------------------
 * Explicit mode stack (savestate-anywhere migration, phase 2).
 *
 * Modal contexts (battle, menus, dialogue, fades, intro) historically ran as
 * blocking C loops that called wait_for_vblank() while holding live locals on
 * the native call stack — state that cannot be serialized to a savestate. The
 * mode stack replaces those loops with run-to-completion "step" functions whose
 * per-frame work is split from the single host_process_frame() yield, and whose
 * former stack locals are hoisted into a serializable ModeState.
 *
 * Modal loops nest (overworld -> battle -> menu -> number-select -> text-wait),
 * so a flat enum is insufficient: the active context is a STACK of modes, each
 * with its own hoisted state. A savestate is then state_dump_save() plus this
 * ModeStack.
 *
 * Migration is leaf -> root. Until the root loop is the only pump, a still-
 * blocking parent drives an already-converted child with pump_mode(), which runs
 * the child's step functions to completion using a LOCAL host_process_frame()
 * yield. pump_mode is deleted at cutover. See
 * docs/plans/savestate-unified-loop.md.
 * ------------------------------------------------------------------------- */

typedef enum {
    GAME_MODE_NONE = 0,
    GAME_MODE_FADE_WAIT,       /* pilot A: wait for a brightness fade to finish */
    GAME_MODE_NUMBER_SELECT,   /* pilot B: interactive multi-digit number entry */
    GAME_MODE_CHAR_SELECT,     /* battle-style HP/PP character column selection */
    GAME_MODE_TEXT_DELAY,      /* fixed frame-count delay (text-speed typing pause) */
    GAME_MODE_ACTIONSCRIPT_WAIT, /* wait until an entity actionscript signals done */
    GAME_MODE_TEXT_PROMPT,     /* cc_halt: text-advance wait + blinking triangle */
    GAME_MODE_COUNT,
} GameMode;

typedef enum {
    STEP_CONTINUE,   /* stay in this mode; the loop yields one frame          */
    STEP_PUSH,       /* enter a child mode (push_mode); the loop yields        */
    STEP_POP,        /* this mode is done; hand pop_result back to the parent  */
} StepKind;

typedef struct {
    StepKind kind;
    GameMode push_mode;   /* valid when kind == STEP_PUSH */
    int32_t  pop_result;  /* valid when kind == STEP_POP  */
} StepResult;

/* Convenience constructors for step functions. */
#define STEP_RESULT_CONTINUE()  ((StepResult){ .kind = STEP_CONTINUE })
#define STEP_RESULT_PUSH(m)     ((StepResult){ .kind = STEP_PUSH, .push_mode = (m) })
#define STEP_RESULT_POP(r)      ((StepResult){ .kind = STEP_POP,  .pop_result = (int32_t)(r) })

/* Which per-frame "tick" body a GAME_MODE_FADE_WAIT runs while the fade is in
 * progress. Each former blocking loop did slightly different per-frame work. */
typedef enum {
    FADE_TICK_OVERWORLD_RENDER = 0, /* oam_clear; run_actionscript_frame; update_screen; fade_update */
    FADE_TICK_BATTLE_EFFECTS,       /* update_battle_screen_effects() */
    FADE_TICK_WINDOW,               /* window_tick_work() — battle/menu fades with live windows */
} FadeTickKind;

typedef struct {
    uint8_t tick_kind;   /* FadeTickKind */
} FadeWaitState;

/* GAME_MODE_NUMBER_SELECT phases. The blocking original (CC 0x52 / NUM_SELECT_
 * PROMPT) was a two-level loop where each rendered frame was followed by two
 * yields before the first input read (window_tick's yield, then the input
 * loop's first update_hppp yield). NS_PRIME reproduces that second yield so the
 * input timing is frame-identical to the blocking version. */
typedef enum {
    NS_RENDER = 0,  /* draw digits + window_tick_work, then yield */
    NS_PRIME,       /* one update_hppp_meter_work frame before reading input */
    NS_INPUT,       /* read fresh input; act; idle frames run update_hppp_meter_work */
} NumberSelectPhase;

typedef struct {
    uint8_t  phase;        /* NumberSelectPhase */
    uint16_t start_x;      /* saved focus-window text cursor (@LOCAL: start col) */
    uint16_t start_y;      /* saved focus-window text cursor (@LOCAL: start row) */
    uint16_t max_digits;   /* number of digit positions (CC arg) */
    uint16_t cursor_pos;   /* selected digit, 1-based from the right (@LOCAL04) */
    int32_t  value;        /* current number (@LOCAL05) */
    int32_t  place_value;  /* multiplier for the selected digit (@LOCAL03) */
} NumberSelectState;

/* GAME_MODE_CHAR_SELECT — battle-style HP/PP character column selection
 * (char_select_prompt, battle.c, mode 0/2; mode 1 keeps the blocking
 * selection_menu path). Its on_change/check_valid callbacks were function
 * pointers, which cannot live in a serializable ModeState, so they are stored as
 * IDs and dispatched via cs_invoke_*() (defined in text.c). */
typedef enum {
    CS_ONCHANGE_NONE = 0,
    CS_ONCHANGE_EQUIPMENT,    /* show_equipment_and_stats_callback */
    CS_ONCHANGE_PSI_LIST,     /* display_character_psi_list */
    CS_ONCHANGE_STATUS,       /* display_status_window */
    CS_ONCHANGE_WEAPON_NAME,  /* get_weapon_item_name_callback */
    CS_ONCHANGE_BODY_NAME,    /* get_body_item_name_callback */
    CS_ONCHANGE_PARTY_SELECT_SCRIPT, /* party_character_selector: show per-member text script */
} CharSelectOnChangeId;

typedef enum {
    CS_CHECKVALID_NONE = 0,
    CS_CHECKVALID_PSI,        /* check_character_psi_availability */
} CharSelectCheckValidId;

typedef enum {
    CSP_RENDER = 0,  /* highlight char + window_tick_work + pagination arrows */
    CSP_PRIME,       /* first update_hppp frame before the input read */
    CSP_INPUT,       /* poll input within the `delay` counter window */
} CharSelectPhase;

typedef struct {
    uint8_t  phase;          /* CharSelectPhase */
    uint8_t  mode;           /* 0 or 2 (battle-style) */
    uint8_t  allow_cancel;
    uint8_t  on_change_id;   /* CharSelectOnChangeId */
    uint8_t  check_valid_id; /* CharSelectCheckValidId */
    uint16_t current_index;  /* selected party slot (0-based) */
    uint16_t delay;          /* input poll frames before pagination toggle */
    uint16_t counter;        /* frames elapsed in the current poll window */
    uint32_t saved_argument_memory; /* restored on pop (focus window arg memory) */
} CharSelectState;

/* GAME_MODE_TEXT_DELAY — run update_hppp_meter_work() for a fixed number of
 * frames, optionally breaking early on a text-advance press. Frame-faithful port
 * of the CC 0x1F 0x60 TEXT_SPEED_DELAY loop: the blocking loop checks the input
 * break AFTER each update_hppp_meter_and_render() (i.e. post-yield), so the check
 * sits at the TOP of each step and `primed` suppresses it on the very first frame
 * (no yield has happened yet inside this mode).
 *
 * cc_pause (CC 0x10) reuses this mode with lead_window=1: it renders one leading
 * window_tick_work() frame (the caller has already cleared instant-printing)
 * before the non-cancelable delay, matching TICK_HPPP_METER_N_FRAMES. */
typedef struct {
    uint16_t remaining;    /* frames left to render */
    uint8_t  cancelable;   /* break on PAD_TEXT_ADVANCE */
    uint8_t  primed;       /* 0 on the first frame (skip the pre-work input check) */
    uint8_t  lead_window;  /* do one leading window_tick_work frame before the delay */
} TextDelayState;

/* GAME_MODE_ACTIONSCRIPT_WAIT — port of CC 0x1F 0x61 WAIT_FOR_ACTIONSCRIPT. An
 * initial window_tick_work() frame renders open windows, then render_frame_tick_
 * work() runs each frame until ert.actionscript_state becomes non-zero. The
 * completion check sits at the top of AS_RENDER (post-yield), so the frame that
 * sets the state still yields, exactly matching the blocking while-loop's yield
 * count. The caller resets ert.actionscript_state before pushing this mode. */
typedef enum {
    AS_INIT = 0,   /* window_tick_work, then yield */
    AS_RENDER,     /* check state set last frame; else render_frame_tick_work */
} ActionscriptWaitPhase;

typedef struct {
    uint8_t phase;   /* ActionscriptWaitPhase */
} ActionscriptWaitState;

/* GAME_MODE_TEXT_PROMPT — run-to-completion port of cc_halt (CC 0x03/0x13/0x14,
 * halt.asm): wait at a text prompt for a button press, with an optional blinking
 * triangle and an optional text-speed auto-advance shortcut.
 *
 * The blocking original was a sequence of distinct loops: (1) drain
 * dt.text_prompt_waiting_for_input via render_frame_tick; (2) one window_tick
 * frame; then one of three mutually-exclusive waits — the text-speed shortcut
 * loop, the no-triangle button wait, or the blinking-triangle animation. Each
 * becomes a phase. `primed` reproduces the post-yield input check of the
 * for/do-while branches (suppress the check on their first frame); the triangle
 * branch checks input pre-work every frame, so it ignores `primed`. */
typedef enum {
    TP_WAIT_PROMPT = 0, /* render_frame_tick_work until prompt-wait clears, then window+decide */
    TP_TEXTSPEED,       /* text-speed auto-advance shortcut (returns w/o teardown) */
    TP_WAIT_BUTTON,     /* no triangle: wait for a text-advance press */
    TP_TRIANGLE,        /* blinking-triangle animation until text-advance */
} TextPromptPhase;

typedef struct {
    uint8_t  phase;          /* TextPromptPhase */
    uint8_t  show_triangle;  /* cc_halt show_triangle param */
    uint8_t  skip_text_speed;/* cc_halt skip_text_speed param */
    uint8_t  primed;         /* 0 on a branch's first frame (post-yield input check) */
    uint8_t  tri_big;        /* triangle: 1 = big sub-frame, 0 = small */
    uint8_t  tri_need_tile;  /* triangle: write the sub-frame's tile this step */
    uint8_t  tri_ticks;      /* triangle: ticks left in the current sub-frame */
    uint16_t tri_pos;        /* triangle: bottom-right tilemap index */
    uint16_t remaining;      /* text-speed shortcut frames left */
} TextPromptState;

/* Per-mode hoisted locals (former stack variables). MUST be plain-old-data: no
 * pointers into the stack or heap that would not survive a save/reload. Sized
 * with headroom so adding a future mode's locals does not change the on-disk
 * ModeStack layout for already-shipped modes. */
typedef union {
    FadeWaitState         fade_wait;
    NumberSelectState     number_select;
    CharSelectState       char_select;
    TextDelayState        text_delay;
    ActionscriptWaitState actionscript_wait;
    TextPromptState       text_prompt;
    uint8_t               _raw[64];
} ModeState;

#define MODE_STACK_MAX 8

typedef struct {
    uint8_t   depth;                          /* number of active modes */
    uint8_t   mode[MODE_STACK_MAX];           /* GameMode per level */
    ModeState state[MODE_STACK_MAX];          /* hoisted locals per level */
    int32_t   child_result[MODE_STACK_MAX];   /* result a child handed back on POP */
} ModeStack;

extern ModeStack g_mode_stack;

/* Run one frame of `mode`'s step function. */
StepResult mode_dispatch_step(GameMode mode, ModeState *st);

/* GAME_MODE_NUMBER_SELECT step (defined in display_text_cc.c, where the text/
 * window helpers it needs live). Declared here so the dispatch table can wire it
 * up. Init via ModeState.number_select before pump_mode(GAME_MODE_NUMBER_SELECT).
 * Pops the entered value, or -1 on cancel. */
StepResult mode_step_number_select(ModeState *st);

/* GAME_MODE_TEXT_DELAY / GAME_MODE_ACTIONSCRIPT_WAIT steps (defined in
 * display_text_cc.c, where the dt/ert/window helpers they need are visible). */
StepResult mode_step_text_delay(ModeState *st);
StepResult mode_step_actionscript_wait(ModeState *st);
StepResult mode_step_text_prompt(ModeState *st);

/* GAME_MODE_CHAR_SELECT step (defined in battle.c). Init via
 * ModeState.char_select before pump_mode(GAME_MODE_CHAR_SELECT). Pops the 1-based
 * party member ID, or 0 on cancel. */
StepResult mode_step_char_select(ModeState *st);

/* Push `mode` onto the stack. If `init` is non-NULL its contents become the new
 * level's ModeState; otherwise the state is zeroed. */
void mode_push(GameMode mode, const ModeState *init);

/* Pop the top mode, recording `result` into the parent's child_result slot.
 * Returns `result`. */
int32_t mode_pop(int32_t result);

/* Migration bridge: push `mode` (with optional initial state) and run it — and
 * any children it pushes — to completion using a LOCAL host_process_frame()
 * yield, then return its pop_result. Lets a still-blocking parent invoke an
 * already-converted child. Deleted at cutover, when the root loop is the only
 * pump. Returns 0 early if the user requested quit. */
int32_t pump_mode(GameMode mode, const ModeState *init);

#endif /* EB_CORE_MODE_STACK_H */
