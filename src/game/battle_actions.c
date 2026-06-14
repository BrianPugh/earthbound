/*
 * Battle action handlers.
 *
 * Extracted from battle.c — contains all btlact_* action implementations,
 * PSI/item/status effect handlers, and the action dispatch table.
 */
#include "game/battle.h"
#include "game/battle_internal.h"
#include "game/game_state.h"
#include "game/display_text.h"
#include "game/inventory.h"
#include "game/audio.h"
#include "game/map_loader.h"
#include "game/overworld.h"
#include "game/window.h"
#include "game/text.h"
#include "game/fade.h"
#include "game/oval_window.h"
#include "game/battle_bg.h"
#include "entity/entity.h"
#include "data/assets.h"
#include "core/math.h"
#include "core/memory.h"
#include "core/log.h"
#include "include/binary.h"
#include "include/pad.h"
#include "snes/ppu.h"
#include "core/decomp.h"
#include "platform/platform.h"
#include "data/text_refs.h"
#include "game/display_text_internal.h"  /* dt_make_child_init (plain text pushes) */
#include <stdio.h>
#include <string.h>

#include "game_main.h"

/* Dispatch-table binary search (defined with the table at the bottom);
 * used by steppers that push another action as a BATTLE_ACTION child
 * (e.g. double_bash → bash). */
static int btlact_find(uint32_t rom_addr);

/* display_text_from_addr as a DISPLAY_TEXT push: no battle prologue/epilogue
 * (used by steppers whose blocking form manages dt.blinking_triangle_flag
 * itself, e.g. switch_weapons/armor). Returns false (warn, like
 * display_text_from_addr) on an unresolvable address — the caller falls
 * through to its resume pc inline. */
static bool push_plain_text(ModeState *child, uint32_t addr) {
    if (dt_make_child_init(child, addr))
        return true;
    LOG_WARN("WARNING: resolve_text_addr(0x%06X) returned NULL\n", addr);
    return false;
}

/* ======================================================================
 * Battle action handlers
 *
 * Converted actions (the GAME_MODE_BATTLE_ACTION long tail) are small
 * btlact_*_step() pc-machines: texts are DISPLAY_TEXT pushes via
 * battle_push_text / battle_push_text_ex (battle.c), and every resume pc
 * starts with the dt.blinking_triangle_flag clear (the blocking
 * display_in_battle_text epilogue). Action→action dispatch goes through
 * battle_action_dispatch (a STEP_PUSH of the child's stepper); the former
 * blocking btlact_*() wrapper column + btlact_pump bridge were deleted with
 * pump_mode at cutover, so the dispatch table's .func column is now used only
 * for the pure, stepper-less actions (see jump_temp_function_pointer).
 * ====================================================================== */

/* The level-N physical damage formula shared by the attack steppers:
 * offense*mult - defense with 25% variance, floored to 1. The variance
 * gate differs by level: raw > 1 for levels 1/2, raw > 0 for levels 3/4
 * (see the battle_level_N_attack blocking forms in battle_calc.c). */
static uint16_t phys_attack_damage(uint16_t mult, bool variance_when_gt1) {
    Battler *atk = battler_from_offset(bt.current_attacker);
    Battler *tgt = battler_from_offset(bt.current_target);

    int16_t raw_damage = (int16_t)(atk->offense * mult) - (int16_t)tgt->defense;

    uint16_t damage;
    if (variance_when_gt1 ? raw_damage > 1 : raw_damage > 0) {
        damage = battle_25pct_variance((uint16_t)raw_damage);
    } else {
        damage = (uint16_t)raw_damage;
    }

    if ((int16_t)damage <= 0)
        damage = 1;
    return damage;
}

/* ----------------------------------------------------------------------
 * Shared stepper for the standard physical-attack shape:
 *   miss check → [SMAAAASH check] → dodge check → offense*mult - defense
 *   (25% variance, floor 1) → CALC_RESIST_DAMAGE → [heal strangeness].
 * The calc pipeline stages are GAME_MODE_BATTLE_CALC pushes (value-returning
 * — see BattleCalcKind); the dodge check is pure (no text) and runs inline,
 * with the dodge text as a DISPLAY_TEXT push. `variance_when_gt1` selects
 * the level-1/2 variance gate (raw > 1) vs the level-3/4 gate (raw > 0).
 * RNG order matches the blocking composition exactly: miss roll → smaaaash
 * roll → dodge roll → variance rolls → resist-pipeline rolls.
 * ---------------------------------------------------------------------- */
static StepResult btlact_phys_attack_step(BattleActionState *st,
                                          uint16_t miss_type, bool do_smaaaash,
                                          uint16_t mult, bool variance_when_gt1,
                                          uint32_t dodge_msg, bool heal_strange) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0:
            st->pc = 1;
            battle_calc_make_init(&child, BC_MISS_CALC, miss_type, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 1:
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);  /* missed */
            if (do_smaaaash) {
                st->pc = 2;
                battle_calc_make_init(&child, BC_SMAAAASH, 0, 0);
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
            }
            st->pc = 3;
            break;
        case 2:
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);  /* SMAAAASH dealt the damage */
            st->pc = 3;
            break;
        case 3: {
            if (battle_determine_dodge()) {
                st->pc = 5;
                if (battle_push_text(&child, dodge_msg))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }

            st->pc = 4;
            battle_calc_make_init(&child, BC_RESIST_DAMAGE,
                                  phys_attack_damage(mult, variance_when_gt1), 0xFF);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        }
        case 4:
            if (heal_strange) {
                st->pc = 6;
                battle_calc_make_init(&child, BC_HEAL_STRANGENESS, 0, 0);
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
            }
            return STEP_RESULT_POP(0);
        case 5:  /* dodge text epilogue */
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        case 6:
        default:
            return STEP_RESULT_POP(0);
        }
    }
}

/*
 * BTLACT_BASH (asm/battle/actions/bash.asm)
 *
 * Standard melee attack: miss check → SMAAAASH check → dodge check →
 * level 2 attack → heal strangeness.
 */
static StepResult btlact_bash_step(BattleActionState *st) {
    return btlact_phys_attack_step(st, 0, true, 2, true,
                                   MSG_BTL4_RESULT_DODGE_ATTACK, true);
}


/*
 * BTLACT_SHOOT (asm/battle/actions/shoot.asm)
 *
 * Ranged attack: miss check (gun miss text) → dodge check → level 2 attack.
 * No SMAAAASH check and no strangeness healing.
 */
static StepResult btlact_shoot_step(BattleActionState *st) {
    return btlact_phys_attack_step(st, 1, false, 2, true,
                                   MSG_BTL4_RESULT_DODGE_QUICK, false);
}


/*
 * BTLACT_SPY (asm/battle/actions/spy.asm)
 *
 * Displays enemy's offense, defense, and elemental vulnerabilities.
 * If the enemy has a stealable item and the player has inventory space,
 * gives the item to the player.
 *
 * Resumable: each text is one pc stage; the conditions re-derive from the
 * target battler each step (battler state does not change during the text
 * displays). pc 8/9 keep the original ordering of bt.item_dropped = 0
 * AFTER its text completes.
 */
static StepResult btlact_spy_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */
    Battler *tgt = battler_from_offset(bt.current_target);

    for (;;) {
        switch (st->pc) {
        case 0:  /* offense */
            st->pc = 1;
            if (battle_push_text_ex(&child, MSG_BTL5_CHECK_OFFENSE_STAT,
                                    false, true, tgt->offense))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        case 1:  /* defense */
            dt.blinking_triangle_flag = 0;
            st->pc = 2;
            if (battle_push_text_ex(&child, MSG_BTL5_CHECK_DEFENSE_STAT,
                                    false, true, tgt->defense))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        /* Elemental resistances — display if 0xFF (complete immunity) */
        case 2:
            dt.blinking_triangle_flag = 0;
            st->pc = 3;
            if (tgt->fire_resist == 0xFF &&
                battle_push_text(&child, MSG_BTL5_CHECK_VULN_PSI_FIRE))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        case 3:
            dt.blinking_triangle_flag = 0;
            st->pc = 4;
            if (tgt->freeze_resist == 0xFF &&
                battle_push_text(&child, MSG_BTL5_CHECK_VULN_PSI_FREEZE))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        case 4:
            dt.blinking_triangle_flag = 0;
            st->pc = 5;
            if (tgt->flash_resist == 0xFF &&
                battle_push_text(&child, MSG_BTL5_CHECK_VULN_PSI_FLASH))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        case 5:
            dt.blinking_triangle_flag = 0;
            st->pc = 6;
            if (tgt->paralysis_resist == 0xFF &&
                battle_push_text(&child, MSG_BTL5_CHECK_VULN_PARALYSIS))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        case 6:
            dt.blinking_triangle_flag = 0;
            st->pc = 7;
            if (tgt->hypnosis_resist == 0xFF &&
                battle_push_text(&child, MSG_BTL5_CHECK_OPEN_TO_HYPNOSIS))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        case 7:
            dt.blinking_triangle_flag = 0;
            st->pc = 8;
            if (tgt->brainshock_resist == 0xFF &&
                battle_push_text(&child, MSG_BTL5_CHECK_VULN_BRAIN_SHOCK))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        case 8:  /* stealable item drop */
            dt.blinking_triangle_flag = 0;
            if (tgt->ally_or_enemy == 1 && find_inventory_space2(3) != 0 &&
                bt.item_dropped != 0) {
                set_current_item((uint8_t)bt.item_dropped);
                st->pc = 9;  /* the item_dropped clear runs after the text */
                if (battle_push_text(&child, MSG_BTL8_PRESENT_BEHIND_ENEMY))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }
            return STEP_RESULT_POP(0);
        case 9:
        default:
            dt.blinking_triangle_flag = 0;
            bt.item_dropped = 0;
            return STEP_RESULT_POP(0);
        }
    }
}


/*
 * BTLACT_LEVEL_1_ATTACK (wrapper — asm/battle/actions/level_1_attack.asm)
 *
 * Standard physical attack with miss/smaaaash/dodge checks.
 * Uses level 1 damage formula (offense - defense).
 */
static StepResult btlact_level_1_attack_step(BattleActionState *st) {
    return btlact_phys_attack_step(st, 0, true, 1, true,
                                   MSG_BTL4_RESULT_DODGE_ATTACK, true);
}


/*
 * BTLACT_LEVEL_3_ATK / BTLACT_LEVEL_4_ATK steppers (the blocking forms are
 * battle_level_3/4_attack in battle_calc.c — full attacks with the
 * miss/smaaaash/dodge prologue, the raw > 0 variance gate, and the
 * strangeness heal).
 */
static StepResult btlact_level_3_attack_step(BattleActionState *st) {
    return btlact_phys_attack_step(st, 0, true, 3, false,
                                   MSG_BTL4_RESULT_DODGE_ATTACK, true);
}

static StepResult btlact_level_4_attack_step(BattleActionState *st) {
    return btlact_phys_attack_step(st, 0, true, 4, false,
                                   MSG_BTL4_RESULT_DODGE_ATTACK, true);
}

/*
 * BTLACT_LEVEL_2_ATK tail (the bare battle_level_2_attack table row,
 * 0xC28523 — no miss/smaaaash/dodge prologue): offense*2 - defense with
 * the raw > 1 variance gate, straight into CALC_RESIST_DAMAGE.
 */
static StepResult btlact_level_2_attack_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    if (st->pc == 0) {
        st->pc = 1;
        battle_calc_make_init(&child, BC_RESIST_DAMAGE,
                              phys_attack_damage(2, true), 0xFF);
        return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
    }
    return STEP_RESULT_POP(0);
}

/* ----------------------------------------------------------------------
 * Shared stepper tail for the most common converted-action shape:
 * "decide + mutate, then one tail text". pc 0 pushes `msg` (0 = no text:
 * pop immediately); pc 1 runs the blocking display_in_battle_text epilogue
 * (the dt.blinking_triangle_flag clear) and pops. The wrapper steppers
 * MUST compute `msg` (and any state mutation deciding it) only when
 * st->pc == 0 — at later pcs the argument is unused, pass 0.
 * ---------------------------------------------------------------------- */
static StepResult btlact_single_text_step_ex(BattleActionState *st, uint32_t msg,
                                             bool has_cnum, uint32_t cnum) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0:
        if (msg == 0)
            return STEP_RESULT_POP(0);
        st->pc = 1;
        if (battle_push_text_ex(&child, msg, false, has_cnum, cnum))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
        /* FALLTHROUGH — unresolvable text: epilogue inline */
    case 1:
    default:
        dt.blinking_triangle_flag = 0;
        return STEP_RESULT_POP(0);
    }
}

static StepResult btlact_single_text_step(BattleActionState *st, uint32_t msg) {
    return btlact_single_text_step_ex(st, msg, false, 0);
}

/* "decide + mutate, then one tail text" with a BattleTailText (the
 * with-cnum display_text_wait_addr variant): the per-action decide runs at
 * pc 0 only and fills the tail (msg 0 = no text). Shared by the stat-mod,
 * recover-tail, and PP-drain actions. */
typedef void (*StatModDecideFn)(BattleTailText *out);

static StepResult btlact_statmod_step(BattleActionState *st,
                                      StatModDecideFn decide) {
    BattleTailText tail = {0};
    if (st->pc == 0)
        decide(&tail);
    return btlact_single_text_step_ex(st, tail.msg, tail.has_cnum, tail.cnum);
}

static void statmod_tail(BattleTailText *out, uint32_t msg, uint32_t cnum) {
    out->msg = msg;
    out->cnum = cnum;
    out->has_cnum = true;
}

/* The NPC test shared by most stat mods (battle_fail_attack_on_npcs'
 * "did not work" + abort, inlined into the decide). */
static bool statmod_npc_fail(BattleTailText *out, Battler *target) {
    if (target->npc_id != 0) {
        out->msg = MSG_BTL4_RESULT_DID_NOT_WORK;
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------------
 * The Healing PSI cascade (alpha ⊂ beta ⊂ gamma ⊂ omega).
 *
 * Each decide helper applies its cures and returns the tail text; the
 * fallback chain mirrors the blocking originals' tail calls. The γ/Ω
 * revive paths are GAME_MODE_BATTLE_REVIVE child pushes (the revive has
 * its own text + enemy palette flash).
 * ---------------------------------------------------------------------- */

/* BTLACT_HEALING_A (asm/battle/actions/healing_alpha.asm):
 * cures cold, sunstroke, or sleep; otherwise "no effect". */
static uint32_t healing_alpha_decide(Battler *tgt) {
    /* Check PERSISTENT_EASYHEAL group first */
    uint8_t easyheal = tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];

    if (easyheal == STATUS_0_COLD) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        return MSG_BTL5_CURED_COLD;
    }
    if (easyheal == STATUS_0_SUNSTROKE) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        return MSG_BTL5_CURED_SUNSTROKE;
    }

    /* Check TEMPORARY group for sleep */
    if (tgt->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_ASLEEP) {
        tgt->afflictions[STATUS_GROUP_TEMPORARY] = 0;
        return MSG_BTL5_CURED_ASLEEP;
    }

    /* No curable status — "no effect" */
    return MSG_BTL4_RESULT_HEAL_NO_EFFECT;
}

/* BTLACT_HEALING_B (asm/battle/actions/healing_beta.asm):
 * cures poison, nausea, crying, strangeness; falls back to Healing Alpha. */
static uint32_t healing_beta_decide(Battler *tgt) {
    uint8_t easyheal = tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];

    if (easyheal == STATUS_0_POISONED) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        return MSG_BTL5_CURED_POISONED;
    }
    if (easyheal == STATUS_0_NAUSEOUS) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        return MSG_BTL5_CURED_NAUSEOUS;
    }
    if (tgt->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_CRYING) {
        tgt->afflictions[STATUS_GROUP_TEMPORARY] = 0;
        return MSG_BTL5_CURED_CRYING;
    }
    if (tgt->afflictions[STATUS_GROUP_STRANGENESS] == STATUS_3_STRANGE) {
        tgt->afflictions[STATUS_GROUP_STRANGENESS] = 0;
        return MSG_BTL5_CURED_STRANGE;
    }

    return healing_alpha_decide(tgt);
}

/* The non-revive Healing-γ cures (paralysis, diamondize) + the Healing-β
 * fallback — the γ/Ω steppers' non-unconscious path. (The easyheal statuses
 * are one byte, so the unconscious case being checked separately first does
 * not change which cure can match.) */
static uint32_t healing_gamma_cures(Battler *tgt) {
    uint8_t easyheal = tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];

    if (easyheal == STATUS_0_PARALYZED) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        return MSG_BTL5_CURED_NUMB;
    }
    if (easyheal == STATUS_0_DIAMONDIZED) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        return MSG_BTL5_CURED_DIAMONDIZED;
    }

    return healing_beta_decide(tgt);
}

static StepResult btlact_healing_alpha_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? healing_alpha_decide(battler_from_offset(bt.current_target)) : 0;
    return btlact_single_text_step(st, msg);
}

static StepResult btlact_healing_beta_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? healing_beta_decide(battler_from_offset(bt.current_target)) : 0;
    return btlact_single_text_step(st, msg);
}

/* BTLACT_HEALING_G (asm/battle/actions/healing_gamma.asm):
 * cures paralysis, diamondize; revives (75%, hp_max/4) — the revive is a
 * BATTLE_REVIVE child push (it has its own text + enemy palette flash),
 * revive failure has its own text; falls back to Healing Beta. The 75% roll
 * happens at pc 0 only (the original sequence point). */
static StepResult btlact_healing_gamma_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0: {
            Battler *tgt = battler_from_offset(bt.current_target);
            if (tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] ==
                STATUS_0_UNCONSCIOUS) {
                /* 75% chance to revive */
                if (battle_success_255(192)) {
                    /* Revive with hp_max / 4 */
                    st->pc = 2;
                    battle_revive_make_init(&child, bt.current_target,
                                            tgt->hp_max >> 2);
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_REVIVE, &child);
                }
                st->pc = 1;
                if (battle_push_text(&child, MSG_BTL5_REVIVE_FAILED))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }
            st->pc = 1;
            if (battle_push_text(&child, healing_gamma_cures(tgt)))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 1:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        case 2:
        default:
            return STEP_RESULT_POP(0);
        }
    }
}

/* BTLACT_HEALING_O (asm/battle/actions/healing_omega.asm):
 * revives with full HP (a BATTLE_REVIVE child push); falls back to Healing
 * Gamma's cures (γ's own unconscious branch is unreachable from here). */
static StepResult btlact_healing_omega_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0: {
            Battler *tgt = battler_from_offset(bt.current_target);
            if (tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] ==
                STATUS_0_UNCONSCIOUS) {
                st->pc = 2;
                battle_revive_make_init(&child, bt.current_target, tgt->hp_max);
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_REVIVE, &child);
            }
            st->pc = 1;
            if (battle_push_text(&child, healing_gamma_cures(tgt)))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 1:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        case 2:
        default:
            return STEP_RESULT_POP(0);
        }
    }
}


/* ----------------------------------------------------------------------
 * The Shield PSI family (asm/battle/actions/shield_alpha.asm,
 * shield_beta.asm, psi_shield_alpha.asm, psi_shield_beta.asm).
 *
 * Applies the shield type to the current target; the text picks
 * applied-vs-stronger on battle_shields_common()'s result (== 0: shield
 * already active — refreshed; assembly: BEQ).
 * ---------------------------------------------------------------------- */
static uint32_t shields_decide(uint16_t shield_type, uint32_t msg_applied,
                               uint32_t msg_stronger) {
    Battler *tgt = battler_from_offset(bt.current_target);
    uint16_t result = battle_shields_common(tgt, shield_type);
    return (result == 0) ? msg_applied : msg_stronger;
}

static StepResult btlact_shield_alpha_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? shields_decide(STATUS_6_SHIELD, MSG_BTL5_SHIELD_OF_LIGHT_APPLIED,
                         MSG_BTL5_SHIELD_OF_LIGHT_STRONGER) : 0;
    return btlact_single_text_step(st, msg);
}

static StepResult btlact_shield_beta_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? shields_decide(STATUS_6_SHIELD_POWER, MSG_BTL5_POWER_SHIELD_APPLIED,
                         MSG_BTL5_POWER_SHIELD_STRONGER) : 0;
    return btlact_single_text_step(st, msg);
}

static StepResult btlact_psi_shield_alpha_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? shields_decide(STATUS_6_PSI_SHIELD, MSG_BTL5_PSYCHIC_SHIELD_APPLIED,
                         MSG_BTL5_PSYCHIC_SHIELD_STRONGER) : 0;
    return btlact_single_text_step(st, msg);
}

static StepResult btlact_psi_shield_beta_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? shields_decide(STATUS_6_PSI_SHIELD_POWER, MSG_BTL5_PSI_POWER_SHIELD_APPLIED,
                         MSG_BTL5_PSI_POWER_SHIELD_STRONGER) : 0;
    return btlact_single_text_step(st, msg);
}


/* ======================================================================
 * HP/PP recovery battle actions
 *
 * All funnel into battle_recover_hp/pp's prepare halves (battle.c): the
 * state mutation runs at pc 0, the tail text (HP/PP recovered / maxed out /
 * couldn't be healed) is the single pushed text. The recovery amounts roll
 * the RNG, so they are computed ONLY at pc 0 (see btlact_single_text_step).
 * ====================================================================== */

static StepResult btlact_recover_step(BattleActionState *st, bool pp, uint16_t amount) {
    BattleTailText tail = {0};
    if (st->pc == 0) {
        if (pp)
            battle_recover_pp_prepare(battler_from_offset(bt.current_target),
                                      amount, &tail);
        else
            battle_recover_hp_prepare(battler_from_offset(bt.current_target),
                                      amount, &tail);
    }
    return btlact_single_text_step_ex(st, tail.msg, tail.has_cnum, tail.cnum);
}

static StepResult btlact_hp_recovery_10_step(BattleActionState *st) {
    return btlact_recover_step(st, false,
                               (st->pc == 0) ? battle_25pct_variance(10) : 0);
}

static StepResult btlact_hp_recovery_50_step(BattleActionState *st) {
    return btlact_recover_step(st, false,
                               (st->pc == 0) ? battle_25pct_variance(50) : 0);
}

static StepResult btlact_hp_recovery_100_step(BattleActionState *st) {
    return btlact_recover_step(st, false,
                               (st->pc == 0) ? battle_25pct_variance(100) : 0);
}

static StepResult btlact_hp_recovery_200_step(BattleActionState *st) {
    return btlact_recover_step(st, false,
                               (st->pc == 0) ? battle_25pct_variance(200) : 0);
}

static StepResult btlact_hp_recovery_300_step(BattleActionState *st) {
    return btlact_recover_step(st, false,
                               (st->pc == 0) ? battle_25pct_variance(300) : 0);
}

/* BTLACT_HP_RECOVERY_1D4 (asm/battle/actions/hp_recovery_1d4.asm):
 * recover rand(4)+1 HP (1-4 HP). Used by weak healing items. */
static StepResult btlact_hp_recovery_1d4_step(BattleActionState *st) {
    return btlact_recover_step(st, false,
                               (st->pc == 0) ? (uint16_t)(rand_limit(4) + 1) : 0);
}

/* BTLACT_HP_RECOVERY_10000 (asm/battle/actions/hp_recovery_10000.asm):
 * if target is Poo, recover 10000 HP (full heal); otherwise fall back to
 * 1d4 recovery (Brain Food Lunch flavor text). The branch condition is
 * stable across the text push (battler id does not change). */
static StepResult btlact_hp_recovery_10000_step(BattleActionState *st) {
    Battler *tgt = battler_from_offset(bt.current_target);
    if (tgt->id == PARTY_MEMBER_POO)
        return btlact_recover_step(st, false, (st->pc == 0) ? 10000 : 0);
    return btlact_hp_recovery_1d4_step(st);
}

static StepResult btlact_pp_recovery_20_step(BattleActionState *st) {
    return btlact_recover_step(st, true,
                               (st->pc == 0) ? battle_25pct_variance(20) : 0);
}

static StepResult btlact_pp_recovery_80_step(BattleActionState *st) {
    return btlact_recover_step(st, true,
                               (st->pc == 0) ? battle_25pct_variance(80) : 0);
}


/* ======================================================================
 * Simple wrapper actions
 * ====================================================================== */

/*
 * BTLACT_DOUBLE_BASH (asm/battle/actions/bash_twice.asm)
 *
 * Execute bash attack twice. Each bash runs as a pushed BATTLE_ACTION
 * child (exec_i counts the iterations).
 */
static StepResult btlact_double_bash_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    if (st->exec_i >= 2)
        return STEP_RESULT_POP(0);
    st->exec_i++;
    memset(&child, 0, sizeof(child));
    child.battle_action.table_index = (uint16_t)btlact_find(0xC2859F); /* bash */
    return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ACTION, &child);
}


