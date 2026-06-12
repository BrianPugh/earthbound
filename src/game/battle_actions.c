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
#include <stdio.h>
#include <string.h>

#include "game_main.h"

/* Blocking bridge for converted actions (defined with the dispatch table at
 * the bottom of this file): looks up the action's resumable stepper by its
 * ROM address and pumps it to completion. */
static void btlact_pump_addr(uint32_t rom_addr);

/* ======================================================================
 * Battle action handlers
 *
 * Converted actions (the GAME_MODE_BATTLE_ACTION long tail) are small
 * btlact_*_step() pc-machines: texts are DISPLAY_TEXT pushes via
 * battle_push_text / battle_push_text_ex (battle.c), and every resume pc
 * starts with the dt.blinking_triangle_flag clear (the blocking
 * display_in_battle_text epilogue). The blocking btlact_*() form remains as
 * a btlact_pump_addr() bridge for direct action→action C calls; table
 * dispatch prefers the stepper (see jump_temp_function_pointer).
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

void btlact_bash(void) { btlact_pump_addr(0xC2859F); }

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

void btlact_shoot(void) { btlact_pump_addr(0xC28740); }

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

void btlact_spy(void) { btlact_pump_addr(0xC28770); }

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

void btlact_level_1_attack(void) { btlact_pump_addr(0xC286CB); }

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

/* ----------------------------------------------------------------------
 * The Healing PSI cascade (alpha ⊂ beta ⊂ gamma ⊂ omega).
 *
 * Each decide helper applies its cures and returns the tail text; the
 * fallback chain mirrors the blocking originals' tail calls. Returns 0
 * when the revive path handled everything itself — battle_revive_target()
 * displays its own text and runs the enemy palette flash inline-blocking
 * (it converts with the shared battle.c helper pipeline).
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

/* BTLACT_HEALING_G (asm/battle/actions/healing_gamma.asm):
 * cures paralysis, diamondize; revives (75%, hp_max/4) — revive failure has
 * its own text; falls back to Healing Beta. */
static uint32_t healing_gamma_decide(Battler *tgt) {
    uint8_t easyheal = tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];

    if (easyheal == STATUS_0_PARALYZED) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        return MSG_BTL5_CURED_NUMB;
    }
    if (easyheal == STATUS_0_DIAMONDIZED) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        return MSG_BTL5_CURED_DIAMONDIZED;
    }
    if (easyheal == STATUS_0_UNCONSCIOUS) {
        /* 75% chance to revive */
        if (battle_success_255(192)) {
            /* Revive with hp_max / 4 (inline-blocking, own text) */
            battle_revive_target(tgt, tgt->hp_max >> 2);
            return 0;
        }
        return MSG_BTL5_REVIVE_FAILED;
    }

    return healing_beta_decide(tgt);
}

/* BTLACT_HEALING_O (asm/battle/actions/healing_omega.asm):
 * revives with full HP; falls back to Healing Gamma. */
static uint32_t healing_omega_decide(Battler *tgt) {
    if (tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS) {
        battle_revive_target(tgt, tgt->hp_max);  /* inline-blocking, own text */
        return 0;
    }
    return healing_gamma_decide(tgt);
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

static StepResult btlact_healing_gamma_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? healing_gamma_decide(battler_from_offset(bt.current_target)) : 0;
    return btlact_single_text_step(st, msg);
}

static StepResult btlact_healing_omega_step(BattleActionState *st) {
    uint32_t msg = (st->pc == 0)
        ? healing_omega_decide(battler_from_offset(bt.current_target)) : 0;
    return btlact_single_text_step(st, msg);
}

void btlact_healing_alpha(void) { btlact_pump_addr(0xC29AEA); }
void btlact_healing_beta(void)  { btlact_pump_addr(0xC29B7A); }
void btlact_healing_gamma(void) { btlact_pump_addr(0xC29C2C); }
void btlact_healing_omega(void) { btlact_pump_addr(0xC29CB8); }

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

void btlact_shield_alpha(void)     { btlact_pump_addr(0xC29D44); }
void btlact_shield_beta(void)      { btlact_pump_addr(0xC29D81); }
void btlact_psi_shield_alpha(void) { btlact_pump_addr(0xC29DBE); }
void btlact_psi_shield_beta(void)  { btlact_pump_addr(0xC29DFB); }

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

void btlact_hp_recovery_10(void)    { btlact_pump_addr(0xC2A360); }
void btlact_hp_recovery_50(void)    { btlact_pump_addr(0xC2A0BF); }
void btlact_hp_recovery_100(void)   { btlact_pump_addr(0xC2A370); }
void btlact_hp_recovery_200(void)   { btlact_pump_addr(0xC2A0CF); }
void btlact_hp_recovery_300(void)   { btlact_pump_addr(0xC2A26F); }
void btlact_hp_recovery_1d4(void)   { btlact_pump_addr(0xC2A0AE); }
void btlact_hp_recovery_10000(void) { btlact_pump_addr(0xC2A380); }
void btlact_pp_recovery_20(void)    { btlact_pump_addr(0xC2A0DF); }
void btlact_pp_recovery_80(void)    { btlact_pump_addr(0xC2A0EF); }

/* ======================================================================
 * Simple wrapper actions
 * ====================================================================== */

/*
 * BTLACT_DOUBLE_BASH (asm/battle/actions/bash_twice.asm)
 *
 * Execute bash attack twice.
 */
void btlact_double_bash(void) {
    btlact_bash();
    btlact_bash();
}

/*
 * BTLACT_FREEZETIME (asm/battle/actions/freeze_time.asm)
 *
 * Multi-hit bash with time frozen. Pauses HPPP rolling, executes 1-5 bash
 * attacks on randomly selected living targets, then resumes rolling.
 * Each hit picks a random target from the current target set.
 */
