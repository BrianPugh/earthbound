#include "game/inventory.h"
#include "include/binary.h"
#include "game/game_state.h"
#include "game/battle.h"
#include "game/display_text.h"
#include "game/overworld.h"
#include "game/audio.h"
#include "entity/entity.h"
#include "data/assets.h"
#include "core/log.h"
#include "data/battle_text_data.h"
#include "include/constants.h"
#include <stdio.h>
#include <stdlib.h>

/* Forward declarations for party management (defined later in this file,
 * exported via inventory.h for CC_1F_11/12 in display_text.c) */

/* --- Item configuration table (loaded from ROM data) --- */

static const uint8_t *item_config_data = NULL;
static size_t item_config_size = 0;

bool ensure_item_config(void) {
    if (item_config_data) return true;
    item_config_size = ASSET_SIZE(ASSET_DATA_ITEM_CONFIGURATION_TABLE_BIN);
    item_config_data = ASSET_DATA(ASSET_DATA_ITEM_CONFIGURATION_TABLE_BIN);
    if (!item_config_data) {
        fprintf(stderr, "inventory: failed to load item_configuration_table.bin\n");
        return false;
    }
    return true;
}

const ItemConfig *get_item_entry(uint16_t item_id) {
    if (!ensure_item_config()) return NULL;
    if (item_id >= ITEM_CONFIG_COUNT) return NULL;
    size_t offset = (size_t)item_id * sizeof(ItemConfig);
    if (offset + sizeof(ItemConfig) > item_config_size) return NULL;
    return (const ItemConfig *)(item_config_data + offset);
}

/* --- Internal helpers --- */

/* Read a signed parameter byte from item configuration table.
 * Matches assembly pattern: SEC; AND #$00FF; SBC #$0080; EOR #$FF80
 * which sign-extends a byte to int16_t.
 * item_id: 1-based (direct inventory value), param_offset: byte offset within params. */
static int16_t get_item_param_signed(uint16_t item_id, int param_offset) {
    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry) return 0;
    return (int16_t)(int8_t)entry->params[param_offset];
}

/* Get a signed equipment stat modifier for a character.
 * char_idx: 0-indexed. equipment_slot: EQUIP_WEAPON..EQUIP_OTHER.
 * param_offset: ITEM_PARAM_STRENGTH..ITEM_PARAM_SPECIAL.
 * For Poo (char_idx==CHARACTER_POO) when reading strength, adds +1 to read epi instead. */
static int16_t get_equipment_strength_modifier(uint16_t char_idx, int equipment_slot, int poo_offset) {
    uint8_t equip_val = party_characters[char_idx].equipment[equipment_slot];
    if (equip_val == 0) return 0;
    uint8_t item_id = party_characters[char_idx].items[equip_val - 1];
    return get_item_param_signed(item_id, ITEM_PARAM_STRENGTH + poo_offset);
}

/* Get a signed ep modifier from an equipment slot (used by speed, guts, luck).
 * No Poo special handling — everyone reads ep the same way. */
static int16_t get_equipment_ep_modifier(uint16_t char_idx, int equipment_slot) {
    uint8_t equip_val = party_characters[char_idx].equipment[equipment_slot];
    if (equip_val == 0) return 0;
    uint8_t item_id = party_characters[char_idx].items[equip_val - 1];
    return get_item_param_signed(item_id, ITEM_PARAM_EP);
}

/* Clamp with dual bounds (lower and upper), matching offense/defense assembly.
 * Assembly uses CLC;SBC (A-M-1) with BRANCHLTEQS.
 * Edge cases faithfully preserved: total=-1 → 255, total=256 → 0. */
static uint8_t clamp_stat_dual(int16_t total) {
    /* Assembly check 1: 0 - total - 1 > 0 (signed), i.e. total <= -2 → clamp to 0 */
    if (total <= -2) return 0;
    /* Assembly check 2: total - 255 - 1 > 0 (signed), i.e. total >= 257 → clamp to 255 */
    if (total >= 257) return 255;
    /* In range -1 to 256: return low byte (assembly AND #$00FF) */
    return (uint8_t)(total & 0xFF);
}

/* Clamp with lower bound only (no upper clamp), matching guts/speed/luck assembly.
 * total <= -2 → 0, otherwise total & 0xFF. */
static uint8_t clamp_stat_lower(int16_t total) {
    if (total <= -2) return 0;
    return (uint8_t)(total & 0xFF);
}

/* --- Stat recalculation functions --- */

/* RECALC_CHARACTER_POSTMATH_OFFENSE: Port of asm/misc/recalc_character_postmath_offense.asm.
 * Reads base_offense, adds weapon strength modifier (epi for Poo), clamps 0-255.
 * char_id: 1-indexed. */
void recalc_character_postmath_offense(uint16_t char_id) {
    uint16_t idx = char_id - 1;
    int16_t total = party_characters[idx].base_offense;

    uint8_t weapon_slot = party_characters[idx].equipment[EQUIP_WEAPON];
    if (weapon_slot != 0) {
        int poo_offset = (idx == CHARACTER_POO) ? 1 : 0;
        uint8_t item_id = party_characters[idx].items[weapon_slot - 1];
        total += get_item_param_signed(item_id, ITEM_PARAM_STRENGTH + poo_offset);
    }

    party_characters[idx].offense = clamp_stat_dual(total);
}

/* RECALC_CHARACTER_POSTMATH_GUTS: Port of asm/misc/recalc_character_postmath_guts.asm.
 * Reads base_guts, adds weapon ep modifier, adds boosted_guts, clamps (lower only).
 * char_id: 1-indexed. */
void recalc_character_postmath_guts(uint16_t char_id) {
    uint16_t idx = char_id - 1;
    int16_t total = party_characters[idx].base_guts;

    uint8_t weapon_slot = party_characters[idx].equipment[EQUIP_WEAPON];
    if (weapon_slot != 0) {
        uint8_t item_id = party_characters[idx].items[weapon_slot - 1];
        total += get_item_param_signed(item_id, ITEM_PARAM_EP);
    }

    /* Add boosted_guts (assembly: PARTY_CHARACTERS+char_struct::boosted_guts) */
    total += party_characters[idx].boosted_guts;

    party_characters[idx].guts = clamp_stat_lower(total);
}

/* RECALC_CHARACTER_POSTMATH_DEFENSE: Port of asm/misc/recalc_character_postmath_defense.asm.
 * Reads base_defense, adds strength modifiers from BODY, ARMS, and OTHER slots
 * (epi for Poo), clamps 0-255 with dual bounds.
 * char_id: 1-indexed. */
void recalc_character_postmath_defense(uint16_t char_id) {
    uint16_t idx = char_id - 1;
    int16_t total = party_characters[idx].base_defense;
    int poo_offset = (idx == CHARACTER_POO) ? 1 : 0;

    /* Add strength modifier from BODY armor */
    total += get_equipment_strength_modifier(idx, EQUIP_BODY, poo_offset);

    /* Add strength modifier from ARMS armor */
    total += get_equipment_strength_modifier(idx, EQUIP_ARMS, poo_offset);

    /* Add strength modifier from OTHER accessory */
    total += get_equipment_strength_modifier(idx, EQUIP_OTHER, poo_offset);

    party_characters[idx].defense = clamp_stat_dual(total);
}

/* RECALC_CHARACTER_POSTMATH_SPEED: Port of asm/misc/recalc_character_postmath_speed.asm.
 * Reads base_speed, adds BODY ep modifier, adds boosted_speed, clamps (lower only).
 * char_id: 1-indexed. */
void recalc_character_postmath_speed(uint16_t char_id) {
    uint16_t idx = char_id - 1;
    int16_t total = party_characters[idx].base_speed;

    /* Add ep modifier from BODY armor */
    total += get_equipment_ep_modifier(idx, EQUIP_BODY);

    /* Add boosted_speed */
    total += party_characters[idx].boosted_speed;

    party_characters[idx].speed = clamp_stat_lower(total);
}

/* RECALC_CHARACTER_POSTMATH_LUCK: Port of asm/misc/recalc_character_postmath_luck.asm.
 * Reads base_luck, adds ep modifiers from ARMS and OTHER slots,
 * adds boosted_luck, clamps (lower only).
 * char_id: 1-indexed. */
void recalc_character_postmath_luck(uint16_t char_id) {
    uint16_t idx = char_id - 1;
    int16_t total = party_characters[idx].base_luck;

    /* Add ep modifier from ARMS */
    total += get_equipment_ep_modifier(idx, EQUIP_ARMS);

    /* Add ep modifier from OTHER */
    total += get_equipment_ep_modifier(idx, EQUIP_OTHER);

    /* Add boosted_luck */
    total += party_characters[idx].boosted_luck;

    party_characters[idx].luck = clamp_stat_lower(total);
}

/* RECALC_CHARACTER_POSTMATH_VITALITY: Port of asm/misc/recalc_character_postmath_vitality.asm.
 * Simply adds base_vitality + boosted_vitality (8-bit add, no clamping).
 * char_id: 1-indexed. */
void recalc_character_postmath_vitality(uint16_t char_id) {
    uint16_t idx = char_id - 1;
    party_characters[idx].vitality = (uint8_t)(party_characters[idx].base_vitality +
                                               party_characters[idx].boosted_vitality);
}

/* RECALC_CHARACTER_POSTMATH_IQ: Port of asm/misc/recalc_character_postmath_iq.asm.
 * Simply adds base_iq + boosted_iq (8-bit add, no clamping).
 * char_id: 1-indexed. */
void recalc_character_postmath_iq(uint16_t char_id) {
    uint16_t idx = char_id - 1;
    party_characters[idx].iq = (uint8_t)(party_characters[idx].base_iq +
                                         party_characters[idx].boosted_iq);
}

/* --- Equipment change functions ---
 * All follow the same pattern from assembly:
 * 1. Read old equipment item ID from the slot
 * 2. Store new item ID to the slot
 * 3. Recalculate affected stats
 * 4. Return old item ID */

/* CHANGE_EQUIPPED_WEAPON: Port of asm/misc/change_equipped_weapon.asm.
 * Recalculates: offense, guts, miss_rate. */
uint16_t change_equipped_weapon(uint16_t char_id, uint16_t new_item_id) {
    uint16_t idx = char_id - 1;
    uint8_t old_item = party_characters[idx].equipment[EQUIP_WEAPON];
    party_characters[idx].equipment[EQUIP_WEAPON] = (uint8_t)new_item_id;

    recalc_character_postmath_offense(char_id);
    recalc_character_postmath_guts(char_id);
    recalc_character_miss_rate(char_id);

    return old_item;
}

/* CHANGE_EQUIPPED_BODY: Port of asm/misc/change_equipped_body.asm.
 * Recalculates: defense, speed, resistances. */
uint16_t change_equipped_body(uint16_t char_id, uint16_t new_item_id) {
    uint16_t idx = char_id - 1;
    uint8_t old_item = party_characters[idx].equipment[EQUIP_BODY];
    party_characters[idx].equipment[EQUIP_BODY] = (uint8_t)new_item_id;

    recalc_character_postmath_defense(char_id);
    recalc_character_postmath_speed(char_id);
    calc_resistances(char_id);

    return old_item;
}

/* CHANGE_EQUIPPED_ARMS: Port of asm/misc/change_equipped_arms.asm.
 * Recalculates: defense, luck, resistances. */
uint16_t change_equipped_arms(uint16_t char_id, uint16_t new_item_id) {
    uint16_t idx = char_id - 1;
    uint8_t old_item = party_characters[idx].equipment[EQUIP_ARMS];
    party_characters[idx].equipment[EQUIP_ARMS] = (uint8_t)new_item_id;

    recalc_character_postmath_defense(char_id);
    recalc_character_postmath_luck(char_id);
    calc_resistances(char_id);

    return old_item;
}