/*
 * BTLACT_FREEZETIME (asm/battle/actions/freeze_time.asm)
 *
 * Multi-hit bash with time frozen. Pauses HPPP rolling, executes 1-5 bash
 * attacks on randomly selected living targets, then resumes rolling.
 * Each hit picks a random target from the current target set.
 */
static StepResult btlact_freezetime_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    /* exec_i = hit counter; scratch16[0] = hits; scratch32 = saved flags. */
    for (;;) {
        switch (st->pc) {
        case 0:
            /* PAUSE_MUSIC: disable HPPP rolling */
            bt.disable_hppp_rolling = 1;

            /* 1-5 hits */
            st->scratch16[0] = rand_limit(4) + 1;

            /* Save and work with target flags */
            st->scratch32 = bt.battler_target_flags;
            st->pc = 1;
            break;

        case 1: {  /* loop head — one pushed bash per hit */
            if (st->exec_i >= st->scratch16[0]) {
                st->pc = 2;
                break;
            }
            st->exec_i++;

            /* Assembly filters whatever is currently in battler_target_flags
             * (the single target from the previous hit, or original on first
             * pass). If that single target is now untargetable, flags go to
             * 0 → exit. */
            battle_remove_status_untargettable_targets();
            if (bt.battler_target_flags == 0) {
                st->pc = 2;
                break;
            }

            /* Assembly passes the original UNFILTERED saved flags to
             * RANDOM_TARGETTING, not the filtered set. This means it can
             * "waste" hits on untargetable targets. */
            uint32_t single_target = battle_random_targeting(st->scratch32);
            bt.battler_target_flags = single_target;

            /* Find the targeted battler */
            for (uint16_t j = 0; j < BATTLER_COUNT; j++) {
                if (battle_is_char_targeted(j)) {
                    bt.current_target = j * sizeof(Battler);
                    break;
                }
            }
            fix_target_name();

            memset(&child, 0, sizeof(child));
            child.battle_action.table_index =
                (uint16_t)btlact_find(0xC2859F); /* bash */
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ACTION, &child);
        }

        case 2:
            /* RESUME_MUSIC: clear rolling flags */
            bt.half_hppp_meter_speed = 0;
            bt.disable_hppp_rolling = 0;

            st->pc = 3;
            if (battle_push_text(&child, MSG_BTL8_TIME_STARTED_AGAIN))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;

        case 3:
        default:
            dt.blinking_triangle_flag = 0;
            bt.battler_target_flags = 0;
            return STEP_RESULT_POP(0);
        }
    }
}


/* ======================================================================
 * Status effect actions
 *
 * Shared decide: the npc_check inlines battle_fail_attack_on_npcs()'s
 * test (NPC target → "did not work", no infliction); otherwise the
 * infliction result picks success-vs-"did not work".
 * ====================================================================== */

static uint32_t inflict_decide(bool npc_check, uint16_t group, uint16_t value,
                               uint32_t msg_success) {
    Battler *tgt = battler_from_offset(bt.current_target);
    if (npc_check && tgt->npc_id != 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;  /* battle_fail_attack_on_npcs */
    return battle_inflict_status(tgt, group, value) != 0
               ? msg_success : MSG_BTL4_RESULT_DID_NOT_WORK;
}

/* inflict_decide with a success roll between the NPC check and the
 * infliction (the resist-checked family). The roll is selected by enum so
 * it only runs when reached — an NPC-target fail must NOT consume the RNG,
 * exactly like the blocking forms' early return. */
typedef enum {
    INFLICT_ROLL_NONE = 0,
    INFLICT_ROLL_LUCK80,            /* battle_success_luck80() */
    INFLICT_ROLL_RESIST_FLASH,      /* battle_success_255(flash_resist) */
    INFLICT_ROLL_RESIST_FREEZE,     /* battle_success_255(freeze_resist) */
    INFLICT_ROLL_RESIST_PARALYSIS,  /* battle_success_255(paralysis_resist) */
    INFLICT_ROLL_RESIST_HYPNOSIS,   /* battle_success_255(hypnosis_resist) */
    INFLICT_ROLL_RESIST_BRAINSHOCK, /* battle_success_255(brainshock_resist) */
} InflictRoll;

static bool inflict_roll(InflictRoll roll, Battler *tgt) {
    switch (roll) {
    case INFLICT_ROLL_LUCK80:
        return battle_success_luck80() != 0;
    case INFLICT_ROLL_RESIST_FLASH:
        return battle_success_255(tgt->flash_resist) != 0;
    case INFLICT_ROLL_RESIST_FREEZE:
        return battle_success_255(tgt->freeze_resist) != 0;
    case INFLICT_ROLL_RESIST_PARALYSIS:
        return battle_success_255(tgt->paralysis_resist) != 0;
    case INFLICT_ROLL_RESIST_HYPNOSIS:
        return battle_success_255(tgt->hypnosis_resist) != 0;
    case INFLICT_ROLL_RESIST_BRAINSHOCK:
        return battle_success_255(tgt->brainshock_resist) != 0;
    case INFLICT_ROLL_NONE:
    default:
        return true;
    }
}

static uint32_t inflict_roll_decide(bool npc_check, InflictRoll roll,
                                    uint16_t group, uint16_t value,
                                    uint32_t msg_success) {
    Battler *tgt = battler_from_offset(bt.current_target);
    if (npc_check && tgt->npc_id != 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;  /* battle_fail_attack_on_npcs */
    if (!inflict_roll(roll, tgt))
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    return battle_inflict_status(tgt, group, value) != 0
               ? msg_success : MSG_BTL4_RESULT_DID_NOT_WORK;
}

/* BTLACT_POISON (asm/battle/actions/poison.asm):
 * inflict poison on current target. Fails on NPCs. */
static StepResult btlact_poison_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? inflict_decide(true, STATUS_GROUP_PERSISTENT_EASYHEAL,
                         STATUS_0_POISONED, MSG_BTL5_STATUS_POISONED) : 0;
    return btlact_single_text_step(st, msg);
}

/* BTLACT_NAUSEATE (asm/battle/actions/nauseate.asm):
 * inflict nausea on current target. Fails on NPCs. */
static StepResult btlact_nauseate_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? inflict_decide(true, STATUS_GROUP_PERSISTENT_EASYHEAL,
                         STATUS_0_NAUSEOUS, MSG_BTL5_STATUS_NAUSEOUS) : 0;
    return btlact_single_text_step(st, msg);
}

/* BTLACT_FEELSTRANGE (asm/battle/actions/feel_strange.asm):
 * inflict "strange" status on current target. Fails on NPCs. */
static StepResult btlact_feel_strange_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? inflict_decide(true, STATUS_GROUP_STRANGENESS,
                         STATUS_3_STRANGE, MSG_BTL5_STATUS_STRANGE) : 0;
    return btlact_single_text_step(st, msg);
}

/* BTLACT_IMMOBILIZE (asm/battle/actions/immobilize.asm):
 * inflict immobilized status on current target (no NPC check). */
static StepResult btlact_immobilize_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? inflict_decide(false, STATUS_GROUP_TEMPORARY,
                         STATUS_2_IMMOBILIZED, MSG_BTL5_STATUS_IMMOBILIZED) : 0;
    return btlact_single_text_step(st, msg);
}


/* ======================================================================
 * Null / empty actions
 * ====================================================================== */

void btlact_null(void) {
    /* No-op action — does nothing. Used as placeholder in action table. */
}

void btlact_enemy_extend(void) {
    /* No-op — placeholder for enemy extended action slot. */
}

/* BTLACT_NULL2-NULL12 (asm/battle/actions/null02.asm through null12.asm)
 * All are no-op placeholder actions. */
void btlact_null2(void) {}
void btlact_null3(void) {}
void btlact_null4(void) {}
void btlact_null5(void) {}
void btlact_null6(void) {}
void btlact_null7(void) {}
void btlact_null8(void) {}
void btlact_null9(void) {}
void btlact_null10(void) {}
void btlact_null11(void) {}
void btlact_null12(void) {}


/*
 * BTLACT_LEVEL_2_ATK_POISON (asm/battle/actions/level_2_attack_poison.asm)
 * BTLACT_LVL_2_ATK_DIAMONDIZE (asm/battle/actions/level_2_attack_diamondize.asm)
 *
 * Level 2 attack + status infliction, NPC-check prefix. Poison inflicts
 * unconditionally (vs the status group's keep-worse rule); diamondize rolls
 * luck80 first and on success clears all other status groups and accumulates
 * the exp/money reward. The shared stepper runs the phys-attack prologue as
 * BC_* pushes, then branches at pc 5 on `diamondize`.
 */
static StepResult btlact_l2_status_attack_step(BattleActionState *st,
                                               bool diamondize) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0:
            st->pc = 1;
            battle_calc_make_init(&child, BC_FAIL_ON_NPCS, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 1:
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);
            st->pc = 2;
            battle_calc_make_init(&child, BC_MISS_CALC, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 2:
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);
            st->pc = 3;
            battle_calc_make_init(&child, BC_SMAAAASH, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 3:
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);
            if (battle_determine_dodge()) {
                st->pc = 6;
                if (battle_push_text(&child, MSG_BTL4_RESULT_DODGE_ATTACK))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }
            st->pc = 4;
            battle_calc_make_init(&child, BC_RESIST_DAMAGE,
                                  phys_attack_damage(2, true), 0xFF);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 4:
            st->pc = 5;
            battle_calc_make_init(&child, BC_HEAL_STRANGENESS, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 5: {
            Battler *target = battler_from_offset(bt.current_target);
            uint32_t msg;
            if (diamondize) {
                if (!battle_success_luck80())
                    return STEP_RESULT_POP(0);
                if (battle_inflict_status(target,
                        STATUS_GROUP_PERSISTENT_EASYHEAL,
                        STATUS_0_DIAMONDIZED) == 0)
                    return STEP_RESULT_POP(0);

                /* Clear all other status groups */
                target->afflictions[STATUS_GROUP_SHIELD] = 0;
                target->afflictions[STATUS_GROUP_HOMESICKNESS] = 0;
                target->afflictions[STATUS_GROUP_CONCENTRATION] = 0;
                target->afflictions[STATUS_GROUP_STRANGENESS] = 0;
                target->afflictions[STATUS_GROUP_TEMPORARY] = 0;
                target->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] = 0;

                /* Accumulate exp and money reward */
                bt.battle_exp_scratch += target->exp;
                bt.battle_money_scratch += target->money;

                msg = MSG_BTL5_STATUS_DIAMONDIZED;
            } else {
                if (battle_inflict_status(target,
                        STATUS_GROUP_PERSISTENT_EASYHEAL,
                        STATUS_0_POISONED) == 0)
                    return STEP_RESULT_POP(0);
                msg = MSG_BTL5_STATUS_POISONED;
            }
            st->pc = 6;
            if (battle_push_text(&child, msg))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 6:  /* dodge / status text epilogue */
        default:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        }
    }
}

static StepResult btlact_level_2_attack_poison_step(BattleActionState *st) {
    return btlact_l2_status_attack_step(st, false);
}

static StepResult btlact_level_2_attack_diamondize_step(BattleActionState *st) {
    return btlact_l2_status_attack_step(st, true);
}


/* ======================================================================
 * PSI common functions
 * ====================================================================== */

/*
 * PSI_FIRE_COMMON (asm/battle/actions/psi_fire_common.asm)
 * PSI_STARSTORM_COMMON (asm/battle/actions/psi_starstorm_common.asm)
 *
 * Common PSI Fire logic: shield check → 25% variance → fire resist → damage.
 * Starstorm is the same shape with no resistance check (0xFF = full damage).
 */
static StepResult btlact_psi_fire_step_common(BattleActionState *st,
                                              uint16_t base_damage,
                                              bool use_fire_resist) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0:
        st->pc = 1;
        battle_calc_make_init(&child, BC_PSI_SHIELD_NULLIFY, 0, 0);
        return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
    case 1: {
        if (mode_child_result() != 0)
            return STEP_RESULT_POP(0);
        uint16_t damage = battle_25pct_variance(base_damage);
        uint16_t resist = 0xFF;
        if (use_fire_resist)
            resist = battler_from_offset(bt.current_target)->fire_resist;
        st->pc = 2;
        battle_calc_make_init(&child, BC_RESIST_DAMAGE, damage, resist);
        return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
    }
    case 2:
        st->pc = 3;
        battle_calc_make_init(&child, BC_WEAKEN_SHIELD, 0, 0);
        return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
    case 3:
    default:
        return STEP_RESULT_POP(0);
    }
}

/*
 * PSI_FREEZE_COMMON (asm/battle/actions/psi_freeze_common.asm)
 *
 * Common PSI Freeze logic: NPC check → shield check → 25% variance →
 * freeze resist → damage. If damage dealt and target alive, 25% chance
 * to inflict solidified status. The solidify roll runs at pc 3, after the
 * resist-damage child pops — the same sequence point as the blocking form.
 */
static StepResult btlact_psi_freeze_step_common(BattleActionState *st,
                                                uint16_t base_damage) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0:
            st->pc = 1;
            battle_calc_make_init(&child, BC_FAIL_ON_NPCS, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 1:
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);
            st->pc = 2;
            battle_calc_make_init(&child, BC_PSI_SHIELD_NULLIFY, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 2: {
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);
            uint16_t damage = battle_25pct_variance(base_damage);
            Battler *target = battler_from_offset(bt.current_target);
            st->pc = 3;
            battle_calc_make_init(&child, BC_RESIST_DAMAGE, damage,
                                  target->freeze_resist);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        }
        case 3: {
            uint16_t dealt = (uint16_t)mode_child_result();
            Battler *target = battler_from_offset(bt.current_target);

            /* If target is unconscious or no damage dealt, skip solidify */
            st->pc = 5;
            if (target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] ==
                    STATUS_0_UNCONSCIOUS)
                break;
            if (dealt == 0)
                break;

            /* 25% chance to inflict solidified */
            if (rand_limit(100) < 25) {
                if (battle_inflict_status(target, STATUS_GROUP_TEMPORARY,
                                          STATUS_2_SOLIDIFIED) != 0) {
                    st->pc = 4;
                    if (battle_push_text(&child, MSG_BTL5_STATUS_SOLIDIFIED))
                        return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT,
                                                     &child);
                }
            }
            break;
        }
        case 4:
            dt.blinking_triangle_flag = 0;
            st->pc = 5;
            break;
        case 5:
            st->pc = 6;
            battle_calc_make_init(&child, BC_WEAKEN_SHIELD, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 6:
        default:
            return STEP_RESULT_POP(0);
        }
    }
}

/*
 * PSI_ROCKIN_COMMON (asm/battle/actions/psi_rockin_common.asm)
 *
 * Common PSI Rockin' logic: shield check → 50% variance → dodge check →
 * damage with full resistance. Uses 50% variance (wider than fire/freeze).
 * The variance rolls BEFORE the dodge roll, as in the blocking form.
 */
static StepResult btlact_psi_rockin_step_common(BattleActionState *st,
                                                uint16_t base_damage) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0:
            st->pc = 1;
            battle_calc_make_init(&child, BC_PSI_SHIELD_NULLIFY, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 1: {
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);
            uint16_t damage = battle_50pct_variance(base_damage);
            if (battle_determine_dodge()) {
                st->pc = 2;
                if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }
            st->pc = 3;
            battle_calc_make_init(&child, BC_RESIST_DAMAGE, damage, 0xFF);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        }
        case 2:
            dt.blinking_triangle_flag = 0;
            st->pc = 4;
            break;
        case 3:
            st->pc = 4;
            break;
        case 4:
            st->pc = 5;
            battle_calc_make_init(&child, BC_WEAKEN_SHIELD, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 5:
        default:
            return STEP_RESULT_POP(0);
        }
    }
}

/* PSI Thunder common — multi-hit logic (188 lines in assembly) */
/*
 * PSI_THUNDER_COMMON (asm/battle/actions/psi_thunder_common.asm)
 *
 * Multi-hit PSI with random target selection per hit.
 * Total effective damage = base_damage * target_count (capped at 255).
 * Each hit picks a random living target. Each hit can miss (SUCCESS_255).
 * Reflects off Franklin Badge. Shield interactions apply.
 */
static StepResult btlact_psi_thunder_step_common(BattleActionState *st,
                                                 uint16_t base_damage,
                                                 uint16_t hits) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    /* exec_i = hit counter; scratch16[0] = effective hit chance;
     * scratch16[1] = Franklin-badge flag for the current hit;
     * scratch32 = saved target flags. */
    for (;;) {
        switch (st->pc) {
        case 0: {
            /* Count targeted battlers */
            uint16_t target_count = 0;
            for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
                if (battle_is_char_targeted(i))
                    target_count++;
            }

            /* Effective hit chance = count * 64, capped at 255 */
            uint16_t effective_damage = target_count * 64;
            if (effective_damage > 255)
                effective_damage = 255;
            st->scratch16[0] = effective_damage;

            /* Save original target flags */
            st->scratch32 = bt.battler_target_flags;
            st->pc = 1;
            break;
        }

        case 1: {  /* loop head — one iteration per hit */
            if (st->exec_i >= hits) {
                st->pc = 9;
                break;
            }
            st->exec_i++;

            /* Restore original targets, then remove dead/diamondized */
            bt.battler_target_flags = st->scratch32;
            battle_remove_status_untargettable_targets();

            /* If no valid targets remain, stop */
            if (bt.battler_target_flags == 0) {
                st->pc = 9;
                break;
            }

            /* Pick one random target */
            uint32_t single = battle_random_targeting(bt.battler_target_flags);
            bt.battler_target_flags = single;

            /* Find which battler it is */
            uint16_t target_idx = 0;
            for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
                if (battle_is_char_targeted(i)) {
                    target_idx = i;
                    break;
                }
            }

            bt.current_target = target_idx * sizeof(Battler);
            fix_target_name();

            /* Hit/miss check */
            if (battle_success_255(st->scratch16[0])) {
                /* Hit — display text based on damage tier */
                st->pc = 2;
                if (battle_push_text(&child, base_damage == 120
                                                 ? MSG_BTL0_PSI_THUNDER_HIT_SMALL
                                                 : MSG_BTL0_PSI_THUNDER_HIT_LARGE))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }
            /* Miss */
            st->pc = 7;
            if (battle_push_text(&child, MSG_BTL0_PSI_THUNDER_MISS))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }

        case 2: {
            /* Wait for the PSI animation to finish */
            dt.blinking_triangle_flag = 0;
            ModeState *w = &child;
            memset(w, 0, sizeof(*w));
            w->battle_wait.kind = BW_PSI_ANIM;
            st->pc = 3;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_WAIT, &child);
        }

        case 3: {
            Battler *target = battler_from_offset(bt.current_target);
            target->use_alt_spritemap = 0;

            /* Franklin Badge check — allies only */
            st->scratch16[1] = 0;
            if (target->ally_or_enemy == 0) {
                uint16_t char_id = (target->row & 0xFF) + 1;
                if (find_item_in_inventory2(char_id, 1)) { /* 1 = FRANKLIN_BADGE */
                    st->scratch16[1] = 1;
                    st->pc = 4;
                    if (battle_push_text(&child, MSG_BTL5_FRANKLIN_BADGE_DEFLECTED))
                        return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT,
                                                     &child);
                    break;
                }
            }
            st->pc = 4;
            break;
        }

        case 4: {
            /* Badge reflect (after its text, as in blocking), then the
             * shield interactions on the (possibly swapped) target */
            dt.blinking_triangle_flag = 0;
            if (st->scratch16[1]) {
                bt.damage_is_reflected = 1;
                swap_attacker_with_target();
            }

            /* Shield alpha/beta: set shield_hp to 1 (absorbs one hit) */
            Battler *target = battler_from_offset(bt.current_target);
            uint8_t shield = target->afflictions[STATUS_GROUP_SHIELD];
            if (shield == 1 || shield == 2) {
                target->shield_hp = 1;
            }

            /* PSI shield nullify check */
            st->pc = 5;
            battle_calc_make_init(&child, BC_PSI_SHIELD_NULLIFY, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        }

        case 5:
            if (mode_child_result() == 0) {
                st->pc = 6;
                battle_calc_make_init(&child, BC_RESIST_DAMAGE,
                                      battle_50pct_variance(base_damage), 0xFF);
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
            }
            st->pc = 6;
            break;

        case 6:
            st->pc = 8;
            battle_calc_make_init(&child, BC_WEAKEN_SHIELD, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);

        case 7:  /* miss text 1 → miss text 2 */
            dt.blinking_triangle_flag = 0;
            st->pc = 8;
            if (battle_push_text(&child, MSG_BTL6_THUNDER_MISSED))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;

        case 8:
            /* loop end — check if either side is wiped out */
            dt.blinking_triangle_flag = 0;
            if (battle_count_chars(0) == 0 || battle_count_chars(1) == 0) {
                st->pc = 9;
                break;
            }
            st->pc = 1;
            break;

        case 9:
        default:
            /* Clear targeting */
            bt.battler_target_flags = 0;
            return STEP_RESULT_POP(0);
        }
    }
}

static StepResult btlact_psi_thunder_alpha_step(BattleActionState *st) {
    return btlact_psi_thunder_step_common(st, THUNDER_ALPHA_DAMAGE,
                                          THUNDER_ALPHA_HITS);
}
static StepResult btlact_psi_thunder_beta_step(BattleActionState *st) {
    return btlact_psi_thunder_step_common(st, THUNDER_BETA_DAMAGE,
                                          THUNDER_BETA_HITS);
}
static StepResult btlact_psi_thunder_gamma_step(BattleActionState *st) {
    return btlact_psi_thunder_step_common(st, THUNDER_GAMMA_DAMAGE,
                                          THUNDER_GAMMA_HITS);
}
static StepResult btlact_psi_thunder_omega_step(BattleActionState *st) {
    return btlact_psi_thunder_step_common(st, THUNDER_OMEGA_DAMAGE,
                                          THUNDER_OMEGA_HITS);
}

/* ======================================================================
 * PSI wrappers
 * ====================================================================== */

static StepResult btlact_psi_fire_alpha_step(BattleActionState *st) {
    return btlact_psi_fire_step_common(st, FIRE_ALPHA_DAMAGE, true);
}
static StepResult btlact_psi_fire_beta_step(BattleActionState *st) {
    return btlact_psi_fire_step_common(st, FIRE_BETA_DAMAGE, true);
}
static StepResult btlact_psi_fire_gamma_step(BattleActionState *st) {
    return btlact_psi_fire_step_common(st, FIRE_GAMMA_DAMAGE, true);
}
static StepResult btlact_psi_fire_omega_step(BattleActionState *st) {
    return btlact_psi_fire_step_common(st, FIRE_OMEGA_DAMAGE, true);
}


static StepResult btlact_psi_freeze_alpha_step(BattleActionState *st) {
    return btlact_psi_freeze_step_common(st, FREEZE_ALPHA_DAMAGE);
}
static StepResult btlact_psi_freeze_beta_step(BattleActionState *st) {
    return btlact_psi_freeze_step_common(st, FREEZE_BETA_DAMAGE);
}
static StepResult btlact_psi_freeze_gamma_step(BattleActionState *st) {
    return btlact_psi_freeze_step_common(st, FREEZE_GAMMA_DAMAGE);
}
static StepResult btlact_psi_freeze_omega_step(BattleActionState *st) {
    return btlact_psi_freeze_step_common(st, FREEZE_OMEGA_DAMAGE);
}


static StepResult btlact_psi_rockin_alpha_step(BattleActionState *st) {
    return btlact_psi_rockin_step_common(st, ROCKIN_ALPHA_DAMAGE);
}
static StepResult btlact_psi_rockin_beta_step(BattleActionState *st) {
    return btlact_psi_rockin_step_common(st, ROCKIN_BETA_DAMAGE);
}
static StepResult btlact_psi_rockin_gamma_step(BattleActionState *st) {
    return btlact_psi_rockin_step_common(st, ROCKIN_GAMMA_DAMAGE);
}
static StepResult btlact_psi_rockin_omega_step(BattleActionState *st) {
    return btlact_psi_rockin_step_common(st, ROCKIN_OMEGA_DAMAGE);
}


static StepResult btlact_psi_starstorm_alpha_step(BattleActionState *st) {
    return btlact_psi_fire_step_common(st, STARSTORM_ALPHA_DAMAGE, false);
}
static StepResult btlact_psi_starstorm_omega_step(BattleActionState *st) {
    return btlact_psi_fire_step_common(st, STARSTORM_OMEGA_DAMAGE, false);
}



/* ======================================================================
 * Lifeup
 * ====================================================================== */

/*
 * LIFEUP_COMMON (asm/battle/actions/lifeup_common.asm)
 *
 * Apply 25% variance to base healing, then recover HP (whose tail text is
 * the push — the btlact_recover_step idiom: decide + mutate at pc 0 only).
 */