void btlact_freezetime(void) {
    /* PAUSE_MUSIC: disable HPPP rolling */
    bt.disable_hppp_rolling = 1;

    /* 1-5 hits */
    uint16_t hits = rand_limit(4) + 1;

    /* Save and work with target flags */
    uint32_t saved_flags = bt.battler_target_flags;

    for (uint16_t i = 0; i < hits; i++) {
        /* Assembly filters whatever is currently in battler_target_flags
         * (the single target from the previous hit, or original on first pass).
         * If that single target is now untargetable, flags go to 0 → exit. */
        battle_remove_status_untargettable_targets();
        if (bt.battler_target_flags == 0)
            break;

        /* Assembly passes the original UNFILTERED saved flags to RANDOM_TARGETTING,
         * not the filtered set. This means it can "waste" hits on untargetable targets. */
        uint32_t single_target = battle_random_targeting(saved_flags);
        bt.battler_target_flags = single_target;

        /* Find the targeted battler */
        for (uint16_t j = 0; j < BATTLER_COUNT; j++) {
            if (battle_is_char_targeted(j)) {
                bt.current_target = j * sizeof(Battler);
                break;
            }
        }
        fix_target_name();
        btlact_bash();
    }

    /* RESUME_MUSIC: clear rolling flags */
    bt.half_hppp_meter_speed = 0;
    bt.disable_hppp_rolling = 0;

    display_in_battle_text_addr(MSG_BTL8_TIME_STARTED_AGAIN);
    bt.battler_target_flags = 0;
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

void btlact_poison(void)       { btlact_pump_addr(0xC28B2C); }
void btlact_nauseate(void)     { btlact_pump_addr(0xC28AEB); }
void btlact_feel_strange(void) { btlact_pump_addr(0xC28DBB); }
void btlact_immobilize(void)   { btlact_pump_addr(0xC28CB8); }

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

void btlact_level_2_attack_poison(void)     { btlact_pump_addr(0xC28F97); }
void btlact_level_2_attack_diamondize(void) { btlact_pump_addr(0xC2916E); }

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
void psi_thunder_common(uint16_t base_damage, uint16_t hits) {
    /* Count targeted battlers */
    uint16_t target_count = 0;
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        if (battle_is_char_targeted(i))
            target_count++;
    }

    /* Effective damage = count * base, capped at 255 */
    uint16_t effective_damage = target_count * 64;
    if (effective_damage > 255)
        effective_damage = 255;

    /* Save original target flags */
    uint32_t saved_flags = bt.battler_target_flags;

    for (uint16_t hit = 0; hit < hits; hit++) {
        /* Restore original targets, then remove dead/diamondized */
        bt.battler_target_flags = saved_flags;
        battle_remove_status_untargettable_targets();

        /* If no valid targets remain, stop */
        if (bt.battler_target_flags == 0)
            break;

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
        if (battle_success_255(effective_damage)) {
            /* Hit — display text based on damage tier */
            if (base_damage == 120) {
                display_in_battle_text_addr(MSG_BTL0_PSI_THUNDER_HIT_SMALL);
            } else {
                display_in_battle_text_addr(MSG_BTL0_PSI_THUNDER_HIT_LARGE);
            }

            /* Wait for PSI animation to finish */
            while (is_psi_animation_active()) {
                window_tick();
            }

            Battler *target = battler_from_offset(bt.current_target);
            target->use_alt_spritemap = 0;

            /* Franklin Badge check — allies only */
            if (target->ally_or_enemy == 0) {
                uint16_t char_id = (target->row & 0xFF) + 1;
                if (find_item_in_inventory2(char_id, 1)) { /* 1 = FRANKLIN_BADGE */
                    display_in_battle_text_addr(MSG_BTL5_FRANKLIN_BADGE_DEFLECTED);
                    bt.damage_is_reflected = 1;
                    swap_attacker_with_target();
                }
            }

            /* Shield alpha/beta: set shield_hp to 1 (absorbs one hit) */
            target = battler_from_offset(bt.current_target);
            uint8_t shield = target->afflictions[STATUS_GROUP_SHIELD];
            if (shield == 1 || shield == 2) {
                target->shield_hp = 1;
            }

            /* PSI shield nullify check */
            if (!battle_psi_shield_nullify()) {
                uint16_t damage = battle_50pct_variance(base_damage);
                battle_calc_resist_damage(damage, 0xFF);
            }

            battle_weaken_shield();
        } else {
            /* Miss */
            display_in_battle_text_addr(MSG_BTL0_PSI_THUNDER_MISS);
            display_in_battle_text_addr(MSG_BTL6_THUNDER_MISSED);
        }

        /* Check if either side is wiped out */
        if (battle_count_chars(0) == 0 || battle_count_chars(1) == 0)
            break;
    }

    /* Clear targeting */
    bt.battler_target_flags = 0;
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

void btlact_psi_fire_alpha(void) { btlact_pump_addr(0xC295AB); }
void btlact_psi_fire_beta(void)  { btlact_pump_addr(0xC295B4); }
void btlact_psi_fire_gamma(void) { btlact_pump_addr(0xC295BD); }
void btlact_psi_fire_omega(void) { btlact_pump_addr(0xC295C6); }

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

void btlact_psi_freeze_alpha(void) { btlact_pump_addr(0xC29647); }
void btlact_psi_freeze_beta(void)  { btlact_pump_addr(0xC29650); }
void btlact_psi_freeze_gamma(void) { btlact_pump_addr(0xC29659); }
void btlact_psi_freeze_omega(void) { btlact_pump_addr(0xC29662); }

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

void btlact_psi_rockin_alpha(void) { btlact_pump_addr(0xC29556); }
void btlact_psi_rockin_beta(void)  { btlact_pump_addr(0xC2955F); }
void btlact_psi_rockin_gamma(void) { btlact_pump_addr(0xC29568); }
void btlact_psi_rockin_omega(void) { btlact_pump_addr(0xC29571); }

static StepResult btlact_psi_starstorm_alpha_step(BattleActionState *st) {
    return btlact_psi_fire_step_common(st, STARSTORM_ALPHA_DAMAGE, false);
}
static StepResult btlact_psi_starstorm_omega_step(BattleActionState *st) {
    return btlact_psi_fire_step_common(st, STARSTORM_OMEGA_DAMAGE, false);
}

void btlact_psi_starstorm_alpha(void) { btlact_pump_addr(0xC29AA6); }
void btlact_psi_starstorm_omega(void) { btlact_pump_addr(0xC29AAF); }

void btlact_psi_thunder_alpha(void) { psi_thunder_common(THUNDER_ALPHA_DAMAGE, THUNDER_ALPHA_HITS); }
void btlact_psi_thunder_beta(void)  { psi_thunder_common(THUNDER_BETA_DAMAGE, THUNDER_BETA_HITS); }
void btlact_psi_thunder_gamma(void) { psi_thunder_common(THUNDER_GAMMA_DAMAGE, THUNDER_GAMMA_HITS); }
void btlact_psi_thunder_omega(void) { psi_thunder_common(THUNDER_OMEGA_DAMAGE, THUNDER_OMEGA_HITS); }

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

void btlact_lifeup_alpha(void) { btlact_pump_addr(0xC29AC6); }
void btlact_lifeup_beta(void)  { btlact_pump_addr(0xC29ACF); }
void btlact_lifeup_gamma(void) { btlact_pump_addr(0xC29AD8); }
void btlact_lifeup_omega(void) { btlact_pump_addr(0xC29AE1); }

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

void btlact_bottle_rocket(void)       { btlact_pump_addr(0xC2A5D1); }
void btlact_big_bottle_rocket(void)   { btlact_pump_addr(0xC2A5DA); }
void btlact_multi_bottle_rocket(void) { btlact_pump_addr(0xC2A5E3); }

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

void btlact_insecticide_spray(void) { btlact_pump_addr(0xC2AA0C); }
void btlact_xterminator_spray(void) { btlact_pump_addr(0xC2AA15); }
void btlact_rust_promoter(void)     { btlact_pump_addr(0xC2AA6D); }
void btlact_rust_promoter_dx(void)  { btlact_pump_addr(0xC2AA76); }

/*
 * BOMB_COMMON (asm/battle/actions/bomb_common.asm)
 *
 * Area damage: deals base_damage (with 50% variance) to primary target, then
 * finds adjacent battlers (left/right) and deals half base_damage to each.
 * For party targets: adjacent = party members in neighboring slots.
 * For enemy targets: adjacent = enemies in same row within sprite blast range.
 */
void bomb_common(uint16_t base_damage) {
    /* 0xFFFF = no adjacent found (can't use 0 since byte offset 0 is valid) */
    uint16_t adjacent_left = 0xFFFF;
    uint16_t adjacent_right = 0xFFFF;

    /* Phase 1: Primary damage with 50% variance, full resist (0xFF) */
    uint16_t damage = battle_50pct_variance(base_damage);
    battle_calc_resist_damage(damage, 0xFF);

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
         * Assembly reads the byte after the last party_members entry (which
         * is leader_x_frac in WRAM, always >= 6) and skips if >= 6. We
         * guard with a bounds check instead to avoid OOB access. */
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

    /* Phase 3: Apply half base_damage to adjacent targets */
    uint16_t saved_target = bt.current_target;

    if (adjacent_left != 0xFFFF) {
        bt.current_target = adjacent_left;
        fix_target_name();
        uint16_t splash = battle_50pct_variance(base_damage >> 1);
        battle_calc_resist_damage(splash, 0xFF);
    }

    if (adjacent_right != 0xFFFF) {
        bt.current_target = adjacent_right;
        fix_target_name();
        uint16_t splash = battle_50pct_variance(base_damage >> 1);
        battle_calc_resist_damage(splash, 0xFF);
    }

    /* Restore original target */
    bt.current_target = saved_target;
    fix_target_name();
}

void btlact_bomb(void)       { bomb_common(90); }
void btlact_super_bomb(void) { bomb_common(270); }

/*
 * BTLACT_TELEPORT_BOX (asm/battle/actions/teleport_box.asm)
 *
 * Item-based battle escape. Checks sector attributes for teleport usability
 * (bit 7 = cannot teleport). Outside battle, always succeeds. In battle,
 * success is probability-based using item strength, and fails in boss battles.
 * On success: removes item from inventory, sets instant teleport, bt.special_defeat=1.
 */
void btlact_teleport_box(void) {
    /* Check sector attributes — bit 7 means teleport unusable in this area */
    uint16_t attrs = load_sector_attrs(
        game_state.leader_x_coord, game_state.leader_y_coord);
    if (attrs & 0x0080) {
        display_in_battle_text_addr(MSG_GOODS1_TELEPORT_BOX_CANT_USE_HERE);
        return;
    }
    Battler *attacker = battler_from_offset(bt.current_attacker);
    /* Outside battle, always succeeds */
    if (ow.battle_mode == 0)
        goto teleport_success;
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
        if (roll >= (uint16_t)threshold) {
            display_in_battle_text_addr(MSG_GOODS1_TELEPORT_BOX_MALFUNCTION);
            return;
        }
    }
    /* Fail in boss battles */
    if (battle_boss_battle_check() == 0) {
        display_in_battle_text_addr(MSG_GOODS1_TELEPORT_BOX_MALFUNCTION);
        return;
    }

teleport_success:;
    /* Remove item from inventory */
    uint8_t slot = attacker->action_item_slot & 0xFF;
    remove_item_from_inventory(attacker->id, slot);
    display_in_battle_text_addr(MSG_GOODS1_TELEPORT_BOX_SUCCESS);
    /* Set teleport state: instant teleport to current destination */
    ow.psi_teleport_destination = game_state.unknownC3;
    ow.psi_teleport_style = 3;  /* TELEPORT_STYLE::INSTANT */
    bt.special_defeat = 1;
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
void call_for_help_common(uint16_t param) {
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

    if (param) {
        display_in_battle_text_addr(MSG_BTL8_SEED_STARTED_GROWING); /* seeds sprouted */
    } else {
        display_in_battle_text_addr(MSG_BTL8_ALLY_JOINED_BATTLE); /* called for help */
    }
    return;

help_failed:
    if (param) {
        display_in_battle_text_addr(MSG_BTL8_SEED_DIDNT_SPROUT); /* seeds didn't sprout */
    } else {
        display_in_battle_text_addr(MSG_BTL8_NO_ALLY_CAME); /* nobody came */
    }
}

void btlact_call_for_help(void) { call_for_help_common(0); }
void btlact_sow_seeds(void)    { call_for_help_common(1); }

/*
 * BTLACT_HP_SUCKER (asm/battle/actions/hp_sucker.asm)
 *
 * HP drain attack used by Hungry HP-sucker enemy.
 * Luck80 check, then drains target's max HP / 8 (with 50% variance)
 * and heals the attacker by the same amount.
 * If target == attacker (self-targeting via strangeness), displays special text.
 * KOs target if HP reaches 0.
 */
void btlact_hp_sucker(void) {
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }

    /* Attacker must be alive (hp_target > 0) */
    Battler *attacker = battler_from_offset(bt.current_attacker);
    if (attacker->hp_target == 0) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }

    /* Self-targeting check (e.g., feeling strange) */
    if (bt.current_target == bt.current_attacker) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DRAINED_OWN_HP);
        return;
    }

    /* Calculate drain amount: 50% variance on target's max HP, then /8 */
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t drain_amount = battle_50pct_variance(target->hp_max) >> 3;

    /* Reduce target's HP */
    battle_reduce_hp(target, drain_amount);

    display_text_wait_addr(MSG_BTL4_RESULT_HP_DRAINED_FROM, drain_amount);

    /* Heal attacker by the drain amount */
    uint16_t new_attacker_hp = attacker->hp + drain_amount;
    battle_set_hp(attacker, new_attacker_hp);

    /* KO target if dead */
    if (target->hp == 0)
        battle_ko_target(target);
}

void btlact_hungry_hp_sucker(void) { btlact_hp_sucker(); }


/*
 * BTLACT_MIRROR (asm/battle/actions/mirror.asm)
 *
 * Enemy morphing action. The attacker copies the target's battler data
 * (keeping its own HP/PP). Checks: target must be enemy (ally_or_enemy != 0),
 * target must not be an NPC ally (npc_id == 0), and a random roll must be
 * below the enemy's mirror_success rate from enemy_config_table.
 * On success, backs up attacker to bt.mirror_battler_backup and copies target data.
 */
