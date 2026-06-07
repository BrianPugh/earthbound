#include "core/mode_stack.h"

#include <string.h>

#include "game_main.h"
#include "platform/platform.h"
#include "game/fade.h"
#include "game/overworld.h"
#include "game/battle.h"
#include "game/display_text.h"
#include "entity/entity.h"

ModeStack g_mode_stack = {
    .depth = 0,
};

/* ---- mode step functions ------------------------------------------------- */

/* GAME_MODE_FADE_WAIT — run per-frame "tick" work until the active brightness
 * fade completes (fade_parameters::step == 0), then pop. The single yield is
 * owned by the caller (pump_mode today, the root loop after cutover), so the
 * body here NEVER calls wait_for_vblank()/host_process_frame() itself.
 *
 * Ported from the blocking loops:
 *   - run_frames_until_fade_done()  (overworld.c, C0DD0F)  -> FADE_TICK_OVERWORLD_RENDER
 *   - battle end-of-battle fade-out (battle.c)             -> FADE_TICK_BATTLE_EFFECTS
 */
static StepResult mode_step_fade_wait(ModeState *st) {
    if (!fade_active())
        return STEP_RESULT_POP(0);

    switch ((FadeTickKind)st->fade_wait.tick_kind) {
    case FADE_TICK_OVERWORLD_RENDER:
        /* Body of the former while(fade_active()) loop in
         * run_frames_until_fade_done(): a full overworld render frame. */
        oam_clear();
        run_actionscript_frame();
        update_screen();
        fade_update();
        break;
    case FADE_TICK_BATTLE_EFFECTS:
        /* Body of the former while(fade_active()) loop at battle end. The
         * blocking version yielded *before* this call; the run-to-completion
         * pump yields *after* every CONTINUE, so update_battle_screen_effects()
         * now runs one frame earlier in each iteration. That is a harmless
         * one-frame phase shift of a brief battle-exit fade-out animation. */
        update_battle_screen_effects();
        break;
    case FADE_TICK_WINDOW:
        /* Body of the former while(fade_active()) loop in
         * wait_for_fade_with_tick() (battle/menu fades that keep windows and
         * HP/PP meters animating). window_tick() yielded internally via
         * render_frame_tick; window_tick_work() does the same work without the
         * yield, which the pump now owns. Work-then-yield matches the original
         * ordering exactly (no phase shift). */
        window_tick_work();
        break;
    }
    return STEP_RESULT_CONTINUE();
}

/* ---- dispatch table ------------------------------------------------------ */

typedef StepResult (*ModeStepFn)(ModeState *st);

static const ModeStepFn mode_step[GAME_MODE_COUNT] = {
    [GAME_MODE_NONE]          = NULL,
    [GAME_MODE_FADE_WAIT]     = mode_step_fade_wait,
    [GAME_MODE_NUMBER_SELECT] = mode_step_number_select,   /* defined in display_text_cc.c */
    [GAME_MODE_CHAR_SELECT]   = mode_step_char_select,     /* defined in battle.c */
    [GAME_MODE_TEXT_DELAY]    = mode_step_text_delay,         /* defined in display_text_cc.c */
    [GAME_MODE_ACTIONSCRIPT_WAIT] = mode_step_actionscript_wait, /* defined in display_text_cc.c */
    [GAME_MODE_TEXT_PROMPT]   = mode_step_text_prompt,        /* defined in display_text_cc.c */
    [GAME_MODE_SELECTION_MENU] = mode_step_selection_menu,    /* defined in window.c */
    [GAME_MODE_TOWN_MAP]      = mode_step_town_map,           /* defined in town_map.c */
    [GAME_MODE_SOUND_STONE]   = mode_step_sound_stone,        /* defined in display_text_menus.c */
    [GAME_MODE_DEBUG_YMENU]   = mode_step_debug_ymenu,        /* defined in game_main.c */
    [GAME_MODE_BATTLE_WAIT]   = mode_step_battle_wait,        /* defined in battle.c */
    [GAME_MODE_BATTLE_ROW_SELECT]   = mode_step_battle_row_select,   /* battle_targeting.c */
    [GAME_MODE_BATTLE_ENEMY_SELECT] = mode_step_battle_enemy_select, /* battle_targeting.c */
    [GAME_MODE_NAMING_EVENTS]       = mode_step_naming_events,       /* file_select.c */
    [GAME_MODE_TEXT_INPUT]          = mode_step_text_input,          /* file_select.c */
    [GAME_MODE_NAMING_PROMPT]       = mode_step_naming_prompt,       /* file_select.c */
    [GAME_MODE_SCREEN_TRANSITION]   = mode_step_screen_transition,   /* door.c */
    [GAME_MODE_PALETTE_FADE]        = mode_step_palette_fade,        /* overworld_palette.c */
};

StepResult mode_dispatch_step(GameMode mode, ModeState *st) {
    return mode_step[mode](st);
}

/* ---- stack management ---------------------------------------------------- */

void mode_push(GameMode mode, const ModeState *init) {
    uint8_t d = g_mode_stack.depth;
    g_mode_stack.mode[d] = (uint8_t)mode;
    if (init)
        g_mode_stack.state[d] = *init;
    else
        memset(&g_mode_stack.state[d], 0, sizeof(ModeState));
    g_mode_stack.child_result[d] = 0;
    g_mode_stack.depth = d + 1;
}

int32_t mode_pop(int32_t result) {
    uint8_t d = --g_mode_stack.depth;
    if (d > 0)
        g_mode_stack.child_result[d - 1] = result;
    return result;
}

/* ---- migration bridge ---------------------------------------------------- */

int32_t pump_mode(GameMode mode, const ModeState *init) {
    mode_push(mode, init);
    uint8_t floor = g_mode_stack.depth;   /* depth that, once popped below, ends the pump */

    for (;;) {
        if (platform_input_quit_requested())
            return 0;

        uint8_t top = (uint8_t)(g_mode_stack.depth - 1);
        StepResult r = mode_dispatch_step((GameMode)g_mode_stack.mode[top],
                                          &g_mode_stack.state[top]);

        if (r.kind == STEP_PUSH) {
            mode_push(r.push_mode, NULL);
        } else if (r.kind == STEP_POP) {
            mode_pop(r.pop_result);
            if (g_mode_stack.depth < floor)
                return r.pop_result;   /* completed without an extra yield */
        }

        host_process_frame();   /* the single (local) yield */
    }
}