static StepResult btlact_lifeup_step_common(BattleActionState *st,
                                            uint16_t base_healing) {
    BattleTailText tail = {0};
    if (st->pc == 0) {
        uint16_t healing = battle_25pct_variance(base_healing);
        battle_recover_hp_prepare(battler_from_offset(bt.current_target),
                                  healing, &tail);
    }
    return btlact_single_text_step_ex(st, tail.msg, tail.has_cnum, tail.cnum);
}

static StepResult btlact_lifeup_alpha_step(BattleActionState *st) {
    return btlact_lifeup_step_common(st, LIFEUP_ALPHA_HEALING);
}
static StepResult btlact_lifeup_beta_step(BattleActionState *st) {
    return btlact_lifeup_step_common(st, LIFEUP_BETA_HEALING);
}
static StepResult btlact_lifeup_gamma_step(BattleActionState *st) {
    return btlact_lifeup_step_common(st, LIFEUP_GAMMA_HEALING);
}
static StepResult btlact_lifeup_omega_step(BattleActionState *st) {
    return btlact_lifeup_step_common(st, LIFEUP_OMEGA_HEALING);
}


/* ======================================================================
 * Bottle rockets
 * ====================================================================== */

/*
 * BOTTLE_ROCKET_COMMON (asm/battle/actions/bottle_rocket_common.asm)
 *
 * Fire 'count' rockets. Each has a speed-based hit chance (SUCCESS_SPEED 100).
 * Total damage = hits * 120, with 25% variance, full resistance.
 */
static StepResult btlact_bottle_rocket_step_common(BattleActionState *st,
                                                   uint16_t count) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0: {
        uint16_t hits = 0;
        for (uint16_t i = 0; i < count; i++) {
            if (battle_success_speed(100))
                hits++;
        }
        if (hits == 0) {
            st->pc = 1;
            if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            /* FALLTHROUGH — unresolvable text: epilogue inline */
            goto epilogue;
        }
        uint16_t damage = battle_25pct_variance(hits * 120);
        st->pc = 2;
        battle_calc_make_init(&child, BC_RESIST_DAMAGE, damage, 0xFF);
        return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
    }
    case 1:
    epilogue:
        dt.blinking_triangle_flag = 0;
        return STEP_RESULT_POP(0);
    case 2:
    default:
        return STEP_RESULT_POP(0);
    }
}

static StepResult btlact_bottle_rocket_step(BattleActionState *st) {
    return btlact_bottle_rocket_step_common(st, BOTTLE_ROCKET_COUNT);
}
static StepResult btlact_big_bottle_rocket_step(BattleActionState *st) {
    return btlact_bottle_rocket_step_common(st, BIG_BOTTLE_ROCKET_COUNT);
}
static StepResult btlact_multi_bottle_rocket_step(BattleActionState *st) {
    return btlact_bottle_rocket_step_common(st, MULTI_BOTTLE_ROCKET_COUNT);
}


/* ======================================================================
 * Item spray/bomb common functions
 * ====================================================================== */

/*
 * INSECT_SPRAY_COMMON (asm/battle/actions/insect_spray_common.asm)
 * RUST_SPRAY_COMMON (asm/battle/actions/rust_promoter_common.asm)
 *
 * Luck80 check, target must be an enemy of the given type (1 = insect,
 * 2 = metallic). 50% variance on base damage. The luck roll short-circuits
 * before the type checks, exactly like the blocking form.
 */
static StepResult btlact_spray_step_common(BattleActionState *st,
                                           uint16_t enemy_type,
                                           uint16_t base_damage) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0: {
        bool failed;
        if (!battle_success_luck80()) {
            failed = true;
        } else {
            Battler *target = battler_from_offset(bt.current_target);
            failed = (target->ally_or_enemy != 1 ||
                      battle_get_enemy_type(target->id) != enemy_type);
        }
        if (failed) {
            st->pc = 1;
            if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            /* FALLTHROUGH — unresolvable text: epilogue inline */
            goto epilogue;
        }
        uint16_t damage = battle_50pct_variance(base_damage);
        st->pc = 2;
        battle_calc_make_init(&child, BC_RESIST_DAMAGE, damage, 0xFF);
        return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
    }
    case 1:
    epilogue:
        dt.blinking_triangle_flag = 0;
        return STEP_RESULT_POP(0);
    case 2:
    default:
        return STEP_RESULT_POP(0);
    }
}

static StepResult btlact_insecticide_spray_step(BattleActionState *st) {
    return btlact_spray_step_common(st, 1, 100);
}
static StepResult btlact_xterminator_spray_step(BattleActionState *st) {
    return btlact_spray_step_common(st, 1, 200);
}
static StepResult btlact_rust_promoter_step(BattleActionState *st) {
    return btlact_spray_step_common(st, 2, 200);
}
static StepResult btlact_rust_promoter_dx_step(BattleActionState *st) {
    return btlact_spray_step_common(st, 2, 400);
}


/*
 * BOMB_COMMON (asm/battle/actions/bomb_common.asm)
 *
 * Area damage: deals base_damage (with 50% variance) to primary target, then
 * finds adjacent battlers (left/right) and deals half base_damage to each.
 * For party targets: adjacent = party members in neighboring slots.
 * For enemy targets: adjacent = enemies in same row within sprite blast range.
 */
static StepResult btlact_bomb_step_common(BattleActionState *st,
                                          uint16_t base_damage) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0:
            /* Phase 1: Primary damage with 50% variance, full resist (0xFF) */
            st->pc = 1;
            battle_calc_make_init(&child, BC_RESIST_DAMAGE,
                                  battle_50pct_variance(base_damage), 0xFF);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);

        case 1: {
            /* Phase 2: find adjacent battlers. 0xFFFF = no adjacent found
             * (can't use 0 since byte offset 0 is valid). Hoisted into
             * scratch16[0]/[1]; the original target into scratch32. */
            uint16_t adjacent_left = 0xFFFF;
            uint16_t adjacent_right = 0xFFFF;
            Battler *target = battler_from_offset(bt.current_target);

            if ((target->ally_or_enemy & 0xFF) == 0) {
                /* Party member target: find adjacent by party slot order */
                uint16_t slot;
                for (slot = 0; slot < 6; slot++) {
                    if ((game_state.party_members[slot] & 0xFF) == target->id)
                        break;
                }

                /* Left neighbor: party member at slot - 1 */
                if (slot != 0) {
                    adjacent_left = (uint16_t)((slot - 1) * sizeof(Battler));
                }

                /* Right neighbor: party member at slot + 1.
                 * Assembly reads the byte after the last party_members entry
                 * (which is leader_x_frac in WRAM, always >= 6) and skips if
                 * >= 6. We guard with a bounds check instead to avoid OOB
                 * access. */
                if (slot + 1 < 6) {
                    uint8_t next_member = game_state.party_members[slot + 1];
                    if (next_member <= 5) {
                        adjacent_right = (uint16_t)((slot + 1) * sizeof(Battler));
                    }
                }
            } else {
                /* Enemy target: scan for enemies in same row within blast range */
                for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
                    uint16_t b_offset = (uint16_t)(i * sizeof(Battler));
                    if (b_offset == bt.current_target)
                        continue;
                    Battler *b = &bt.battlers_table[i];
                    if ((b->ally_or_enemy & 0xFF) != 1)
                        continue;
                    if (b->row != target->row)
                        continue;

                    uint8_t b_x = b->sprite_x;
                    uint8_t t_x = target->sprite_x;
                    uint16_t range = (get_battle_sprite_width(target->sprite) +
                                      get_battle_sprite_width(b->sprite)) * 4 + 8;

                    if (b_x < t_x) {
                        /* Neighbor to the left */
                        uint16_t dist = (uint16_t)(t_x - b_x);
                        if (dist <= range)
                            adjacent_left = b_offset;
                    } else {
                        /* Neighbor to the right (or same position) */
                        uint16_t dist = (uint16_t)(b_x - t_x);
                        if (dist <= range)
                            adjacent_right = b_offset;
                    }
                }
            }

            st->scratch16[0] = adjacent_left;
            st->scratch16[1] = adjacent_right;
            st->scratch32 = bt.current_target;  /* saved_target */

            /* Phase 3: Apply half base_damage to adjacent targets */
            if (adjacent_left != 0xFFFF) {
                bt.current_target = adjacent_left;
                fix_target_name();
                st->pc = 2;
                battle_calc_make_init(&child, BC_RESIST_DAMAGE,
                                      battle_50pct_variance(base_damage >> 1),
                                      0xFF);
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
            }
            st->pc = 2;
            break;
        }

        case 2:
            if (st->scratch16[1] != 0xFFFF) {
                bt.current_target = st->scratch16[1];
                fix_target_name();
                st->pc = 3;
                battle_calc_make_init(&child, BC_RESIST_DAMAGE,
                                      battle_50pct_variance(base_damage >> 1),
                                      0xFF);
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
            }
            st->pc = 3;
            break;

        case 3:
        default:
            /* Restore original target */
            bt.current_target = (uint16_t)st->scratch32;
            fix_target_name();
            return STEP_RESULT_POP(0);
        }
    }
}

static StepResult btlact_bomb_step(BattleActionState *st) {
    return btlact_bomb_step_common(st, 90);
}
static StepResult btlact_super_bomb_step(BattleActionState *st) {
    return btlact_bomb_step_common(st, 270);
}


/*
 * BTLACT_TELEPORT_BOX (asm/battle/actions/teleport_box.asm)
 *
 * Item-based battle escape. Checks sector attributes for teleport usability
 * (bit 7 = cannot teleport). Outside battle, always succeeds. In battle,
 * success is probability-based using item strength, and fails in boss battles.
 * On success: removes item from inventory, sets instant teleport, bt.special_defeat=1.
 *
 * Resumable: all checks + the success roll + the item removal happen at
 * pc 0 (the original sequence points — everything precedes the text in the
 * blocking form); the teleport-state writes follow the success text in the
 * blocking form, so they run at its resume pc.
 */
static StepResult btlact_teleport_box_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0: {
            /* Check sector attributes — bit 7 means teleport unusable here */
            uint16_t attrs = load_sector_attrs(
                game_state.leader_x_coord, game_state.leader_y_coord);
            if (attrs & 0x0080) {
                st->pc = 1;
                if (battle_push_text(&child, MSG_GOODS1_TELEPORT_BOX_CANT_USE_HERE))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }

            Battler *attacker = battler_from_offset(bt.current_attacker);
            bool failed = false;
            /* Outside battle, always succeeds */
            if (ow.battle_mode != 0) {
                /* Probability check using item strength */
                uint16_t roll = rand_limit(100);
                uint8_t item_id = attacker->current_action_argument & 0xFF;
                const ItemConfig *item_entry = get_item_entry(item_id);
                if (item_entry != NULL) {
                    /* Assembly: strength - 0x80, then EOR #$FF80 to negate upper bits.
                     * Effectively: success_threshold = 128 - strength (for strength < 128)
                     * or success_threshold = strength - 128 (when strength >= 128).
                     * The formula maps 0x80→0, 0xFF→127, i.e. higher strength = easier. */
                    uint8_t strength = item_entry->params[ITEM_PARAM_STRENGTH];
                    int16_t threshold = ((int16_t)(strength & 0xFF) - 0x80) ^ (int16_t)0xFF80;
                    if (roll >= (uint16_t)threshold)
                        failed = true;
                }
                /* Fail in boss battles */
                if (!failed && battle_boss_battle_check() == 0)
                    failed = true;
            }

            if (failed) {
                st->pc = 1;
                if (battle_push_text(&child, MSG_GOODS1_TELEPORT_BOX_MALFUNCTION))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }

            /* Remove item from inventory (before the text, as in blocking) */
            uint8_t slot = attacker->action_item_slot & 0xFF;
            remove_item_from_inventory(attacker->id, slot);
            st->pc = 2;
            if (battle_push_text(&child, MSG_GOODS1_TELEPORT_BOX_SUCCESS))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 2:
            dt.blinking_triangle_flag = 0;
            /* Set teleport state: instant teleport to current destination */
            ow.psi_teleport_destination = game_state.unknownC3;
            ow.psi_teleport_style = 3;  /* TELEPORT_STYLE::INSTANT */
            bt.special_defeat = 1;
            return STEP_RESULT_POP(0);
        case 1:
        default:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        }
    }
}


/*
 * CALL_FOR_HELP_COMMON (asm/battle/call_for_help_common.asm)
 *
 * Enemy summon action used by "call for help" (param=0) and "sow seeds" (param=1).
 * Checks if the target enemy type exists in the current battle group,
 * counts existing same-type enemies, calculates success probability based on
 * max_called from enemy config, then finds a valid screen position for the new
 * enemy. Tries same row first (left or right of existing sprites), then swaps
 * to the other row, then tries replacing a dead battler of equal sprite size.
 */
static uint32_t call_for_help_decide(uint16_t param) {
    Battler *attacker = battler_from_offset(bt.current_attacker);

    /* Must be an enemy */
    if ((attacker->ally_or_enemy & 0xFF) != 1)
        goto help_failed;

    uint16_t enemy_id = attacker->current_action_argument & 0xFF;

    /* Check if this enemy type exists in the current battle group.
     * Assembly scans BTL_ENTRY_PTR_TABLE group data via ROM pointer;
     * we scan bt.enemies_in_battle_ids[] which was populated from the same data. */
    bool found_in_group = false;
    for (uint16_t i = 0; i < bt.enemies_in_battle; i++) {
        if (bt.enemies_in_battle_ids[i] == enemy_id) {
            found_in_group = true;
            break;
        }
    }
    if (!found_in_group)
        goto help_failed;

    /* Count existing alive enemies of the same type (slots 8-31) */
    uint16_t existing_count = 0;
    for (int i = FIRST_ENEMY_INDEX; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if ((b->consciousness & 0xFF) != 1)
            continue;
        if ((b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] & 0xFF) == STATUS_0_UNCONSCIOUS)
            continue;
        if (b->enemy_type_id != (uint8_t)enemy_id)
            continue;
        existing_count++;
    }

    /* Calculate success probability:
     * threshold = (max_called - existing_count) * 0xCD / max_called
     * 0xCD = 205; when existing==0, threshold=205 (~80%); when existing==max, threshold=0 */
    if (!enemy_config_table) goto help_failed;
    const EnemyData *edata = &enemy_config_table[enemy_id];
    uint16_t max_called = edata->max_called;
    int16_t numerator = ((int16_t)max_called - (int16_t)existing_count) * 0xCD;
    int16_t threshold = (max_called > 0) ? (numerator / (int16_t)max_called) : 0;

    if (!battle_success_255((uint16_t)(uint8_t)threshold))
        goto help_failed;

    /* Get new enemy's battle sprite info */
    uint16_t battle_sprite = edata->battle_sprite;
    uint16_t new_tile_width = get_battle_sprite_width(battle_sprite);
    uint16_t new_full_width = new_tile_width * 8 + 0x10;  /* pixel width + padding */
    uint16_t target_row = edata->row;

    /* Check total width of all conscious enemies + new enemy <= 32 tiles */
    uint16_t total_width = calculate_battler_row_width();
    if (total_width + get_battle_sprite_width(battle_sprite) > 0x20)
        goto no_space;

    /* Scan existing battlers to find left/right bounds in same and other row.
     * All bounds initialized to 128 (screen center). */
    uint16_t same_row_left = 0x80;
    uint16_t same_row_right = 0x80;
    uint16_t other_row_left = 0x80;
    uint16_t other_row_right = 0x80;

    for (int i = FIRST_ENEMY_INDEX; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if ((b->consciousness & 0xFF) == 0)
            continue;

        uint16_t sprite_tile_w = get_battle_sprite_width(b->sprite);
        uint16_t half_px = (sprite_tile_w * 8) / 2;
        uint16_t sx = b->sprite_x & 0xFF;

        if ((b->row & 0xFF) == target_row) {
            /* Same row */
            uint16_t left_edge = sx - half_px;
            uint16_t right_edge = sx + half_px;
            if (left_edge < same_row_left)
                same_row_left = left_edge;
            if (right_edge > same_row_right)
                same_row_right = right_edge;
        } else {
            /* Other row */
            uint16_t left_edge = sx - half_px;
            uint16_t right_edge = sx + half_px;
            if (left_edge < other_row_left)
                other_row_left = left_edge;
            if (right_edge > other_row_right)
                other_row_right = right_edge;
        }
    }

    /* Determine placement position.
     * Compare how far sprites extend left vs right of center (128). */
    uint16_t new_x;
    uint16_t right_extend = same_row_right - 0x80;
    uint16_t left_extend = 0x80 - same_row_left;

    if (left_extend >= right_extend) {
        /* More room on the right — try placing right of rightmost sprite */
        if (same_row_right + new_full_width < 0x100) {
            new_x = same_row_right + new_full_width / 2;
            goto place_new_enemy;
        }
    } else {
        /* More room on the left — try placing left of leftmost sprite */
        if (same_row_left > new_full_width) {
            new_x = same_row_left - new_full_width / 2;
            goto place_new_enemy;
        }
    }

    /* Try the other row */
    target_row = 1 - target_row;
    {
        uint16_t other_right_extend = other_row_right - 0x80;
        uint16_t other_left_extend = 0x80 - other_row_left;

        if (other_left_extend >= other_right_extend) {
            /* More room on the right in other row */
            if (other_row_right + new_full_width < 0x100) {
                new_x = other_row_right + new_full_width / 2;
                goto place_new_enemy;
            }
        } else {
            /* More room on the left in other row */
            if (other_row_left > new_full_width) {
                new_x = other_row_left - new_full_width / 2;
                goto place_new_enemy;
            }
        }
    }

no_space:
    /* Last resort: replace a dead battler of the same sprite size */
    for (int i = FIRST_ENEMY_INDEX; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if ((b->consciousness & 0xFF) != 1)
            continue;
        if ((b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] & 0xFF) != STATUS_0_UNCONSCIOUS)
            continue;
        /* Dead enemy — check if sprite widths match */
        uint16_t new_w = get_battle_sprite_width(battle_sprite);
        uint16_t dead_w = get_battle_sprite_width(b->sprite);
        if (new_w != dead_w)
            continue;
        /* Replace: clear consciousness and take its position */
        b->consciousness = 0;
        new_x = b->sprite_x & 0xFF;
        target_row = b->row & 0xFF;
        goto place_new_enemy;
    }
    goto help_failed;

place_new_enemy:
    /* Verify placement still fits (row width could have changed) */
    {
        uint16_t row_total = calculate_battler_row_width();
        uint16_t sprite_w = get_battle_sprite_width(battle_sprite);
        if (row_total + sprite_w > 0x20)
            goto help_failed;
    }

    /* Find an empty battler slot (slots 8-31) */
    {
        uint16_t slot = FIRST_ENEMY_INDEX;
        for (; slot < BATTLER_COUNT; slot++) {
            if ((bt.battlers_table[slot].consciousness & 0xFF) == 0)
                break;
        }
        if (slot >= BATTLER_COUNT)
            FATAL("call_for_help: no empty enemy slot (slot=%u)\n", slot);

        bt.current_target = battler_to_offset(&bt.battlers_table[slot]);
        Battler *newb = battler_from_offset(bt.current_target);
        battle_init_enemy_stats(newb, enemy_id);

        newb->sprite_x = (uint8_t)new_x;
        newb->row = (uint8_t)target_row;

        /* Set sprite_y based on row: row 0 (front) = 0x90, row != 0 (back) = 0x80 */
        newb->sprite_y = (target_row == 0) ? 0x90 : 0x80;

        newb->vram_sprite_index = (uint8_t)find_battle_sprite_for_enemy(enemy_id);
        newb->has_taken_turn = 1;

        fix_target_name();
    }

    return param ? MSG_BTL8_SEED_STARTED_GROWING   /* seeds sprouted */
                 : MSG_BTL8_ALLY_JOINED_BATTLE;    /* called for help */

help_failed:
    return param ? MSG_BTL8_SEED_DIDNT_SPROUT      /* seeds didn't sprout */
                 : MSG_BTL8_NO_ALLY_CAME;          /* nobody came */
}

static StepResult btlact_call_for_help_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? call_for_help_decide(0) : 0);
}
static StepResult btlact_sow_seeds_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? call_for_help_decide(1) : 0);
}


/*
 * BTLACT_HP_SUCKER (asm/battle/actions/hp_sucker.asm)
 *
 * HP drain attack used by Hungry HP-sucker enemy.
 * Luck80 check, then drains target's max HP / 8 (with 50% variance)
 * and heals the attacker by the same amount.
 * If target == attacker (self-targeting via strangeness), displays special text.
 * KOs target if HP reaches 0.
 */
static StepResult btlact_hp_sucker_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0: {
            uint32_t fail_msg = 0;
            if (!battle_success_luck80()) {
                fail_msg = MSG_BTL4_RESULT_DID_NOT_WORK;
            } else {
                /* Attacker must be alive (hp_target > 0) */
                Battler *attacker = battler_from_offset(bt.current_attacker);
                if (attacker->hp_target == 0) {
                    fail_msg = MSG_BTL4_RESULT_DID_NOT_WORK;
                } else if (bt.current_target == bt.current_attacker) {
                    /* Self-targeting check (e.g., feeling strange) */
                    fail_msg = MSG_BTL4_RESULT_DRAINED_OWN_HP;
                }
            }
            if (fail_msg != 0) {
                st->pc = 1;
                if (battle_push_text(&child, fail_msg))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }

            /* Calculate drain amount: 50% variance on target's max HP, then /8 */
            Battler *target = battler_from_offset(bt.current_target);
            uint16_t drain_amount = battle_50pct_variance(target->hp_max) >> 3;
            st->scratch16[0] = drain_amount;

            /* Reduce target's HP (before the text, as in the blocking form) */
            battle_reduce_hp(target, drain_amount);

            st->pc = 2;
            if (battle_push_text_ex(&child, MSG_BTL4_RESULT_HP_DRAINED_FROM,
                                    false, true, drain_amount))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 2: {
            dt.blinking_triangle_flag = 0;
            uint16_t drain_amount = st->scratch16[0];

            /* Heal attacker by the drain amount */
            Battler *attacker = battler_from_offset(bt.current_attacker);
            battle_set_hp(attacker, attacker->hp + drain_amount);

            /* KO target if dead (a BATTLE_KO child push) */
            Battler *target = battler_from_offset(bt.current_target);
            if (target->hp == 0) {
                st->pc = 3;
                battle_ko_make_init(&child, bt.current_target);
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_KO, &child);
            }
            return STEP_RESULT_POP(0);
        }
        case 3:
            return STEP_RESULT_POP(0);
        case 1:
        default:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        }
    }
}



/*
 * BTLACT_MIRROR (asm/battle/actions/mirror.asm)
 *
 * Enemy morphing action. The attacker copies the target's battler data
 * (keeping its own HP/PP). Checks: target must be enemy (ally_or_enemy != 0),
 * target must not be an NPC ally (npc_id == 0), and a random roll must be
 * below the enemy's mirror_success rate from enemy_config_table.
 * On success, backs up attacker to bt.mirror_battler_backup and copies target data.
 */
static uint32_t mirror_decide(void) {
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t target_id = target->id;

    /* Must target an enemy (not ally) */
    if ((target->ally_or_enemy & 0xFF) == 0)
        return MSG_BTL5_MORPH_FAILED;
    /* Must not be an NPC ally */
    if ((target->npc_id & 0xFF) != 0)
        return MSG_BTL5_MORPH_FAILED;
    /* Check mirror success rate from enemy config table */
    uint16_t roll = rand_limit(100);
    if (enemy_config_table != NULL) {
        uint8_t success_rate = enemy_config_table[target_id].mirror_success;
        if (roll >= success_rate)
            return MSG_BTL5_MORPH_FAILED;
    }

    /* Success: set up mirror state */
    bt.mirror_enemy = target_id;
    bt.mirror_turn_timer = DEFAULT_MIRROR_TURN_COUNT;

    /* Backup attacker's current state */
    Battler *attacker = battler_from_offset(bt.current_attacker);
    memcpy(&bt.mirror_battler_backup, attacker, sizeof(Battler));

    /* Copy target's data to attacker (preserving attacker's HP/PP/identity) */
    battle_copy_mirror_data(attacker, target);

    return MSG_BTL5_MORPH_SUCCESS;
}

static StepResult btlact_mirror_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? mirror_decide() : 0);
}


/*
 * BTLACT_RAINBOW_OF_COLOURS (asm/battle/actions/rainbow_of_colours.asm)
 *
 * Enemy transformation: replaces the attacker with a new enemy type
 * (specified in current_action_argument). Preserves sprite position,
 * updates sprite index, marks turn as taken, and skips death text.
 */