/* CHANGE_EQUIPPED_OTHER: Port of asm/misc/change_equipped_other.asm.
 * Recalculates: defense, luck, resistances. */
uint16_t change_equipped_other(uint16_t char_id, uint16_t new_item_id) {
    uint16_t idx = char_id - 1;
    uint8_t old_item = party_characters[idx].equipment[EQUIP_OTHER];
    party_characters[idx].equipment[EQUIP_OTHER] = (uint8_t)new_item_id;

    recalc_character_postmath_defense(char_id);
    recalc_character_postmath_luck(char_id);
    calc_resistances(char_id);

    return old_item;
}

/* --- calc_resistances ---
 * Port of CALC_RESISTANCES (asm/battle/calc_resistances.asm, 494 lines).
 * Calculates elemental resistance values from BODY, OTHER, and ARMS equipment.
 *
 * The item_parameters::special byte encodes resistances as 2-bit fields:
 *   bits 0-1: fire resistance
 *   bits 2-3: freeze resistance
 *   bits 4-5: flash resistance
 *   bits 6-7: paralysis resistance
 *
 * BODY and OTHER both contribute to fire/freeze/flash/paralysis (summed, clamped 0-3).
 * ARMS contributes only hypnosis_brainshock_resist (full signed byte value).
 *
 * char_id: 1-indexed. */
void calc_resistances(uint16_t char_id) {
    uint16_t idx = char_id - 1;
    CharStruct *ch = &party_characters[idx];

    /* Helper: get the special param as a signed-extended 16-bit value for an equip slot.
     * Returns 0 if slot is empty. */
    #define GET_SPECIAL(slot) ({ \
        int16_t _val = 0; \
        uint8_t _eq = ch->equipment[(slot)]; \
        if (_eq != 0) { \
            uint8_t _item = ch->items[_eq - 1]; \
            _val = get_item_param_signed(_item, ITEM_PARAM_SPECIAL); \
        } \
        _val; \
    })

    int16_t body_special = GET_SPECIAL(EQUIP_BODY);
    int16_t other_special = GET_SPECIAL(EQUIP_OTHER);
    int16_t arms_special = GET_SPECIAL(EQUIP_ARMS);

    #undef GET_SPECIAL

    /* --- fire_resist: bits 0-1 from BODY + OTHER, clamped 0-3 --- */
    {
        int16_t total = (body_special & 0x03) + (other_special & 0x03);
        ch->fire_resist = (total > 3) ? 3 : (uint8_t)total;
    }

    /* --- freeze_resist: bits 2-3 from BODY + OTHER, shifted >> 2, clamped 0-3 --- */
    {
        int16_t total = ((body_special & 0x0C) >> 2) + ((other_special & 0x0C) >> 2);
        ch->freeze_resist = (total > 3) ? 3 : (uint8_t)total;
    }

    /* --- flash_resist: bits 4-5 from BODY + OTHER, shifted >> 4, clamped 0-3 --- */
    {
        int16_t total = ((body_special & 0x30) >> 4) + ((other_special & 0x30) >> 4);
        ch->flash_resist = (total > 3) ? 3 : (uint8_t)total;
    }

    /* --- paralysis_resist: bits 6-7 from BODY + OTHER, shifted >> 6, clamped 0-3 --- */
    {
        int16_t total = ((body_special & 0xC0) >> 6) + ((other_special & 0xC0) >> 6);
        ch->paralysis_resist = (total > 3) ? 3 : (uint8_t)total;
    }

    /* --- hypnosis_brainshock_resist: full signed byte from ARMS --- */
    ch->hypnosis_brainshock_resist = (uint8_t)(arms_special & 0xFF);
}

/* --- Item transformation system ---
 * Port of the timed item transformation subsystem (Fresh Egg → Chick → Chicken).
 * ROM table: TIMED_ITEM_TRANSFORMATION_TABLE (4 entries × 5 bytes).
 * RAM state: LOADED_TIMED_ITEM_TRANSFORMATIONS (4 slots × 4 bytes). */

#define ITEM_TRANSFORM_ENTRY_SIZE   5   /* sizeof(timed_item_transformation) */
#define ITEM_TRANSFORM_MAX_ENTRIES  4   /* max active transformation slots */
#define LOADED_TRANSFORM_ENTRY_SIZE 4   /* sizeof(loaded_timed_item_transformation) */

/* ROM table field offsets (timed_item_transformation) */
#define TIT_OFF_ITEM                0
#define TIT_OFF_SFX                 1
#define TIT_OFF_SFX_FREQUENCY       2
#define TIT_OFF_TARGET_ITEM         3
#define TIT_OFF_TRANSFORMATION_TIME 4

/* RAM slot field offsets (loaded_timed_item_transformation) */
#define LIT_OFF_SFX                     0
#define LIT_OFF_SFX_FREQUENCY           1
#define LIT_OFF_SFX_COUNTDOWN           2
#define LIT_OFF_TRANSFORMATION_COUNTDOWN 3

/* Transformation table (loaded from ROM) */
static const uint8_t *transform_table_data = NULL;
static size_t transform_table_size = 0;

static bool ensure_transform_table(void) {
    if (transform_table_data) return true;
    transform_table_size = ASSET_SIZE(ASSET_DATA_TIMED_ITEM_TRANSFORMATION_TABLE_BIN);
    transform_table_data = ASSET_DATA(ASSET_DATA_TIMED_ITEM_TRANSFORMATION_TABLE_BIN);
    if (!transform_table_data) {
        fprintf(stderr, "inventory: failed to load timed_item_transformation_table.bin\n");
        return false;
    }
    return true;
}

/* Active transformation state (4 slots × 4 bytes) */
static uint8_t loaded_transformations[ITEM_TRANSFORM_MAX_ENTRIES * LOADED_TRANSFORM_ENTRY_SIZE];
uint16_t item_transformations_loaded = 0;
static uint8_t time_until_next_item_transformation_check = 0;

void reset_item_transformations(void) {
    item_transformations_loaded = 0;
}

/* IS_VALID_ITEM_TRANSFORMATION: Port of asm/overworld/is_valid_item_transformation.asm.
 * Returns 1 if the slot has an active transformation, 0 otherwise. */
static uint16_t is_valid_item_transformation(uint16_t slot) {
    uint16_t off = slot * LOADED_TRANSFORM_ENTRY_SIZE;
    if (loaded_transformations[off + LIT_OFF_TRANSFORMATION_COUNTDOWN] != 0)
        return 1;
    if (loaded_transformations[off + LIT_OFF_SFX_FREQUENCY] != 0)
        return 1;
    return 0;
}

/* CANCEL_ITEM_TRANSFORMATION: Port of src/inventory/cancel_item_transformation.asm.
 * Cancels an active transformation slot. */
static void cancel_item_transformation(uint16_t slot) {
    if (is_valid_item_transformation(slot) == 0)
        return;
    item_transformations_loaded--;
    uint16_t off = slot * LOADED_TRANSFORM_ENTRY_SIZE;
    loaded_transformations[off + LIT_OFF_SFX_FREQUENCY] = 0;
    loaded_transformations[off + LIT_OFF_TRANSFORMATION_COUNTDOWN] = 0;
}

/* INITIALIZE_ITEM_TRANSFORMATION: Port of asm/overworld/initialize_item_transformation.asm.
 * Sets up a loaded transformation slot from the ROM table entry. */
static void initialize_item_transformation(uint16_t slot) {
    if (!ensure_transform_table()) return;

    if (is_valid_item_transformation(slot) == 0) {
        time_until_next_item_transformation_check = 60;
        item_transformations_loaded++;
    }

    uint16_t loaded_off = slot * LOADED_TRANSFORM_ENTRY_SIZE;
    uint16_t table_off = slot * ITEM_TRANSFORM_ENTRY_SIZE;

    uint8_t sfx = transform_table_data[table_off + TIT_OFF_SFX];
    uint8_t sfx_freq = transform_table_data[table_off + TIT_OFF_SFX_FREQUENCY];
    uint8_t trans_time = transform_table_data[table_off + TIT_OFF_TRANSFORMATION_TIME];

    loaded_transformations[loaded_off + LIT_OFF_SFX] = sfx;
    loaded_transformations[loaded_off + LIT_OFF_SFX_FREQUENCY] = sfx_freq;

    /* Initial countdown: sfx_freq + rand(0,2) - 1
     * Assembly: RAND_MOD(2) = rand() % 3 (range 0-2) */
    uint8_t rand_val = (uint8_t)(rand() % 3);
    loaded_transformations[loaded_off + LIT_OFF_SFX_COUNTDOWN] =
        (uint8_t)(sfx_freq + rand_val - 1);

    loaded_transformations[loaded_off + LIT_OFF_TRANSFORMATION_COUNTDOWN] = trans_time;
}

/* START_ITEM_TRANSFORMATION: Port of src/inventory/start_item_transformation.asm.
 * Searches the transformation table for item_id. If found and the slot is
 * not already valid, initializes the transformation.
 * item_id: the item to start transforming. */
static void start_item_transformation(uint8_t item_id) {
    if (!ensure_transform_table()) return;

    for (uint16_t i = 0; ; i++) {
        uint16_t table_off = i * ITEM_TRANSFORM_ENTRY_SIZE;
        if (table_off >= transform_table_size) break;
        uint8_t entry_item = transform_table_data[table_off + TIT_OFF_ITEM];
        if (entry_item == 0) break;
        if (entry_item == item_id) {
            if (is_valid_item_transformation(i) != 0)
                return;  /* already active */
            initialize_item_transformation(i);
            return;
        }
    }
}

/* START_TIMED_ITEM_TRANSFORMATION: Port of src/inventory/start_timed_item_transformation.asm.
 * Searches the transformation table for item_id, cancels any existing
 * transformation for that slot, then searches all party members' inventories
 * for the item. If found in inventory, initializes the transformation.
 * item_id: the item whose transformation to start. */
static void start_timed_item_transformation(uint8_t item_id) {
    if (!ensure_transform_table()) return;

    /* Find the table entry for this item */
    uint16_t found_slot = 0;
    bool found = false;
    for (uint16_t i = 0; ; i++) {
        uint16_t table_off = i * ITEM_TRANSFORM_ENTRY_SIZE;
        if (table_off >= transform_table_size) break;
        uint8_t entry_item = transform_table_data[table_off + TIT_OFF_ITEM];
        if (entry_item == 0) break;
        if (entry_item == item_id) {
            found_slot = i;
            found = true;
            break;
        }
        found_slot = i + 1;
    }

    /* If no matching entry found and found_slot is OOB, that's a bug */
    if (!found && found_slot >= ITEM_TRANSFORM_MAX_ENTRIES)
        FATAL("start_timed_item_transformation: found_slot=%u >= max=%d for item=%u\n",
              found_slot, ITEM_TRANSFORM_MAX_ENTRIES, item_id);

    /* Cancel the existing transformation for this slot */
    cancel_item_transformation(found_slot);

    /* Search all party members' inventories for the item */
    uint8_t party_count = game_state.player_controlled_party_count;
    for (uint16_t p = 0; p < party_count; p++) {
        uint8_t member = game_state.party_members[p];
        if (member == 0) continue;
        uint16_t char_idx = member - 1;
        /* Assembly: LDA #14; CLC; SBC slot; BRANCHLTEQS → loops slot 0..13 */
        for (uint16_t slot = 0; slot < ITEM_INVENTORY_SIZE; slot++) {
            uint8_t inv_item = party_characters[char_idx].items[slot];
            if (inv_item == 0) break;
            if (inv_item == item_id) {
                initialize_item_transformation(found_slot);
                return;
            }
        }
    }
}