void btlact_mirror(void) {
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t target_id = target->id;

    /* Must target an enemy (not ally) */
    if ((target->ally_or_enemy & 0xFF) == 0) {
        display_in_battle_text_addr(MSG_BTL5_MORPH_FAILED);
        return;
    }
    /* Must not be an NPC ally */
    if ((target->npc_id & 0xFF) != 0) {
        display_in_battle_text_addr(MSG_BTL5_MORPH_FAILED);
        return;
    }
    /* Check mirror success rate from enemy config table */
    uint16_t roll = rand_limit(100);
    if (enemy_config_table != NULL) {
        uint8_t success_rate = enemy_config_table[target_id].mirror_success;
        if (roll >= success_rate) {
            display_in_battle_text_addr(MSG_BTL5_MORPH_FAILED);
            return;
        }
    }

    /* Success: set up mirror state */
    bt.mirror_enemy = target_id;
    bt.mirror_turn_timer = DEFAULT_MIRROR_TURN_COUNT;

    /* Backup attacker's current state */
    Battler *attacker = battler_from_offset(bt.current_attacker);
    memcpy(&bt.mirror_battler_backup, attacker, sizeof(Battler));

    /* Copy target's data to attacker (preserving attacker's HP/PP/identity) */
    battle_copy_mirror_data(attacker, target);

    display_in_battle_text_addr(MSG_BTL5_MORPH_SUCCESS);
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
static const uint8_t *battle_apply_condiment(void) {
    Battler *atk = battler_from_offset(bt.current_attacker);
    uint8_t food_id = atk->current_action_argument & 0xFF;

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
            /* Condiment match — display "great flavor!" text and return condiment params */
            display_in_battle_text_addr(MSG_GOODS0_CONDIMENT_TASTED_GOOD);
            return &entry->strength;  /* points to [strength, epi, ep, special] */
        }
        /* Wrong condiment for this food */
        display_in_battle_text_addr(MSG_GOODS0_CONDIMENT_BAD_TASTE);
        return default_params;
    }

    /* Food not in condiment table — wrong condiment */
    display_in_battle_text_addr(MSG_GOODS0_CONDIMENT_BAD_TASTE);
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
void btlact_eat_food(void) {
    Battler *tgt = battler_from_offset(bt.current_target);
    uint16_t char_id = tgt->id;  /* @LOCAL03: 1-indexed character ID */

    /* Assembly lines 20-30: if target is unconscious, show no-effect text */
    uint16_t idx = char_id - 1;
    if (party_characters[idx].afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL]
            == STATUS_0_UNCONSCIOUS) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }

    /* Assembly lines 31-35: apply condiment, get params pointer */
    const uint8_t *params = battle_apply_condiment();
    if (!params)
        return;

    /*
     * Assembly lines 36-51: select amount field.
     * Poo (char_id == 4) uses params[2] (ep); others use params[1] (epi).
     * @LOCAL02 = amount, effect_type = params[0].
     */
    uint8_t amount     = (char_id == PARTY_MEMBER_POO) ? params[2] : params[1];
    uint8_t effect_type = params[0];

    /* Assembly lines 55-76: dispatch on effect type */
    switch (effect_type) {

    case 0: /* HP recovery */
        if (amount == 0) {
            battle_recover_hp(tgt, 30000);
        } else {
            uint16_t heal = battle_25pct_variance((uint16_t)amount * 6);
            battle_recover_hp(tgt, heal);
        }
        break;

    case 1: /* PP recovery */
        if (amount == 0) {
            battle_recover_pp(tgt, 30000);
        } else {
            uint16_t pp = battle_25pct_variance(amount);
            battle_recover_pp(tgt, pp);
        }
        break;

    case 2: /* HP + PP recovery */
        /* HP portion */
        if (amount == 0) {
            battle_recover_hp(tgt, 30000);
        } else {
            uint16_t heal = battle_25pct_variance((uint16_t)amount * 6);
            battle_recover_hp(tgt, heal);
        }
        /* PP portion — re-read amount (@LOCAL02 reload in assembly) */
        if (amount == 0) {
            battle_recover_pp(tgt, 30000);
        } else {
            uint16_t pp = battle_25pct_variance(amount);
            battle_recover_pp(tgt, pp);
        }
        break;

    case 3: /* Random stat boost (one of IQ/Guts/Speed/Vitality/Luck) */
        switch (rand_limit(4)) {
        case 0: goto do_boost_iq;
        case 1: goto do_boost_guts;
        case 2: goto do_boost_speed;
        case 3: goto do_boost_vitality;
        default: goto check_special;  /* rand_limit(4) can return 0-3 only */
        }
        break;

    case 4: goto do_boost_iq;
    case 5: goto do_boost_guts;
    case 6: goto do_boost_speed;
    case 7: goto do_boost_vitality;
    case 8: goto do_boost_luck;

    case 9: /* Heal status (BTLACT_HEALING_A: cures cold/sunstroke/sleep) */
        btlact_healing_alpha();
        break;

    case 10: /* Cure poison */
        btlact_heal_poison();
        break;

    default:
        break;
    }
    goto check_special;

do_boost_iq:
    /*
     * Assembly @BOOST_IQ: increment battler.iq (8-bit), then increment
     * party_characters[idx].boosted_iq (8-bit), then recalculate IQ.
     */
    tgt->iq += (uint8_t)amount;
    party_characters[idx].boosted_iq += (uint8_t)amount;
    recalc_character_postmath_iq(char_id);
    display_text_wait_addr(MSG_BTL6_IQ_WENT_UP, amount);
    goto check_special;

do_boost_guts:
    /*
     * Assembly @BOOST_GUTS: increment battler.guts (16-bit), then increment
     * party_characters[idx].boosted_guts (8-bit), then recalculate guts.
     */
    tgt->guts += (uint16_t)amount;
    party_characters[idx].boosted_guts += (uint8_t)amount;
    recalc_character_postmath_guts(char_id);
    display_text_wait_addr(MSG_BTL6_GUTS_WENT_UP, amount);
    goto check_special;

do_boost_speed:
    /*
     * Assembly @BOOST_SPEED: increment battler.speed (16-bit), then increment
     * party_characters[idx].boosted_speed (8-bit), then recalculate speed.
     */
    tgt->speed += (uint16_t)amount;
    party_characters[idx].boosted_speed += (uint8_t)amount;
    recalc_character_postmath_speed(char_id);
    display_text_wait_addr(MSG_BTL6_SPEED_WENT_UP, amount);
    goto check_special;

do_boost_vitality:
    /*
     * Assembly @BOOST_VITALITY: increment battler.vitality (8-bit), then
     * increment party_characters[idx].boosted_vitality (8-bit), then
     * recalculate vitality.
     */
    tgt->vitality += (uint8_t)amount;
    party_characters[idx].boosted_vitality += (uint8_t)amount;
    recalc_character_postmath_vitality(char_id);
    display_text_wait_addr(MSG_BTL6_VITALITY_WENT_UP, amount);
    goto check_special;

do_boost_luck:
    /*
     * Assembly @BOOST_LUCK: increment battler.luck (16-bit), then increment
     * party_characters[idx].boosted_luck (8-bit), then recalculate luck.
     */
    tgt->luck += (uint16_t)amount;
    party_characters[idx].boosted_luck += (uint8_t)amount;
    recalc_character_postmath_luck(char_id);
    display_text_wait_addr(MSG_BTL6_LUCK_WENT_UP, amount);
    /* fall through to check_special */

check_special:
    /*
     * Assembly @CHECK_CONDIMENT_SPECIAL (lines 368-380):
     * If params[3] (special) != 0: schedule party animation reset
     * with (special * 6) frames of delay.
     */
    {
        uint8_t special = params[3];
        if (special != 0) {
            battle_schedule_party_animation_reset((uint16_t)special * 6);
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

void btlact_350_fire_damage(void) { btlact_pump_addr(0xC2900B); }

/*
 * BTLACT_BAG_OF_DRAGONITE (asm/battle/actions/bag_of_dragonite.asm)
 *
 * Fixed 800 fire damage with 25% variance, modified by fire resistance.
 */
void btlact_bag_of_dragonite(void) {
    uint16_t damage = battle_25pct_variance(800);
    Battler *target = battler_from_offset(bt.current_target);
    battle_calc_resist_damage(damage, target->fire_resist);
}

/*
 * BTLACT_YOGURT_DISPENSER (asm/battle/actions/yogurt_dispenser.asm)
 *
 * Speed-based check, then 1-4 damage.
 */
void btlact_yogurt_dispenser(void) {
    if (!battle_success_speed(250)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    uint16_t damage = rand_limit(4) + 1;
    battle_calc_resist_damage(damage, 0xFF);
}

/*
 * BTLACT_SNAKE (asm/battle/actions/snake.asm)
 *
 * 1-4 damage, 50% chance to poison. Fails on NPCs.
 */
void btlact_snake(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!battle_success_speed(250)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    uint16_t damage = rand_limit(4) + 1;
    battle_calc_resist_damage(damage, 0xFF);

    /* 50% chance to poison */
    if (battle_success_255(128)) {
        uint16_t result = battle_inflict_status(
            battler_from_offset(bt.current_target),
            STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_POISONED);
        if (result != 0) {
            display_in_battle_text_addr(MSG_BTL5_STATUS_POISONED);
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
void btlact_cold(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->freeze_resist)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_COLD);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_COLD);
    } else {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
    }
}

/*
 * BTLACT_INFLICT_POISON (asm/battle/actions/inflict_poison.asm)
 *
 * Inflict poison with paralysis_resist check. No NPC check.
 */
void btlact_inflict_poison(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_POISONED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_POISONED);
    } else {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
    }
}

/*
 * BTLACT_PARALYZE (asm/battle/actions/paralyze.asm)
 *
 * Inflict paralysis. Luck80 check + paralysis_resist check. Fails on NPCs.
 */
void btlact_paralyze(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_PARALYZED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_NUMB);
    } else {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
    }
}

/*
 * BTLACT_INFLICT_SOLIDIFICATION (asm/battle/actions/inflict_solidification.asm)
 *
 * Inflict solidified. Luck80 check + paralysis_resist check. No NPC check.
 */
void btlact_inflict_solidification(void) {
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_SOLIDIFIED);
    } else {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
    }
}

/*
 * BTLACT_COUNTER_PSI (asm/battle/actions/counter_psi.asm)
 *
 * Seal target's PSI for 4 turns. Luck40 check. Fails on NPCs.
 * Won't stack if already can't concentrate.
 */
void btlact_counter_psi(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!battle_success_luck40()) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (target->afflictions[STATUS_GROUP_CONCENTRATION] != 0) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    target->afflictions[STATUS_GROUP_CONCENTRATION] = 4;
    display_in_battle_text_addr(MSG_BTL5_STATUS_PSI_BLOCKED);
}

/*
 * BTLACT_DISTRACT (asm/battle/actions/distract.asm)
 *
 * Make target unable to concentrate for 4 turns.
 * Luck40 + paralysis_resist check. Fails on NPCs.
 * Sets CANT_CONCENTRATE4 (value 4) if concentration slot is empty.
 */
