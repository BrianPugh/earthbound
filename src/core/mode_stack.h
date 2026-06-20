#ifndef EB_CORE_MODE_STACK_H
#define EB_CORE_MODE_STACK_H

#include "core/types.h"
#include "game/display_text.h"  /* ScriptReader (embedded in DisplayTextState) */
#include "game/overworld.h"     /* OwDamageState (embedded in OverworldState) */

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
    GAME_MODE_DOOR_TRANSITION,     /* door/teleport transition driver (door_transition) */
    GAME_MODE_QUICK_CHECKTALK,     /* L-button quick talk/check (open_menu_button_checktalk) */
    GAME_MODE_PAUSE_MENU,          /* overworld pause menu (open_menu_button) */
    GAME_MODE_EQUIP_MENU,          /* pause-menu Equip cascade (open_equipment_menu) */
    GAME_MODE_STATUS_MENU,         /* pause-menu Status cascade (open_status_menu) */
    GAME_MODE_HPPP_DISPLAY,        /* B-button HP/PP + money display (open_hppp_display) */
    GAME_MODE_PSI_MENU,            /* pause-menu PSI cascade (overworld_psi_menu) */
    GAME_MODE_USE_ITEM,            /* pause-menu Goods→Use driver (overworld_use_item) */
    GAME_MODE_TELEPORT_MENU,       /* PSI Teleport destination menu (open_teleport_destination_menu) */
    GAME_MODE_DETERMINE_TARGETING, /* battle-action targeting dispatch (determine_targetting) */
    GAME_MODE_LEVEL_UP,            /* inventory level-up sequence (gain_exp / level_up_char) */
    GAME_MODE_BATTLE_PSI_MENU,     /* in-battle PSI selection cascade (battle_psi_menu) */
    GAME_MODE_BATTLE_MENU,         /* per-character battle command menu (battle_selection_menu) */
    GAME_MODE_BATTLE,              /* main battle loop (battle_routine) */
    GAME_MODE_INSTANT_WIN,         /* auto-victory sequence (instant_win_handler) */
    GAME_MODE_BATTLE_ENTRY,        /* overworld encounter entry/exit (init_battle_overworld) */
    GAME_MODE_BATTLE_SCRIPTED,     /* scripted/event battle entry/exit (init_battle_scripted) */
    GAME_MODE_BATTLE_ACTION,       /* one battle-action function (btlact_* long tail) */
    GAME_MODE_BATTLE_CALC,         /* battle_calc.c text pipeline (miss/smaaaash/damage/shields) */
    GAME_MODE_BATTLE_REVIVE,       /* revive a KO'd battler (battle_revive_target) */
    GAME_MODE_BATTLE_APPLY,        /* per-target action apply loop (apply_action_to_targets) */
    GAME_MODE_BATTLE_KO,           /* battler death driver (battle_ko_target) */
    GAME_MODE_CHECK_DEAD_PLAYERS,  /* sync party HP/PP + "ally collapsed" KO text (check_dead_players) */
    GAME_MODE_ACTIONSCRIPT_FRAME,  /* finish an interrupted run_actionscript_frame() */
    GAME_MODE_PP_RECOVERY_FLASH,   /* instant-win PP recovery purple flashes (event script) */
    GAME_MODE_TELEPORT,            /* PSI teleport driver (teleport_mainloop) */
    GAME_MODE_BICYCLE_DISMOUNT,    /* "got off the bicycle" message + dismount */
    GAME_MODE_HP_ALERT,            /* "HP is very low!" overworld warning (show_hp_alert) */
    GAME_MODE_GAME_OVER,           /* game-over / comeback sequence (spawn + play_comeback_sequence) */
    GAME_MODE_OVERWORLD,           /* overworld root mode (overworld_post + overworld_step) */
    GAME_MODE_DEBUG_GOODS,         /* debug Y-button Goods item browser/giver (debug_y_button_goods) */
    GAME_MODE_DEBUG_MENU,          /* debug Y-button parent menu (debug_y_button_menu) */
    GAME_MODE_TELEPORT_TO,         /* script/debug instant teleport sequence (CC_1F_21 / debug CAST/STAFF) */
    GAME_MODE_NEW_GAME_NAMING,     /* new-game naming flow (new_game_naming) */
    GAME_MODE_SPECIAL_EVENT,       /* CC_1F_41 special-event dispatch (dispatch_special_event) */
    GAME_MODE_ENTER_NAME,          /* M2/EB player-name registry prompt (enter_your_name_please) */
    GAME_MODE_LOAD_BATTLE_SCENE,   /* boss-transition scene (re)load (load_battle_scene) */
    GAME_MODE_STORE_MENU,          /* shop item-purchase menu (open_store_menu) */
    GAME_MODE_ESCARGO_MENU,        /* Escargo Express stored-goods menu (select_escargo_express_item) */
    GAME_MODE_TELEPHONE_MENU,      /* phone directory menu + contact text (open_telephone_menu) */
    GAME_MODE_WAIT_FRAMES,         /* render N frames (mode form of wait_frames_with_updates) */
    GAME_MODE_ENDING,              /* end-of-game cast scene + staff credits (play_cast_scene/play_credits) */
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
    FADE_TICK_SCREEN_ONLY,          /* oam_clear; update_screen; fade_update — port of wait_for_fade_complete() */
} FadeTickKind;

