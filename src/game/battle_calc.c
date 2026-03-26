/*
 * Battle calculation functions.
 *
 * Extracted from battle.c — contains damage calculations, success checks,
 * dodge/miss/smash mechanics, stat modifiers, shield handling, and
 * physical attack level implementations.
 */
#include "game/battle.h"
#include "game/battle_internal.h"
#include "game/game_state.h"
#include "game/audio.h"
#include "game/display_text.h"
#include "game/inventory.h"
#include "game/text.h"
#include "game/window.h"

#include "data/battle_text_data.h"


/* ======================================================================
 * Combat calculations
 * ====================================================================== */

/*
 * SUCCESS_255 (asm/battle/success_255.asm)
 *
 * Returns 1 if random byte < threshold (unsigned comparison).
 * Probability = threshold / 256.
 */
uint16_t battle_success_255(uint16_t threshold) {
    uint8_t roll = rand_byte();
    return (roll < (uint8_t)threshold) ? 1 : 0;
}


/*
 * SUCCESS_500 (asm/battle/success_500.asm)
 *
 * Returns 1 if rand_limit(500) < threshold.
 * RAND_LIMIT(500) gives a value in [0, 499].
 */
uint16_t battle_success_500(uint16_t threshold) {
    uint16_t roll = rand_limit(500);
    return (roll < threshold) ? 1 : 0;
}


/*
 * SUCCESS_SPEED (asm/battle/success_speed.asm)
 *
 * Speed-modified random check.
 *
 * Algorithm:
 *   speed_diff = (target.speed * 2) - attacker.speed
 *   if speed_diff < attacker.speed (target not fast enough):
 *     adjusted_threshold = 0  (always fail)
 *   else:
 *     adjusted_threshold = speed_diff - attacker.speed
 *   roll = rand_limit(base_chance)
 *   return (roll >= adjusted_threshold) ? 1 : 0
 */
uint16_t battle_success_speed(uint16_t base_chance) {
    Battler *tgt = battler_from_offset(bt.current_target);
    Battler *atk = battler_from_offset(bt.current_attacker);

    uint16_t target_speed_2x = tgt->speed * 2;
    uint16_t attacker_speed = atk->speed;
    uint16_t adjusted_threshold;

    if (target_speed_2x < attacker_speed) {
        /* Attacker is faster — threshold = 0 (always succeeds) */
        adjusted_threshold = 0;
    } else {
        /* Target has speed advantage */
        adjusted_threshold = target_speed_2x - attacker_speed;
    }

    uint16_t roll = rand_limit(base_chance);
    return (roll >= adjusted_threshold) ? 1 : 0;
}


/*
 * DETERMINE_DODGE (asm/battle/determine_dodge.asm)
 *
 * Checks if the current target dodges the current attacker's attack.
 * Paralyzed, asleep, immobilized, and solidified targets cannot dodge.
 *
 * Speed formula: dodge_chance = target.speed*2 - attacker.speed
 * If negative (attacker is too fast), no dodge possible.
 * Otherwise, SUCCESS_500(dodge_chance) determines if dodge happens.
 */
uint16_t battle_determine_dodge(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    /* Status checks — disabled targets can't dodge */
    if (tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_PARALYZED)
        return 0;

    uint8_t temp_status = tgt->afflictions[STATUS_GROUP_TEMPORARY];
    if (temp_status == STATUS_2_ASLEEP ||
        temp_status == STATUS_2_IMMOBILIZED ||
        temp_status == STATUS_2_SOLIDIFIED)
        return 0;

    /* Speed-based dodge calculation */
    Battler *atk = battler_from_offset(bt.current_attacker);
    int16_t dodge_chance = (int16_t)(tgt->speed * 2) - (int16_t)atk->speed;

    /* If dodge_chance < 0 (using BRANCHGTS: 0 - dodge_chance > 0, i.e., dodge_chance < 0),
     * no dodge is possible */
    if (dodge_chance < 0)
        return 0;

    return battle_success_500((uint16_t)dodge_chance) ? 1 : 0;
}


/* ======================================================================
 * Damage variance
 * ====================================================================== */

/*
 * TWENTY_FIVE_PERCENT_VARIANCE (asm/battle/25_percent_variance.asm)
 *
 * Generates two random numbers in [-128, 127], picks the one with
 * smaller absolute value (triangular distribution toward center),
 * then applies half of that as a percentage of the input value.
 *
 * The effective range is roughly +/- 12.5% of the base value.
 */
uint16_t battle_25pct_variance(uint16_t value) {
    /* Get two random values centered at 0 */
    int16_t r1 = (int16_t)(rand_byte()) - 0x80;
    int16_t r2 = (int16_t)(rand_byte()) - 0x80;

    int16_t abs_r1 = (r1 < 0) ? -r1 : r1;
    int16_t abs_r2 = (r2 < 0) ? -r2 : r2;

    /* Pick the one closer to center (smaller absolute value) */
    int16_t chosen;
    int16_t chosen_abs;
    if (abs_r1 <= abs_r2) {
        chosen = r1;
        chosen_abs = abs_r1;
    } else {
        chosen = r2;
        chosen_abs = abs_r2;
    }

    /* Apply half the percentage offset to the value.
     * The assembly uses TRUNCATE_16_TO_8 which is (value * abs) >> 8,
     * then divides by 2 (LSR). */
    uint8_t adjustment = (uint8_t)(((uint16_t)value * chosen_abs) >> 8);
    adjustment >>= 1; /* Half for 25% variant (12.5% effective range) */

    if (chosen < 0) {
        /* Negative: subtract from value */
        if (adjustment > value)
            return 0;
        return value - adjustment;
    } else {
        /* Positive: add to value */
        return value + adjustment;
    }
}