void btlact_distract(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!battle_success_luck40()) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    if (target->afflictions[STATUS_GROUP_CONCENTRATION] != 0) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    target->afflictions[STATUS_GROUP_CONCENTRATION] = STATUS_4_CANT_CONCENTRATE4;
    display_in_battle_text_addr(MSG_BTL5_STATUS_PSI_BLOCKED);
}

/*
 * BTLACT_NEUTRALIZE (asm/battle/actions/neutralize.asm)
 *
 * Reset all combat stats to base values, remove shields.
 */
void btlact_neutralize(void) {
    Battler *target = battler_from_offset(bt.current_target);
    target->offense = target->base_offense;
    target->defense = target->base_defense;
    target->speed   = target->base_speed;
    target->guts    = target->base_guts;
    target->luck    = target->base_luck;
    target->shield_hp = 0;
    target->afflictions[STATUS_GROUP_SHIELD] = 0;
    display_in_battle_text_addr(MSG_BTL5_PSI_EFFECTS_NEUTRALIZED);
}


/*
 * HEAL_POISON (asm/battle/actions/heal_poison.asm)
 *
 * Cure poison status (group 0 value 5) from current target.
 */
void btlact_heal_poison(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_POISONED) {
        target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        display_in_battle_text_addr(MSG_BTL5_CURED_POISONED);
    }
}

/*
 * BTLACT_SHIELD_KILLER (asm/battle/actions/shield_killer.asm)
 *
 * Remove shield from target. Luck80 check.
 */
void btlact_shield_killer(void) {
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (target->afflictions[STATUS_GROUP_SHIELD] == 0) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    target->afflictions[STATUS_GROUP_SHIELD] = 0;
    display_in_battle_text_addr(MSG_BTL5_SHIELD_DISAPPEARED);
}

/* ======================================================================
 * Redirect wrappers (enemy reuse of player PSI)
 * ====================================================================== */

void redirect_btlact_brainshock_alpha(void)  { btlact_brainshock_alpha(); }
void redirect_btlact_hypnosis_alpha(void)    { btlact_hypnosis_alpha(); }
void redirect_btlact_paralysis_alpha(void)   { btlact_paralysis_alpha(); }
void redirect_btlact_offense_up_alpha(void)  { btlact_offense_up_alpha(); }
void redirect_btlact_defense_down_alpha(void) { btlact_defense_down_alpha(); }
void redirect_btlact_shield_alpha(void)      { btlact_shield_alpha(); }
void redirect_btlact_shield_beta(void)       { btlact_shield_beta(); }
void redirect_btlact_psi_shield_alpha(void)  { btlact_psi_shield_alpha(); }
void redirect_btlact_psi_shield_beta(void)   { btlact_psi_shield_beta(); }
/* Additional redirect copies (asm/battle/actions/ copy and redirect variants) */
void redirect_btlact_brainshock_a_copy(void)     { btlact_brainshock_alpha(); }
void redirect_btlact_hypnosis_a_copy(void)       { btlact_hypnosis_alpha(); }

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

void btlact_diamondize(void) { btlact_pump_addr(0xC289CE); }

/*
 * BTLACT_POSSESS (asm/battle/actions/possess.asm)
 *
 * Possesses target (ally only). Inflicts POSSESSED status.
 * If the first enemy slot (index TOTAL_PARTY_COUNT) is empty (unconscious),
 * spawns a Tiny Lil' Ghost there as an NPC ally for the possessor.
 */
void btlact_possess(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    /* Only works on allies (party members) */
    if ((target->ally_or_enemy & 0xFF) != 0) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_HARDHEAL, STATUS_1_POSSESSED);
    if (result == 0) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    display_in_battle_text_addr(MSG_BTL5_STATUS_POSSESSED_GHOST);
    /* If first enemy slot is empty, spawn Tiny Lil' Ghost as NPC ally */
    if ((bt.battlers_table[TOTAL_PARTY_COUNT].consciousness & 0xFF) == 0) {
        battle_init_enemy_stats(&bt.battlers_table[TOTAL_PARTY_COUNT],
                                ENEMY_TINY_LIL_GHOST);
        bt.battlers_table[TOTAL_PARTY_COUNT].npc_id = ENEMY_TINY_LIL_GHOST;
        bt.battlers_table[TOTAL_PARTY_COUNT].has_taken_turn = 1;
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
void btlact_reduce_pp(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->pp_target == 0) {
        display_in_battle_text_addr(MSG_BTL6_TARGET_HAS_NO_PP);
        return;
    }
    uint16_t drain = target->pp_max / 16;
    if (drain == 0) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    drain = battle_50pct_variance(drain);
    battle_reduce_pp(target, drain);
    display_text_wait_addr(MSG_BTL4_RESULT_PP_LOST, drain);
}

/*
 * BTLACT_MAGNET_A (asm/battle/actions/magnet_alpha.asm)
 *
 * PP drain attack: drains 2-9 PP from target and adds it to attacker.
 * If target has 0 PP, shows "no PP" message. Drain amount is
 * rand_limit(4) + rand_limit(4) + 2, clamped to target's current PP.
 */
void btlact_magnet_a(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->pp_target == 0) {
        display_in_battle_text_addr(MSG_BTL6_TARGET_HAS_NO_PP);
        return;
    }
    /* Assembly lines 15-28: drain = rand(4) + rand(4) + 2 → range [2..9] */
    uint16_t drain = rand_limit(4) + rand_limit(4) + 2;
    /* Clamp to target's actual PP */
    if (target->pp_target < drain)
        drain = target->pp_target;
    display_text_wait_addr(MSG_BTL4_RESULT_PP_DRAINED_FROM, drain);
    battle_reduce_pp(target, drain);
    /* Add drained PP to attacker */
    Battler *attacker = battler_from_offset(bt.current_attacker);
    uint16_t new_pp = attacker->pp_target + drain;
    battle_set_pp(attacker, new_pp);
}

/*
 * BTLACT_MAGNET_O (asm/battle/actions/magnet_omega.asm)
 *
 * Same as Magnet Alpha, but skips if target is an ally and is Jeff
 * (Jeff has no PP to drain).
 */
void btlact_magnet_o(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if ((target->ally_or_enemy & 0xFF) == 0 && target->id == PARTY_MEMBER_JEFF)
        return;
    btlact_magnet_a();
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
void btlact_handbag_strap(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!battle_success_speed(250)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    int16_t damage = HANDBAG_STRAP_BASE_DAMAGE - (int16_t)target->defense;
    if (damage <= 0) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    battle_calc_resist_damage((uint16_t)damage, 0xFF);
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_SOLIDIFIED);
    }
}

/*
 * BTLACT_MUMMY_WRAP (asm/battle/actions/mummy_wrap.asm)
 *
 * Same as handbag_strap but with 400 base damage.
 */
void btlact_mummy_wrap(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!battle_success_speed(250)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    int16_t damage = MUMMY_WRAP_BASE_DAMAGE - (int16_t)target->defense;
    if (damage <= 0) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return;
    }
    battle_calc_resist_damage((uint16_t)damage, 0xFF);
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_SOLIDIFIED);
    }
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
void btlact_fly_honey(void) {
    for (uint16_t i = FIRST_ENEMY_INDEX; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if (b->consciousness == 0)
            continue;
        if (b->ally_or_enemy != 1)
            continue;
        if (b->id == ENEMY_MASTER_BELCH_1 || b->id == ENEMY_MASTER_BELCH_3) {
            b->id = ENEMY_MASTER_BELCH_2;
            display_in_battle_text_addr(MSG_BTL6_FLY_HONEY_BELCH_GRABS); /* fly honey worked! */
            return;
        }
    }
    display_in_battle_text_addr(MSG_BTL6_FLY_HONEY_BELCH_IGNORED); /* no Master Belch found */
}

/* ======================================================================
 * PSI Flash
 * ====================================================================== */

/*
 * FLASH_IMMUNITY_TEST (asm/battle/actions/psi_flash_immunity_test.asm)
 *
 * Check if target can be affected by PSI Flash.
 * Tests PSI shield nullification first, then flash_resist.
 * Returns 1 if target is vulnerable, 0 if immune/nullified.
 */
uint16_t flash_immunity_test(void) {
    if (battle_psi_shield_nullify())
        return 0;
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->flash_resist)) {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
        return 0;
    }
    return 1;
}

/*
 * BTLACT_PSI_FLASH_A (asm/battle/actions/psi_flash_alpha.asm)
 *
 * PSI Flash α: 1/8 chance of "feeling strange", 7/8 chance of crying.
 * Fails on NPCs.
 */
void btlact_psi_flash_alpha(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!flash_immunity_test())
        goto weaken;
    uint16_t roll = rand_byte() & 0x07;
    if (roll == 0) {
        flash_inflict_feeling_strange();
    } else {
        flash_inflict_crying();
    }
weaken:
    battle_weaken_shield();
}

/*
 * BTLACT_PSI_FLASH_B (asm/battle/actions/psi_flash_beta.asm)
 *
 * PSI Flash β: 1/8 KO, 1/8 paralysis, 1/8 strange, 5/8 crying.
 */
void btlact_psi_flash_beta(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!flash_immunity_test())
        goto weaken;
    uint16_t roll = rand_byte() & 0x07;
    if (roll == 0) {
        battle_ko_target(battler_from_offset(bt.current_target));
    } else if (roll == 1) {
        flash_inflict_paralysis();
    } else if (roll == 2) {
        flash_inflict_feeling_strange();
    } else {
        flash_inflict_crying();
    }
weaken:
    battle_weaken_shield();
}

/*
 * BTLACT_PSI_FLASH_G (asm/battle/actions/psi_flash_gamma.asm)
 *
 * PSI Flash γ: 2/8 KO, 1/8 paralysis, 1/8 strange, 4/8 crying.
 */
void btlact_psi_flash_gamma(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!flash_immunity_test())
        goto weaken;
    uint16_t roll = rand_byte() & 0x07;
    if (roll <= 1) {
        battle_ko_target(battler_from_offset(bt.current_target));
    } else if (roll == 2) {
        flash_inflict_paralysis();
    } else if (roll == 3) {
        flash_inflict_feeling_strange();
    } else {
        flash_inflict_crying();
    }
weaken:
    battle_weaken_shield();
}

/*
 * BTLACT_PSI_FLASH_O (asm/battle/actions/psi_flash_omega.asm)
 *
 * PSI Flash Ω: 3/8 KO, 1/8 paralysis, 1/8 strange, 3/8 crying.
 */