typedef struct {
    uint8_t tick_kind;   /* FadeTickKind */
    uint8_t asf_flush;   /* FADE_TICK_OVERWORLD_RENDER: 1 = resume after a parked
                          * actionscript frame's child popped (do the post-render
                          * update_screen + fade_update, then loop). */
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

/* GAME_MODE_DOOR_TRANSITION phases. Port of door_transition(): show optional door
 * text, fade/swirl out, load the destination map + place the party, fade/swirl in,
 * spawn deliveries. The yielding children (door text → TEXT_WAIT_FADE; the two
 * screen_transition() calls → SCREEN_TRANSITION) are STEP_PUSHed; the door-field
 * scalars are hoisted at DTR_BEGIN so no ROM pointer is carried across a yield.
 * The *_FIN phases run screen_transition_finalize() after the pushed transition
 * pops (then fall through inline, matching the blocking original's ordering). */
typedef enum {
    DTR_BEGIN = 0,       /* read door fields; push door text if any */
    DTR_AFTER_TEXT,      /* flag check, clear flags, exit-transition out */
    DTR_TRANS_OUT_FIN,   /* screen_transition_finalize() after exit pop */
    DTR_AFTER_OUT,       /* load map, place party, enter-transition in */
    DTR_TRANS_IN_FIN,    /* screen_transition_finalize() after enter pop */
    DTR_FINALIZE,        /* finalize; push the buzz-buzz check text */
    DTR_BUZZ_DONE,       /* spawn delivery entities, clear using_door, pop */
    DTR_EXIT_PREPARE,    /* after the 2-frame exit pre-wait pops: prepare + push exit transition */
} DoorTransitionPhase;

typedef struct {
    uint8_t  phase;            /* DoorTransitionPhase */
    uint8_t  transition_type;  /* door_data::unknown10 */
    uint32_t door_ptr;         /* original SNES door-data pointer (for re-fetch) */
    uint32_t text_ptr;         /* door text address (0 = none) */
    uint16_t event_flag_raw;   /* door_data::event_flag */
    uint16_t unknown6;         /* dest y (low 14 bits) + direction class (bits 14-15) */
    uint16_t unknown8;         /* dest x tile */
} DoorTransitionState;

/* GAME_MODE_TELEPORT_TO phases. Run-to-completion port of the TELEPORT sequence
 * (asm/overworld/teleport.asm): clear temp flags, fade/swirl out, load the
 * destination map + place the party, fade/swirl in, spawn deliveries. Shared by
 * CC_1F_21 (TELEPORT_TO script command, pushed via the display_text CC channel) and
 * the debug menu's CAST/STAFF teleport-back. Only the destination id is carried
 * (re-resolved each step via get_teleport_dest, so no ROM pointer crosses a yield);
 * the two screen_transition() calls → SCREEN_TRANSITION pushes (via
 * screen_transition_prepare/_finalize), the buzz-buzz check text → DISPLAY_TEXT. The
 * *_FIN phases run screen_transition_finalize() after the pushed transition pops.
 * The blocking screen_transition() pump bridge was deleted in D4b. */
typedef enum {
    TT_BEGIN = 0,       /* save+set suppression, clear flags, door interactions, exit-transition out */
    TT_TRANS_OUT_FIN,   /* screen_transition_finalize() after exit pop */
    TT_AFTER_OUT,       /* load map, place party, music, enter-transition in */
    TT_TRANS_IN_FIN,    /* screen_transition_finalize() after enter pop */
    TT_FINALIZE,        /* stairs reset; push the buzz-buzz check text */
    TT_BUZZ_DONE,       /* spawn deliveries, restore suppression, pop */
    TT_EXIT_PREPARE,    /* after the 2-frame exit pre-wait pops: prepare + push exit transition */
} TeleportToPhase;

typedef struct {
    uint8_t phase;             /* TeleportToPhase */
    uint8_t dest_id;           /* teleport destination index (re-resolved each step) */
    uint8_t saved_suppression; /* ow.overworld_status_suppression to restore at the end */
} TeleportToState;

/* GAME_MODE_WAIT_FRAMES — run-to-completion form of wait_frames_with_updates()
 * (asm WAIT_FRAMES_WITH_UPDATES / C0DD2C): render `remaining` frames (each
 * oam_clear -> run_actionscript_frame -> update_screen -> yield), then POP. A parked
 * callroutine becomes a STEP_PUSH of GAME_MODE_ACTIONSCRIPT_FRAME (the WF_FLUSH
 * resume finishes that frame), matching the original's blocking pump. Init with
 * ModeState.wait_frames (phase = WF_FRAME, remaining = N) before the STEP_PUSH. */
typedef enum {
    WF_FRAME = 0,  /* render one frame; on a park, push ACTIONSCRIPT_FRAME and resume at WF_FLUSH */
    WF_FLUSH,      /* resume after the (possibly parked) frame: update_screen, count down, yield */
} WaitFramesPhase;

typedef struct {
    uint8_t  phase;     /* WaitFramesPhase */
    uint16_t remaining; /* frames left to render */
} WaitFramesState;

/* GAME_MODE_ENDING — run-to-completion port of the two blocking end-of-game
 * sequences play_cast_scene() and play_credits() (asm/ending/). Each of their
 * former blocking `render_frame_tick()` loops becomes a frame-yielding phase: the
 * frame is rendered via render_frame_tick_work_step() and, on a parked callroutine,
 * STEP_PUSHes GAME_MODE_ACTIONSCRIPT_FRAME (resumed at EN_FLUSH, which jumps back to
 * `resume_phase`), exactly like GAME_MODE_WAIT_FRAMES. Init via ModeState.ending
 * (phase = EN_CAST_SETUP for the cast scene, or EN_CR_SETUP for credits) before the
 * STEP_PUSH; always POPs 0. The synchronous teardown's single
 * force_blank_and_wait_vblank() is left as-is (a one-frame transient, not a loop). */
typedef enum {
    /* cast scene (play_cast_scene) */
    EN_CAST_SETUP = 0, /* load scene, fade in, init EVENT_801 wipe */
    EN_CAST_LOOP,      /* render until ert.actionscript_state != 0 */
    EN_CAST_FLUSH,     /* cast loop: resume after a parked frame */
    EN_CAST_TEARDOWN,  /* fade out, re-init party, POP */
    /* credits (play_credits) */
    EN_CR_SETUP,       /* load assets, init credits scene, fade in, count photos */
    EN_CR_PHOTO_TOP,   /* per-photo: try render; skip or start fade-in */
    EN_CR_FADEIN,      /* 64-frame photo palette fade-in */
    EN_CR_SLIDE,       /* slide_credits_photograph frame loop */
    EN_CR_SCROLLWAIT,  /* wait for scroll to pass next_photo_pos */
    EN_CR_FADEOUT,     /* 64-frame photo palette fade-out */
    EN_CR_PHOTO_TAIL,  /* clear palette, one settle frame, advance photo */
    EN_CR_SCROLLFINAL, /* wait for scroll to reach CREDITS_LENGTH */
    EN_CR_HOLD,        /* 2000-frame hold, then teardown + POP */
    EN_FLUSH,          /* resume after a parked frame: render_frame_tick_work_flush, -> resume_phase */
} EndingPhase;

typedef struct {
    uint8_t  phase;        /* EndingPhase */
    uint8_t  resume_phase; /* EndingPhase EN_FLUSH returns to */
    uint16_t photo_idx;    /* current photo (0..NUM_PHOTOS-1) */
    uint16_t photo_spacing;
    uint16_t next_photo_pos;
    uint16_t fade_counter; /* 64-frame fade-in/out countdown */
    uint16_t hold_counter; /* final 2000-frame hold */
    int16_t  slide_dx, slide_dy;
    uint16_t slide_total_frames;
    uint16_t slide_frame;
    int32_t  slide_accum_x, slide_accum_y;
    uint16_t slide_start_x, slide_start_y;
} EndingState;

/* GAME_MODE_QUICK_CHECKTALK phases. Port of open_menu_button_checktalk(): the
 * L-button quick talk/check. Resolve the target text (talk_to → check_action →
 * fallback), show it, close the windows, wait for the entity fade. */
typedef enum {
    QCT_TEXT = 0,  /* disable entities, resolve text, STEP_PUSH DISPLAY_TEXT */
    QCT_FADE,      /* close windows, STEP_PUSH ENTITY_FADE_WAIT */
    QCT_DONE,      /* enable entities + POP */
} QuickChecktalkPhase;

typedef struct {
    uint8_t phase;   /* QuickChecktalkPhase */
} QuickChecktalkState;

/* GAME_MODE_PAUSE_MENU phases. Port of open_menu_button() (text.c,
 * asm/overworld/open_menu.asm): the full overworld pause menu — Talk to, Goods
 * (with the Use/Give/Drop/Help cascade), PSI, Equip, Check, Status. The former
 * goto-heavy for(;;) becomes a phase machine in the file_menu idiom: each
 * sub-menu builds its window synchronously, STEP_PUSHes GAME_MODE_SELECTION_MENU
 * (or GAME_MODE_CHAR_SELECT / GAME_MODE_DISPLAY_TEXT), and reads the choice back
 * via mode_child_result() in the matching *_RESULT phase. Phases chain inside an
 * internal for(;;) so no-yield transitions match the blocking original.
 *
 * All four former blocking sub-drivers — PSI, Equip, Status, and Goods→Use —
 * are now their own modes (GAME_MODE_PSI_MENU / EQUIP_MENU / STATUS_MENU /
 * USE_ITEM), STEP_PUSHed from here with the tails in PM_*_RESUME phases. */
typedef enum {
    PM_ENTER = 0,           /* one-shot setup (disable entities, command menu); no yield */
    PM_MAIN,                /* focus command menu; push SELECTION_MENU(1) */
    PM_MAIN_RESULT,         /* dispatch Talk/Goods/PSI/Equip/Check/Status */
    PM_GOODS_CHAR,          /* goods character select (single inline / multi push CHAR_SELECT) */
    PM_GOODS_CHAR_RESULT,   /* after the multi-party CHAR_SELECT pops */
    PM_GOODS_INV,           /* "Which?" header; push SELECTION_MENU(1) on the inventory */
    PM_GOODS_INV_RESULT,    /* item chosen or cancelled; build the Use/Give/Drop/Help menu */
    PM_ACTION_MENU,         /* @ITEM_ACTION_LOOP head; push SELECTION_MENU(1) */
    PM_ACTION_RESULT,       /* dispatch Use/Give/Drop/Help/cancel */
    PM_USE_RESUME,          /* after USE_ITEM pops: used->cleanup / cancel->PM_ACTION_MENU */
    PM_HELP_RESUME,         /* after the help text pops: rebuild menus -> PM_GOODS_INV */
    PM_GIVE_CHAR_RESULT,    /* after the give-target CHAR_SELECT pops */
    PM_GIVE_BLOCKED_RESUME, /* after the EXCLUSIVE_CARRIER text pops -> PM_ACTION_MENU */
    PM_GIVE_MSG_RESUME,     /* after the give message pops: swap item + close -> PM_MAIN */
    PM_DROP_RESUME,         /* after the drop text pops: close windows -> PM_MAIN */
    PM_EQUIP_RESUME,        /* after EQUIP_MENU pops: single-party sfx tail -> PM_MAIN */
    PM_PSI_RESUME,          /* after PSI_MENU pops: used->cleanup / single-PSI sfx tail */
    PM_CLEANUP,             /* @CLEANUP_AND_CLOSE; push ENTITY_FADE_WAIT */
    PM_DONE,                /* enable entities + POP */
} PauseMenuPhase;

typedef struct {
    uint8_t  phase;          /* PauseMenuPhase */
    uint8_t  result_ready;   /* 1 = `result` holds an inline early-exit value (no child pushed) */
    uint8_t  action_reentry; /* @VIRTUAL02: action-menu re-entry focuses the inventory */
    uint8_t  reprint_inventory; /* @LOCAL04: re-entry also reprints the inventory items
                                 * (set by the give-cancel/give-blocked returns, whose
                                 * CLEAR_FOCUS_WINDOW_CONTENT wiped the item list) */
    uint8_t  give_case;      /* give message case index (0-9) */
    uint16_t result;         /* inline early-exit selection result */
    uint16_t goods_char;     /* 1-based character whose inventory is open */
    uint16_t item_slot;      /* 1-based selected item slot */
    uint16_t give_target;    /* 1-based give recipient */
} PauseMenuState;

/* GAME_MODE_EQUIP_MENU phases. Port of open_equipment_menu() +
 * equipment_change_menu() (src/inventory/equipment/open_equipment_menu.asm 66
 * lines + equipment_change_menu.asm 252 lines): the pause menu's whole Equip
 * cascade — character selection (multi-party CHAR_SELECT with the
 * equipment-stats on_change, or single-party auto-select), then the
 * slot-selection ("Where?") and item-selection ("Which?") menus, both
 * SELECTION_MENU pushes (the stat-preview cursor callbacks live in the
 * re-fetchable WindowInfo). Always pops 0; the pause-menu parent runs its
 * single-party sfx tail in PM_EQUIP_RESUME. */
typedef enum {
    EQ_ENTER = 0,     /* save text attrs; single-party initial equipment display */
    EQ_SELECT,        /* loop head: multi pushes CHAR_SELECT, single selects inline */
    EQ_SELECT_RESULT, /* after the CHAR_SELECT pops: cancel exits, else change */
    EQ_CHANGE,        /* enter the change menu: load the equip text data */
    EQ_SLOT,          /* "Where?" header; push SELECTION_MENU on the slot menu */
    EQ_SLOT_RESULT,   /* cancel loops/exits; else build the item list + push */
    EQ_ITEM_RESULT,   /* unequip/equip/cancel; close list; refresh; -> EQ_SLOT */
    EQ_EXIT,          /* close windows, restore text attrs, POP */
} EquipMenuPhase;

typedef struct {
    uint8_t  phase;        /* EquipMenuPhase */
    uint8_t  result_ready; /* 1 = `result` holds an inline early-exit value */
    uint16_t result;       /* inline early-exit selection result */
    uint16_t equip_char;   /* 1-based character being equipped */
    uint16_t slot_type;    /* selected equipment slot (1=Weapon..4=Other) */
} EquipMenuState;

/* GAME_MODE_STATUS_MENU phases. Port of open_status_menu()
 * (asm/text/menu/open_status_menu.asm, 118 lines): the pause menu's Status
 * cascade — character selection (CHAR_SELECT with the status-window on_change,
 * which is instant-printed and never yields), then the PSI category menu and
 * the PSI description browse loop (both SELECTION_MENU pushes; their cursor
 * callbacks live in the re-fetchable WindowInfo, exactly as when the blocking
 * selection_menu() pumped the same step). The FORCE_LEFT_TEXT_ALIGNMENT
 * bracket the pause-menu caller used to hold lives inside the mode (set at
 * SU_SELECT, cleared at SU_EXIT), keeping it self-contained for the push. */
typedef enum {
    SU_SELECT = 0,    /* alignment on; push CHAR_SELECT (status on_change) */
    SU_SELECT_RESULT, /* cancel exits; Jeff re-selects; else build category menu */
    SU_CAT_HEAD,      /* focus category; first display prints + one window frame */
    SU_CAT_BODY,      /* status window refresh; push SELECTION_MENU (PSI list cb) */
    SU_CAT_RESULT,    /* cancel closes cascade; empty category re-loops; else browse */
    SU_PSI,           /* PSI description browse: push SELECTION_MENU (descr cb) */
    SU_PSI_RESULT,    /* non-zero stays browsing; cancel closes PSI windows */
    SU_EXIT,          /* close STATUS_MENU window, alignment off, POP */
    SU_CAT_HEAD_FLUSH, /* park-propagating resume of SU_CAT_HEAD's first-display frame */
} StatusMenuPhase;

typedef struct {
    uint8_t  phase;         /* StatusMenuPhase */
    uint8_t  first_display; /* @VIRTUAL02 == 0: category menu's first render pending */
    uint8_t  result_ready;  /* 1 = `result` holds an inline early-exit value */
    uint16_t result;        /* inline early-exit selection result */
} StatusMenuState;

/* GAME_MODE_HPPP_DISPLAY phases. Port of open_hppp_display()
 * (asm/text/open_hppp_display.asm): the B/Select overworld HP/PP + money
 * display. Shows the windows, then idles on window frames until A/L opens the
 * full pause menu (STEP_PUSH GAME_MODE_PAUSE_MENU, which owns ALL the cleanup
 * — the display just pops after it) or B/Select dismisses. `primed`
 * reproduces the blocking loop's render-before-first-read order (the input
 * acted on at the top of a step is what the pump's previous yield latched). */
typedef enum {
    HD_ENTER = 0,  /* disable entities, sfx, show HPPP + money windows */
    HD_TICK,       /* idle window frame + input: A/L → pause menu, B → dismiss */
    HD_MENU_DONE,  /* the pushed PAUSE_MENU popped (it did all cleanup) → POP */
    HD_EXIT,       /* post-dismiss-frame: enable entities → POP */
    HD_TICK_FLUSH, /* park-propagating resume of an HD_TICK window frame (stay in HD_TICK) */
    HD_CANCEL_FLUSH, /* park-propagating resume of the dismiss window frame (→ HD_EXIT) */
} HpppDisplayPhase;

typedef struct {
    uint8_t phase;  /* HpppDisplayPhase */
    uint8_t primed; /* 0 = first frame: render without reading input */
} HpppDisplayState;

/* GAME_MODE_PSI_MENU phases. Port of overworld_psi_menu()
 * (asm/text/menu/overworld_psi_menu.asm, 571 lines): the pause menu's PSI
 * cascade — character select (CHAR_SELECT push with the PSI-list on_change +
 * PSI-availability check_valid), ability select (SELECTION_MENU push with the
 * target/cost cursor callback), PP-cost / teleport-blocked failure texts
 * (DISPLAY_TEXT pushes sharing the PS_FAIL_RESUME tail), then targeting and
 * execution.
 *
 * The teleport destination menu and targeting are GAME_MODE_TELEPORT_MENU /
 * GAME_MODE_DETERMINE_TARGETING STEP_PUSHes (results read in
 * PS_TELEPORT_RESULT / PS_TARGET_RESULT). The action execution dispatches via
 * battle_action_dispatch(): converted actions are GAME_MODE_BATTLE_ACTION
 * STEP_PUSHes (PS_EXEC_* phases), unconverted ones run inline-blocking.
 *
 * Pops 1 if a PSI was used (the pause menu closes), else 0; the pause-menu
 * parent branches in PM_PSI_RESUME. */
typedef enum {
    PS_ENTER = 0,       /* reset trackers, fall through to the char-select loop */
    PS_CHAR,            /* @CHARACTER_SELECT: single auto-selects, multi pushes */
    PS_CHAR_RESULT,     /* after the CHAR_SELECT pops: cancel exits */
    PS_ABILITY,         /* @PSI_ABILITY_LOOP head: redisplay + push SELECTION_MENU */
    PS_ABILITY_RESULT,  /* PP/teleport checks; targeting (inline) or failure text */
    PS_TELEPORT_RESULT, /* after the TELEPORT_MENU pops: store result -> PS_HANDLE */
    PS_TARGET_RESULT,   /* after DETERMINE_TARGETING pops: store result -> PS_HANDLE */
    PS_FAIL_RESUME,     /* after a failure text pops: close window, retry ability */
    PS_HANDLE,          /* @HANDLE_RESULT: retry/back, or execute (desc text push) */
    PS_EXECUTE,         /* battle-action dispatch head: teleport/null/all-vs-single */
    PS_EXEC_STEP,       /* all-party loop head: per-member setup + action dispatch */
    PS_EXEC_STEP_DONE,  /* after one member's action: affliction copy, next member */
    PS_EXEC_DONE,       /* after a single-target action: affliction copy + render */
    PS_RADE,            /* render_and_disable_entities split: work + yield */
    PS_RADE_FLUSH,      /* render_and_disable_entities split: flush a parked frame */
    PS_RADE_FINISH,     /* render_and_disable_entities split: disable tail -> PS_EXIT */
    PS_EXIT,            /* close the text window, POP the result */
} PsiMenuPhase;

typedef struct {
    uint8_t  phase;            /* PsiMenuPhase */
    uint8_t  exec_i;           /* PS_EXEC_STEP all-party loop index */
    uint8_t  result_ready;     /* 1 = `menu_result` holds an inline early-exit value */
    uint8_t  pp_cost;          /* selected ability's PP cost */
    uint8_t  psi_category;     /* selected ability's category (PSI_CAT_*) */
    uint16_t menu_result;      /* inline early-exit selection result */
    uint16_t char_id;          /* 1-based PSI user */
    uint16_t last_ability;     /* @VIRTUAL01: last ability selection (0xFF = initial) */
    uint16_t battle_action_id; /* selected ability's battle action */
    uint16_t action_result;    /* @LOCAL09: targeting/teleport result; 1 = PSI used */
} PsiMenuState;

/* GAME_MODE_USE_ITEM phases. Port of overworld_use_item()
 * (asm/overworld/use_item.asm, 545 lines): the pause menu's Goods → Use path.
 * UI_ENTER classifies the item (type flags, per-character usability, sector
 * context, nearby NPCs) and, if usable, STEP_PUSHes DETERMINE_TARGETING
 * (cancel pops 0 in UI_TARGET_RESULT, which also runs the consume-on-use
 * removal). UI_SETUP does the text-window/battle-name setup and pushes the
 * description (or failure) text as a DISPLAY_TEXT child; the action execution
 * resumes in UI_EXECUTE after the text pops.
 *
 * The action execution dispatches via battle_action_dispatch(): converted
 * actions are GAME_MODE_BATTLE_ACTION STEP_PUSHes (UI_EXEC_* phases),
 * unconverted ones run inline-blocking.
 *
 * Pops 0 if targeting was cancelled (the pause menu re-enters the action
 * menu), else 1 (item used or message shown — the pause menu closes); the
 * parent branches in PM_USE_RESUME. */
typedef enum {
    UI_ENTER = 0,      /* classify; usable items push DETERMINE_TARGETING */
    UI_TARGET_RESULT,  /* cancel pops 0; else consume-on-use -> UI_SETUP */
    UI_SETUP,          /* @SETUP_ACTION_WINDOW: window/name setup; push text */
    UI_EXECUTE,        /* after the desc text pops: battle-action dispatch head */
    UI_EXEC_STEP,      /* all-party loop head: per-member setup + action dispatch */
    UI_EXEC_STEP_DONE, /* after one member's action: affliction copy, next member */
    UI_EXEC_DONE,      /* after a single-target action: affliction copy + render */
    UI_RADE,           /* render_and_disable_entities split: work + yield */
    UI_RADE_FLUSH,     /* render_and_disable_entities split: flush a parked frame */
    UI_RADE_FINISH,    /* render_and_disable_entities split: disable tail -> UI_EXIT */
    UI_EXIT,           /* @CLOSE_TEXT_WINDOW: close the text window, POP 1 */
} UseItemPhase;

typedef struct {
    uint8_t  phase;          /* UseItemPhase */
    uint8_t  exec_i;         /* UI_EXEC_STEP all-party loop index */
    uint8_t  target_id;      /* @VIRTUAL00: 1-based target; 0xFF = all party */
    uint8_t  can_use;        /* @LOCAL07: item passed the usability checks */
    uint16_t char_id;        /* 1-based item user (input, set by the parent) */
    uint16_t item_slot;      /* selected inventory slot (input, set by the parent) */
    uint16_t item_id;        /* item being used */
    uint16_t effect_id;      /* item's battle-action table index */
    uint32_t desc_text_addr; /* @LOCAL08: description/failure text (0 = fallback) */
} UseItemState;

/* GAME_MODE_TELEPORT_MENU phases. Port of open_teleport_destination_menu()
 * (asm/text/menu/open_teleport_destination_menu.asm, 95 lines): the PSI
 * Teleport "Where?" destination menu. TPM_ENTER builds the window (header
 * text, "To:" title, one menu item per unlocked destination) and STEP_PUSHes
 * SELECTION_MENU; the window cleanup runs in TPM_RESULT after it pops. An
 * empty destination list skips the push and pops 0 after the same cleanup.
 * Pushed by the PSI menu's teleport case (result read in PS_TELEPORT_RESULT)
 * and by CC 1A 0x0B (result stored via DT_RESUME_CC1A_TELEPORT).
 * Pops the 1-based destination index, or 0 if cancelled/empty. */
typedef enum {
    TPM_ENTER = 0,  /* build the destination menu; push SELECTION_MENU */
    TPM_RESULT,     /* close windows, restore text attrs, POP the selection */
} TeleportMenuPhase;

typedef struct {
    uint8_t phase;  /* TeleportMenuPhase */
} TeleportMenuState;

/* GAME_MODE_STORE_MENU phases. Port of open_store_menu()
 * (src/inventory/store/open_store_menu.asm): the shop item-purchase list.
 * STM_ENTER builds the window (money window, store item list with prices,
 * HPPP equip-preview cursor callback) and STEP_PUSHes SELECTION_MENU; the
 * cleanup runs in STM_RESULT after it pops. An empty item list skips the
 * push and pops 0 after the same cleanup. Pushed by CC 1A 0x06; result
 * stored to working memory via DT_RESUME_MENU_RESULT.
 * Pops the selected item id, or 0 if cancelled/empty. */
typedef enum {
    STM_ENTER = 0,  /* build the shop menu; push SELECTION_MENU */
    STM_RESULT,     /* cleanup, POP the selected item id */
} StoreMenuPhase;

typedef struct {
    uint8_t  phase;    /* StoreMenuPhase */
    uint16_t shop_id;  /* STORE_TABLE shop index */
} StoreMenuState;

/* GAME_MODE_ESCARGO_MENU phases. Port of select_escargo_express_item()
 * (src/inventory/select_escargo_express_item.asm): the Escargo Express
 * "Stored Goods" item list. EEM_ENTER builds the window and STEP_PUSHes
 * SELECTION_MENU; the cleanup runs in EEM_RESULT after it pops. An empty
 * storage list skips the push and pops 0 after the same cleanup. Pushed by
 * CC 1A 0x07; result stored to working memory via DT_RESUME_MENU_RESULT.
 * Pops the 1-based selection index, or 0 if cancelled/empty. */
typedef enum {
    EEM_ENTER = 0,  /* build the storage menu; push SELECTION_MENU */
    EEM_RESULT,     /* cleanup, POP the selection index */
} EscargoMenuPhase;

typedef struct {
    uint8_t phase;  /* EscargoMenuPhase */
} EscargoMenuState;

/* GAME_MODE_TELEPHONE_MENU phases. Port of open_telephone_menu() +
 * display_telephone_contact_text() (asm/text/menu/open_telephone_menu.asm,
 * asm/text/display_telephone_contact_text.asm): the phone directory. TPH_ENTER
 * builds the contact menu and STEP_PUSHes SELECTION_MENU; TPH_AFTER_MENU runs
 * the window cleanup, and when show_text is set and a contact was chosen it
 * STEP_PUSHes DISPLAY_TEXT (the contact's call text) before popping. An empty
 * contact list skips the push and pops 0 after the same cleanup. Pushed by
 * CC 1F 0x90 (show_text=0) and CC 1A 0x0A (show_text=1); result stored to
 * working memory via DT_RESUME_MENU_RESULT.
 * Pops the 1-based contact index, or 0 if cancelled/empty. */
typedef enum {
    TPH_ENTER = 0,    /* build the contact menu; push SELECTION_MENU */
    TPH_AFTER_MENU,   /* cleanup; (optional) push the contact's call text */
    TPH_AFTER_TEXT,   /* POP the contact index after the call text */
} TelephoneMenuPhase;

typedef struct {
    uint8_t  phase;      /* TelephoneMenuPhase */
    uint8_t  show_text;  /* 1 = display the chosen contact's call text */
    uint16_t selection;  /* chosen contact index (carried across pushes) */
} TelephoneMenuState;

/* GAME_MODE_DETERMINE_TARGETING phases. Port of determine_targetting()
 * (asm/battle/determine_targetting.asm): looks up the battle action's
 * direction and target type from battle_action_table, then either resolves
 * the target inline (auto/self/random/all — TGT_ENTER pops immediately) or
 * runs a targeting UI as a child mode:
 *   enemy ONE  -> STEP_PUSH BATTLE_ENEMY_SELECT  -> TGT_PICK_RESULT
 *   enemy ROW  -> STEP_PUSH BATTLE_ROW_SELECT    -> TGT_PICK_RESULT
 *   ally  ONE  -> (multi-party) "Whom?" header + the char_select_prompt
 *                 mode-1 name window, STEP_PUSH SELECTION_MENU -> TGT_ALLY_RESULT
 *
 * Pushed by the PSI menu (PS_TARGET_RESULT), the use-item driver
 * (UI_TARGET_RESULT), the battle PSI menu (BP_TARGET_RESULT), and the battle
 * command menu's Goods case (BM_ITEM_TGT_RESULT).
 *
 * Pops (targeting_mode << 8) | target_index (TARGETTED_* flags in the high
 * byte; 0xFF = auto/all target), or 0 if a targeting UI was cancelled. */
typedef enum {
    TGT_ENTER = 0,    /* classify; inline cases POP, UI cases push a child */
    TGT_PICK_RESULT,  /* after enemy/row select pops: 0 = cancel, else pack */
    TGT_ALLY_RESULT,  /* after the ally SELECTION_MENU pops: cleanup + pack */
} TargetingPhase;

typedef struct {
    uint8_t  phase;          /* TargetingPhase */
    uint8_t  targeting_mode; /* TARGETTED_* flags (the packed high byte) */
    uint16_t action_id;      /* battle_action_table index (input) */
    uint16_t char_id;        /* 1-based acting character (input) */
    uint16_t ally_window_id; /* mode-1 char-select window to close on pop */
    uint32_t saved_argument_memory; /* restored in TGT_ALLY_RESULT */
} TargetingState;

/* GAME_MODE_BATTLE_PSI_MENU phases. Port of battle_psi_menu()
 * (asm/battle/battle_psi_menu.asm): the in-battle PSI selection cascade —
 * the category menu (Offense/Recover/Assist/Other) and the per-category
 * ability list (both SELECTION_MENU pushes; their cursor callbacks —
 * generate_battle_psi_list_callback / display_psi_target_and_cost — live in
 * the re-fetchable WindowInfo, exactly as when the blocking selection_menu()
 * pumped the same step), the not-enough-PP message (DISPLAY_TEXT push
 * resuming at BP_PP_RESUME), and targeting (DETERMINE_TARGETING push).
 *
 * On success fills the bt.battle_menu_* selection fields and pops 1; pops 0
 * on cancel. Pushed by GAME_MODE_BATTLE_MENU's PSI case (BM_PSI_RESULT). */
typedef enum {
    BP_OPEN = 0,        /* @OPEN_CATEGORY_WINDOW: (re)create + populate */
    BP_CATEGORY,        /* @CATEGORY_SELECTION: focus; push SELECTION_MENU */
    BP_CATEGORY_RESULT, /* cancel exits; empty category re-loops; else list */
    BP_LIST,            /* @OPEN_PSI_LIST: build the list; push SELECTION_MENU */
    BP_LIST_RESULT,     /* cancel -> category; PP-fail text; else targeting */
    BP_PP_RESUME,       /* after the PP-fail text pops: close, retry the list */
    BP_TARGET_RESULT,   /* close windows; cancel -> BP_OPEN; else store + pop 1 */
} BattlePsiMenuPhase;

typedef struct {
    uint8_t  phase;            /* BattlePsiMenuPhase */
    uint8_t  menu_printed;     /* @LOCALEB: category items printed once (US) */
    uint8_t  action_direction; /* selected action's direction (window mgmt) */
    uint8_t  action_target;    /* selected action's target type (window mgmt) */
    uint16_t char_id;          /* bt.battle_menu_user at entry (input) */
    uint16_t category;         /* selected PSI category */
    uint16_t psi_selection;    /* selected ability id */
    uint16_t battle_action_id; /* selected ability's battle action */
} BattlePsiMenuState;

/* GAME_MODE_BATTLE_MENU phases. Port of battle_selection_menu()
 * (asm/battle/menu_handler.asm): the per-character battle command menu —
 * Bash/Shoot, Goods, Auto Fight, PSI/Spy, Defend, Run Away, Pray/Mirror.
 * BM_ENTER runs the whole synchronous front half (attack palette, weapon-type
 * classification, the auto-fight AI — which resolves and pops inline without
 * ever yielding — and the manual menu construction). BM_MAIN is the
 * @MENU_SELECTION_LOOP head (focus + print-once + SELECTION_MENU push);
 * BM_MAIN_RESULT handles cancel/debug input and dispatches the chosen command.
 * Target selection for Bash/Shoot/Spy/Mirror is a BATTLE_ENEMY_SELECT push
 * (the select_battle_target_dispatch mode-0 path, via enemy_select_make_init);
 * PSI pushes BATTLE_PSI_MENU; Goods folds the former battle_item_menu() +
 * determine_battle_item_target() drivers into BM_ITEM/BM_ITEM_RESULT/
 * BM_ITEM_TGT_RESULT (inventory SELECTION_MENU push, then a
 * DETERMINE_TARGETING push for item types that target).
 *
 * Pops the selected battle action (stored in bt.battle_menu_*), 0 for
 * cancel/"back", or 0xFFFF for the debug instant win. battle_selection_menu()
 * (battle.c) is the pump bridge for the still-blocking battle_routine. */
typedef enum {
    BM_ENTER = 0,       /* weapon-type calc; auto-fight pops inline; build the menu */
    BM_MAIN,            /* @MENU_SELECTION_LOOP: focus, print-once, push SELECTION_MENU */
    BM_MAIN_RESULT,     /* cancel/debug handling, or dispatch the chosen command */
    BM_TARGET_RESULT,   /* after the enemy select pops (Bash/Shoot/Spy/Mirror) */
    BM_PSI_RESULT,      /* after BATTLE_PSI_MENU pops */
    BM_ITEM,            /* Goods: build the inventory window; push SELECTION_MENU */
    BM_ITEM_RESULT,     /* item chosen/cancelled; classify; push targeting if needed */
    BM_ITEM_TGT_RESULT, /* after the item's DETERMINE_TARGETING pops */
} BattleMenuPhase;

typedef struct {
    uint8_t  phase;           /* BattleMenuPhase */
    uint8_t  menu_printed;    /* @LOCAL08: command items printed once */
    uint8_t  window_index;    /* @LOCAL03: battle_window_sizes[] index */
    uint8_t  weapon_type;     /* @LOCAL06: 0=bash, 1=shoot, 2=paralyzed/immobilized */
    uint16_t char_id;         /* 1-based character whose turn it is (input) */
    uint16_t num_selected;    /* characters already committed this round (input) */
    uint16_t selected_action; /* @LOCAL05/@VIRTUAL02: the action being built */
    uint16_t item_effect;     /* Goods: effect id carried across DETERMINE_TARGETING */
} BattleMenuState;

/* GAME_MODE_BATTLE phases. Port of battle_routine()
 * (asm/battle/main_battle_routine.asm): the main battle loop — init/reinit,
 * encounter texts, the per-turn player menus (GAME_MODE_BATTLE_MENU pushes),
 * enemy AI, the run-away check, turn execution, battle-end checks, EXP
 * distribution (GAME_MODE_LEVEL_UP pushes via gain_exp_prepare) and the
 * battle ending. The former goto labels map onto the BTL_* phases; every
 * display_in_battle_text_addr / display_text_wait_addr /
 * display_text_with_prompt_addr site is a DISPLAY_TEXT push via
 * battle_push_text_ex() (battle.c — the battle text prologue runs inline
 * before the push, the resume phase runs the epilogue prompt-flag clear),
 * and the former BATTLE_WAIT / FADE_WAIT pumps are STEP_PUSHes.
 *
 * The action execution dispatches via battle_action_dispatch() in BTL_TARGET:
 * converted actions are GAME_MODE_BATTLE_ACTION STEP_PUSHes (resume in
 * BTL_TARGET_POST), unconverted ones run inline-blocking (the battle_actions.c
 * long tail, convertible incrementally). Kept inline-blocking (documented
 * deferrals): the debug-only encounter-setup loop (+ enemy_select_mode), and
 * the force_blank/blank_screen one-shot vblank helpers.
 *
 * Pops the battle result: 0 = victory, 1 = party defeated, 2 = special
 * defeat code. Pushed by GAME_MODE_BATTLE_ENTRY (overworld encounters) and
 * GAME_MODE_BATTLE_SCRIPTED (event-triggered battles), which inline
 * init_battle_common()'s fade-out/party-update halves around the push. */
typedef enum {
    BTL_BEGIN = 0,        /* one-time setup (entry-table loads, BG/letterbox) */
    BTL_REINIT,           /* reinit_battle: scene + battler init; pre-debug window_tick */
    BTL_DEBUG,            /* debug encounter-setup loop (inline-blocking) / battle music */
    BTL_PREP,             /* buzz-buzz/possessed, item drop, initiative, encounter text */
    BTL_AURA,             /* party-first "Green/blue/red aura!" text */
    BTL_ANNOUNCE,         /* per-enemy initial status texts (announce_i/announce_stage) */
    BTL_TURN,             /* start_turn: initiative rolls; reset the menu loop */
    BTL_MENU,             /* player menu loop head: skip checks or push BATTLE_MENU */
    BTL_MENU_RESULT,      /* selection writeback / back / run-away / debug exits */
    BTL_ENEMY_AI,         /* enemy action selection; enemy-first announce text */
    BTL_RUN_CHECK,        /* run-away attempt: success/failure text */
    BTL_RUN_SUCCESS,      /* after the escape text: battle over */
    BTL_RUN_FAIL,         /* after the failure text: clear initiative, execute turns */
    BTL_EXEC,             /* execute_turns: pick attacker, overrides, status-damage text */
    BTL_EXEC_DMG,         /* apply status damage after its text */
    BTL_EXEC_DMG_KO,      /* after a status-damage BATTLE_KO pops: end checks */
    BTL_EXEC_SETUP,       /* targeting, PP check (fail text), attack palette, bob wait */
    BTL_EXEC_RETARGET1,   /* after the bob: "acting unusual" text */
    BTL_EXEC_RETARGET2,   /* "acting funky" text */
    BTL_EXEC_DESC,        /* action description text (with prompt) */
    BTL_EXEC_ACT,         /* action-0 skip; PSI-animation wait */
    BTL_TARGET,           /* per-target loop head: gone text, action dispatch */
    BTL_TARGET_POST,      /* after the action: defeat checks, screen-effect wait */
    BTL_AFTER_ACTION,     /* consume item; mirror countdown text */
    BTL_AFTER_STATUS,     /* post-action recovery rolls: cured text */
    BTL_AFTER_CURED,      /* clear the cured temporary status after its text */
    BTL_AFTER_CONC,       /* concentration countdown: cured text */
    BTL_AFTER_TAIL,       /* clear alt spritemaps, re-show HP/PP windows */
    BTL_END_CHECK,        /* party defeated: enemy-victory text */
    BTL_DEFEAT_DONE,      /* after the defeat text: flag battle end */
    BTL_END_CHECK2,       /* enemies dead -> victory; else continue the turn */
    BTL_VICTORY,          /* money/EXP split; victory text */
    BTL_VICTORY_DROP,     /* item-drop announce text */
    BTL_VICTORY_EXP,      /* per-member EXP loop: GAME_MODE_LEVEL_UP pushes */
    BTL_TURN_CONT,        /* check_turn_continue */
    BTL_CLOSE_WINDOW,     /* close_battle_window + check_new_turn */
    BTL_ENDING,           /* battle_ending: HP/PP-stable wait */
    BTL_ENDING_MIRROR,    /* mirror restore + cleanup; debug reinit; fade-out wait */
    BTL_EXIT,             /* final window/effect teardown; pop the result */
    BTL_REINIT_FLUSH,     /* park-propagating resume of BTL_REINIT's pre-debug window_tick */
    /* Resume points after a GAME_MODE_CHECK_DEAD_PLAYERS push (each former
     * check_dead_players() site pushes the mode, then resumes its body here). */
    BTL_MENU_BODY,        /* menu-loop iteration body (post dead-check) */
    BTL_EXEC_BODY,        /* execute_turns body (post dead-check) */
    BTL_TARGET_POST_BODY, /* post-action defeat checks (post dead-check) */
    BTL_AFTER_STATUS_BODY,/* post-action recovery body (post dead-check) */
    BTL_AFTER_TAIL_BODY,  /* re-show HP/PP windows (post dead-check) */
    BTL_ENDING_CLEANUP,   /* post-battle cleanup + fade-out (post mirror dead-check) */
} BattleRoutinePhase;

typedef struct {
    uint8_t  phase;             /* BattleRoutinePhase */
    uint8_t  announce_stage;    /* BTL_ANNOUNCE: which status text is next (0-2) */
    uint16_t battle_result;     /* 0 victory / 1 defeat / 2 special */
    uint16_t initiative_mode;   /* INITIATIVE_* (+ the run-away variants 3/4) */
    uint16_t turn_counter;
    uint16_t run_attempt;
    uint16_t post_battle_exit;
    uint16_t bg_id;             /* battle background config (debug loop can change) */
    uint16_t palette_id;
    uint16_t letterbox_style;
    uint16_t debug_party_flags; /* debug loop: party bitmask */
    uint16_t debug_enemy_level; /* debug loop: enemy level */
    uint16_t announce_i;        /* BTL_ANNOUNCE enemy index */
    uint16_t num_selected;      /* player menu: actions committed this turn */
    uint16_t party_slot;        /* player menu loop index */
    int16_t  attacker;          /* execute_turns: best-battler index */
    uint16_t target_i;          /* BTL_TARGET loop index */
    uint16_t retargeted;        /* mushroomized/strange retarget flag */
    uint16_t status_damage;     /* per-turn status damage carried across its text */
    uint16_t exp_i;             /* BTL_VICTORY_EXP loop index */
} BattleRoutineState;

/* GAME_MODE_INSTANT_WIN — run-to-completion port of instant_win_handler()
 * (battle.c, asm/battle/instant_win_handler.asm): the auto-victory sequence
 * when the party vastly outclasses the enemies. The former raw
 * wait_for_vblank loops become per-frame phases (the 7 one-frame palette
 * flashes, the 6-frame fade back from black); the "YOU WON" / item-drop
 * texts are DISPLAY_TEXT pushes via battle_push_text_ex (each resume phase
 * clears the prompt flag, i.e. the blocking epilogue); the per-battler
 * gain_exp(1, ...) calls run as gain_exp_prepare() inline + a
 * GAME_MODE_LEVEL_UP push (the same idiom as BTL_VICTORY_EXP). Always pops
 * 0. Pushed by GAME_MODE_BATTLE_ENTRY's instant-win branch. */
typedef enum {
    IW_BEGIN = 0,   /* music + stop the swirl, then into the flash frames */
    IW_FLASH,       /* 7 one-frame palette fills: green/red/blue x2, black */
    IW_FADE,        /* 6-frame palette fade from black back to the saved colors */
    IW_VICTORY,     /* money deposit, battler re-init, EXP split; "YOU WON" text */
    IW_EXP,         /* per-battler EXP loop: GAME_MODE_LEVEL_UP pushes */
    IW_DROP,        /* random item-drop roll; present-dropped text */
    IW_FINISH,      /* close windows, restore music, re-enable entities; pop */
} InstantWinPhase;

typedef struct {
    uint8_t phase;      /* InstantWinPhase */
    uint8_t flash_i;    /* IW_FLASH index into the 7-color flash sequence */
    uint8_t fade_i;     /* IW_FADE frames run (0 = setup pending) */
    uint8_t battler_i;  /* IW_EXP battler loop index */
} InstantWinState;

/* GAME_MODE_BATTLE_ENTRY — run-to-completion port of init_battle_overworld()
 * (battle.c, asm/battle/init_overworld.asm), the random/overworld encounter
 * entry/exit driver, with init_battle_common() (asm/battle/init_common.asm)
 * inlined around the GAME_MODE_BATTLE push. The debug exit-button busy-wait
 * is the BE_DEBUG_WAIT phase; an instant win pushes GAME_MODE_INSTANT_WIN;
 * otherwise the mosaic fade-out starts and GAME_MODE_BATTLE is pushed.
 * BE_BATTLE_DONE runs the shared post-battle tail (update_party + flags)
 * and the per-result handling; a post-battle PSI teleport STEP_PUSHes
 * GAME_MODE_TELEPORT (resume BE_RESET).
 *
 * Kept inline-blocking (documented deferral): reload_map()'s
 * force_blank/blank_screen one-shot vblank helpers.
 *
 * Always pops 0 (the blocking original returns void — a defeat result is
 * handled by the caller observing game state, not a return value). */
typedef enum {
    BE_ENTER = 0,     /* battle_mode gate, debug checks, instant win / battle */
    BE_DEBUG_WAIT,    /* debug_mode_number==2: wait for the B button, skip battle */
    BE_IW_DONE,       /* after GAME_MODE_INSTANT_WIN pops: clear battle_mode */
    BE_BATTLE_DONE,   /* after GAME_MODE_BATTLE pops: party update + map reload */
    BE_RESET,         /* reset_entities tail: collision/pathfinding/intangibility */
} BattleEntryPhase;

typedef struct {
    uint8_t phase;    /* BattleEntryPhase */
} BattleEntryState;

/* GAME_MODE_BATTLE_SCRIPTED — run-to-completion port of init_battle_scripted()
 * (battle.c, asm/battle/init_scripted.asm), the scripted/event-triggered
 * battle entry/exit driver, with init_battle_common() inlined around the
 * GAME_MODE_BATTLE push (same shape as GAME_MODE_BATTLE_ENTRY). BS_ENTER
 * parses the enemy group, starts the swirl, and pushes the BW_SWIRL_UPDATE
 * wait; BS_SWIRL_DONE starts the mosaic fade-out and pushes GAME_MODE_BATTLE;
 * BS_BATTLE_DONE runs the shared post-battle tail and per-result handling;
 * BS_CLEANUP/BS_FINISH split render_and_disable_entities() at its
 * render_frame_tick yield (work-then-yield, then the entity disable). A
 * post-battle PSI teleport STEP_PUSHes GAME_MODE_TELEPORT (resume BS_CLEANUP on
 * victory, BS_TELEPORT_DEFEATED to pop 1 on defeat).
 *
 * Kept inline-blocking (documented deferral): reload_map()'s one-shot vblank
 * helpers, as in GAME_MODE_BATTLE_ENTRY.
 *
 * Pops 0 = normal victory/post-battle, 1 = party defeated. Pushed by
 * CC_1F_23 TRIGGER_BATTLE (cc_1f_dispatch push-signal; the result is stored
 * to working memory in the DT_RESUME_CC1F_BATTLE handler). */
typedef enum {
    BS_ENTER = 0,     /* parse enemy group, start the swirl; push the swirl wait */
    BS_SWIRL_DONE,    /* mosaic fade-out; push GAME_MODE_BATTLE */
    BS_BATTLE_DONE,   /* post-battle: party update, teleport/reload handling */
    BS_CLEANUP,       /* render_and_disable front half: party + render work */
    BS_FINISH,        /* entity disable + intangibility frames; pop the result */
    BS_TELEPORT_DEFEATED, /* after a post-battle PSI teleport with the party defeated: pop 1 */
    BS_CLEANUP_FLUSH, /* D4b: resume after a parked actionscript frame popped (BS_CLEANUP) */
} BattleScriptedPhase;

typedef struct {
    uint8_t  phase;         /* BattleScriptedPhase */
    uint16_t battle_group;  /* input: BTL_ENTRY_PTR_TABLE index */
} BattleScriptedState;

/* GAME_MODE_BATTLE_ACTION — one battle-action function (the btlact_* /
 * battle_actions.c long tail), run-to-completion. The mode is generic: the
 * step dispatches to the action's resumable stepper via the `step` column of
 * btlact_dispatch_table (battle_actions.c); each converted action is a small
 * pc-machine whose texts are DISPLAY_TEXT pushes (battle_push_text_ex idiom —
 * the resume pc clears dt.blinking_triangle_flag, i.e. the blocking
 * epilogue). Actions without a stepper still run inline-blocking through
 * jump_temp_function_pointer(), which doubles as the pump bridge for
 * converted actions (so unconverted drivers and action→action calls keep
 * working unchanged).
 *
 * Pushed by GAME_MODE_BATTLE's BTL_TARGET (resume BTL_TARGET_POST), the PSI
 * menu's PS_EXEC_* phases and the use-item UI_EXEC_* phases (both text.c),
 * via battle_action_dispatch(). Always pops 0 (action functions return
 * nothing — their results flow through `bt`). */
typedef struct {
    uint8_t  pc;            /* per-action resume point (0 = entry) */
    uint8_t  exec_i;        /* generic loop counter for actions that need one */
    uint16_t table_index;   /* btlact_dispatch_table index (which action) */
    uint16_t scratch16[2];  /* per-action hoisted locals */
    uint32_t scratch32;     /* per-action hoisted 32-bit local */
} BattleActionState;

/* GAME_MODE_BATTLE_CALC — the battle_calc.c text-displaying calculation
 * pipeline (miss/SMAAAASH/damage/shield/sleep-wake texts), run-to-completion
 * as a VALUE-RETURNING child mode: the pop result is the blocking function's
 * return value, read back via mode_child_result(). One mode, one kind per
 * former blocking function; the kinds nest by pushing each other
 * (BC_SMAAAASH → BC_RESIST_DAMAGE → BC_CALC_DAMAGE → DISPLAY_TEXT), exactly
 * mirroring the blocking call tree. All RNG/mutation runs at its original
 * sequence point (decide at the pushing pc, never at a resume pc); every
 * resume pc starts with the dt.blinking_triangle_flag clear (the blocking
 * display_in_battle_text epilogue). The KO checks in BC_RESIST_DAMAGE are
 * GAME_MODE_BATTLE_KO child pushes (pcs 8/9 are their resume points).
 *
 * Pushed by the converted btlact_* action steppers (battle_actions.c) and by
 * its own nesting; the blocking battle_*() forms in battle_calc.c are
 * pump_mode bridges, so the ~100 unconverted blocking callers keep working
 * unchanged. Init via battle_calc_make_init() (battle_internal.h). */
typedef enum {
    BC_MISS_CALC = 0,       /* arg0 = miss_message_type; pops 1 = missed */
    BC_SMAAAASH,            /* pops 1 = critical hit happened */
    BC_CALC_DAMAGE,         /* arg0 = target offset, arg1 = damage; pops 1 (0 = "didn't work") */
    BC_RESIST_DAMAGE,       /* arg0 = damage, arg1 = resist modifier; pops final damage */
    BC_PSI_SHIELD_NULLIFY,  /* pops 1 = attack nullified (absorbed) */
    BC_WEAKEN_SHIELD,       /* pops 0 */
    BC_HEAL_STRANGENESS,    /* pops 0 */
    BC_FAIL_ON_NPCS,        /* pops 1 = target is an NPC (attack fails) */
} BattleCalcKind;

typedef struct {
    uint8_t  kind;          /* BattleCalcKind */
    uint8_t  pc;            /* per-kind resume point (0 = entry) */
    uint16_t arg0, arg1;    /* inputs (see BattleCalcKind); arg0 doubles as the
                             * mutable working value (damage / target offset) */
    uint16_t local[2];      /* per-kind hoisted locals (flags / saved target /
                             * reflected damage) */
} BattleCalcState;

/* GAME_MODE_BATTLE_REVIVE — run-to-completion port of battle_revive_target()
 * (battle.c, asm/battle/revive_target.asm): the revive text (DISPLAY_TEXT
 * push), the affliction/HP writeback at its resume pc, and — for enemy
 * revives only — the palette flash (zero bank 12, fade to white, restore
 * from bank 8) whose two waits are BW_FRAMES pushes. Pushed by the
 * healing-γ/Ω and pray_rainbow action steppers (battle_actions.c) via
 * battle_revive_make_init() (battle_internal.h). Always pops 0. */
typedef struct {
    uint8_t  pc;      /* resume point (0 = entry) */
    uint16_t target;  /* battler offset (byte offset into bt.battlers_table) */
    uint16_t hp;      /* HP to revive with */
} BattleReviveState;

/* GAME_MODE_BATTLE_APPLY — run-to-completion port of apply_action_to_targets()
 * (battle.c, asm/battle/apply_action_to_targets.asm): wait for the PSI
 * animation (a BW_PSI_ANIM push), then iterate the targeted battlers —
 * enemies (8..31) first, then party (0..7) — running the action once per
 * target. Like the assembly, the action is a 24-bit ROM address written to
 * bt.temp_function_pointer per call (battle_action_dispatch): converted
 * actions run as BATTLE_ACTION child pushes, pure/unconverted ones inline.
 * action_addr 0 = iterate without calling (the assembly's NULL check).
 * bt.current_target walks the battler table exactly as in the blocking form
 * (set at each pass start, += sizeof(Battler) per advance — it is global
 * serialized state, so it survives the per-target yields). Pushed by the
 * pray / apply_neutralize_to_all action steppers and pumped by the
 * battle_ko_target final-attack path, via battle_apply_make_init()
 * (battle_internal.h). Always pops 0. */
typedef struct {
    uint8_t  pc;          /* 0 = PSI wait; 1 = pass start; 2 = loop head; 3 = post-action advance */
    uint8_t  party_pass;  /* 0 = enemy pass (8..31), 1 = party pass (0..7) */
    uint16_t index;       /* current battler index */
    uint32_t action_addr; /* per-target action's ROM address (0 = none) */
} BattleApplyState;

/* GAME_MODE_BATTLE_KO — run-to-completion port of battle_ko_target()
 * (battle.c, asm/battle/ko_target.asm): the battler death driver. Enemy
 * deaths run the final-attack bracket (description text push + a
 * BATTLE_APPLY child carrying the final action's ROM address; the saved
 * attacker/target/target-flags live in this state), the death text push,
 * the white-flash/black-fade palette animation (BW_FRAMES pushes), the
 * death_type group-death sequence, and the ghost-respawn logic; player/NPC
 * deaths run the possession handling and their own death/collapse texts.
 * Pushed by BC_RESIST_DAMAGE (battle_calc.c), the hp_sucker / PSI-flash
 * action steppers (battle_actions.c) and GAME_MODE_BATTLE's status-damage
 * phase, via battle_ko_make_init() (battle_internal.h). Always pops 0. */
typedef struct {
    uint8_t  pc;              /* resume point (0 = entry) */
    uint16_t target;          /* battler offset of the dying battler */
    uint16_t saved_attacker;  /* final-attack bracket: saved bt.current_attacker */
    uint16_t saved_target;    /* final-attack bracket: saved bt.current_target */
    uint32_t saved_flags;     /* final-attack bracket: saved bt.battler_target_flags */
} BattleKoState;

/* GAME_MODE_CHECK_DEAD_PLAYERS — run-to-completion port of check_dead_players()
 * (battle.c, asm/battle/check_dead_players.asm): syncs each party battler's HP/PP
 * from its char_struct and, when one has just dropped to 0 HP, marks it unconscious
 * and pushes the "X collapsed!" KO text as a DISPLAY_TEXT child (the blocking form
 * waited at that ▼). `i` is the party-loop cursor; `existing` remembers whether the
 * battle-text window was already open across the text push. Pushed at the six
 * check_dead_players() sites in GAME_MODE_BATTLE via check_dead_players_make_init()
 * (battle_internal.h). Always pops 0. */
typedef struct {
    uint8_t pc;        /* 0 = loop body, 1 = resume after a collapse's KO text */
    uint8_t i;         /* party-battler loop cursor */
    uint8_t existing;  /* was WINDOW::TEXT_BATTLE already open before this collapse */
} CheckDeadPlayersState;

/* GAME_MODE_LEVEL_UP phases. Port of the gain_exp() level-up loop +
 * LEVEL_UP_CHAR (asm/misc/gain_exp.asm lines 68-118 + asm/misc/
 * level_up_char.asm, 763 lines) for the text-displaying play_sound != 0 path:
 * per gained level — the level-up music, the "reached level X" text, the seven
 * stat growths, the max HP/PP increases (each gain pushing its DISPLAY_TEXT
 * message), the PSI-learn scan — then the next-threshold re-check, looping
 * while more levels are pending. Everything between two texts runs inside the
 * step's internal for(;;) with no extra yield. The silent play_sound == 0 path
 * (reset_char_level_one, silent gain_exp) never yields and stays the
 * synchronous level_up_char_silent() loop in inventory.c.
 *
 * Pushed by CC_1E_09 GIVE_EXPERIENCE (cc_1e_dispatch push-signal), by
 * GAME_MODE_BATTLE's end-of-round EXP loop (BTL_VICTORY_EXP), and by
 * GAME_MODE_INSTANT_WIN's EXP loop (IW_EXP). Always pops 0. */
typedef enum {
    LU_LEVEL = 0,   /* loop head: music, level++, push the "reached level" text */
    LU_STAT,        /* apply growth stage `stage`; push the gain text if any */
    LU_PSI,         /* PSI-learn scan from `psi_index`; push the learned text */
    LU_NEXT,        /* threshold re-check: another level or POP */
} LevelUpPhase;

typedef struct {
    uint8_t  phase;     /* LevelUpPhase */
    uint8_t  stage;     /* next LU_STAT growth stage (LU_STAGE_*, inventory.c) */
    uint8_t  psi_index; /* next PSI id for the LU_PSI scan */
    uint16_t char_id;   /* 1-based character (input) */
    uint16_t old_level; /* level before this iteration's increment */
} LevelUpState;

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
    uint8_t  flush;        /* resume code after an actionscript park (D4b): 1=lead
                            * window frame, 2=delay frame; 0=none */
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
    uint8_t flush;   /* resume code after an actionscript park (D4b): 1=AS_INIT
                      * window frame, 2=AS_RENDER frame; 0=none */
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
    uint8_t  flush;          /* resume code after an actionscript park (D4b):
                              * 1=TP_WAIT_PROMPT render, 3=tp_window_and_decide
                              * window, 4=TP_TEXTSPEED, 5=TP_WAIT_BUTTON,
                              * 6=TP_TRIANGLE; 0=none */
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
    SM_SETUP_RESUME, /* finish SM_SETUP after a deferred cursor-callback text push */
    SM_MOVE_RESUME,  /* finish a cursor move after a deferred cursor-callback text push */
} SelectionMenuPhase;

