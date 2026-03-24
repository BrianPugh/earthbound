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
#include "include/binary.h"
#include "include/pad.h"
#include "snes/ppu.h"
#include "core/decomp.h"
#include "platform/platform.h"
#include "data/battle_text_data.h"
#include <stdio.h>
#include <string.h>

#include "game_main.h"


/* ======================================================================
 * Battle action handlers
 * ====================================================================== */

/*
 * BTLACT_BASH (asm/battle/actions/bash.asm)
 *
 * Standard melee attack: miss check → SMAAAASH check → dodge check →
 * level 2 attack → heal strangeness.
 */
void btlact_bash(void) {
    if (battle_miss_calc(0))
        return;
    if (battle_smaaaash())
        return;
    if (battle_determine_dodge()) {
        display_in_battle_text_addr(MSG_BTL_TATAKU_YOKETA); /* dodged */
        return;
    }
    battle_level_2_attack();
    battle_heal_strangeness();
}

/*
 * BTLACT_SHOOT (asm/battle/actions/shoot.asm)
 *
 * Ranged attack: miss check (gun miss text) → dodge check → level 2 attack.
 * No SMAAAASH check and no strangeness healing.
 */
void btlact_shoot(void) {
    if (battle_miss_calc(1))
        return;
    if (battle_determine_dodge()) {
        display_in_battle_text_addr(MSG_BTL_UTU_YOKETA); /* dodged shot */
        return;
    }
    battle_level_2_attack();
}

/*
 * BTLACT_SPY (asm/battle/actions/spy.asm)
 *
 * Displays enemy's offense, defense, and elemental vulnerabilities.
 * If the enemy has a stealable item and the player has inventory space,
 * gives the item to the player.
 */
void btlact_spy(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    /* Display offense */
    display_text_wait_addr(MSG_BTL_CHECK_OFFENSE, tgt->offense);

    /* Display defense */
    display_text_wait_addr(MSG_BTL_CHECK_DEFENSE, tgt->defense);

    /* Check elemental resistances — display if 0xFF (complete immunity) */
    if (tgt->fire_resist == 0xFF) {
        display_in_battle_text_addr(MSG_BTL_CHECK_ANTI_FIRE);
    }
    if (tgt->freeze_resist == 0xFF) {
        display_in_battle_text_addr(MSG_BTL_CHECK_ANTI_FREEZE);
    }
    if (tgt->flash_resist == 0xFF) {
        display_in_battle_text_addr(MSG_BTL_CHECK_ANTI_FLASH);
    }
    if (tgt->paralysis_resist == 0xFF) {
        display_in_battle_text_addr(MSG_BTL_CHECK_ANTI_PARALYSIS);
    }
    if (tgt->hypnosis_resist == 0xFF) {
        display_in_battle_text_addr(MSG_BTL_CHECK_BRAIN_LEVEL_0);
    }
    if (tgt->brainshock_resist == 0xFF) {
        display_in_battle_text_addr(MSG_BTL_CHECK_BRAIN_LEVEL_3);
    }

    /* Check for stealable item drop */
    if (tgt->ally_or_enemy == 1) {
        if (find_inventory_space2(3) != 0) {
            if (bt.item_dropped != 0) {
                set_current_item((uint8_t)bt.item_dropped);
                display_in_battle_text_addr(MSG_BTL_CHECK_PRESENT_GET);
                bt.item_dropped = 0;
            }
        }
    }
}

/*
 * BTLACT_LEVEL_1_ATTACK (wrapper — asm/battle/actions/level_1_attack.asm)
 *
 * Standard physical attack with miss/smaaaash/dodge checks.
 * Uses level 1 damage formula (offense - defense).
 */
void btlact_level_1_attack(void) {
    if (battle_miss_calc(0))
        return;
    if (battle_smaaaash())
        return;
    if (battle_determine_dodge()) {
        display_in_battle_text_addr(MSG_BTL_TATAKU_YOKETA); /* dodged */
        return;
    }
    battle_level_1_attack();
    battle_heal_strangeness();
}

/*
 * BTLACT_HEALING_A (asm/battle/actions/healing_alpha.asm)
 *
 * Healing Alpha PSI: cures cold, sunstroke, or sleep.
 * If none of those are active, displays "no effect" message.
 */
void btlact_healing_alpha(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    /* Check PERSISTENT_EASYHEAL group first */
    uint8_t easyheal = tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];

    if (easyheal == STATUS_0_COLD) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        display_in_battle_text_addr(MSG_BTL_KAZE_OFF);
        return;
    }
    if (easyheal == STATUS_0_SUNSTROKE) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        display_in_battle_text_addr(MSG_BTL_NISSYA_OFF);
        return;
    }

    /* Check TEMPORARY group for sleep */
    uint8_t temp = tgt->afflictions[STATUS_GROUP_TEMPORARY];

    if (temp == STATUS_2_ASLEEP) {
        tgt->afflictions[STATUS_GROUP_TEMPORARY] = 0;
        display_in_battle_text_addr(MSG_BTL_NEMURI_OFF);
        return;
    }

    /* No curable status — display "no effect" */
    display_in_battle_text_addr(MSG_BTL_HEAL_NG);
}

/*
 * BTLACT_SHIELD_A (asm/battle/actions/shield_alpha.asm)
 *
 * Shield Alpha PSI: applies STATUS_6_SHIELD to current target.
 * Displays appropriate text for new shield or shield refresh.
 */
void btlact_shield_alpha(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    uint16_t result = battle_shields_common(tgt, STATUS_6_SHIELD);

    if (result == 0) {
        /* Shield already active — refreshed (assembly: BEQ = result==0) */
        display_in_battle_text_addr(MSG_BTL_SHIELD_ON);
    } else {
        /* New shield applied */
        display_in_battle_text_addr(MSG_BTL_SHIELD_ADD);
    }
}

/*
 * BTLACT_HEALING_B (asm/battle/actions/healing_beta.asm)
 *
 * Healing Beta PSI: cures poison, nausea, crying, strangeness.
 * Falls back to Healing Alpha if none of those are active.
 */
void btlact_healing_beta(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    /* Check PERSISTENT_EASYHEAL group */
    uint8_t easyheal = tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];

    if (easyheal == STATUS_0_POISONED) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        display_in_battle_text_addr(MSG_BTL_MODOKU_OFF);
        return;
    }
    if (easyheal == STATUS_0_NAUSEOUS) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        display_in_battle_text_addr(MSG_BTL_KIMOCHI_OFF);
        return;
    }

    /* Check TEMPORARY group for crying */
    if (tgt->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_CRYING) {
        tgt->afflictions[STATUS_GROUP_TEMPORARY] = 0;
        display_in_battle_text_addr(MSG_BTL_NAMIDA_OFF);
        return;
    }

    /* Check STRANGENESS group */
    if (tgt->afflictions[STATUS_GROUP_STRANGENESS] == STATUS_3_STRANGE) {
        tgt->afflictions[STATUS_GROUP_STRANGENESS] = 0;
        display_in_battle_text_addr(MSG_BTL_HEN_OFF);
        return;
    }

    /* Fall back to Healing Alpha */
    btlact_healing_alpha();
}

