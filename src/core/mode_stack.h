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
    GAME_MODE_SELECTION_MENU,  /* keystone menu primitive (selection_menu) */
    GAME_MODE_TOWN_MAP,        /* town map viewer (display_town_map / run_town_map_menu) */
    GAME_MODE_SOUND_STONE,     /* sound stone melody playback (use_sound_stone) */
    GAME_MODE_DEBUG_YMENU,     /* debug Y-button leaf menus (flag editor, guide counter) */
    GAME_MODE_BATTLE_WAIT,     /* battle wait loops (PSI/screen-effect/meter/swirl/frames) */
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

/* GAME_MODE_SELECTION_MENU — run-to-completion port of selection_menu()
 * (window.c), the keystone menu primitive (pause menu, shops, file select,
 * mode-1 char select, ...). The blocking two-level loop becomes a three-phase
 * machine. Almost all of the menu's live state already lives in the serializable
 * WindowInfo (current_option, selected_option, menu_page_number, text_x/y), so
 * little is hoisted here; `w` is re-fetched via get_window(win.current_focus_
 * window) at the top of each step (a pointer is not serializable, and the focus
 * window is stable for the menu's lifetime — restored after each callback).
 *
 * The window's cursor_move_callback is invoked directly off the (live, re-
 * fetchable) WindowInfo; it is NOT hoisted. WindowInfo already stores it as a
 * raw function pointer (and content_tilemap as a heap pointer) and is serialized
 * by SECTION_WINDOW today — making those pointers savestate-safe is a pre-
 * existing serialization-hardening task for the cutover, independent of this
 * control-flow conversion.
 *
 * Frame timing mirrors the original exactly via `primed`: the blocking loop reads
 * input only AFTER its per-frame update_hppp_meter_and_render() yield, and the
 * entry path yields twice (setup window_tick, then the first update_hppp) before
 * the first input read. SM_SETUP is the first yield; an SM_MAIN render-only frame
 * (primed=0) is the second; thereafter SM_MAIN reads input then renders (primed=1).
 * A cursor move adds one window_tick_work yield + one render-only frame before the
 * next input read; a page-flip adds two window_tick_work yields + one render-only
 * frame — each matching the blocking version's `continue` paths frame for frame. */
typedef enum {
    SM_SETUP = 0,  /* one-shot setup; ends with window_tick_work, then yields */
    SM_MAIN,       /* cursor blink + per-frame HP/PP render + input handling */
    SM_PAGE2,      /* second half of an overflow page-flip re-render */
} SelectionMenuPhase;

typedef struct {
    uint8_t  phase;         /* SelectionMenuPhase */
    uint8_t  allow_cancel;  /* selection_menu() arg */
    uint8_t  primed;        /* SM_MAIN: 1 = read input this frame, 0 = render only */
    uint8_t  redraw_cursor; /* toggle + rewrite the cursor tiles this frame */
    uint8_t  cursor_frame;  /* blink sub-frame (0/1) */
    uint16_t frame_counter; /* frames since last cursor toggle */
} SelectionMenuState;

/* GAME_MODE_TOWN_MAP — run-to-completion port of display_town_map() (overworld X
 * button) and run_town_map_menu() (items menu). Both share one mode via
 * `menu_mode`. The blocking helper load_town_map_data() embedded a bare
 * while(fade_active()) wait; it is split into load_town_map_begin() (fade_out +
 * decomp) and load_town_map_finish() (palette/VRAM uploads + fade_in), with the
 * former fade-wait inlined as TM_LOAD_WAIT (each CONTINUE yields, advancing the
 * fade via host_process_frame). The menu variant re-enters TM_LOAD_BEGIN to
 * reload when the up/down selection changes maps.
 *
 * Input timing follows the established post-yield pattern (overworld_step): a step
 * renders the current frame, then acts on the input the pump's prior yield
 * latched. The display variant checks its exit buttons after update_screen (like
 * the blocking loop); the menu variant checks A after render but before
 * update_screen (matching the blocking `if (A) break;` placement). */
typedef enum {
    TM_LOAD_BEGIN = 0, /* fade_out + decomp gfx, then wait for fade */
    TM_LOAD_WAIT,      /* bare fade-wait; on done, finish load -> TM_MAIN */
    TM_MAIN,           /* render icons + handle input (display exit / menu nav) */
    TM_FADEOUT,        /* display variant: 16-frame fade-out render loop, then pop */
} TownMapPhase;