void btlact_psi_flash_omega(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!flash_immunity_test())
        goto weaken;
    uint16_t roll = rand_byte() & 0x07;
    if (roll <= 2) {
        battle_ko_target(battler_from_offset(bt.current_target));
    } else if (roll == 3) {
        flash_inflict_paralysis();
    } else if (roll == 4) {
        flash_inflict_feeling_strange();
    } else {
        flash_inflict_crying();
    }
weaken:
    battle_weaken_shield();
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

void btlact_crying(void) { btlact_pump_addr(0xC28C69); }

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

void btlact_crying2(void) { btlact_pump_addr(0xC28DFC); }

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

void btlact_solidify(void) { btlact_pump_addr(0xC28CF1); }

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

void btlact_solidify_2(void) { btlact_pump_addr(0xC2A82A); }

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

void btlact_mushroomize(void) { btlact_pump_addr(0xC28BBE); }

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

void btlact_paralysis_alpha(void) { btlact_pump_addr(0xC29FFE); }

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

void btlact_hypnosis_alpha(void) { btlact_pump_addr(0xC29F06); }

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

void btlact_brainshock_alpha(void) { btlact_pump_addr(0xC2A056); }


/* ======================================================================
 * Stat modification battle actions
 *
 * All are "decide + mutate, then one tail text" shapes: a per-action
 * decide fills a BattleTailText at pc 0 (the NPC-fail and luck-fail paths
 * return the plain "did not work" text, has_cnum = false; the success
 * paths return the stat message with the change amount as cnum, matching
 * display_text_wait_addr), then the shared single-text stepper runs it.
 * reduce_offense_defense is the one two-text exception (own pc machine).
 * ====================================================================== */

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

void btlact_offense_up_alpha(void) { btlact_pump_addr(0xC29E38); }

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

void btlact_defense_down_alpha(void) { btlact_pump_addr(0xC29E86); }

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

void btlact_speed_up_1d4(void)    { btlact_pump_addr(0xC2A193); }
void btlact_guts_up_1d4(void)     { btlact_pump_addr(0xC2A14B); }
void btlact_vitality_up_1d4(void) { btlact_pump_addr(0xC2A1DB); }
void btlact_iq_up_1d4(void)       { btlact_pump_addr(0xC2A0FF); }
void btlact_luck_up_1d4(void)     { btlact_pump_addr(0xC2A227); }

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

void btlact_random_stat_up_1d4(void) { btlact_pump_addr(0xC2A27F); }

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

void btlact_reduce_offense(void) { btlact_pump_addr(0xC29254); }

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

void btlact_reduce_offense_defense(void) { btlact_pump_addr(0xC28F21); }

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

void btlact_sudden_guts_pill(void) { btlact_pump_addr(0xC2AA7F); }

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

void btlact_defense_spray(void)  { btlact_pump_addr(0xC2AAC6); }
void btlact_defense_shower(void) { btlact_pump_addr(0xC2AB0D); }

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

void btlact_cut_guts(void) { btlact_pump_addr(0xC28EAE); }

/* ======================================================================
 * Prayer sub-actions (called from BTLACT_PRAY dispatch)
 * ====================================================================== */

/*
 * BTLACT_PRAY_SUBTLE (asm/battle/actions/pray_subtle.asm)
 *
 * Recover HP = max_hp / 16 for target.
 */
void btlact_pray_subtle(void) {
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t heal = target->hp_max >> 4;
    battle_recover_hp(target, heal);
}

/*
 * BTLACT_PRAY_WARM (asm/battle/actions/pray_warm.asm)
 *
 * Recover HP = max_hp / 8 for target.
 */
void btlact_pray_warm(void) {
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t heal = target->hp_max >> 3;
    battle_recover_hp(target, heal);
}

/*
 * BTLACT_PRAY_MYSTERIOUS (asm/battle/actions/pray_mysterious.asm)
 *
 * Recover PP = 50% variance of 5 (at least 1) for target.
 */
void btlact_pray_mysterious(void) {
    uint16_t amount = battle_50pct_variance(5);
    if (amount == 0)
        amount = 1;
    battle_recover_pp(battler_from_offset(bt.current_target), amount);
}

/*
 * BTLACT_PRAY_GOLDEN (asm/battle/actions/pray_golden.asm)
 *
 * Recover HP = target's max_hp - attacker's hp_target for target.
 * The attacker sacrifices their remaining HP as healing.
 */
void btlact_pray_golden(void) {
    Battler *target = battler_from_offset(bt.current_target);
    Battler *attacker = battler_from_offset(bt.current_attacker);
    uint16_t heal = target->hp_max - attacker->hp_target;
    battle_recover_hp(target, heal);
}

/*
 * BTLACT_PRAY_AROMA (asm/battle/actions/pray_aroma.asm)
 *
 * Inflict sleep on target. Fails on NPCs.
 */
void btlact_pray_aroma(void) {
    if (battle_fail_attack_on_npcs())
        return;
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_TEMPORARY, STATUS_2_ASLEEP);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_ASLEEP);
    } else {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
    }
}

/*
 * BTLACT_PRAY_RAINBOW (asm/battle/actions/pray_rainbow.asm)
 *
 * If target is unconscious, revive with full HP.
 */
void btlact_pray_rainbow(void) {
    Battler *target = battler_from_offset(bt.current_target);
    if (target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS) {
        battle_revive_target(target, target->hp_max);
    }
}

/*
 * BTLACT_PRAY_RENDING_SOUND (asm/battle/actions/pray_rending_sound.asm)
 *
 * Inflict strangeness on target. Fails on NPCs.
 */
void btlact_pray_rending_sound(void) {
    if (battle_fail_attack_on_npcs())
        return;
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_STRANGENESS, STATUS_3_STRANGE);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL5_STATUS_STRANGE);
    } else {
        display_in_battle_text_addr(MSG_BTL4_RESULT_DID_NOT_WORK);
    }
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
 */