/*
 * FIFTY_PERCENT_VARIANCE (asm/battle/50_percent_variance.asm)
 *
 * Same as 25% variance but without the /2 — full percentage offset.
 * Effective range is roughly +/- 25%.
 */
uint16_t battle_50pct_variance(uint16_t value) {
    int16_t r1 = (int16_t)(rand_byte()) - 0x80;
    int16_t r2 = (int16_t)(rand_byte()) - 0x80;

    int16_t abs_r1 = (r1 < 0) ? -r1 : r1;
    int16_t abs_r2 = (r2 < 0) ? -r2 : r2;

    int16_t chosen;
    int16_t chosen_abs;
    if (abs_r1 <= abs_r2) {
        chosen = r1;
        chosen_abs = abs_r1;
    } else {
        chosen = r2;
        chosen_abs = abs_r2;
    }

    /* No /2 for 50% variant — full adjustment */
    uint8_t adjustment = (uint8_t)(((uint16_t)value * chosen_abs) >> 8);

    if (chosen < 0) {
        if (adjustment > value)
            return 0;
        return value - adjustment;
    } else {
        return value + adjustment;
    }
}


/* ======================================================================
 * PSI resistance modifiers
 * ====================================================================== */

/*
 * CALC_PSI_DMG_MODIFIERS (asm/battle/calc_psi_damage_modifiers.asm)
 *
 * Returns damage multiplier (0-255) based on element resistance level.
 * Damage is scaled as: final_damage = base_damage * result / 255.
 */
uint8_t battle_calc_psi_dmg_modifier(uint8_t resist_level) {
    switch (resist_level) {
        case 0: return 255;  /* No resistance — full damage */
        case 1: return 179;  /* Low — ~70% */
        case 2: return 102;  /* Medium — ~40% */
        case 3: return 13;   /* High — ~5% */
        default: return 255;
    }
}


/*
 * CALC_PSI_RES_MODIFIERS (asm/battle/calc_psi_resistance_modifiers.asm)
 *
 * Returns success rate multiplier (0-255) for status PSI.
 */
uint8_t battle_calc_psi_res_modifier(uint8_t resist_level) {
    switch (resist_level) {
        case 0: return 255;  /* No resistance — always lands */
        case 1: return 128;  /* Low — ~50% */
        case 2: return 26;   /* Medium — ~10% */
        case 3: return 0;    /* High — immune */
        default: return 255;
    }
}


/* ======================================================================
 * Stat modification
 * ====================================================================== */

/*
 * INCREASE_OFFENSE_16TH (asm/battle/increase_offense_16th.asm)
 *
 * Increases target's offense by 1/16th (minimum 1).
 * Clamps to base_offense * 5 / 4 (125% of base).
 */
void battle_increase_offense(Battler *target) {
    uint16_t increment = target->offense >> 4;
    if (increment == 0) increment = 1;

    target->offense += increment;

    /* Clamp to 125% of base */
    uint16_t max_offense = ((uint16_t)target->base_offense * 5) >> 2;
    if (target->offense > max_offense)
        target->offense = max_offense;
}


/*
 * HEXADECIMATE_OFFENSE (asm/battle/decrease_offense_16th.asm)
 *
 * Decreases target's offense by 1/16th (minimum 1).
 * Clamps to base_offense * 3 / 4 (75% of base).
 */
void battle_decrease_offense(Battler *target) {
    uint16_t decrement = target->offense >> 4;
    if (decrement == 0) decrement = 1;

    target->offense -= decrement;

    /* Clamp to 75% of base */
    uint16_t min_offense = ((uint16_t)target->base_offense * 3) >> 2;
    if (target->offense < min_offense)
        target->offense = min_offense;
}


/*
 * INCREASE_DEFENSE_16TH (asm/battle/increase_defense_16th.asm)
 *
 * Increases target's defense by 1/16th (minimum 1).
 * Clamps to base_defense * 5 / 4 (125% of base).
 */
void battle_increase_defense(Battler *target) {
    uint16_t increment = target->defense >> 4;
    if (increment == 0) increment = 1;

    target->defense += increment;

    uint16_t max_defense = ((uint16_t)target->base_defense * 5) >> 2;
    if (target->defense > max_defense)
        target->defense = max_defense;
}


/*
 * HEXADECIMATE_DEFENSE (asm/battle/decrease_defense_16th.asm)
 *
 * Decreases target's defense by 1/16th (minimum 1).
 * Clamps to base_defense * 3 / 4 (75% of base).
 */
void battle_decrease_defense(Battler *target) {
    uint16_t decrement = target->defense >> 4;
    if (decrement == 0) decrement = 1;

    target->defense -= decrement;

    uint16_t min_defense = ((uint16_t)target->base_defense * 3) >> 2;
    if (target->defense < min_defense)
        target->defense = min_defense;
}