void btlact_rainbow_of_colours(void) {
    Battler *attacker = battler_from_offset(bt.current_attacker);
    /* Save position */
    uint8_t saved_x = attacker->sprite_x;
    uint8_t saved_y = attacker->sprite_y;
    /* Reinitialize as new enemy */
    uint16_t new_enemy_id = attacker->current_action_argument & 0xFF;
    battle_init_enemy_stats(attacker, new_enemy_id);
    /* Restore position */
    attacker->sprite_x = saved_x;
    attacker->sprite_y = saved_y;
    /* Update sprite index */
    attacker->vram_sprite_index = (uint8_t)find_battle_sprite_for_enemy(attacker->id);
    attacker->has_taken_turn = 1;
    bt.skip_death_text_and_cleanup = 1;
}


/* Forward declaration needed because btlact_heal_poison is defined later */
void btlact_heal_poison(void);

/* ======================================================================
 * EAT_FOOD helpers (asm/battle/eat_food.asm, asm/battle/apply_condiment.asm,
 *                   asm/overworld/party/schedule_party_animation_reset.asm,
 *                   asm/overworld/party/initialize_party_member_animations.asm)
 * ====================================================================== */

/*
 * INITIALIZE_PARTY_MEMBER_ANIMATIONS callback
 * (asm/overworld/party/initialize_party_member_animations.asm)
 *
 * Called by the overworld task scheduler to reset party walk animations after
 * eating a speed/agility-boosting food item.
 * Sets game_state.party_status = 0 and sets entities.var[3] to 8
 * for party entity slots 24-28.
 */
static void initialize_party_member_animations(void) {
    game_state.party_status = 0;
    /* Assembly lines 10-23: loop slots 24..28, set var3 = 8 */
    for (int slot = 24; slot <= 28; slot++) {
        entities.var[3][ENT(slot)] = 8;
    }
}

/*
 * SCHEDULE_PARTY_ANIMATION_RESET
 * (asm/overworld/party/schedule_party_animation_reset.asm)
 *
 * Called by eat_food when a food item has a nonzero "special" value.
 * Sets game_state.party_status = 3, sets entities.var[3] to 5
 * for party entity slots 24-28, then schedules
 * initialize_party_member_animations as an overworld task with
 * `frames` frames of delay (= special * 6).
 */
static void battle_schedule_party_animation_reset(uint16_t frames) {
    /* Assembly lines 12-13: early exit if already scheduled */
    if ((game_state.party_status & 0xFF) == 3)
        return;
    /* Assembly lines 15-17: set party_status = 3 */
    game_state.party_status = 3;
    /* Assembly lines 19-30: loop slots 24..28, set var3 = 5 */
    for (int slot = 24; slot <= 28; slot++) {
        entities.var[3][ENT(slot)] = 5;
    }
    /* Assembly lines 33-35: schedule the animation reset callback */
    schedule_overworld_task(initialize_party_member_animations, frames);
}

/*
 * APPLY_CONDIMENT (asm/battle/apply_condiment.asm)
 *
 * Checks if the current attacker has a condiment in their inventory.
 * If so, removes it and searches CONDIMENT_TABLE for a (food, condiment)
 * match. On match: displays "great flavor!" text and returns a pointer to
 * the enhanced item_parameters from the condiment table.
 * On mismatch or no condiment: returns a pointer to the food item's own
 * item_parameters (from the item configuration table).
 *
 * The condiment table (asm/data/condiment_table.asm) has one 7-byte entry
 * per food item: [food_id, cond1_id, cond2_id, strength, epi, ep, special].
 * The function returns &entry[3] on a hit or ItemConfig.params on miss/none.
 *
 * Returns: pointer to 4-byte item_parameters [str, epi, ep, special].
 */
static const uint8_t *battle_apply_condiment_prepare(uint32_t *out_msg) {
    Battler *atk = battler_from_offset(bt.current_attacker);
    uint8_t food_id = atk->current_action_argument & 0xFF;
    *out_msg = 0;

    /* Load CONDIMENT_TABLE from ROM asset (asm/data/condiment_table.asm).
     * 43 data entries + 1 zero terminator, 7 bytes each = 308 bytes total. */
    const CondimentEntry *table =
        (const CondimentEntry *)ASSET_DATA(ASSET_DATA_CONDIMENT_TABLE_BIN);
    if (!table)
        return NULL;

    /* Find the food item's row for default params */
    const ItemConfig *item_entry = get_item_entry(food_id);
    const uint8_t *default_params = item_entry ? item_entry->params : NULL;

    /* Search for a condiment in the attacker's inventory */
    uint16_t condiment_id = find_condiment(food_id);

    /* No condiment — return item's own params without any message */
    if (condiment_id == 0) {
        return default_params;
    }

    /* Remove condiment from attacker's inventory */
    take_item_from_character(atk->id, condiment_id);

    /* Search condiment_table for a (food, condiment) match */
    for (const CondimentEntry *entry = table; entry->food_id != 0; entry++) {
        if (entry->food_id != food_id)
            continue;
        /* Check if condiment_id matches condiment1 or condiment2 */
        if (entry->condiment1_id == (uint8_t)condiment_id ||
            entry->condiment2_id == (uint8_t)condiment_id) {
            /* Condiment match — "great flavor!" text + condiment params */
            *out_msg = MSG_GOODS0_CONDIMENT_TASTED_GOOD;
            return &entry->strength;  /* points to [strength, epi, ep, special] */
        }
        /* Wrong condiment for this food */
        *out_msg = MSG_GOODS0_CONDIMENT_BAD_TASTE;
        return default_params;
    }

    /* Food not in condiment table — wrong condiment */
    *out_msg = MSG_GOODS0_CONDIMENT_BAD_TASTE;
    return default_params;
}

/*
 * EAT_FOOD (asm/battle/eat_food.asm, ROM $C2B27D)
 *
 * Handles eating a food item in battle. Applies condiment bonuses first,
 * then dispatches on effect type (params[0]):
 *   0 = HP recovery (amount*6 with 25% variance; 0=full 30000)
 *   1 = PP recovery (amount with 25% variance; 0=full 30000)
 *   2 = HP+PP recovery (HP=amount*6; PP=amount; both 0=full 30000)
 *   3 = random stat boost (IQ/Guts/Speed/Vitality/Luck, 1 of 4 random)
 *   4 = boost IQ
 *   5 = boost Guts
 *   6 = boost Speed
 *   7 = boost Vitality
 *   8 = boost Luck
 *   9 = heal status (BTLACT_HEALING_A: cures cold/sunstroke/sleep)
 *  10 = cure poison (HEAL_POISON)
 * After the effect, if params[3] (special) != 0:
 *   calls SCHEDULE_PARTY_ANIMATION_RESET with (special * 6) frames.
 *
 * Non-Poo characters use params[1] (epi) as amount.
 * Poo uses params[2] (ep) as amount.
 * Stat boosts also increment the matching char_struct boosted_* field
 * and call recalc_character_postmath_*() to update the composite stat.
 * If target is unconscious, displays "no effect" and returns immediately.
 */
/* The @BOOST_* tails of eat_food.asm: bump the battler stat AND the
 * char_struct boosted_* field, recalc the composite stat, and hand back the
 * stat text. which: 0=IQ 1=Guts 2=Speed 3=Vitality 4=Luck (the random
 * effect rolls rand_limit(4), so it never picks Luck — matching the
 * assembly's jump table). */
static void eat_food_boost(uint16_t which, uint8_t amount,
                           BattleTailText *out) {
    Battler *tgt = battler_from_offset(bt.current_target);
    uint16_t char_id = tgt->id;
    uint16_t idx = char_id - 1;

    switch (which) {
    case 0: /* @BOOST_IQ: battler.iq (8-bit) + boosted_iq (8-bit) */
        tgt->iq += amount;
        party_characters[idx].boosted_iq += amount;
        recalc_character_postmath_iq(char_id);
        statmod_tail(out, MSG_BTL6_IQ_WENT_UP, amount);
        break;
    case 1: /* @BOOST_GUTS: battler.guts (16-bit) + boosted_guts (8-bit) */
        tgt->guts += (uint16_t)amount;
        party_characters[idx].boosted_guts += amount;
        recalc_character_postmath_guts(char_id);
        statmod_tail(out, MSG_BTL6_GUTS_WENT_UP, amount);
        break;
    case 2: /* @BOOST_SPEED: battler.speed (16-bit) + boosted_speed (8-bit) */
        tgt->speed += (uint16_t)amount;
        party_characters[idx].boosted_speed += amount;
        recalc_character_postmath_speed(char_id);
        statmod_tail(out, MSG_BTL6_SPEED_WENT_UP, amount);
        break;
    case 3: /* @BOOST_VITALITY: battler.vitality (8-bit) + boosted_vitality */
        tgt->vitality += amount;
        party_characters[idx].boosted_vitality += amount;
        recalc_character_postmath_vitality(char_id);
        statmod_tail(out, MSG_BTL6_VITALITY_WENT_UP, amount);
        break;
    case 4: /* @BOOST_LUCK: battler.luck (16-bit) + boosted_luck (8-bit) */
    default:
        tgt->luck += (uint16_t)amount;
        party_characters[idx].boosted_luck += amount;
        recalc_character_postmath_luck(char_id);
        statmod_tail(out, MSG_BTL6_LUCK_WENT_UP, amount);
        break;
    }
}

static StepResult btlact_eat_food_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    /* scratch16[0] = amount; scratch16[1] = effect_type | (special << 8).
     * The condiment params pointer is consumed entirely at pc 0 — no ROM
     * pointer crosses a yield. */
    for (;;) {
        switch (st->pc) {
        case 0: {
            Battler *tgt = battler_from_offset(bt.current_target);
            uint16_t char_id = tgt->id;  /* @LOCAL03: 1-indexed character ID */

            /* Assembly lines 20-30: if target is unconscious, show no-effect text */
            uint16_t idx = char_id - 1;
            if (party_characters[idx].afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL]
                    == STATUS_0_UNCONSCIOUS) {
                st->pc = 6;
                if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }

            /* Assembly lines 31-35: apply condiment, get params pointer.
             * The condiment text (tasted good / bad taste) is returned by the
             * prepare split and pushed below. */
            uint32_t condiment_msg;
            const uint8_t *params = battle_apply_condiment_prepare(&condiment_msg);
            if (!params)
                return STEP_RESULT_POP(0);

            /* Assembly lines 36-51: select amount field.
             * Poo (char_id == 4) uses params[2] (ep); others use params[1] (epi). */
            st->scratch16[0] = (char_id == PARTY_MEMBER_POO) ? params[2] : params[1];
            st->scratch16[1] = (uint16_t)(params[0] | ((uint16_t)params[3] << 8));

            st->pc = 1;
            if (condiment_msg != 0 && battle_push_text(&child, condiment_msg))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }

        case 1: {
            dt.blinking_triangle_flag = 0;
            Battler *tgt = battler_from_offset(bt.current_target);
            uint8_t amount = (uint8_t)st->scratch16[0];
            uint8_t effect_type = (uint8_t)st->scratch16[1];
            BattleTailText tail = {0};

            /* Assembly lines 55-76: dispatch on effect type */
            switch (effect_type) {
            case 0: /* HP recovery */
                battle_recover_hp_prepare(tgt, amount == 0
                        ? 30000
                        : battle_25pct_variance((uint16_t)amount * 6), &tail);
                break;

            case 1: /* PP recovery */
                battle_recover_pp_prepare(tgt, amount == 0
                        ? 30000
                        : battle_25pct_variance(amount), &tail);
                break;

            case 2: /* HP + PP recovery — HP portion; PP portion at pc 2 */
                battle_recover_hp_prepare(tgt, amount == 0
                        ? 30000
                        : battle_25pct_variance((uint16_t)amount * 6), &tail);
                st->pc = 2;
                if (tail.msg != 0 &&
                    battle_push_text_ex(&child, tail.msg, false, tail.has_cnum,
                                        tail.cnum))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                continue;  /* re-dispatch at pc 2 inline */

            case 3: /* Random stat boost (one of IQ/Guts/Speed/Vitality) */
                eat_food_boost(rand_limit(4), amount, &tail);
                break;

            case 4: eat_food_boost(0, amount, &tail); break;  /* IQ */
            case 5: eat_food_boost(1, amount, &tail); break;  /* Guts */
            case 6: eat_food_boost(2, amount, &tail); break;  /* Speed */
            case 7: eat_food_boost(3, amount, &tail); break;  /* Vitality */
            case 8: eat_food_boost(4, amount, &tail); break;  /* Luck */

            case 9: /* Heal status (BTLACT_HEALING_A: cures cold/sunstroke/sleep) */
                st->pc = 5;
                memset(&child, 0, sizeof(child));
                child.battle_action.table_index =
                    (uint16_t)btlact_find(0xC29AEA); /* healing_alpha */
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ACTION, &child);

            case 10: /* Cure poison */
                st->pc = 5;
                memset(&child, 0, sizeof(child));
                child.battle_action.table_index =
                    (uint16_t)btlact_find(0xC2A39D); /* heal_poison */
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ACTION, &child);

            default:
                break;
            }

            st->pc = 5;
            if (tail.msg != 0 &&
                battle_push_text_ex(&child, tail.msg, false, tail.has_cnum,
                                    tail.cnum))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }

        case 2: {
            /* HP+PP recovery, PP portion (re-reads amount, @LOCAL02 reload) */
            dt.blinking_triangle_flag = 0;
            Battler *tgt = battler_from_offset(bt.current_target);
            uint8_t amount = (uint8_t)st->scratch16[0];
            BattleTailText tail = {0};
            battle_recover_pp_prepare(tgt, amount == 0
                    ? 30000
                    : battle_25pct_variance(amount), &tail);
            st->pc = 5;
            if (tail.msg != 0 &&
                battle_push_text_ex(&child, tail.msg, false, tail.has_cnum,
                                    tail.cnum))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }

        case 5: {
            /* Assembly @CHECK_CONDIMENT_SPECIAL (lines 368-380):
             * If params[3] (special) != 0: schedule party animation reset
             * with (special * 6) frames of delay. */
            dt.blinking_triangle_flag = 0;
            uint8_t special = (uint8_t)(st->scratch16[1] >> 8);
            if (special != 0) {
                battle_schedule_party_animation_reset((uint16_t)special * 6);
            }
            return STEP_RESULT_POP(0);
        }

        case 6:  /* unconscious target — no check_special, like the blocking
                  * form's early return */
        default:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        }
    }
}



/* ======================================================================
 * Item damage actions
 * ====================================================================== */

/*
 * BTLACT_350_FIRE_DAMAGE (asm/battle/actions/350_fire_damage.asm)
 *
 * Fixed 350 fire damage with 25% variance, modified by fire resistance.
 */
static StepResult btlact_350_fire_damage_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0: {
        uint16_t damage = battle_25pct_variance(350);
        Battler *target = battler_from_offset(bt.current_target);
        st->pc = 1;
        battle_calc_make_init(&child, BC_RESIST_DAMAGE, damage,
                              target->fire_resist);
        return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
    }
    case 1:
    default:
        return STEP_RESULT_POP(0);
    }
}


/*
 * BTLACT_BAG_OF_DRAGONITE (asm/battle/actions/bag_of_dragonite.asm)
 *
 * Fixed 800 fire damage with 25% variance, modified by fire resistance.
 */
static StepResult btlact_bag_of_dragonite_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    if (st->pc == 0) {
        uint16_t damage = battle_25pct_variance(800);
        Battler *target = battler_from_offset(bt.current_target);
        st->pc = 1;
        battle_calc_make_init(&child, BC_RESIST_DAMAGE, damage,
                              target->fire_resist);
        return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
    }
    return STEP_RESULT_POP(0);
}


/*
 * BTLACT_YOGURT_DISPENSER (asm/battle/actions/yogurt_dispenser.asm)
 *
 * Speed-based check, then 1-4 damage.
 */
static StepResult btlact_yogurt_dispenser_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0:
        if (!battle_success_speed(250)) {
            st->pc = 2;
            if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            goto epilogue;
        }
        st->pc = 1;
        battle_calc_make_init(&child, BC_RESIST_DAMAGE, rand_limit(4) + 1, 0xFF);
        return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
    case 2:
    epilogue:
        dt.blinking_triangle_flag = 0;
        return STEP_RESULT_POP(0);
    case 1:
    default:
        return STEP_RESULT_POP(0);
    }
}


/*
 * BTLACT_SNAKE (asm/battle/actions/snake.asm)
 *
 * 1-4 damage, 50% chance to poison. Fails on NPCs.
 */
static StepResult btlact_snake_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0:
            st->pc = 1;
            battle_calc_make_init(&child, BC_FAIL_ON_NPCS, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 1:
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);
            if (!battle_success_speed(250)) {
                st->pc = 4;
                if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }
            st->pc = 2;
            battle_calc_make_init(&child, BC_RESIST_DAMAGE, rand_limit(4) + 1,
                                  0xFF);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 2:
            /* 50% chance to poison */
            if (battle_success_255(128)) {
                if (battle_inflict_status(
                        battler_from_offset(bt.current_target),
                        STATUS_GROUP_PERSISTENT_EASYHEAL,
                        STATUS_0_POISONED) != 0) {
                    st->pc = 3;
                    if (battle_push_text(&child, MSG_BTL5_STATUS_POISONED))
                        return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT,
                                                     &child);
                    break;
                }
            }
            return STEP_RESULT_POP(0);
        case 3:
        case 4:
        default:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        }
    }
}


/* ======================================================================
 * Additional status effect actions
 * ====================================================================== */

/*
 * BTLACT_COLD (asm/battle/actions/cold.asm)
 *
 * Inflict cold on target. Checks freeze_resist for success.
 * Fails on NPCs.
 */
static StepResult btlact_cold_step(BattleActionState *st) {
    uint32_t msg = st->pc == 0
        ? inflict_roll_decide(true, INFLICT_ROLL_RESIST_FREEZE,
                              STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_COLD,
                              MSG_BTL5_STATUS_COLD) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_INFLICT_POISON (asm/battle/actions/inflict_poison.asm)
 *
 * Inflict poison with paralysis_resist check. No NPC check.
 */
static StepResult btlact_inflict_poison_step(BattleActionState *st) {
    uint32_t msg = st->pc == 0
        ? inflict_roll_decide(false, INFLICT_ROLL_RESIST_PARALYSIS,
                              STATUS_GROUP_PERSISTENT_EASYHEAL,
                              STATUS_0_POISONED, MSG_BTL5_STATUS_POISONED) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_PARALYZE (asm/battle/actions/paralyze.asm)
 *
 * Inflict paralysis. Luck80 check + paralysis_resist check. Fails on NPCs.
 */
static uint32_t paralyze_decide(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->npc_id != 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;  /* battle_fail_attack_on_npcs */
    if (!battle_success_luck80())
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    if (!battle_success_255(target->paralysis_resist))
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    return battle_inflict_status(target, STATUS_GROUP_PERSISTENT_EASYHEAL,
                                 STATUS_0_PARALYZED) != 0
               ? MSG_BTL5_STATUS_NUMB : MSG_BTL4_RESULT_DID_NOT_WORK;
}

static StepResult btlact_paralyze_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? paralyze_decide() : 0);
}


/*
 * BTLACT_INFLICT_SOLIDIFICATION (asm/battle/actions/inflict_solidification.asm)
 *
 * Inflict solidified. Luck80 check + paralysis_resist check. No NPC check.
 */
static uint32_t inflict_solidification_decide(void) {
    if (!battle_success_luck80())
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist))
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    return battle_inflict_status(target, STATUS_GROUP_TEMPORARY,
                                 STATUS_2_SOLIDIFIED) != 0
               ? MSG_BTL5_STATUS_SOLIDIFIED : MSG_BTL4_RESULT_DID_NOT_WORK;
}

static StepResult btlact_inflict_solidification_step(BattleActionState *st) {
    return btlact_single_text_step(
        st, st->pc == 0 ? inflict_solidification_decide() : 0);
}


/*
 * BTLACT_COUNTER_PSI (asm/battle/actions/counter_psi.asm)
 *
 * Seal target's PSI for 4 turns. Luck40 check. Fails on NPCs.
 * Won't stack if already can't concentrate.
 */
static uint32_t counter_psi_decide(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->npc_id != 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;  /* battle_fail_attack_on_npcs */
    if (!battle_success_luck40())
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    if (target->afflictions[STATUS_GROUP_CONCENTRATION] != 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    target->afflictions[STATUS_GROUP_CONCENTRATION] = 4;
    return MSG_BTL5_STATUS_PSI_BLOCKED;
}

static StepResult btlact_counter_psi_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? counter_psi_decide() : 0);
}


/*
 * BTLACT_DISTRACT (asm/battle/actions/distract.asm)
 *
 * Make target unable to concentrate for 4 turns.
 * Luck40 + paralysis_resist check. Fails on NPCs.
 * Sets CANT_CONCENTRATE4 (value 4) if concentration slot is empty.
 */
static uint32_t distract_decide(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->npc_id != 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;  /* battle_fail_attack_on_npcs */
    if (!battle_success_luck40())
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    if (!battle_success_255(target->paralysis_resist))
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    if (target->afflictions[STATUS_GROUP_CONCENTRATION] != 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    target->afflictions[STATUS_GROUP_CONCENTRATION] = STATUS_4_CANT_CONCENTRATE4;
    return MSG_BTL5_STATUS_PSI_BLOCKED;
}

static StepResult btlact_distract_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? distract_decide() : 0);
}


/*
 * BTLACT_NEUTRALIZE (asm/battle/actions/neutralize.asm)
 *
 * Reset all combat stats to base values, remove shields.
 */
static uint32_t neutralize_decide(void) {
    Battler *target = battler_from_offset(bt.current_target);
    target->offense = target->base_offense;
    target->defense = target->base_defense;
    target->speed   = target->base_speed;
    target->guts    = target->base_guts;
    target->luck    = target->base_luck;
    target->shield_hp = 0;
    target->afflictions[STATUS_GROUP_SHIELD] = 0;
    return MSG_BTL5_PSI_EFFECTS_NEUTRALIZED;
}

static StepResult btlact_neutralize_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? neutralize_decide() : 0);
}



/*
 * HEAL_POISON (asm/battle/actions/heal_poison.asm)
 *
 * Cure poison status (group 0 value 5) from current target.
 */
static uint32_t heal_poison_decide(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] != STATUS_0_POISONED)
        return 0;  /* no text */
    target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
    return MSG_BTL5_CURED_POISONED;
}

static StepResult btlact_heal_poison_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? heal_poison_decide() : 0);
}


/*
 * BTLACT_SHIELD_KILLER (asm/battle/actions/shield_killer.asm)
 *
 * Remove shield from target. Luck80 check.
 */
static uint32_t shield_killer_decide(void) {
    if (!battle_success_luck80())
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    Battler *target = battler_from_offset(bt.current_target);
    if (target->afflictions[STATUS_GROUP_SHIELD] == 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    target->afflictions[STATUS_GROUP_SHIELD] = 0;
    return MSG_BTL5_SHIELD_DISAPPEARED;
}

static StepResult btlact_shield_killer_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? shield_killer_decide() : 0);
}


/* ======================================================================
 * Redirect wrappers (enemy reuse of player PSI)
 * ====================================================================== */

/* Additional redirect copies (asm/battle/actions/ copy and redirect variants) */

/* ======================================================================
 * Diamondize
 * ====================================================================== */

/*
 * BTLACT_DIAMONDIZE (asm/battle/actions/diamondize.asm)
 *
 * Turn target to diamond. Clears all non-persistent statuses.
 * Accumulates exp/money from diamondized enemy. Fails on NPCs.
 * Uses paralysis_resist for chance check.
 */
static uint32_t diamondize_decide(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->npc_id != 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;  /* battle_fail_attack_on_npcs */
    if (!battle_success_255(target->paralysis_resist))
        return MSG_BTL4_RESULT_DID_NOT_WORK;
    if (battle_inflict_status(target, STATUS_GROUP_PERSISTENT_EASYHEAL,
                              STATUS_0_DIAMONDIZED) == 0)
        return MSG_BTL4_RESULT_DID_NOT_WORK;

    /* Clear all other status groups */
    target->afflictions[STATUS_GROUP_SHIELD] = 0;
    target->afflictions[STATUS_GROUP_HOMESICKNESS] = 0;
    target->afflictions[STATUS_GROUP_CONCENTRATION] = 0;
    target->afflictions[STATUS_GROUP_STRANGENESS] = 0;
    target->afflictions[STATUS_GROUP_TEMPORARY] = 0;
    target->afflictions[STATUS_GROUP_PERSISTENT_HARDHEAL] = 0;

    /* Accumulate exp and money reward */
    bt.battle_exp_scratch += target->exp;
    bt.battle_money_scratch += target->money;

    return MSG_BTL5_STATUS_DIAMONDIZED;
}

