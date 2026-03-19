#ifndef GAME_INVENTORY_H
#define GAME_INVENTORY_H

#include "core/types.h"
#include "include/binary.h"

/* Item configuration table constants.
 * 255 entries x 39 bytes (US). Entry 0 is unused; item IDs 1-254 are valid.
 * Struct layout from include/structs.asm:
 *   name[25], type(1), cost(2), flags(1), effect(2), params(4), help_text(4) = 39 bytes. */
#define ITEM_CONFIG_COUNT       255  /* entries in table (0=unused, 1-254=real items) */
#define ITEM_NAME_LEN           25
#define ITEM_INVENTORY_SIZE     14   /* slots per character */
#define CHAR_ID_ANY             0xFF /* search all player-controlled party members */

/* item_parameters sub-offsets within params[] */
#define ITEM_PARAM_STRENGTH  0
#define ITEM_PARAM_EPI       1
#define ITEM_PARAM_EP        2
#define ITEM_PARAM_SPECIAL   3

/* Item configuration entry (39 bytes, packed to match ROM binary layout) */
PACKED_STRUCT
typedef struct {
    uint8_t  name[ITEM_NAME_LEN];  /*  0: EB-encoded name */
    uint8_t  type;                 /* 25: item type (see ITEM_TYPE_* constants) */
    uint16_t cost;                 /* 26: buy price (LE) */
    uint8_t  flags;                /* 28: item flags (see ITEM_FLAG_* constants) */
    uint16_t effect_id;            /* 29: battle action table index (LE) */
    uint8_t  params[4];            /* 31: [strength, epi, ep, special] */
    uint32_t help_text;            /* 35: SNES address of help text (LE, 24-bit used) */
} ItemConfig;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(ItemConfig, 39);

/* Equipment slot indices (from EQUIPMENT_SLOT enum in enums.asm) */
#define EQUIP_WEAPON  0
#define EQUIP_BODY    1
#define EQUIP_ARMS    2
#define EQUIP_OTHER   3
#define EQUIP_COUNT   4

/* Item type byte layout (item.type):
 *   Bits 2-5 (mask 0x3C): item category (from ITEM_TYPE enum in items.asm)
 *   Bits 0-1: unused
 *   Bits 6-7: unused */
#define ITEM_TYPE_MASK             0x3C  /* bits 2-5: item category */

#define ITEM_TYPE_TEDDY_BEAR       0x04  /* ITEM_TYPE::TEDDY_BEAR */
#define ITEM_TYPE_BROKEN           0x08  /* ITEM_TYPE::BROKEN */
#define ITEM_TYPE_WEAPON_BASH      0x10  /* ITEM_TYPE::WEAPON_BASH */
#define ITEM_TYPE_WEAPON_SHOOT     0x11  /* ITEM_TYPE::WEAPON_SHOOT */
#define ITEM_TYPE_ARMOUR_BODY      0x14  /* ITEM_TYPE::ARMOUR_BODY */
#define ITEM_TYPE_ARMOUR_ARM       0x18  /* ITEM_TYPE::ARMOUR_ARM */
#define ITEM_TYPE_ARMOUR_OTHER     0x1C  /* ITEM_TYPE::ARMOUR_OTHER */
#define ITEM_TYPE_FOOD             0x20  /* ITEM_TYPE::EDIBLE */
#define ITEM_TYPE_DRINK            0x24  /* ITEM_TYPE::DRINK */
#define ITEM_TYPE_CONDIMENT        0x28  /* ITEM_TYPE::CONDIMENT */
#define ITEM_TYPE_PARTY_FOOD       0x2C  /* ITEM_TYPE::PARTY_FOOD */
#define ITEM_TYPE_HEALING          0x30  /* ITEM_TYPE::HEALING_ITEM */
#define ITEM_TYPE_BATTLE_OFFENSIVE 0x34  /* ITEM_TYPE::BATTLE_OFFENSIVE */
#define ITEM_TYPE_BATTLE_DEFENSIVE 0x35  /* ITEM_TYPE::BATTLE_DEFENSIVE */
#define ITEM_TYPE_KEY_ITEM         0x38  /* ITEM_TYPE::KEY_ITEM */
#define ITEM_TYPE_KEY_AREA         0x3A  /* ITEM_TYPE::KEY_ITEM_SPECIFIC_AREA */
#define ITEM_TYPE_KEY_SOMEONE      0x3B  /* ITEM_TYPE::KEY_ITEM_FOR_SOMEONE */