typedef struct {
    uint8_t  phase;         /* SelectionMenuPhase */
    uint8_t  allow_cancel;  /* selection_menu() arg */
    uint8_t  primed;        /* SM_MAIN: 1 = read input this frame, 0 = render only */
    uint8_t  redraw_cursor; /* toggle + rewrite the cursor tiles this frame */
    uint8_t  cursor_frame;  /* blink sub-frame (0/1) */
    uint8_t  menu_window;   /* focus window id captured at SM_SETUP; used to restore
                             * focus on SM_*_RESUME before the menu-less-window early-out
                             * (a deferred cursor-callback text push moves focus away) */
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
    SS_RTC_FLUSH,  /* resume after a parked force/blank frame: flush, -> resume_phase */
} SoundStonePhase;

typedef struct {
    uint8_t  phase;          /* SoundStonePhase */
    uint8_t  cancellable;    /* use_sound_stone() arg: A/B/X cancels early */
    uint8_t  resume_phase;   /* SoundStonePhase SS_RTC_FLUSH returns to */
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
 * goods is its own GAME_MODE_DEBUG_GOODS — its A action's char_select_prompt(mode
 * 1) is now a STEP_PUSH of SELECTION_MENU via char_select_overworld_prepare.) */
typedef enum {
    DBG_YMENU_FLAG = 0,  /* event flag editor */
    DBG_YMENU_GUIDE,     /* active-script entity counter (draw once, wait for cancel) */
} DebugYMenuKind;