/* UPDATE_TEDDY_BEAR_PARTY: Port of asm/battle/update_teddy_bear_party.asm.
 * Scans all party members' inventories for teddy-bear-type items (type==4).
 * Finds the one with the highest EP value. Uses the item's "strength" parameter
 * (sign-extended: value - 0x80 XOR 0xFF80) to determine which character to add.
 * Removes existing teddy bear / plush teddy bear from party, then adds the
 * matching character if found. */
static void update_teddy_bear_party(void) {
    if (!ensure_item_config()) return;

    uint8_t best_item = 0;
    uint8_t party_count = game_state.player_controlled_party_count;

    for (uint16_t p = 0; p < party_count; p++) {
        uint8_t member = game_state.party_members[p];
        if (member == 0) continue;
        uint16_t char_idx = member - 1;

        for (uint16_t slot = 0; slot < ITEM_INVENTORY_SIZE; slot++) {
            uint8_t inv_item = party_characters[char_idx].items[slot];
            if (inv_item == 0) break;

            const ItemConfig *entry = get_item_entry(inv_item);
            if (!entry) continue;

            uint8_t item_type = entry->type;
            if (item_type != ITEM_TYPE_TEDDY_BEAR) continue;

            if (best_item == 0) {
                best_item = inv_item;
            } else {
                /* Compare EP values: branch if current best has higher EP */
                const ItemConfig *best_entry = get_item_entry(best_item);
                if (!best_entry) continue;
                int8_t cur_ep = (int8_t)entry->params[ITEM_PARAM_EP];
                int8_t best_ep = (int8_t)best_entry->params[ITEM_PARAM_EP];
                /* Assembly: LDA best_ep; CLC; SBC cur_ep; BRANCHLTEQS @NEXT_ITEM
                 * CLC+SBC = best_ep - cur_ep - 1.
                 * BRANCHLTEQS: skip update if best_ep - cur_ep - 1 <= 0 → best_ep <= cur_ep.
                 * So update only if cur_ep > best_ep (strictly greater). */
                if (cur_ep > best_ep) {
                    best_item = inv_item;
                }
            }
        }
    }

    if (best_item != 0) {
        /* Get the strength parameter, sign-extend: (val - 0x80) ^ 0xFF80 */
        const ItemConfig *entry = get_item_entry(best_item);
        if (!entry) return;
        uint8_t raw_strength = entry->params[ITEM_PARAM_STRENGTH];
        /* Assembly: SEC; AND #$00FF; SBC #$0080; EOR #$FF80
         * This sign-extends an 8-bit signed value to 16-bit. */
        int16_t char_id = (int16_t)(int8_t)(raw_strength);

        if (check_character_in_party((uint16_t)char_id) != 0)
            return;  /* Character already in party */

        remove_char_from_party(PARTY_MEMBER_TEDDY_BEAR);
        remove_char_from_party(PARTY_MEMBER_PLUSH_TEDDY_BEAR);
        add_char_to_party((uint16_t)char_id);
    } else {
        remove_char_from_party(PARTY_MEMBER_TEDDY_BEAR);
        remove_char_from_party(PARTY_MEMBER_PLUSH_TEDDY_BEAR);
    }
}

/* INIT_ALL_ITEM_TRANSFORMATIONS: Port of src/inventory/init_all_item_transformations.asm.
 * Iterates through the TIMED_ITEM_TRANSFORMATION_TABLE. For each item entry,
 * checks if the item is already in anyone's inventory (FIND_ITEM_IN_INVENTORY2).
 * If found, calls start_item_transformation; if not, calls start_timed_item_transformation. */
static void init_all_item_transformations(void) {
    if (!ensure_transform_table()) return;

    for (uint16_t i = 0; ; i++) {
        uint16_t table_off = i * ITEM_TRANSFORM_ENTRY_SIZE;
        if (table_off >= transform_table_size) break;
        uint8_t entry_item = transform_table_data[table_off + TIT_OFF_ITEM];
        if (entry_item == 0) break;

        /* Check if any party member has this item (0xFF = search all) */
        uint16_t found = find_item_in_inventory2(CHAR_ID_ANY, (uint16_t)entry_item);
        if (found != 0) {
            start_item_transformation(entry_item);
        } else {
            start_timed_item_transformation(entry_item);
        }
    }
}

/* PROCESS_ITEM_TRANSFORMATIONS: Port of asm/overworld/process_item_transformations.asm.
 * Called each frame from the overworld main loop. Manages SFX countdowns
 * and transformation countdowns for all active transformation slots.
 * When a transformation countdown expires, takes the source item from
 * the character and gives them the target item. */
void process_item_transformations(void) {
    if (!ensure_transform_table()) return;

    /* Don't process during battle entry or transitions */
    if (ow.enemy_has_been_touched + ow.battle_swirl_countdown != 0)
        return;
    if (ow.disabled_transitions != 0)
        return;
    if (game_state.camera_mode == 2)
        return;

    /* Decrement the check timer; only process when it reaches 0 */
    time_until_next_item_transformation_check--;
    if (time_until_next_item_transformation_check != 0)
        return;

    /* Reset timer to 60 frames (1 second) */
    time_until_next_item_transformation_check = 60;

    bool sfx_played_this_frame = false;

    for (uint16_t i = 0; i < ITEM_TRANSFORM_MAX_ENTRIES; i++) {
        uint16_t off = i * LOADED_TRANSFORM_ENTRY_SIZE;

        /* SFX countdown management */
        if (!sfx_played_this_frame) {
            uint8_t sfx_freq = loaded_transformations[off + LIT_OFF_SFX_FREQUENCY];
            if (sfx_freq != 0) {
                uint8_t countdown = loaded_transformations[off + LIT_OFF_SFX_COUNTDOWN];
                countdown--;
                loaded_transformations[off + LIT_OFF_SFX_COUNTDOWN] = countdown;

                if (countdown == 0) {
                    /* Reset countdown: sfx_freq + rand(0,2) - 1
                     * Assembly: RAND_MOD(2) = rand() % 3 (range 0-2) */
                    uint8_t rand_val = (uint8_t)(rand() % 3);
                    loaded_transformations[off + LIT_OFF_SFX_COUNTDOWN] =
                        (uint8_t)(sfx_freq + rand_val - 1);
                    /* Play the SFX */
                    uint8_t sfx_id = loaded_transformations[off + LIT_OFF_SFX];
                    play_sfx((uint16_t)sfx_id);
                    sfx_played_this_frame = true;
                }
            }
        }

        /* Transformation countdown management */
        uint8_t trans_countdown =
            loaded_transformations[off + LIT_OFF_TRANSFORMATION_COUNTDOWN];
        if (trans_countdown != 0) {
            trans_countdown--;
            loaded_transformations[off + LIT_OFF_TRANSFORMATION_COUNTDOWN] = trans_countdown;

            if (trans_countdown == 0) {
                /* Transformation complete: swap the item */
                uint16_t table_off = i * ITEM_TRANSFORM_ENTRY_SIZE;
                uint8_t source_item = transform_table_data[table_off + TIT_OFF_ITEM];
                uint8_t target_item = transform_table_data[table_off + TIT_OFF_TARGET_ITEM];

                /* Take source item from whichever party member has it (0xFF = search all) */
                uint16_t who = take_item_from_character(CHAR_ID_ANY, (uint16_t)source_item);
                /* Give target item to that character */
                give_item_to_character(who, (uint16_t)target_item);
            }
        }
    }
}

/* ADD_CHAR_TO_PARTY: Port of asm/misc/party_add_char.asm.
 * Adds a character to game_state.party_members[] in sorted order.
 * Calls ADD_PARTY_MEMBER for entity creation, then if char_id <= 4 (PC),
 * calls UPDATE_TEDDY_BEAR_PARTY and INIT_ALL_ITEM_TRANSFORMATIONS.
 * char_id: 1-indexed. */
void add_char_to_party(uint16_t char_id) {
    /* Find the insertion point: party_members is sorted by char_id.
     * Assembly iterates positions 0-5: if party_members[pos] == char_id, return.
     * If party_members[pos] > char_id or == 0, insert here. */
    int insert_pos = -1;
    for (int i = 0; i < 6; i++) {
        uint8_t member = game_state.party_members[i];
        if (member == (uint8_t)char_id)
            return;  /* Already in party */
        if (member > (uint8_t)char_id || member == 0) {
            insert_pos = i;
            break;
        }
    }
    if (insert_pos < 0) return;  /* Party full */

    /* Find the first empty slot at or after insert_pos.
     * Assembly: LDA #6; CLC; SBC pos; BRANCHLTEQS @EXIT → pos <= 5 */
    int end_pos = insert_pos;
    while (end_pos < 6 && game_state.party_members[end_pos] != 0) {
        end_pos++;
    }
    if (end_pos >= 6) return;  /* No space */

    /* Shift elements right to make room at insert_pos.
     * Assembly: shifts party_members[insert_pos..end_pos-1] right by one. */
    for (int i = end_pos; i > insert_pos; i--) {
        game_state.party_members[i] = game_state.party_members[i - 1];
    }

    /* Store the character at the insertion point */
    game_state.party_members[insert_pos] = (uint8_t)char_id;

    uint16_t entity_slot = add_party_member(char_id);

    /* Assembly: after ADD_PARTY_MEMBER, set OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED */
    if (entity_slot > 0) {
        int16_t ent_off = (int16_t)(entity_slot);
        entities.tick_callback_hi[ent_off] |= OBJECT_TICK_DISABLED;
        entities.tick_callback_hi[ent_off] |= OBJECT_MOVE_DISABLED;
    }

    /* For PC characters (1-4), update teddy bear state and item transformations */
    if (char_id <= 4) {
        update_teddy_bear_party();
        init_all_item_transformations();
    }
}

/* REMOVE_CHAR_FROM_PARTY: Port of asm/misc/party_remove_char.asm (53 lines).
 * Removes a character from game_state.party_members[] by shifting remaining
 * members left, then calls REMOVE_PARTY_MEMBER (entity removal), and if
 * char_id <= 4 (PC, not NPC), calls UPDATE_TEDDY_BEAR_PARTY and
 * INIT_ALL_ITEM_TRANSFORMATIONS.
 *
 * Assembly: FAR function. A = char_id (1-indexed).
 * Loop uses BRANCHGTS with party_count. */
void remove_char_from_party(uint16_t char_id) {
    /* Search party_members[] for the character */
    uint8_t count = game_state.party_count;
    int found = -1;
    for (int i = 0; i < count; i++) {
        if (game_state.party_members[i] == (uint8_t)char_id) {
            found = i;
            break;
        }
    }
    if (found < 0) return;

    /* Shift remaining members left to fill the gap.
     * Assembly: LDA #6; CLC; SBC @VIRTUAL02; BRANCHGTS @SHIFT_MEMBER
     * CLC+SBC gives 6-pos-1 = 5-pos. BRANCHGTS branches when >= 0,
     * so loops while pos <= 5 (shifts positions found..5). */
    for (int i = found; i < 5; i++) {
        game_state.party_members[i] = game_state.party_members[i + 1];
    }
    /* Clear the last position (assembly: DEX then STZ) */
    game_state.party_members[5] = 0;

    remove_party_member(char_id);

    /* For PC characters (1-4), update teddy bear state and item transformations */
    if (char_id <= 4) {
        update_teddy_bear_party();
        init_all_item_transformations();
    }
}

/* --- Item access functions --- */

/* GET_CHARACTER_ITEM: Port of asm/misc/get_character_item.asm.
 * Assembly calling convention: A=char_id, X=slot (both 1-indexed).
 * Returns the item ID at items[slot-1] for party_characters[char_id-1]. */