static StepResult btlact_diamondize_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? diamondize_decide() : 0);
}


/*
 * BTLACT_POSSESS (asm/battle/actions/possess.asm)
 *
 * Possesses target (ally only). Inflicts POSSESSED status.
 * If the first enemy slot (index TOTAL_PARTY_COUNT) is empty (unconscious),
 * spawns a Tiny Lil' Ghost there as an NPC ally for the possessor.
 */
static StepResult btlact_possess_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0: {
        Battler *target = battler_from_offset(bt.current_target);
        uint32_t msg = MSG_BTL4_RESULT_DID_NOT_WORK;
        bool possessed = false;
        if (target->npc_id != 0) {
            /* battle_fail_attack_on_npcs */
        } else if ((target->ally_or_enemy & 0xFF) != 0) {
            /* Only works on allies (party members) */
        } else if (battle_inflict_status(target,
                       STATUS_GROUP_PERSISTENT_HARDHEAL,
                       STATUS_1_POSSESSED) != 0) {
            msg = MSG_BTL5_STATUS_POSSESSED_GHOST;
            possessed = true;
        }
        st->pc = possessed ? 2 : 1;
        if (battle_push_text(&child, msg))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
        /* FALLTHROUGH — unresolvable text: resume inline */
        if (!possessed) {
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        }
    }
    /* FALLTHROUGH */
    case 2:
        dt.blinking_triangle_flag = 0;
        /* If first enemy slot is empty, spawn Tiny Lil' Ghost as NPC ally
         * (after the text, as in the blocking form) */
        if ((bt.battlers_table[TOTAL_PARTY_COUNT].consciousness & 0xFF) == 0) {
            battle_init_enemy_stats(&bt.battlers_table[TOTAL_PARTY_COUNT],
                                    ENEMY_TINY_LIL_GHOST);
            bt.battlers_table[TOTAL_PARTY_COUNT].npc_id = ENEMY_TINY_LIL_GHOST;
            bt.battlers_table[TOTAL_PARTY_COUNT].has_taken_turn = 1;
        }
        return STEP_RESULT_POP(0);
    case 1:
    default:
        dt.blinking_triangle_flag = 0;
        return STEP_RESULT_POP(0);
    }
}



uint16_t find_stealable_items(void) {
    uint16_t count = 0;

    for (uint16_t party_idx = 0; party_idx < TOTAL_PARTY_COUNT; party_idx++) {
        uint16_t char_id = game_state.party_members[party_idx] & 0xFF;
        if (char_id < 1 || char_id > 4) continue;

        /* Find the battler for this character to get action_item_slot */
        uint16_t action_slot = 0;
        for (uint16_t b = 0; b < TOTAL_PARTY_COUNT; b++) {
            Battler *b_ptr = &bt.battlers_table[b];
            if ((b_ptr->consciousness & 0xFF) == 0) continue;
            if (b_ptr->id != char_id) continue;
            if ((b_ptr->npc_id & 0xFF) != 0) continue;
            action_slot = b_ptr->action_item_slot & 0xFF;
            break;
        }

        CharStruct *cs = &party_characters[char_id - 1];
        for (uint16_t slot = 0; slot < 14; slot++) {
            /* Skip the item being used this turn (1-based) */
            if ((slot + 1) == action_slot) continue;

            uint8_t item_id = cs->items[slot];
            if (item_id == 0) continue;

            const ItemConfig *entry = get_item_entry(item_id);
            if (!entry) continue;

            /* Cost must be > 0 and < 290 */
            uint16_t cost = entry->cost;
            if (cost == 0 || cost >= 290) continue;

            /* Item type bits 4-5 must be 0x20 */
            uint8_t type = entry->type;
            if ((type & 0x30) != 0x20) continue;

            /* Must not be currently equipped (equipment stores 1-based slot) */
            uint16_t slot_1 = slot + 1;
            bool equipped = false;
            for (int e = 0; e < 4; e++) {
                if ((cs->equipment[e] & 0xFF) == slot_1) {
                    equipped = true;
                    break;
                }
            }
            if (equipped) continue;

            stealable_item_candidates[count] = item_id;
            count++;
        }
    }

    return count;
}

/*
 * SELECT_STEALABLE_ITEM (asm/battle/select_stealable_item.asm)
 *
 * Calls FIND_STEALABLE_ITEMS, then with 50% probability picks a random
 * item from the candidates. Returns 0 if no items or failed the coin flip.
 */
uint16_t select_stealable_item(void) {
    uint16_t count = find_stealable_items();
    if (count == 0) return 0;
    /* 50% chance to fail: bit 7 of rand [0-255] */
    if (rand_byte() & 0x80) return 0;
    uint16_t idx = rand_limit(count);
    return stealable_item_candidates[idx];
}

/*
 * IS_ITEM_STEALABLE (asm/battle/is_item_stealable.asm)
 *
 * Checks if a specific item ID is in the current stealable candidates list.
 * Calls FIND_STEALABLE_ITEMS, then searches the list.
 * Returns 1 if found, 0 if not.
 */
uint16_t is_item_stealable(uint16_t item_id) {
    uint16_t count = find_stealable_items();
    for (uint16_t i = 0; i < count; i++) {
        if (stealable_item_candidates[i] == (uint8_t)item_id)
            return 1;
    }
    return 0;
}

/*
 * BTLACT_STEAL (asm/battle/actions/steal.asm)
 *
 * Steal an item from the attacker and give it to the enemy team.
 * Fails if: target is an enemy (ally_or_enemy==1), target is an NPC,
 * or attacker is mirrored Poo (MIRROR_ENEMY active, attacker is ally with id==4).
 * Uses action_argument as item to steal, 0xFF as char_id (any character).
 */
void btlact_steal(void) {
    Battler *target = battler_from_offset(bt.current_target);
    /* Only steal from enemies, not allies */
    if ((target->ally_or_enemy & 0xFF) == 1)
        return;
    /* NPC allies can't be stolen from */
    if ((target->npc_id & 0xFF) != 0)
        return;
    /* If mirror is active, don't let mirrored Poo steal */
    if (bt.mirror_enemy != 0) {
        Battler *attacker = battler_from_offset(bt.current_attacker);
        if ((attacker->ally_or_enemy & 0xFF) == 0 && attacker->id == PARTY_MEMBER_POO)
            return;
    }
    /* Get item to steal */
    Battler *attacker = battler_from_offset(bt.current_attacker);
    uint8_t item_id = attacker->current_action_argument & 0xFF;
    if (item_id == 0)
        return;
    take_item_from_character(CHAR_ID_ANY, (uint16_t)item_id);
}

/* ======================================================================
 * Reduce PP
 * ====================================================================== */

/*
 * BTLACT_REDUCEPP (asm/battle/actions/reduce_pp.asm)
 *
 * Drain target's PP by pp_max/16 with 50% variance.
 * If target has 0 PP, display "no PP" message.
 */
static void reduce_pp_decide(BattleTailText *out) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->pp_target == 0) {
        out->msg = MSG_BTL6_TARGET_HAS_NO_PP;
        return;
    }
    uint16_t drain = target->pp_max / 16;
    if (drain == 0) {
        out->msg = MSG_BTL4_RESULT_DID_NOT_WORK;
        return;
    }
    drain = battle_50pct_variance(drain);
    battle_reduce_pp(target, drain);
    statmod_tail(out, MSG_BTL4_RESULT_PP_LOST, drain);
}

static StepResult btlact_reduce_pp_step(BattleActionState *st) {
    return btlact_statmod_step(st, reduce_pp_decide);
}


/*
 * BTLACT_MAGNET_A (asm/battle/actions/magnet_alpha.asm)
 *
 * PP drain attack: drains 2-9 PP from target and adds it to attacker.
 * If target has 0 PP, shows "no PP" message. Drain amount is
 * rand_limit(4) + rand_limit(4) + 2, clamped to target's current PP.
 */
static StepResult btlact_magnet_a_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0: {
        Battler *target = battler_from_offset(bt.current_target);
        if (target->pp_target == 0) {
            st->pc = 1;
            if (battle_push_text(&child, MSG_BTL6_TARGET_HAS_NO_PP))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            goto epilogue;
        }
        /* Assembly lines 15-28: drain = rand(4) + rand(4) + 2 → range [2..9] */
        uint16_t drain = rand_limit(4) + rand_limit(4) + 2;
        /* Clamp to target's actual PP */
        if (target->pp_target < drain)
            drain = target->pp_target;
        st->scratch16[0] = drain;
        st->pc = 2;
        if (battle_push_text_ex(&child, MSG_BTL4_RESULT_PP_DRAINED_FROM,
                                false, true, drain))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
    }
    /* FALLTHROUGH */
    case 2: {
        /* The drain itself runs after the text, as in the blocking form */
        dt.blinking_triangle_flag = 0;
        uint16_t drain = st->scratch16[0];
        battle_reduce_pp(battler_from_offset(bt.current_target), drain);
        /* Add drained PP to attacker */
        Battler *attacker = battler_from_offset(bt.current_attacker);
        battle_set_pp(attacker, attacker->pp_target + drain);
        return STEP_RESULT_POP(0);
    }
    case 1:
    epilogue:
    default:
        dt.blinking_triangle_flag = 0;
        return STEP_RESULT_POP(0);
    }
}


/*
 * BTLACT_MAGNET_O (asm/battle/actions/magnet_omega.asm)
 *
 * Same as Magnet Alpha, but skips if target is an ally and is Jeff
 * (Jeff has no PP to drain).
 */
static StepResult btlact_magnet_o_step(BattleActionState *st) {
    if (st->pc == 0) {
        Battler *target = battler_from_offset(bt.current_target);
        if ((target->ally_or_enemy & 0xFF) == 0 &&
            target->id == PARTY_MEMBER_JEFF)
            return STEP_RESULT_POP(0);
    }
    return btlact_magnet_a_step(st);
}


/* ======================================================================
 * Physical + status combo attacks
 * ====================================================================== */

/*
 * BTLACT_HANDBAG_STRAP (asm/battle/actions/handbag_strap.asm)
 *
 * Fixed damage (100 - defense), then inflict solidified.
 * Speed check. Fails on NPCs. If damage <= 0, "no effect".
 */
static StepResult btlact_strap_step_common(BattleActionState *st,
                                           int16_t base_damage) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0:
            st->pc = 1;
            battle_calc_make_init(&child, BC_FAIL_ON_NPCS, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 1: {
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);
            if (!battle_success_speed(250)) {
                st->pc = 4;
                if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }
            Battler *target = battler_from_offset(bt.current_target);
            int16_t damage = base_damage - (int16_t)target->defense;
            if (damage <= 0) {
                st->pc = 4;
                if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }
            st->pc = 2;
            battle_calc_make_init(&child, BC_RESIST_DAMAGE, (uint16_t)damage,
                                  0xFF);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        }
        case 2:
            if (battle_inflict_status(battler_from_offset(bt.current_target),
                                      STATUS_GROUP_TEMPORARY,
                                      STATUS_2_SOLIDIFIED) != 0) {
                st->pc = 3;
                if (battle_push_text(&child, MSG_BTL5_STATUS_SOLIDIFIED))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }
            return STEP_RESULT_POP(0);
        case 3:
        case 4:
        default:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        }
    }
}

static StepResult btlact_handbag_strap_step(BattleActionState *st) {
    return btlact_strap_step_common(st, HANDBAG_STRAP_BASE_DAMAGE);
}


/*
 * BTLACT_MUMMY_WRAP (asm/battle/actions/mummy_wrap.asm)
 *
 * Same as handbag_strap but with 400 base damage.
 */
static StepResult btlact_mummy_wrap_step(BattleActionState *st) {
    return btlact_strap_step_common(st, MUMMY_WRAP_BASE_DAMAGE);
}


/* ======================================================================
 * Fly Honey (Master Belch weakener)
 * ====================================================================== */

/*
 * BTLACT_FLY_HONEY (asm/battle/actions/fly_honey.asm)
 *
 * Searches all enemy battlers for Master Belch (IDs 93 or 192).
 * If found, transforms them to the weakened variant (ID 169).
 */
static uint32_t fly_honey_decide(void) {
    for (uint16_t i = FIRST_ENEMY_INDEX; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0)
            continue;
        if (b->ally_or_enemy != 1)
            continue;
        if (b->id == ENEMY_MASTER_BELCH_1 || b->id == ENEMY_MASTER_BELCH_3) {
            b->id = ENEMY_MASTER_BELCH_2;
            return MSG_BTL6_FLY_HONEY_BELCH_GRABS; /* fly honey worked! */
        }
    }
    return MSG_BTL6_FLY_HONEY_BELCH_IGNORED; /* no Master Belch found */
}

static StepResult btlact_fly_honey_step(BattleActionState *st) {
    return btlact_single_text_step(st, st->pc == 0 ? fly_honey_decide() : 0);
}


/* ======================================================================
 * PSI Flash
 * ====================================================================== */

/* FLASH_IMMUNITY_TEST (asm/battle/actions/psi_flash_immunity_test.asm) — the
 * standalone blocking form (PSI shield nullify + flash_resist test) was deleted
 * with pump_mode at cutover; its logic is inlined into the PSI Flash steppers
 * (btlact_psi_flash_*_step), which STEP_PUSH BC_PSI_SHIELD_NULLIFY. */

/*
 * BTLACT_PSI_FLASH_A (asm/battle/actions/psi_flash_alpha.asm)
 *
 * PSI Flash α: 1/8 chance of "feeling strange", 7/8 chance of crying.
 * Fails on NPCs.
 */
/*
 * Shared resumable form. Effect mapping by the 0-7 roll (the per-tier
 * thresholds): roll <= ko_max — KO; roll == para_idx — paralysis;
 * roll == strange_idx — feeling strange; otherwise crying. -1 disables a
 * branch (alpha has no KO/paralysis). flash_immunity_test()'s halves are
 * inlined: the BC_PSI_SHIELD_NULLIFY push, then the flash_resist roll with
 * its "didn't work" text. The crying infliction uses the group-equals-ID
 * idiom (see flash_inflict_crying / psi_flash_crying.asm). The KO outcome
 * is a BATTLE_KO child push.
 */
static StepResult btlact_psi_flash_step_common(BattleActionState *st,
                                               int16_t ko_max,
                                               int16_t para_idx,
                                               int16_t strange_idx) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0:
            st->pc = 1;
            battle_calc_make_init(&child, BC_FAIL_ON_NPCS, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 1:
            if (mode_child_result() != 0)
                return STEP_RESULT_POP(0);
            /* flash_immunity_test: shield nullify first... */
            st->pc = 2;
            battle_calc_make_init(&child, BC_PSI_SHIELD_NULLIFY, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 2: {
            if (mode_child_result() != 0) {
                st->pc = 5;  /* nullified: straight to weaken */
                break;
            }
            /* ...then the flash resist roll */
            Battler *target = battler_from_offset(bt.current_target);
            if (!battle_success_255(target->flash_resist)) {
                st->pc = 3;
                if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }

            /* Effect roll */
            int16_t roll = (int16_t)(rand_byte() & 0x07);
            if (roll <= ko_max) {
                /* KO outcome: a BATTLE_KO child push, resuming at the
                 * shield-weaken pc */
                st->pc = 5;
                battle_ko_make_init(&child, bt.current_target);
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_KO, &child);
            }
            uint32_t msg;
            if (roll == para_idx) {
                msg = inflict_decide(false, STATUS_GROUP_PERSISTENT_EASYHEAL,
                                     STATUS_0_PARALYZED, MSG_BTL5_STATUS_NUMB);
            } else if (roll == strange_idx) {
                msg = inflict_decide(false, STATUS_GROUP_STRANGENESS,
                                     STATUS_3_STRANGE, MSG_BTL5_STATUS_STRANGE);
            } else {
                msg = inflict_decide(false, STATUS_2_CRYING, STATUS_2_CRYING,
                                     MSG_BTL5_STATUS_CRYING);
            }
            st->pc = 4;
            if (battle_push_text(&child, msg))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 3:
        case 4:
            dt.blinking_triangle_flag = 0;
            st->pc = 5;
            break;
        case 5:
            st->pc = 6;
            battle_calc_make_init(&child, BC_WEAKEN_SHIELD, 0, 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        case 6:
        default:
            return STEP_RESULT_POP(0);
        }
    }
}

static StepResult btlact_psi_flash_alpha_step(BattleActionState *st) {
    return btlact_psi_flash_step_common(st, -1, -1, 0);
}


/*
 * BTLACT_PSI_FLASH_B (asm/battle/actions/psi_flash_beta.asm)
 *
 * PSI Flash β: 1/8 KO, 1/8 paralysis, 1/8 strange, 5/8 crying.
 */
static StepResult btlact_psi_flash_beta_step(BattleActionState *st) {
    return btlact_psi_flash_step_common(st, 0, 1, 2);
}


/*
 * BTLACT_PSI_FLASH_G (asm/battle/actions/psi_flash_gamma.asm)
 *
 * PSI Flash γ: 2/8 KO, 1/8 paralysis, 1/8 strange, 4/8 crying.
 */
static StepResult btlact_psi_flash_gamma_step(BattleActionState *st) {
    return btlact_psi_flash_step_common(st, 1, 2, 3);
}


/*
 * BTLACT_PSI_FLASH_O (asm/battle/actions/psi_flash_omega.asm)
 *
 * PSI Flash Ω: 3/8 KO, 1/8 paralysis, 1/8 strange, 3/8 crying.
 */
static StepResult btlact_psi_flash_omega_step(BattleActionState *st) {
    return btlact_psi_flash_step_common(st, 2, 3, 4);
}



/*
 * AUTOHEALING (asm/battle/autohealing.asm)
 *
 * Scans party_members[0..5] for NESS..POO who have unknown94==0 and
 * afflictions[status_group]==status_id. Returns the 1-based member ID of
 * the one with the lowest current_hp_target (and sets their unknown94=1),
 * or 0 if none found.
 */
uint16_t autohealing(uint16_t status_group, uint16_t status_id) {
    uint16_t best_hp = 9999;
    uint16_t best_member = 0;

    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        uint8_t member = game_state.party_members[i];
        if (member < PARTY_MEMBER_NESS || member > PARTY_MEMBER_POO)
            continue;

        CharStruct *ch = &party_characters[member - 1];
        if (ch->unknown94 != 0)
            continue;
        if (ch->afflictions[status_group] != status_id)
            continue;
        if (ch->current_hp_target >= best_hp)
            continue;

        best_hp = ch->current_hp_target;
        best_member = member;
    }

    if (best_member != 0) {
        party_characters[best_member - 1].unknown94 = 1;
    }
    return best_member;
}

/*
 * AUTOLIFEUP (asm/battle/autolifeup.asm)
 *
 * Scans party_members[0..5] for NESS..POO who have unknown94==0,
 * are not unconscious, and have current_hp_target < max_hp/4.
 * Returns the 1-based member ID of the one with the lowest HP
 * (and sets their unknown94=1), or 0 if none found.
 */
uint16_t autolifeup(void) {
    uint16_t best_hp = 9999;
    uint16_t best_member = 0;

    for (int i = 0; i < TOTAL_PARTY_COUNT; i++) {
        uint8_t member = game_state.party_members[i];
        if (member < PARTY_MEMBER_NESS || member > PARTY_MEMBER_POO)
            continue;

        CharStruct *ch = &party_characters[member - 1];
        if (ch->unknown94 != 0)
            continue;
        if (ch->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS)
            continue;

        uint16_t threshold = ch->max_hp >> 2;
        if (ch->current_hp_target >= threshold)
            continue;
        if (ch->current_hp_target >= best_hp)
            continue;

        best_hp = ch->current_hp_target;
        best_member = member;
    }

    if (best_member != 0) {
        party_characters[best_member - 1].unknown94 = 1;
    }
    return best_member;
}

/* ======================================================================
 * Status effect battle actions — resist-checked
 * ====================================================================== */

/*
 * BTLACT_CRYING (asm/battle/actions/crying.asm)
 *
 * Inflict crying on target. Checks flash_resist for success.
 * Fails on NPCs.
 */