typedef enum {
    DY_DRAW = 0,  /* (re)draw the window via window_tick_work, then yield */
    DY_INPUT,     /* read input; FLAG: nav/toggle/cancel; GUIDE: wait for cancel */
    DY_DRAW_FLUSH, /* park-propagating resume of DY_DRAW's window_tick frame */
} DebugYMenuPhase;

typedef struct {
    uint8_t  phase;  /* DebugYMenuPhase */
    uint8_t  kind;   /* DebugYMenuKind */
    uint16_t index;  /* FLAG: current flag index (1-1999) */
} DebugYMenuState;

/* GAME_MODE_DEBUG_GOODS — run-to-completion port of debug_y_button_goods
 * (game_main.c), the debug Y-button "Goods" item browser/giver. The blocking
 * form was a raw for(;;){...wait_for_vblank();...} loop with an inline
 * char_select_prompt(mode 1) — the last non-mode debug driver. D-pad browses
 * item ids (±1 held up/down, ±10 left/right), A gives the item to a selected
 * party member (auto-equips weapons/armor), B exits. The A path STEP_PUSHes
 * SELECTION_MENU exactly as the determine-targetting ally pick does
 * (char_select_overworld_prepare/finish bracket the push). */
typedef enum {
    DG_DRAW = 0,       /* (re)draw the item id + name window, then yield */
    DG_INPUT,          /* read input; nav / A (give) / B (exit) */
    DG_GIVE_RESULT,    /* the char-select SELECTION_MENU popped: give/equip or redraw */
    DG_DRAW_FLUSH,     /* park-propagating resume of DG_DRAW's window_tick frame */
} DebugGoodsPhase;