uint16_t get_character_item(uint16_t char_id, uint16_t slot) {
    if (char_id == 0 || char_id > TOTAL_PARTY_COUNT) return 0;
    uint16_t char_idx = char_id - 1;
    uint16_t slot_idx = slot - 1;
    return party_characters[char_idx].items[slot_idx];
}

/* FIND_EMPTY_INVENTORY_SLOT: Port of asm/battle/find_empty_inventory_slot.asm.
 * Scans character's inventory for first empty slot.
 * char_id: 1-indexed.
 * Returns 0-based index of first empty slot.
 * Note: assembly only checks slots 0-12 due to off-by-one (CLC;SBC with
 * BRANCHLTEQS exits when 13-slot <= 0, so slot 13 is never checked). */
uint16_t find_empty_inventory_slot(uint16_t char_id) {
    uint16_t char_idx = char_id - 1;
    if (char_idx >= TOTAL_PARTY_COUNT) return 14; /* invalid → full */
    uint16_t slot = 0;
    /* Assembly: LDA #14; CLC; SBC slot; BRANCHLTEQS @DONE
     * CLC+SBC = A - M - 1 = 13 - slot. Exits when 13 - slot <= 0 → slot >= 13.
     * So loops for slot 0 through 12 only (13 iterations, not 14). */
    while (slot < 13) {
        if (party_characters[char_idx].items[slot] == 0)
            break;
        slot++;
    }
    return slot;
}

/* --- Item give/remove functions --- */

/* GIVE_ITEM_TO_SPECIFIC_CHARACTER: Port of asm/misc/give_item_to_specific_character.asm.
 * Finds an empty inventory slot and places the item there.
 * Handles TEDDY_BEAR type (calls UPDATE_TEDDY_BEAR_PARTY) and
 * TRANSFORM flag (calls START_ITEM_TRANSFORMATION).
 * char_id: 1-indexed. item_id: 1-based.
 * Returns char_id on success, 0 if inventory full.
 *
 * Assembly loop uses: LDA #sizeof(items); CLC; SBC counter; JUMPGTS
 * JUMPGTS branches when >= 0 → loops for counter 0 through 13 (14 iterations). */
uint16_t give_item_to_specific_character(uint16_t char_id, uint16_t item_id) {
    uint16_t char_idx = char_id - 1;
    if (char_idx >= TOTAL_PARTY_COUNT)
        FATAL("give_item_to_specific_character: invalid char_id=%u\n", char_id);

    /* Search for empty slot: assembly loops counter 0..13 (14 slots) */
    for (uint16_t slot = 0; slot < ITEM_INVENTORY_SIZE; slot++) {
        if (party_characters[char_idx].items[slot] != 0)
            continue;

        /* Found empty slot — store item */
        party_characters[char_idx].items[slot] = (uint8_t)item_id;

        /* Check if item type is TEDDY_BEAR */
        const ItemConfig *entry = get_item_entry(item_id);
        if (entry && entry->type == ITEM_TYPE_TEDDY_BEAR) {
            update_teddy_bear_party();
        }

        /* Check if item has TRANSFORM flag */
        if (entry && (entry->flags & ITEM_FLAG_TRANSFORM)) {
            start_item_transformation((uint8_t)item_id);
        }

        return char_idx + 1; /* success: return char_id (1-indexed) */
    }

    return 0; /* all slots full */
}

/* GIVE_ITEM_TO_CHARACTER: Port of asm/misc/give_item_to_character.asm.
 * If char_id == 0xFF, tries each player-controlled party member.
 * Otherwise, tries the specific character.
 * Returns receiving member's char_id on success, 0 if all full.
 *
 * Assembly: FAR function. First param A=char_id, second param X=item_id. */
uint16_t give_item_to_character(uint16_t char_id, uint16_t item_id) {
    if (char_id != 0xFF) {
        /* Single character mode */
        return give_item_to_specific_character(char_id, item_id);
    }

    /* All-party mode: try each player-controlled party member */
    uint8_t party_count = game_state.player_controlled_party_count;
    for (uint16_t i = 0; i < party_count; i++) {
        uint8_t member_id = game_state.party_members[i];
        uint16_t result = give_item_to_specific_character(member_id, item_id);
        if (result != 0) {
            /* Return the party_members[] value (character ID), not the result */
            return member_id;
        }
    }

    return 0; /* all party members' inventories full */
}

/* REMOVE_ITEM_FROM_INVENTORY: Port of asm/misc/remove_item_from_inventory.asm.
 * char_id: 1-indexed. item_slot: 1-indexed position.
 *
 * Steps:
 * 1. Check each equipment slot — if it matches item_slot, unequip it
 * 2. For each equipment slot with index > removed position, decrement it
 * 3. Save the item being removed
 * 4. Shift items left to compact the array
 * 5. Clear the last occupied slot
 * 6. Handle teddy bear removal (REMOVE_CHAR_FROM_PARTY + UPDATE_TEDDY_BEAR_PARTY)
 * 7. Handle transform flag (START_TIMED_ITEM_TRANSFORMATION)
 * Returns char_id. */
uint16_t remove_item_from_inventory(uint16_t char_id, uint16_t item_slot) {
    uint16_t char_idx = char_id - 1;

    /* --- Step 1: Unequip if the removed item is equipped --- */
    /* Check WEAPON slot */
    uint8_t weapon_equip = party_characters[char_idx].equipment[EQUIP_WEAPON];
    if (weapon_equip == item_slot) {
        change_equipped_weapon(char_id, 0);
    }
    /* Check BODY slot */
    uint8_t body_equip = party_characters[char_idx].equipment[EQUIP_BODY];
    if (body_equip == item_slot) {
        change_equipped_body(char_id, 0);
    }
    /* Check ARMS slot */
    uint8_t arms_equip = party_characters[char_idx].equipment[EQUIP_ARMS];
    if (arms_equip == item_slot) {
        change_equipped_arms(char_id, 0);
    }
    /* Check OTHER slot */
    uint8_t other_equip = party_characters[char_idx].equipment[EQUIP_OTHER];
    if (other_equip == item_slot) {
        change_equipped_other(char_id, 0);
    }

    /* --- Step 2: Adjust equipment indices for the shift ---
     * For each equipment slot, if its value (1-based item index) is greater than
     * the removed position, the item has shifted left by one, so decrement the index.
     * Assembly uses BCS (unsigned >=) to skip the decrement when equip >= item_slot.
     * In the assembly: LDA item_slot; CMP equip_value; BCS skip_dec
     * This means: if item_slot >= equip_value, skip. Decrement when item_slot < equip_value.
     * Equivalently: decrement when equip_value > item_slot. */
    if (party_characters[char_idx].equipment[EQUIP_WEAPON] > item_slot) {
        party_characters[char_idx].equipment[EQUIP_WEAPON]--;
    }
    if (party_characters[char_idx].equipment[EQUIP_BODY] > item_slot) {
        party_characters[char_idx].equipment[EQUIP_BODY]--;
    }
    if (party_characters[char_idx].equipment[EQUIP_ARMS] > item_slot) {
        party_characters[char_idx].equipment[EQUIP_ARMS]--;
    }
    if (party_characters[char_idx].equipment[EQUIP_OTHER] > item_slot) {
        party_characters[char_idx].equipment[EQUIP_OTHER]--;
    }

    /* --- Step 3: Save the removed item --- */
    uint16_t removed_slot_idx = item_slot - 1; /* 0-based */
    uint8_t removed_item = party_characters[char_idx].items[removed_slot_idx];

    /* --- Step 4: Shift items left to fill the gap ---
     * Starting from item_slot, copy items[i+1] to items[i] until end or empty.
     * Assembly: loops from item_slot to 13, reading next item and writing to current.
     * Stops when next item is 0 (empty) or reached slot 13. */
    uint16_t shift_pos = item_slot; /* 1-based, starts at position after removed */
    while (shift_pos < ITEM_INVENTORY_SIZE) {
        uint8_t next_item = party_characters[char_idx].items[shift_pos]; /* next slot (0-based = shift_pos) */
        party_characters[char_idx].items[shift_pos - 1] = next_item;
        if (next_item == 0) break;
        shift_pos++;
    }

    /* --- Step 5: Clear the last slot ---
     * Assembly: writes 0 to items[shift_pos - 1] after loop exits. */
    if (shift_pos <= ITEM_INVENTORY_SIZE) {
        party_characters[char_idx].items[shift_pos - 1] = 0;
    }

    /* --- Step 6: Handle teddy bear type ---
     * If removed item is TEDDY_BEAR type, remove the corresponding party member
     * and update teddy bear party state.
     * Assembly reads item.params.strength as signed byte, uses it as char_id for removal. */
    const ItemConfig *entry = get_item_entry(removed_item);
    if (entry && entry->type == ITEM_TYPE_TEDDY_BEAR) {
        /* Read strength parameter as signed, used as char_id for removal */
        int16_t strength_signed = (int16_t)(int8_t)entry->params[ITEM_PARAM_STRENGTH];
        remove_char_from_party((uint16_t)strength_signed);
        update_teddy_bear_party();
    }

    /* --- Step 7: Handle transform flag --- */
    if (entry && (entry->flags & ITEM_FLAG_TRANSFORM)) {
        start_timed_item_transformation(removed_item);
    }

    return char_idx + 1; /* return char_id (1-indexed) */
}

/* Item type equipment category mask and values.
 * item.type bits 2-3 encode the equipment slot category.
 * Assembly: AND #EQUIPMENT_SLOT::ALL<<2, then CMP with shifted values. */
#define EQUIP_TYPE_MASK    0x0C  /* EQUIPMENT_SLOT::ALL << 2 = 3 << 2 = 0x0C */
#define EQUIP_TYPE_WEAPON  0x00  /* EQUIPMENT_SLOT::WEAPON << 2 = 0 << 2 */
#define EQUIP_TYPE_BODY    0x04  /* EQUIPMENT_SLOT::BODY << 2 = 1 << 2 */
#define EQUIP_TYPE_ARMS    0x08  /* EQUIPMENT_SLOT::ARMS << 2 = 2 << 2 */
#define EQUIP_TYPE_OTHER   0x0C  /* EQUIPMENT_SLOT::OTHER << 2 = 3 << 2 */

/* EQUIP_ITEM: Port of asm/misc/equip_item.asm.
 * Reads item at the given slot, determines equipment category from item.type,
 * and calls the appropriate change_equipped_* function.
 * char_id: 1-indexed. item_slot: 1-indexed inventory position.
 * Returns old equipment slot index, or 0 if not equippable. */
uint16_t equip_item(uint16_t char_id, uint16_t item_slot) {
    uint16_t char_idx = char_id - 1;
    uint16_t slot_idx = item_slot - 1;

    /* Read item ID at the given slot */
    uint8_t item_id = party_characters[char_idx].items[slot_idx];
    if (item_id == 0) return 0;

    /* Look up item type to determine equipment category */
    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry) return 0;

    uint8_t equip_type = entry->type & EQUIP_TYPE_MASK;

    switch (equip_type) {
    case EQUIP_TYPE_WEAPON:
        return change_equipped_weapon(char_id, item_slot);
    case EQUIP_TYPE_BODY:
        return change_equipped_body(char_id, item_slot);
    case EQUIP_TYPE_ARMS:
        return change_equipped_arms(char_id, item_slot);
    case EQUIP_TYPE_OTHER:
        return change_equipped_other(char_id, item_slot);
    default:
        return 0; /* not equippable */
    }
}

