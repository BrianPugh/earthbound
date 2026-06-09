#ifndef EB_CORE_MODE_STACK_H
#define EB_CORE_MODE_STACK_H

#include "core/types.h"
#include "game/display_text.h"  /* ScriptReader (embedded in DisplayTextState) */

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
    GAME_MODE_BATTLE_ROW_SELECT,   /* select_battle_row: front/back row targeting */
    GAME_MODE_BATTLE_ENEMY_SELECT, /* select_battle_target: single-battler targeting */
    GAME_MODE_NAMING_EVENTS,       /* naming-screen walk-out: wait for entity scripts to finish */
    GAME_MODE_TEXT_INPUT,          /* on-screen keyboard naming dialog (text_input_dialog) */
    GAME_MODE_NAMING_PROMPT,       /* naming prompt: render name box, wait for any button */
    GAME_MODE_SCREEN_TRANSITION,   /* door/screen fade-in/out transition (screen_transition) */
    GAME_MODE_PALETTE_FADE,        /* overworld palette-fade loops (skippable_pause et al) */
    GAME_MODE_MAP_PALETTE_FADE,    /* map-load BG palette cross-fade (load_map_palette) */
    GAME_MODE_MOSAIC_FADE,         /* brightness-ramp mosaic fade in/out (callroutine, flyover) */
    GAME_MODE_FLYOVER,             /* flyover text + coffee/tea cutscene interpreters */
    GAME_MODE_INTRO_LOGO,          /* intro logo sequence (Nintendo/APE/HAL) */
    GAME_MODE_GAS_STATION,         /* gas-station prologue (RUN_GAS_STATION_CREDITS) */
    GAME_MODE_TITLE_SCREEN,        /* title screen (show_title_screen) */
    GAME_MODE_ATTRACT,             /* attract-mode demo scene (run_attract_mode) */
    GAME_MODE_FILE_MENU,           /* file-select cascade (file_menu_loop) */
    GAME_MODE_INIT_INTRO,          /* intro state machine (init_intro) */
    GAME_MODE_DISPLAY_TEXT,        /* text bytecode interpreter (display_text) */
    GAME_MODE_ENTITY_FADE_WAIT,    /* wait until ow.entity_fade_entity == -1 (window_tick) */
    GAME_MODE_TEXT_WAIT_FADE,      /* overworld interaction: dialogue then entity-fade wait */
    GAME_MODE_PROCESS_INTERACTION, /* overworld interaction dispatch (process_queued_interactions) */
    GAME_MODE_COUNT,
} GameMode;

typedef enum {
    STEP_CONTINUE,   /* stay in this mode; the loop yields one frame          */
    STEP_PUSH,       /* enter a child mode (push_mode); the loop yields        */
    STEP_POP,        /* this mode is done; hand pop_result back to the parent  */
} StepKind;

/* Forward declaration: ModeState is defined further down (it references the
 * per-mode state structs). StepResult embeds one by value so a step that returns
 * STEP_PUSH can carry the child's initial state — a pointer to a step-local would
 * be dangling by the time the pump/root applies it. */
typedef union ModeState ModeState;

typedef struct {
    StepKind  kind;
    GameMode  push_mode;   /* valid when kind == STEP_PUSH */
    int32_t   pop_result;  /* valid when kind == STEP_POP  */
    ModeState *push_init;  /* STEP_PUSH: optional initial state (NULL = zeroed) */
} StepResult;

/* Convenience constructors for step functions. */
#define STEP_RESULT_CONTINUE()  ((StepResult){ .kind = STEP_CONTINUE })
#define STEP_RESULT_PUSH(m)     ((StepResult){ .kind = STEP_PUSH, .push_mode = (m) })
#define STEP_RESULT_POP(r)      ((StepResult){ .kind = STEP_POP,  .pop_result = (int32_t)(r) })

/* STEP_PUSH carrying an initial ModeState for the child. `init` must point at
 * storage that outlives the dispatch call — in practice a `static` ModeState in
 * the step function, or a field hoisted into the parent's own ModeState (which
 * lives in the serializable g_mode_stack, not on the C stack). The pump/root
 * copies *init into the child's level immediately, so the pointer is only
 * dereferenced within the same dispatch turn. */
#define STEP_RESULT_PUSH_INIT(m, init) \
    ((StepResult){ .kind = STEP_PUSH, .push_mode = (m), .push_init = (init) })

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

/* GAME_MODE_TEXT_WAIT_FADE phases. Port of display_text_and_wait_for_fade():
 * disable entities, show dialogue, wait for the entity fade-out to finish, then
 * re-enable. Each phase STEP_PUSHes the next child so the whole interaction lives
 * on the mode stack (serializable) instead of the C stack. */
typedef enum {
    TWF_TEXT = 0,  /* disable entities + STEP_PUSH GAME_MODE_DISPLAY_TEXT */
    TWF_FADE,      /* STEP_PUSH GAME_MODE_ENTITY_FADE_WAIT */
    TWF_DONE,      /* enable entities + POP */
} TextWaitFadePhase;

typedef struct {
    uint8_t  phase;       /* TextWaitFadePhase */
    uint32_t text_addr;   /* dialogue address to resolve + display */
} TextWaitFadeState;

/* GAME_MODE_PROCESS_INTERACTION phases. Port of process_queued_interactions():
 * dequeue one interaction and dispatch by type. Text types (0/8/9/10) STEP_PUSH
 * GAME_MODE_TEXT_WAIT_FADE; the door type (2) calls door_transition() inline
 * (still a blocking driver — deferred); the trailing pending/clear bookkeeping
 * runs in PI_RESUME (after the pushed text pops) or inline for the non-text
 * types (no extra yield, matching the original). */
typedef enum {
    PI_DISPATCH = 0,  /* dequeue + dispatch */
    PI_RESUME,        /* post-text bookkeeping after TEXT_WAIT_FADE pops */
} ProcessInteractionPhase;

typedef struct {
    uint8_t  phase;       /* ProcessInteractionPhase */
    uint16_t type;        /* dequeued interaction type */
    uint32_t data_ptr;    /* dequeued interaction data (text addr / door ptr) */
} ProcessInteractionState;

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
    CSP_INIT = 0,    /* initial on_change (may STEP_PUSH a text child) then first render */
    CSP_RENDER,      /* highlight char + window_tick_work + pagination arrows */
    CSP_PRIME,       /* first update_hppp frame before the input read */
    CSP_INPUT,       /* poll input within the `delay` counter window */
} CharSelectPhase;