typedef struct {
    uint8_t  phase;                  /* DebugGoodsPhase */
    uint16_t item_id;                /* current item id (0-255) */
    uint16_t give_window_id;         /* char-select window id to close in DG_GIVE_RESULT */
    uint32_t saved_argument_memory;  /* focus window argument_memory across the push */
} DebugGoodsState;

/* GAME_MODE_DEBUG_MENU — run-to-completion port of debug_y_button_menu
 * (game_main.c), the debug Y-button parent menu (hold B/SELECT + R in the
 * overworld with ow.debug_flag set). The blocking form was a `display_menu:`-goto
 * loop: build a 23-item phone menu, selection_menu(1), a 23-case dispatch, an
 * @AFTER_COMMAND message-display loop, then a @CLEANUP fade wait. Each phase here
 * matches an asm sequence point; every blocking child driver becomes a STEP_PUSH.
 * Commands that still block via wait_for_vblank but never pump (Warp/CAST/STAFF) or
 * are synchronous (Save/learn_special_psi/Meter) run inline within DM_DISPATCH;
 * the deep pump bridges they cannot yet avoid (enter_your_name_please naming,
 * debug_teleport after CAST/STAFF) stay inline this commit — D4b converts them. */
typedef enum {
    DM_ENTER = 0,  /* one-shot: disable entities, SFX, show HP/PP windows */
    DM_BUILD,      /* (re)build the 23-item menu window, push SELECTION_MENU */
    DM_DISPATCH,   /* the menu popped: 23-case command dispatch */
    DM_AFTER,      /* @AFTER_COMMAND: optional message text, then rebuild the menu */
    DM_ENDING_TELEPORT, /* after CAST/STAFF ending: push TELEPORT_TO(dest 1), resume DM_AFTER */
    DM_CLEANUP,    /* close windows + hide HP/PP, push ENTITY_FADE_WAIT */
    DM_DONE,       /* re-enable entities, POP 0 */
} DebugMenuPhase;

typedef struct {
    uint8_t  phase;         /* DebugMenuPhase */
    uint32_t message_addr;  /* @AFTER_COMMAND text addr (0 = none); DM_DISPATCH→DM_AFTER */
} DebugMenuState;

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
 *                      (GAME_MODE_BATTLE_SCRIPTED). The blocking loop yields BEFORE the
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

/* GAME_MODE_LOAD_BATTLE_SCENE — run-to-completion port of load_battle_scene()
 * (battle_ui.c): (re)loads the battle scene during boss transitions. The three
 * blocking waits become STEP_PUSHes: the swirl-in / swirl-out are
 * GAME_MODE_BATTLE_WAIT (BW_SWIRL_WINDOW), the fade-in is GAME_MODE_FADE_WAIT
 * (FADE_TICK_WINDOW = the former wait_for_fade_with_tick()). The interleaved
 * force_blank/blank-screen vblank waits block via host_process_frame only
 * (pump-free) and run inline in the LBS_LOAD step. Init via group/music (phase =
 * LBS_ENTER); skip_swirl is computed in LBS_ENTER. Always pops 0. */
typedef enum {
    LBS_ENTER = 0,  /* compute skip_swirl; swirl-in push (or continue) */
    LBS_LOAD,       /* load visuals (inline) + fade-in / swirl-out push */
    LBS_DONE,       /* pop */
} LoadBattleScenePhase;

typedef struct {
    uint8_t  phase;       /* LoadBattleScenePhase */
    uint8_t  skip_swirl;  /* captured in LBS_ENTER */
    uint16_t group;       /* enemy group index */
    uint16_t music;       /* music track (0 = unchanged) */
} LoadBattleSceneState;

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
    uint8_t  flush;        /* D4b: resume code after a parked actionscript frame
                            * (1 = targeting render -> BR_PRIME, 2 = hppp -> BR_INPUT) */
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
    uint8_t  flush;         /* D4b: resume code after a parked actionscript frame
                             * (1 = targeting -> ET_PRIME, 2 = hppp -> ET_INPUT,
                             *  3 = targeting -> ET_DISPLAY) */
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
    uint8_t  flush;        /* D4b: resume code after a parked actionscript frame
                            * (1 = NE_WAIT_PENDING, 2 = NE_WAIT_SCRIPTS) */
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
    uint8_t  flush;                  /* D4b: 1 = resume after a parked actionscript frame */
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
    uint8_t  flush;          /* D4b: 1 = resume after a parked actionscript frame */
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
    ST_EXIT_BODY_FLUSH, /* resume after a parked actionscript frame popped (ST_EXIT_BODY) */
    ST_ENTER_BODY_FLUSH,/* resume after a parked actionscript frame popped (ST_ENTER_BODY) */
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
    FOP_RTC_FLUSH,   /* resume after a parked force/blank frame: flush, -> resume_phase */
    FOP_CT_INIT_BODY, /* flyover_init_screen body (VRAM build) + blank-screen frame */
    FOP_CT_SETUP_A2,  /* oam_clear + load_background_animation start force-blank frame */
} FlyoverPhase;

typedef struct {
    uint8_t  kind;                /* FlyoverKind */
    uint8_t  phase;               /* FlyoverPhase */
    uint8_t  sub;                 /* opcode 0x09 sub-state */
    uint8_t  fade_primed;         /* FOP_CT_FADEWAIT: work-after-yield flag */
    uint8_t  resume_phase;        /* FlyoverPhase FOP_RTC_FLUSH returns to */
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
    GS_PH4_FLUSH,   /* resume after a parked actionscript frame popped (GS_PH4) */
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
    uint8_t  flush;           /* D4b: resume code after a parked actionscript frame
                               * (1 = TS_WARMUP render, 2 = TS_INPUT render) */
    uint16_t frame;           /* TS_WARMUP: warm-up frame counter */
} TitleScreenState;

/* GAME_MODE_ATTRACT — run-to-completion port of run_attract_mode() (attract_mode.c),
 * an idle title-screen demo scene. The one-shot setup runs inline in
 * run_attract_mode_prepare() (called by the init_intro parent before the push);
 * the scene-driving DISPLAY_TEXT and the three post-script loops live here:
 *
 *   AT_SCRIPT     - STEP_PUSH GAME_MODE_DISPLAY_TEXT with the scene's attract-mode
 *                   bytecode (attract_mode_text_addrs[scene_index]) — the script
 *                   sets flags, teleports, spawns entities, and pauses for the scene
 *                   duration. On its POP -> AT_MAIN. (Replaces the former blocking
 *                   display_text_from_addr() in run_attract_mode().)
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
    AT_SCRIPT = 0,
    AT_MAIN,
    AT_OVAL_CLOSE,
    AT_FADEOUT,
} AttractPhase;

typedef struct {
    uint8_t  phase;          /* AttractPhase */
    uint8_t  button_pressed; /* result: a button ended the scene */
    uint8_t  flush;          /* D4b: resume code after a parked actionscript frame
                              * (1 = AT_MAIN, 2 = AT_OVAL_CLOSE, 3 = AT_FADEOUT) */
    uint16_t loop_frame;     /* AT_MAIN: frame counter (TM override + timeout) */
    uint16_t scene_index;    /* AT_SCRIPT: which attract scene script to run */
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
    FM_NG_NAMING,       /* new-game: push GAME_MODE_NEW_GAME_NAMING */
    FM_NG_NAMING_RESULT,/* naming popped: started (pop 1) or backed out (re-enter flavour) */
} FileMenuPhase;

typedef struct {
    uint8_t  phase;         /* FileMenuPhase */
    uint8_t  result_ready;  /* 1 = `result` holds an inline early-exit value (no child was pushed) */
    uint16_t selected;      /* chosen slot, 1-based (file_select_menu result) */
    uint16_t result;        /* inline early-exit result for the *_RESULT phase */
} FileMenuState;

/* GAME_MODE_NEW_GAME_NAMING — run-to-completion port of new_game_naming()
 * (file_select.c), the new-game character/pet/food/thing naming flow reached from
 * the file-select new-game cascade (FM_NG_NAMING). Folds in the former blocking
 * helpers name_a_character() and init_naming_screen_events(): each character is a
 * three-push sequence — NAMING_PROMPT (render name box, wait for a button) ->
 * TEXT_INPUT (the on-screen keyboard) -> NAMING_EVENTS (walk-out animation wait).
 * The naming loop advances/retreats on the keyboard result; the confirmation
 * screen pushes SELECTION_MENU and, on Yep, counts a 180-frame settle before the
 * synchronous game-state initialisation. Nope restarts from NGN_ENTER (the
 * original recursion). All yield-free setup/finalize runs inline at the phase
 * boundaries.
 *
 * Pops 1 when the game is started (overworld follows), 0 when the player backs
 * out past the first character (the parent re-enters flavour selection). */
typedef enum {
    NGN_ENTER = 0,      /* one-shot setup (music/asset/entity init); -> NGN_LOOP_HEAD */
    NGN_LOOP_HEAD,      /* per-character: spawn sprites, build name box+prompt, push NAMING_PROMPT */
    NGN_PROMPT_DONE,    /* prompt button pressed: build keyboard, push TEXT_INPUT */
    NGN_INPUT_DONE,     /* keyboard done: record result, push NAMING_EVENTS (walk-out) */
    NGN_EVENTS_DONE,    /* walk-out done: advance / retreat / back out */
    NGN_CONFIRM_BUILD,  /* fill defaults, build confirmation windows, push SELECTION_MENU */
    NGN_CONFIRM_DONE,   /* Nope -> restart; Yep -> change music + settle */
    NGN_CONFIRM_WAIT,   /* 180-frame confirmation-animation settle */
    NGN_FINALIZE,       /* initialise character stats/position/prologue, POP 1 */
} NewGameNamingPhase;

typedef struct {
    uint8_t  phase;        /* NewGameNamingPhase */
    int8_t   char_i;       /* current naming index (0..THINGS_NAMED_COUNT) */
    int16_t  char_result;  /* keyboard result for the current character: 0 confirm / -1 cancel */
    uint16_t wait_frames;  /* NGN_CONFIRM_WAIT countdown */
} NewGameNamingState;

/* GAME_MODE_SPECIAL_EVENT — run-to-completion port of dispatch_special_event()
 * (display_text_menus.c), the 18-case dispatcher behind CC_1F_41. Most cases run
 * inline with no yield (status suppression, flag clears, homesickness, bicycle
 * dismount) or block only via host_process_frame (the cast/credits cutscenes);
 * those compute their result and POP in SE_ENTER. The five modal cases STEP_PUSH
 * an existing child — coffeetea (FLYOVER), the M2/EB name prompt (ENTER_NAME),
 * the town map (TOWN_MAP), the sound stone (SOUND_STONE) and the title screen
 * (TITLE_SCREEN) — and resume in SE_RESULT (POP the precomputed return value) or
 * SE_RESULT_CHILD (POP the child's result, for the name prompt). The CC_1F_41
 * channel stores the popped value to working memory in DT_RESUME_CC1F_SPECIAL_EVENT. */