/* SWAP_ITEM_INTO_EQUIPMENT: Port of asm/battle/swap_item_into_equipment.asm (643 lines).
 *
 * Moves an item from source character's inventory to target character's inventory,
 * adjusting equipment indices on the source character. Used by the Goods → Give action.
 *
 * Assembly calling convention:
 *   A = target_char_id (1-based)
 *   X = source_char_id (1-based)
 *   Y = item_slot (1-based position in source's inventory)
 *
 * The assembly version has separate code paths for each of the 4 equipment slots,
 * and separate paths for same-character vs different-character transfers.
 * This C version uses loops to avoid the ~500 lines of repetitive assembly. */
void swap_item_into_equipment(uint16_t source_char_id, uint16_t item_slot,
                               uint16_t target_char_id) {
    uint16_t src_idx = source_char_id - 1;  /* 0-based */
    uint16_t slot_0 = item_slot - 1;        /* 0-based */

    /* 1. Read the item being moved */
    uint8_t item_id = party_characters[src_idx].items[slot_0];

    /* 2. Compact source inventory: shift items at positions >= item_slot left by 1.
     * Assembly: starts at LOCAL04 = item_slot (1-based), reads items[pos],
     * stores at items[pos-1], increments until pos >= 14 or item is 0. */
    uint16_t compact_end = item_slot;  /* 1-based, tracks last valid position */
    for (uint16_t pos = item_slot; pos < ITEM_INVENTORY_SIZE; pos++) {
        uint8_t next_item = party_characters[src_idx].items[pos];
        if (next_item == 0) break;
        party_characters[src_idx].items[pos - 1] = next_item;
        compact_end = pos + 1;
    }
    /* Clear the last used slot */
    party_characters[src_idx].items[compact_end - 1] = 0;

    /* 3. Give the item to the target character */
    give_item_to_character(target_char_id, item_id);

    /* 4. Adjust equipment indices on the source character */
    if (source_char_id == target_char_id) {
        /* Same character: item was removed and re-added.
         * Assembly checks each equipment slot sequentially (WEAPON→BODY→ARMS→OTHER).
         * On first match: update to new position, adjust the other 3, then RETURN.
         * If no match (@NO_EQUIP_MATCH): adjust all 4, then RETURN.
         * These paths are mutually exclusive. */
        bool matched = false;
        for (int eq = 0; eq < EQUIP_COUNT; eq++) {
            uint8_t eq_val = party_characters[src_idx].equipment[eq];
            if (eq_val == 0) continue;

            if (eq_val == item_slot) {
                matched = true;
                /* This equipment slot pointed at the moved item.
                 * Assembly: calls FIND_EMPTY_INVENTORY_SLOT to locate new position. */
                uint16_t new_pos = find_empty_inventory_slot(target_char_id);
                party_characters[src_idx].equipment[eq] = (uint8_t)new_pos;

                /* Adjust the other 3 equipment indices above item_slot. */
                for (int other_eq = 0; other_eq < EQUIP_COUNT; other_eq++) {
                    if (other_eq == eq) continue;
                    uint8_t other_val = party_characters[src_idx].equipment[other_eq];
                    if (other_val > item_slot) {
                        party_characters[src_idx].equipment[other_eq] = other_val - 1;
                    }
                }
                break;  /* Assembly only fixes one matching equipment slot */
            }
        }
        if (!matched) {
            /* @NO_EQUIP_MATCH path: just decrement indices above removed slot */
            for (int eq = 0; eq < EQUIP_COUNT; eq++) {
                uint8_t eq_val = party_characters[src_idx].equipment[eq];
                if (eq_val > item_slot) {
                    party_characters[src_idx].equipment[eq] = eq_val - 1;
                }
            }
        }
    } else {
        /* Different character: item moved to another character.
         * Check if the item was equipped on source; if so, unequip it.
         * Assembly @DIFFERENT_CHARACTER: checks each slot of TARGET char...
         * wait — actually the assembly checks SOURCE char's equipment to see
         * if the moved item was equipped, then calls CHANGE_EQUIPPED_*(source, 0).
         * Then adjusts remaining indices above removed position. */
        for (int eq = 0; eq < EQUIP_COUNT; eq++) {
            uint8_t eq_val = party_characters[src_idx].equipment[eq];
            if (eq_val == item_slot) {
                /* Unequip: set equipment slot to 0 */
                switch (eq) {
                case EQUIP_WEAPON: change_equipped_weapon(source_char_id, 0); break;
                case EQUIP_BODY:   change_equipped_body(source_char_id, 0); break;
                case EQUIP_ARMS:   change_equipped_arms(source_char_id, 0); break;
                case EQUIP_OTHER:  change_equipped_other(source_char_id, 0); break;
                }
                break;
            }
        }

        /* Adjust remaining equipment indices above removed position */
        for (int eq = 0; eq < EQUIP_COUNT; eq++) {
            uint8_t eq_val = party_characters[src_idx].equipment[eq];
            if (eq_val > item_slot) {
                party_characters[src_idx].equipment[eq] = eq_val - 1;
            }
        }
    }

}

/* TAKE_ITEM_FROM_SPECIFIC_CHARACTER: Port of asm/misc/take_item_from_specific_character.asm.
 * Searches character's inventory for the first occurrence of item_id.
 * If found, removes it via remove_item_from_inventory.
 * char_id: 1-indexed. item_id: item to find and remove.
 * Returns char_id on success, 0 if item not found in inventory. */
uint16_t take_item_from_specific_character(uint16_t char_id, uint16_t item_id) {
    uint16_t char_idx = char_id - 1;

    for (uint16_t slot = 0; slot < ITEM_INVENTORY_SIZE; slot++) {
        if (party_characters[char_idx].items[slot] == (uint8_t)item_id) {
            /* Found the item — remove it (slot+1 for 1-based, char_idx+1 for char_id) */
            return remove_item_from_inventory(char_idx + 1, slot + 1);
        }
    }

    return 0; /* item not found */
}

/* IS_ITEM_EQUIPPED_BY_ID: Port of src/inventory/is_item_equipped_by_id.asm.
 * Checks if an item (by item ID) is equipped by a character.
 * For each of the 4 equipment slots:
 *   1. Read slot value (1-based inventory position)
 *   2. If empty (0), skip
 *   3. Look up items[slot-1] to get actual item ID
 *   4. Compare with target item_id
 * char_id: 1-indexed. Returns 1 if equipped, 0 if not. */
uint16_t is_item_equipped_by_id(uint16_t char_id, uint16_t item_id) {
    uint16_t char_idx = char_id - 1;

    for (int slot = 0; slot < EQUIP_COUNT; slot++) {
        uint8_t equip_pos = party_characters[char_idx].equipment[slot];
        if (equip_pos == 0) continue; /* empty slot */
        uint8_t equipped_item = party_characters[char_idx].items[equip_pos - 1];
        if (equipped_item == (uint8_t)item_id) {
            return 1;
        }
    }

    return 0;
}

/* CHECK_ITEM_EQUIPPED: Port of asm/misc/check_item_equipped.asm.
 * Checks if a specific inventory slot position is currently equipped.
 * Equipment slots store 1-based inventory positions; this compares directly.
 * char_id: 1-indexed. item_slot: 1-based inventory position.
 * Returns 1 if that slot is equipped, 0 if not. */
uint16_t check_item_equipped(uint16_t char_id, uint16_t item_slot) {
    uint16_t char_idx = char_id - 1;

    for (int slot = 0; slot < EQUIP_COUNT; slot++) {
        uint8_t equip_pos = party_characters[char_idx].equipment[slot];
        if (equip_pos == (uint8_t)item_slot) {
            return 1;
        }
    }

    return 0;
}

/* FIND_ITEM_IN_INVENTORY: Port of asm/misc/find_item_in_inventory.asm.
 * Near function. Searches character's items[0..13] for item_id.
 * char_id: 1-indexed.
 * Returns char_id (1-based) if found, 0 if not found.
 *
 * Assembly: A=char_id, X=item_id. Loop uses BRANCHGTS with 14 iterations. */
uint16_t find_item_in_inventory(uint16_t char_id, uint16_t item_id) {
    uint16_t char_idx = char_id - 1;
    if (char_idx >= TOTAL_PARTY_COUNT) return 0; /* invalid → not found */

    for (uint16_t slot = 0; slot < ITEM_INVENTORY_SIZE; slot++) {
        if (party_characters[char_idx].items[slot] == (uint8_t)item_id) {
            return char_idx + 1; /* return char_id (1-based) */
        }
    }

    return 0; /* not found */
}

/* FIND_ITEM_IN_INVENTORY2: Port of asm/misc/find_item_in_inventory2.asm.
 * Far wrapper around FIND_ITEM_IN_INVENTORY.
 * If char_id == 0xFF, searches all player-controlled party members.
 * Otherwise, searches the specified character.
 *
 * Assembly: A=char_id_or_0xFF, X=item_id.
 * When 0xFF: loops party_members[0..party_count-1], calls FIND_ITEM_IN_INVENTORY
 * for each, returns party_members[i] (char_id) on first hit.
 * When specific: calls FIND_ITEM_IN_INVENTORY directly, returns as-is. */
uint16_t find_item_in_inventory2(uint16_t char_id, uint16_t item_id) {
    if (char_id != 0xFF) {
        return find_item_in_inventory(char_id, item_id);
    }

    /* Search all player-controlled party members */
    uint8_t party_count = game_state.player_controlled_party_count;
    for (uint16_t i = 0; i < party_count; i++) {
        uint8_t member_id = game_state.party_members[i];
        uint16_t result = find_item_in_inventory(member_id, item_id);
        if (result != 0) {
            return member_id; /* return the party_members[] char_id */
        }
    }

    return 0; /* not found in any party member */
}

/* TAKE_ITEM_FROM_CHARACTER: Port of asm/misc/take_item_from_character.asm.
 * If char_id == 0xFF, tries each player-controlled party member.
 * Otherwise, tries the specific character.
 * Returns character's member ID on success, 0 if not found.
 *
 * Assembly: FAR function. First param A=char_id, second param X=item_id. */
uint16_t take_item_from_character(uint16_t char_id, uint16_t item_id) {
    if (char_id != 0xFF) {
        /* Single character mode */
        return take_item_from_specific_character(char_id, item_id);
    }

    /* All-party mode: try each player-controlled party member */
    uint8_t party_count = game_state.player_controlled_party_count;
    for (uint16_t i = 0; i < party_count; i++) {
        uint8_t member_id = game_state.party_members[i];
        uint16_t result = take_item_from_specific_character(member_id, item_id);
        if (result != 0) {
            return member_id;
        }
    }

    return 0; /* item not found in any party member */
}

/* --- Escargo Express functions --- */

/* ESCARGO_EXPRESS_STORE: Port of asm/misc/escargo_express_store.asm.
 * Finds first empty slot in escargo_express_items[] and stores item.
 * Assembly loop: counter 0 to sizeof(escargo_express_items)-1, checks each
 * slot, stores item at first empty (0) slot.
 * Returns item_id (nonzero) on success, 0 if full. */
uint16_t escargo_express_store(uint16_t item_id) {
    for (int i = 0; i < 36; i++) {
        if (game_state.escargo_express_items[i] == 0) {
            game_state.escargo_express_items[i] = (uint8_t)item_id;
            return item_id;
        }
    }
    return 0; /* full */
}

/* REMOVE_ESCARGO_EXPRESS_ITEM: Port of src/inventory/remove_escargo_express_item.asm.
 * Removes item at slot (1-indexed), shifts remaining items left, clears last.
 * Assembly: DEX (0-based), saves item, reads items[X+1] in loop,
 * copies to items[X], advances X while X+1 < 36 and items[X+1] != 0.
 * Clears items[X] at end. Returns removed item ID. */