/* Item flags (from ITEM_FLAGS enum in enums.asm) */
#define ITEM_FLAG_TRANSFORM     0x10
#define ITEM_FLAG_CANNOT_GIVE   0x20
#define ITEM_FLAG_CONSUMED      0x80

/* Ensure item configuration table is loaded. Returns true if available. */
bool ensure_item_config(void);

/* Get a pointer to the item config entry.
 * item_id is used directly as a table index (1-based in inventory, 0 = unused entry).
 * Returns NULL if out of bounds or table not loaded. */
const ItemConfig *get_item_entry(uint16_t item_id);

/* --- Stat recalculation functions ---
 * All take char_id 1-indexed (1=Ness, 2=Paula, 3=Jeff, 4=Poo).
 * Read base stats + equipment modifiers, clamp, and store the computed stat. */

void recalc_character_postmath_offense(uint16_t char_id);
void recalc_character_postmath_guts(uint16_t char_id);
void recalc_character_postmath_defense(uint16_t char_id);
void recalc_character_postmath_speed(uint16_t char_id);
void recalc_character_postmath_luck(uint16_t char_id);
void recalc_character_postmath_vitality(uint16_t char_id);
void recalc_character_postmath_iq(uint16_t char_id);
void recalc_character_miss_rate(uint16_t char_id);

/* CALC_RESISTANCES: Port of asm/battle/calc_resistances.asm.
 * Calculates fire/freeze/flash/paralysis/hypnosis resistances from
 * BODY, OTHER, and ARMS equipment special parameters.
 * char_id: 1-indexed. */
void calc_resistances(uint16_t char_id);

/* --- Equipment change functions ---
 * All take char_id (1-indexed) in A and new_item_id in X.
 * Store the new item, recalculate affected stats, return old item ID. */

uint16_t change_equipped_weapon(uint16_t char_id, uint16_t new_item_id);
uint16_t change_equipped_body(uint16_t char_id, uint16_t new_item_id);
uint16_t change_equipped_arms(uint16_t char_id, uint16_t new_item_id);
uint16_t change_equipped_other(uint16_t char_id, uint16_t new_item_id);

/* --- Item access functions --- */

/* GET_CHARACTER_ITEM: Port of asm/misc/get_character_item.asm.
 * Returns the item ID at the given slot for the given character.
 * char_id: 1-indexed. slot: 1-indexed (1-14). Returns 0 if empty. */
uint16_t get_character_item(uint16_t char_id, uint16_t slot);

/* FIND_EMPTY_INVENTORY_SLOT: Port of asm/battle/find_empty_inventory_slot.asm.
 * Returns 0-based index of first empty slot in character's inventory.
 * char_id: 1-indexed.
 * Note: original assembly only checks slots 0-12 (off-by-one, slot 13 is never checked). */
uint16_t find_empty_inventory_slot(uint16_t char_id);

/* --- Item give/remove functions --- */

/* GIVE_ITEM_TO_SPECIFIC_CHARACTER: Port of asm/misc/give_item_to_specific_character.asm.
 * Finds an empty inventory slot and places the item there.
 * char_id: 1-indexed. item_id: 1-based item to give.
 * Returns char_id on success, 0 if inventory full. */
uint16_t give_item_to_specific_character(uint16_t char_id, uint16_t item_id);

/* GIVE_ITEM_TO_CHARACTER: Port of asm/misc/give_item_to_character.asm.
 * If char_id == 0xFF, tries each player-controlled party member in order.
 * Otherwise, tries the specific character.
 * Returns receiving character's member ID on success, 0 if all full. */
uint16_t give_item_to_character(uint16_t char_id, uint16_t item_id);

/* REMOVE_ITEM_FROM_INVENTORY: Port of asm/misc/remove_item_from_inventory.asm.
 * Removes item at the given slot position, shifts remaining items,
 * adjusts equipment indices, handles teddy bear and transform side effects.
 * char_id: 1-indexed. item_slot: 1-indexed position in inventory.
 * Returns char_id. */