typedef enum {
    SE_ENTER = 0,       /* dispatch on event_id: inline-POP or set up a child push */
    SE_RESULT,          /* child popped: POP the precomputed `result` */
    SE_RESULT_CHILD,    /* child popped: POP mode_child_result() (ENTER_NAME) */
} SpecialEventPhase;

typedef struct {
    uint8_t  phase;     /* SpecialEventPhase */
    uint8_t  event_id;  /* CC_1F_41 argument byte (1..18) */
    uint16_t result;    /* precomputed return value carried across a child push */
} SpecialEventState;

/* GAME_MODE_ENTER_NAME — run-to-completion port of enter_your_name_please()
 * (display_text_menus.c), the M2/EB player-name registry prompt (CC_1F_41 cases
 * 3/4, also the debug menu's Player 0/1). EN_ENTER sets the text flags, creates
 * WINDOW_NAMING_PROMPT, prints the prompt / existing name, then text_input_prepare()
 * + STEP_PUSH TEXT_INPUT. EN_DONE reads the keyboard result (param-0 also copies
 * the EB name into the M2 name), closes the window, restores the flags, POPs the
 * result. */
typedef enum {
    EN_ENTER = 0,       /* set up prompt window + push TEXT_INPUT */
    EN_DONE,            /* keyboard done: finalize + POP the result */
} EnterNamePhase;

typedef struct {
    uint8_t  phase;     /* EnterNamePhase */
    uint8_t  param;     /* 0 = EarthBound name path, 1 = Mother 2 name path */
    uint16_t result;    /* keyboard result, captured on resume */
} EnterNameState;

/* GAME_MODE_INIT_INTRO — run-to-completion port of init_intro()'s state machine
 * (init_intro.c). STEP_PUSHes the converted intro leaves (INTRO_LOGO, GAS_STATION,
 * TITLE_SCREEN), ATTRACT, and FILE_MENU as children, branching on
 * mode_child_result(). The yield-free transitions (change_music,
 * fade_out_if_visible, the PPU cleanup, the per-scene attract setup via
 * run_attract_mode_prepare(), the file-menu setup via file_menu_setup(), the
 * post-file-menu cleanup) run inline at the phase boundaries. The one-shot
 * init_intro() setup runs in the init_intro() entry. The intro is now STEP_PUSH
 * end-to-end (no inline pump bridges remain). */
typedef enum {
    II_LOGO = 0,        /* push INTRO_LOGO */
    II_LOGO_RESULT,
    II_GAS,             /* change_music(GAS_STATION); push GAS_STATION */
    II_GAS_RESULT,
    II_TITLE,           /* change_music(TITLE_SCREEN); title setup; push TITLE_SCREEN */
    II_TITLE_RESULT,
    II_ATTRACT,         /* per-scene setup (run_attract_mode_prepare); push ATTRACT */
    II_ATTRACT_RESULT,  /* button -> file menu; else next scene or back to title */
    II_FILE_MENU,       /* exit cleanup; change_music(SETUP); setup; push FILE_MENU */
    II_FILE_MENU_POST,  /* post-file-menu cleanup (clear_instant_printing/window_tick_work) */
    II_FILE_MENU_DONE,  /* clear disabled_transitions; free title script data; pop */
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
    DT_DELAY_FLUSH, /* resume after a typewriter-frame actionscript park: finish the
                     * window_tick_work render (update_screen) + count the frame */
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
    DT_RESUME_CC1A_TELEPORT,    /* CC_1A_0B teleport menu: store TELEPORT_MENU result */
    DT_RESUME_CC1F_BATTLE,      /* CC_1F_23 trigger battle: store BATTLE_SCRIPTED result */
    DT_RESUME_CC1F_PHOTO,       /* CC_1F_D2 photographer: save_photo_state(cc1f_aux) */
    DT_RESUME_CC1F_SPECIAL_EVENT, /* CC_1F_41: store SPECIAL_EVENT result to working_memory */
    DT_RESUME_CC1A_SEL,         /* CC_1A_08/09 selection menu: store result to working_memory */
    DT_RESUME_CC1A_SEL_CLEAR,   /* CC_1A_04 selection menu: store result + clear focus menu */
    DT_RESUME_CC18_SEL,         /* CC_18_08 selection menu: store result + cancel-jump on 0 */
    DT_RESUME_CC18_SEL_RESTORE, /* CC_18_09 selection menu: restore text attrs + store result */
    DT_RESUME_MENU_RESULT,      /* STORE/ESCARGO/TELEPHONE menu: store result to working_memory */
} DisplayTextResume;

typedef struct {
    uint8_t      phase;            /* DisplayTextPhase */
    uint8_t      saved_cc18_attrs; /* this call level's saved g_cc18_attrs_saved */
    uint8_t      resume;           /* DisplayTextResume: post-child work pending on POP */
    uint16_t     delay_remaining;  /* DT_DELAY: window_tick_work frames left */
    uint16_t     cc1f_aux;         /* small per-resume carry (DT_RESUME_CC1F_PHOTO: photo_id) */
    uint16_t     cc1a_window_id;   /* DT_RESUME_CC1A_PARTY_SEL: window to close on POP */
    uint32_t     cc1a_saved_argmem;/* DT_RESUME_CC1A_PARTY_SEL: argument_memory to restore */
    uint32_t     cc18_cancel_target;/* DT_RESUME_CC18_SEL: CC_18_08 cancel jump target */
    ScriptReader reader;           /* offset-based script cursor (serializable) */
} DisplayTextModeState;  /* note: DisplayTextState (display_text.h) is the `dt` global type */

/* GAME_MODE_ACTIONSCRIPT_FRAME — finish an interrupted run_actionscript_frame()
 * (script.c). The entity-script interpreter is already run-to-completion per
 * frame; its only yields were callroutines that ran a child modal context
 * inline-blocking: MOVEMENT_DISPLAY_TEXT (DISPLAY_TEXT), the two
 * FADE_OUT_WITH_MOSAIC wrappers (MOSAIC_FADE) and PLAY_FLYOVER_SCRIPT (FLYOVER).
 * Those callroutines now record a yield REQUEST (transient — set and consumed
 * within one frame's work, never crossing a yield) and abort the interpreter
 * loops; run_actionscript_frame() parks the frame position here and the step
 * pushes the requested child, then on POP runs the callroutine's post-child
 * epilogue at its original sequence point and finishes the interrupted frame:
 * the suspended script (from `pc`), the rest of that entity's script chain +
 * tick callback, the remaining phase-1 entities, then the movement/draw phases.
 * A resumed script can immediately request another child (two texts back to
 * back) — the step refills from the new request and pushes it in the same step.
 *
 * The iteration cursors live in the already-serialized globals exactly as the
 * blocking loops used them (ert.next_active_entity / ert.actionscript_current_
 * script are deliberately re-read after each script so child-side entity/script
 * frees retarget the iteration). Only the former C locals are hoisted here; in
 * particular `pc` mirrors the blocking form's local pc — scripts.pc[] is NOT
 * written at the interrupt point, and the loop-exit writeback overwrites any
 * child-side rewrite, exactly like the blocking original.
 * ert.disable_actionscript stays 1 across the child (the blocking pump ran
 * under the same guard); the frame tail clears it. Always pops 0. */
typedef enum {
    ASF_PUSH = 0,   /* push the pending child mode (child_kind) */
    ASF_RESUME,     /* epilogue + finish the interrupted frame */
} ActionscriptFramePhase;

typedef enum {
    AS_CHILD_NONE = 0,
    AS_CHILD_TEXT,        /* GAME_MODE_DISPLAY_TEXT  (cr_movement_display_text) */
    AS_CHILD_MOSAIC,      /* GAME_MODE_MOSAIC_FADE   (FADE_OUT_WITH_MOSAIC, MF_OUT+final_hdma) */
    AS_CHILD_FLYOVER,     /* GAME_MODE_FLYOVER       (cr_play_flyover_script, FO_SCRIPT) */
    AS_CHILD_PP_RECOVERY, /* GAME_MODE_PP_RECOVERY_FLASH (INSTANT_WIN_PP_RECOVERY) */
} AsChildKind;

typedef enum {
    AS_EPI_NONE = 0,
    AS_EPI_TEXT_FLAG,  /* event_flag_set(2) — text-done flag, after the text pops */
} AsEpilogue;

/* GAME_MODE_PP_RECOVERY_FLASH — run-to-completion port of
 * INSTANT_WIN_PP_RECOVERY (battle.c, asm/battle/instant_win_pp_recovery.asm),
 * the event-script callroutine run after an instant-win battle: two purple
 * screen flashes (fill palettes purple, 12-frame fade back to the saved
 * colors), then +20 PP for Ness/Paula/Poo. Pushed by
 * GAME_MODE_ACTIONSCRIPT_FRAME (AS_CHILD_PP_RECOVERY); the recovery SFX plays
 * at the callroutine's request point. Each step: when a 12-frame fade has
 * completed, finalize it (and on the second flash, apply the PP recovery and
 * POP — blocking bundled the finalize with the NEXT frame's work, so the
 * finalize runs at the top of the following step, not with the 12th update);
 * a fresh flash saves the palettes / fills purple / preps the slopes, then
 * every step runs one update_map_palette_animation(). Always pops 0. */
typedef struct {
    uint8_t flash_i;  /* completed flashes (0-1) */
    uint8_t frame_i;  /* fade frames run for the current flash (0-12) */
} PpRecoveryFlashState;

typedef struct {
    uint8_t  phase;            /* ActionscriptFramePhase */
    uint8_t  child_kind;       /* AsChildKind: pending child to push at ASF_PUSH */
    uint8_t  epilogue;         /* AsEpilogue: post-child work before resuming */
    int16_t  script_offset;    /* interrupted script slot */
    int16_t  entity_offset;    /* interrupted entity (chain/tick continuation) */
    uint16_t pc;               /* the interrupted script's continuation pc */
    uint32_t text_addr;        /* AS_CHILD_TEXT: text address */
    uint8_t  mf_step;          /* AS_CHILD_MOSAIC: brightness step */
    uint8_t  mf_mosaic_bgs;    /* AS_CHILD_MOSAIC: mosaic BG mask (low nibble) */
    uint16_t mf_delay;         /* AS_CHILD_MOSAIC: yields between steps */
    uint16_t fo_id;            /* AS_CHILD_FLYOVER: flyover id 0-7 */
    uint16_t fo_saved_tick_hi; /* AS_CHILD_FLYOVER: entity 23 tick_callback_hi to restore */
    uint32_t fo_script_size;   /* AS_CHILD_FLYOVER: script byte length */
} ActionscriptFrameState;

/* GAME_MODE_TELEPORT — run-to-completion port of teleport_mainloop()
 * (overworld_teleport.c, asm/misc/teleport_mainloop.asm): the PSI-teleport
 * driver. Stops music + one wait (TP_BEGIN), then the synchronous style setup
 * (freeze entities, tick-callback assignment, teleport music) in TP_SETUP, then
 * the per-frame animation loop (TP_LOOP: oam_clear + run_actionscript_frame +
 * teleport_freeze_entities_conditional + update_screen, one yield) until
 * ow.psi_teleport_state leaves 0. State 1 (arrived) runs the arrival load across
 * TP_ARRIVE/TP_ARRIVE_DEST/TP_ARRIVE_DONE, STEP_PUSHing GAME_MODE_FADE_WAIT for the
 * single fade-wait (formerly run_frames_until_fade_done — arrival's for non-INSTANT,
 * INSTANT departure's). The non-INSTANT departure animation (init_teleport_departure.asm)
 * is now run-to-completion across TP_DEPART_SETUP/_WAIT/_ANIM (formerly the blocking
 * init_teleport_departure_run): synchronous party/speed/callback/music setup, a 30-frame
 * bare wait (no script advance, so the decelerate tick is frozen), then an animate-one-
 * frame-per-yield loop until ow.psi_teleport_speed reaches 0. State 2 (failed) runs the
 * 180-frame charred-status failure sequence (TP_FAIL_WAIT) plus a 10-frame settle
 * (TP_FAIL_SETTLE); TP_CLEANUP
 * restores the normal tick callbacks and clears teleport state. All the live
 * teleport state already lives in serialized globals (ow.psi_teleport_*,
 * game_state.party_status); only the failure-loop frame counter is hoisted.
 *
 * Faithfulness note: the assembly loop body renders ONCE per yield
 * (WAIT_UNTIL_NEXT_FRAME is a bare wait). The prior blocking C port called
 * render_frame_tick() AFTER the inlined body in the main loop (and after
 * stop_music), double-rendering the actionscript frame each iteration (~2x
 * animation speed). This port matches the assembly: a single render per yield.
 *
 * teleport_mainloop() (overworld_teleport.c) is the pump bridge for its three
 * still-blocking callers (the overworld root in game_main.c, and the
 * post-battle teleport checks in GAME_MODE_BATTLE_ENTRY / _SCRIPTED); they
 * STEP_PUSH at Phase D. Always pops 0. */
typedef enum {
    TP_BEGIN = 0,    /* stop_music, one wait */
    TP_SETUP,        /* freeze + clears + tick-callback setup + music (synchronous) */
    TP_LOOP,         /* animation loop: render one frame until state != 0 */
    TP_ARRIVE,       /* state 1: init_teleport_arrival_setup; STEP_PUSH fade-wait if pending */
    TP_ARRIVE_DEST,  /* load destination; INSTANT center+fade (STEP_PUSH fade-wait), else TP_DEPART_SETUP */
    TP_ARRIVE_DONE,  /* STAR_MASTER learn-text queue, then cleanup */
    TP_FAIL_WAIT,    /* failure: 180-frame charred-status render loop */
    TP_FAIL_SETTLE,  /* failure: wait_frames_with_updates(10) */
    TP_CLEANUP,      /* restore callbacks, reset entities, clear state, POP */
    TP_LOOP_FLUSH,        /* resume after a parked actionscript frame popped (TP_LOOP) */
    TP_FAIL_SETTLE_FLUSH, /* resume after a parked actionscript frame popped (TP_FAIL_SETTLE) */
    TP_FAIL_WAIT_FLUSH,   /* resume after a parked actionscript frame popped (TP_FAIL_WAIT) */
    TP_DEPART_SETUP,      /* non-INSTANT departure: synchronous party/speed/callback/music setup */
    TP_DEPART_WAIT,       /* non-INSTANT departure: 30-frame bare wait (no script advance) */
    TP_DEPART_ANIM,       /* non-INSTANT departure: animate one frame until speed reaches 0 */
    TP_DEPART_ANIM_FLUSH, /* resume after a parked actionscript frame popped (TP_DEPART_ANIM) */
} TeleportPhase;

typedef struct {
    uint8_t  phase;     /* TeleportPhase */
    uint16_t frame_i;   /* TP_FAIL_WAIT / TP_FAIL_SETTLE frame counter */
} TeleportState;