uint16_t remove_escargo_express_item(uint16_t slot) {
    uint16_t idx = slot - 1; /* 0-based */
    uint8_t removed_item = game_state.escargo_express_items[idx];

    /* Shift remaining items left to fill the gap */
    for (;;) {
        if (idx >= 35) break; /* can't read beyond last slot */
        uint8_t next = game_state.escargo_express_items[idx + 1];
        if (next == 0) break;
        game_state.escargo_express_items[idx] = next;
        idx++;
    }
    game_state.escargo_express_items[idx] = 0; /* clear final position */

    return (uint16_t)removed_item;
}

/* ESCARGO_EXPRESS_MOVE: Port of asm/misc/escargo_express_move.asm.
 * Assembly: GET_CHARACTER_ITEM(char_id, item_slot) → ESCARGO_EXPRESS_STORE(item)
 *           → if failed return 0 → REMOVE_ITEM_FROM_INVENTORY(char_id, item_slot). */
uint16_t escargo_express_move(uint16_t char_id, uint16_t item_slot) {
    uint16_t item_id = get_character_item(char_id, item_slot);
    uint16_t stored = escargo_express_store(item_id);
    if (stored == 0) return 0; /* escargo full */
    return remove_item_from_inventory(char_id, item_slot);
}

/* DELIVER_ESCARGO_EXPRESS_ITEM: Port of src/inventory/deliver_escargo_express_item.asm.
 * Assembly: saves char_id(A) to @LOCAL00, REMOVE_ESCARGO_EXPRESS_ITEM(escargo_slot)
 *           → item_id, GIVE_ITEM_TO_CHARACTER(char_id, item_id), returns char_id. */
uint16_t deliver_escargo_express_item(uint16_t char_id, uint16_t escargo_slot) {
    uint16_t item_id = remove_escargo_express_item(escargo_slot);
    give_item_to_character(char_id, item_id);
    return char_id; /* assembly always returns the original char_id param */
}

/* --- Experience table --- */

#define MAX_LEVEL 99
#define EXP_LEVELS_PER_CHAR 100
#define EXP_ENTRY_SIZE 4  /* 4 bytes per .DWORD */

static const uint8_t *exp_table_data = NULL;
static size_t exp_table_size = 0;

static bool ensure_exp_table(void) {
    if (exp_table_data) return true;
    exp_table_size = ASSET_SIZE(ASSET_DATA_EXP_TABLE_BIN);
    exp_table_data = ASSET_DATA(ASSET_DATA_EXP_TABLE_BIN);
    if (!exp_table_data) {
        fprintf(stderr, "inventory: failed to load exp_table.bin\n");
        return false;
    }
    return true;
}

/* QUEUE_ITEM_FOR_CHARACTER: Port of src/inventory/queue_item_for_character.asm.
 * Stores item_id and char_id in the first empty unknownB6/B8 slot. */
void queue_item_for_character(uint16_t char_id, uint16_t item_id) {
    for (int i = 0; i < 3; i++) {
        if (game_state.unknownB6[i] == 0) {
            game_state.unknownB6[i] = (uint8_t)item_id;
            game_state.unknownB8[i] = (uint8_t)char_id;
            return;
        }
    }
}

/* GET_ITEM_EP: Port of src/inventory/get_item_ep.asm.
 * If item.type == ITEM_TYPE_BROKEN (8), returns item.params.ep (fixed item ID).
 * Otherwise returns 0. */
uint16_t get_item_ep(uint16_t item_id) {
    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry) return 0;
    if (entry->type != ITEM_TYPE_BROKEN) return 0;
    return (uint16_t)entry->params[ITEM_PARAM_EP];
}

/* TRY_FIX_BROKEN_ITEM: Port of src/inventory/try_fix_broken_item.asm.
 * Scans Jeff's inventory for broken items. For each broken item:
 *   1. Checks item.type == 8 (broken)
 *   2. Checks item.params.epi <= Jeff's IQ
 *   3. Rolls rand()%256 % 100 < fix_probability
 * On success, replaces the broken item with item.params.ep in-place.
 * Returns original broken item ID on success, 0 if nothing fixed. */
uint16_t try_fix_broken_item(uint16_t fix_probability) {
    /* Jeff must be in party (character 3, 1-indexed) */
    uint8_t jeff_in_party = 0;
    for (int i = 0; i < game_state.party_count; i++) {
        if (game_state.party_members[i] == (ITEM_FIXING_CHARACTER + 1)) {
            jeff_in_party = 1;
            break;
        }
    }
    if (!jeff_in_party) return 0;

    CharStruct *jeff = &party_characters[ITEM_FIXING_CHARACTER];

    for (int i = 0; i < ITEM_INVENTORY_SIZE; i++) {
        uint8_t item_id = jeff->items[i];
        if (item_id == 0) continue;

        const ItemConfig *entry = get_item_entry(item_id);
        if (!entry) continue;

        /* Check if item is broken (type == 8) */
        if (entry->type != ITEM_TYPE_BROKEN) continue;

        /* Check if Jeff's IQ >= item's epi (required IQ to fix) */
        uint8_t epi = entry->params[ITEM_PARAM_EPI];
        if (epi > jeff->iq) continue;

        /* Random check: rand % 100, fix succeeds if rand < probability */
        uint16_t roll = (uint16_t)((rand() % 256) % 100);
        if (roll >= fix_probability) continue;

        /* Fix the item: replace broken with fixed version */
        uint8_t fixed_item_id = entry->params[ITEM_PARAM_EP];
        jeff->items[i] = fixed_item_id;

        return (uint16_t)item_id;  /* Return original broken item ID */
    }

    return 0;  /* No item was fixed */
}

/* GET_REQUIRED_EXP: Port of asm/misc/get_required_exp.asm.
 * Returns EXP needed for next level, or 0 if at max level.
 * char_id: 1-indexed (1=Ness, 2=Paula, 3=Jeff, 4=Poo). */
uint32_t get_required_exp(uint16_t char_id) {
    if (!ensure_exp_table()) return 0;
    if (char_id < 1 || char_id > 4) return 0;

    uint16_t char_index = char_id - 1;
    uint8_t level = party_characters[char_index].level;

    if (level >= MAX_LEVEL) return 0;

    /* EXP_TABLE layout: 4 chars x 100 levels x 4 bytes.
     * Next level's cumulative EXP = table[char_index * 100 + level].
     * (level is 0-indexed in table; level N gives EXP needed TO REACH level N.
     *  So level+1's entry minus current EXP gives remaining.) */
    size_t offset = ((size_t)char_index * EXP_LEVELS_PER_CHAR + (size_t)level) * EXP_ENTRY_SIZE;
    /* Assembly adds 4 to skip to next level: ASL ASL then 4x INC */
    offset += EXP_ENTRY_SIZE;

    if (offset + EXP_ENTRY_SIZE > exp_table_size) return 0;

    uint32_t next_level_exp = read_u32_le(&exp_table_data[offset]);

    uint32_t current_exp = party_characters[char_index].exp;

    if (next_level_exp <= current_exp) return 0;
    return next_level_exp - current_exp;
}

/* --- Stats growth data (loaded from ROM data) --- */

#define STATS_GROWTH_CHARS     4
#define STATS_GROWTH_STATS     7   /* offense, defense, speed, guts, vitality, IQ, luck */
#define STATS_GROWTH_SIZE      (STATS_GROWTH_CHARS * STATS_GROWTH_STATS)
#define STAT_GAIN_MOD_SIZE     4

static const uint8_t *stats_growth_data = NULL;
static size_t stats_growth_size = 0;
static const uint8_t *stat_gain_mod_data = NULL;
static size_t stat_gain_mod_size = 0;

static bool ensure_stats_growth(void) {
    if (!stats_growth_data) {
        stats_growth_size = ASSET_SIZE(ASSET_DATA_STATS_GROWTH_VARS_BIN);
        stats_growth_data = ASSET_DATA(ASSET_DATA_STATS_GROWTH_VARS_BIN);
        if (!stats_growth_data || stats_growth_size < STATS_GROWTH_SIZE) {
            fprintf(stderr, "inventory: failed to load stats_growth_vars.bin\n");
            return false;
        }
    }
    if (!stat_gain_mod_data) {
        stat_gain_mod_size = ASSET_SIZE(ASSET_DATA_STAT_GAIN_MODIFIER_TABLE_BIN);
        stat_gain_mod_data = ASSET_DATA(ASSET_DATA_STAT_GAIN_MODIFIER_TABLE_BIN);
        if (!stat_gain_mod_data || stat_gain_mod_size < STAT_GAIN_MOD_SIZE) {
            fprintf(stderr, "inventory: failed to load stat_gain_modifier_table.bin\n");
            return false;
        }
    }
    return true;
}

/* --- PSI ability table (loaded from ROM data, for PSI learning checks) --- */

static const PsiAbility *psi_table_data_inv = NULL;
static size_t psi_table_size_inv = 0;

static bool ensure_psi_table_inv(void) {
    if (psi_table_data_inv) return true;
    psi_table_size_inv = ASSET_SIZE(ASSET_DATA_PSI_ABILITY_TABLE_BIN);
    psi_table_data_inv = (const PsiAbility *)ASSET_DATA(ASSET_DATA_PSI_ABILITY_TABLE_BIN);
    return psi_table_data_inv != NULL;
}

/* Event flag for Ness PP boost (Ness won the "Star Master" title).
 * From include/constants/event_flags.asm: FLG_WIN_OSCAR = 74. */
#define FLG_WIN_OSCAR 74

/* CALCULATE_STAT_GAIN: Port of asm/text/calculate_stat_gain.asm.
 * Computes how much a base stat should increase at level-up.
 * growth_var: from STATS_GROWTH_VARS (character+stat specific).
 * base_stat: current base value of the stat.
 * old_level: level BEFORE the level-up.
 * Returns gain amount (0 if no increase). */
static int16_t calculate_stat_gain(uint8_t growth_var, uint8_t base_stat, uint16_t old_level) {
    /* Assembly lines 20-33: diff = growth_var * old_level - (base_stat - 2) * 10 */
    int16_t diff = (int16_t)((uint16_t)growth_var * (uint16_t)old_level)
                 - (int16_t)(((int16_t)base_stat - 2) * 10);

    /* Assembly lines 36-41: if diff <= 0, return 0 */
    if (diff <= 0) return 0;

    /* Assembly lines 42-62: apply random modifier.
     * rand_val = rand() % 4 (RAND_MOD(3))
     * cycle = (old_level + 1) % 4
     * table_val = STAT_GAIN_MODIFIER_TABLE[cycle]
     * modifier = table_val + rand_val - 1
     * result = diff * modifier / 50 (signed) */
    int16_t rand_val = (int16_t)(rand() % 4);
    int16_t cycle = (int16_t)((old_level + 1) % 4);
    int16_t table_val = (int16_t)stat_gain_mod_data[cycle];
    int16_t modifier = table_val + rand_val - 1;
    int16_t result = (int16_t)((int32_t)diff * (int32_t)modifier / 50);

    return result;
}

/* Low-level vitality/IQ gain formula used when old_level < 10.
 * Port of assembly lines 265-294 / 355-382 in level_up_char.asm.
 * Returns: (growth_var * old_level - (base_stat - 2) * 10) / 10 */
static int16_t calculate_stat_gain_simple(uint8_t growth_var, uint8_t base_stat, uint16_t old_level) {
    int16_t numerator = (int16_t)((uint16_t)growth_var * (uint16_t)old_level)
                      - (int16_t)(((int16_t)base_stat - 2) * 10);
    return numerator / 10;
}

/* LEVEL_UP_CHAR: Port of asm/misc/level_up_char.asm (763 lines).
 * Increments level, applies stat growths, increases max HP/PP,
 * checks for new PSI abilities.
 * char_id: 1-indexed. play_sound_flag: passed through from GAIN_EXP. */