/* ======================================================================
 * PSI shield
 * ====================================================================== */

/*
 * PSI_SHIELD_NULLIFY (asm/battle/psi_shield_nullify.asm)
 *
 * Checks if the current target's PSI shield blocks or reflects the attack.
 * Only applies to PSI-type actions (ACTION_TYPE_PSI).
 *
 * PSI_SHIELD_POWER: reflects the attack (swaps attacker/target).
 * PSI_SHIELD: absorbs the attack, decrements shield_hp. When shield_hp
 *   reaches 0, the shield is removed.
 *
 * Returns 1 if attack was nullified (absorbed), 0 if it should proceed.
 * Note: reflection sets DAMAGE_IS_REFLECTED but returns 0 (attack proceeds
 * with swapped targets).
 */
uint16_t battle_psi_shield_nullify(void) {
    bt.shield_has_nullified_damage = 1;

    Battler *atk = battler_from_offset(bt.current_attacker);

    /* Set current item from action argument */
    set_current_item(atk->current_action_argument);

    /* Check if the current action is PSI type */
    /* Action type is at offset 2 in the 12-byte action entry */
    if (battle_action_table == NULL)
        return 0;
    uint16_t action_id = atk->current_action;
    uint8_t action_type = battle_action_table[action_id].type;

    if (action_type != ACTION_TYPE_PSI)
        return 0;

    /* Check target's shield status */
    Battler *tgt = battler_from_offset(bt.current_target);
    uint8_t shield = tgt->afflictions[STATUS_GROUP_SHIELD];

    if (shield == STATUS_6_PSI_SHIELD_POWER) {
        /* PSI Shield Power — reflect attack */
        display_in_battle_text_addr(MSG_BTL_PSYPOWER_TURN);
        bt.damage_is_reflected = 1;
        swap_attacker_with_target();
        return 0;  /* Attack proceeds with swapped targets */
    } else if (shield == STATUS_6_PSI_SHIELD) {
        /* PSI Shield — absorb attack */
        display_in_battle_text_addr(MSG_BTL_PSYCO_TURN);
        tgt->shield_hp--;
        if (tgt->shield_hp == 0) {
            tgt->afflictions[STATUS_GROUP_SHIELD] = 0;
            display_in_battle_text_addr(MSG_BTL_SHIELD_OFF);
        }
        return 1;  /* Attack nullified */
    }

    /* No PSI shield active */
    return 0;
}


/* ======================================================================
 * Action type lookup
 * ====================================================================== */

/*
 * GET_BATTLE_ACTION_TYPE (asm/battle/get_battle_action_type.asm)
 *
 * Looks up the action type (physical, PSI, item, etc.) for a given action ID
 * from the BATTLE_ACTION_TABLE.
 */
uint16_t battle_get_action_type(uint16_t action_id) {
    if (battle_action_table == NULL)
        return 0;
    return battle_action_table[action_id].type;
}


/* ======================================================================
 * Damage calculation
 * ====================================================================== */

/*
 * CALC_DAMAGE (asm/battle/calc_damage.asm)
 *
 * Core damage application function. Applies damage to target's HP,
 * handles special cases (Giygas redirect, guts save, final attack),
 * displays appropriate damage text, and triggers screen effects.
 *
 * Parameters:
 *   target_offset: byte offset into bt.battlers_table
 *   damage: amount of damage to deal
 *
 * Returns: 1 always
 */