/* GAME_MODE_BICYCLE_DISMOUNT — run-to-completion port of
 * get_off_bicycle_with_message() (overworld_teleport.c,
 * asm/overworld/get_off_bicycle.asm): show the "got off the bicycle" message,
 * then dismount. BD_TEXT creates the standard text window and STEP_PUSHes the
 * message (DISPLAY_TEXT); BD_CLOSE closes the window + one window_tick frame;
 * BD_DISMOUNT calls dismount_bicycle() and POPs. The caller (the overworld
 * root) brackets the call with disable/enable_all_entities, unchanged.
 * get_off_bicycle_with_message() is the pump bridge; the root STEP_PUSHes at
 * Phase D. Always pops 0. */
typedef enum {
    BD_TEXT = 0,   /* create window + push the "got off the bicycle" text */
    BD_CLOSE,      /* close focus window + one window_tick frame */
    BD_DISMOUNT,   /* dismount_bicycle, POP */
    BD_CLOSE_FLUSH, /* park-propagating resume of BD_CLOSE's window_tick frame */
} BicycleDismountPhase;

typedef struct {
    uint8_t phase;  /* BicycleDismountPhase */
} BicycleDismountState;

/* GAME_MODE_HP_ALERT — run-to-completion port of SHOW_HP_ALERT
 * (asm/overworld/show_hp_alert.asm), the "[name]'s HP is very low!" overworld
 * warning shown by check_low_hp_alert() (overworld_palette.c) when a party
 * member drops below 20% HP. HA_TEXT disables entities, opens the text window,
 * sets the battler name, and STEP_PUSHes the warning (DISPLAY_TEXT); HA_CLOSE
 * closes the window + one window_tick frame; HA_DONE re-enables entities and
 * POPs. party_index selects whose name/HP triggered it (re-derived at HA_TEXT).
 * Pumped by check_low_hp_alert() (still called inline-blocking mid-loop from
 * update_overworld_damage, a per-frame root function — it STEP_PUSHes at
 * Phase D). Always pops 0. */
typedef enum {
    HA_TEXT = 0,   /* disable entities, window + battler name, push the warning */
    HA_CLOSE,      /* close focus window + one window_tick frame */
    HA_DONE,       /* enable entities, POP */
    HA_CLOSE_FLUSH, /* park-propagating resume of HA_CLOSE's window_tick frame */
} HpAlertPhase;

typedef struct {
    uint8_t phase;        /* HpAlertPhase */
    uint8_t party_index;  /* 0-based party slot whose name + HP triggered the alert */
} HpAlertState;

/* GAME_MODE_GAME_OVER — run-to-completion port of spawn() (asm/overworld/spawn.asm)
 * with initialize_game_over_screen() and play_comeback_sequence() inlined as
 * phases. Shown when the whole party is KO'd in the overworld. The former blocking
 * yields become child pushes / CONTINUEs:
 *   - the game-over screen's fade-out/fade-in (wait_for_fade_complete) -> FADE_WAIT
 *     (FADE_TICK_SCREEN_ONLY)
 *   - the comeback dialogue (MSG_SYS_REVIVE_AFTER_KO) and the buzz-buzz check text
 *     (MSG_EVT0_BUZZBUZZ_CHECK) -> DISPLAY_TEXT pushes
 *   - the skippable pauses and the four map-palette fade phases -> PALETTE_FADE
 *     (PF_SKIPPABLE_PAUSE / PF_MAP_CHANGE) pushes; a skip (-1) short-circuits the
 *     no-continue fade chain straight to the world reinit (the assembly returns 0)
 *   - fade_palette_to_white / animate_palette_fade_with_rendering -> PALETTE_FADE
 *     (PF_TO_WHITE / PF_WITH_RENDERING) pushes
 *   - the pre-map wait_for_vblank() -> one CONTINUE
 * spawn_buzz_buzz() is decomposed at its call site exactly like the door
 * DTR_FINALIZE/DTR_BUZZ_DONE path: push the buzz-buzz check text, then run
 * spawn_delivery_entities() on resume.
 *
 * Pops the comeback result: -1 = player chose "Continue" (overworld_post re-renders
 * the same map), 0 = "No Continue" (the world was reinitialised at the respawn
 * point; overworld_post re-boots). spawn() (overworld_palette.c) is the pump bridge
 * for the still-blocking overworld root loop caller; the root STEP_PUSHes at D1. */
typedef enum {
    GO_ENTER = 0,        /* save respawn, disable entities, game-over music + fade-out */
    GO_SETUP,            /* game-over screen sync setup + fade-in */
    GO_CB_PAUSE,         /* comeback: initial skippable_pause(60) (result ignored) */
    GO_CB_TEXT,          /* comeback: push the MSG_SYS_REVIVE_AFTER_KO dialogue */
    GO_CB_CLOSE1,        /* close_all_windows + one window_tick frame */
    GO_CB_CLOSE2,        /* hide_hppp_windows + one window_tick frame */
    GO_CB_DECIDE,        /* branch on the No-Continue flag */
    GO_CONT_FADE,        /* Continue: after the trailing pause, fade_out(2,1) */
    GO_CONT_DONE,        /* Continue: enable entities, POP -1 */
    GO_NC_SEQ,           /* No-Continue: push the next pause/anim in the fade chain */
    GO_NC_SEQ_CHECK,     /* No-Continue: skip -> tail, else advance the chain */
    GO_NC_WHITE,         /* No-Continue tail: fade_palette_to_white(32) */
    GO_NC_REINIT,        /* No-Continue: audio/PPU reset, then a pre-map vblank wait */
    GO_NC_MAP,           /* No-Continue: initialize_map, leader/flags/entity reset, buzz text */
    GO_NC_BUZZ_DONE,     /* No-Continue: spawn deliveries, enable entities, render-fade */
    GO_NC_FINISH,        /* No-Continue: POP 0 */
    GO_CB_CLOSE1_FLUSH,  /* park-propagating resume of GO_CB_CLOSE1's window_tick */
    GO_CB_CLOSE2_FLUSH,  /* park-propagating resume of GO_CB_CLOSE2's window_tick */
} GameOverPhase;

typedef struct {
    uint8_t  phase;     /* GameOverPhase */
    uint8_t  nc_step;   /* GO_NC_SEQ index into the 8-entry no-continue fade chain */
    uint16_t saved_x;   /* ow.respawn_x captured at GO_ENTER (used at GO_NC_MAP) */
    uint16_t saved_y;   /* ow.respawn_y captured at GO_ENTER */
} GameOverState;

/* GAME_MODE_OVERWORLD — the permanent root mode (g_mode_stack[0]), pushed once at
 * game_init() and never popped. It folds in the BOOT machine (D3): its OWP_BOOT_*
 * phases run boot_begin() (which pushes INIT_INTRO as a child, or does the
 * skip-intro init) and then overworld_boot() + the first render, with no yield
 * between overworld_boot() and OWP_RENDER. After boot it is the overworld loop:
 * run-to-completion port of overworld_post() + overworld_step() (game_main.c, port
 * of main.asm lines 25-161). Each frame the post-vblank input logic (steps 1-8 of
 * overworld_post) runs across the OWP_POST_* phases, then the render tail
 * (oam_clear / run_actionscript_frame / update_screen / update_swirl_effect) runs
 * at OWP_RENDER and the step CONTINUEs (one yield = one frame).
 *
 * Every former inline pump-bridge driver becomes a STEP_PUSH with a resume phase:
 *   process_queued_interactions -> PROCESS_INTERACTION (OWP_RESUME_INTERACTION)
 *   init_battle_overworld       -> BATTLE_ENTRY        (OWP_RESUME_BATTLE)
 *   get_off_bicycle_with_message-> BICYCLE_DISMOUNT    (OWP_RESUME_BICYCLE)
 *   open_menu_button            -> PAUSE_MENU          (-> OWP_POST_TELEPORT)
 *   open_hppp_display           -> HPPP_DISPLAY        (-> OWP_POST_TELEPORT)
 *   show_town_map               -> TOWN_MAP            (OWP_RESUME_TOWNMAP)
 *   open_menu_button_checktalk  -> QUICK_CHECKTALK     (-> OWP_POST_TELEPORT)
 *   teleport_mainloop           -> TELEPORT            (-> OWP_LOOP_START)
 *   update_overworld_damage loop-> HP_ALERT mid-loop   (-> OWP_LOOP_END resume)
 *   spawn                       -> GAME_OVER           (OWP_GAMEOVER_RESULT)
 * debug_y_button_menu() stays inline-blocking (debug-only; its Goods sub-front
 * cannot be a mode yet — documented deferral, like battle.c's debug loops).
 *
 * The first frame after boot renders without running POST (the assembly's first
 * loop iteration): OWP_BOOT_AWAIT advances to OWP_RENDER, which renders then
 * advances to OWP_POST_TOP. A "Continue" game-over resets the root's phase back to
 * OWP_BOOT_SETUP (replaying boot/intro) rather than popping. The root never pops. */
typedef enum {
    OWP_RENDER = 0,         /* render tail; -> OWP_POST_TOP (also the entry/first-frame phase) */
    OWP_POST_TOP,           /* post 1-4: interaction / special-mode / battle / bicycle */
    OWP_POST_DEBUG,         /* post 5-6: debug menu (inline), swirl/touched skip */
    OWP_POST_INPUT,         /* post 7: A/B/X/L input handlers */
    OWP_POST_TELEPORT,      /* post 8: PSI teleport check */
    OWP_RESUME_INTERACTION, /* after PROCESS_INTERACTION: input_disable++ -> loop_end */
    OWP_RESUME_BATTLE,      /* after BATTLE_ENTRY: input_disable++ -> OWP_POST_DEBUG */
    OWP_RESUME_BICYCLE,     /* after BICYCLE_DISMOUNT: enable entities -> render (skip loop_end) */
    OWP_RESUME_TOWNMAP,     /* after TOWN_MAP: enable entities -> OWP_POST_TELEPORT */
    OWP_LOOP_START,         /* loop_end entry: reset the damage-loop state */
    OWP_LOOP_END,           /* damage loop driver: HP_ALERT pushes mid-loop, spawn at the end */
    OWP_GAMEOVER_RESULT,    /* after GAME_OVER pops: Continue -> reboot, No Continue -> render */
    OWP_BOOT_SETUP,         /* boot stage 1: boot_begin() (push INIT_INTRO / skip-intro). The
                             * root's initial phase + the game-over "Continue" reboot target. */
    OWP_BOOT_AWAIT,         /* boot stage 3: after INIT_INTRO pops back, overworld_boot() then
                             * -> OWP_RENDER in the same step (no yield = no Ness-flash). */
    OWP_RENDER_FLUSH,       /* resume after a parked actionscript frame's child popped:
                             * update_screen + update_swirl_effect -> OWP_POST_TOP. */
    OWP_BOOT_FLUSH,         /* resume after the boot actionscript frame parked (rare —
                             * EVENT_001/002 run no modal): overworld_boot_flush() ->
                             * OWP_RENDER in the same step. */
} OverworldPhase;

typedef struct {
    uint8_t       phase; /* OverworldPhase */
    OwDamageState dmg;   /* update_overworld_damage loop progress (resumable HP_ALERT) */
} OverworldModeState;

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
    LoadBattleSceneState  load_battle_scene;
    BattleRowSelectState  battle_row_select;
    BattleEnemySelectState battle_enemy_select;
    NamingEventsState     naming_events;
    TextInputState        text_input;
    NamingPromptState     naming_prompt;
    ScreenTransitionState screen_transition;
    WaitFramesState       wait_frames;
    EndingState           ending;
    PaletteFadeState      palette_fade;
    MapPaletteFadeState   map_palette_fade;
    MosaicFadeState       mosaic_fade;
    FlyoverState          flyover;
    IntroLogoState        intro_logo;
    GasStationState       gas_station;
    TitleScreenState      title_screen;
    AttractState          attract;
    FileMenuState         file_menu;
    NewGameNamingState    new_game_naming;
    SpecialEventState     special_event;
    EnterNameState        enter_name;
    InitIntroState        init_intro;
    DisplayTextModeState  display_text;
    TextWaitFadeState     text_wait_fade;
    ProcessInteractionState process_interaction;
    DoorTransitionState   door_transition;
    TeleportToState       teleport_to;
    QuickChecktalkState   quick_checktalk;
    PauseMenuState        pause_menu;
    EquipMenuState        equip_menu;
    StatusMenuState       status_menu;
    HpppDisplayState      hppp_display;
    PsiMenuState          psi_menu;
    UseItemState          use_item;
    TeleportMenuState     teleport_menu;
    TargetingState        targeting;
    LevelUpState          level_up;
    BattlePsiMenuState    battle_psi_menu;
    BattleMenuState       battle_menu;
    BattleRoutineState    battle;
    InstantWinState       instant_win;
    BattleEntryState      battle_entry;
    BattleScriptedState   battle_scripted;
    BattleActionState     battle_action;
    BattleCalcState       battle_calc;
    BattleReviveState     battle_revive;
    BattleApplyState      battle_apply;
    BattleKoState         battle_ko;
    CheckDeadPlayersState check_dead;
    ActionscriptFrameState actionscript_frame;
    PpRecoveryFlashState  pp_recovery_flash;
    TeleportState         teleport;
    BicycleDismountState  bicycle_dismount;
    HpAlertState          hp_alert;
    GameOverState         game_over;
    OverworldModeState    overworld;
    DebugGoodsState       debug_goods;
    DebugMenuState        debug_menu;
    StoreMenuState        store_menu;
    EscargoMenuState      escargo_menu;
    TelephoneMenuState    telephone_menu;
    uint8_t               _raw[160];
};

/* Deepest realistic chain (a scripted battle triggered from NPC dialogue):
 * PROCESS_INTERACTION → TEXT_WAIT_FADE → DISPLAY_TEXT → BATTLE_SCRIPTED →
 * BATTLE is 5 levels in, and the turn-execution branch below it is the
 * deepest: BATTLE_ACTION → BC_SMAAAASH → BC_RESIST_DAMAGE → BATTLE_KO →
 * BATTLE_APPLY (final attack) → BATTLE_ACTION (the final action) →
 * BC_RESIST_DAMAGE → BC_CALC_DAMAGE → DISPLAY_TEXT → TEXT_PROMPT reaches
 * 15 (an entity-script text via ACTIONSCRIPT_FRAME → DISPLAY_TEXT roots a
 * similar chain two levels shallower), plus CC_08 CALL_TEXT can nest extra
 * DISPLAY_TEXT levels and the Phase-D flip adds the BOOT/OVERWORLD root.
 * 24 leaves headroom
 * (mode_push logs + drops on overflow rather than corrupting the stack).
 * Raising this changes the on-disk SECTION_MODE_STACK size — harmless
 * pre-cutover (savestates are not cross-build compatible). */
#define MODE_STACK_MAX 24

typedef struct {
    uint8_t   depth;                          /* number of active modes */
    uint8_t   mode[MODE_STACK_MAX];           /* GameMode per level */
    ModeState state[MODE_STACK_MAX];          /* hoisted locals per level */
    int32_t   child_result[MODE_STACK_MAX];   /* result a child handed back on POP */
} ModeStack;

extern ModeStack g_mode_stack;

/* Run one frame of `mode`'s step function. */
StepResult mode_dispatch_step(GameMode mode, ModeState *st);