/*
 * BTLACT_HEALING_G (asm/battle/actions/healing_gamma.asm)
 *
 * Healing Gamma PSI: cures paralysis, diamondize, unconscious (revive).
 * Revive: 75% success rate (SUCCESS_255 with threshold 192).
 *   On success: revive with hp_max/4.
 *   On failure: display "failed to revive" text.
 * Falls back to Healing Beta if none of those are active.
 */
void btlact_healing_gamma(void) {
    Battler *tgt = battler_from_offset(bt.current_target);
    uint8_t easyheal = tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];

    if (easyheal == STATUS_0_PARALYZED) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        display_in_battle_text_addr(MSG_BTL_SHIBIRE_OFF);
        return;
    }
    if (easyheal == STATUS_0_DIAMONDIZED) {
        tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] = 0;
        display_in_battle_text_addr(MSG_BTL_DAIYA_OFF);
        return;
    }
    if (easyheal == STATUS_0_UNCONSCIOUS) {
        /* 75% chance to revive */
        if (battle_success_255(192)) {
            /* Revive with hp_max / 4 */
            uint16_t revive_hp = tgt->hp_max >> 2;
            battle_revive_target(tgt, revive_hp);
        } else {
            display_in_battle_text_addr(MSG_BTL_IKIKAERI_F); /* revive failed */
        }
        return;
    }

    /* Fall back to Healing Beta */
    btlact_healing_beta();
}

/*
 * BTLACT_HEALING_O (asm/battle/actions/healing_omega.asm)
 *
 * Healing Omega PSI: revives with full HP, or falls back to Healing Gamma.
 */
void btlact_healing_omega(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    if (tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS) {
        /* Revive with full hp_max */
        battle_revive_target(tgt, tgt->hp_max);
        return;
    }

    /* Fall back to Healing Gamma */
    btlact_healing_gamma();
}

/*
 * BTLACT_SHIELD_B (asm/battle/actions/shield_beta.asm)
 *
 * Shield Beta PSI: applies SHIELD_POWER to current target.
 */
void btlact_shield_beta(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    uint16_t result = battle_shields_common(tgt, STATUS_6_SHIELD_POWER);

    if (result == 0) {
        display_in_battle_text_addr(MSG_BTL_POWER_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_POWER_ADD);
    }
}

/*
 * BTLACT_PSI_SHIELD_A (asm/battle/actions/psi_shield_alpha.asm)
 *
 * PSI Shield Alpha: applies PSI_SHIELD to current target.
 */
void btlact_psi_shield_alpha(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    uint16_t result = battle_shields_common(tgt, STATUS_6_PSI_SHIELD);

    if (result == 0) {
        display_in_battle_text_addr(MSG_BTL_PSYCO_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_PSYCO_ADD);
    }
}

/*
 * BTLACT_PSI_SHIELD_B (asm/battle/actions/psi_shield_beta.asm)
 *
 * PSI Shield Beta: applies PSI_SHIELD_POWER to current target.
 */
void btlact_psi_shield_beta(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    uint16_t result = battle_shields_common(tgt, STATUS_6_PSI_SHIELD_POWER);

    if (result == 0) {
        display_in_battle_text_addr(MSG_BTL_PSYPOWER_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_PSYPOWER_ADD);
    }
}


/* ======================================================================
 * HP/PP recovery battle actions
 * ====================================================================== */

void btlact_hp_recovery_10(void) {
    uint16_t amount = battle_25pct_variance(10);
    battle_recover_hp(battler_from_offset(bt.current_target), amount);
}

void btlact_hp_recovery_50(void) {
    uint16_t amount = battle_25pct_variance(50);
    battle_recover_hp(battler_from_offset(bt.current_target), amount);
}

void btlact_hp_recovery_100(void) {
    uint16_t amount = battle_25pct_variance(100);
    battle_recover_hp(battler_from_offset(bt.current_target), amount);
}

void btlact_hp_recovery_200(void) {
    uint16_t amount = battle_25pct_variance(200);
    battle_recover_hp(battler_from_offset(bt.current_target), amount);
}

void btlact_hp_recovery_300(void) {
    uint16_t amount = battle_25pct_variance(300);
    battle_recover_hp(battler_from_offset(bt.current_target), amount);
}

/*
 * BTLACT_HP_RECOVERY_1D4 (asm/battle/actions/hp_recovery_1d4.asm)
 *
 * Recover rand(4)+1 HP (1-4 HP). Used by weak healing items.
 */
void btlact_hp_recovery_1d4(void) {
    uint16_t amount = rand_limit(4) + 1;
    battle_recover_hp(battler_from_offset(bt.current_target), amount);
}

/*
 * BTLACT_HP_RECOVERY_10000 (asm/battle/actions/hp_recovery_10000.asm)
 *
 * If target is Poo, recover 10000 HP (full heal).
 * Otherwise, fall back to 1d4 recovery (Brain Food Lunch flavor text).
 */
void btlact_hp_recovery_10000(void) {
    Battler *tgt = battler_from_offset(bt.current_target);
    if (tgt->id == PARTY_MEMBER_POO) {
        battle_recover_hp(tgt, 10000);
    } else {
        btlact_hp_recovery_1d4();
    }
}

void btlact_pp_recovery_20(void) {
    uint16_t amount = battle_25pct_variance(20);
    battle_recover_pp(battler_from_offset(bt.current_target), amount);
}

void btlact_pp_recovery_80(void) {
    uint16_t amount = battle_25pct_variance(80);
    battle_recover_pp(battler_from_offset(bt.current_target), amount);
}

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

    display_in_battle_text_addr(MSG_BTL_TIMESTOP_RET);
    bt.battler_target_flags = 0;
}

/* ======================================================================
 * Status effect actions
 * ====================================================================== */

/*
 * BTLACT_POISON (asm/battle/actions/poison.asm)
 *
 * Inflict poison on current target. Fails on NPCs.
 */
void btlact_poison(void) {
    if (battle_fail_attack_on_npcs())
        return;
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_POISONED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_MODOKU_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_NAUSEATE (asm/battle/actions/nauseate.asm)
 *
 * Inflict nausea on current target. Fails on NPCs.
 */
void btlact_nauseate(void) {
    if (battle_fail_attack_on_npcs())
        return;
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_NAUSEOUS);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_KIMOCHI_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_FEELSTRANGE (asm/battle/actions/feel_strange.asm)
 *
 * Inflict "strange" status on current target. Fails on NPCs.
 */
void btlact_feel_strange(void) {
    if (battle_fail_attack_on_npcs())
        return;
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_STRANGENESS, STATUS_3_STRANGE);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_HEN_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_IMMOBILIZE (asm/battle/actions/immobilize.asm)
 *
 * Inflict immobilized status on current target (no NPC check).
 */
void btlact_immobilize(void) {
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_TEMPORARY, STATUS_2_IMMOBILIZED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_SHIBARA_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
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
 *
 * Level 2 attack + poison infliction. Fails on NPCs.
 */
void btlact_level_2_attack_poison(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (battle_miss_calc(0))
        return;
    if (battle_smaaaash())
        return;
    if (battle_determine_dodge()) {
        display_in_battle_text_addr(MSG_BTL_TATAKU_YOKETA);
        return;
    }
    battle_level_2_attack();
    battle_heal_strangeness();
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_POISONED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_MODOKU_ON);
    }
}