uint16_t battle_calc_damage(uint16_t target_offset, uint16_t damage) {
    Battler *tgt = battler_from_offset(target_offset);
    uint16_t saved_giygas_redirect = 0;
    uint16_t saved_target = 0;

    if (damage == 0) {
        /* "It didn't work" text */
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return 0;
    }

    /* Giygas phase 2 redirect: redirect damage to random conscious party member */
    if (tgt->ally_or_enemy != 0 && tgt->id == ENEMY_GIYGAS_2) {
        saved_giygas_redirect = 1;
        saved_target = bt.current_target;

        /* Pick random conscious non-NPC non-unconscious non-diamondized party member */
        for (;;) {
            uint16_t r = rng_next_byte() & 0x03;
            uint16_t candidate_offset = (uint16_t)(r * sizeof(Battler));
            Battler *cand = battler_from_offset(candidate_offset);
            if (cand->consciousness == 0) continue;
            if (cand->npc_id != 0) continue;
            uint8_t aff = cand->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL];
            if (aff == STATUS_0_UNCONSCIOUS || aff == STATUS_0_DIAMONDIZED) continue;
            bt.current_target = candidate_offset;
            target_offset = candidate_offset;
            tgt = cand;
            break;
        }
        fix_target_name();
        bt.reflect_flash_duration = 0x10;
        play_sfx(73);  /* SFX::REFLECT_DAMAGE (assembly: calc_damage.asm line 66) */
        battle_wait(HALF_OF_A_SECOND);
    }

    /* Apply damage via REDUCE_HP (unless target is a special boss that takes no HP damage) */
    bool skip_hp_reduction = false;
    if (tgt->ally_or_enemy != 0) {
        uint16_t eid = tgt->id;
        if (eid == ENEMY_MASTER_BELCH_1 || eid == ENEMY_MASTER_BELCH_3 ||
            eid == ENEMY_GIYGAS_2 || eid == ENEMY_GIYGAS_3 ||
            eid == ENEMY_GIYGAS_5 || eid == ENEMY_GIYGAS_6) {
            skip_hp_reduction = true;
        }
    }
    /* Assembly (calc_damage.asm:70-103): save hp_target BEFORE reduce_hp for guts check */
    uint16_t saved_hp = tgt->hp_target;
    if (!skip_hp_reduction) {
        battle_reduce_hp(tgt, damage);
    }

    /* Guts save: if player character's hp_target hit 0 and had >1 HP before,
     * guts stat gives a chance to survive with 1 HP.
     * Assembly (calc_damage.asm:70-103): saves hp_target BEFORE reduce_hp. */
    if (tgt->ally_or_enemy == 0) {
        if (tgt->hp_target == 0) {
            if (saved_hp > 1) {
                Battler *atk = battler_from_offset(bt.current_attacker);
                uint16_t guts = atk->guts;
                if (guts < GUTS_FLOOR_FOR_SMAAAASH_CHANCE)
                    guts = GUTS_FLOOR_FOR_SMAAAASH_CHANCE;
                if (battle_success_500(guts)) {
                    battle_set_hp(tgt, 1);
                }
            }
        }

        /* During enemy final attacks: if only 1 enemy and 1 party member left,
         * ensure party member survives with 1 HP */
        if (bt.enemy_performing_final_attack) {
            if (battle_count_chars(1) == 1 && battle_count_chars(0) == 1) {
                battle_set_hp(tgt, 1);
            }
        }
    }

    /* Display damage text and screen effects */
    if (tgt->ally_or_enemy != 0) {
        /* Enemy target: set hit animation frame, display damage text */
        tgt->blink_timer = 0x15;

        if (bt.is_smaaaash_attack) {
            display_text_wait_addr(MSG_BTL_DAMAGE_SMASH_M, damage);
            bt.is_smaaaash_attack = 0;
        } else {
            display_text_wait_addr(MSG_BTL_DAMAGE_M, damage);
        }

        /* Giygas 3/4/5/6: green background flash on damage */
        uint16_t eid = tgt->id;
        if (eid == ENEMY_GIYGAS_3 || eid == ENEMY_GIYGAS_4 ||
            eid == ENEMY_GIYGAS_5 || eid == ENEMY_GIYGAS_6) {
            bt.green_background_flash_duration = 0x10;
        }
    } else {
        /* Party member target */
        if (tgt->npc_id == 0) {
            /* Player character: set HP/PP box blink */
            if (bt.hp_pp_box_blink_duration == 0) {
                bt.hp_pp_box_blink_duration = 0x15;

                /* Find which party slot this character occupies */
                for (uint16_t i = 0; i < TOTAL_PARTY_COUNT; i++) {
                    if ((game_state.party_members[i] & 0xFF) == (tgt->id & 0xFF)) {
                        bt.hp_pp_box_blink_target = i;
                        break;
                    }
                }
            }
        }

        if (tgt->hp_target == 0) {
            /* Mortal blow: stronger screen shake */
            bt.vertical_shake_duration = 8; /* DAMAGE_TAKEN_SCREEN_SHAKE_DURATION_MORTAL */
            bt.vertical_shake_hold_duration = 16; /* DAMAGE_TAKEN_SCREEN_SHAKE_DURATION_MORTAL_HOLD */
            display_text_wait_addr(MSG_BTL_DAMAGE_TO_DEATH, damage);
        } else if (bt.is_smaaaash_attack) {
            bt.vertical_shake_duration = 4; /* DAMAGE_TAKEN_SCREEN_SHAKE_DURATION_REGULAR */
            display_text_wait_addr(MSG_BTL_DAMAGE_SMASH, damage);
            bt.vertical_shake_hold_duration = 0;
            bt.is_smaaaash_attack = 0;
        } else {
            bt.vertical_shake_duration = 4; /* DAMAGE_TAKEN_SCREEN_SHAKE_DURATION_NORMAL */
            display_text_wait_addr(MSG_BTL_DAMAGE, damage);
            bt.vertical_shake_hold_duration = 0;
        }

        bt.screen_effect_minimum_wait_frames = 0x28;
    }

    /* Restore Giygas redirect state */
    if (saved_giygas_redirect) {
        bt.current_target = saved_target;
        fix_target_name();
    }

    return 1;
}


/* ======================================================================
 * Resist damage (guard, shield, reflection)
 * ====================================================================== */

/*
 * CALC_RESIST_DAMAGE (asm/battle/calc_damage_reduction.asm)
 *
 * Main damage application pipeline with resistance modifiers:
 *   1. Floor negative damage to 0
 *   2. Apply resistance modifier: damage = (damage * resist) / 256
 *   3. Check target is alive and conscious
 *   4. Halve damage if target is guarding (physical attacks only)
 *   5. Halve damage if target has physical shield (SHIELD/SHIELD_POWER)
 *   6. Floor damage to 1
 *   7. Apply damage via CALC_DAMAGE, check for KO
 *   8. Handle SHIELD_POWER reflection (half damage back to attacker)
 *   9. Weaken shield (decrement shield_hp, remove if 0)
 *  10. Check sleep wake chance
 *
 * Parameters:
 *   damage: raw damage amount (may be negative for heals — floored to 0)
 *   resist_modifier: 0-255 resistance factor (255 = full damage)
 *
 * Returns: final damage dealt (after all modifiers)
 */