static StepResult btlact_crying_step(BattleActionState *st) {
    uint32_t msg = st->pc == 0
        ? inflict_roll_decide(true, INFLICT_ROLL_RESIST_FLASH,
                              STATUS_GROUP_TEMPORARY, STATUS_2_CRYING,
                              MSG_BTL5_STATUS_CRYING) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_CRYING2 (asm/battle/actions/crying2.asm)
 *
 * Inflict crying on target without resist check.
 * Fails on NPCs. Status group is same as status ID.
 */
static StepResult btlact_crying2_step(BattleActionState *st) {
    /* NOTE: passes STATUS_2_CRYING as the status GROUP — crying2.asm does
     * TYX ("Status group is identical to status ID"). */
    uint32_t msg = st->pc == 0
        ? inflict_decide(true, STATUS_2_CRYING, STATUS_2_CRYING,
                         MSG_BTL5_STATUS_CRYING) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_SOLIDIFY (asm/battle/actions/solidify.asm)
 *
 * Inflict solidified on target. Luck80 check. Fails on NPCs.
 */
static StepResult btlact_solidify_step(BattleActionState *st) {
    uint32_t msg = st->pc == 0
        ? inflict_roll_decide(true, INFLICT_ROLL_LUCK80,
                              STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED,
                              MSG_BTL5_STATUS_SOLIDIFIED) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_SOLIDIFY_2 (asm/battle/actions/solidify_2.asm)
 *
 * Inflict solidified on target. Luck80 check. No NPC check.
 */
static StepResult btlact_solidify_2_step(BattleActionState *st) {
    /* No NPC check — faithful to the blocking form. */
    uint32_t msg = st->pc == 0
        ? inflict_roll_decide(false, INFLICT_ROLL_LUCK80,
                              STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED,
                              MSG_BTL5_STATUS_SOLIDIFIED) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_MUSHROOMIZE (asm/battle/actions/mushroomize.asm)
 *
 * Inflict mushroomized on target. No resist check.
 * Fails on NPCs. Status group is same as status ID.
 */
static StepResult btlact_mushroomize_step(BattleActionState *st) {
    /* NOTE: passes STATUS_1_MUSHROOMIZED as the status GROUP — the same
     * group-equals-ID assembly idiom as crying2. */
    uint32_t msg = st->pc == 0
        ? inflict_decide(true, STATUS_1_MUSHROOMIZED, STATUS_1_MUSHROOMIZED,
                         MSG_BTL5_STATUS_FEEL_STRANGE) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_PARALYSIS_A (asm/battle/actions/paralysis_alpha.asm)
 *
 * PSI Paralysis α: Inflict paralysis with resist check via paralysis_resist.
 * Fails on NPCs.
 */
static StepResult btlact_paralysis_alpha_step(BattleActionState *st) {
    uint32_t msg = st->pc == 0
        ? inflict_roll_decide(true, INFLICT_ROLL_RESIST_PARALYSIS,
                              STATUS_GROUP_PERSISTENT_EASYHEAL,
                              STATUS_0_PARALYZED, MSG_BTL5_STATUS_NUMB) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_HYPNOSIS_A (asm/battle/actions/hypnosis_alpha.asm)
 *
 * PSI Hypnosis α: Inflict sleep with resist check via hypnosis_resist.
 * Fails on NPCs.
 */
static StepResult btlact_hypnosis_alpha_step(BattleActionState *st) {
    uint32_t msg = st->pc == 0
        ? inflict_roll_decide(true, INFLICT_ROLL_RESIST_HYPNOSIS,
                              STATUS_GROUP_TEMPORARY, STATUS_2_ASLEEP,
                              MSG_BTL5_STATUS_ASLEEP) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_BRAINSHOCK_A (asm/battle/actions/brainshock_alpha.asm)
 *
 * PSI Brainshock α: Inflict "strange" with resist check via brainshock_resist.
 * Fails on NPCs.
 */
static StepResult btlact_brainshock_alpha_step(BattleActionState *st) {
    uint32_t msg = st->pc == 0
        ? inflict_roll_decide(true, INFLICT_ROLL_RESIST_BRAINSHOCK,
                              STATUS_GROUP_STRANGENESS, STATUS_3_STRANGE,
                              MSG_BTL5_STATUS_STRANGE) : 0;
    return btlact_single_text_step(st, msg);
}



/* ======================================================================
 * Stat modification battle actions
 *
 * All are "decide + mutate, then one tail text" shapes: a per-action
 * decide fills a BattleTailText at pc 0 (the NPC-fail and luck-fail paths
 * return the plain "did not work" text, has_cnum = false; the success
 * paths return the stat message with the change amount as cnum, matching
 * display_text_wait_addr), then the shared single-text stepper runs it
 * (btlact_statmod_step, defined with the single-text helpers above).
 * reduce_offense_defense is the one two-text exception (own pc machine).
 * ====================================================================== */

/*
 * BTLACT_OFFENSE_UP_A (asm/battle/actions/offense_up_alpha.asm)
 *
 * Increase target's offense by 1/16th and display the change amount.
 * Fails on NPCs.
 */
static void offense_up_alpha_decide(BattleTailText *out) {
    Battler *target = battler_from_offset(bt.current_target);
    if (statmod_npc_fail(out, target))
        return;
    uint16_t old_offense = target->offense;
    battle_increase_offense(target);
    statmod_tail(out, MSG_BTL6_OFFENSE_WENT_UP, target->offense - old_offense);
}

static StepResult btlact_offense_up_alpha_step(BattleActionState *st) {
    return btlact_statmod_step(st, offense_up_alpha_decide);
}


/*
 * BTLACT_DEFENSE_DOWN_A (asm/battle/actions/defense_down_alpha.asm)
 *
 * Decrease target's defense by 1/16th. Luck80 check for success.
 * Fails on NPCs. Displays the reduction amount (clamped to >= 0).
 */
static void defense_down_alpha_decide(BattleTailText *out) {
    Battler *target = battler_from_offset(bt.current_target);
    if (statmod_npc_fail(out, target))
        return;
    if (!battle_success_luck80()) {
        out->msg = MSG_BTL4_RESULT_DID_NOT_WORK;
        return;
    }
    uint16_t old_defense = target->defense;
    battle_decrease_defense(target);
    int16_t diff = (int16_t)(old_defense - target->defense);
    if (diff < 0)
        diff = 0;
    statmod_tail(out, MSG_BTL6_DEFENSE_WENT_DOWN, (uint32_t)diff);
}

static StepResult btlact_defense_down_alpha_step(BattleActionState *st) {
    return btlact_statmod_step(st, defense_down_alpha_decide);
}


/*
 * BTLACT_SPEED_UP_1D4 / GUTS / VITALITY / IQ / LUCK
 * (asm/battle/actions/{speed,guts,vitality,iq,luck}_up_1d4.asm)
 *
 * Increase the stat by 1-4 points (random). Vitality and IQ are 8-bit
 * adds. No NPC check.
 */
static void speed_up_1d4_decide(BattleTailText *out) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->speed += amount;
    statmod_tail(out, MSG_BTL6_SPEED_WENT_UP, amount);
}

static void guts_up_1d4_decide(BattleTailText *out) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->guts += amount;
    statmod_tail(out, MSG_BTL6_GUTS_WENT_UP, amount);
}

static void vitality_up_1d4_decide(BattleTailText *out) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->vitality += (uint8_t)amount;
    statmod_tail(out, MSG_BTL6_VITALITY_WENT_UP, amount);
}

static void iq_up_1d4_decide(BattleTailText *out) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->iq += (uint8_t)amount;
    statmod_tail(out, MSG_BTL6_IQ_WENT_UP, amount);
}

static void luck_up_1d4_decide(BattleTailText *out) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->luck += amount;
    statmod_tail(out, MSG_BTL6_LUCK_WENT_UP, amount);
}

static StepResult btlact_speed_up_1d4_step(BattleActionState *st) {
    return btlact_statmod_step(st, speed_up_1d4_decide);
}
static StepResult btlact_guts_up_1d4_step(BattleActionState *st) {
    return btlact_statmod_step(st, guts_up_1d4_decide);
}
static StepResult btlact_vitality_up_1d4_step(BattleActionState *st) {
    return btlact_statmod_step(st, vitality_up_1d4_decide);
}
static StepResult btlact_iq_up_1d4_step(BattleActionState *st) {
    return btlact_statmod_step(st, iq_up_1d4_decide);
}
static StepResult btlact_luck_up_1d4_step(BattleActionState *st) {
    return btlact_statmod_step(st, luck_up_1d4_decide);
}


/*
 * BTLACT_RANDOM_STAT_UP_1D4 (asm/battle/actions/random_stat_up_1d4.asm)
 *
 * Randomly boosts one of seven stats by 1-4 points.
 * Stat selection: 0=defense, 1=offense, 2=speed, 3=guts, 4=vitality, 5=IQ, 6=luck.
 */
static void random_stat_up_1d4_decide(BattleTailText *out) {
    uint16_t stat = rand_limit(7);
    switch (stat) {
    case 0: { /* Defense */
        uint16_t amount = rand_limit(4) + 1;
        Battler *target = battler_from_offset(bt.current_target);
        target->defense += amount;
        statmod_tail(out, MSG_BTL6_DEFENSE_WENT_UP, amount);
        break;
    }
    case 1: { /* Offense */
        uint16_t amount = rand_limit(4) + 1;
        Battler *target = battler_from_offset(bt.current_target);
        target->offense += amount;
        statmod_tail(out, MSG_BTL6_OFFENSE_WENT_UP, amount);
        break;
    }
    case 2: speed_up_1d4_decide(out); break;
    case 3: guts_up_1d4_decide(out); break;
    case 4: vitality_up_1d4_decide(out); break;
    case 5: iq_up_1d4_decide(out); break;
    case 6: luck_up_1d4_decide(out); break;
    }
}

static StepResult btlact_random_stat_up_1d4_step(BattleActionState *st) {
    return btlact_statmod_step(st, random_stat_up_1d4_decide);
}


/*
 * BTLACT_REDUCEOFF (asm/battle/actions/reduce_offense.asm)
 *
 * Decrease target's offense by 1/16th and display the reduction.
 * Fails on NPCs.
 */
static void reduce_offense_decide(BattleTailText *out) {
    Battler *target = battler_from_offset(bt.current_target);
    if (statmod_npc_fail(out, target))
        return;
    uint16_t old_offense = target->offense;
    battle_decrease_offense(target);
    statmod_tail(out, MSG_BTL6_OFFENSE_WENT_DOWN, old_offense - target->offense);
}

static StepResult btlact_reduce_offense_step(BattleActionState *st) {
    return btlact_statmod_step(st, reduce_offense_decide);
}


/*
 * BTLACT_REDUCEOFFDEF (asm/battle/actions/reduce_offense_defense.asm)
 *
 * Decrease target's offense and defense each by 1/16th.
 * Displays both changes separately. Fails on NPCs. The defense decrement
 * runs after the offense text completes, as in the blocking form.
 */
static StepResult btlact_reduce_offense_defense_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0: {
        Battler *target = battler_from_offset(bt.current_target);
        if (target->npc_id != 0) {
            /* battle_fail_attack_on_npcs */
            st->pc = 3;
            if (battle_push_text(&child, MSG_BTL4_RESULT_DID_NOT_WORK))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            goto epilogue;
        }

        /* Reduce offense */
        uint16_t old_offense = target->offense;
        battle_decrease_offense(target);
        uint16_t off_diff = old_offense - target->offense;
        st->pc = 1;
        if (battle_push_text_ex(&child, MSG_BTL6_OFFENSE_WENT_DOWN, false,
                                true, off_diff))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
    }
    /* FALLTHROUGH */
    case 1: {
        dt.blinking_triangle_flag = 0;

        /* Reduce defense */
        Battler *target = battler_from_offset(bt.current_target);
        uint16_t old_defense = target->defense;
        battle_decrease_defense(target);
        uint16_t def_diff = old_defense - target->defense;
        st->pc = 2;
        if (battle_push_text_ex(&child, MSG_BTL6_DEFENSE_WENT_DOWN, false,
                                true, def_diff))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
    }
    /* FALLTHROUGH */
    case 2:
    case 3:
    epilogue:
    default:
        dt.blinking_triangle_flag = 0;
        return STEP_RESULT_POP(0);
    }
}


/*
 * BTLACT_SUDDEN_GUTS_PILL (asm/battle/actions/sudden_guts_pill.asm)
 *
 * Double target's guts, clamped to 255. Fails on NPCs.
 * Displays the new guts value.
 */
static void sudden_guts_pill_decide(BattleTailText *out) {
    Battler *target = battler_from_offset(bt.current_target);
    if (statmod_npc_fail(out, target))
        return;
    uint16_t new_guts = target->guts * 2;
    if (new_guts > 0xFF)
        new_guts = 0xFF;
    target->guts = new_guts;
    statmod_tail(out, MSG_BTL6_GUTS_AMAZINGLY_BECAME, target->guts);
}

static StepResult btlact_sudden_guts_pill_step(BattleActionState *st) {
    return btlact_statmod_step(st, sudden_guts_pill_decide);
}


/*
 * BTLACT_DEFENSE_SPRAY (asm/battle/actions/defense_spray.asm)
 * BTLACT_DEFENSE_SHOWER (asm/battle/actions/defense_shower.asm)
 *
 * Increase target's defense by 1/16th and display the change.
 * Fails on NPCs. Shower is the same effect under a different item
 * (its own table row points at the same stepper).
 */
static void defense_spray_decide(BattleTailText *out) {
    Battler *target = battler_from_offset(bt.current_target);
    if (statmod_npc_fail(out, target))
        return;
    uint16_t old_defense = target->defense;
    battle_increase_defense(target);
    statmod_tail(out, MSG_BTL6_DEFENSE_WENT_UP, target->defense - old_defense);
}

static StepResult btlact_defense_spray_step(BattleActionState *st) {
    return btlact_statmod_step(st, defense_spray_decide);
}


/*
 * BTLACT_CUTGUTS (asm/battle/actions/cut_guts.asm)
 *
 * Reduce target's guts to 3/4 of current value.
 * Floor at base_guts / 2. Fails on NPCs.
 */
static void cut_guts_decide(BattleTailText *out) {
    Battler *target = battler_from_offset(bt.current_target);
    if (statmod_npc_fail(out, target))
        return;
    uint16_t old_guts = target->guts;

    /* guts = guts * 3 / 4 */
    target->guts = (target->guts * 3) / 4;

    /* Floor at base_guts / 2 */
    uint16_t min_guts = target->base_guts / 2;
    if (target->guts < min_guts)
        target->guts = min_guts;

    statmod_tail(out, MSG_BTL6_GUTS_WENT_DOWN, old_guts - target->guts);
}

static StepResult btlact_cut_guts_step(BattleActionState *st) {
    return btlact_statmod_step(st, cut_guts_decide);
}


/* ======================================================================
 * Prayer sub-actions (called from BTLACT_PRAY dispatch)
 * ====================================================================== */

/*
 * BTLACT_PRAY_SUBTLE (asm/battle/actions/pray_subtle.asm)
 *
 * Recover HP = max_hp / 16 for target.
 */
static StepResult btlact_pray_subtle_step(BattleActionState *st) {
    BattleTailText tail = {0};
    if (st->pc == 0) {
        Battler *target = battler_from_offset(bt.current_target);
        battle_recover_hp_prepare(target, target->hp_max >> 4, &tail);
    }
    return btlact_single_text_step_ex(st, tail.msg, tail.has_cnum, tail.cnum);
}


/*
 * BTLACT_PRAY_WARM (asm/battle/actions/pray_warm.asm)
 *
 * Recover HP = max_hp / 8 for target.
 */
static StepResult btlact_pray_warm_step(BattleActionState *st) {
    BattleTailText tail = {0};
    if (st->pc == 0) {
        Battler *target = battler_from_offset(bt.current_target);
        battle_recover_hp_prepare(target, target->hp_max >> 3, &tail);
    }
    return btlact_single_text_step_ex(st, tail.msg, tail.has_cnum, tail.cnum);
}


/*
 * BTLACT_PRAY_MYSTERIOUS (asm/battle/actions/pray_mysterious.asm)
 *
 * Recover PP = 50% variance of 5 (at least 1) for target.
 */
static StepResult btlact_pray_mysterious_step(BattleActionState *st) {
    BattleTailText tail = {0};
    if (st->pc == 0) {
        uint16_t amount = battle_50pct_variance(5);
        if (amount == 0)
            amount = 1;
        battle_recover_pp_prepare(battler_from_offset(bt.current_target),
                                  amount, &tail);
    }
    return btlact_single_text_step_ex(st, tail.msg, tail.has_cnum, tail.cnum);
}


/*
 * BTLACT_PRAY_GOLDEN (asm/battle/actions/pray_golden.asm)
 *
 * Recover HP = target's max_hp - attacker's hp_target for target.
 * The attacker sacrifices their remaining HP as healing.
 */
static StepResult btlact_pray_golden_step(BattleActionState *st) {
    BattleTailText tail = {0};
    if (st->pc == 0) {
        Battler *target = battler_from_offset(bt.current_target);
        Battler *attacker = battler_from_offset(bt.current_attacker);
        battle_recover_hp_prepare(target,
                                  target->hp_max - attacker->hp_target, &tail);
    }
    return btlact_single_text_step_ex(st, tail.msg, tail.has_cnum, tail.cnum);
}


/*
 * BTLACT_PRAY_AROMA (asm/battle/actions/pray_aroma.asm)
 *
 * Inflict sleep on target. Fails on NPCs.
 */
static StepResult btlact_pray_aroma_step(BattleActionState *st) {
    uint32_t msg = st->pc == 0
        ? inflict_decide(true, STATUS_GROUP_TEMPORARY, STATUS_2_ASLEEP,
                         MSG_BTL5_STATUS_ASLEEP) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_PRAY_RAINBOW (asm/battle/actions/pray_rainbow.asm)
 *
 * If target is unconscious, revive with full HP (a BATTLE_REVIVE child
 * push); otherwise a no-op (no text, no yield).
 */
static StepResult btlact_pray_rainbow_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    if (st->pc == 0) {
        Battler *target = battler_from_offset(bt.current_target);
        if (target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] ==
            STATUS_0_UNCONSCIOUS) {
            st->pc = 1;
            battle_revive_make_init(&child, bt.current_target, target->hp_max);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_REVIVE, &child);
        }
    }
    return STEP_RESULT_POP(0);
}


/*
 * BTLACT_PRAY_RENDING_SOUND (asm/battle/actions/pray_rending_sound.asm)
 *
 * Inflict strangeness on target. Fails on NPCs.
 */
static StepResult btlact_pray_rending_sound_step(BattleActionState *st) {
    uint32_t msg = st->pc == 0
        ? inflict_decide(true, STATUS_GROUP_STRANGENESS, STATUS_3_STRANGE,
                         MSG_BTL5_STATUS_STRANGE) : 0;
    return btlact_single_text_step(st, msg);
}


/*
 * BTLACT_PRAY (asm/battle/actions/pray.asm)
 *
 * Paula's Pray command.  Randomly selects one of 10 prayer types using
 * a weighted probability table (16 entries), displays the prayer text,
 * sets up appropriate targeting, then dispatches the sub-action to all
 * valid targets via apply_action_to_targets.
 *
 * Prayer types:
 *   0 = Subtle (allies, heal HP/16)
 *   1 = Warm (allies, heal HP/8)
 *   2 = Mysterious (allies, recover PP)
 *   3 = Golden (random ally, sacrifice HP)
 *   4 = Rockin (random enemy, PSI Rockin β)
 *   5 = Flash (all, PSI Flash α)
 *   6 = Rainbow (all, revive with full HP)
 *   7 = Aroma (all, inflict sleep)
 *   8 = Rending Sound (all, inflict strangeness)
 *   9 = Defense Down (all, Defense Down α)
 *
 * Resumable: the prayer roll happens at pc 0 (then the text push); the
 * targeting setup — including golden/rockin's random-targeting rolls —
 * runs at the text's resume pc, exactly the blocking sequence points. The
 * per-target dispatch is a BATTLE_APPLY child push carrying the
 * sub-action's ROM address (the dispatch-table rows of the same functions
 * the blocking form passed by pointer).
 */
static StepResult btlact_pray_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    /* PRAYER_LIST: 16-entry weighted probability table (asm/data/battle/prayer_list.asm) */
    static const uint8_t prayer_list[16] = {
        0, 0, 0, 0, 0, 1, 1, 2, 3, 4, 5, 5, 6, 7, 8, 9
    };

    /* PRAYER_TEXT_PTRS: text address for each prayer type (asm/data/battle/prayer_text_pointers.asm) */
    static const uint32_t prayer_text_addrs[10] = {
        MSG_BTL6_PRAY_SUBTLE_LIGHT,   /* 0: subtle */
        MSG_BTL6_PRAY_WARM_LIGHT,   /* 1: warm */
        MSG_BTL6_PRAY_MYSTERIOUS_LIGHT,   /* 2: mysterious */
        MSG_BTL6_PRAY_GOLDEN_LIGHT,   /* 3: golden */
        MSG_BTL6_PRAY_LIGHT_CHASED_ENEMY,   /* 4: rockin */
        MSG_BTL6_PRAY_DAZZLING_LIGHT,   /* 5: flash */
        MSG_BTL6_PRAY_RAINBOW_LIGHT,   /* 6: rainbow */
        MSG_BTL6_PRAY_MYSTERIOUS_AROMA,   /* 7: aroma */
        MSG_BTL6_PRAY_HEAVEN_RENDING_SOUND,  /* 8: rending sound */
        MSG_BTL6_PRAY_HEAVY_AIR,   /* 9: defense down */
    };

    /* Sub-action ROM address for each prayer type (the btlact_dispatch_table
     * rows of the functions the blocking form passed by pointer) */
    static const uint32_t prayer_action_addrs[10] = {
        0xC2AC2A,  /* 0: btlact_pray_subtle */
        0xC2AC3E,  /* 1: btlact_pray_warm */
        0xC2AC68,  /* 2: btlact_pray_mysterious */
        0xC2AC51,  /* 3: btlact_pray_golden */
        0xC2955F,  /* 4: btlact_psi_rockin_beta (reused) */
        0xC29987,  /* 5: btlact_psi_flash_alpha (reused) */
        0xC2AC7B,  /* 6: btlact_pray_rainbow */
        0xC2AC99,  /* 7: btlact_pray_aroma */
        0xC2ACDA,  /* 8: btlact_pray_rending_sound */
        0xC29E86,  /* 9: btlact_defense_down_alpha (reused) */
    };

    for (;;) {
        switch (st->pc) {
        case 0: {
            /* Pick random prayer type */
            uint16_t index = rand_limit(16);
            st->scratch16[0] = prayer_list[index];

            /* Display prayer text */
            st->pc = 1;
            if (battle_push_text(&child, prayer_text_addrs[st->scratch16[0]]))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 1: {
            dt.blinking_triangle_flag = 0;
            uint16_t prayer_type = st->scratch16[0];

            /* Set up targeting based on prayer type */
            switch (prayer_type) {
            case 0: /* subtle */
            case 1: /* warm */
            case 2: /* mysterious */
                battle_target_allies();
                battle_remove_npc_targeting();
                break;
            case 3: /* golden — random single ally */
                battle_target_allies();
                battle_remove_npc_targeting();
                battle_remove_dead_targeting();
                bt.battler_target_flags = battle_random_targeting(bt.battler_target_flags);
                break;
            case 4: /* rockin — random single enemy */
                battle_target_all_enemies();
                battle_remove_npc_targeting();
                battle_remove_dead_targeting();
                bt.battler_target_flags = battle_random_targeting(bt.battler_target_flags);
                break;
            case 5: /* flash */
            case 6: /* rainbow */
            case 7: /* aroma */
            case 8: /* rending sound */
            case 9: /* defense down */
                battle_target_all();
                break;
            default:
                break;
            }

            /* Remove dead targets (except rainbow which can revive) */
            if (prayer_type != 6) {
                battle_remove_dead_targeting();
            }

            /* Apply the prayer action to all targets */
            st->pc = 2;
            battle_apply_make_init(&child, prayer_type <= 9
                                               ? prayer_action_addrs[prayer_type]
                                               : 0);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_APPLY, &child);
        }
        case 2:
        default:
            bt.battler_target_flags = 0;
            return STEP_RESULT_POP(0);
        }
    }
}


/*
 * APPLY_NEUTRALIZE_TO_ALL (asm/battle/apply_neutralize_to_all.asm)
 *
 * If mirror (metamorphose) is active, finds the mirrored Poo battler,
 * restores original stats from bt.mirror_battler_backup, clears mirror state
 * (with the morph-neutralized text). Then targets all conscious battlers and
 * applies btlact_neutralize to each (a BATTLE_APPLY child push).
 */
static StepResult apply_neutralize_to_all_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0:
            st->pc = 1;
            /* If mirror is active, reverse metamorphosis first */
            if (bt.mirror_enemy != 0) {
                for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
                    Battler *b = &bt.battlers_table[i];
                    if (b->consciousness == 0) continue;
                    if (b->ally_or_enemy != 0) continue;
                    if (b->id != PARTY_MEMBER_POO) continue;

                    bt.mirror_enemy = 0;
                    battle_copy_mirror_data(b, &bt.mirror_battler_backup);
                    b->current_action = 0;
                    st->pc = 3;
                    if (battle_push_text(&child, MSG_BTL5_MORPH_NEUTRALIZED))
                        return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                    break;  /* only cure one */
                }
            }
            break;
        case 3:
            dt.blinking_triangle_flag = 0;
            st->pc = 1;
            break;
        case 1:
            battle_target_all();
            battle_remove_dead_targeting();
            st->pc = 2;
            battle_apply_make_init(&child, 0xC29051 /* btlact_neutralize */);
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_APPLY, &child);
        case 2:
        default:
            bt.battler_target_flags = 0;
            return STEP_RESULT_POP(0);
        }
    }
}


/* ======================================================================
 * Equipment switching in battle
 * ====================================================================== */

/* CHECK_ITEM_USABLE_BY: now shared via inventory.h — see inventory.c. */

/*
 * BTLACT_SWITCH_WEAPONS (asm/battle/actions/switch_weapon.asm)
 *
 * Equips a new weapon during battle.  Saves the current offense/guts
 * bonuses from equipment, equips the new item, then reapplies the bonuses
 * on top of the new base stats from char_struct.  If the new weapon has
 * ammunition of type 1 (projectile), dispatches to the shoot action (5);
 * otherwise dispatches to the normal attack action (4).
 *
 * Resumable: the blocking form holds dt.blinking_triangle_flag = 1 across
 * ALL its texts (raw display_text_from_addr, no battle epilogue) and clears
 * it once at the end, so the resume pcs do NOT clear it — only the final pc
 * does. The equip mutations precede the success text (pc 0); the attack
 * dispatch is a BATTLE_ACTION child push (scratch16[0] = char_id,
 * scratch16[1] = the dispatched action 4/5).
 */
static StepResult btlact_switch_weapons_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0: {
            Battler *attacker = battler_from_offset(bt.current_attacker);
            uint16_t char_id = attacker->id;
            st->scratch16[0] = char_id;

            dt.blinking_triangle_flag = 1;

            uint32_t msg;
            /* Check if the character can use this item */
            if (!check_item_usable_by(char_id, attacker->current_action_argument)) {
                msg = MSG_GOODS4_EQUIP_WEAPON_FAIL_OLD_WEAPON;
            } else {
                CharStruct *ch = &party_characters[char_id - 1];

                /* Save the offense/guts bonuses (current minus base from equipment) */
                int16_t offense_bonus = attacker->offense - (uint16_t)attacker->base_offense;
                int16_t guts_bonus = attacker->guts - (uint16_t)attacker->base_guts;

                /* Equip the new weapon (action_item_slot is the inventory slot) */
                equip_item(char_id, (uint16_t)attacker->action_item_slot);

                /* Update battler base stats from char_struct and reapply bonuses */
                attacker->base_offense = ch->offense;
                attacker->offense = (uint16_t)attacker->base_offense + offense_bonus;

                attacker->base_guts = ch->guts;
                attacker->guts = (uint16_t)attacker->base_guts + guts_bonus;

                msg = MSG_GOODS4_EQUIP_ITEM_SUCCESS;
            }

            st->pc = 1;
            if (push_plain_text(&child, msg))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 1: {
            /* Dispatch: check if the (now-equipped) weapon is a projectile type */
            CharStruct *ch2 = &party_characters[st->scratch16[0] - 1];
            uint16_t action = 4;  /* normal weapon — bash */
            uint8_t weapon_slot = ch2->equipment[EQUIP_WEAPON];
            if (weapon_slot != 0) {
                uint8_t weapon_item_id = ch2->items[weapon_slot - 1];
                if (weapon_item_id != 0) {
                    const ItemConfig *entry = get_item_entry(weapon_item_id);
                    if (entry && (entry->type & 0x03) == 1)
                        action = 5;  /* projectile weapon — shoot */
                }
            }
            st->scratch16[1] = action;

            if (battle_action_table == NULL) {
                st->pc = 3;
                break;
            }
            st->pc = 2;
            if (push_plain_text(&child,
                                battle_action_table[action].description_text_pointer))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 2:
            /* Run the attack (writes bt.temp_function_pointer like the
             * blocking form; bash/shoot are converted steppers → child push) */
            st->pc = 3;
            if (battle_action_dispatch(
                    battle_action_table[st->scratch16[1]].battle_function_pointer,
                    &child))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_ACTION, &child);
            break;
        case 3:
        default:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        }
    }
}