uint16_t remove_item_from_inventory(uint16_t char_id, uint16_t item_slot);

/* EQUIP_ITEM: Port of asm/misc/equip_item.asm.
 * Reads item at the given slot, determines equipment category from item.type,
 * and calls the appropriate change_equipped_* function.
 * char_id: 1-indexed. item_slot: 1-indexed position in inventory.
 * Returns old equipment slot index (from change_equipped), or 0 if not equippable. */
uint16_t equip_item(uint16_t char_id, uint16_t item_slot);

/* SWAP_ITEM_INTO_EQUIPMENT: Port of asm/battle/swap_item_into_equipment.asm.
 * Moves an item from source character's inventory to target character.
 * Compacts source inventory, gives item to target, adjusts equipment indices.
 * source_char_id: 1-based. item_slot: 1-based. target_char_id: 1-based. */
void swap_item_into_equipment(uint16_t source_char_id, uint16_t item_slot,
                               uint16_t target_char_id);

/* TAKE_ITEM_FROM_SPECIFIC_CHARACTER: Port of asm/misc/take_item_from_specific_character.asm.
 * Searches character's inventory for item_id, removes first match.
 * char_id: 1-indexed. item_id: item to remove.
 * Returns char_id on success (via remove_item_from_inventory), 0 if not found. */
uint16_t take_item_from_specific_character(uint16_t char_id, uint16_t item_id);

/* TAKE_ITEM_FROM_CHARACTER: Port of asm/misc/take_item_from_character.asm.
 * If char_id == 0xFF, tries each player-controlled party member in order.
 * Otherwise, tries the specific character.
 * Returns character's member ID on success, 0 if item not found. */
uint16_t take_item_from_character(uint16_t char_id, uint16_t item_id);

/* --- Item search functions --- */

/* IS_ITEM_EQUIPPED_BY_ID: Port of src/inventory/is_item_equipped_by_id.asm.
 * Checks if an item (by item ID) is equipped by a character.
 * For each of the 4 equipment slots, dereferences the slot index to get the
 * actual item at that inventory position, then compares with item_id.
 * char_id: 1-indexed. item_id: item ID to check for.
 * Returns 1 if equipped, 0 if not. */
uint16_t is_item_equipped_by_id(uint16_t char_id, uint16_t item_id);

/* CHECK_ITEM_EQUIPPED: Port of asm/misc/check_item_equipped.asm.
 * Checks if a specific inventory slot is currently equipped.
 * Compares equipment slot values (1-based inventory positions) against item_slot.
 * char_id: 1-indexed. item_slot: 1-based inventory position.
 * Returns 1 if that slot is equipped, 0 if not. */
uint16_t check_item_equipped(uint16_t char_id, uint16_t item_slot);

/* FIND_ITEM_IN_INVENTORY: Port of asm/misc/find_item_in_inventory.asm.
 * Searches a single character's inventory for item_id.
 * char_id: 1-indexed. item_id: item to search for.
 * Returns char_id if found, 0 if not. */
uint16_t find_item_in_inventory(uint16_t char_id, uint16_t item_id);

/* FIND_ITEM_IN_INVENTORY2: Port of asm/misc/find_item_in_inventory2.asm.
 * If char_id == 0xFF, searches all player-controlled party members.
 * Otherwise, searches the specific character.
 * char_id: 1-indexed or 0xFF for all. item_id: item to search for.
 * Returns character's ID (1-indexed) if found, 0 if not. */
uint16_t find_item_in_inventory2(uint16_t char_id, uint16_t item_id);

/* --- Escargo Express functions --- */

/* ESCARGO_EXPRESS_STORE: Port of asm/misc/escargo_express_store.asm.
 * Finds first empty slot in escargo_express_items[] and stores item.
 * item_id: item to store (1-based).
 * Returns item_id on success, 0 if escargo express is full. */
uint16_t escargo_express_store(uint16_t item_id);