/* Post-child resume for an on_change callback that STEP_PUSHed a GAME_MODE_DISPLAY_TEXT
 * child (CS_ONCHANGE_PARTY_SELECT_SCRIPT). The deferred render tail runs on the frame
 * the child pops back into mode_step_char_select. */
typedef enum {
    CS_RESUME_NONE = 0,
    CS_RESUME_INIT,  /* initial on_change child popped: proceed to first render */
    CS_RESUME_NAV,   /* per-navigation on_change child popped: re-render at PRIME */
} CharSelectResume;

typedef struct {
    uint8_t  phase;          /* CharSelectPhase */
    uint8_t  mode;           /* 0 or 2 (battle-style) */
    uint8_t  allow_cancel;
    uint8_t  on_change_id;   /* CharSelectOnChangeId */
    uint8_t  check_valid_id; /* CharSelectCheckValidId */
    uint8_t  resume;         /* CharSelectResume: post-child work pending on POP */
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

/* GAME_MODE_BATTLE_ROW_SELECT — run-to-completion port of select_battle_row()
 * (battle_targeting.c): UP/DOWN choose the front (1) or back (2) row, A confirms,
 * B cancels. Three-phase machine in the verified char-select idiom: a render frame
 * (set_battler_flashing + target text + the inline "WINDOW_TICK equivalent" battle
 * render, via targeting_render_work()), then BR_PRIME's first update_hppp_meter_
 * work() frame before the input read, then BR_INPUT. A row change re-renders INLINE
 * in BR_INPUT (no extra leading yield) and returns to BR_PRIME, exactly like
 * char-select's navigation path. Pops row+1 (1=front, 2=back), or 0 on cancel. */
typedef enum {
    BR_RENDER = 0, /* flashing + target text + targeting render frame, then yield */
    BR_PRIME,      /* one update_hppp_meter_work frame before the input read */
    BR_INPUT,      /* read input: UP/DOWN row change, A confirm, B cancel */
} BattleRowSelectPhase;

typedef struct {
    uint8_t  phase;        /* BattleRowSelectPhase */
    uint8_t  allow_cancel; /* select_battle_row() arg */
    uint16_t current_row;  /* 0=front, 1=back */
} BattleRowSelectState;

/* GAME_MODE_BATTLE_ENEMY_SELECT — run-to-completion port of select_battle_target()
 * (battle_targeting.c): LEFT/RIGHT cycle battlers within a row, UP/DOWN switch
 * rows, A confirms, B cancels. The blocking goto-machine had three re-render entry
 * points; this maps to:
 *   ET_DISPLAY - the `update_target_display` work (recompute x_pos, enemy_flashing_
 *                on, conditional target text when target_shown==0, target_shown++,
 *                targeting_render_work), then yield. `pending_sfx` plays at the top
 *                (the blocking code's play_sfx ran right after the apply-common
 *                yield, i.e. in this frame). Used for the initial entry and as the
 *                second render of a selection change.
 *   ET_PRIME   - one update_hppp_meter_work frame before the input read.
 *   ET_INPUT   - read input. A confirm/B cancel pop. A selection change does the
 *                `apply_selection_common` work INLINE (target_shown=0, recreate the
 *                target window, targeting_render_work) -> ET_DISPLAY (the change's
 *                two render frames, matching the blocking yield C then yield A). A
 *                refresh-without-change (find returned nothing) re-runs the display
 *                work inline -> ET_PRIME (one render frame). An idle frame runs
 *                update_hppp_meter_work and stays in ET_INPUT.
 * Pops the 1-based target index, or 0 on cancel. */
typedef enum {
    ET_DISPLAY = 0, /* update_target_display work, then yield */
    ET_PRIME,       /* one update_hppp_meter_work frame before the input read */
    ET_INPUT,       /* read input: LEFT/RIGHT/UP/DOWN navigation, A confirm, B cancel */
} BattleEnemySelectPhase;

typedef struct {
    uint8_t  phase;         /* BattleEnemySelectPhase */
    uint8_t  allow_cancel;  /* select_battle_target() arg */
    uint8_t  pending_sfx;   /* sfx to play at the top of ET_DISPLAY after a change (0 = none) */
    uint16_t action_param;  /* select_battle_target() arg (targetability check) */
    uint16_t current_enemy; /* index within the current row */
    uint16_t current_row;   /* 0=front, 1=back */
    uint16_t x_pos;         /* current battler x position (recomputed each display) */
    uint16_t target_shown;  /* 0 until the target text has been displayed once */
} BattleEnemySelectState;

/* GAME_MODE_NAMING_EVENTS — run-to-completion port of init_naming_screen_events
 * (file_select.c): after a character is named, wait for the pending naming
 * actionscript, reassign the walk-out animation scripts, then wait for every
 * walk-out entity script to finish before clearing sprite VRAM.
 *
 * Two former blocking loops over render_frame_tick_naming():
 *   NE_WAIT_PENDING - check-before (FADE_WAIT pattern): render until
 *                     ert.wait_for_naming_screen_actionscript == 0. When it
 *                     clears, the script-reassign setup runs inline (no yield); an
 *                     early-out condition pops immediately (no cleanup), otherwise
 *                     it falls straight into the first NE_WAIT_SCRIPTS frame.
 *   NE_WAIT_SCRIPTS - the blocking loop computes the AND of script_table BEFORE
 *                     its render and breaks AFTER it, so it always renders once
 *                     more than strictly needed. `done` reproduces that: the step
 *                     computes the result, renders, and sets done when the result
 *                     is -1; the following step does the cleanup + pop with no
 *                     extra render (matching break -> cleanup -> return). */
typedef enum {
    NE_WAIT_PENDING = 0,
    NE_WAIT_SCRIPTS,
} NamingEventsPhase;

typedef struct {
    uint8_t  phase;        /* NamingEventsPhase */
    uint8_t  done;         /* NE_WAIT_SCRIPTS: set after the final render; next step pops */
    uint16_t naming_index; /* init_naming_screen_events() arg */
} NamingEventsState;

/* GAME_MODE_TEXT_INPUT — run-to-completion port of text_input_dialog()
 * (file_select.c), the on-screen keyboard used by the naming screens and the
 * Mother-2/EarthBound player-name registry. The blocking loop was a single
 * render -> wait_for_vblank -> read-input -> handle cycle; the step renders the
 * frame and lets the pump own the yield. `primed` reproduces the original's
 * "first iteration renders before any input is read" (no yield has happened yet),
 * exactly like overworld_step's post/render split: a primed step reads the input
 * latched by the previous frame's yield, acts on it, then renders.
 *
 * The output buffer was a uint8_t* parameter, which cannot live in a POD
 * ModeState. It is replaced by a NameTargetId (file_select.h) resolved to the
 * (stable global) buffer on confirm — the same serializable-by-ID pattern as the
 * char-select callbacks. The eb_name work buffer is hoisted here; any
 * existing-name pre-fill is done by the wrapper into the initial state, so the
 * existing_name pointer never enters ModeState. */
typedef struct {
    uint8_t  primed;                 /* 0 = first frame: render without reading input */
    uint8_t  name_target;            /* NameTargetId — resolved to a buffer on confirm */
    uint8_t  has_dont_care;          /* naming_index >= 0 */
    uint8_t  is_lowercase;           /* keyboard case toggle */
    int16_t  naming_index;           /* -1 (player names) or 0..6 (Don't Care group) */
    int16_t  dont_care_row;          /* -1 init, cycles 0..6 on each Don't Care press */
    uint16_t name_pos;               /* characters entered so far */
    uint16_t max_len;                /* name capacity */
    uint16_t name_display_window_id; /* window that shows the name being typed */
    int16_t  name_text_y;            /* text row of the name display */
    int16_t  cur_x;                  /* keyboard cursor column (window text coords) */
    int16_t  cur_y;                  /* keyboard cursor row */
    uint16_t frame_counter;          /* cursor-blink timer */
    uint8_t  eb_name[32];            /* the name being built (EB-encoded) */
} TextInputState;

/* GAME_MODE_NAMING_PROMPT — run-to-completion port of the name_a_character()
 * prompt-wait loop: render the name box (with its bullet+dashes display) and the
 * prompt message each frame until any button is pressed, then pop so the caller
 * proceeds to the keyboard (GAME_MODE_TEXT_INPUT). The one-shot setup (create
 * windows, print prompt, render the initial name tiles -> name_tile_cols) is done
 * by the wrapper before pumping; name_tile_cols is the only hoisted local.
 * `primed` reproduces the blocking loop's render-before-first-read order. */
typedef struct {
    uint8_t  primed;         /* 0 = first frame: render without reading input */
    int16_t  name_tile_cols; /* columns of the pre-rendered name display */
} NamingPromptState;

/* GAME_MODE_SCREEN_TRANSITION — run-to-completion port of the two frame loops in
 * screen_transition() (door.c, asm/overworld/screen_transition.asm), the door
 * fade-out (mode==1, "exit") and fade-in (mode==0, "enter") animations. The
 * one-shot setup (resolve config, init scroll velocity, the leading 2-frame
 * wait, init the swirl, prime the palette fade) stays in the blocking wrapper;
 * so does the trailing shared cleanup. Only the per-frame loops + the single
 * finalize frame live here.
 *
 * Both loops carry an OPTIONAL pre-yield: `if (palette_upload_mode) wait_for_
 * vblank();` flushes a pending palette DMA before the frame's palette animation
 * update — at most one extra yield per iteration. `pal_waited` tracks whether
 * that pre-yield has been taken this iteration (reset after the frame's main
 * yield), reproducing the conditional double-yield exactly.
 *
 * Exit path: ST_EXIT_BODY (loop) -> ST_EXIT_FINALIZE (fade-to-white or force-
 * blank frame) -> ST_EXIT_POST (post-yield wipe flag + enable entities, POP).
 * Enter path: ST_ENTER_BODY (loop) -> ST_ENTER_POST (post-yield frame==1 disable
 * + increment); when the loop ends, finalize_palette_fade() runs inline and the
 * mode POPs (no extra yield). The enter loop's post-yield disable_all_entities()
 * on frame 1 keeps its original placement (after the yield, before increment). */
typedef enum {
    ST_EXIT_BODY = 0,   /* exit fade-out loop iteration */
    ST_EXIT_FINALIZE,   /* fade-to-white / force-blank frame */
    ST_EXIT_POST,       /* post-finalize-yield: wipe flag + enable entities, POP */
    ST_ENTER_BODY,      /* enter fade-in loop iteration */
    ST_ENTER_POST,      /* post-yield: frame==1 disable, then increment */
} ScreenTransitionPhase;

typedef struct {
    uint8_t  phase;      /* ScreenTransitionPhase */
    uint8_t  pal_waited; /* the conditional palette pre-yield was taken this iteration */
    uint8_t  enter_mode; /* enter path: 0 = palette fade, 1 = brightness fade */
    uint8_t  fade_style; /* exit finalize: >= 49 fades to white, else force-blank */
    uint16_t frame;      /* current loop frame index */
    uint16_t duration;   /* eff_duration (exit) / secondary_duration (enter) */
} ScreenTransitionState;

/* GAME_MODE_PALETTE_FADE — run-to-completion port of the family of fixed-length
 * "run a palette fade for N frames" loops in overworld_palette.c. The one-shot
 * setup (load_palette_to_fade_buffer / prepare_palette_fade_slopes /
 * load_map_palette_animation_frame + initialize_map_palette_fade) stays in each
 * blocking wrapper; only the per-frame loop (and its finalize) live here. `kind`
 * selects the per-frame body, whether a button press skips it, and the finalize:
 *
 *   PF_SKIPPABLE_PAUSE - no body; pad1_pressed pops -1, else pop 0 when done.
 *                        (skippable_pause)
 *   PF_MAP_CHANGE      - body update_map_palette_fade(); pad1_pressed pops -1
 *                        (no copy-back); on normal completion copies the staged
 *                        palette back to ert.palettes groups 2-7, pops 0.
 *                        (animate_map_palette_change)
 *   PF_TO_WHITE        - body update_map_palette_animation(); not skippable; on
 *                        completion fills ert.palettes white + sets PALETTE_
 *                        UPLOAD_FULL, then takes ONE extra yield (phase 1) before
 *                        popping 0. (fade_palette_to_white)
 *   PF_WITH_RENDERING  - body update_map_palette_animation()+oam_clear()+run_
 *                        actionscript_frame()+update_screen(); not skippable; on
 *                        completion calls finalize_palette_fade() and pops 0 with
 *                        no extra yield. (animate_palette_fade_with_rendering)
 *
 * Each kind follows the blocking loop's ordering: test "done" (remaining==0)
 * first, then (skippable kinds) the pad, else run the body, decrement, and yield.
 * The body runs before the yield in every case — frame-identical to the originals. */
typedef enum {
    PF_SKIPPABLE_PAUSE = 0,
    PF_MAP_CHANGE,
    PF_TO_WHITE,
    PF_WITH_RENDERING,
} PaletteFadeKind;

typedef struct {
    uint8_t  kind;      /* PaletteFadeKind */
    uint8_t  phase;     /* PF_TO_WHITE: 1 = the post-white-fill final yield, then POP */
    uint16_t remaining; /* frames left to run */
} PaletteFadeState;

/* GAME_MODE_MAP_PALETTE_FADE — run-to-completion port of load_map_palette()'s
 * fade path (map_loader.c). The one-shot setup (parse target palette + compute
 * the per-channel 8.8 accumulators/slopes into ert.buffer scratch) stays in the
 * wrapper; only the per-frame UPDATE_MAP_PALETTE_FADE loop and the post-fade
 * finalize live here.
 *
 * The blocking loop yields BEFORE each accumulate (wait_for_vblank() at the top
 * of the body), so `primed` makes the first step the leading wait with no
 * accumulate. Each subsequent step accumulates one frame (the inner 96-color
 * loop) + sets PALETTE_UPLOAD_BG_ONLY. When the last frame is accumulated, the
 * finalize (slam final BG palette, reload/adjust sprite palettes, PALETTE_UPLOAD
 * _FULL) runs inline and `done` is set; the following step takes the trailing
 * wait_for_vblank() and pops. Net yields = fade_frames + 1, matching the
 * original. fade_frames == 0 is handled by the wrapper's instant path (never
 * enters this mode). */
typedef struct {
    uint8_t  primed;    /* 0 = first frame is the leading wait (no accumulate yet) */
    uint8_t  done;      /* set after the finalize; the next step pops */
    uint16_t remaining; /* accumulate frames left */
} MapPaletteFadeState;

/* GAME_MODE_MOSAIC_FADE — run-to-completion port of the brightness-ramp mosaic
 * fades: FADE_OUT_WITH_MOSAIC (callroutine.c) and flyover.c's fade_in/out. The
 * INIDISP brightness nibble is ramped by `step` each brightness step, optionally
 * driving the MOSAIC register (size inversely proportional to brightness) when
 * `mosaic_bgs != 0`; each brightness step is followed by `delay` yields.
 *
 *   MF_IN  - ramp brightness up. Each step: next = b + step; if next >= 0x0F set
 *            INIDISP brightness to 0x0F and POP (no trailing delay). The wrapper
 *            primes INIDISP=0x00 / MOSAIC=0 before pumping.
 *   MF_OUT - ramp brightness down. Each step clears MOSAIC, breaks if INIDISP is
 *            already force-blank or next = b - step < 0; on break sets INIDISP =
 *            0x80 (force blank). When `final_hdma` is set (the callroutine
 *            variant) it also clears window_hdma_active and takes ONE extra yield
 *            (phase 1) before popping.
 *
 * delay == 0 means the whole ramp completes in a single step with no yields
 * (the original's inner for-loop ran zero times) — the step's internal loop
 * advances brightness steps back-to-back until a yield (delay > 0) or completion. */
typedef enum { MF_IN = 0, MF_OUT } MosaicFadeKind;

typedef struct {
    uint8_t  kind;        /* MosaicFadeKind */
    uint8_t  phase;       /* MF_OUT + final_hdma: 1 = the extra trailing yield, then POP */
    uint8_t  step;        /* brightness change per step */
    uint8_t  mosaic_bgs;  /* mosaic enable mask (low nibble); 0 = no mosaic */
    uint8_t  final_hdma;  /* MF_OUT: also clear window_hdma_active + 1 extra yield */
    uint16_t delay;       /* yields between brightness steps */
    uint16_t delay_left;  /* remaining delay yields before the next brightness step */
} MosaicFadeState;

/* GAME_MODE_FLYOVER — run-to-completion port of the two flyover/cutscene
 * bytecode interpreters in flyover.c: play_flyover_script() (FO_SCRIPT, the map
 * intro "fly over" text) and coffeetea_scene() (FO_COFFEETEA, the coffee/tea
 * break). Both walk a script of EB-character/control opcodes, then fade in,
 * display, and fade back out with the usual force-blank/undraw cleanup.
 *
 * The flyover module's render state (flyover_screen_offset, flyover_vwf_x/y, …)
 * stays in file-static .bss (set by the wrappers / the static helpers) — it is
 * run-to-completion-safe; serializing it is deferred (same policy as town_map's
 * anim counters). Only the former C-stack locals are hoisted here. The script
 * pointer is re-derived from `id` each step (FO_SCRIPT: flyover_script_ids[id];
 * FO_COFFEETEA: ASSET_COFFEE/TEA_BIN by `id` = type), so no pointer is stored.
 *
 * The flyover brightness ramps have no mosaic, so they are inlined (not pushed as
 * GAME_MODE_MOSAIC_FADE — STEP_PUSH cannot yet carry init state). `sub` drives
 * the multi-yield opcode 0x09 (FO_SCRIPT: upload→wait→scroll; FO_COFFEETEA: the
 * smooth-scroll inner loop). load_background_animation()'s body is replicated in
 * the CT setup phases because the public blocking version is still used by
 * ending.c / callroutine.c. Always pops 0. */
typedef enum { FO_SCRIPT = 0, FO_COFFEETEA } FlyoverKind;

typedef enum {
    /* FO_SCRIPT (play_flyover_script) */
    FOP_S_PARSE = 0, /* walk opcodes; 0x09 => upload+wait+scroll (sub 1/2) */
    FOP_S_FADEIN,    /* brightness ramp up (step 1, delay 3, no mosaic) */
    FOP_S_DISPLAY,   /* hold the text for 180 frames */
    FOP_S_FADEOUT,   /* brightness ramp down (step 1, delay 3) */
    FOP_S_CLEAN1,    /* tm/bg2/word-wrap + force-blank frame */
    FOP_S_CLEAN2,    /* undraw + restore entity 23 + blank-screen frame */
    FOP_S_DONE,      /* POP */
    /* FO_COFFEETEA (coffeetea_scene) */
    FOP_CT_FADEOUT1, /* initial brightness ramp down (step 1, delay 1) */
    FOP_CT_SETUP_A,  /* init screen + oam_clear + force-blank frame */
    FOP_CT_SETUP_B,  /* BG mode/locations + load_battle_bg + blank-screen frame */
    FOP_CT_SETUP_C,  /* fade_in + screen offset + script null-check */
    FOP_CT_PARSE,    /* walk opcodes; 0x09 => smooth-scroll inner loop (sub 1) */
    FOP_CT_FADEWAIT, /* fade_out then wait while updating battle effects */
    FOP_CT_CLEAN1,   /* force-blank frame */
    FOP_CT_CLEAN2,   /* reload_map + bg2 + word-wrap + force-blank frame */
    FOP_CT_CLEAN3,   /* undraw + blank-screen frame */
    FOP_CT_DONE,     /* fade_in + POP */
} FlyoverPhase;

typedef struct {
    uint8_t  kind;                /* FlyoverKind */
    uint8_t  phase;               /* FlyoverPhase */
    uint8_t  sub;                 /* opcode 0x09 sub-state */
    uint8_t  fade_primed;         /* FOP_CT_FADEWAIT: work-after-yield flag */
    uint16_t id;                  /* FO_SCRIPT: flyover id 0-7; FO_COFFEETEA: type 0/1 */
    uint16_t ramp_delay_left;     /* yields left before the next brightness step */
    uint16_t display_left;        /* FO_SCRIPT 180-frame display countdown */
    uint16_t saved_ent23_tick_hi; /* FO_SCRIPT: restored at cleanup */
    uint16_t scroll_accum;        /* FO_COFFEETEA smooth-scroll accumulator */
    uint32_t pos;                 /* script parse position */
    uint32_t script_size;         /* script byte length */
} FlyoverState;

/* GAME_MODE_INTRO_LOGO — run-to-completion port of logo_screen() (logo_screen.c):
 * the Nintendo -> APE -> HAL logo sequence shown at boot. Each logo is loaded
 * (no yield), faded in, held, and faded out. The brightness ramps are exactly
 * GAME_MODE_MOSAIC_FADE (MF_IN / MF_OUT with no mosaic), so this mode PUSHes a
 * MOSAIC_FADE child for each fade via STEP_PUSH-with-init rather than re-inlining
 * the ramp — the first real use of that mechanism.
 *
 *   LG_LOAD  - load logo[idx] gfx, prime INIDISP=0x00 / MOSAIC=0, set the hold
 *              length, then PUSH MF_IN; resume at LG_HOLD.
 *   LG_HOLD  - hold loop. Nintendo (idx 0) is fixed at 180 frames and NOT
 *              skippable; APE/HAL (idx 1/2) hold up to 120 frames and skip on any
 *              button press (post-yield read). A skip PUSHes a faster MF_OUT
 *              (step 2, delay 1) and pops 1; a normal time-out PUSHes MF_OUT
 *              (step 1, delay 2). Either way resume at LG_DONE_FADE.
 *   LG_DONE_FADE - after the fade-out: a skip pops 1; otherwise advance to the
 *                  next logo (LG_LOAD) or pop 0 after HAL.
 *
 * Pops 0 on normal completion, 1 if a button skipped APE/HAL (matching the
 * blocking logo_screen() return). The few extra force-blank/brightness-0 frames
 * the pump inserts at each PUSH/POP boundary are imperceptible on this cosmetic
 * sequence (same class of accepted <=1-frame shift as the other conversions). */
typedef enum {
    LG_LOAD = 0,
    LG_HOLD,
    LG_DONE_FADE,
} IntroLogoPhase;

typedef struct {
    uint8_t  phase;          /* IntroLogoPhase */
    uint8_t  logo_idx;       /* 0 = Nintendo, 1 = APE, 2 = HAL */
    uint8_t  skipping;       /* a button skip is in progress: pop 1 after the fade-out */
    uint16_t hold_remaining; /* frames left to hold the current logo */
} IntroLogoState;

/* GAME_MODE_GAS_STATION — run-to-completion port of gas_station() /
 * RUN_GAS_STATION_CREDITS (gas_station.c), the "red Giygas static" prologue. The
 * one-shot setup (entity_system_init + gas_station_load, both yield-free) stays
 * in the blocking wrapper; the six former blocking loops become phases sharing
 * one `remaining` countdown:
 *
 *   GS_PH1 - 236-frame static intro with the NMI brightness fade-in
 *            (fade_delay_left / brightness_fading drive the $80->$0F ramp).
 *   GS_PH2 - 480-frame palette interpolation (gas station fades in, battle BG
 *            fades out); on completion FINALIZE_PALETTE_FADE + disable color math.
 *   GS_PH3 - 120-frame hold at full brightness; on completion CHANGE_MUSIC +
 *            entity_init_wipe(EVENT_860) (the flash sequence).
 *   GS_PH4 - run EVENT_860 until its script clears; a button skip deactivates
 *            the entity first; on completion sets up the fade-to-white.
 *   GS_PH5 - 330-frame fade to white; on completion clears the screen/palettes.
 *   GS_PH6 - 30-frame final wait (NOT button-skippable), then pop.
 *
 * Every phase except GS_PH6 skips to pop-1 on any button (post-yield read),
 * matching the blocking WAIT_FRAMES_OR_UNTIL_PRESSED / pad checks. Pops 0 on a
 * full run, 1 on a button skip. Each phase does its frame's work then decrements
 * `remaining`, performing the (yield-free) transition to the next phase on the
 * frame that reaches 0 — so the frame counts match the originals. */
typedef enum {
    GS_PH1 = 0,
    GS_PH2,
    GS_PH3,
    GS_PH4,
    GS_PH5,
    GS_PH6,
} GasStationPhase;

typedef struct {
    uint8_t  phase;             /* GasStationPhase */
    uint8_t  brightness_fading; /* GS_PH1: NMI brightness fade still ramping */
    int16_t  fade_delay_left;   /* GS_PH1: frames until the next brightness step */
    int16_t  entity_offset;     /* GS_PH4: the EVENT_860 flash entity */
    uint16_t remaining;         /* frames left in the current countdown phase */
} GasStationState;

/* GAME_MODE_TITLE_SCREEN — run-to-completion port of show_title_screen()
 * (title_screen.c). The one-shot setup (force-blank, entity init, BG/OAM/graphics
 * load, entity_init_wipe(TITLE_SCREEN_1), and the quick/non-quick pre-loop setup
 * — sprite-palette decomp + fade-target/slopes, or fade_in(4,1)) all stay in the
 * blocking wrapper. The three former blocking loops become phases:
 *
 *   TS_WARMUP  - 60-frame warm-up. quick_mode selects the body: quick =
 *                fade_update() + render_frame_tick_work(); non-quick = the
 *                sprite-palette lerp (group 8) + update_map_palette_animation() +
 *                render_frame_tick_work(). `frame` counts to 60, then -> TS_INPUT.
 *   TS_INPUT   - the @CHECK_ACTIONSCRIPT / @INPUT_LOOP goto machine as one
 *                self-looping phase: each step checks actionscript_state
 *                (1 -> exit to attract, result 0), then any button (-> exit,
 *                result 1), else render_frame_tick_work(). Input/state are read
 *                at the top (post-yield), matching the original's button-then-
 *                render-then-recheck order.
 *   TS_FADEOUT - the manual brightness ramp-down (0x0F..1, four frames each, then
 *                force-blank) + the exit cleanup (restore viewport/sprite offset,
 *                clear actionscript_state, setup_entity_color_math, entity reset).
 *                Inlined rather than pushed as MOSAIC_FADE: the existing C loop
 *                displays 0x0F first and ends at 1, one brightness level off from
 *                FADE_OUT_WITH_MOSAIC; inlining keeps this refactor behavior-exact.
 *
 * Pops 0 on idle time-out (-> attract mode), 1 on a button press (-> file select),
 * matching the blocking return. */
typedef enum {
    TS_WARMUP = 0,
    TS_INPUT,
    TS_FADEOUT,
} TitleScreenPhase;

typedef struct {
    uint8_t  phase;           /* TitleScreenPhase */
    uint8_t  quick_mode;      /* selects the TS_WARMUP body */
    uint8_t  result;          /* 0 = time-out (attract), 1 = button pressed */
    uint8_t  fade_b;          /* TS_FADEOUT: current brightness (0x0F..1) */
    uint8_t  fade_delay_left; /* TS_FADEOUT: frames left at the current brightness */
    uint16_t frame;           /* TS_WARMUP: warm-up frame counter */
} TitleScreenState;

/* GAME_MODE_ATTRACT — run-to-completion port of the three blocking loops at the
 * tail of run_attract_mode() (attract_mode.c), an idle title-screen demo scene.
 * The one-shot setup AND the blocking display_text_from_addr() that drives the
 * scene script (which pumps its own converted text waits internally) stay in the
 * blocking wrapper; only the post-script loops live here:
 *
 *   AT_MAIN       - while(actionscript_state == 0): update_swirl_effect(), then a
 *                   button check (any button -> result 1), then render_frame_tick_
 *                   work() + fade_update() + the frame<=1 TM override + the 36000-
 *                   frame safety timeout. On any exit, close_oval_window() ->
 *                   AT_OVAL_CLOSE.
 *   AT_OVAL_CLOSE - while(is_psi_animation_active()): render_frame_tick_work() +
 *                   update_swirl_effect(). On completion fade_out(1,1) ->
 *                   AT_FADEOUT.
 *   AT_FADEOUT    - while(fade_active()): fade_update() + render_frame_tick_work().
 *                   On completion stop_oval_window() + clear_map_entities(), pop.
 *
 * Pops the button-pressed flag (1 if a button ended the scene, else 0), matching
 * the blocking return. The swirl update in AT_OVAL_CLOSE runs one render-frame
 * earlier than the blocking loop (which yielded before it) — an accepted
 * imperceptible shift on this brief cosmetic close animation. */
typedef enum {
    AT_MAIN = 0,
    AT_OVAL_CLOSE,
    AT_FADEOUT,
} AttractPhase;

typedef struct {
    uint8_t  phase;          /* AttractPhase */
    uint8_t  button_pressed; /* result: a button ended the scene */
    uint16_t loop_frame;     /* AT_MAIN: frame counter (TM override + timeout) */
} AttractState;

/* GAME_MODE_FILE_MENU — run-to-completion port of file_menu_loop() (file_select.c),
 * the file-select cascade reached from the intro. Each former blocking sub-menu
 * (file_select_menu / show_file_select_submenu / text_speed / sound_mode / flavour
 * / delete-confirm) was a thin wrapper around selection_menu(); the cascade now
 * builds each menu's window synchronously, STEP_PUSHes GAME_MODE_SELECTION_MENU,
 * and reads the choice back via mode_child_result() in the matching *_RESULT phase.
 *
 * Two things deliberately stay blocking, called inline from the step (the C-stack
 * during them is acceptable — they are terminal, input-driven, and depend on
 * subsystems not yet converted): new_game_naming() (its own multi-character driver
 * over the already-converted naming modes) and the synchronous load/save/erase
 * helpers (no yield). The leading fade-in wait is phase FM_FADEIN_WAIT.
 *
 * Pops 1 when a game is started/loaded (overworld follows), 0 on quit. The result
 * is ignored by the init_intro parent (it runs the same post-file-menu cleanup
 * either way), matching the blocking original. */
typedef enum {
    FM_FADEIN_WAIT = 0, /* while(fade_active()): battle_bg_update + fade_update */
    FM_SELECT,          /* battle_bg_update; build slot list; push selection_menu(0) */
    FM_SELECT_RESULT,   /* branch on chosen slot: submenu (existing) or new-game cascade */
    FM_SUBMENU,         /* build Continue/Copy/Delete/SetUp; push selection_menu(1) */
    FM_SUBMENU_RESULT,  /* dispatch the submenu action */
    FM_DELETE_RESULT,   /* after the delete-confirm selection_menu(1) */
    FM_SETUP_TS,        /* existing-save Set Up: text-speed menu */
    FM_SETUP_TS_RESULT,
    FM_SETUP_SND,       /* existing-save Set Up: sound-mode menu */
    FM_SETUP_SND_RESULT,
    FM_SETUP_FLV,       /* existing-save Set Up: flavour menu */
    FM_SETUP_FLV_RESULT,
    FM_NG_TS,           /* new-game: text-speed menu */
    FM_NG_TS_RESULT,
    FM_NG_SND,          /* new-game: sound-mode menu */
    FM_NG_SND_RESULT,
    FM_NG_FLV,          /* new-game: flavour menu */
    FM_NG_FLV_RESULT,
    FM_NG_NAMING,       /* new-game: run naming (blocking) + finalize, or back to flavour */
} FileMenuPhase;

typedef struct {
    uint8_t  phase;         /* FileMenuPhase */
    uint8_t  result_ready;  /* 1 = `result` holds an inline early-exit value (no child was pushed) */
    uint16_t selected;      /* chosen slot, 1-based (file_select_menu result) */
    uint16_t result;        /* inline early-exit result for the *_RESULT phase */
} FileMenuState;

/* GAME_MODE_INIT_INTRO — run-to-completion port of init_intro()'s state machine
 * (init_intro.c). STEP_PUSHes the converted intro leaves (INTRO_LOGO, GAS_STATION,
 * TITLE_SCREEN) and FILE_MENU as children, branching on mode_child_result(). The
 * yield-free transitions (change_music, fade_out_if_visible, the PPU cleanup, the
 * post-file-menu cleanup) run inline at the phase boundaries. Attract scenes still
 * run via the blocking run_attract_mode() wrapper, called inline: its scene is
 * driven by display_text_from_addr() (the text interpreter), which is not yet a
 * mode. The one-shot init_intro() setup stays in the blocking wrapper. */
typedef enum {
    II_LOGO = 0,        /* push INTRO_LOGO */
    II_LOGO_RESULT,
    II_GAS,             /* change_music(GAS_STATION); push GAS_STATION */
    II_GAS_RESULT,
    II_TITLE,           /* change_music(TITLE_SCREEN); title setup; push TITLE_SCREEN */
    II_TITLE_RESULT,
    II_ATTRACT,         /* run the attract scene table (blocking run_attract_mode) */
    II_FILE_MENU,       /* exit cleanup; change_music(SETUP); push FILE_MENU */
    II_FILE_MENU_DONE,  /* post-file-menu cleanup (window_tick_work) then pop */
} InitIntroPhase;

typedef struct {
    uint8_t  phase;            /* InitIntroPhase */
    uint8_t  title_quick_mode; /* skip-to-title quick mode for the next title-screen push */
    uint8_t  attract_index;    /* which attract scene table entry is next */
} InitIntroState;

/* GAME_MODE_DISPLAY_TEXT — run-to-completion port of the text bytecode
 * interpreter display_text() (display_text.c, asm/text/display_text.asm). The
 * blocking while-loop body is the DT_RUN phase, run inside an internal for(;;)
 * that processes control codes back-to-back and only returns (yields) at a real
 * frame boundary:
 *   - the per-character typewriter delay (window_tick x text_speed+1) becomes the
 *     DT_DELAY phase (one window_tick_work() per step);
 *   - CC_08 CALL_TEXT recursion becomes a STEP_PUSH of a nested DISPLAY_TEXT
 *     child (the parent resumes DT_RUN on POP).
 * DT_ENTER does the per-call prologue (save the parent's g_cc18_attrs_saved, zero
 * the global, reset upcoming_word_length) then falls through to DT_RUN with no
 * yield, matching the blocking display_text() entry. The saved value is restored
 * on END_BLOCK / end-of-stream / quit before the POP, so per-call attribute state
 * is naturally per-mode.
 *
 * Staged landing (plan Phase A, strategy b): only the typewriter delay and CALL
 * recursion are run-to-completion here. The remaining yielding control codes
 * (cc_halt/cc_pause/CC_11 selection_menu/the cc_1f sub-ops) still call their
 * blocking forms inline, which internally pump_mode their already-converted
 * children — C-stack state for now, converted to STEP_PUSH in later commits. */
typedef enum {
    DT_ENTER = 0, /* per-call prologue, then fall through to DT_RUN (no yield) */
    DT_RUN,       /* interpret control codes until a yield point */
    DT_DELAY,     /* typewriter per-character delay countdown */
} DisplayTextPhase;

/* Post-child work a DISPLAY_TEXT level owes when a STEP_PUSHed child pops back to
 * it. A CC that pushes a child and then needs the child's result records this; the
 * top of DT_RUN handles it (reading mode_child_result()) before reading the next
 * byte. CCs with no post-work leave it DT_RESUME_NONE. */
typedef enum {
    DT_RESUME_NONE = 0,
    DT_RESUME_CC11,             /* CC_11 selection_menu: store result to working_memory */
    DT_RESUME_CC1F_NUMSEL,      /* CC_1F_52 number-select: store entered value / cancel */
    DT_RESUME_CC1A_PARTY_SEL,   /* CC_1A_00/01 overworld party select: cleanup + store result */
    DT_RESUME_CC1A_BATTLE_SEL,  /* CC_1A_00/01 battle party select: store CHAR_SELECT result */
} DisplayTextResume;

typedef struct {
    uint8_t      phase;            /* DisplayTextPhase */
    uint8_t      saved_cc18_attrs; /* this call level's saved g_cc18_attrs_saved */
    uint8_t      resume;           /* DisplayTextResume: post-child work pending on POP */
    uint16_t     delay_remaining;  /* DT_DELAY: window_tick_work frames left */
    uint16_t     cc1a_window_id;   /* DT_RESUME_CC1A_PARTY_SEL: window to close on POP */
    uint32_t     cc1a_saved_argmem;/* DT_RESUME_CC1A_PARTY_SEL: argument_memory to restore */
    ScriptReader reader;           /* offset-based script cursor (serializable) */
} DisplayTextModeState;  /* note: DisplayTextState (display_text.h) is the `dt` global type */

/* Per-mode hoisted locals (former stack variables). MUST be plain-old-data: no
 * pointers into the stack or heap that would not survive a save/reload. Sized
 * with headroom so adding a future mode's locals does not change the on-disk
 * ModeStack layout for already-shipped modes. (The reserve grew from 64 to 160
 * for SoundStoneState's ps[8]; savestates are not cross-build compatible, so the
 * one-time on-disk size change is harmless pre-cutover.) */
union ModeState {
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
    BattleRowSelectState  battle_row_select;
    BattleEnemySelectState battle_enemy_select;
    NamingEventsState     naming_events;
    TextInputState        text_input;
    NamingPromptState     naming_prompt;
    ScreenTransitionState screen_transition;
    PaletteFadeState      palette_fade;
    MapPaletteFadeState   map_palette_fade;
    MosaicFadeState       mosaic_fade;
    FlyoverState          flyover;
    IntroLogoState        intro_logo;
    GasStationState       gas_station;
    TitleScreenState      title_screen;
    AttractState          attract;
    FileMenuState         file_menu;
    InitIntroState        init_intro;
    DisplayTextModeState  display_text;
    TextWaitFadeState     text_wait_fade;
    ProcessInteractionState process_interaction;
    uint8_t               _raw[160];
};

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

/* GAME_MODE_BATTLE_ROW_SELECT / GAME_MODE_BATTLE_ENEMY_SELECT steps (defined in
 * battle_targeting.c, where the targeting/flashing helpers live). Init the
 * matching ModeState union member (phase = BR_RENDER / ET_DISPLAY) before
 * pump_mode(). Row-select pops row+1 (or 0 on cancel); enemy-select pops the
 * 1-based target index (or 0 on cancel). */
StepResult mode_step_battle_row_select(ModeState *st);
StepResult mode_step_battle_enemy_select(ModeState *st);

/* GAME_MODE_NAMING_EVENTS step (defined in file_select.c, where the naming-
 * entity tables and render_frame_tick_naming_work() live). Init via
 * ModeState.naming_events (phase = NE_WAIT_PENDING, naming_index) before
 * pump_mode(GAME_MODE_NAMING_EVENTS). Always pops 0. */
StepResult mode_step_naming_events(ModeState *st);

/* GAME_MODE_TEXT_INPUT step (defined in file_select.c). Init via
 * ModeState.text_input before pump_mode(GAME_MODE_TEXT_INPUT). Pops 0 on confirm
 * (name written to the resolved target buffer), -1 on cancel. */
StepResult mode_step_text_input(ModeState *st);

/* GAME_MODE_NAMING_PROMPT step (defined in file_select.c). Init via
 * ModeState.naming_prompt (name_tile_cols) before pump_mode(GAME_MODE_NAMING_
 * PROMPT). Always pops 0 (a button was pressed). */
StepResult mode_step_naming_prompt(ModeState *st);

/* GAME_MODE_SCREEN_TRANSITION step (defined in door.c, where the transition
 * helpers and the ert/dr/ppu state it touches are visible). Init via
 * ModeState.screen_transition (phase = ST_EXIT_BODY or ST_ENTER_BODY) before
 * pump_mode(GAME_MODE_SCREEN_TRANSITION). Always pops 0. */
StepResult mode_step_screen_transition(ModeState *st);

/* GAME_MODE_PALETTE_FADE step (defined in overworld_palette.c). Init via
 * ModeState.palette_fade (kind, remaining) before pump_mode(GAME_MODE_PALETTE_
 * FADE). Pops 0 normally; the skippable kinds pop -1 if a button was pressed. */
StepResult mode_step_palette_fade(ModeState *st);

/* GAME_MODE_MAP_PALETTE_FADE step (defined in map_loader.c, where the sprite-
 * palette helpers and BUF_FLASH_* scratch layout are visible). Init via
 * ModeState.map_palette_fade (remaining = fade_frames) before pump_mode. Pops 0. */
StepResult mode_step_map_palette_fade(ModeState *st);

/* GAME_MODE_MOSAIC_FADE step (defined in callroutine.c). Init via
 * ModeState.mosaic_fade (kind, step, delay, mosaic_bgs, final_hdma) before
 * pump_mode(GAME_MODE_MOSAIC_FADE). Always pops 0. */
StepResult mode_step_mosaic_fade(ModeState *st);

/* GAME_MODE_FLYOVER step (defined in flyover.c, where the flyover render helpers
 * and module statics live). Init via ModeState.flyover (kind, phase = FOP_S_PARSE
 * or FOP_CT_FADEOUT1, id, pos, script_size, …) before pump_mode(GAME_MODE_
 * FLYOVER). Always pops 0. */
StepResult mode_step_flyover(ModeState *st);

/* GAME_MODE_INTRO_LOGO step (defined in logo_screen.c). Init via
 * ModeState.intro_logo (phase = LG_LOAD, logo_idx = 0) before
 * pump_mode(GAME_MODE_INTRO_LOGO). Pops 0 normally, 1 on a button skip. */
StepResult mode_step_intro_logo(ModeState *st);

/* GAME_MODE_GAS_STATION step (defined in gas_station.c). Init via
 * ModeState.gas_station (phase = GS_PH1, fade_delay_left = 11,
 * brightness_fading = 1, remaining = 236) before pump_mode(GAME_MODE_GAS_STATION).
 * Pops 0 on a full run, 1 on a button skip. */
StepResult mode_step_gas_station(ModeState *st);

/* GAME_MODE_TITLE_SCREEN step (defined in title_screen.c). Init via
 * ModeState.title_screen (phase = TS_WARMUP, quick_mode) before
 * pump_mode(GAME_MODE_TITLE_SCREEN). Pops 0 on time-out (attract mode), 1 on a
 * button press (file select). */
StepResult mode_step_title_screen(ModeState *st);

/* GAME_MODE_ATTRACT step (defined in attract_mode.c). Init via ModeState.attract
 * (phase = AT_MAIN) before pump_mode(GAME_MODE_ATTRACT) — the wrapper runs the
 * one-shot setup + the blocking scene script first. Pops the button-pressed
 * flag (1 if a button ended the scene, else 0). */
StepResult mode_step_attract_mode(ModeState *st);

/* GAME_MODE_FILE_MENU step (defined in file_select.c). Init via
 * ModeState.file_menu (phase = FM_FADEIN_WAIT) before pump_mode(GAME_MODE_FILE_
 * MENU). Pops 1 when a game starts/loads, 0 on quit. */
StepResult mode_step_file_menu(ModeState *st);

/* GAME_MODE_INIT_INTRO step (defined in init_intro.c). Init via
 * ModeState.init_intro (phase = II_LOGO) before pump_mode(GAME_MODE_INIT_INTRO).
 * Pops 0. */
StepResult mode_step_init_intro(ModeState *st);

/* GAME_MODE_DISPLAY_TEXT step (defined in display_text.c). Normally entered via
 * the display_text() wrapper (pump_mode); CC_08 CALL_TEXT STEP_PUSHes a nested
 * instance. Init with phase = DT_ENTER and the reader fields set. Always pops 0. */
StepResult mode_step_display_text(ModeState *st);

/* GAME_MODE_TEXT_WAIT_FADE step (defined in overworld_interaction.c). Init with
 * ModeState.text_wait_fade (phase = TWF_TEXT, text_addr) before
 * pump_mode(GAME_MODE_TEXT_WAIT_FADE). Drives the overworld text-interaction
 * primitive: disable entities, push DISPLAY_TEXT, wait for the entity fade-out,
 * re-enable entities. Always pops 0. GAME_MODE_ENTITY_FADE_WAIT (the wait child)
 * is defined in mode_stack.c and takes no init. */
StepResult mode_step_text_wait_fade(ModeState *st);

/* GAME_MODE_PROCESS_INTERACTION step (defined in overworld_interaction.c). Init
 * with ModeState.process_interaction (phase = PI_DISPATCH) before
 * pump_mode(GAME_MODE_PROCESS_INTERACTION). Always pops 0. */
StepResult mode_step_process_interaction(ModeState *st);

/* Push `mode` onto the stack. If `init` is non-NULL its contents become the new
 * level's ModeState; otherwise the state is zeroed. */
void mode_push(GameMode mode, const ModeState *init);

/* Pop the top mode, recording `result` into the parent's child_result slot.
 * Returns `result`. */
int32_t mode_pop(int32_t result);

/* Read the result the most-recently-popped child handed back to the current
 * (now-top) mode. A parent mode that STEP_PUSHes a child reads this on its next
 * step to branch on the child's pop value. Returns child_result[depth-1]. */
int32_t mode_child_result(void);

/* Migration bridge: push `mode` (with optional initial state) and run it — and
 * any children it pushes — to completion using a LOCAL host_process_frame()
 * yield, then return its pop_result. Lets a still-blocking parent invoke an
 * already-converted child. Deleted at cutover, when the root loop is the only
 * pump. Returns 0 early if the user requested quit. */
int32_t pump_mode(GameMode mode, const ModeState *init);

#endif /* EB_CORE_MODE_STACK_H */