static void level_up_char(uint16_t char_id, uint16_t play_sound_flag) {
    if (char_id < 1 || char_id > 4) return;
    uint16_t char_index = char_id - 1;
    CharStruct *ch = &party_characters[char_index];

    /* Assembly lines 29-40: read old level, increment */
    uint16_t old_level = (uint16_t)ch->level;
    ch->level++;

    /* Assembly lines 42-59: if play_sound_flag, display "reached level XX" */
    if (play_sound_flag) {
        dt.blinking_triangle_flag = 1;
        set_battle_target_name((const char *)ch->name, 5);
        set_cnum((uint32_t)ch->level);
        display_text_from_addr(MSG_BTL8_LEVEL_UP);
        dt.blinking_triangle_flag = 2;
    }

    if (!ensure_stats_growth()) return;

    /* --- Offense growth (assembly lines 62-107) ---
     * STATS_GROWTH_VARS[char_index * 7 + 0] */
    {
        uint8_t growth = stats_growth_data[char_index * STATS_GROWTH_STATS + 0];
        int16_t gain = calculate_stat_gain(growth, ch->base_offense, old_level);
        if (gain > 0) {
            ch->base_offense += (uint8_t)gain;
            recalc_character_postmath_offense(char_id);
            if (play_sound_flag) {
                set_cnum((uint32_t)gain);
                display_text_from_addr(MSG_BTL8_LEVEL_OFFENSE_UP);
            }
        }
    }

    /* --- Defense growth (assembly lines 108-155) ---
     * STATS_GROWTH_VARS[char_index * 7 + 1] */
    {
        uint8_t growth = stats_growth_data[char_index * STATS_GROWTH_STATS + 1];
        int16_t gain = calculate_stat_gain(growth, ch->base_defense, old_level);
        if (gain > 0) {
            ch->base_defense += (uint8_t)gain;
            recalc_character_postmath_defense(char_id);
            if (play_sound_flag) {
                set_cnum((uint32_t)gain);
                display_text_from_addr(MSG_BTL8_LEVEL_DEFENSE_UP);
            }
        }
    }

    /* --- Speed growth (assembly lines 156-209) ---
     * STATS_GROWTH_VARS[char_index * 7 + 2] */
    {
        uint8_t growth = stats_growth_data[char_index * STATS_GROWTH_STATS + 2];
        int16_t gain = calculate_stat_gain(growth, ch->base_speed, old_level);
        if (gain > 0) {
            ch->base_speed += (uint8_t)gain;
            recalc_character_postmath_speed(char_id);
            if (play_sound_flag) {
                set_cnum((uint32_t)gain);
                display_text_from_addr(MSG_BTL8_LEVEL_SPEED_UP);
            }
        }
    }

    /* --- Guts growth (assembly lines 210-259) ---
     * STATS_GROWTH_VARS[char_index * 7 + 3] */
    {
        uint8_t growth = stats_growth_data[char_index * STATS_GROWTH_STATS + 3];
        int16_t gain = calculate_stat_gain(growth, ch->base_guts, old_level);
        if (gain > 0) {
            ch->base_guts += (uint8_t)gain;
            recalc_character_postmath_guts(char_id);
            if (play_sound_flag) {
                set_cnum((uint32_t)gain);
                display_text_from_addr(MSG_BTL8_LEVEL_GUTS_UP);
            }
        }
    }

    /* --- Vitality growth (assembly lines 260-349) ---
     * Uses CALCULATE_STAT_GAIN for level >= 10, simpler formula for level < 10.
     * STATS_GROWTH_VARS[char_index * 7 + 4] */
    {
        uint8_t growth = stats_growth_data[char_index * STATS_GROWTH_STATS + 4];
        int16_t gain;
        if (old_level < 10) {
            /* Assembly lines 265-294: simple formula */
            gain = calculate_stat_gain_simple(growth, ch->base_vitality, old_level);
        } else {
            /* Assembly lines 296-318: standard CALCULATE_STAT_GAIN */
            gain = calculate_stat_gain(growth, ch->base_vitality, old_level);
        }
        if (gain > 0) {  /* Assembly: CLC; SBC #0; BRANCHLTEQS — skips gain <= 0 */
            ch->base_vitality += (uint8_t)gain;
            recalc_character_postmath_vitality(char_id);
            if (play_sound_flag) {
                set_cnum((uint32_t)gain);
                display_text_from_addr(MSG_BTL8_LEVEL_VITALITY_UP);
            }
        }
    }

    /* --- IQ growth (assembly lines 350-435) ---
     * Same level<10 branching as vitality.
     * STATS_GROWTH_VARS[char_index * 7 + 5] */
    {
        uint8_t growth = stats_growth_data[char_index * STATS_GROWTH_STATS + 5];
        int16_t gain;
        if (old_level < 10) {
            gain = calculate_stat_gain_simple(growth, ch->base_iq, old_level);
        } else {
            gain = calculate_stat_gain(growth, ch->base_iq, old_level);
        }
        if (gain > 0) {  /* Assembly: CLC; SBC #0; BRANCHLTEQS — skips gain <= 0 */
            ch->base_iq += (uint8_t)gain;
            recalc_character_postmath_iq(char_id);
            if (play_sound_flag) {
                set_cnum((uint32_t)gain);
                display_text_from_addr(MSG_BTL8_LEVEL_IQ_UP);
            }
        }
    }

    /* --- Luck growth (assembly lines 436-489) ---
     * STATS_GROWTH_VARS[char_index * 7 + 6] */
    {
        uint8_t growth = stats_growth_data[char_index * STATS_GROWTH_STATS + 6];
        int16_t gain = calculate_stat_gain(growth, ch->base_luck, old_level);
        if (gain > 0) {
            ch->base_luck += (uint8_t)gain;
            recalc_character_postmath_luck(char_id);
            if (play_sound_flag) {
                set_cnum((uint32_t)gain);
                display_text_from_addr(MSG_BTL8_LEVEL_LUCK_UP);
            }
        }
    }

    /* --- Max HP increase (assembly lines 490-540) ---
     * hp_potential = vitality * 15 - max_hp
     * If hp_potential > 2: hp_increase = hp_potential
     * Otherwise: hp_increase = rand()%3 + 1 (i.e., 1-3) */
    {
        int16_t hp_potential = (int16_t)ch->vitality * 15 - (int16_t)ch->max_hp;
        uint16_t hp_increase;
        if (hp_potential > 1) {
            hp_increase = (uint16_t)hp_potential;
        } else {
            hp_increase = (uint16_t)(rand() % 3) + 1;  /* RAND_MOD(2) + 1 */
        }
        ch->max_hp += hp_increase;
        ch->current_hp_target += hp_increase;
        if (play_sound_flag) {
            set_cnum((uint32_t)hp_increase);
            display_text_from_addr(MSG_BTL8_LEVEL_MAX_HP_UP);
        }
    }

    /* --- Max PP increase (assembly lines 541-626) ---
     * Skip for Jeff (char_index == 2).
     * For Ness with FLG_WIN_OSCAR: pp_base = iq * 2
     * Otherwise: pp_base = iq
     * pp_potential = pp_base * 5 - max_pp
     * If pp_potential > 2: pp_increase = pp_potential
     * Otherwise: pp_increase = rand()%3 (i.e., 0-2) */
    if (char_index != 2) {
        uint16_t pp_base = (uint16_t)ch->iq;
        if (char_index == 0 && event_flag_get(FLG_WIN_OSCAR)) {
            pp_base <<= 1;  /* ASL: iq * 2 */
        }
        int16_t pp_potential = (int16_t)(pp_base * 5) - (int16_t)ch->max_pp;
        uint16_t pp_increase;
        if (pp_potential > 1) {
            pp_increase = (uint16_t)pp_potential;
        } else {
            pp_increase = (uint16_t)(rand() % 3);  /* RAND_MOD(2) */
        }
        if (pp_increase != 0) {
            ch->max_pp += pp_increase;
            ch->current_pp_target += pp_increase;
            if (play_sound_flag) {
                set_cnum((uint32_t)pp_increase);
                display_text_from_addr(MSG_BTL8_LEVEL_MAX_PP_UP);
            }
        }
    }

    /* --- PSI learning check (assembly lines 627-757) ---
     * Only when play_sound_flag is set (battle context).
     * For Ness/Paula/Poo, check all PSI abilities to see if any are learned
     * at the new level. */
    if (play_sound_flag && ensure_psi_table_inv()) {
        /* Only Ness (0), Paula (1), and Poo (3) can learn PSI */
        bool can_learn_psi = (char_index == 0 || char_index == 1 || char_index == 3);

        if (can_learn_psi) {
            uint8_t new_level = ch->level;
            for (int psi_id = 1; psi_id < PSI_MAX_ENTRIES; psi_id++) {
                if ((size_t)psi_id * sizeof(PsiAbility) + sizeof(PsiAbility) > psi_table_size_inv) break;
                const PsiAbility *psi = &psi_table_data_inv[psi_id];
                /* Assembly: if name byte == 0, end of entries */
                if (psi->name == 0) break;
                uint8_t learn_level = 0;
                if (char_index == 0) learn_level = psi->ness_level;
                else if (char_index == 1) learn_level = psi->paula_level;
                else if (char_index == 3) learn_level = psi->poo_level;
                if (learn_level == new_level) {
                    set_current_item((uint8_t)psi_id);
                    display_text_from_addr(MSG_BTL8_LEARNED_PSI);
                }
            }
        }
    }

    /* Assembly line 759-761: clear blinking prompt if play_sound_flag */
    if (play_sound_flag) {
        dt.blinking_triangle_flag = 0;
    }
}

/* Helper: read 32-bit LE from exp_table_data at byte offset. */
static uint32_t read_exp_at(size_t offset) {
    return read_u32_le(&exp_table_data[offset]);
}

/* RESET_CHAR_LEVEL_ONE: Port of asm/misc/reset_char_level_one.asm.
 * Resets a character to starting stats (level 1, base offense/defense/etc = 2,
 * HP = 30, PP = 10 or 0 for Jeff), then levels up to target_level by repeatedly
 * calling level_up_char. If set_exp is non-zero, sets EXP to match the level.
 * char_id: 1-indexed. target_level: number of levels to gain from 1.
 * set_exp: if non-zero, sets EXP from EXP_TABLE for the final level. */