/*
 * BTLACT_SWITCH_ARMOR (asm/battle/actions/switch_armor.asm)
 *
 * Equips new armor during battle.  Saves defense/speed/luck bonuses,
 * equips the item, reapplies bonuses with new base stats, then
 * recalculates all elemental and status resistances from char_struct.
 *
 * Resumable: like switch_weapons, the blinking flag is held across the raw
 * texts and cleared only at the end. The blocking form equips BEFORE the
 * success text but reapplies stats/resistances AFTER it, so the stat
 * writeback runs at the resume pc; the saved bonuses cross the push in
 * scratch16[0]/[1]/scratch32 (defense/speed/luck).
 */
static StepResult btlact_switch_armor_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0: {
            Battler *attacker = battler_from_offset(bt.current_attacker);
            uint16_t char_id = attacker->id;

            dt.blinking_triangle_flag = 1;

            /* Check if the character can use this item */
            if (!check_item_usable_by(char_id, attacker->current_action_argument)) {
                st->pc = 1;
                if (push_plain_text(&child, MSG_GOODS4_EQUIP_WEAPON_FAIL_OLD_WEAPON))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
                break;
            }

            /* Save bonuses: current stat minus base (from equipment) */
            st->scratch16[0] = (uint16_t)(attacker->defense - (uint16_t)attacker->base_defense);
            st->scratch16[1] = (uint16_t)(attacker->speed - (uint16_t)attacker->base_speed);
            st->scratch32 = (uint16_t)(attacker->luck - (uint16_t)attacker->base_luck);

            /* Equip the new armor */
            equip_item(char_id, (uint16_t)attacker->action_item_slot);

            st->pc = 2;
            if (push_plain_text(&child, MSG_GOODS4_EQUIP_ITEM_SUCCESS))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 2: {
            /* Update battler base stats from char_struct and reapply bonuses
             * (after the text, as in the blocking form) */
            Battler *attacker = battler_from_offset(bt.current_attacker);
            CharStruct *ch = &party_characters[attacker->row];

            attacker->base_defense = ch->defense;
            attacker->defense = (uint16_t)attacker->base_defense + (int16_t)st->scratch16[0];

            attacker->base_speed = ch->speed;
            attacker->speed = (uint16_t)attacker->base_speed + (int16_t)st->scratch16[1];

            attacker->base_luck = ch->luck;
            attacker->luck = (uint16_t)attacker->base_luck + (int16_t)(uint16_t)st->scratch32;

            /* Recalculate all elemental/status resistances from char_struct */
            attacker->fire_resist = battle_calc_psi_dmg_modifier(ch->fire_resist);
            attacker->freeze_resist = battle_calc_psi_dmg_modifier(ch->freeze_resist);
            attacker->flash_resist = battle_calc_psi_res_modifier(ch->flash_resist);
            attacker->paralysis_resist = battle_calc_psi_res_modifier(ch->paralysis_resist);
            attacker->hypnosis_resist = battle_calc_psi_res_modifier(ch->hypnosis_brainshock_resist);
            /* brainshock = 3 - hypnosis_brainshock_resist (inverted) */
            uint8_t brainshock_base = 3 - ch->hypnosis_brainshock_resist;
            attacker->brainshock_resist = battle_calc_psi_res_modifier(brainshock_base);

            st->pc = 1;
            break;
        }
        case 1:
        default:
            dt.blinking_triangle_flag = 0;
            return STEP_RESULT_POP(0);
        }
    }
}


/* ======================================================================
 * Clumsy Robot death
 * ====================================================================== */

/*
 * BTLACT_CLUMSYDEATH (asm/battle/actions/clumsy_robot_death.asm)
 *
 * Special death handler for the Clumsy Robot enemy.
 * Checks event flag from PSI teleport destination entry 13 to determine
 * where to teleport:
 *   - Flag set: teleport to destination 15 (normal end)
 *   - Flag not set: teleport to destination 13, bt.special_defeat=1
 */
static StepResult btlact_clumsydeath_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0: {
        /* Load the PSI teleport destination table to read entry 13's event flag */
        const uint8_t *table = ASSET_DATA(ASSET_DATA_PSI_TELEPORT_DEST_TABLE_BIN);

        /* Entry 13: each entry is 31 bytes, event_flag at byte offset 25
         * (struct ow.psi_teleport_destination: name[25] + event_flag[2] + x[2] + y[2]) */
        uint16_t event_flag = 0;
        if (table) {
            const uint8_t *entry = table + 13 * 31;
            event_flag = read_u16_le(entry + 25);
        }

        st->scratch16[0] = event_flag_get(event_flag) ? 1 : 0;
        st->pc = 1;
        if (battle_push_text(&child, st->scratch16[0]
                                         ? MSG_BTL4_RUNAWAY5_RESCUE
                                         : MSG_BTL4_ENEMY_ESCAPE_SMOKE_FAIL))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
    }
    /* FALLTHROUGH */
    case 1:
    default:
        /* The teleport setup runs after the text, as in the blocking form */
        dt.blinking_triangle_flag = 0;
        if (st->scratch16[0]) {
            ow.psi_teleport_style = 3;  /* TELEPORT_STYLE::INSTANT */
            ow.psi_teleport_destination = 15;
        } else {
            ow.psi_teleport_style = 3;  /* TELEPORT_STYLE::INSTANT */
            ow.psi_teleport_destination = 13;
            bt.special_defeat = 1;
        }
        return STEP_RESULT_POP(0);
    }
}



/* ======================================================================
 * BTLACT_MASTERBARFDEATH (asm/battle/actions/master_barf_death.asm)
 *
 * Special boss action: when Master Barf is defeated, Poo joins the party
 * mid-battle and performs a Starstorm Alpha attack on all enemies.
 *
 * Resumable: the Poo setup runs at pc 0 (entrance text = a with-prompt
 * push), the Starstorm desc text at pc 1, and the per-enemy damage loop is
 * one BC_CALC_DAMAGE push per conscious enemy (exec_i resumes the scan;
 * each variance roll happens at its push, the original per-target order).
 * The saved attacker/target cross the pushes in scratch16[0]/[1].
 * ====================================================================== */
static StepResult btlact_masterbarfdeath_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    for (;;) {
        switch (st->pc) {
        case 0: {
            st->scratch16[0] = bt.current_attacker;
            st->scratch16[1] = bt.current_target;

            /* Hide HP/PP windows, add Poo to party */
            hide_hppp_windows();
            add_char_to_party(PARTY_MEMBER_POO);

            /* Find first empty battler slot for Poo */
            for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
                if (bt.battlers_table[i].consciousness == 0) {
                    battle_init_player_stats(PARTY_MEMBER_POO, &bt.battlers_table[i]);
                    bt.current_attacker = (uint16_t)(i * sizeof(Battler));
                    break;
                }
            }

            /* Show HP/PP windows with Poo */
            redirect_show_hppp_windows();

            /* Find Poo's position in party_members and select menu character */
            for (uint16_t i = 0; i < TOTAL_PARTY_COUNT; i++) {
                if (game_state.party_members[i] == PARTY_MEMBER_POO) {
                    select_battle_menu_character_far(i);
                    break;
                }
            }

            /* Display Poo's entrance text (the with-prompt variant) */
            st->pc = 1;
            if (battle_push_text_ex(&child, MSG_BTL4_POO_USES_STARSTORM,
                                    true, false, 0))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;
        }
        case 1: {
            dt.blinking_triangle_flag = 0;

            /* Set up Starstorm Alpha attack */
            fix_attacker_name(0);
            set_current_item(21);  /* PSI::STARSTORM_ALPHA */

            /* Display Starstorm Alpha description text (action 30 in battle_action_table) */
            st->pc = 2;
            st->exec_i = 0;
            if (battle_action_table != NULL) {
                uint32_t desc_addr = battle_action_table[30].description_text_pointer;
                if (desc_addr != 0 && battle_push_text(&child, desc_addr))
                    return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            }
            break;
        }
        case 2:
        default:
            dt.blinking_triangle_flag = 0;

            /* Deal Starstorm Alpha damage to all conscious enemies */
            for (uint16_t i = st->exec_i; i < BATTLER_COUNT; i++) {
                if (bt.battlers_table[i].consciousness == 0)
                    continue;
                if ((bt.battlers_table[i].ally_or_enemy & 0xFF) != 1)
                    continue;
                bt.current_target = (uint16_t)(i * sizeof(Battler));
                fix_target_name();
                uint16_t damage = battle_25pct_variance(STARSTORM_ALPHA_DAMAGE);
                st->exec_i = (uint8_t)(i + 1);
                battle_calc_make_init(&child, BC_CALC_DAMAGE,
                                      bt.current_target, damage);
                return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
            }

            /* Restore original attacker and target */
            bt.current_attacker = st->scratch16[0];
            bt.current_target = st->scratch16[1];
            fix_attacker_name(0);
            fix_target_name();
            return STEP_RESULT_POP(0);
        }
    }
}



/* ======================================================================
 * Giygas prayer damage constants (from include/enums.asm)
 * ====================================================================== */
#define GIYGAS_PRAYER_DAMAGE_1   50
#define GIYGAS_PRAYER_DAMAGE_2  100
#define GIYGAS_PRAYER_DAMAGE_3  200
#define GIYGAS_PRAYER_DAMAGE_4  400
#define GIYGAS_PRAYER_DAMAGE_5  800
#define GIYGAS_PRAYER_DAMAGE_6 1600
#define GIYGAS_PRAYER_DAMAGE_7 3200
#define GIYGAS_PRAYER_DAMAGE_8 6400
#define GIYGAS_PRAYER_DAMAGE_9 12800
#define GIYGAS_PRAYER_DAMAGE_10 25600

/* Music constants for Giygas battle (from include/constants/music.asm) */
#define MUSIC_NONE              0
#define MUSIC_GIYGAS_PHASE1   186
#define MUSIC_GIYGAS_PHASE2    73
#define MUSIC_GIYGAS_PHASE3   185
#define MUSIC_GIYGAS_WEAKENED2  74

/* SFX constants (from include/constants/sfx.asm) */
#define SFX_PSI_STARSTORM      64

/* ======================================================================
 * Giygas cutscene sub-machines
 *
 * The pokey_speech / giygas_prayer actions compose three former blocking
 * helpers, each here as a composable pc-RANGE stepper: it owns pcs
 * [base, base+N), returns true with *out filled while it still has work,
 * false once complete (st->pc parked at base+N, the caller's next range).
 * Fade waits are GAME_MODE_FADE_WAIT pushes (FADE_TICK_WINDOW =
 * wait_for_fade_with_tick's loop body); timed waits are BW_FRAMES pushes.
 * ====================================================================== */

static StepResult push_fade_wait_window(ModeState *child) {
    memset(child, 0, sizeof(*child));
    child->fade_wait.tick_kind = FADE_TICK_WINDOW;
    return STEP_RESULT_PUSH_INIT(GAME_MODE_FADE_WAIT, child);
}

static StepResult push_battle_wait_frames(ModeState *child, uint16_t frames) {
    memset(child, 0, sizeof(*child));
    child->battle_wait.kind = BW_FRAMES;
    child->battle_wait.remaining = frames;
    return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_WAIT, child);
}

/* STEP_PUSH the former blocking load_battle_scene() — now
 * GAME_MODE_LOAD_BATTLE_SCENE (mode_step_load_battle_scene, battle_ui.c). */
static StepResult push_load_battle_scene(ModeState *child, uint16_t group,
                                         uint16_t music) {
    memset(child, 0, sizeof(*child));
    child->load_battle_scene.phase = LBS_ENTER;
    child->load_battle_scene.group = group;
    child->load_battle_scene.music = music;
    return STEP_RESULT_PUSH_INIT(GAME_MODE_LOAD_BATTLE_SCENE, child);
}

/* DISPLAY_BATTLE_CUTSCENE_TEXT (asm/battle/display_battle_cutscene_text.asm):
 * fade out, show cutscene text with the battle UI hidden, reload the battle
 * scene, restore the UI, 1-second wait. Occupies 4 yield pcs. */
static bool cutscene_text_steps(BattleActionState *st, uint8_t base,
                                uint16_t group, uint16_t music, uint32_t text,
                                StepResult *out) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc - base) {
    case 0:
        fade_out(1, 4);
        st->pc = base + 1;
        *out = push_fade_wait_window(&child);
        return true;
    case 1:
        bt.battle_mode_flag = 0;
        ml.current_map_music_track = 0;
        close_all_windows_and_hide_hppp();
        st->pc = base + 2;
        if (battle_push_text(&child, text)) {
            *out = STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            return true;
        }
        /* FALLTHROUGH — unresolvable text: continue inline */
    case 2:
        dt.blinking_triangle_flag = 0;
        fade_out(1, 2);
        st->pc = base + 3;
        *out = push_fade_wait_window(&child);
        return true;
    case 3:
        st->pc = base + 4;
        *out = push_load_battle_scene(&child, group, music);
        return true;
    case 4:
        bt.battle_mode_flag = 1;
        redirect_show_hppp_windows();
        create_window(0x0E);  /* WINDOW::TEXT_BATTLE */
        st->pc = base + 5;
        *out = push_battle_wait_frames(&child, FRAMES_PER_SECOND);
        return true;
    default:
        return false;  /* complete (st->pc == base + 5) */
    }
}
#define CUTSCENE_TEXT_PCS 5

/* PLAY_GIYGAS_WEAKENED_SEQUENCE (asm/battle/play_giygas_weakened_sequence.asm):
 * fade to black, Giygas-weakened text on a BG3-only screen, fade back.
 * Occupies 7 yield pcs. */
static bool weakened_seq_steps(BattleActionState *st, uint8_t base,
                               uint16_t music, uint32_t text,
                               StepResult *out) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc - base) {
    case 0:
        fade_out(1, 1);
        write_apu_port1(2);
        st->pc = base + 1;
        *out = push_fade_wait_window(&child);
        return true;
    case 1:
        bt.battle_mode_flag = 0;
        close_all_windows_and_hide_hppp();
        ppu.tm = 0x04;  /* BG3 only */
        change_music(191);  /* MUSIC::GIYGAS_WEAKENED */
        fade_in(1, 1);
        st->pc = base + 2;
        *out = push_fade_wait_window(&child);
        return true;
    case 2:
        st->pc = base + 3;
        *out = push_battle_wait_frames(&child, 20);  /* 2 * SIXTHS_OF_A_SECOND */
        return true;
    case 3:
        st->pc = base + 4;
        if (battle_push_text(&child, text)) {
            *out = STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            return true;
        }
        /* FALLTHROUGH — unresolvable text: continue inline */
    case 4:
        dt.blinking_triangle_flag = 0;
        bt.battle_mode_flag = 1;
        st->pc = base + 5;
        *out = push_battle_wait_frames(&child, 20);
        return true;
    case 5:
        write_apu_port1(2);
        fade_out(1, 1);
        st->pc = base + 6;
        *out = push_fade_wait_window(&child);
        return true;
    case 6:
        redirect_show_hppp_windows();
        create_window(0x0E);  /* WINDOW::TEXT_BATTLE */
        ppu.tm = 0x17;  /* BG1 + BG2 + BG3 + OBJ */
        change_music(music);
        fade_in(1, 1);
        st->pc = base + 7;
        *out = push_fade_wait_window(&child);
        return true;
    default:
        return false;  /* complete (st->pc == base + 7) */
    }
}
#define WEAKENED_SEQ_PCS 7

/* GIYGAS_HURT_PRAYER (asm/battle/giygas_hurt_prayer.asm): 1-second wait,
 * green flash + SMAAAASH flag, 25%-variance damage to Giygas (slot 8),
 * 1-second wait. Occupies 3 yield pcs. */
static bool hurt_prayer_steps(BattleActionState *st, uint8_t base,
                              uint16_t base_damage, StepResult *out) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc - base) {
    case 0:
        st->pc = base + 1;
        *out = push_battle_wait_frames(&child, FRAMES_PER_SECOND);
        return true;
    case 1:
        /* Target Giygas (slot 8 = first enemy) */
        bt.current_target = FIRST_ENEMY_INDEX * sizeof(Battler);
        fix_target_name();

        bt.green_flash_duration = FRAMES_PER_SECOND;
        bt.is_smaaaash_attack = 1;

        st->pc = base + 2;
        battle_calc_make_init(&child, BC_RESIST_DAMAGE,
                              battle_25pct_variance(base_damage), 0xFF);
        *out = STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_CALC, &child);
        return true;
    case 2:
        st->pc = base + 3;
        *out = push_battle_wait_frames(&child, FRAMES_PER_SECOND);
        return true;
    default:
        return false;  /* complete (st->pc == base + 3) */
    }
}
#define HURT_PRAYER_PCS 3

/* ======================================================================
 * BTLACT_POKEY_SPEECH (asm/battle/actions/pokey_speech_1.asm)
 *
 * Giygas phase transition: Pokey's first speech. Sets DEVILS_MACHINE_OFF,
 * replaces boss with GIYGAS_3, loads phase 1 scene, shows text,
 * kills slot 9, transitions to GIYGAS_STARTS_ATTACKING phase,
 * replaces with GIYGAS_4, loads phase 2 scene.
 * ====================================================================== */
static StepResult btlact_pokey_speech_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0:
        bt.giygas_phase = GIYGAS_DEVILS_MACHINE_OFF;
        replace_boss_battler(ENEMY_GIYGAS_3);
        st->pc = 1;
        return push_load_battle_scene(&child, ENEMY_GROUP_BOSS_GIYGAS_PHASE_1,
                                      MUSIC_GIYGAS_PHASE1);
    case 1:
        st->pc = 2;
        if (battle_push_text_ex(&child, MSG_BTL6_MECH_POKEY_SPEECH_1B,
                                true, false, 0))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
        /* FALLTHROUGH */
    case 2:
        dt.blinking_triangle_flag = 0;
        /* Kill slot 9 (Pokey's mech) */
        bt.battlers_table[9].consciousness = 0;
        bt.giygas_phase = GIYGAS_STARTS_ATTACKING;
        /* FINAL_BATTLE_ANTIPIRACY_CHECK: intentional no-op.
         * Assembly checksums hardware registers and wipes SRAM on failure.
         * Always passes for legitimate ROM; not applicable to C port. */
        replace_boss_battler(ENEMY_GIYGAS_4);
        st->pc = 3;
        return push_load_battle_scene(&child, ENEMY_GROUP_BOSS_GIYGAS_PHASE_2,
                                      MUSIC_GIYGAS_PHASE2);
    case 3:
    default:
        bt.skip_death_text_and_cleanup = 1;
        return STEP_RESULT_POP(0);
    }
}


/* ======================================================================
 * BTLACT_POKEY_SPEECH_2 (asm/battle/actions/pokey_speech_2.asm)
 *
 * Giygas phase transition: Pokey's second speech. Sets START_PRAYING phase,
 * shows/hides slot 9, displays text, replaces boss with GIYGAS_5.
 * ====================================================================== */
static StepResult btlact_pokey_speech_2_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */

    switch (st->pc) {
    case 0:
        bt.giygas_phase = GIYGAS_START_PRAYING;
        st->pc = 1;
        return push_battle_wait_frames(&child, 2 * FRAMES_PER_SECOND);
    case 1:
        /* Show slot 9 consciousness */
        bt.battlers_table[9].consciousness = 1;
        render_all_battle_sprites();
        st->pc = 2;
        if (battle_push_text_ex(&child, MSG_BTL6_MECH_POKEY_SPEECH_2,
                                true, false, 0))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
        /* FALLTHROUGH */
    case 2:
        dt.blinking_triangle_flag = 0;
        /* Hide slot 9 */
        bt.battlers_table[9].consciousness = 0;
        render_all_battle_sprites();
        st->pc = 3;
        return push_battle_wait_frames(&child, FRAMES_PER_SECOND);
    case 3:
        replace_boss_battler(ENEMY_GIYGAS_5);
        st->pc = 4;
        return push_load_battle_scene(&child,
                                      ENEMY_GROUP_BOSS_GIYGAS_DURING_PRAYER_1,
                                      MUSIC_GIYGAS_PHASE3);
    case 4:
    default:
        bt.skip_death_text_and_cleanup = 1;
        return STEP_RESULT_POP(0);
    }
}


/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_1 (asm/battle/actions/giygas_prayer_1.asm)
 *
 * First prayer: plays cutscene text, SFX, screen shake, damages Giygas,
 * replaces boss, loads after-prayer scene.
 * ====================================================================== */
static StepResult btlact_giygas_prayer_1_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */
    StepResult r;

    if (cutscene_text_steps(st, 0, ENEMY_GROUP_BOSS_GIYGAS_DURING_PRAYER_1,
                            MUSIC_GIYGAS_PHASE3,
                            MSG_EVT4_PAULA_PRAYER_MR_SATURN_RESPONDS, &r))
        return r;

    switch (st->pc - CUTSCENE_TEXT_PCS) {
    case 0:
        st->pc++;
        return push_battle_wait_frames(&child, 2 * FRAMES_PER_SECOND);
    case 1:
        play_sfx(SFX_PSI_STARSTORM);
        st->pc++;
        return push_battle_wait_frames(&child, 30);  /* HALF_OF_A_SECOND */
    case 2:
        bt.vertical_shake_duration = FRAMES_PER_SECOND;
        bt.vertical_shake_hold_duration = 12;  /* FIFTH_OF_A_SECOND */
        st->pc++;
        if (battle_push_text_ex(&child, MSG_BTL7_GIYGAS_DEFENSES_UNSTABLE,
                                true, false, 0))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
        /* FALLTHROUGH */
    case 3:
        dt.blinking_triangle_flag = 0;
        bt.giygas_phase = GIYGAS_PRAYER_1_USED;
        replace_boss_battler(ENEMY_GIYGAS_6);
        st->pc++;
        return push_load_battle_scene(&child,
                                      ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                      MUSIC_NONE);
    case 4:
    default:
        return STEP_RESULT_POP(0);
    }
}


/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_2..6 (asm/battle/actions/giygas_prayer_2..6.asm)
 *
 * Prayers 2-6: show cutscene text, deal escalating prayer damage.
 * ====================================================================== */
static StepResult btlact_giygas_prayer_n_step(BattleActionState *st,
                                              uint32_t text, uint16_t damage,
                                              uint8_t phase_after) {
    StepResult r;

    if (cutscene_text_steps(st, 0, ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                            MUSIC_GIYGAS_PHASE3, text, &r))
        return r;
    if (hurt_prayer_steps(st, CUTSCENE_TEXT_PCS, damage, &r))
        return r;
    bt.giygas_phase = phase_after;
    return STEP_RESULT_POP(0);
}

static StepResult btlact_giygas_prayer_2_step(BattleActionState *st) {
    return btlact_giygas_prayer_n_step(st, MSG_EVT4_PAULA_PRAYER_RUNAWAY_FIVE,
                                       GIYGAS_PRAYER_DAMAGE_1,
                                       GIYGAS_PRAYER_2_USED);
}
static StepResult btlact_giygas_prayer_3_step(BattleActionState *st) {
    return btlact_giygas_prayer_n_step(st, MSG_EVT4_PAULA_PRAYER_PAULAS_FAMILY,
                                       GIYGAS_PRAYER_DAMAGE_2,
                                       GIYGAS_PRAYER_3_USED);
}
static StepResult btlact_giygas_prayer_4_step(BattleActionState *st) {
    return btlact_giygas_prayer_n_step(st, MSG_EVT4_PAULA_PRAYER_TONY_AND_CLASS,
                                       GIYGAS_PRAYER_DAMAGE_3,
                                       GIYGAS_PRAYER_4_USED);
}
static StepResult btlact_giygas_prayer_5_step(BattleActionState *st) {
    return btlact_giygas_prayer_n_step(st, MSG_EVT4_PAULA_PRAYER_DALAAM_MASTER,
                                       GIYGAS_PRAYER_DAMAGE_4,
                                       GIYGAS_PRAYER_5_USED);
}
static StepResult btlact_giygas_prayer_6_step(BattleActionState *st) {
    return btlact_giygas_prayer_n_step(st, MSG_EVT4_PAULA_PRAYER_FRANK_RESPONDS,
                                       GIYGAS_PRAYER_DAMAGE_5,
                                       GIYGAS_PRAYER_6_USED);
}