typedef struct {
    uint8_t  phase;         /* TownMapPhase */
    uint8_t  menu_mode;     /* 0 = display_town_map, 1 = run_town_map_menu */
    uint8_t  map_id;        /* current map index (0-5) */
    uint8_t  prev_map;      /* menu variant: last loaded map (suppresses re-reload) */
    uint16_t fadeout_count; /* display variant: fade-out render frames remaining */
} TownMapState;

/* GAME_MODE_SOUND_STONE — run-to-completion port of use_sound_stone()
 * (display_text_menus.c), the Sound Stone melody-playback screen. The blocking
 * original had a one-shot setup with two embedded yields (force-blank, then
 * blank-screen + fade-in), a long per-frame animation/sequencing loop, and a
 * fade-out + force-blank teardown. Each former yield becomes a phase boundary;
 * the heavy per-melody animation state (ps[8]) and loop scalars are hoisted here
 * so nothing lives on the C stack across a frame. Asset pointers are re-derived
 * from ASSET_DATA at the top of each step (deterministic, not serialized). */
typedef struct {
    int16_t state;       /* 0=inactive, 1=idle, 2=playing */
    int16_t counter;     /* animation frame counter */
    int16_t tile_toggle; /* orbit tile frame modifier (0 or 2) */
    int16_t orbit_frame; /* index into melody data */
    int16_t orbit_pos1;  /* orbit radius/position */
    int16_t orbit_pos2;  /* orbit angle accumulator */
    int16_t pad;         /* unused (matches the 14-byte ROM layout) */
} SoundStonePlayback;

typedef enum {
    SS_SETUP1 = 0, /* parse config + force-blank work, then yield */
    SS_SETUP2,     /* load gfx/palettes/bg + init melodies + blank-screen work, then yield */
    SS_FADEIN,     /* fade_in + init loop scalars, then yield (matches the loop's first yield) */
    SS_MAIN,       /* per-frame sequencing + sprite animation; on exit -> SS_FADEOUT */
    SS_FADEOUT,    /* wait for fade-out; then force-blank work + yield */
    SS_EXIT,       /* set color math + reload_map + fade_in, then pop */
} SoundStonePhase;

typedef struct {
    uint8_t  phase;          /* SoundStonePhase */
    uint8_t  cancellable;    /* use_sound_stone() arg: A/B/X cancels early */
    int16_t  center_timer;   /* @LOCAL0E */
    int16_t  center_frame;   /* @LOCAL0F */
    int16_t  initial_delay;  /* @LOCAL0D */
    int16_t  exit_countdown; /* @LOCAL0C */
    int16_t  seq_index;      /* @LOCAL0B */
    int16_t  timing_counter; /* @VIRTUAL04 / @LOCAL0A */
    int16_t  current_melody; /* @VIRTUAL02 / @LOCAL09 */
    int16_t  collected_count;
    SoundStonePlayback ps[8];
} SoundStoneState;

/* GAME_MODE_DEBUG_YMENU — run-to-completion port of the two clean-leaf debug
 * Y-button menus (debug_y_button_flag, debug_y_button_guide in game_main.c). Both
 * are an outer redraw + inner input wait; `kind` selects which. (debug_y_button_
 * goods is NOT here: its A action calls char_select_prompt(mode 1), which is still
 * a blocking selection_menu wrapper, not a pushable mode — converting goods now
 * would leave its "inside char_select" position on the native stack. Revisit when
 * char_select_prompt mode 1 becomes a mode, then convert goods via STEP_PUSH.) */
typedef enum {
    DBG_YMENU_FLAG = 0,  /* event flag editor */
    DBG_YMENU_GUIDE,     /* active-script entity counter (draw once, wait for cancel) */
} DebugYMenuKind;

typedef enum {
    DY_DRAW = 0,  /* (re)draw the window via window_tick_work, then yield */
    DY_INPUT,     /* read input; FLAG: nav/toggle/cancel; GUIDE: wait for cancel */
} DebugYMenuPhase;

typedef struct {
    uint8_t  phase;  /* DebugYMenuPhase */
    uint8_t  kind;   /* DebugYMenuKind */
    uint16_t index;  /* FLAG: current flag index (1-1999) */
} DebugYMenuState;