uint16_t battle_calc_resist_damage(uint16_t damage, uint16_t resist_modifier) {
    /* Floor negative values to 0 (assembly uses signed check) */
    if ((int16_t)damage < 0)
        damage = 0;

    /* Apply resistance modifier if not 0xFF (full damage) */
    if (resist_modifier < 0xFF) {
        damage = (uint16_t)(((uint32_t)damage * resist_modifier) >> 8);
    }

    /* Check target is conscious and alive */
    Battler *tgt = battler_from_offset(bt.current_target);
    if (tgt->consciousness != 1)
        goto done;
    if (tgt->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS)
        goto done;

    /* Halve damage if guarding (physical attacks only) */
    if (tgt->guarding == 1) {
        Battler *atk = battler_from_offset(bt.current_attacker);
        uint16_t action_type = battle_get_action_type(atk->current_action);
        if (action_type == ACTION_TYPE_PHYSICAL) {
            damage >>= 1;  /* ASR16 with Y=1 */
        }
    }

    /* Halve damage if target has physical shield (physical attacks only) */
    {
        Battler *atk = battler_from_offset(bt.current_attacker);
        uint16_t action_type = battle_get_action_type(atk->current_action);
        if (action_type == ACTION_TYPE_PHYSICAL) {
            uint8_t shield = tgt->afflictions[STATUS_GROUP_SHIELD];
            if (shield == STATUS_6_SHIELD_POWER || shield == STATUS_6_SHIELD) {
                damage >>= 1;  /* ASR16 with Y=1 */
            }
        }
    }

    /* Floor damage to minimum 1 */
    if (damage == 0)
        damage = 1;

    /* Apply damage */
    battle_calc_damage(bt.current_target, damage);

    /* Check for KO */
    if (damage != 0 && tgt->hp == 0) {
        battle_ko_target(tgt);
    }

    /* Floor damage again after KO processing */
    if (damage == 0)
        damage = 1;

    /* Shield handling */
    if (bt.shield_has_nullified_damage)
        goto shields_done;

    {
        uint8_t shield = tgt->afflictions[STATUS_GROUP_SHIELD];

        if (shield == STATUS_6_SHIELD_POWER) {
            /* SHIELD_POWER: reflect half damage back to attacker */
            if (!bt.enemy_performing_final_attack) {
                uint16_t reflected = damage >> 1;
                if (reflected == 0) reflected = 1;

                display_in_battle_text_addr(MSG_BTL_POWER_TURN);
                swap_attacker_with_target();
                battle_calc_damage(bt.current_target, reflected);

                /* Check if reflected damage KO'd the original attacker */
                Battler *reflected_tgt = battler_from_offset(bt.current_target);
                if (reflected_tgt->hp == 0) {
                    battle_ko_target(reflected_tgt);
                }
                swap_attacker_with_target();
            }
            /* Fall through to weaken shield */
        } else if (shield == STATUS_6_SHIELD) {
            /* Fall through to weaken shield */
        } else {
            goto shields_done;
        }

        /* Weaken shield: decrement shield_hp, remove if depleted */
        tgt->shield_hp--;
        if (tgt->shield_hp == 0) {
            tgt->afflictions[STATUS_GROUP_SHIELD] = 0;
            display_in_battle_text_addr(MSG_BTL_SHIELD_OFF);
        }
    }

shields_done:
    /* Sleep wake check: if target is a non-NPC party member who is still alive,
     * check if being attacked wakes them from sleep */
    if (tgt->ally_or_enemy == 0 && tgt->npc_id == 0) {
        /* Check if player character's rolling HP reached 0 (they're dying) */
        uint8_t char_row = tgt->row;
        if (party_characters[char_row].current_hp == 0)
            goto done;
    }

    if (tgt->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_ASLEEP) {
        if (battle_success_255(CHANCE_OF_WAKING_UP_WHEN_ATTACKED)) {
            tgt->current_action = 0;
            tgt->afflictions[STATUS_GROUP_TEMPORARY] = 0;
            display_in_battle_text_addr(MSG_BTL_NEMURI_OFF);
        }
    }

done:
    return damage;
}


/* ======================================================================
 * Miss calculation
 * ====================================================================== */

/*
 * MISS_CALC (asm/battle/miss_calc.asm)
 *
 * Determines if the current attacker's attack misses.
 *
 * For player characters: reads weapon's miss rate from item configuration table.
 *   - No weapon equipped → miss_chance = 1 (very low)
 *   - Weapon equipped → reads item.params.special, adjusts by subtracting 0x80
 *     and flipping high bits.
 *
 * For enemies/NPCs: reads miss_rate from enemy configuration table.
 *
 * Crying or nauseous attackers get +8 to miss chance (out of 16).
 *
 * Final check: RAND_LIMIT(16) < miss_chance → miss
 *
 * Parameters:
 *   miss_message_type: 0 = physical miss text, 1 = gun/shoot miss text
 *
 * Returns: 1 if attack missed, 0 if hit
 */
