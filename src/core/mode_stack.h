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
    GAME_MODE_FADE_WAIT,   /* pilot A: wait for a brightness fade to finish */
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
} FadeTickKind;

typedef struct {
    uint8_t tick_kind;   /* FadeTickKind */
} FadeWaitState;

/* Per-mode hoisted locals (former stack variables). MUST be plain-old-data: no
 * pointers into the stack or heap that would not survive a save/reload. Sized
 * with headroom so adding a future mode's locals does not change the on-disk
 * ModeStack layout for already-shipped modes. */
typedef union {
    FadeWaitState fade_wait;
    uint8_t       _raw[64];
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