/* GAME_MODE_BATTLE_WAIT — run-to-completion port of the family of blocking
 * "advance one frame until <condition>" loops scattered through the battle code.
 * Each former loop body funnelled through window_tick() (or, for the swirl-update
 * variant, wait_for_vblank()+update_swirl_effect()); the single yield now belongs
 * to the pump. `kind` selects the per-frame body and the exit condition:
 *
 *   BW_FRAMES        - run window_tick_work() for `remaining` frames (battle_wait;
 *                      the 12-frame attacker-bob delay). Check-before (POP at 0).
 *   BW_PSI_ANIM      - window_tick_work() while is_psi_animation_active().
 *   BW_SCREEN_EFFECT - window_tick_work() while bt.screen_effect_minimum_wait_frames.
 *   BW_HPPP_STABLE   - window_tick_work()+reset_hppp_meter_speed_if_stable() until
 *                      check_all_hppp_meters_stable(). The blocking loop checks the
 *                      condition AFTER the per-frame work, so `primed` suppresses
 *                      the exit check on the first step (same scheme as TEXT_DELAY).
 *   BW_SWIRL_WINDOW  - window_tick_work() while is_battle_swirl_active()
 *                      (load_battle_scene swirl-in / swirl-out).
 *   BW_SWIRL_UPDATE  - update_swirl_effect() while is_battle_swirl_active()
 *                      (init_battle_scripted). The blocking loop yields BEFORE the
 *                      update, so `primed` defers the update to the step that
 *                      follows the prior yield, keeping the yield/update interleave
 *                      frame-identical (no phase shift).
 *
 * The check-before kinds (PSI/SCREEN_EFFECT/SWIRL_WINDOW) and BW_FRAMES match the
 * GAME_MODE_FADE_WAIT pattern: test the exit condition at the top, else do the
 * frame's work and CONTINUE (the pump yields). */
typedef enum {
    BW_FRAMES = 0,
    BW_PSI_ANIM,
    BW_SCREEN_EFFECT,
    BW_HPPP_STABLE,
    BW_SWIRL_WINDOW,
    BW_SWIRL_UPDATE,
} BattleWaitKind;

typedef struct {
    uint8_t  kind;      /* BattleWaitKind */
    uint8_t  primed;    /* BW_HPPP_STABLE: 0 skips the exit check; BW_SWIRL_UPDATE: 0 skips the update */
    uint16_t remaining; /* BW_FRAMES: frames left to render */
} BattleWaitState;

/* Per-mode hoisted locals (former stack variables). MUST be plain-old-data: no
 * pointers into the stack or heap that would not survive a save/reload. Sized
 * with headroom so adding a future mode's locals does not change the on-disk
 * ModeStack layout for already-shipped modes. (The reserve grew from 64 to 160
 * for SoundStoneState's ps[8]; savestates are not cross-build compatible, so the
 * one-time on-disk size change is harmless pre-cutover.) */
typedef union {
    FadeWaitState         fade_wait;
    NumberSelectState     number_select;
    CharSelectState       char_select;
    TextDelayState        text_delay;
    ActionscriptWaitState actionscript_wait;
    TextPromptState       text_prompt;
    SelectionMenuState    selection_menu;
    TownMapState          town_map;
    SoundStoneState       sound_stone;
    DebugYMenuState       debug_ymenu;
    BattleWaitState       battle_wait;
    uint8_t               _raw[160];
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

/* GAME_MODE_SELECTION_MENU step (defined in window.c, where selection_menu's
 * helpers and the cursor VRAM layout live). Init via ModeState.selection_menu
 * (phase = SM_SETUP, allow_cancel) before pump_mode(GAME_MODE_SELECTION_MENU).
 * Pops the chosen item's userdata, or 0 on cancel. */
StepResult mode_step_selection_menu(ModeState *st);

/* GAME_MODE_TOWN_MAP step (defined in town_map.c). Init via ModeState.town_map
 * (phase = TM_LOAD_BEGIN, menu_mode, map_id) before pump_mode(GAME_MODE_TOWN_MAP).
 * Always pops 0; the caller derives its return value from its own state. */
StepResult mode_step_town_map(ModeState *st);

/* GAME_MODE_SOUND_STONE step (defined in display_text_menus.c). Init via
 * ModeState.sound_stone (phase = SS_SETUP1, cancellable) before
 * pump_mode(GAME_MODE_SOUND_STONE). Always pops 0. */
StepResult mode_step_sound_stone(ModeState *st);

/* GAME_MODE_DEBUG_YMENU step (defined in game_main.c). Init via
 * ModeState.debug_ymenu (phase = DY_DRAW, kind, index) before
 * pump_mode(GAME_MODE_DEBUG_YMENU). Always pops 0. */
StepResult mode_step_debug_ymenu(ModeState *st);

/* GAME_MODE_BATTLE_WAIT step (defined in battle.c, where the swirl/PSI/meter
 * predicates live). Init via ModeState.battle_wait (kind, plus `remaining` for
 * BW_FRAMES) before pump_mode(GAME_MODE_BATTLE_WAIT). Always pops 0. */
StepResult mode_step_battle_wait(ModeState *st);

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