void btlact_pray(void) {
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

    /* Sub-action function for each prayer type */
    static const battle_action_fn prayer_actions[10] = {
        btlact_pray_subtle,         /* 0 */
        btlact_pray_warm,           /* 1 */
        btlact_pray_mysterious,     /* 2 */
        btlact_pray_golden,         /* 3 */
        btlact_psi_rockin_beta,     /* 4: reuses PSI Rockin β */
        btlact_psi_flash_alpha,     /* 5: reuses PSI Flash α */
        btlact_pray_rainbow,        /* 6 */
        btlact_pray_aroma,          /* 7 */
        btlact_pray_rending_sound,  /* 8 */
        btlact_defense_down_alpha,  /* 9: reuses Defense Down α */
    };

    /* Pick random prayer type */
    uint16_t index = rand_limit(16);
    uint16_t prayer_type = prayer_list[index];

    /* Display prayer text */
    display_in_battle_text_addr(prayer_text_addrs[prayer_type]);

    /* Set up targeting based on prayer type */
    battle_action_fn action = NULL;
    if (prayer_type <= 9)
        action = prayer_actions[prayer_type];

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
    apply_action_to_targets(action);
    bt.battler_target_flags = 0;
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
 */
void btlact_switch_weapons(void) {
    Battler *attacker = battler_from_offset(bt.current_attacker);
    uint16_t char_id = attacker->id;

    dt.blinking_triangle_flag = 1;

    /* Check if the character can use this item */
    uint16_t item_slot_arg = attacker->current_action_argument;
    if (!check_item_usable_by(char_id, item_slot_arg)) {
        display_text_from_addr(MSG_GOODS4_EQUIP_WEAPON_FAIL_OLD_WEAPON);
        goto dispatch;
    }

    /* Get pointer to character struct */
    CharStruct *ch = &party_characters[char_id - 1];

    /* Save the offense bonus: current offense minus base offense from equipment */
    int16_t offense_bonus = attacker->offense - (uint16_t)attacker->base_offense;
    /* Save the guts bonus */
    int16_t guts_bonus = attacker->guts - (uint16_t)attacker->base_guts;

    /* Equip the new weapon (action_item_slot is the inventory slot) */
    equip_item(char_id, (uint16_t)attacker->action_item_slot);

    /* Update battler base stats from char_struct and reapply bonuses */
    attacker->base_offense = ch->offense;
    attacker->offense = (uint16_t)attacker->base_offense + offense_bonus;

    attacker->base_guts = ch->guts;
    attacker->guts = (uint16_t)attacker->base_guts + guts_bonus;

    display_text_from_addr(MSG_GOODS4_EQUIP_ITEM_SUCCESS);

dispatch:;
    /* Check if the (now-equipped) weapon is a projectile type */
    CharStruct *ch2 = &party_characters[char_id - 1];
    uint8_t weapon_slot = ch2->equipment[EQUIP_WEAPON];
    if (weapon_slot != 0) {
        uint8_t weapon_item_id = ch2->items[weapon_slot - 1];
        if (weapon_item_id != 0) {
            const ItemConfig *entry = get_item_entry(weapon_item_id);
            if (entry && (entry->type & 0x03) == 1) {
                /* Projectile weapon — dispatch to action 5 (shoot) */
                if (battle_action_table) {
                    display_text_from_addr(battle_action_table[5].description_text_pointer);
                    bt.temp_function_pointer = battle_action_table[5].battle_function_pointer;
                    jump_temp_function_pointer();
                }
                dt.blinking_triangle_flag = 0;
                return;
            }
        }
    }

    /* Normal weapon — dispatch to action 4 (bash) */
    if (battle_action_table) {
        display_text_from_addr(battle_action_table[4].description_text_pointer);
        bt.temp_function_pointer = battle_action_table[4].battle_function_pointer;
        jump_temp_function_pointer();
    }
    dt.blinking_triangle_flag = 0;
}

/*
 * BTLACT_SWITCH_ARMOR (asm/battle/actions/switch_armor.asm)
 *
 * Equips new armor during battle.  Saves defense/speed/luck bonuses,
 * equips the item, reapplies bonuses with new base stats, then
 * recalculates all elemental and status resistances from char_struct.
 */
void btlact_switch_armor(void) {
    Battler *attacker = battler_from_offset(bt.current_attacker);
    uint16_t char_id = attacker->id;

    dt.blinking_triangle_flag = 1;

    /* Check if the character can use this item */
    uint16_t item_slot_arg = attacker->current_action_argument;
    if (!check_item_usable_by(char_id, item_slot_arg)) {
        display_text_from_addr(MSG_GOODS4_EQUIP_WEAPON_FAIL_OLD_WEAPON);
        dt.blinking_triangle_flag = 0;
        return;
    }

    /* Get pointer to character struct (use row for 0-indexed lookup) */
    CharStruct *ch = &party_characters[attacker->row];

    /* Save bonuses: current stat minus base (from equipment) */
    int16_t defense_bonus = attacker->defense - (uint16_t)attacker->base_defense;
    int16_t speed_bonus = attacker->speed - (uint16_t)attacker->base_speed;
    int16_t luck_bonus = attacker->luck - (uint16_t)attacker->base_luck;

    /* Equip the new armor */
    equip_item(char_id, (uint16_t)attacker->action_item_slot);

    display_text_from_addr(MSG_GOODS4_EQUIP_ITEM_SUCCESS);

    /* Update battler base stats from char_struct and reapply bonuses */
    attacker->base_defense = ch->defense;
    attacker->defense = (uint16_t)attacker->base_defense + defense_bonus;

    attacker->base_speed = ch->speed;
    attacker->speed = (uint16_t)attacker->base_speed + speed_bonus;

    attacker->base_luck = ch->luck;
    attacker->luck = (uint16_t)attacker->base_luck + luck_bonus;

    /* Recalculate all elemental/status resistances from char_struct */
    attacker->fire_resist = battle_calc_psi_dmg_modifier(ch->fire_resist);
    attacker->freeze_resist = battle_calc_psi_dmg_modifier(ch->freeze_resist);
    attacker->flash_resist = battle_calc_psi_res_modifier(ch->flash_resist);
    attacker->paralysis_resist = battle_calc_psi_res_modifier(ch->paralysis_resist);
    attacker->hypnosis_resist = battle_calc_psi_res_modifier(ch->hypnosis_brainshock_resist);
    /* brainshock = 3 - hypnosis_brainshock_resist (inverted) */
    uint8_t brainshock_base = 3 - ch->hypnosis_brainshock_resist;
    attacker->brainshock_resist = battle_calc_psi_res_modifier(brainshock_base);

    dt.blinking_triangle_flag = 0;
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
void btlact_clumsydeath(void) {
    /* Load the PSI teleport destination table to read entry 13's event flag */
    const uint8_t *table = ASSET_DATA(ASSET_DATA_PSI_TELEPORT_DEST_TABLE_BIN);

    /* Entry 13: each entry is 31 bytes, event_flag at byte offset 25
     * (struct ow.psi_teleport_destination: name[25] + event_flag[2] + x[2] + y[2]) */
    uint16_t event_flag = 0;
    if (table) {
        const uint8_t *entry = table + 13 * 31;
        event_flag = read_u16_le(entry + 25);
    }

    if (event_flag_get(event_flag)) {
        display_in_battle_text_addr(MSG_BTL4_RUNAWAY5_RESCUE);
        ow.psi_teleport_style = 3;  /* TELEPORT_STYLE::INSTANT */
        ow.psi_teleport_destination = 15;
    } else {
        display_in_battle_text_addr(MSG_BTL4_ENEMY_ESCAPE_SMOKE_FAIL);
        ow.psi_teleport_style = 3;  /* TELEPORT_STYLE::INSTANT */
        ow.psi_teleport_destination = 13;
        bt.special_defeat = 1;
    }
}


/* ======================================================================
 * BTLACT_MASTERBARFDEATH (asm/battle/actions/master_barf_death.asm)
 *
 * Special boss action: when Master Barf is defeated, Poo joins the party
 * mid-battle and performs a Starstorm Alpha attack on all enemies.
 * ====================================================================== */
void btlact_masterbarfdeath(void) {
    uint16_t saved_attacker = bt.current_attacker;
    uint16_t saved_target = bt.current_target;

    /* Hide HP/PP windows, add Poo to party */
    hide_hppp_windows();
    add_char_to_party(PARTY_MEMBER_POO);

    /* Find first empty battler slot for Poo */
    uint16_t poo_offset = 0;
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        if (bt.battlers_table[i].consciousness == 0) {
            poo_offset = i * sizeof(Battler);
            battle_init_player_stats(PARTY_MEMBER_POO, &bt.battlers_table[i]);
            bt.current_attacker = poo_offset;
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

    /* Display Poo's entrance text */
    display_text_with_prompt_addr(MSG_BTL4_POO_USES_STARSTORM);

    /* Set up Starstorm Alpha attack */
    fix_attacker_name(0);
    set_current_item(21);  /* PSI::STARSTORM_ALPHA */

    /* Display Starstorm Alpha description text (action 30 in battle_action_table) */
    if (battle_action_table != NULL) {
        uint32_t desc_addr = battle_action_table[30].description_text_pointer;
        if (desc_addr != 0) {
            display_in_battle_text_addr(desc_addr);
        }
    }

    /* Deal Starstorm Alpha damage to all conscious enemies */
    for (uint16_t i = 0; i < BATTLER_COUNT; i++) {
        if (bt.battlers_table[i].consciousness == 0)
            continue;
        if ((bt.battlers_table[i].ally_or_enemy & 0xFF) != 1)
            continue;
        bt.current_target = i * sizeof(Battler);
        fix_target_name();
        uint16_t damage = battle_25pct_variance(STARSTORM_ALPHA_DAMAGE);
        battle_calc_damage(bt.current_target, damage);
    }

    /* Restore original attacker and target */
    bt.current_attacker = saved_attacker;
    bt.current_target = saved_target;
    fix_attacker_name(0);
    fix_target_name();
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
 * BTLACT_POKEY_SPEECH (asm/battle/actions/pokey_speech_1.asm)
 *
 * Giygas phase transition: Pokey's first speech. Sets DEVILS_MACHINE_OFF,
 * replaces boss with GIYGAS_3, loads phase 1 scene, shows text,
 * kills slot 9, transitions to GIYGAS_STARTS_ATTACKING phase,
 * replaces with GIYGAS_4, loads phase 2 scene.
 * ====================================================================== */
void btlact_pokey_speech(void) {
    bt.giygas_phase = GIYGAS_DEVILS_MACHINE_OFF;
    replace_boss_battler(ENEMY_GIYGAS_3);
    load_battle_scene(ENEMY_GROUP_BOSS_GIYGAS_PHASE_1, MUSIC_GIYGAS_PHASE1);
    display_text_with_prompt_addr(MSG_BTL6_MECH_POKEY_SPEECH_1B);
    /* Kill slot 9 (Pokey's mech) */
    bt.battlers_table[9].consciousness = 0;
    bt.giygas_phase = GIYGAS_STARTS_ATTACKING;
    /* FINAL_BATTLE_ANTIPIRACY_CHECK: intentional no-op.
     * Assembly checksums hardware registers and wipes SRAM on failure.
     * Always passes for legitimate ROM; not applicable to C port. */
    replace_boss_battler(ENEMY_GIYGAS_4);
    load_battle_scene(ENEMY_GROUP_BOSS_GIYGAS_PHASE_2, MUSIC_GIYGAS_PHASE2);
    bt.skip_death_text_and_cleanup = 1;
}

/* ======================================================================
 * BTLACT_POKEY_SPEECH_2 (asm/battle/actions/pokey_speech_2.asm)
 *
 * Giygas phase transition: Pokey's second speech. Sets START_PRAYING phase,
 * shows/hides slot 9, displays text, replaces boss with GIYGAS_5.
 * ====================================================================== */
void btlact_pokey_speech_2(void) {
    bt.giygas_phase = GIYGAS_START_PRAYING;
    battle_wait(2 * FRAMES_PER_SECOND);  /* 2 seconds */
    /* Show slot 9 consciousness */
    bt.battlers_table[9].consciousness = 1;
    render_all_battle_sprites();
    display_text_with_prompt_addr(MSG_BTL6_MECH_POKEY_SPEECH_2);
    /* Hide slot 9 */
    bt.battlers_table[9].consciousness = 0;
    render_all_battle_sprites();
    battle_wait(FRAMES_PER_SECOND);  /* 1 second */
    replace_boss_battler(ENEMY_GIYGAS_5);
    load_battle_scene(ENEMY_GROUP_BOSS_GIYGAS_DURING_PRAYER_1, MUSIC_GIYGAS_PHASE3);
    bt.skip_death_text_and_cleanup = 1;
}

/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_1 (asm/battle/actions/giygas_prayer_1.asm)
 *
 * First prayer: plays cutscene text, SFX, screen shake, damages Giygas,
 * replaces boss, loads after-prayer scene.
 * ====================================================================== */
void btlact_giygas_prayer_1(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_DURING_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT4_PAULA_PRAYER_MR_SATURN_RESPONDS);
    battle_wait(2 * FRAMES_PER_SECOND);
    play_sfx(SFX_PSI_STARSTORM);
    battle_wait(30);  /* HALF_OF_A_SECOND */
    bt.vertical_shake_duration = FRAMES_PER_SECOND;
    bt.vertical_shake_hold_duration = 12;  /* FIFTH_OF_A_SECOND */
    display_text_with_prompt_addr(MSG_BTL7_GIYGAS_DEFENSES_UNSTABLE);
    bt.giygas_phase = GIYGAS_PRAYER_1_USED;
    replace_boss_battler(ENEMY_GIYGAS_6);
    load_battle_scene(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1, MUSIC_NONE);
}

/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_2..6 (asm/battle/actions/giygas_prayer_2..6.asm)
 *
 * Prayers 2-6: show cutscene text, deal escalating prayer damage.
 * ====================================================================== */
void btlact_giygas_prayer_2(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT4_PAULA_PRAYER_RUNAWAY_FIVE);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_1);
    bt.giygas_phase = GIYGAS_PRAYER_2_USED;
}

void btlact_giygas_prayer_3(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT4_PAULA_PRAYER_PAULAS_FAMILY);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_2);
    bt.giygas_phase = GIYGAS_PRAYER_3_USED;
}

void btlact_giygas_prayer_4(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT4_PAULA_PRAYER_TONY_AND_CLASS);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_3);
    bt.giygas_phase = GIYGAS_PRAYER_4_USED;
}

void btlact_giygas_prayer_5(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT4_PAULA_PRAYER_DALAAM_MASTER);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_4);
    bt.giygas_phase = GIYGAS_PRAYER_5_USED;
}

void btlact_giygas_prayer_6(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT4_PAULA_PRAYER_FRANK_RESPONDS);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_5);
    bt.giygas_phase = GIYGAS_PRAYER_6_USED;
}

/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_7 (asm/battle/actions/giygas_prayer_7.asm)
 *
 * Prayer 7 (Ness's Mom): cutscene text, damage, reload scene with
 * AFTER_PRAYER_7 group and weakened music.
 * ====================================================================== */
void btlact_giygas_prayer_7(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT4_PAULA_PRAYER_NES_MOM_RESPONDS);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_6);
    bt.giygas_phase = GIYGAS_PRAYER_7_USED;
    load_battle_scene(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_7, MUSIC_GIYGAS_WEAKENED2);
}