uint16_t battle_miss_calc(uint16_t miss_message_type) {
    Battler *atk = battler_from_offset(bt.current_attacker);
    uint16_t miss_chance;

    if (atk->ally_or_enemy == 0 && atk->npc_id == 0) {
        /* Player character — look up weapon miss rate */
        uint8_t char_row = atk->row;
        uint8_t weapon_slot = party_characters[char_row].equipment[EQUIP_WEAPON];

        if (weapon_slot != 0) {
            /* Get the actual item ID from inventory */
            uint8_t item_id = party_characters[char_row].items[weapon_slot - 1];

            /* Look up item's special parameter (miss rate) */
            const ItemConfig *entry = get_item_entry(item_id);
            if (entry != NULL) {
                uint8_t special = entry->params[ITEM_PARAM_SPECIAL];
                /* Assembly: SEC; AND #$00FF; SBC #$0080; EOR #$FF80 */
                /* This converts the 0-255 special byte to a signed miss rate */
                int16_t signed_val = (int16_t)(uint16_t)special - 0x80;
                miss_chance = (uint16_t)(signed_val ^ (int16_t)0xFF80);
            } else {
                miss_chance = 1;
            }
        } else {
            /* No weapon — default miss chance of 1/16 */
            miss_chance = 1;
        }

        /* Crying or nauseous: +8 miss chance (50% extra on 16 scale) */
        if (atk->afflictions[STATUS_GROUP_TEMPORARY] == STATUS_2_CRYING ||
            atk->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_NAUSEOUS) {
            miss_chance += 8;
        }
    } else {
        /* Enemy or NPC — read miss_rate from enemy config table */
        if (enemy_config_table != NULL) {
            miss_chance = enemy_config_table[atk->id].miss_rate;
        } else {
            miss_chance = 0;
        }
    }

    /* Roll for miss */
    if (miss_chance == 0)
        return 0;

    uint16_t roll = rand_limit(16);
    if ((miss_chance - 1) >= roll) {
        /* Miss! Display appropriate text */
        if (miss_message_type != 0) {
            display_in_battle_text_addr(MSG_BTL_KARABURI_UTSU); /* gun miss */
        } else {
            display_in_battle_text_addr(MSG_BTL_KARABURI); /* physical miss */
        }
        return 1;
    }

    return 0;
}


/* ======================================================================
 * SMAAAASH (critical hit)
 * ====================================================================== */

/*
 * SMAAAASH (asm/battle/smaaaash.asm)
 *
 * Checks for a critical hit. Player characters have a minimum guts floor
 * of GUTS_FLOOR_FOR_SMAAAASH_CHANCE (25). The guts value is tested against
 * SUCCESS_500.
 *
 * On success:
 *   - Sets screen flash (green for player, red for enemy)
 *   - Weakens target's shield to 1 HP
 *   - Sets IS_SMAAAAASH_ATTACK flag
 *   - Deals offense*4 - defense damage via CALC_RESIST_DAMAGE
 *
 * Returns: 1 if SMAAAASH, 0 if not
 */
uint16_t battle_smaaaash(void) {
    bt.is_smaaaash_attack = 0;

    Battler *atk = battler_from_offset(bt.current_attacker);
    uint16_t guts = atk->guts;

    /* Player characters get minimum guts floor */
    if (atk->ally_or_enemy == 0) {
        if (guts < GUTS_FLOOR_FOR_SMAAAASH_CHANCE)
            guts = GUTS_FLOOR_FOR_SMAAAASH_CHANCE;
    }

    if (!battle_success_500(guts))
        return 0;

    /* SMAAAASH! */
    if (atk->ally_or_enemy == 0) {
        bt.green_flash_duration = SMAAAASH_FLASH_DURATION;
        display_in_battle_text_addr(MSG_BTL_SMASH_PLAYER);
    } else {
        bt.red_flash_duration = SMAAAASH_FLASH_DURATION;
        display_in_battle_text_addr(MSG_BTL_SMASH_MONSTER);
    }

    /* Weaken target's shield to 1 HP if they have one */
    Battler *tgt = battler_from_offset(bt.current_target);
    uint8_t shield = tgt->afflictions[STATUS_GROUP_SHIELD];
    if (shield == STATUS_6_SHIELD_POWER || shield == STATUS_6_SHIELD) {
        tgt->shield_hp = 1;
    }

    bt.is_smaaaash_attack = 1;

    /* Calculate SMAAAASH damage: offense * 4 - defense */
    uint16_t smash_damage = (atk->offense * 4) - tgt->defense;
    battle_calc_resist_damage(smash_damage, 0xFF);

    return 1;
}


/* ======================================================================
 * Heal strangeness
 * ====================================================================== */

/*
 * HEAL_STRANGENESS (asm/battle/heal_strangeness.asm)
 *
 * If the current target has STATUS_3_STRANGE, clear it and display message.
 */
void battle_heal_strangeness(void) {
    Battler *tgt = battler_from_offset(bt.current_target);

    if (tgt->afflictions[STATUS_GROUP_STRANGENESS] == STATUS_3_STRANGE) {
        tgt->afflictions[STATUS_GROUP_STRANGENESS] = 0;
        display_in_battle_text_addr(MSG_BTL_HEN_OFF);
    }
}


/* ======================================================================
 * Weaken shield (post-PSI reflection)
 * ====================================================================== */