void reset_char_level_one(uint16_t char_id, uint16_t target_level, uint16_t set_exp) {
    if (char_id < 1 || char_id > TOTAL_PARTY_COUNT) return;

    /* Assembly lines 11-18: compute char_index and struct offset */
    uint16_t char_index = char_id - 1;
    CharStruct *ch = &party_characters[char_index];

    /* Assembly lines 19-29: set starting stats */
    ch->level = 1;   /* STARTING_LEVEL */
    ch->base_offense = 2;   /* STARTING_STATS */
    ch->base_defense = 2;
    ch->base_speed = 2;
    ch->base_guts = 2;
    ch->base_luck = 2;
    ch->base_vitality = 2;
    ch->base_iq = 2;

    /* Assembly lines 31-34: set starting HP */
    ch->max_hp = 30;   /* STARTING_HP */
    ch->current_hp_target = 30;
    ch->current_hp = 30;

    /* Assembly lines 35-51: set starting PP (Jeff gets 0) */
    uint16_t starting_pp = (char_index == 2) ? 0 : 10;  /* STARTING_PP_JEFF : STARTING_PP */
    ch->max_pp = starting_pp;
    ch->current_pp_target = starting_pp;
    ch->current_pp = starting_pp;

    /* Assembly lines 52-54 (TXY; INY; STY @LOCAL00) just prepare char_id
     * for the recalculate calls below. No afflictions are cleared in the assembly. */

    /* Assembly lines 55-80: recalculate all post-math stats */
    recalc_character_postmath_offense(char_id);
    recalc_character_postmath_defense(char_id);
    recalc_character_postmath_speed(char_id);
    recalc_character_postmath_guts(char_id);
    recalc_character_postmath_luck(char_id);
    recalc_character_postmath_vitality(char_id);
    recalc_character_postmath_iq(char_id);

    /* Assembly lines 81-92: level up (target_level - 1) times */
    for (uint16_t i = 1; i < target_level; i++) {
        level_up_char(char_id, 0);  /* play_sound = 0 */
    }

    /* Assembly lines 93-125: if set_exp, set EXP from EXP_TABLE */
    if (set_exp != 0) {
        if (!ensure_exp_table()) return;
        uint8_t level = ch->level;
        /* EXP_TABLE layout: 4 chars x 100 levels x 4 bytes.
         * Offset = char_index * (MAX_LEVEL+1) * 4 + level * 4 */
        size_t offset = ((size_t)char_index * EXP_LEVELS_PER_CHAR + (size_t)level) * EXP_ENTRY_SIZE;
        if (offset + EXP_ENTRY_SIZE <= exp_table_size) {
            ch->exp = read_exp_at(offset);
        }
    }
}

/* GAIN_EXP: Port of asm/misc/gain_exp.asm.
 * A=play_sound, X=char_id, PARAM_INT32=exp_amount.
 * Adds EXP, then loops checking level thresholds. */
void gain_exp(uint16_t play_sound, uint16_t char_id, uint32_t exp_amount) {
    if (!ensure_exp_table()) return;
    if (char_id < 1 || char_id > 4) return;

    uint16_t char_index = char_id - 1;
    CharStruct *ch = &party_characters[char_index];

    /* Assembly lines 20-30: add exp to character */
    ch->exp += exp_amount;

    /* Assembly lines 33-40: check if at max level */
    uint8_t level = ch->level;
    if (level >= MAX_LEVEL) return;

    /* Assembly lines 42-66: compute next level threshold and compare.
     * EXP_TABLE layout: 4 chars x 100 levels x 4 bytes.
     * threshold = EXP_TABLE[char_index * 100 + level + 1] */
    size_t offset = ((size_t)char_index * EXP_LEVELS_PER_CHAR + (size_t)level) * EXP_ENTRY_SIZE;
    offset += EXP_ENTRY_SIZE; /* skip to next level entry */
    if (offset + EXP_ENTRY_SIZE > exp_table_size) return;

    uint32_t threshold = read_exp_at(offset);
    /* Assembly CLC;SBC comparison: level up when current_exp >= threshold */
    if (ch->exp < threshold) return;

    /* Level-up loop */
    do {
        /* Assembly lines 68-71: play level-up music if requested */
        if (play_sound != 0) {
            change_music(6); /* MUSIC::LEVEL_UP */
        }

        /* Assembly lines 72-77: call LEVEL_UP_CHAR */
        level_up_char(char_id, play_sound);

        /* Assembly lines 78-90: re-check level */
        level = ch->level;
        if (level >= MAX_LEVEL) break;

        /* Assembly lines 91-118: compute new threshold and compare */
        offset = ((size_t)char_index * EXP_LEVELS_PER_CHAR + (size_t)level) * EXP_ENTRY_SIZE;
        offset += EXP_ENTRY_SIZE;
        if (offset + EXP_ENTRY_SIZE > exp_table_size) break;

        threshold = read_exp_at(offset);
    } while (ch->exp >= threshold);
}

/* --- Financial functions --- */

/* Maximum wallet balance */
#define WALLET_LIMIT 99999u

/* ATM account limit (from include/config.asm) */
#define ATM_ACCOUNT_LIMIT 9999999u

/* INCREASE_WALLET_BALANCE: Port of asm/misc/increase_wallet_balance.asm (21 lines).
 * Adds amount to money_carried, clamped to 99999.
 * Returns the new balance. */
uint32_t increase_wallet_balance(uint32_t amount) {
    uint32_t balance = game_state.money_carried + amount;
    if (balance > WALLET_LIMIT) {
        balance = WALLET_LIMIT;
    }
    game_state.money_carried = balance;
    return balance;
}

/* DECREASE_WALLET_BALANCE: Port of asm/misc/decrease_wallet_balance.asm (27 lines).
 * Subtracts amount from money_carried.
 * Returns 0 on success, 1 if insufficient funds (balance unchanged). */
uint16_t decrease_wallet_balance(uint32_t amount) {
    if (amount > game_state.money_carried) {
        return 1; /* insufficient funds */
    }
    game_state.money_carried -= amount;
    return 0;
}

/* DEPOSIT_INTO_ATM: Port of asm/misc/atm_deposit.asm (32 lines).
 * Transfers from wallet to bank, clamped to ATM_ACCOUNT_LIMIT.
 * Returns the actual amount deposited (may be less if bank hit limit). */
uint32_t deposit_into_atm(uint32_t amount) {
    uint32_t new_balance = game_state.bank_balance + amount;
    if (new_balance > ATM_ACCOUNT_LIMIT) {
        new_balance = ATM_ACCOUNT_LIMIT;
    }
    /* Actual deposited = new_balance - old_balance */
    uint32_t actual = new_balance - game_state.bank_balance;
    game_state.bank_balance = new_balance;
    return actual;
}

/* WITHDRAW_FROM_ATM: Port of asm/misc/atm_withdraw.asm (21 lines).
 * Transfers from bank to wallet. No-op if insufficient bank balance.
 * Assembly: compares amount vs bank_balance, exits if amount > balance (BCS).
 * Otherwise subtracts amount from bank_balance. */
void withdraw_from_atm(uint32_t amount) {
    if (amount > game_state.bank_balance) {
        return; /* insufficient funds */
    }
    game_state.bank_balance -= amount;
}

/* --- Item usability query --- */

/* CHECK_ITEM_USABLE_BY: Port of src/inventory/check_item_usable_by.asm.
 * Checks if char_id (1-indexed) can use item_id by ANDing item.flags
 * with the per-character usable bitmask. Returns 1 (usable) or 0.
 * Assembly: reads item config entry, checks flags byte bits 0-3. */
static const uint8_t item_usable_flags[4] = { 0x01, 0x02, 0x04, 0x08 };

uint16_t check_item_usable_by(uint16_t char_id, uint16_t item_id) {
    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry || char_id < 1 || char_id > 4) return 0;
    uint8_t flags = entry->flags;
    return (flags & item_usable_flags[char_id - 1]) ? 1 : 0;
}

/* --- Item type query --- */

/* GET_ITEM_TYPE: Port of asm/misc/get_item_type.asm (43 lines).
 * Returns item type category:
 *   (type & 0x30) == 0x00 → 1 (consumable)
 *   (type & 0x30) == 0x10 → 2 (weapon)
 *   (type & 0x30) == 0x20 → 3 (armor)
 *   (type & 0x30) == 0x30 → 4 (accessory/other)
 *   otherwise → 0 (unusable)
 * item_id: 1-based item ID. */
uint16_t get_item_type(uint16_t item_id) {
    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry) return 0;
    uint8_t type = entry->type & 0x30;
    switch (type) {
    case 0x00: return 1;
    case 0x10: return 2;
    case 0x20: return 3;
    case 0x30: return 4;
    default:   return 0;
    }
}

/* GET_ITEM_SUBTYPE: Port of src/inventory/get_item_subtype.asm (35 lines).
 * Returns equipment slot type from item.type bits 2-3:
 *   0x00 → 1 (weapon), 0x04 → 2 (body), 0x08 → 3 (arms), 0x0C → 4 (other)
 *   anything else → 0 (not equippable).
 * item_id: 1-based item ID (passed in A, multiplied by sizeof(item)). */
uint16_t get_item_subtype(uint16_t item_id) {
    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry) return 0;
    uint8_t subtype = entry->type & EQUIP_TYPE_MASK;
    switch (subtype) {
    case EQUIP_TYPE_WEAPON: return 1;
    case EQUIP_TYPE_BODY:   return 2;
    case EQUIP_TYPE_ARMS:   return 3;
    case EQUIP_TYPE_OTHER:  return 4;
    default:                return 0;
    }
}

/* --- Condiment search --- */

/* FIND_CONDIMENT: Port of asm/misc/find_condiment.asm (66 lines).
 * Takes an item_id. First checks if the item is food-type (type & 0x3C == 0x20).
 * If so, searches the CURRENT_ATTACKER's inventory for a condiment (type & 0x3C == 0x28).
 * Returns the item_id of the first condiment found, or 0 if none. */
uint16_t find_condiment(uint16_t item_id) {
    /* Assembly lines 11-20: check if input item is food-type */
    uint8_t id = (uint8_t)(item_id & 0xFF);
    const ItemConfig *entry = get_item_entry(id);
    if (!entry) return 0;
    if ((entry->type & ITEM_TYPE_MASK) != ITEM_TYPE_FOOD) return 0;

    /* Assembly lines 21-25: get attacker's character index (0-indexed) */
    Battler *atk = battler_from_offset(bt.current_attacker);
    uint16_t char_idx = atk->id - 1;

    /* Assembly lines 26-62: loop through character's inventory for a condiment */
    for (int slot = 0; slot < ITEM_INVENTORY_SIZE; slot++) {
        uint8_t inv_item = party_characters[char_idx].items[slot];
        if (inv_item == 0) continue;
        const ItemConfig *inv_entry = get_item_entry(inv_item);
        if (!inv_entry) continue;
        /* Check if inventory item is condiment-type */
        if ((inv_entry->type & ITEM_TYPE_MASK) == ITEM_TYPE_CONDIMENT) {
            return (uint16_t)inv_item;  /* Return the condiment's item_id */
        }
    }
    return 0;
}

/* --- Inventory space query --- */

/* FIND_INVENTORY_SPACE2: Port of asm/misc/find_inventory_space2.asm (52 lines).
 * FAR wrapper. If char_id == 0xFF, searches all player-controlled party members
 * for any character with an empty inventory slot.
 * Returns char_id (1-indexed) of a character with space, 0 if all full.
 * For specific char_id, returns char_id if they have space, 0 if full. */
/* COUNT_ALIVE_PARTY_MEMBERS (asm/battle/count_alive_party_members.asm)
 *
 * Iterates player-controlled party members via party_order[],
 * counts those whose afflictions[0] (STATUS_GROUP_PERSISTENT_EASYHEAL)
 * is neither UNCONSCIOUS (1) nor DIAMONDIZED (2).
 */
uint16_t count_alive_party_members(void) {
    uint16_t alive = 0;
    uint8_t count = game_state.player_controlled_party_count;
    for (int i = 0; i < count; i++) {
        uint8_t member = game_state.party_order[i];
        CharStruct *ch = &party_characters[member - 1];
        uint8_t status = ch->afflictions[0];  /* STATUS_GROUP_PERSISTENT_EASYHEAL */
        if (status != 1 && status != 2) {  /* Not UNCONSCIOUS, not DIAMONDIZED */
            alive++;
        }
    }
    return alive;
}

uint16_t find_inventory_space2(uint16_t char_id) {
    if (char_id != 0xFF) {
        uint16_t slot = find_empty_inventory_slot(char_id);
        return (slot < 13) ? char_id : 0;
    }

    uint8_t count = game_state.player_controlled_party_count;
    for (int i = 0; i < count; i++) {
        uint8_t member = game_state.party_members[i];
        uint16_t slot = find_empty_inventory_slot(member);
        if (slot < 13) {
            return member;
        }
    }
    return 0;
}