/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_8 (asm/battle/actions/giygas_prayer_8.asm)
 *
 * Prayer 8: uses weakened sequence instead of cutscene text.
 * ====================================================================== */
void btlact_giygas_prayer_8(void) {
    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL7_PRAY_ABSORBED_BY_DARKNESS);
    bt.giygas_phase = GIYGAS_PRAYER_8_USED;
}

/* Music/SFX constants for Giygas prayer 9 (from include/constants/) */
#define MUSIC_GIYGAS_DEATH    190
#define MUSIC_GIYGAS_DEATH2    75
#define MUSIC_GIYGAS_STATIC   182
#define SFX_DOOR_OPEN           8
#define SFX_DOOR_CLOSE          9
#define SFX_RECOVER_HP         36
#define SFX_PSI_THUNDER_DAMAGE 63

/* ======================================================================
 * BTLACT_GIYGAS_PRAYER_9 (asm/battle/actions/giygas_prayer_9.asm)
 *
 * The final prayer sequence. Deals remaining damage to Giygas, plays
 * the death sequence with static noise transitions, battle swirl,
 * and transition to the final post-Giygas scene.
 * ====================================================================== */
void btlact_giygas_prayer_9(void) {
    /* Reset HP/PP rolling counters */
    reset_hppp_rolling();

    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL7_PRAY_RESPONSE_STRANGER);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_7);

    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL7_PRAY_KEPT_PRAYING_1);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_8);

    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL7_PRAY_KEPT_PRAYING_2);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_9);

    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL7_PRAY_KEPT_PRAYING_3);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_10);

    /* Close windows and hide HP/PP */
    redirect_close_focus_window();
    bt.battle_mode_flag = 0;
    hide_hppp_windows();
    bt.battle_mode_flag = 1;
    window_tick();

    /* Giygas defeated */
    bt.giygas_phase = GIYGAS_DEFEATED;
    change_music(MUSIC_GIYGAS_DEATH);

    /* Play prayer noise sequence from ROM table */
    {
        size_t noise_size = ASSET_SIZE(ASSET_DATA_FINAL_GIYGAS_PRAYER_NOISE_TABLE_BIN);
        const uint8_t *noise_table = ASSET_DATA(ASSET_DATA_FINAL_GIYGAS_PRAYER_NOISE_TABLE_BIN);
        if (noise_table) {
            uint16_t idx = 0;
            while (idx + 1 < noise_size) {
                uint8_t sfx_id = noise_table[idx];
                uint8_t delay = noise_table[idx + 1];
                idx += 2;
                play_sfx(sfx_id);
                if (delay == 0) break;
                battle_wait(delay);
            }
        }
    }

    /* Switch to Giygas death music phase 2 */
    change_music(MUSIC_GIYGAS_DEATH2);
    bt.giygas_phase = 0;
    battle_wait(8 * FRAMES_PER_SECOND);

    /* Briefly show Pokey (battler slot 9), display his text, then hide */
    bt.battlers_table[9].consciousness = 1;
    render_all_battle_sprites();
    display_in_battle_text_addr(MSG_BTL6_POKEY_ESCAPES);
    bt.battlers_table[9].consciousness = 0;
    render_all_battle_sprites();
    battle_wait(FRAMES_PER_SECOND);

    /* Static transition: alternate distortion + APU port 2 toggle */
    {
        size_t delays_size = ASSET_SIZE(ASSET_DATA_GIYGAS_DEATH_STATIC_TRANSITION_DELAYS_BIN);
        const uint8_t *delays_data = ASSET_DATA(
            ASSET_DATA_GIYGAS_DEATH_STATIC_TRANSITION_DELAYS_BIN);
        uint16_t apu_toggle = 2;
        uint16_t shake_countdown = 45;
        uint16_t shake_repeats = 2;

        bt.vertical_shake_duration = FRAMES_PER_SECOND;

        if (delays_data) {
            uint16_t step = 0;
            for (;;) {
                /* Read the delay for this step */
                if (step * 2 + 1 >= delays_size) break;
                uint16_t target_frames = read_u16_le(&delays_data[step * 2]);
                if (target_frames == 0) break;

                /* Wait for target_frames, ticking each frame */
                for (uint16_t f = 0; f < target_frames; f++) {
                    window_tick();
                    /* Decrement vertical shake and restart if repeats remain */
                    if (shake_repeats > 0) {
                        shake_countdown--;
                        if (shake_countdown == 0) {
                            shake_repeats--;
                            shake_countdown = 45;
                            bt.vertical_shake_duration = FRAMES_PER_SECOND;
                        }
                    }
                }

                /* Rotate distortion and toggle APU static */
                rotate_bg_distortion();
                write_apu_port2(apu_toggle);
                apu_toggle = (apu_toggle == 2) ? 1 : 2;
                step++;
            }
        }
    }

    /* Play static noise music */
    change_music(MUSIC_GIYGAS_STATIC);
    battle_wait(10 * FRAMES_PER_SECOND);

    /* Final swirl and scene transition */
    play_sfx(SFX_PSI_THUNDER_DAMAGE);
    stop_music();
    start_battle_swirl(5, 0, 0);
    while (is_battle_swirl_active()) {
        window_tick();
    }

    stop_music();
    load_battle_scene(ENEMY_GROUP_BOSS_GIYGAS_PHASE_FINAL, MUSIC_NONE);
    battle_wait(8 * FRAMES_PER_SECOND);

    /* Signal special defeat */
    bt.special_defeat = 3;
}