/*
 * BTLACT_LVL_2_ATK_DIAMONDIZE (asm/battle/actions/level_2_attack_diamondize.asm)
 *
 * Level 2 attack + diamondize infliction. Fails on NPCs.
 * On diamondize success: clears all other status groups, accumulates exp/money.
 */
void btlact_level_2_attack_diamondize(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (battle_miss_calc(0))
        return;
    if (battle_smaaaash())
        return;
    if (battle_determine_dodge()) {
        display_in_battle_text_addr(MSG_BTL_TATAKU_YOKETA);
        return;
    }
    battle_level_2_attack();
    battle_heal_strangeness();
    if (!battle_success_luck80()) {
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_DIAMONDIZED);
    if (result == 0) {
        return;
    }

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

    display_in_battle_text_addr(MSG_BTL_DAIYA_ON);
}

/* ======================================================================
 * PSI common functions
 * ====================================================================== */

/*
 * PSI_FIRE_COMMON (asm/battle/actions/psi_fire_common.asm)
 *
 * Common PSI Fire logic: shield check → 25% variance → fire resist → damage.
 */
void btlact_psi_fire_common(uint16_t base_damage) {
    if (battle_psi_shield_nullify())
        return;
    uint16_t damage = battle_25pct_variance(base_damage);
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t resist = target->fire_resist;
    battle_calc_resist_damage(damage, resist);
    battle_weaken_shield();
}

/*
 * PSI_FREEZE_COMMON (asm/battle/actions/psi_freeze_common.asm)
 *
 * Common PSI Freeze logic: NPC check → shield check → 25% variance →
 * freeze resist → damage. If damage dealt and target alive, 25% chance
 * to inflict solidified status.
 */
void btlact_psi_freeze_common(uint16_t base_damage) {
    if (battle_fail_attack_on_npcs())
        return;
    if (battle_psi_shield_nullify())
        return;
    uint16_t damage = battle_25pct_variance(base_damage);
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t resist = target->freeze_resist;
    uint16_t dealt = battle_calc_resist_damage(damage, resist);

    /* If target is unconscious or no damage dealt, skip solidify */
    if (target->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS)
        goto weaken;
    if (dealt == 0)
        goto weaken;

    /* 25% chance to inflict solidified */
    if (rand_limit(100) < 25) {
        uint16_t result = battle_inflict_status(target,
            STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED);
        if (result != 0) {
            display_in_battle_text_addr(MSG_BTL_KOORI_ON);
        }
    }

weaken:
    battle_weaken_shield();
}

/*
 * PSI_ROCKIN_COMMON (asm/battle/actions/psi_rockin_common.asm)
 *
 * Common PSI Rockin' logic: shield check → 50% variance → dodge check →
 * damage with full resistance. Uses 50% variance (wider than fire/freeze).
 */
void btlact_psi_rockin_common(uint16_t base_damage) {
    if (battle_psi_shield_nullify())
        return;
    uint16_t damage = battle_50pct_variance(base_damage);
    if (battle_determine_dodge()) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        goto weaken;
    }
    battle_calc_resist_damage(damage, 0xFF);
weaken:
    battle_weaken_shield();
}

/*
 * PSI_STARSTORM_COMMON (asm/battle/actions/psi_starstorm_common.asm)
 *
 * Common PSI Starstorm logic: shield check → 25% variance → damage.
 * No resistance check (0xFF = full damage).
 */
void btlact_psi_starstorm_common(uint16_t base_damage) {
    if (battle_psi_shield_nullify())
        return;
    uint16_t damage = battle_25pct_variance(base_damage);
    battle_calc_resist_damage(damage, 0xFF);
    battle_weaken_shield();
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
                display_in_battle_text_addr(MSG_BTL_THUNDER_SMALL);
            } else {
                display_in_battle_text_addr(MSG_BTL_THUNDER_LARGE);
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
                    display_in_battle_text_addr(MSG_BTL_FRANKLIN_TURN);
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
            display_in_battle_text_addr(MSG_BTL_THUNDER_MISS_SE);
            display_in_battle_text_addr(MSG_BTL_KAMINARI_HAZURE);
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

void btlact_psi_fire_alpha(void) { btlact_psi_fire_common(FIRE_ALPHA_DAMAGE); }
void btlact_psi_fire_beta(void)  { btlact_psi_fire_common(FIRE_BETA_DAMAGE); }
void btlact_psi_fire_gamma(void) { btlact_psi_fire_common(FIRE_GAMMA_DAMAGE); }
void btlact_psi_fire_omega(void) { btlact_psi_fire_common(FIRE_OMEGA_DAMAGE); }

void btlact_psi_freeze_alpha(void) { btlact_psi_freeze_common(FREEZE_ALPHA_DAMAGE); }
void btlact_psi_freeze_beta(void)  { btlact_psi_freeze_common(FREEZE_BETA_DAMAGE); }
void btlact_psi_freeze_gamma(void) { btlact_psi_freeze_common(FREEZE_GAMMA_DAMAGE); }
void btlact_psi_freeze_omega(void) { btlact_psi_freeze_common(FREEZE_OMEGA_DAMAGE); }

void btlact_psi_rockin_alpha(void) { btlact_psi_rockin_common(ROCKIN_ALPHA_DAMAGE); }
void btlact_psi_rockin_beta(void)  { btlact_psi_rockin_common(ROCKIN_BETA_DAMAGE); }
void btlact_psi_rockin_gamma(void) { btlact_psi_rockin_common(ROCKIN_GAMMA_DAMAGE); }
void btlact_psi_rockin_omega(void) { btlact_psi_rockin_common(ROCKIN_OMEGA_DAMAGE); }

void btlact_psi_starstorm_alpha(void) { btlact_psi_starstorm_common(STARSTORM_ALPHA_DAMAGE); }
void btlact_psi_starstorm_omega(void) { btlact_psi_starstorm_common(STARSTORM_OMEGA_DAMAGE); }

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
 * Apply 25% variance to base healing, then recover HP.
 */
void lifeup_common(uint16_t base_healing) {
    uint16_t healing = battle_25pct_variance(base_healing);
    battle_recover_hp(battler_from_offset(bt.current_target), healing);
}

void btlact_lifeup_alpha(void) { lifeup_common(LIFEUP_ALPHA_HEALING); }
void btlact_lifeup_beta(void)  { lifeup_common(LIFEUP_BETA_HEALING); }
void btlact_lifeup_gamma(void) { lifeup_common(LIFEUP_GAMMA_HEALING); }
void btlact_lifeup_omega(void) { lifeup_common(LIFEUP_OMEGA_HEALING); }

/* ======================================================================
 * Bottle rockets
 * ====================================================================== */

/*
 * BOTTLE_ROCKET_COMMON (asm/battle/actions/bottle_rocket_common.asm)
 *
 * Fire 'count' rockets. Each has a speed-based hit chance (SUCCESS_SPEED 100).
 * Total damage = hits * 120, with 25% variance, full resistance.
 */
void bottle_rocket_common(uint16_t count) {
    uint16_t hits = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (battle_success_speed(100))
            hits++;
    }
    if (hits == 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t damage = battle_25pct_variance(hits * 120);
    battle_calc_resist_damage(damage, 0xFF);
}

void btlact_bottle_rocket(void)       { bottle_rocket_common(BOTTLE_ROCKET_COUNT); }
void btlact_big_bottle_rocket(void)   { bottle_rocket_common(BIG_BOTTLE_ROCKET_COUNT); }
void btlact_multi_bottle_rocket(void) { bottle_rocket_common(MULTI_BOTTLE_ROCKET_COUNT); }

/* ======================================================================
 * Item spray/bomb common functions
 * ====================================================================== */

/*
 * INSECT_SPRAY_COMMON (asm/battle/actions/insect_spray_common.asm)
 *
 * Luck80 check, target must be enemy, enemy type must be 1 (insect).
 * 50% variance on base damage.
 */
void insect_spray_common(uint16_t base_damage) {
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (target->ally_or_enemy != 1) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    if (battle_get_enemy_type(target->id) != 1) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t damage = battle_50pct_variance(base_damage);
    battle_calc_resist_damage(damage, 0xFF);
}

/*
 * RUST_SPRAY_COMMON (asm/battle/actions/rust_promoter_common.asm)
 *
 * Same as insect spray but checks enemy type 2 (metallic).
 */
void rust_spray_common(uint16_t base_damage) {
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (target->ally_or_enemy != 1) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    if (battle_get_enemy_type(target->id) != 2) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t damage = battle_50pct_variance(base_damage);
    battle_calc_resist_damage(damage, 0xFF);
}

void btlact_insecticide_spray(void) { insect_spray_common(100); }
void btlact_xterminator_spray(void) { insect_spray_common(200); }
void btlact_rust_promoter(void)     { rust_spray_common(200); }
void btlact_rust_promoter_dx(void)  { rust_spray_common(400); }

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
        display_in_battle_text_addr(MSG_BTL_TLPTBOX_CANT);
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
            display_in_battle_text_addr(MSG_BTL_TLPTBOX_NG);
            return;
        }
    }
    /* Fail in boss battles */
    if (battle_boss_battle_check() == 0) {
        display_in_battle_text_addr(MSG_BTL_TLPTBOX_NG);
        return;
    }