/* REMOVE_ESCARGO_EXPRESS_ITEM: Port of src/inventory/remove_escargo_express_item.asm.
 * Removes item at the given slot, shifts remaining items left.
 * slot: 1-indexed position in escargo_express_items[].
 * Returns the removed item's ID. */
uint16_t remove_escargo_express_item(uint16_t slot);

/* ESCARGO_EXPRESS_MOVE: Port of asm/misc/escargo_express_move.asm.
 * Moves item from character's inventory to escargo express.
 * Gets item at char_id's inventory slot, stores in escargo, removes from inventory.
 * char_id: 1-indexed. item_slot: 1-indexed inventory position.
 * Returns char_id on success (via remove_item_from_inventory), 0 if escargo full. */
uint16_t escargo_express_move(uint16_t char_id, uint16_t item_slot);

/* DELIVER_ESCARGO_EXPRESS_ITEM: Port of src/inventory/deliver_escargo_express_item.asm.
 * Removes item from escargo express and gives it to a character.
 * char_id: 1-indexed (or 0xFF for any party member).
 * escargo_slot: 1-indexed position in escargo_express_items[].
 * Returns char_id. */
uint16_t deliver_escargo_express_item(uint16_t char_id, uint16_t escargo_slot);

/* --- Item queue functions --- */

/* QUEUE_ITEM_FOR_CHARACTER: Port of src/inventory/queue_item_for_character.asm.
 * Stores item_id into the first empty slot of game_state.unknownB6[0..2],
 * and char_id into the corresponding game_state.unknownB8[0..2] slot.
 * Used by CC_19_1C to queue items removed from inventory/escargo for
 * later retrieval by CC_19_1D (GET_CHARACTER_ENCOUNTER_DATA).
 * char_id: 1-indexed character or 0xFF (escargo source). item_id: item to queue. */
void queue_item_for_character(uint16_t char_id, uint16_t item_id);

/* --- Item fix functions (Jeff's repair ability) --- */

/* (ITEM_TYPE_BROKEN is defined above with the other ITEM_TYPE constants) */

/* ITEM_FIXING_CHARACTER index (0-based) — Jeff */
#define ITEM_FIXING_CHARACTER  2  /* from include/config.asm */

/* GET_ITEM_EP: Port of src/inventory/get_item_ep.asm.
 * If item.type == ITEM_TYPE_BROKEN, returns item.params.ep (the fixed item ID).
 * Otherwise returns 0.
 * item_id: 1-based item ID. */
uint16_t get_item_ep(uint16_t item_id);

/* TRY_FIX_BROKEN_ITEM: Port of src/inventory/try_fix_broken_item.asm.
 * Scans Jeff's inventory for broken items. For each broken item, checks if
 * Jeff's IQ >= item.epi, then rolls a random check against fix_probability.
 * If successful, replaces the broken item with the fixed version in-place.
 * fix_probability: 0-99, higher = more likely to fix.
 * Returns the original broken item's ID on success, 0 if no item was fixed. */
uint16_t try_fix_broken_item(uint16_t fix_probability);

/* --- Experience functions --- */

/* RESET_CHAR_LEVEL_ONE: Port of asm/misc/reset_char_level_one.asm.
 * Resets character to level 1 base stats, then levels up target_level times.
 * If set_exp != 0, sets EXP from EXP_TABLE to match the final level.
 * char_id: 1-indexed. target_level: desired level. set_exp: boolean. */
void reset_char_level_one(uint16_t char_id, uint16_t target_level, uint16_t set_exp);

/* GET_REQUIRED_EXP: Port of asm/misc/get_required_exp.asm.
 * Returns EXP needed for the character's next level, or 0 if at max level.
 * char_id: 1-indexed (1=Ness, 2=Paula, 3=Jeff, 4=Poo). */
uint32_t get_required_exp(uint16_t char_id);

/* GAIN_EXP: Port of asm/misc/gain_exp.asm.
 * Adds experience to character, checks level thresholds, triggers level-ups.
 * play_sound: if non-zero, plays MUSIC::LEVEL_UP on level up.
 * char_id: 1-indexed (1=Ness, 2=Paula, 3=Jeff, 4=Poo).
 * exp: experience points to add. */
void gain_exp(uint16_t play_sound, uint16_t char_id, uint32_t exp);