/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_7 (asm/battle/actions/giygas_prayer_7.asm)
 *
 * Prayer 7 (Ness's Mom): cutscene text, damage, reload scene with
 * AFTER_PRAYER_7 group and weakened music.
 * ====================================================================== */
static StepResult btlact_giygas_prayer_7_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */
    StepResult r;

    if (cutscene_text_steps(st, 0, ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                            MUSIC_GIYGAS_PHASE3,
                            MSG_EVT4_PAULA_PRAYER_NES_MOM_RESPONDS, &r))
        return r;
    if (hurt_prayer_steps(st, CUTSCENE_TEXT_PCS, GIYGAS_PRAYER_DAMAGE_6, &r))
        return r;

    /* Tail: phase + scene reload (a LOAD_BATTLE_SCENE push), then pop. */
    uint8_t tail = CUTSCENE_TEXT_PCS + HURT_PRAYER_PCS;
    if (st->pc == tail) {
        bt.giygas_phase = GIYGAS_PRAYER_7_USED;
        st->pc = tail + 1;
        return push_load_battle_scene(&child,
                                      ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_7,
                                      MUSIC_GIYGAS_WEAKENED2);
    }
    return STEP_RESULT_POP(0);
}


/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_8 (asm/battle/actions/giygas_prayer_8.asm)
 *
 * Prayer 8: uses weakened sequence instead of cutscene text.
 * ====================================================================== */
static StepResult btlact_giygas_prayer_8_step(BattleActionState *st) {
    StepResult r;

    if (weakened_seq_steps(st, 0, MUSIC_GIYGAS_WEAKENED2,
                           MSG_BTL7_PRAY_ABSORBED_BY_DARKNESS, &r))
        return r;
    bt.giygas_phase = GIYGAS_PRAYER_8_USED;
    return STEP_RESULT_POP(0);
}


/* Music/SFX constants for Giygas prayer 9 (from include/constants/) */
#define MUSIC_GIYGAS_DEATH    190
#define MUSIC_GIYGAS_DEATH2    75
#define MUSIC_GIYGAS_STATIC   182
#define SFX_DOOR_OPEN           8
#define SFX_DOOR_CLOSE          9
#define SFX_PSI_THUNDER_DAMAGE 63

/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_9 (asm/battle/actions/giygas_prayer_9.asm)
 *
 * The final prayer sequence. Deals remaining damage to Giygas, plays
 * the death sequence with static noise transitions, battle swirl,
 * and transition to the final post-Giygas scene.
 *
 * pc map: 1..11 = one weakened+hurt pair (exec_i picks the text/damage,
 * looping 4 times); 12+ = the death tail. Loop state: scratch16[0] =
 * noise-table index, then the static-transition step; scratch16[1] = the
 * current step's remaining frames; scratch32 = packed shake bookkeeping
 * (byte 0 apu toggle, byte 1 shake countdown, byte 2 shake repeats).
 * ====================================================================== */
static StepResult btlact_giygas_prayer_9_step(BattleActionState *st) {
    static ModeState child;  /* outlives the dispatch (the pump copies it) */
    static const uint32_t pair_texts[4] = {
        MSG_BTL7_PRAY_RESPONSE_STRANGER,
        MSG_BTL7_PRAY_KEPT_PRAYING_1,
        MSG_BTL7_PRAY_KEPT_PRAYING_2,
        MSG_BTL7_PRAY_KEPT_PRAYING_3,
    };
    static const uint16_t pair_damages[4] = {
        GIYGAS_PRAYER_DAMAGE_7,
        GIYGAS_PRAYER_DAMAGE_8,
        GIYGAS_PRAYER_DAMAGE_9,
        GIYGAS_PRAYER_DAMAGE_10,
    };
    StepResult r;

    for (;;) {
        if (st->pc == 0) {
            /* Reset HP/PP rolling counters */
            reset_hppp_rolling();
            st->pc = 1;
        }

        /* pcs [1, 12): the current weakened-sequence + hurt-prayer pair */
        if (st->pc < 1 + WEAKENED_SEQ_PCS + HURT_PRAYER_PCS + 1) {
            if (weakened_seq_steps(st, 1, MUSIC_GIYGAS_WEAKENED2,
                                   pair_texts[st->exec_i], &r))
                return r;
            if (hurt_prayer_steps(st, 1 + WEAKENED_SEQ_PCS,
                                  pair_damages[st->exec_i], &r))
                return r;
            /* Pair complete */
            st->exec_i++;
            if (st->exec_i < 4) {
                st->pc = 1;  /* next pair */
                continue;
            }
            st->pc = 12;
        }

        switch (st->pc) {
        case 12:
            /* Close windows and hide HP/PP */
            redirect_close_focus_window();
            bt.battle_mode_flag = 0;
            hide_hppp_windows();
            bt.battle_mode_flag = 1;
            /* window_tick(): one frame of work, then the yield */
            window_tick_work();
            st->pc = 13;
            return STEP_RESULT_CONTINUE();

        case 13:
            /* Giygas defeated */
            bt.giygas_phase = GIYGAS_DEFEATED;
            change_music(MUSIC_GIYGAS_DEATH);
            st->scratch16[0] = 0;  /* noise table index */
            st->pc = 14;
            break;

        case 14: {  /* prayer noise sequence from the ROM table */
            size_t noise_size =
                ASSET_SIZE(ASSET_DATA_FINAL_GIYGAS_PRAYER_NOISE_TABLE_BIN);
            const uint8_t *noise_table =
                ASSET_DATA(ASSET_DATA_FINAL_GIYGAS_PRAYER_NOISE_TABLE_BIN);
            uint16_t idx = st->scratch16[0];
            if (!noise_table || idx + 1 >= noise_size) {
                st->pc = 15;
                break;
            }
            uint8_t sfx_id = noise_table[idx];
            uint8_t delay = noise_table[idx + 1];
            st->scratch16[0] = idx + 2;
            play_sfx(sfx_id);
            if (delay == 0) {
                st->pc = 15;
                break;
            }
            return push_battle_wait_frames(&child, delay);  /* stay at pc 14 */
        }

        case 15:
            /* Switch to Giygas death music phase 2 */
            change_music(MUSIC_GIYGAS_DEATH2);
            bt.giygas_phase = 0;
            st->pc = 16;
            return push_battle_wait_frames(&child, 8 * FRAMES_PER_SECOND);

        case 16:
            /* Briefly show Pokey (battler slot 9), display his text, then hide */
            bt.battlers_table[9].consciousness = 1;
            render_all_battle_sprites();
            st->pc = 17;
            if (battle_push_text(&child, MSG_BTL6_POKEY_ESCAPES))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            break;

        case 17:
            dt.blinking_triangle_flag = 0;
            bt.battlers_table[9].consciousness = 0;
            render_all_battle_sprites();
            st->pc = 18;
            return push_battle_wait_frames(&child, FRAMES_PER_SECOND);

        case 18:
            /* Static transition setup: alternate distortion + APU port 2
             * toggle. Packed state: byte 0 = apu toggle, byte 1 = shake
             * countdown, byte 2 = shake repeats. */
            bt.vertical_shake_duration = FRAMES_PER_SECOND;
            st->scratch16[0] = 0;  /* delays-table step */
            st->scratch32 = 2u | (45u << 8) | (2u << 16);
            st->pc = 19;
            break;

        case 19: {  /* per-step head: read the next delay */
            size_t delays_size = ASSET_SIZE(
                ASSET_DATA_GIYGAS_DEATH_STATIC_TRANSITION_DELAYS_BIN);
            const uint8_t *delays_data = ASSET_DATA(
                ASSET_DATA_GIYGAS_DEATH_STATIC_TRANSITION_DELAYS_BIN);
            uint16_t step = st->scratch16[0];
            if (!delays_data || (size_t)(step * 2 + 1) >= delays_size) {
                st->pc = 21;
                break;
            }
            uint16_t target_frames = read_u16_le(&delays_data[step * 2]);
            if (target_frames == 0) {
                st->pc = 21;
                break;
            }
            st->scratch16[1] = target_frames;
            st->pc = 20;
            break;
        }

        case 20: {  /* per-frame: tick + shake bookkeeping, then rotate */
            if (st->scratch16[1] != 0) {
                st->scratch16[1]--;
                window_tick_work();
                /* Decrement vertical shake and restart if repeats remain */
                uint8_t countdown = (uint8_t)(st->scratch32 >> 8);
                uint8_t repeats = (uint8_t)(st->scratch32 >> 16);
                if (repeats > 0) {
                    countdown--;
                    if (countdown == 0) {
                        repeats--;
                        countdown = 45;
                        bt.vertical_shake_duration = FRAMES_PER_SECOND;
                    }
                    st->scratch32 = (st->scratch32 & 0xFF) |
                                    ((uint32_t)countdown << 8) |
                                    ((uint32_t)repeats << 16);
                }
                return STEP_RESULT_CONTINUE();
            }
            /* Rotate distortion and toggle APU static */
            rotate_bg_distortion();
            uint8_t apu_toggle = (uint8_t)st->scratch32;
            write_apu_port2(apu_toggle);
            apu_toggle = (apu_toggle == 2) ? 1 : 2;
            st->scratch32 = (st->scratch32 & ~0xFFu) | apu_toggle;
            st->scratch16[0]++;
            st->pc = 19;
            break;
        }

        case 21:
            /* Play static noise music */
            change_music(MUSIC_GIYGAS_STATIC);
            st->pc = 22;
            return push_battle_wait_frames(&child, 10 * FRAMES_PER_SECOND);

        case 22:
            /* Final swirl and scene transition */
            play_sfx(SFX_PSI_THUNDER_DAMAGE);
            stop_music();
            start_battle_swirl(5, 0, 0);
            memset(&child, 0, sizeof(child));
            child.battle_wait.kind = BW_SWIRL_WINDOW;
            st->pc = 23;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_BATTLE_WAIT, &child);

        case 23:
            stop_music();
            st->pc = 24;
            return push_load_battle_scene(&child,
                                          ENEMY_GROUP_BOSS_GIYGAS_PHASE_FINAL,
                                          MUSIC_NONE);

        case 24:
            st->pc = 25;
            return push_battle_wait_frames(&child, 8 * FRAMES_PER_SECOND);

        case 25:
        default:
            /* Signal special defeat */
            bt.special_defeat = 3;
            return STEP_RESULT_POP(0);
        }
    }
}



static const BattleActionEntry btlact_dispatch_table[] = {
    /* Sorted by ROM address for binary search */
    { 0xC1DE43, NULL, btlact_switch_weapons_step },
    { 0xC1E00F, NULL, btlact_switch_armor_step },
    { 0xC28523, NULL, btlact_level_2_attack_step },
    { 0xC2859F, NULL, btlact_bash_step },
    { 0xC285DA, NULL, btlact_level_4_attack_step },
    { 0xC28651, NULL, btlact_level_3_attack_step },
    { 0xC286CB, NULL, btlact_level_1_attack_step },
    { 0xC28740, NULL, btlact_shoot_step },
    { 0xC28770, NULL, btlact_spy_step },
    { 0xC2889B, btlact_null, NULL },
    { 0xC2889E, btlact_steal, NULL },
    { 0xC288EB, NULL, btlact_freezetime_step },
    { 0xC289CE, NULL, btlact_diamondize_step },
    { 0xC28A92, NULL, btlact_paralyze_step },
    { 0xC28AEB, NULL, btlact_nauseate_step },
    { 0xC28B2C, NULL, btlact_poison_step },
    { 0xC28B6D, NULL, btlact_cold_step },
    { 0xC28BBE, NULL, btlact_mushroomize_step },
    { 0xC28BFD, NULL, btlact_possess_step },
    { 0xC28C69, NULL, btlact_crying_step },
    { 0xC28CB8, NULL, btlact_immobilize_step },
    { 0xC28CF1, NULL, btlact_solidify_step },
    { 0xC28D3A, NULL, btlact_brainshock_alpha_step },
    { 0xC28D5A, NULL, btlact_distract_step },
    { 0xC28DBB, NULL, btlact_feel_strange_step },
    { 0xC28DFC, NULL, btlact_crying2_step },
    { 0xC28E3B, NULL, btlact_hypnosis_alpha_step },
    { 0xC28E42, NULL, btlact_reduce_pp_step },
    { 0xC28EAE, NULL, btlact_cut_guts_step },
    { 0xC28F21, NULL, btlact_reduce_offense_defense_step },
    { 0xC28F97, NULL, btlact_level_2_attack_poison_step },
    { 0xC28FF9, NULL, btlact_double_bash_step },
    { 0xC2900B, NULL, btlact_350_fire_damage_step },
    { 0xC2902C, NULL, btlact_level_3_attack_step },  /* REDIRECT_BTLACT_LEVEL_3_ATK */
    { 0xC29033, btlact_null2, NULL },
    { 0xC29036, btlact_null3, NULL },
    { 0xC29039, btlact_null4, NULL },
    { 0xC2903C, btlact_null5, NULL },
    { 0xC2903F, btlact_null6, NULL },
    { 0xC29042, btlact_null7, NULL },
    { 0xC29045, btlact_null8, NULL },
    { 0xC29048, btlact_null9, NULL },
    { 0xC2904B, btlact_null10, NULL },
    { 0xC2904E, btlact_null11, NULL },
    { 0xC29051, NULL, btlact_neutralize_step },
    { 0xC290C6, NULL, apply_neutralize_to_all_step },
    { 0xC2916E, NULL, btlact_level_2_attack_diamondize_step },
    { 0xC29254, NULL, btlact_reduce_offense_step },
    { 0xC29298, NULL, btlact_clumsydeath_step },
    { 0xC292EB, btlact_enemy_extend, NULL },
    { 0xC292EE, NULL, btlact_masterbarfdeath_step },
    { 0xC29556, NULL, btlact_psi_rockin_alpha_step },
    { 0xC2955F, NULL, btlact_psi_rockin_beta_step },
    { 0xC29568, NULL, btlact_psi_rockin_gamma_step },
    { 0xC29571, NULL, btlact_psi_rockin_omega_step },
    { 0xC295AB, NULL, btlact_psi_fire_alpha_step },
    { 0xC295B4, NULL, btlact_psi_fire_beta_step },
    { 0xC295BD, NULL, btlact_psi_fire_gamma_step },
    { 0xC295C6, NULL, btlact_psi_fire_omega_step },
    { 0xC29647, NULL, btlact_psi_freeze_alpha_step },
    { 0xC29650, NULL, btlact_psi_freeze_beta_step },
    { 0xC29659, NULL, btlact_psi_freeze_gamma_step },
    { 0xC29662, NULL, btlact_psi_freeze_omega_step },
    { 0xC29871, NULL, btlact_psi_thunder_alpha_step },
    { 0xC2987D, NULL, btlact_psi_thunder_beta_step },
    { 0xC29889, NULL, btlact_psi_thunder_gamma_step },
    { 0xC29895, NULL, btlact_psi_thunder_omega_step },
    { 0xC29987, NULL, btlact_psi_flash_alpha_step },
    { 0xC299AE, NULL, btlact_psi_flash_beta_step },
    { 0xC299EF, NULL, btlact_psi_flash_gamma_step },
    { 0xC29A35, NULL, btlact_psi_flash_omega_step },
    { 0xC29AA6, NULL, btlact_psi_starstorm_alpha_step },
    { 0xC29AAF, NULL, btlact_psi_starstorm_omega_step },
    { 0xC29AC6, NULL, btlact_lifeup_alpha_step },
    { 0xC29ACF, NULL, btlact_lifeup_beta_step },
    { 0xC29AD8, NULL, btlact_lifeup_gamma_step },
    { 0xC29AE1, NULL, btlact_lifeup_omega_step },
    { 0xC29AEA, NULL, btlact_healing_alpha_step },
    { 0xC29B7A, NULL, btlact_healing_beta_step },
    { 0xC29C2C, NULL, btlact_healing_gamma_step },
    { 0xC29CB8, NULL, btlact_healing_omega_step },
    { 0xC29D44, NULL, btlact_shield_alpha_step },
    { 0xC29D7A, NULL, btlact_shield_alpha_step },
    { 0xC29D81, NULL, btlact_shield_beta_step },
    { 0xC29DB7, NULL, btlact_shield_beta_step },
    { 0xC29DBE, NULL, btlact_psi_shield_alpha_step },
    { 0xC29DF4, NULL, btlact_psi_shield_alpha_step },
    { 0xC29DFB, NULL, btlact_psi_shield_beta_step },
    { 0xC29E31, NULL, btlact_psi_shield_beta_step },
    { 0xC29E38, NULL, btlact_offense_up_alpha_step },
    { 0xC29E7F, NULL, btlact_offense_up_alpha_step },
    { 0xC29E86, NULL, btlact_defense_down_alpha_step },
    { 0xC29EFF, NULL, btlact_defense_down_alpha_step },
    { 0xC29F06, NULL, btlact_hypnosis_alpha_step },
    { 0xC29F57, NULL, btlact_hypnosis_alpha_step },
    { 0xC29F5E, NULL, btlact_magnet_a_step },
    { 0xC29FE1, NULL, btlact_magnet_o_step },
    { 0xC29FFE, NULL, btlact_paralysis_alpha_step },
    { 0xC2A04F, NULL, btlact_paralysis_alpha_step },
    { 0xC2A056, NULL, btlact_brainshock_alpha_step },
    { 0xC2A0A7, NULL, btlact_brainshock_alpha_step },
    { 0xC2A0AE, NULL, btlact_hp_recovery_1d4_step },
    { 0xC2A0BF, NULL, btlact_hp_recovery_50_step },
    { 0xC2A0CF, NULL, btlact_hp_recovery_200_step },
    { 0xC2A0DF, NULL, btlact_pp_recovery_20_step },
    { 0xC2A0EF, NULL, btlact_pp_recovery_80_step },
    { 0xC2A0FF, NULL, btlact_iq_up_1d4_step },
    { 0xC2A14B, NULL, btlact_guts_up_1d4_step },
    { 0xC2A193, NULL, btlact_speed_up_1d4_step },
    { 0xC2A1DB, NULL, btlact_vitality_up_1d4_step },
    { 0xC2A227, NULL, btlact_luck_up_1d4_step },
    { 0xC2A26F, NULL, btlact_hp_recovery_300_step },
    { 0xC2A27F, NULL, btlact_random_stat_up_1d4_step },
    { 0xC2A360, NULL, btlact_hp_recovery_10_step },
    { 0xC2A370, NULL, btlact_hp_recovery_100_step },
    { 0xC2A380, NULL, btlact_hp_recovery_10000_step },
    { 0xC2A39D, NULL, btlact_heal_poison_step },
    { 0xC2A3D1, NULL, btlact_counter_psi_step },
    { 0xC2A422, NULL, btlact_shield_killer_step },
    { 0xC2A46B, NULL, btlact_hp_sucker_step },
    { 0xC2A507, NULL, btlact_hp_sucker_step },
    { 0xC2A50E, NULL, btlact_mummy_wrap_step },
    { 0xC2A5D1, NULL, btlact_bottle_rocket_step },
    { 0xC2A5DA, NULL, btlact_big_bottle_rocket_step },
    { 0xC2A5E3, NULL, btlact_multi_bottle_rocket_step },
    { 0xC2A5EC, NULL, btlact_handbag_strap_step },
    { 0xC2A818, NULL, btlact_bomb_step },
    { 0xC2A821, NULL, btlact_super_bomb_step },
    { 0xC2A82A, NULL, btlact_solidify_2_step },
    { 0xC2A86B, NULL, btlact_yogurt_dispenser_step },
    { 0xC2A89D, NULL, btlact_snake_step },
    { 0xC2A902, NULL, btlact_inflict_solidification_step },
    { 0xC2A953, NULL, btlact_inflict_poison_step },
    { 0xC2A99C, NULL, btlact_bag_of_dragonite_step },
    { 0xC2AA0C, NULL, btlact_insecticide_spray_step },
    { 0xC2AA15, NULL, btlact_xterminator_spray_step },
    { 0xC2AA6D, NULL, btlact_rust_promoter_step },
    { 0xC2AA76, NULL, btlact_rust_promoter_dx_step },
    { 0xC2AA7F, NULL, btlact_sudden_guts_pill_step },
    { 0xC2AAC6, NULL, btlact_defense_spray_step },
    { 0xC2AB0D, NULL, btlact_defense_spray_step },
    { 0xC2AB71, NULL, btlact_teleport_box_step },
    { 0xC2AC2A, NULL, btlact_pray_subtle_step },
    { 0xC2AC3E, NULL, btlact_pray_warm_step },
    { 0xC2AC51, NULL, btlact_pray_golden_step },
    { 0xC2AC68, NULL, btlact_pray_mysterious_step },
    { 0xC2AC7B, NULL, btlact_pray_rainbow_step },
    { 0xC2AC99, NULL, btlact_pray_aroma_step },
    { 0xC2ACDA, NULL, btlact_pray_rending_sound_step },
    { 0xC2AD1B, NULL, btlact_pray_step },
    { 0xC2B0A1, NULL, btlact_mirror_step },
    { 0xC2B27D, NULL, btlact_eat_food_step },
    { 0xC2C13C, NULL, btlact_sow_seeds_step },
    { 0xC2C145, NULL, btlact_call_for_help_step },
    { 0xC2C14E, (void(*)(void))btlact_rainbow_of_colours, NULL },
    { 0xC2C1BD, NULL, btlact_fly_honey_step },
    { 0xC2C4C0, NULL, btlact_pokey_speech_step },
    { 0xC2C513, btlact_null12, NULL },
    { 0xC2C516, NULL, btlact_pokey_speech_2_step },
    { 0xC2C572, NULL, btlact_giygas_prayer_1_step },
    { 0xC2C5D1, NULL, btlact_giygas_prayer_2_step },
    { 0xC2C5FA, NULL, btlact_giygas_prayer_3_step },
    { 0xC2C623, NULL, btlact_giygas_prayer_4_step },
    { 0xC2C64C, NULL, btlact_giygas_prayer_5_step },
    { 0xC2C675, NULL, btlact_giygas_prayer_6_step },
    { 0xC2C69E, NULL, btlact_giygas_prayer_7_step },
    { 0xC2C6D0, NULL, btlact_giygas_prayer_8_step },
    { 0xC2C6F0, NULL, btlact_giygas_prayer_9_step },
};

#define BTLACT_DISPATCH_COUNT (sizeof(btlact_dispatch_table) / sizeof(btlact_dispatch_table[0]))

/* Binary search the sorted dispatch table. Returns the entry index, or -1. */
static int btlact_find(uint32_t rom_addr) {
    int lo = 0, hi = (int)BTLACT_DISPATCH_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (btlact_dispatch_table[mid].rom_addr == rom_addr)
            return mid;
        if (btlact_dispatch_table[mid].rom_addr < rom_addr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/*
 * JUMP_TEMP_FUNCTION_POINTER — Port of asm/overworld/jump_temp_function_pointer.asm.
 * Assembly: JML (TEMP_FUNCTION_POINTER) — indirect long jump through a
 * 24-bit ROM address stored in bt.temp_function_pointer. The C port
 * dispatches through btlact_dispatch_table instead; the ROM addresses come
 * from the battle_action_table asset (loaded from the donor ROM).
 *
 * Only reached for stepper-less (pure, non-yielding) actions: every action
 * with a resumable stepper is routed to a STEP_PUSH by battle_action_dispatch
 * (its only caller), so an entry reaching here always has a non-NULL .func and
 * a NULL .step. The former blocking pump path (btlact_pump over the resumable
 * form) was deleted with pump_mode at cutover.
 */
void jump_temp_function_pointer(void) {
    int idx = btlact_find(bt.temp_function_pointer);
    if (idx < 0) {
        LOG_WARN("WARN: unknown battle action ROM addr $%06X\n", bt.temp_function_pointer);
        return;
    }
    if (!btlact_dispatch_table[idx].func) {
        /* A stepper-bearing action reached the blocking path — should be
         * impossible (battle_action_dispatch routes those via STEP_PUSH). */
        LOG_WARN("WARN: battle action $%06X has no blocking form\n",
                 bt.temp_function_pointer);
        return;
    }
    btlact_dispatch_table[idx].func();
}

bool battle_action_dispatch(uint32_t func_addr, ModeState *init) {
    bt.temp_function_pointer = func_addr;
    int idx = btlact_find(func_addr);
    if (idx >= 0 && btlact_dispatch_table[idx].step) {
        memset(init, 0, sizeof(*init));
        init->battle_action.table_index = (uint16_t)idx;
        return true;
    }
    jump_temp_function_pointer();  /* unconverted/unknown: inline (warns) */
    return false;
}

StepResult mode_step_battle_action(ModeState *ms) {
    BattleActionState *st = &ms->battle_action;
    if (st->table_index >= BTLACT_DISPATCH_COUNT ||
        !btlact_dispatch_table[st->table_index].step) {
        LOG_WARN("WARN: BATTLE_ACTION with no stepper (index %u)\n",
                 (unsigned)st->table_index);
        return STEP_RESULT_POP(0);
    }
    return btlact_dispatch_table[st->table_index].step(st);
}