static const BattleActionEntry btlact_dispatch_table[] = {
    /* Sorted by ROM address for binary search */
    { 0xC1DE43, btlact_switch_weapons, NULL },
    { 0xC1E00F, btlact_switch_armor, NULL },
    { 0xC28523, (void(*)(void))battle_level_2_attack, NULL },
    { 0xC2859F, btlact_bash, btlact_bash_step },
    { 0xC285DA, (void(*)(void))battle_level_4_attack, btlact_level_4_attack_step },
    { 0xC28651, (void(*)(void))battle_level_3_attack, btlact_level_3_attack_step },
    { 0xC286CB, btlact_level_1_attack, btlact_level_1_attack_step },
    { 0xC28740, btlact_shoot, btlact_shoot_step },
    { 0xC28770, btlact_spy, btlact_spy_step },
    { 0xC2889B, btlact_null, NULL },
    { 0xC2889E, btlact_steal, NULL },
    { 0xC288EB, btlact_freezetime, NULL },
    { 0xC289CE, btlact_diamondize, btlact_diamondize_step },
    { 0xC28A92, btlact_paralyze, NULL },
    { 0xC28AEB, btlact_nauseate, btlact_nauseate_step },
    { 0xC28B2C, btlact_poison, btlact_poison_step },
    { 0xC28B6D, btlact_cold, NULL },
    { 0xC28BBE, btlact_mushroomize, btlact_mushroomize_step },
    { 0xC28BFD, btlact_possess, NULL },
    { 0xC28C69, btlact_crying, btlact_crying_step },
    { 0xC28CB8, btlact_immobilize, btlact_immobilize_step },
    { 0xC28CF1, btlact_solidify, btlact_solidify_step },
    { 0xC28D3A, redirect_btlact_brainshock_alpha, btlact_brainshock_alpha_step },
    { 0xC28D5A, btlact_distract, NULL },
    { 0xC28DBB, btlact_feel_strange, btlact_feel_strange_step },
    { 0xC28DFC, btlact_crying2, btlact_crying2_step },
    { 0xC28E3B, redirect_btlact_hypnosis_alpha, btlact_hypnosis_alpha_step },
    { 0xC28E42, btlact_reduce_pp, NULL },
    { 0xC28EAE, btlact_cut_guts, btlact_cut_guts_step },
    { 0xC28F21, btlact_reduce_offense_defense, btlact_reduce_offense_defense_step },
    { 0xC28F97, btlact_level_2_attack_poison, btlact_level_2_attack_poison_step },
    { 0xC28FF9, btlact_double_bash, NULL },
    { 0xC2900B, btlact_350_fire_damage, btlact_350_fire_damage_step },
    { 0xC2902C, (void(*)(void))battle_level_3_attack, btlact_level_3_attack_step },  /* REDIRECT_BTLACT_LEVEL_3_ATK */
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
    { 0xC29051, btlact_neutralize, NULL },
    { 0xC290C6, apply_neutralize_to_all, NULL },
    { 0xC2916E, btlact_level_2_attack_diamondize, btlact_level_2_attack_diamondize_step },
    { 0xC29254, btlact_reduce_offense, btlact_reduce_offense_step },
    { 0xC29298, btlact_clumsydeath, NULL },
    { 0xC292EB, btlact_enemy_extend, NULL },
    { 0xC292EE, btlact_masterbarfdeath, NULL },
    { 0xC29556, btlact_psi_rockin_alpha, btlact_psi_rockin_alpha_step },
    { 0xC2955F, btlact_psi_rockin_beta, btlact_psi_rockin_beta_step },
    { 0xC29568, btlact_psi_rockin_gamma, btlact_psi_rockin_gamma_step },
    { 0xC29571, btlact_psi_rockin_omega, btlact_psi_rockin_omega_step },
    { 0xC295AB, btlact_psi_fire_alpha, btlact_psi_fire_alpha_step },
    { 0xC295B4, btlact_psi_fire_beta, btlact_psi_fire_beta_step },
    { 0xC295BD, btlact_psi_fire_gamma, btlact_psi_fire_gamma_step },
    { 0xC295C6, btlact_psi_fire_omega, btlact_psi_fire_omega_step },
    { 0xC29647, btlact_psi_freeze_alpha, btlact_psi_freeze_alpha_step },
    { 0xC29650, btlact_psi_freeze_beta, btlact_psi_freeze_beta_step },
    { 0xC29659, btlact_psi_freeze_gamma, btlact_psi_freeze_gamma_step },
    { 0xC29662, btlact_psi_freeze_omega, btlact_psi_freeze_omega_step },
    { 0xC29871, btlact_psi_thunder_alpha, NULL },
    { 0xC2987D, btlact_psi_thunder_beta, NULL },
    { 0xC29889, btlact_psi_thunder_gamma, NULL },
    { 0xC29895, btlact_psi_thunder_omega, NULL },
    { 0xC29987, btlact_psi_flash_alpha, NULL },
    { 0xC299AE, btlact_psi_flash_beta, NULL },
    { 0xC299EF, btlact_psi_flash_gamma, NULL },
    { 0xC29A35, btlact_psi_flash_omega, NULL },
    { 0xC29AA6, btlact_psi_starstorm_alpha, btlact_psi_starstorm_alpha_step },
    { 0xC29AAF, btlact_psi_starstorm_omega, btlact_psi_starstorm_omega_step },
    { 0xC29AC6, btlact_lifeup_alpha, btlact_lifeup_alpha_step },
    { 0xC29ACF, btlact_lifeup_beta, btlact_lifeup_beta_step },
    { 0xC29AD8, btlact_lifeup_gamma, btlact_lifeup_gamma_step },
    { 0xC29AE1, btlact_lifeup_omega, btlact_lifeup_omega_step },
    { 0xC29AEA, btlact_healing_alpha, btlact_healing_alpha_step },
    { 0xC29B7A, btlact_healing_beta, btlact_healing_beta_step },
    { 0xC29C2C, btlact_healing_gamma, btlact_healing_gamma_step },
    { 0xC29CB8, btlact_healing_omega, btlact_healing_omega_step },
    { 0xC29D44, btlact_shield_alpha, btlact_shield_alpha_step },
    { 0xC29D7A, redirect_btlact_shield_alpha, NULL },
    { 0xC29D81, btlact_shield_beta, btlact_shield_beta_step },
    { 0xC29DB7, redirect_btlact_shield_beta, NULL },
    { 0xC29DBE, btlact_psi_shield_alpha, btlact_psi_shield_alpha_step },
    { 0xC29DF4, redirect_btlact_psi_shield_alpha, NULL },
    { 0xC29DFB, btlact_psi_shield_beta, btlact_psi_shield_beta_step },
    { 0xC29E31, redirect_btlact_psi_shield_beta, NULL },
    { 0xC29E38, btlact_offense_up_alpha, btlact_offense_up_alpha_step },
    { 0xC29E7F, redirect_btlact_offense_up_alpha, btlact_offense_up_alpha_step },
    { 0xC29E86, btlact_defense_down_alpha, btlact_defense_down_alpha_step },
    { 0xC29EFF, redirect_btlact_defense_down_alpha, btlact_defense_down_alpha_step },
    { 0xC29F06, btlact_hypnosis_alpha, btlact_hypnosis_alpha_step },
    { 0xC29F57, redirect_btlact_hypnosis_a_copy, btlact_hypnosis_alpha_step },
    { 0xC29F5E, btlact_magnet_a, NULL },
    { 0xC29FE1, btlact_magnet_o, NULL },
    { 0xC29FFE, btlact_paralysis_alpha, btlact_paralysis_alpha_step },
    { 0xC2A04F, redirect_btlact_paralysis_alpha, btlact_paralysis_alpha_step },
    { 0xC2A056, btlact_brainshock_alpha, btlact_brainshock_alpha_step },
    { 0xC2A0A7, redirect_btlact_brainshock_a_copy, btlact_brainshock_alpha_step },
    { 0xC2A0AE, btlact_hp_recovery_1d4, btlact_hp_recovery_1d4_step },
    { 0xC2A0BF, btlact_hp_recovery_50, btlact_hp_recovery_50_step },
    { 0xC2A0CF, btlact_hp_recovery_200, btlact_hp_recovery_200_step },
    { 0xC2A0DF, btlact_pp_recovery_20, btlact_pp_recovery_20_step },
    { 0xC2A0EF, btlact_pp_recovery_80, btlact_pp_recovery_80_step },
    { 0xC2A0FF, btlact_iq_up_1d4, btlact_iq_up_1d4_step },
    { 0xC2A14B, btlact_guts_up_1d4, btlact_guts_up_1d4_step },
    { 0xC2A193, btlact_speed_up_1d4, btlact_speed_up_1d4_step },
    { 0xC2A1DB, btlact_vitality_up_1d4, btlact_vitality_up_1d4_step },
    { 0xC2A227, btlact_luck_up_1d4, btlact_luck_up_1d4_step },
    { 0xC2A26F, btlact_hp_recovery_300, btlact_hp_recovery_300_step },
    { 0xC2A27F, btlact_random_stat_up_1d4, btlact_random_stat_up_1d4_step },
    { 0xC2A360, btlact_hp_recovery_10, btlact_hp_recovery_10_step },
    { 0xC2A370, btlact_hp_recovery_100, btlact_hp_recovery_100_step },
    { 0xC2A380, btlact_hp_recovery_10000, btlact_hp_recovery_10000_step },
    { 0xC2A39D, btlact_heal_poison, NULL },
    { 0xC2A3D1, btlact_counter_psi, NULL },
    { 0xC2A422, btlact_shield_killer, NULL },
    { 0xC2A46B, (void(*)(void))btlact_hp_sucker, NULL },
    { 0xC2A507, btlact_hungry_hp_sucker, NULL },
    { 0xC2A50E, btlact_mummy_wrap, NULL },
    { 0xC2A5D1, btlact_bottle_rocket, btlact_bottle_rocket_step },
    { 0xC2A5DA, btlact_big_bottle_rocket, btlact_big_bottle_rocket_step },
    { 0xC2A5E3, btlact_multi_bottle_rocket, btlact_multi_bottle_rocket_step },
    { 0xC2A5EC, btlact_handbag_strap, NULL },
    { 0xC2A818, btlact_bomb, NULL },
    { 0xC2A821, btlact_super_bomb, NULL },
    { 0xC2A82A, btlact_solidify_2, btlact_solidify_2_step },
    { 0xC2A86B, btlact_yogurt_dispenser, NULL },
    { 0xC2A89D, btlact_snake, NULL },
    { 0xC2A902, btlact_inflict_solidification, NULL },
    { 0xC2A953, btlact_inflict_poison, NULL },
    { 0xC2A99C, btlact_bag_of_dragonite, NULL },
    { 0xC2AA0C, btlact_insecticide_spray, btlact_insecticide_spray_step },
    { 0xC2AA15, btlact_xterminator_spray, btlact_xterminator_spray_step },
    { 0xC2AA6D, btlact_rust_promoter, btlact_rust_promoter_step },
    { 0xC2AA76, btlact_rust_promoter_dx, btlact_rust_promoter_dx_step },
    { 0xC2AA7F, btlact_sudden_guts_pill, btlact_sudden_guts_pill_step },
    { 0xC2AAC6, btlact_defense_spray, btlact_defense_spray_step },
    { 0xC2AB0D, btlact_defense_shower, btlact_defense_spray_step },
    { 0xC2AB71, (void(*)(void))btlact_teleport_box, NULL },
    { 0xC2AC2A, btlact_pray_subtle, NULL },
    { 0xC2AC3E, btlact_pray_warm, NULL },
    { 0xC2AC51, btlact_pray_golden, NULL },
    { 0xC2AC68, btlact_pray_mysterious, NULL },
    { 0xC2AC7B, btlact_pray_rainbow, NULL },
    { 0xC2AC99, btlact_pray_aroma, NULL },
    { 0xC2ACDA, btlact_pray_rending_sound, NULL },
    { 0xC2AD1B, btlact_pray, NULL },
    { 0xC2B0A1, (void(*)(void))btlact_mirror, NULL },
    { 0xC2B27D, btlact_eat_food, NULL },
    { 0xC2C13C, btlact_sow_seeds, NULL },
    { 0xC2C145, btlact_call_for_help, NULL },
    { 0xC2C14E, (void(*)(void))btlact_rainbow_of_colours, NULL },
    { 0xC2C1BD, btlact_fly_honey, NULL },
    { 0xC2C4C0, btlact_pokey_speech, NULL },
    { 0xC2C513, btlact_null12, NULL },
    { 0xC2C516, btlact_pokey_speech_2, NULL },
    { 0xC2C572, btlact_giygas_prayer_1, NULL },
    { 0xC2C5D1, btlact_giygas_prayer_2, NULL },
    { 0xC2C5FA, btlact_giygas_prayer_3, NULL },
    { 0xC2C623, btlact_giygas_prayer_4, NULL },
    { 0xC2C64C, btlact_giygas_prayer_5, NULL },
    { 0xC2C675, btlact_giygas_prayer_6, NULL },
    { 0xC2C69E, btlact_giygas_prayer_7, NULL },
    { 0xC2C6D0, btlact_giygas_prayer_8, NULL },
    { 0xC2C6F0, btlact_giygas_prayer_9, NULL },
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

/* Blocking bridge: run a converted action's resumable stepper to completion.
 * Used by jump_temp_function_pointer (unconverted drivers / action→action
 * dispatch through the table) and by the converted actions' own btlact_*()
 * wrappers (direct C calls from other actions, e.g. healing_beta →
 * healing_alpha). Deleted at cutover with pump_mode. */
static void btlact_pump(uint16_t table_index) {
    ModeState init = {0};
    init.battle_action.table_index = table_index;
    pump_mode(GAME_MODE_BATTLE_ACTION, &init);
}

/* Bridge variant for the converted actions' blocking btlact_*() wrappers:
 * looks the action up by its own ROM address (the same constant as its table
 * row) WITHOUT touching bt.temp_function_pointer — a direct JSR in the
 * assembly does not rewrite it. */
static void btlact_pump_addr(uint32_t rom_addr) {
    int idx = btlact_find(rom_addr);
    if (idx < 0 || !btlact_dispatch_table[idx].step) {
        LOG_WARN("WARN: btlact_pump_addr($%06X): no resumable form\n", rom_addr);
        return;
    }
    btlact_pump((uint16_t)idx);
}

/*
 * JUMP_TEMP_FUNCTION_POINTER — Port of asm/overworld/jump_temp_function_pointer.asm.
 * Assembly: JML (TEMP_FUNCTION_POINTER) — indirect long jump through a
 * 24-bit ROM address stored in bt.temp_function_pointer. The C port
 * dispatches through btlact_dispatch_table instead; the ROM addresses come
 * from the battle_action_table asset (loaded from the donor ROM).
 */
void jump_temp_function_pointer(void) {
    int idx = btlact_find(bt.temp_function_pointer);
    if (idx < 0) {
        LOG_WARN("WARN: unknown battle action ROM addr $%06X\n", bt.temp_function_pointer);
        return;
    }
    if (btlact_dispatch_table[idx].step) {
        btlact_pump((uint16_t)idx);  /* converted: pump the resumable form */
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