/*
 * WEAKEN_SHIELD (asm/battle/weaken_shield.asm)
 *
 * Called after PSI damage with potential reflection.
 * If damage was reflected (DAMAGE_IS_REFLECTED set), swaps targets and
 * decrements the reflector's shield_hp. If shield_hp reaches 0, removes shield.
 */
void battle_weaken_shield(void) {
    bt.shield_has_nullified_damage = 0;

    if (!bt.damage_is_reflected)
        return;

    /* Swap so we're operating on the reflector's shield */
    swap_attacker_with_target();

    Battler *tgt = battler_from_offset(bt.current_target);
    tgt->shield_hp--;
    if (tgt->shield_hp == 0) {
        tgt->afflictions[STATUS_GROUP_SHIELD] = 0;
        display_in_battle_text_addr(MSG_BTL_SHIELD_OFF);
    }

    bt.damage_is_reflected = 0;
}


/* ======================================================================
 * Shield application
 * ====================================================================== */

/*
 * SHIELDS_COMMON (asm/battle/actions/shield_common.asm)
 *
 * Applies or refreshes a shield on a battler.
 *
 * If the target already has the same shield type:
 *   - Increment shield_hp by 3, clamp to max 8
 *   - Return 1 (existing shield refreshed)
 *
 * If different or no shield:
 *   - Set shield type and shield_hp = 3
 *   - Return 0 (new shield applied)
 *
 * Parameters:
 *   target: battler to apply shield to (byte offset)
 *   shield_type: STATUS_6_SHIELD, STATUS_6_SHIELD_POWER, etc.
 *
 * Note: Assembly returns 1 for "already has this shield" and 0 for "new shield",
 * which is inverted from what you might expect. The callers use this to pick
 * which text message to display.
 */
uint16_t battle_shields_common(Battler *target, uint16_t shield_type) {
    if (target->afflictions[STATUS_GROUP_SHIELD] == (uint8_t)shield_type) {
        /* Same shield already active — refresh by adding 3, cap at 8 */
        target->shield_hp += 3;
        if (target->shield_hp >= 9)
            target->shield_hp = 8;
        return 1;
    }

    /* New shield type (or no shield) — apply fresh */
    target->afflictions[STATUS_GROUP_SHIELD] = (uint8_t)shield_type;
    target->shield_hp = 3;
    return 0;
}


/* ======================================================================
 * Attack damage calculations
 * ====================================================================== */

/*
 * BTLACT_LEVEL_1_ATK (asm/battle/actions/level_1_attack.asm)
 *
 * Standard physical attack: damage = offense - defense.
 * Applies 25% variance, floor of 1, then CALC_RESIST_DAMAGE.
 */
void battle_level_1_attack(void) {
    Battler *atk = battler_from_offset(bt.current_attacker);
    Battler *tgt = battler_from_offset(bt.current_target);

    int16_t raw_damage = (int16_t)atk->offense - (int16_t)tgt->defense;

    /* Only apply variance if damage > 1 (assembly: CLC; SBC #0; BRANCHLTEQS skips when <= 1)
     * BRANCHLTEQS @APPLY_VARIANCE fires when (raw_damage - 1) <= 0, i.e., raw_damage <= 1.
     * So variance is applied only when raw_damage > 1 (raw_damage >= 2). */
    uint16_t damage;
    if (raw_damage > 1) {
        damage = battle_25pct_variance((uint16_t)raw_damage);
    } else {
        damage = (uint16_t)raw_damage;
    }

    /* Floor damage to 1 (assembly checks CLC; SBC #0; BRANCHGTS → if <= 0, set to 1) */
    if ((int16_t)damage <= 0)
        damage = 1;

    battle_calc_resist_damage(damage, 0xFF);
}


/*
 * BTLACT_LEVEL_2_ATK (asm/battle/actions/level_2_attack.asm)
 *
 * Doubled physical attack: damage = offense*2 - defense.
 * Applies 25% variance, floor of 1, then CALC_RESIST_DAMAGE.
 */
void battle_level_2_attack(void) {
    Battler *atk = battler_from_offset(bt.current_attacker);
    Battler *tgt = battler_from_offset(bt.current_target);

    int16_t raw_damage = (int16_t)(atk->offense * 2) - (int16_t)tgt->defense;

    /* Only apply variance when raw_damage > 1 (assembly: BRANCHLTEQS skips when <= 1) */
    uint16_t damage;
    if (raw_damage > 1) {
        damage = battle_25pct_variance((uint16_t)raw_damage);
    } else {
        damage = (uint16_t)raw_damage;
    }

    if ((int16_t)damage <= 0)
        damage = 1;

    battle_calc_resist_damage(damage, 0xFF);
}


/*
 * SUCCESS_LUCK40 (asm/battle/success_luck40.asm)
 *
 * Random check: generates rand(40) and compares to target's luck.
 * Returns 1 if rand_result >= luck (success), 0 otherwise.
 * Assembly: RAND_LIMIT(40), CMP battler::luck, BCS @SUCCESS
 */
uint16_t battle_success_luck40(void) {
    uint16_t r = rand_limit(40);
    Battler *tgt = battler_from_offset(bt.current_target);
    return (r >= tgt->luck) ? 1 : 0;
}


/*
 * SUCCESS_LUCK80 (asm/battle/success_luck80.asm)
 *
 * Same as SUCCESS_LUCK40 but with range 80.
 */