teleport_success:;
    /* Remove item from inventory */
    uint8_t slot = attacker->action_item_slot & 0xFF;
    remove_item_from_inventory(attacker->id, slot);
    display_in_battle_text_addr(MSG_BTL_TLPTBOX_OK);
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
        display_in_battle_text_addr(MSG_BTL_TANEMAKI_HAETA); /* seeds sprouted */
    } else {
        display_in_battle_text_addr(MSG_BTL_NAKAMA_KITA); /* called for help */
    }
    return;

help_failed:
    if (param) {
        display_in_battle_text_addr(MSG_BTL_TANEMAKI_NO); /* seeds didn't sprout */
    } else {
        display_in_battle_text_addr(MSG_BTL_NAKAMA_NO); /* nobody came */
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }

    /* Attacker must be alive (hp_target > 0) */
    Battler *attacker = battler_from_offset(bt.current_attacker);
    if (attacker->hp_target == 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }

    /* Self-targeting check (e.g., feeling strange) */
    if (bt.current_target == bt.current_attacker) {
        display_in_battle_text_addr(MSG_BTL_HPSUCK_ME);
        return;
    }

    /* Calculate drain amount: 50% variance on target's max HP, then /8 */
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t drain_amount = battle_50pct_variance(target->hp_max) >> 3;

    /* Reduce target's HP */
    battle_reduce_hp(target, drain_amount);

    display_text_wait_addr(MSG_BTL_HPSUCK_ON, drain_amount);

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
        display_in_battle_text_addr(MSG_BTL_METAMORPHOSE_NG);
        return;
    }
    /* Must not be an NPC ally */
    if ((target->npc_id & 0xFF) != 0) {
        display_in_battle_text_addr(MSG_BTL_METAMORPHOSE_NG);
        return;
    }
    /* Check mirror success rate from enemy config table */
    uint16_t roll = rand_limit(100);
    if (enemy_config_table != NULL) {
        uint8_t success_rate = enemy_config_table[target_id].mirror_success;
        if (roll >= success_rate) {
            display_in_battle_text_addr(MSG_BTL_METAMORPHOSE_NG);
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

    display_in_battle_text_addr(MSG_BTL_METAMORPHOSE_OK);
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
            display_in_battle_text_addr(MSG_BTL_EAT_SPICE_ATARI);
            return &entry->strength;  /* points to [strength, epi, ep, special] */
        }
        /* Wrong condiment for this food */
        display_in_battle_text_addr(MSG_BTL_EAT_SPICE_HAZURE);
        return default_params;
    }

    /* Food not in condiment table — wrong condiment */
    display_in_battle_text_addr(MSG_BTL_EAT_SPICE_HAZURE);
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
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
    display_text_wait_addr(MSG_BTL_IQ_UP, amount);
    goto check_special;

do_boost_guts:
    /*
     * Assembly @BOOST_GUTS: increment battler.guts (16-bit), then increment
     * party_characters[idx].boosted_guts (8-bit), then recalculate guts.
     */
    tgt->guts += (uint16_t)amount;
    party_characters[idx].boosted_guts += (uint8_t)amount;
    recalc_character_postmath_guts(char_id);
    display_text_wait_addr(MSG_BTL_GUTS_UP, amount);
    goto check_special;

do_boost_speed:
    /*
     * Assembly @BOOST_SPEED: increment battler.speed (16-bit), then increment
     * party_characters[idx].boosted_speed (8-bit), then recalculate speed.
     */
    tgt->speed += (uint16_t)amount;
    party_characters[idx].boosted_speed += (uint8_t)amount;
    recalc_character_postmath_speed(char_id);
    display_text_wait_addr(MSG_BTL_SPEED_UP, amount);
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
    display_text_wait_addr(MSG_BTL_VITA_UP, amount);
    goto check_special;

do_boost_luck:
    /*
     * Assembly @BOOST_LUCK: increment battler.luck (16-bit), then increment
     * party_characters[idx].boosted_luck (8-bit), then recalculate luck.
     */
    tgt->luck += (uint16_t)amount;
    party_characters[idx].boosted_luck += (uint8_t)amount;
    recalc_character_postmath_luck(char_id);
    display_text_wait_addr(MSG_BTL_LUCK_UP, amount);
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
void btlact_350_fire_damage(void) {
    uint16_t damage = battle_25pct_variance(350);
    Battler *target = battler_from_offset(bt.current_target);
    battle_calc_resist_damage(damage, target->fire_resist);
}

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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
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
            display_in_battle_text_addr(MSG_BTL_MODOKU_ON);
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_COLD);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_KAZE_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_POISONED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_MODOKU_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist)) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_PARALYZED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_SHIBIRE_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_INFLICT_SOLIDIFICATION (asm/battle/actions/inflict_solidification.asm)
 *
 * Inflict solidified. Luck80 check + paralysis_resist check. No NPC check.
 */