/* ---- run_actionscript_frame, mode-step form (entity/script.c) ----------------
 * A mode-step context renders one entity-system frame with these two calls:
 *
 *     oam_clear();
 *     st->phase = <flush phase>;
 *     if (run_actionscript_frame_step())
 *         return actionscript_frame_take_push();
 *     continue;   // no park: flush in the same step
 *   <flush phase>:
 *     update_screen(); ...post-render work...
 *
 * run_actionscript_frame_step() runs phase 1 (entity scripts + ticks). If a
 * callroutine requests a child modal (text / mosaic / flyover / pp-recovery) the
 * frame is PARKED and the function returns true: the caller must STEP_PUSH
 * GAME_MODE_ACTIONSCRIPT_FRAME via actionscript_frame_take_push(), which finishes
 * the parked frame (the child mode runs, then phases 2-3 + the reentrancy clear).
 * On its pop the caller resumes at its flush phase and runs the post-render work
 * (the part that followed run_actionscript_frame() in the blocking form). When no
 * child is requested it returns false having completed the whole frame inline.
 *
 * The blocking helper layer (render_frame_tick / window_tick / wait_frames_with_
 * updates / dismount_bicycle / teleport-departure / ending) keeps calling the
 * plain void run_actionscript_frame() (entity.h), which warns-and-drops a stray
 * park — those contexts do not run cutscene callroutines (battle is guarded by
 * battle_mode_flag; transitions/teleport freeze entities; menus/credits run no
 * action scripts), so a park there is not expected. */
bool       run_actionscript_frame_step(void);
StepResult actionscript_frame_take_push(void);

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

/* GAME_MODE_DEBUG_GOODS step (defined in game_main.c). Init via
 * ModeState.debug_goods (phase = DG_DRAW, item_id = 0). Always pops 0. */
StepResult mode_step_debug_goods(ModeState *st);

/* GAME_MODE_DEBUG_MENU step (defined in game_main.c). Init via
 * ModeState.debug_menu (phase = DM_ENTER). Always pops 0. */
StepResult mode_step_debug_menu(ModeState *st);

/* GAME_MODE_BATTLE_WAIT step (defined in battle.c, where the swirl/PSI/meter
 * predicates live). Init via ModeState.battle_wait (kind, plus `remaining` for
 * BW_FRAMES) before pump_mode(GAME_MODE_BATTLE_WAIT). Always pops 0. */
StepResult mode_step_battle_wait(ModeState *st);

/* GAME_MODE_LOAD_BATTLE_SCENE — see LoadBattleSceneState above. Init via
 * ModeState.load_battle_scene (group, music; phase = LBS_ENTER) before the
 * STEP_PUSH from the Giygas cutscene battle-action steppers. Always pops 0.
 * Defined in battle_ui.c. */
StepResult mode_step_load_battle_scene(ModeState *st);

/* GAME_MODE_BATTLE_ROW_SELECT / GAME_MODE_BATTLE_ENEMY_SELECT steps (defined in
 * battle_targeting.c, where the targeting/flashing helpers live). Init the
 * matching ModeState union member (phase = BR_RENDER / ET_DISPLAY) before
 * pump_mode(). Row-select pops row+1 (or 0 on cancel); enemy-select pops the
 * 1-based target index (or 0 on cancel). */
StepResult mode_step_battle_row_select(ModeState *st);
StepResult mode_step_battle_enemy_select(ModeState *st);

/* GAME_MODE_NAMING_EVENTS step (defined in file_select.c, where the naming-
 * entity tables and render_frame_tick_naming_work_step()/_flush() live). Init via
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

/* GAME_MODE_WAIT_FRAMES step (defined in overworld.c). Init via
 * ModeState.wait_frames (phase = WF_FRAME, remaining = N) before the STEP_PUSH;
 * renders N frames (run-to-completion form of wait_frames_with_updates) then POPs 0. */
StepResult mode_step_wait_frames(ModeState *st);

/* GAME_MODE_ENDING step (defined in ending.c). Init via ModeState.ending
 * (phase = EN_CAST_SETUP for the cast scene, EN_CR_SETUP for credits) before the
 * STEP_PUSH; runs the end-of-game sequence to completion and POPs 0. */
StepResult mode_step_ending(ModeState *st);

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

/* GAME_MODE_NEW_GAME_NAMING step (defined in file_select.c). Init via
 * ModeState.new_game_naming (phase = NGN_ENTER) before pushing GAME_MODE_NEW_GAME_
 * NAMING. Pops 1 when the game starts, 0 when backed out past the first name. */
StepResult mode_step_new_game_naming(ModeState *st);

/* GAME_MODE_SPECIAL_EVENT step (defined in display_text_menus.c). Init via
 * ModeState.special_event (phase = SE_ENTER, event_id) before pushing
 * GAME_MODE_SPECIAL_EVENT. Pops the dispatch_special_event() return value;
 * CC_1F_41 stores it to working memory in DT_RESUME_CC1F_SPECIAL_EVENT. */
StepResult mode_step_special_event(ModeState *st);

/* GAME_MODE_ENTER_NAME step (defined in display_text_menus.c). Init via
 * ModeState.enter_name (phase = EN_ENTER, param) before pushing
 * GAME_MODE_ENTER_NAME. Pops the keyboard result (the former
 * enter_your_name_please() return value). */
StepResult mode_step_enter_name(ModeState *st);

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

/* GAME_MODE_DOOR_TRANSITION step (defined in door.c). Init with
 * ModeState.door_transition (phase = DTR_BEGIN, door_ptr) before
 * pump_mode(GAME_MODE_DOOR_TRANSITION). Always pops 0. */
StepResult mode_step_door_transition(ModeState *st);

/* GAME_MODE_TELEPORT_TO step (defined in door.c). Init with
 * ModeState.teleport_to (phase = TT_BEGIN, dest_id). STEP_PUSHed by CC_1F_21 (the
 * display_text TELEPORT_TO command) and the debug menu's CAST/STAFF. Always pops 0. */
StepResult mode_step_teleport_to(ModeState *st);

/* GAME_MODE_QUICK_CHECKTALK step (defined in text.c). Init with
 * ModeState.quick_checktalk (phase = QCT_TEXT) before
 * pump_mode(GAME_MODE_QUICK_CHECKTALK). Always pops 0. */
StepResult mode_step_quick_checktalk(ModeState *st);

/* GAME_MODE_PAUSE_MENU step (defined in text.c, where the command-menu /
 * inventory / give helpers live). Init with ModeState.pause_menu
 * (phase = PM_ENTER) before pump_mode(GAME_MODE_PAUSE_MENU). Always pops 0. */
StepResult mode_step_pause_menu(ModeState *st);

/* GAME_MODE_EQUIP_MENU step (defined in text.c, where equipment_change_menu and
 * the equipment-stats helpers live). Init with ModeState.equip_menu
 * (phase = EQ_ENTER); entered via STEP_PUSH from the pause menu. Always pops 0. */
StepResult mode_step_equip_menu(ModeState *st);

/* GAME_MODE_STATUS_MENU step (defined in text.c, where the PSI list/description
 * callbacks live). Init with ModeState.status_menu (phase = SU_SELECT); entered
 * via STEP_PUSH from the pause menu. Always pops 0. */
StepResult mode_step_status_menu(ModeState *st);

/* GAME_MODE_HPPP_DISPLAY step (defined in text.c). Init with
 * ModeState.hppp_display (phase = HD_ENTER) before
 * pump_mode(GAME_MODE_HPPP_DISPLAY). Always pops 0. */
StepResult mode_step_hppp_display(ModeState *st);

/* GAME_MODE_PSI_MENU step (defined in text.c, where the PSI list/details
 * helpers live). Init with ModeState.psi_menu (phase = PS_ENTER); entered via
 * STEP_PUSH from the pause menu. Pops 1 if a PSI was used, else 0. */
StepResult mode_step_psi_menu(ModeState *st);

/* GAME_MODE_USE_ITEM step (defined in text.c, with the rest of the pause-menu
 * machinery). Init with ModeState.use_item (phase = UI_ENTER, char_id,
 * item_slot); entered via STEP_PUSH from the pause menu. Pops 0 if targeting
 * was cancelled, else 1. */
StepResult mode_step_use_item(ModeState *st);

/* GAME_MODE_TELEPORT_MENU step (defined in display_text_menus.c). Init with
 * ModeState.teleport_menu (phase = TPM_ENTER); entered via STEP_PUSH from the
 * PSI menu's teleport case or CC 1A 0x0B. Pops the 1-based destination index,
 * or 0 if cancelled/empty. */
StepResult mode_step_teleport_menu(ModeState *st);

/* GAME_MODE_STORE_MENU step (defined in display_text_menus.c). Init with
 * ModeState.store_menu (phase = STM_ENTER, shop_id); entered via STEP_PUSH from
 * CC 1A 0x06. Pops the selected item id, or 0 if cancelled/empty. */
StepResult mode_step_store_menu(ModeState *st);

/* GAME_MODE_ESCARGO_MENU step (defined in display_text_menus.c). Init with
 * ModeState.escargo_menu (phase = EEM_ENTER); entered via STEP_PUSH from CC 1A
 * 0x07. Pops the 1-based selection index, or 0 if cancelled/empty. */
StepResult mode_step_escargo_menu(ModeState *st);

/* GAME_MODE_TELEPHONE_MENU step (defined in display_text_menus.c). Init with
 * ModeState.telephone_menu (phase = TPH_ENTER, show_text); entered via STEP_PUSH
 * from CC 1F 0x90 (show_text=0) or CC 1A 0x0A (show_text=1). Pops the 1-based
 * contact index, or 0 if cancelled/empty. */
StepResult mode_step_telephone_menu(ModeState *st);

/* GAME_MODE_DETERMINE_TARGETING step (defined in battle_targeting.c). Init
 * with ModeState.targeting (phase = TGT_ENTER, action_id, char_id); entered
 * via STEP_PUSH from the PSI/use-item/battle-menu steps.
 * Pops (targeting_mode << 8) | target_index, or 0 on cancel. */
StepResult mode_step_determine_targeting(ModeState *st);

/* GAME_MODE_LEVEL_UP step (defined in inventory.c, where the stat-growth and
 * EXP tables live). Init via level_up_make_init() (inventory.h); entered via
 * STEP_PUSH from CC_1E_09 or the gain_exp() pump bridge. Always pops 0. */
StepResult mode_step_level_up(ModeState *st);

/* GAME_MODE_BATTLE_PSI_MENU step (defined in battle_psi.c, where the PSI
 * table and list/cost callbacks live). Init with ModeState.battle_psi_menu
 * (phase = BP_OPEN, char_id); entered via STEP_PUSH from the battle command
 * menu's PSI case. Pops 1 on success (selection stored in bt.battle_menu_*),
 * 0 on cancel. */
StepResult mode_step_battle_psi_menu(ModeState *st);

/* GAME_MODE_BATTLE_MENU step (defined in battle.c, where the battle menu
 * window tables live). Init with ModeState.battle_menu (phase = BM_ENTER,
 * char_id, num_selected); entered via STEP_PUSH from GAME_MODE_BATTLE's
 * player-menu phase. Pops the selected battle action, 0 for cancel/"back",
 * or 0xFFFF for the debug instant win. */
StepResult mode_step_battle_menu(ModeState *st);

/* GAME_MODE_BATTLE step (defined in battle.c). Init with ModeState.battle
 * (phase = BTL_BEGIN); pushed by GAME_MODE_BATTLE_ENTRY and
 * GAME_MODE_BATTLE_SCRIPTED. Pops the battle result (0 = victory, 1 =
 * party defeated, 2 = special defeat code). */
StepResult mode_step_battle(ModeState *st);

/* GAME_MODE_INSTANT_WIN step (defined in battle.c, where the battler/money/
 * drop helpers live). Init with ModeState.instant_win (phase = IW_BEGIN);
 * pushed by GAME_MODE_BATTLE_ENTRY's instant-win branch. Always pops 0. */
StepResult mode_step_instant_win(ModeState *st);

/* GAME_MODE_BATTLE_ENTRY step (defined in battle.c). Init with
 * ModeState.battle_entry (phase = BE_ENTER); entered via the
 * init_battle_overworld() pump bridge until the overworld root loop
 * converts (Phase D). Always pops 0. */
StepResult mode_step_battle_entry(ModeState *st);

/* GAME_MODE_BATTLE_SCRIPTED step (defined in battle.c). Init with
 * ModeState.battle_scripted (phase = BS_ENTER, battle_group); pushed by
 * CC_1F_23 TRIGGER_BATTLE via the cc_1f_dispatch push-signal. Pops 0 =
 * normal victory/post-battle, 1 = party defeated. */
StepResult mode_step_battle_scripted(ModeState *st);

/* GAME_MODE_BATTLE_ACTION step (defined in battle_actions.c, where the
 * btlact_dispatch_table and the per-action steppers live). Dispatches to the
 * resumable stepper named by BattleActionState.table_index. Init via
 * battle_action_dispatch() (battle.h). Always pops 0. */
StepResult mode_step_battle_action(ModeState *st);

/* GAME_MODE_BATTLE_CALC step (defined in battle_calc.c, where the pipeline
 * lives). Dispatches by BattleCalcState.kind. Init via battle_calc_make_init()
 * (battle_internal.h). Pops the blocking function's return value (see
 * BattleCalcKind). */
StepResult mode_step_battle_calc(ModeState *st);

/* GAME_MODE_BATTLE_REVIVE step (defined in battle.c, with the palette-effect
 * helpers it uses). Init via battle_revive_make_init() (battle_internal.h).
 * Always pops 0. */
StepResult mode_step_battle_revive(ModeState *st);

/* GAME_MODE_BATTLE_APPLY step (defined in battle.c). Init via
 * battle_apply_make_init() (battle_internal.h). Always pops 0. */
StepResult mode_step_battle_apply(ModeState *st);

/* GAME_MODE_BATTLE_KO step (defined in battle.c). Init via
 * battle_ko_make_init() (battle_internal.h). Always pops 0. */
StepResult mode_step_battle_ko(ModeState *st);

/* GAME_MODE_CHECK_DEAD_PLAYERS step (defined in battle.c). Init via
 * check_dead_players_make_init() (battle_internal.h). Always pops 0. */
StepResult mode_step_check_dead_players(ModeState *st);

/* GAME_MODE_ACTIONSCRIPT_FRAME step (defined in entity/script.c). Init is built
 * by run_actionscript_frame() from a callroutine's yield request; never pushed
 * directly by other code. Always pops 0. */
StepResult mode_step_actionscript_frame(ModeState *st);

/* GAME_MODE_PP_RECOVERY_FLASH step (defined in battle.c). Zero-init; pushed by
 * GAME_MODE_ACTIONSCRIPT_FRAME (AS_CHILD_PP_RECOVERY). Always pops 0. */
StepResult mode_step_pp_recovery_flash(ModeState *st);

/* GAME_MODE_TELEPORT step (defined in overworld_teleport.c). Zero-init; pumped
 * by teleport_mainloop(). Always pops 0. */
StepResult mode_step_teleport(ModeState *st);

/* GAME_MODE_BICYCLE_DISMOUNT step (defined in overworld_teleport.c). Zero-init;
 * pumped by get_off_bicycle_with_message(). Always pops 0. */
StepResult mode_step_bicycle_dismount(ModeState *st);

/* GAME_MODE_HP_ALERT step (defined in overworld_palette.c). Init via
 * ModeState.hp_alert{party_index}; pumped by check_low_hp_alert(). Always pops 0. */
StepResult mode_step_hp_alert(ModeState *st);

/* GAME_MODE_GAME_OVER step (defined in overworld_palette.c). Zero-init
 * (phase = GO_ENTER); pumped by spawn(). Pops -1 (Continue) or 0 (No Continue). */
StepResult mode_step_game_over(ModeState *st);

/* GAME_MODE_OVERWORLD step (defined in game_main.c). The overworld root mode at
 * g_mode_stack[0]; pushed with phase = OWP_RENDER at the boot->overworld handoff.
 * POPs only to signal a "Continue" game-over reboot; otherwise runs forever. */
StepResult mode_step_overworld(ModeState *st);

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