uint16_t battle_success_luck80(void) {
    uint16_t r = rand_limit(80);
    Battler *tgt = battler_from_offset(bt.current_target);
    return (r >= tgt->luck) ? 1 : 0;
}


/*
 * FAIL_ATTACK_ON_NPCS (asm/battle/fail_attack_on_npcs.asm)
 *
 * If current target is an NPC (npc_id != 0), displays "no effect" message
 * and returns 1. Otherwise returns 0.
 */
uint16_t battle_fail_attack_on_npcs(void) {
    Battler *tgt = battler_from_offset(bt.current_target);
    if (tgt->npc_id != 0) {
        display_in_battle_text_addr(MSG_BTL_KIKANAI);
        return 1;
    }
    return 0;
}


/*
 * LOSE_HP_STATUS (asm/battle/lose_hp_status.asm)
 *
 * Reduce target's HP by amount (from status effects like poison/sunstroke).
 * Floors at 0, applies via SET_HP.
 * Parameters: A = target offset, X = amount
 */
void battle_lose_hp_status(Battler *target, uint16_t amount) {
    uint16_t hp = target->hp_target;
    uint16_t new_hp;
    if (amount >= hp)
        new_hp = 0;
    else
        new_hp = hp - amount;
    battle_set_hp(target, new_hp);
}


/*
 * GET_SHIELD_TARGETTING (asm/battle/get_shield_targetting.asm)
 *
 * Returns 1 for single-target shield actions (sigma/omega variants),
 * 0 for multi-target (alpha/beta).
 */
uint16_t battle_get_shield_targeting(uint16_t action) {
    if (action == BATTLE_ACTION_PSI_SHIELD_SIGMA ||
        action == BATTLE_ACTION_PSI_SHIELD_OMEGA ||
        action == BATTLE_ACTION_PSI_PSI_SHIELD_SIGMA ||
        action == BATTLE_ACTION_PSI_PSI_SHIELD_OMEGA)
        return 1;
    return 0;
}


/* ======================================================================
 * Physical attack levels 3 and 4
 * ====================================================================== */

/*
 * BTLACT_LEVEL_3_ATK (asm/battle/actions/level_3_attack.asm)
 *
 * Triple physical attack: damage = offense*3 - defense, 25% variance.
 * Miss → SMAAAASH → dodge → damage → heal strangeness.
 */
void battle_level_3_attack(void) {
    if (battle_miss_calc(0))
        return;
    if (battle_smaaaash())
        return;
    if (battle_determine_dodge()) {
        display_in_battle_text_addr(MSG_BTL_TATAKU_YOKETA);
        return;
    }

    Battler *atk = battler_from_offset(bt.current_attacker);
    Battler *tgt = battler_from_offset(bt.current_target);

    int16_t raw_damage = (int16_t)(atk->offense * 3) - (int16_t)tgt->defense;

    uint16_t damage;
    if (raw_damage > 0) {
        damage = battle_25pct_variance((uint16_t)raw_damage);
    } else {
        damage = (uint16_t)raw_damage;
    }

    if ((int16_t)damage <= 0)
        damage = 1;

    battle_calc_resist_damage(damage, 0xFF);
    battle_heal_strangeness();
}


/*
 * BTLACT_LEVEL_4_ATK (asm/battle/actions/level_4_attack.asm)
 *
 * Quadruple physical attack: damage = offense*4 - defense, 25% variance.
 */
void battle_level_4_attack(void) {
    if (battle_miss_calc(0))
        return;
    if (battle_smaaaash())
        return;
    if (battle_determine_dodge()) {
        display_in_battle_text_addr(MSG_BTL_TATAKU_YOKETA);
        return;
    }

    Battler *atk = battler_from_offset(bt.current_attacker);
    Battler *tgt = battler_from_offset(bt.current_target);

    int16_t raw_damage = (int16_t)(atk->offense * 4) - (int16_t)tgt->defense;

    uint16_t damage;
    if (raw_damage > 0) {
        damage = battle_25pct_variance((uint16_t)raw_damage);
    } else {
        damage = (uint16_t)raw_damage;
    }

    if ((int16_t)damage <= 0)
        damage = 1;

    battle_calc_resist_damage(damage, 0xFF);
    battle_heal_strangeness();
}


/* ======================================================================
 * Character stat recalculation
 * ====================================================================== */

/*
 * RECALC_CHARACTER_MISS_RATE (asm/battle/recalc_character_miss_rate.asm)
 *
 * Reads the equipped weapon's "special" parameter byte from the item config
 * table and stores it as the character's miss_rate. If no weapon is equipped,
 * miss_rate is set to 0.
 * character_id is 1-based (NESS=1, PAULA=2, JEFF=3, POO=4).
 */
void recalc_character_miss_rate(uint16_t character_id) {
    CharStruct *ch = &party_characters[character_id - 1];
    uint8_t weapon_slot = ch->equipment[EQUIP_WEAPON];
    if (weapon_slot == 0) {
        ch->miss_rate = 0;
        return;
    }
    uint8_t item_id = ch->items[weapon_slot - 1];
    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry) {
        ch->miss_rate = 0;
        return;
    }
    /* The assembly does (byte - 0x80) ^ 0xFF80 which is identity for the low byte */
    ch->miss_rate = entry->params[ITEM_PARAM_SPECIAL];
}