void btlact_inflict_solidification(void) {
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist)) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_KOORI_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (target->afflictions[STATUS_GROUP_CONCENTRATION] != 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    target->afflictions[STATUS_GROUP_CONCENTRATION] = 4;
    display_in_battle_text_addr(MSG_BTL_FUUIN_ON);
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist)) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    if (target->afflictions[STATUS_GROUP_CONCENTRATION] != 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    target->afflictions[STATUS_GROUP_CONCENTRATION] = STATUS_4_CANT_CONCENTRATE4;
    display_in_battle_text_addr(MSG_BTL_FUUIN_ON);
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
    display_in_battle_text_addr(MSG_BTL_NEUTRALIZE_RESULT);
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
        display_in_battle_text_addr(MSG_BTL_MODOKU_OFF);
    }
}

/*
 * BTLACT_SHIELD_KILLER (asm/battle/actions/shield_killer.asm)
 *
 * Remove shield from target. Luck80 check.
 */
void btlact_shield_killer(void) {
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    if (target->afflictions[STATUS_GROUP_SHIELD] == 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    target->afflictions[STATUS_GROUP_SHIELD] = 0;
    display_in_battle_text_addr(MSG_BTL_SHIELD_OFF);
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
void btlact_diamondize(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist)) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_DIAMONDIZED);
    if (result == 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }

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

    display_in_battle_text_addr(MSG_BTL_DAIYA_ON);
}

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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_HARDHEAL, STATUS_1_POSSESSED);
    if (result == 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    display_in_battle_text_addr(MSG_BTL_TORITSU_ON);
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
        display_in_battle_text_addr(MSG_BTL_PPSUCK_ZERO);
        return;
    }
    uint16_t drain = target->pp_max / 16;
    if (drain == 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    drain = battle_50pct_variance(drain);
    battle_reduce_pp(target, drain);
    display_text_wait_addr(MSG_BTL_PPSUCK_OBJ, drain);
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
        display_in_battle_text_addr(MSG_BTL_PPSUCK_ZERO);
        return;
    }
    /* Assembly lines 15-28: drain = rand(4) + rand(4) + 2 → range [2..9] */
    uint16_t drain = rand_limit(4) + rand_limit(4) + 2;
    /* Clamp to target's actual PP */
    if (target->pp_target < drain)
        drain = target->pp_target;
    display_text_wait_addr(MSG_BTL_PPSUCK, drain);
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    int16_t damage = HANDBAG_STRAP_BASE_DAMAGE - (int16_t)target->defense;
    if (damage <= 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    battle_calc_resist_damage((uint16_t)damage, 0xFF);
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_KOORI_ON);
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    int16_t damage = MUMMY_WRAP_BASE_DAMAGE - (int16_t)target->defense;
    if (damage <= 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    battle_calc_resist_damage((uint16_t)damage, 0xFF);
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_KOORI_ON);
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
            display_in_battle_text_addr(MSG_BTL_G_HAEMITU_G); /* fly honey worked! */
            return;
        }
    }
    display_in_battle_text_addr(MSG_BTL_G_HAEMITU_NG); /* no Master Belch found */
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
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
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
void btlact_crying(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->flash_resist)) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_TEMPORARY, STATUS_2_CRYING);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_NAMIDA_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_CRYING2 (asm/battle/actions/crying2.asm)
 *
 * Inflict crying on target without resist check.
 * Fails on NPCs. Status group is same as status ID.
 */
void btlact_crying2(void) {
    if (battle_fail_attack_on_npcs())
        return;
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_2_CRYING, STATUS_2_CRYING);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_NAMIDA_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_SOLIDIFY (asm/battle/actions/solidify.asm)
 *
 * Inflict solidified on target. Luck80 check. Fails on NPCs.
 */
void btlact_solidify(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_KOORI_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_SOLIDIFY_2 (asm/battle/actions/solidify_2.asm)
 *
 * Inflict solidified on target. Luck80 check. No NPC check.
 */
void btlact_solidify_2(void) {
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_GROUP_TEMPORARY, STATUS_2_SOLIDIFIED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_KOORI_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_MUSHROOMIZE (asm/battle/actions/mushroomize.asm)
 *
 * Inflict mushroomized on target. No resist check.
 * Fails on NPCs. Status group is same as status ID.
 */
void btlact_mushroomize(void) {
    if (battle_fail_attack_on_npcs())
        return;
    uint16_t result = battle_inflict_status(
        battler_from_offset(bt.current_target),
        STATUS_1_MUSHROOMIZED, STATUS_1_MUSHROOMIZED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_KINOKO_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_PARALYSIS_A (asm/battle/actions/paralysis_alpha.asm)
 *
 * PSI Paralysis α: Inflict paralysis with resist check via paralysis_resist.
 * Fails on NPCs.
 */
void btlact_paralysis_alpha(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->paralysis_resist)) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_PERSISTENT_EASYHEAL, STATUS_0_PARALYZED);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_SHIBIRE_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_HYPNOSIS_A (asm/battle/actions/hypnosis_alpha.asm)
 *
 * PSI Hypnosis α: Inflict sleep with resist check via hypnosis_resist.
 * Fails on NPCs.
 */
void btlact_hypnosis_alpha(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->hypnosis_resist)) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_TEMPORARY, STATUS_2_ASLEEP);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_NEMURI_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}

/*
 * BTLACT_BRAINSHOCK_A (asm/battle/actions/brainshock_alpha.asm)
 *
 * PSI Brainshock α: Inflict "strange" with resist check via brainshock_resist.
 * Fails on NPCs.
 */
void btlact_brainshock_alpha(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    if (!battle_success_255(target->brainshock_resist)) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    uint16_t result = battle_inflict_status(target,
        STATUS_GROUP_STRANGENESS, STATUS_3_STRANGE);
    if (result != 0) {
        display_in_battle_text_addr(MSG_BTL_HEN_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
    }
}


/* ======================================================================
 * Stat modification battle actions
 * ====================================================================== */

/*
 * BTLACT_OFFENSE_UP_A (asm/battle/actions/offense_up_alpha.asm)
 *
 * Increase target's offense by 1/16th and display the change amount.
 * Fails on NPCs.
 */
void btlact_offense_up_alpha(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t old_offense = target->offense;
    battle_increase_offense(target);
    uint16_t diff = target->offense - old_offense;
    display_text_wait_addr(MSG_BTL_OFFENSE_UP, diff);
}

/*
 * BTLACT_DEFENSE_DOWN_A (asm/battle/actions/defense_down_alpha.asm)
 *
 * Decrease target's defense by 1/16th. Luck80 check for success.
 * Fails on NPCs. Displays the reduction amount (clamped to >= 0).
 */
void btlact_defense_down_alpha(void) {
    if (battle_fail_attack_on_npcs())
        return;
    if (!battle_success_luck80()) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return;
    }
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t old_defense = target->defense;
    battle_decrease_defense(target);
    int16_t diff = (int16_t)(old_defense - target->defense);
    if (diff < 0)
        diff = 0;
    display_text_wait_addr(MSG_BTL_DEFENSE_DOWN, (uint32_t)diff);
}