/* --- Financial functions --- */

/* INCREASE_WALLET_BALANCE: Port of asm/misc/increase_wallet_balance.asm.
 * Adds amount to money_carried, clamped to 99999. Returns new balance. */
uint32_t increase_wallet_balance(uint32_t amount);

/* DECREASE_WALLET_BALANCE: Port of asm/misc/decrease_wallet_balance.asm.
 * Returns 0 on success, 1 if insufficient funds. */
uint16_t decrease_wallet_balance(uint32_t amount);

/* DEPOSIT_INTO_ATM: Port of asm/misc/atm_deposit.asm.
 * Returns actual amount deposited (may be less if bank hit limit). */
uint32_t deposit_into_atm(uint32_t amount);

/* WITHDRAW_FROM_ATM: Port of asm/misc/atm_withdraw.asm.
 * No-op if insufficient bank balance. */
void withdraw_from_atm(uint32_t amount);

/* --- Item usability query --- */

/* CHECK_ITEM_USABLE_BY: Port of src/inventory/check_item_usable_by.asm.
 * Checks if char_id (1-indexed) can use item_id by ANDing item.flags
 * with the per-character usable bitmask. Returns 1 (usable) or 0. */
uint16_t check_item_usable_by(uint16_t char_id, uint16_t item_id);

/* --- Item type query --- */

/* GET_ITEM_TYPE: Port of asm/misc/get_item_type.asm.
 * Returns category: 1=consumable, 2=weapon, 3=armor, 4=accessory, 0=other. */
uint16_t get_item_type(uint16_t item_id);

/* GET_ITEM_SUBTYPE: Port of src/inventory/get_item_subtype.asm.
 * Returns equipment slot type: 1=weapon, 2=body, 3=arms, 4=other, 0=none. */
uint16_t get_item_subtype(uint16_t item_id);

/* --- Condiment search --- */

/* FIND_CONDIMENT: Port of asm/misc/find_condiment.asm.
 * Takes item_id; checks if food-type, then searches CURRENT_ATTACKER's inventory
 * for a condiment. Returns condiment item_id, or 0 if none. */
uint16_t find_condiment(uint16_t item_id);

/* --- Inventory space query --- */

/* FIND_INVENTORY_SPACE2: Port of asm/misc/find_inventory_space2.asm.
 * char_id == 0xFF searches all party members.
 * Returns char_id with space, 0 if all full. */
uint16_t find_inventory_space2(uint16_t char_id);

/* --- Party management --- */

/* ADD_CHAR_TO_PARTY: Port of asm/misc/party_add_char.asm.
 * Sorted insertion into game_state.party_members[].
 * If char_id <= 4 (PC), also calls UPDATE_TEDDY_BEAR_PARTY and
 * INIT_ALL_ITEM_TRANSFORMATIONS. Sets entity tick/move disabled flags.
 * char_id: 1-indexed character ID. */
void add_char_to_party(uint16_t char_id);

/* REMOVE_CHAR_FROM_PARTY: Port of asm/misc/party_remove_char.asm.
 * Removes char_id from game_state.party_members[] and compacts.
 * Also calls INIT_ALL_ITEM_TRANSFORMATIONS.
 * char_id: 1-indexed character ID. */
void remove_char_from_party(uint16_t char_id);

/* --- Item transformation system --- */

/* PROCESS_ITEM_TRANSFORMATIONS: Port of asm/overworld/process_item_transformations.asm.
 * Called each frame from the overworld main loop. Manages SFX and transformation
 * countdowns. When a transformation completes, swaps items in inventory. */
void process_item_transformations(void);

/* Reset item transformation counter.
 * Port of `STZ ITEM_TRANSFORMATIONS_LOADED` in assembly.
 * Called when leaving overworld (e.g., game over screen). */
void reset_item_transformations(void);

/* --- Party utility functions --- */

/* COUNT_ALIVE_PARTY_MEMBERS: Port of asm/battle/count_alive_party_members.asm.
 * Counts player-controlled party members who are not unconscious or diamondized.
 * Uses party_order[] and player_controlled_party_count. */
uint16_t count_alive_party_members(void);

#endif /* GAME_INVENTORY_H */