/*
 * BTLACT_SPEED_UP_1D4 (asm/battle/actions/speed_up_1d4.asm)
 *
 * Increase target's speed by 1-4 points (random).
 */
void btlact_speed_up_1d4(void) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->speed += amount;
    display_text_wait_addr(MSG_BTL_SPEED_UP, amount);
}

/*
 * BTLACT_GUTS_UP_1D4 (asm/battle/actions/guts_up_1d4.asm)
 *
 * Increase target's guts by 1-4 points (random).
 */
void btlact_guts_up_1d4(void) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->guts += amount;
    display_text_wait_addr(MSG_BTL_GUTS_UP, amount);
}

/*
 * BTLACT_VITALITY_UP_1D4 (asm/battle/actions/vitality_up_1d4.asm)
 *
 * Increase target's vitality by 1-4 points (random). 8-bit add.
 */
void btlact_vitality_up_1d4(void) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->vitality += (uint8_t)amount;
    display_text_wait_addr(MSG_BTL_VITA_UP, amount);
}

/*
 * BTLACT_IQ_UP_1D4 (asm/battle/actions/iq_up_1d4.asm)
 *
 * Increase target's IQ by 1-4 points (random). 8-bit add.
 */
void btlact_iq_up_1d4(void) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->iq += (uint8_t)amount;
    display_text_wait_addr(MSG_BTL_IQ_UP, amount);
}

/*
 * BTLACT_LUCK_UP_1D4 (asm/battle/actions/luck_up_1d4.asm)
 *
 * Increase target's luck by 1-4 points (random). 16-bit add.
 */
void btlact_luck_up_1d4(void) {
    uint16_t amount = rand_limit(4) + 1;
    Battler *target = battler_from_offset(bt.current_target);
    target->luck += amount;
    display_text_wait_addr(MSG_BTL_LUCK_UP, amount);
}

/*
 * BTLACT_RANDOM_STAT_UP_1D4 (asm/battle/actions/random_stat_up_1d4.asm)
 *
 * Randomly boosts one of seven stats by 1-5 points.
 * Stat selection: 0=defense, 1=offense, 2=speed, 3=guts, 4=vitality, 5=IQ, 6=luck.
 */
void btlact_random_stat_up_1d4(void) {
    uint16_t stat = rand_limit(7);
    switch (stat) {
    case 0: { /* Defense */
        uint16_t amount = rand_limit(4) + 1;
        Battler *target = battler_from_offset(bt.current_target);
        target->defense += amount;
        display_text_wait_addr(MSG_BTL_DEFENSE_UP, amount);
        break;
    }
    case 1: { /* Offense */
        uint16_t amount = rand_limit(4) + 1;
        Battler *target = battler_from_offset(bt.current_target);
        target->offense += amount;
        display_text_wait_addr(MSG_BTL_OFFENSE_UP, amount);
        break;
    }
    case 2: btlact_speed_up_1d4(); break;
    case 3: btlact_guts_up_1d4(); break;
    case 4: btlact_vitality_up_1d4(); break;
    case 5: btlact_iq_up_1d4(); break;
    case 6: btlact_luck_up_1d4(); break;
    }
}

/*
 * BTLACT_REDUCEOFF (asm/battle/actions/reduce_offense.asm)
 *
 * Decrease target's offense by 1/16th and display the reduction.
 * Fails on NPCs.
 */
void btlact_reduce_offense(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t old_offense = target->offense;
    battle_decrease_offense(target);
    uint16_t diff = old_offense - target->offense;
    display_text_wait_addr(MSG_BTL_OFFENSE_DOWN, diff);
}

/*
 * BTLACT_REDUCEOFFDEF (asm/battle/actions/reduce_offense_defense.asm)
 *
 * Decrease target's offense and defense each by 1/16th.
 * Displays both changes separately. Fails on NPCs.
 */
void btlact_reduce_offense_defense(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);

    /* Reduce offense */
    uint16_t old_offense = target->offense;
    battle_decrease_offense(target);
    uint16_t off_diff = old_offense - target->offense;
    display_text_wait_addr(MSG_BTL_OFFENSE_DOWN, off_diff);

    /* Reduce defense */
    uint16_t old_defense = target->defense;
    battle_decrease_defense(target);
    uint16_t def_diff = old_defense - target->defense;
    display_text_wait_addr(MSG_BTL_DEFENSE_DOWN, def_diff);
}

/*
 * BTLACT_SUDDEN_GUTS_PILL (asm/battle/actions/sudden_guts_pill.asm)
 *
 * Double target's guts, clamped to 255. Fails on NPCs.
 * Displays the new guts value.
 */
void btlact_sudden_guts_pill(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t new_guts = target->guts * 2;
    if (new_guts > 0xFF)
        new_guts = 0xFF;
    target->guts = new_guts;
    display_text_wait_addr(MSG_BTL_2GUTS_UP, target->guts);
}

/*
 * BTLACT_DEFENSE_SPRAY (asm/battle/actions/defense_spray.asm)
 *
 * Increase target's defense by 1/16th and display the change.
 * Fails on NPCs.
 */
void btlact_defense_spray(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t old_defense = target->defense;
    battle_increase_defense(target);
    uint16_t diff = target->defense - old_defense;
    display_text_wait_addr(MSG_BTL_DEFENSE_UP, diff);
}

/*
 * BTLACT_DEFENSE_SHOWER (asm/battle/actions/defense_shower.asm)
 *
 * Wrapper for defense_spray (same effect, different item).
 */
void btlact_defense_shower(void) {
    btlact_defense_spray();
}

/*
 * BTLACT_CUTGUTS (asm/battle/actions/cut_guts.asm)
 *
 * Reduce target's guts to 3/4 of current value.
 * Floor at base_guts / 2. Fails on NPCs.
 */
void btlact_cut_guts(void) {
    if (battle_fail_attack_on_npcs())
        return;
    Battler *target = battler_from_offset(bt.current_target);
    uint16_t old_guts = target->guts;

    /* guts = guts * 3 / 4 */
    target->guts = (target->guts * 3) / 4;

    /* Floor at base_guts / 2 */
    uint16_t min_guts = target->base_guts / 2;
    if (target->guts < min_guts)
        target->guts = min_guts;

    uint16_t diff = old_guts - target->guts;
    display_text_wait_addr(MSG_BTL_GUTS_DOWN, diff);
}

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
        display_in_battle_text_addr(MSG_BTL_NEMURI_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
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
        display_in_battle_text_addr(MSG_BTL_HEN_ON);
    } else {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
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
        MSG_BTL_INORU_6,   /* 0: subtle */
        MSG_BTL_INORU_2,   /* 1: warm */
        MSG_BTL_INORU_1,   /* 2: mysterious */
        MSG_BTL_INORU_7,   /* 3: golden */
        MSG_BTL_INORU_5,   /* 4: rockin */
        MSG_BTL_INORU_3,   /* 5: flash */
        MSG_BTL_INORU_8,   /* 6: rainbow */
        MSG_BTL_INORU_9,   /* 7: aroma */
        MSG_BTL_INORU_10,  /* 8: rending sound */
        MSG_BTL_INORU_4,   /* 9: defense down */
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
        display_text_from_snes_addr(MSG_BTL_EQUIP_NG_WEAPON);
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

    display_text_from_snes_addr(MSG_BTL_EQUIP_OK);

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
                    display_text_from_snes_addr(battle_action_table[5].description_text_pointer);
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
        display_text_from_snes_addr(battle_action_table[4].description_text_pointer);
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
        display_text_from_snes_addr(MSG_BTL_EQUIP_NG_WEAPON);
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

    display_text_from_snes_addr(MSG_BTL_EQUIP_OK);

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
        display_in_battle_text_addr(MSG_BTL_TONZURA_BREAK_IN_OK);
        ow.psi_teleport_style = 3;  /* TELEPORT_STYLE::INSTANT */
        ow.psi_teleport_destination = 15;
    } else {
        display_in_battle_text_addr(MSG_BTL_TONZURA_BREAK_IN_NG);
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
    display_text_with_prompt_addr(MSG_BTL_POO_BREAK_IN_2);

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
    display_text_with_prompt_addr(MSG_BTL_MECHPOKEY_1_TALK_B);
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
    display_text_with_prompt_addr(MSG_BTL_MECHPOKEY_2_TALK_2);
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
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT_PRAY_7_DOSEI);
    battle_wait(2 * FRAMES_PER_SECOND);
    play_sfx(SFX_PSI_STARSTORM);
    battle_wait(30);  /* HALF_OF_A_SECOND */
    bt.vertical_shake_duration = FRAMES_PER_SECOND;
    bt.vertical_shake_hold_duration = 12;  /* FIFTH_OF_A_SECOND */
    display_text_with_prompt_addr(MSG_BTL_INORU_DAMAGE_1);
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
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT_PRAY_2_TONZURA);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_1);
    bt.giygas_phase = GIYGAS_PRAYER_2_USED;
}

void btlact_giygas_prayer_3(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT_PRAY_3_PAULA_PAPA);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_2);
    bt.giygas_phase = GIYGAS_PRAYER_3_USED;
}

void btlact_giygas_prayer_4(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT_PRAY_4_TONY);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_3);
    bt.giygas_phase = GIYGAS_PRAYER_4_USED;
}

void btlact_giygas_prayer_5(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT_PRAY_5_RAMA);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_4);
    bt.giygas_phase = GIYGAS_PRAYER_5_USED;
}

void btlact_giygas_prayer_6(void) {
    display_battle_cutscene_text(ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1,
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT_PRAY_6_FRANK);
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
                                 MUSIC_GIYGAS_PHASE3, MSG_EVT_PRAY_1_NES_MAMA);
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
    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL_INORU_BACK_TO_PC_8);
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

    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL_INORU_BACK_TO_PC_9);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_7);

    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL_INORU_BACK_TO_PC_F_1);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_8);

    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL_INORU_BACK_TO_PC_F_2);
    giygas_hurt_prayer(GIYGAS_PRAYER_DAMAGE_9);

    play_giygas_weakened_sequence(MUSIC_GIYGAS_WEAKENED2, MSG_BTL_INORU_BACK_TO_PC_F_3);
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
    display_in_battle_text_addr(MSG_BTL_POKEY_RUN_AWAY);
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


const BattleActionEntry btlact_dispatch_table[] = {
    /* Sorted by ROM address for binary search */
    { 0xC1DE43, btlact_switch_weapons },
    { 0xC1E00F, btlact_switch_armor },
    { 0xC28523, (void(*)(void))battle_level_2_attack },
    { 0xC2859F, btlact_bash },
    { 0xC285DA, (void(*)(void))battle_level_4_attack },
    { 0xC28651, (void(*)(void))battle_level_3_attack },
    { 0xC286CB, btlact_level_1_attack },
    { 0xC28740, btlact_shoot },
    { 0xC28770, btlact_spy },
    { 0xC2889B, btlact_null },
    { 0xC2889E, btlact_steal },
    { 0xC288EB, btlact_freezetime },
    { 0xC289CE, btlact_diamondize },
    { 0xC28A92, btlact_paralyze },
    { 0xC28AEB, btlact_nauseate },
    { 0xC28B2C, btlact_poison },
    { 0xC28B6D, btlact_cold },
    { 0xC28BBE, btlact_mushroomize },
    { 0xC28BFD, btlact_possess },
    { 0xC28C69, btlact_crying },
    { 0xC28CB8, btlact_immobilize },
    { 0xC28CF1, btlact_solidify },
    { 0xC28D3A, redirect_btlact_brainshock_alpha },
    { 0xC28D5A, btlact_distract },
    { 0xC28DBB, btlact_feel_strange },
    { 0xC28DFC, btlact_crying2 },
    { 0xC28E3B, redirect_btlact_hypnosis_alpha },
    { 0xC28E42, btlact_reduce_pp },
    { 0xC28EAE, btlact_cut_guts },
    { 0xC28F21, btlact_reduce_offense_defense },
    { 0xC28F97, btlact_level_2_attack_poison },
    { 0xC28FF9, btlact_double_bash },
    { 0xC2900B, btlact_350_fire_damage },
    { 0xC2902C, (void(*)(void))battle_level_3_attack },  /* REDIRECT_BTLACT_LEVEL_3_ATK */
    { 0xC29033, btlact_null2 },
    { 0xC29036, btlact_null3 },
    { 0xC29039, btlact_null4 },
    { 0xC2903C, btlact_null5 },
    { 0xC2903F, btlact_null6 },
    { 0xC29042, btlact_null7 },
    { 0xC29045, btlact_null8 },
    { 0xC29048, btlact_null9 },
    { 0xC2904B, btlact_null10 },
    { 0xC2904E, btlact_null11 },
    { 0xC29051, btlact_neutralize },
    { 0xC290C6, apply_neutralize_to_all },
    { 0xC2916E, btlact_level_2_attack_diamondize },
    { 0xC29254, btlact_reduce_offense },
    { 0xC29298, btlact_clumsydeath },
    { 0xC292EB, btlact_enemy_extend },
    { 0xC292EE, btlact_masterbarfdeath },
    { 0xC29556, btlact_psi_rockin_alpha },
    { 0xC2955F, btlact_psi_rockin_beta },
    { 0xC29568, btlact_psi_rockin_gamma },
    { 0xC29571, btlact_psi_rockin_omega },
    { 0xC295AB, btlact_psi_fire_alpha },
    { 0xC295B4, btlact_psi_fire_beta },
    { 0xC295BD, btlact_psi_fire_gamma },
    { 0xC295C6, btlact_psi_fire_omega },
    { 0xC29647, btlact_psi_freeze_alpha },
    { 0xC29650, btlact_psi_freeze_beta },
    { 0xC29659, btlact_psi_freeze_gamma },
    { 0xC29662, btlact_psi_freeze_omega },
    { 0xC29871, btlact_psi_thunder_alpha },
    { 0xC2987D, btlact_psi_thunder_beta },
    { 0xC29889, btlact_psi_thunder_gamma },
    { 0xC29895, btlact_psi_thunder_omega },
    { 0xC29987, btlact_psi_flash_alpha },
    { 0xC299AE, btlact_psi_flash_beta },
    { 0xC299EF, btlact_psi_flash_gamma },
    { 0xC29A35, btlact_psi_flash_omega },
    { 0xC29AA6, btlact_psi_starstorm_alpha },
    { 0xC29AAF, btlact_psi_starstorm_omega },
    { 0xC29AC6, btlact_lifeup_alpha },
    { 0xC29ACF, btlact_lifeup_beta },
    { 0xC29AD8, btlact_lifeup_gamma },
    { 0xC29AE1, btlact_lifeup_omega },
    { 0xC29AEA, btlact_healing_alpha },
    { 0xC29B7A, btlact_healing_beta },
    { 0xC29C2C, btlact_healing_gamma },
    { 0xC29CB8, btlact_healing_omega },
    { 0xC29D44, btlact_shield_alpha },
    { 0xC29D7A, redirect_btlact_shield_alpha },
    { 0xC29D81, btlact_shield_beta },
    { 0xC29DB7, redirect_btlact_shield_beta },
    { 0xC29DBE, btlact_psi_shield_alpha },
    { 0xC29DF4, redirect_btlact_psi_shield_alpha },
    { 0xC29DFB, btlact_psi_shield_beta },
    { 0xC29E31, redirect_btlact_psi_shield_beta },
    { 0xC29E38, btlact_offense_up_alpha },
    { 0xC29E7F, redirect_btlact_offense_up_alpha },
    { 0xC29E86, btlact_defense_down_alpha },
    { 0xC29EFF, redirect_btlact_defense_down_alpha },
    { 0xC29F06, btlact_hypnosis_alpha },
    { 0xC29F57, redirect_btlact_hypnosis_a_copy },
    { 0xC29F5E, btlact_magnet_a },
    { 0xC29FE1, btlact_magnet_o },
    { 0xC29FFE, btlact_paralysis_alpha },
    { 0xC2A04F, redirect_btlact_paralysis_alpha },
    { 0xC2A056, btlact_brainshock_alpha },
    { 0xC2A0A7, redirect_btlact_brainshock_a_copy },
    { 0xC2A0AE, btlact_hp_recovery_1d4 },
    { 0xC2A0BF, btlact_hp_recovery_50 },
    { 0xC2A0CF, btlact_hp_recovery_200 },
    { 0xC2A0DF, btlact_pp_recovery_20 },
    { 0xC2A0EF, btlact_pp_recovery_80 },
    { 0xC2A0FF, btlact_iq_up_1d4 },
    { 0xC2A14B, btlact_guts_up_1d4 },
    { 0xC2A193, btlact_speed_up_1d4 },
    { 0xC2A1DB, btlact_vitality_up_1d4 },
    { 0xC2A227, btlact_luck_up_1d4 },
    { 0xC2A26F, btlact_hp_recovery_300 },
    { 0xC2A27F, btlact_random_stat_up_1d4 },
    { 0xC2A360, btlact_hp_recovery_10 },
    { 0xC2A370, btlact_hp_recovery_100 },
    { 0xC2A380, btlact_hp_recovery_10000 },
    { 0xC2A39D, btlact_heal_poison },
    { 0xC2A3D1, btlact_counter_psi },
    { 0xC2A422, btlact_shield_killer },
    { 0xC2A46B, (void(*)(void))btlact_hp_sucker },
    { 0xC2A507, btlact_hungry_hp_sucker },
    { 0xC2A50E, btlact_mummy_wrap },
    { 0xC2A5D1, btlact_bottle_rocket },
    { 0xC2A5DA, btlact_big_bottle_rocket },
    { 0xC2A5E3, btlact_multi_bottle_rocket },
    { 0xC2A5EC, btlact_handbag_strap },
    { 0xC2A818, btlact_bomb },
    { 0xC2A821, btlact_super_bomb },
    { 0xC2A82A, btlact_solidify_2 },
    { 0xC2A86B, btlact_yogurt_dispenser },
    { 0xC2A89D, btlact_snake },
    { 0xC2A902, btlact_inflict_solidification },
    { 0xC2A953, btlact_inflict_poison },
    { 0xC2A99C, btlact_bag_of_dragonite },
    { 0xC2AA0C, btlact_insecticide_spray },
    { 0xC2AA15, btlact_xterminator_spray },
    { 0xC2AA6D, btlact_rust_promoter },
    { 0xC2AA76, btlact_rust_promoter_dx },
    { 0xC2AA7F, btlact_sudden_guts_pill },
    { 0xC2AAC6, btlact_defense_spray },
    { 0xC2AB0D, btlact_defense_shower },
    { 0xC2AB71, (void(*)(void))btlact_teleport_box },
    { 0xC2AC2A, btlact_pray_subtle },
    { 0xC2AC3E, btlact_pray_warm },
    { 0xC2AC51, btlact_pray_golden },
    { 0xC2AC68, btlact_pray_mysterious },
    { 0xC2AC7B, btlact_pray_rainbow },
    { 0xC2AC99, btlact_pray_aroma },
    { 0xC2ACDA, btlact_pray_rending_sound },
    { 0xC2AD1B, btlact_pray },
    { 0xC2B0A1, (void(*)(void))btlact_mirror },
    { 0xC2B27D, btlact_eat_food },
    { 0xC2C13C, btlact_sow_seeds },
    { 0xC2C145, btlact_call_for_help },
    { 0xC2C14E, (void(*)(void))btlact_rainbow_of_colours },
    { 0xC2C1BD, btlact_fly_honey },
    { 0xC2C4C0, btlact_pokey_speech },
    { 0xC2C513, btlact_null12 },
    { 0xC2C516, btlact_pokey_speech_2 },
    { 0xC2C572, btlact_giygas_prayer_1 },
    { 0xC2C5D1, btlact_giygas_prayer_2 },
    { 0xC2C5FA, btlact_giygas_prayer_3 },
    { 0xC2C623, btlact_giygas_prayer_4 },
    { 0xC2C64C, btlact_giygas_prayer_5 },
    { 0xC2C675, btlact_giygas_prayer_6 },
    { 0xC2C69E, btlact_giygas_prayer_7 },
    { 0xC2C6D0, btlact_giygas_prayer_8 },
    { 0xC2C6F0, btlact_giygas_prayer_9 },
};

#define BTLACT_DISPATCH_COUNT (sizeof(btlact_dispatch_table) / sizeof(btlact_dispatch_table[0]))

void jump_temp_function_pointer(void) {
    /* Binary search the sorted dispatch table */
    int lo = 0, hi = (int)BTLACT_DISPATCH_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (btlact_dispatch_table[mid].rom_addr == bt.temp_function_pointer) {
            btlact_dispatch_table[mid].func();
            return;
        } else if (btlact_dispatch_table[mid].rom_addr < bt.temp_function_pointer) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    LOG_WARN("WARN: unknown battle action ROM addr $%06X\n", bt.temp_function_pointer);
}

