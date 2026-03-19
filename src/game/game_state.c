#include "game/game_state.h"
#include "game/battle.h"
#include "core/memory.h"
#include "data/assets.h"
#include "entity/entity.h"
#include "platform/platform.h"
#include <string.h>

GameState game_state;
CharStruct party_characters[TOTAL_PARTY_COUNT];
uint8_t event_flags[EVENT_FLAG_COUNT / 8];
uint8_t fastest_hppp_meter_speed;
uint8_t current_save_slot;

void game_state_init(void) {
    memset(&game_state, 0, sizeof(game_state));
    memset(party_characters, 0, sizeof(party_characters));
    memset(event_flags, 0, sizeof(event_flags));

    /* Defaults */
    game_state.text_speed = 2; /* medium */
    game_state.sound_setting = 0; /* stereo */
}

bool event_flag_get(uint16_t flag) {
    /* Assembly GET_EVENT_FLAG (get_event_flag.asm:10) does DEC first:
     * flag IDs are 1-based (0 = EVENT_FLAG::NONE sentinel). */
    if (flag == 0 || flag > EVENT_FLAG_COUNT) return false;
    flag--;
    return (event_flags[flag / 8] >> (flag % 8)) & 1;
}

void event_flag_set(uint16_t flag) {
    /* Assembly SET_EVENT_FLAG (set_event_flag.asm:15) does DEC first:
     * flag IDs are 1-based (0 = EVENT_FLAG::NONE sentinel). */
    if (flag == 0 || flag > EVENT_FLAG_COUNT) return;
    flag--;
    event_flags[flag / 8] |= (1 << (flag % 8));
}

void event_flag_clear(uint16_t flag) {
    /* Assembly SET_EVENT_FLAG (set_event_flag.asm:15) does DEC first:
     * flag IDs are 1-based (0 = EVENT_FLAG::NONE sentinel). */
    if (flag == 0 || flag > EVENT_FLAG_COUNT) return;
    flag--;
    event_flags[flag / 8] &= ~(1 << (flag % 8));
}

/* Port of CLEAR_ALL_STATUS_EFFECTS (asm/misc/clear_all_status_effects.asm).
 * Clears all entity surface flags and party member afflictions. */
void clear_all_status_effects(void) {
    /* Assembly lines 8-20: clear ENTITY_SURFACE_FLAGS for all entities */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entities.surface_flags[ENT(i)] = 0;
    }

    /* Assembly lines 21-54: clear afflictions for all 6 party members */
    for (int member = 0; member < TOTAL_PARTY_COUNT; member++) {
        for (int group = 0; group < AFFLICTION_GROUP_COUNT; group++) {
            party_characters[member].afflictions[group] = 0;
        }
    }

    /* Assembly lines 55-57: clear party_status */
    game_state.party_status = 0;
}

/* CHECK_CHARACTER_IN_PARTY: Port of asm/battle/check_character_in_party.asm.
 * Searches party_members[0..party_count-1] for char_id.
 * Returns char_id if found, 0 otherwise. */
uint16_t check_character_in_party(uint16_t char_id) {
    uint8_t count = game_state.party_count;
    for (int i = 0; i < count; i++) {
        if (game_state.party_members[i] == char_id)
            return char_id;
    }
    return 0;
}

/* CHECK_STATUS_GROUP: Port of asm/misc/check_status_group.asm.
 * A=status_group (1-7=affliction index, 8=party_status).
 * X=char_id (1-indexed).
 * Returns affliction_value+1 (so 0 means "not in party", 1 means "affliction==0").
 *
 * NOTE: The original assembly (asm/misc/check_status_group.asm) has a bug:
 * it passes status_group (not char_id) to CHECK_CHARACTER_IN_PARTY, and computes
 * the affliction address as (status_group-1)*95 + (char_id-1) instead of the
 * correct (char_id-1)*95 + (status_group-1). The C port uses the correct formula,
 * which means it diverges from the original game's behavior. This is an intentional
 * fix of an original game bug. */
uint16_t check_status_group(uint16_t status_group, uint16_t char_id) {
    /* Assembly line 15: CPY #8 → if char_id==8, return party_status+1 */
    if (char_id == 8) {
        return (uint16_t)((game_state.party_status & 0xFF) + 1);
    }
    /* Assembly line 22-25: check if character is in party */
    if (check_character_in_party(char_id) == 0)
        return 0;
    /* Assembly lines 26-43: compute afflictions[group-1]+1 */
    if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT && status_group >= 1 && status_group <= 7) {
        uint8_t affliction = party_characters[char_id - 1].afflictions[status_group - 1];
        return (uint16_t)(affliction + 1);
    }
    return 0;
}

/* INFLICT_STATUS_NONBATTLE: Port of asm/misc/inflict_status_nonbattle.asm.
 * A=char_id, X=status_group, Y=value.
 * If group==8, sets party_status = value-1 and returns char_id.
 * Otherwise sets afflictions[group-1] = value-1 for char_id.
 * Returns char_id on success, 0 if character not in party. */
uint16_t inflict_status_nonbattle(uint16_t char_id, uint16_t status_group, uint16_t value) {
    /* Assembly lines 22-30: party status special case */
    if (status_group == 8) {
        game_state.party_status = (uint8_t)(value - 1);
        return char_id;
    }
    /* Assembly lines 32-35: check character is in party */
    if (check_character_in_party(char_id) == 0)
        return 0;
    /* Assembly lines 36-54: set affliction value */
    if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT && status_group >= 1 && status_group <= 7) {
        party_characters[char_id - 1].afflictions[status_group - 1] = (uint8_t)(value - 1);
    }
    render_and_disable_entities();
    return char_id;
}

/* LEARN_SPECIAL_PSI: Port of asm/misc/learn_special_psi.asm.
 * Sets the corresponding bit in game_state.party_psi.
 * The assembly maps: 1→bit0(TELEPORT_ALPHA), 2→bit1(STARSTORM_ALPHA),
 * 3→bit2(STARSTORM_OMEGA), 4→bit3(TELEPORT_BETA). */
void learn_special_psi(uint16_t psi_type) {
    if (psi_type >= 1 && psi_type <= 4) {
        game_state.party_psi |= (1 << (psi_type - 1));
    }
}

static const PsiAbility *psi_ability_table_data = NULL;
static size_t psi_ability_table_size = 0;

static bool ensure_psi_ability_table(void) {
    if (psi_ability_table_data) return true;
    psi_ability_table_size = ASSET_SIZE(ASSET_DATA_PSI_ABILITY_TABLE_BIN);
    psi_ability_table_data = (const PsiAbility *)ASSET_DATA(ASSET_DATA_PSI_ABILITY_TABLE_BIN);
    return psi_ability_table_data != NULL;
}

/* CHECK_IF_PSI_KNOWN: Port of asm/misc/check_if_psi_known.asm.
 * A=char_id (PARTY_MEMBER enum), X=psi_ability_id.
 * Returns 1 if character's level >= PSI learning level, 0 otherwise. */
uint16_t check_if_psi_known(uint16_t char_id, uint16_t psi_ability_id) {
    if (!ensure_psi_ability_table()) return 0;
    if (psi_ability_id >= PSI_MAX_ENTRIES) return 0;

    const PsiAbility *psi = &psi_ability_table_data[psi_ability_id];

    /* Assembly dispatch: NESS=1→ness_level, PAULA=2→paula_level, POO=4→poo_level */
    uint8_t learn_level;
    switch (char_id) {
    case 1:  learn_level = psi->ness_level; break;
    case 2:  learn_level = psi->paula_level; break;
    case 4:  learn_level = psi->poo_level; break;
    default: return 0;
    }

    /* Assembly line 60: if learn_level == 0, ability is not learnable */
    if (learn_level == 0) return 0;

    /* Assembly lines 68-73: compare learn_level with character's current level */
    if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT) {
        uint8_t current_level = party_characters[char_id - 1].level;
        if (learn_level <= current_level)
            return 1;
    }
    return 0;
}

/* --- HP/PP target modification functions --- */

/* HEAL_CHARACTER_HP: Port of asm/battle/heal_character_hp.asm (73 lines).
 * A=char_id, X=amount, Y=mode (0=percent, nonzero=absolute).
 * Modifies current_hp_target, wakes from KO, clamps to max_hp. */
void heal_character_hp(uint16_t char_id, uint16_t amount, uint16_t mode) {
    /* Assembly line 13: BEQ → if char_id==0, do nothing */
    if (char_id == 0) return;
    uint16_t idx = char_id - 1;
    if (idx >= TOTAL_PARTY_COUNT) return;
    CharStruct *ch = &party_characters[idx];

    /* Assembly lines 17-30: if mode==0, convert percentage to absolute */
    uint16_t heal = amount;
    if (mode == 0) {
        heal = (uint16_t)((uint32_t)ch->max_hp * (uint32_t)amount / 100);
    }

    /* Assembly lines 31-42: hp_target += heal */
    ch->current_hp_target += heal;

    /* Assembly lines 43-50: if current_hp == 0, set to 1 (wake from KO) */
    if (ch->current_hp == 0) {
        ch->current_hp = 1;
    }

    /* Assembly lines 52-70: clamp hp_target to max_hp */
    if (ch->current_hp_target > ch->max_hp) {
        ch->current_hp_target = ch->max_hp;
    }
}

/* HEAL_CHARACTER_PP: Port of asm/battle/heal_character_pp.asm (58 lines).
 * A=char_id, X=amount, Y=mode. Modifies current_pp_target, clamps to max_pp. */
void heal_character_pp(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0) return;
    uint16_t idx = char_id - 1;
    if (idx >= TOTAL_PARTY_COUNT) return;
    CharStruct *ch = &party_characters[idx];

    uint16_t heal = amount;
    if (mode == 0) {
        heal = (uint16_t)((uint32_t)ch->max_pp * (uint32_t)amount / 100);
    }

    /* Assembly lines 31-44: pp_target += heal */
    ch->current_pp_target += heal;

    /* Assembly lines 45-55: clamp pp_target to max_pp */
    if (ch->current_pp_target > ch->max_pp) {
        ch->current_pp_target = ch->max_pp;
    }
}

/* REDUCE_HP_TARGET: Port of asm/battle/reduce_hp_target.asm (47 lines).
 * A=char_id, X=amount, Y=mode. Subtracts from hp_target, floors at 0. */
void reduce_hp_target(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0) return;
    uint16_t idx = char_id - 1;
    if (idx >= TOTAL_PARTY_COUNT) return;
    CharStruct *ch = &party_characters[idx];

    uint16_t damage = amount;
    if (mode == 0) {
        damage = (uint16_t)((uint32_t)ch->max_hp * (uint32_t)amount / 100);
    }

    /* Assembly lines 30-40: hp_target -= damage */
    uint16_t new_target = ch->current_hp_target - damage;
    /* Assembly lines 41-44: unsigned underflow check (result > max_hp → set 0) */
    if (new_target > ch->max_hp) {
        new_target = 0;
    }
    ch->current_hp_target = new_target;
}

/* REDUCE_PP_TARGET: Port of asm/battle/reduce_pp_target.asm (47 lines).
 * A=char_id, X=amount, Y=mode. Subtracts from pp_target, floors at 0. */
void reduce_pp_target(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0) return;
    uint16_t idx = char_id - 1;
    if (idx >= TOTAL_PARTY_COUNT) return;
    CharStruct *ch = &party_characters[idx];

    uint16_t cost = amount;
    if (mode == 0) {
        cost = (uint16_t)((uint32_t)ch->max_pp * (uint32_t)amount / 100);
    }

    uint16_t new_target = ch->current_pp_target - cost;
    if (new_target > ch->max_pp) {
        new_target = 0;
    }
    ch->current_pp_target = new_target;
}

/* RECOVER_HP_AMTPERCENT: Port of asm/misc/recover_hp_amtpercent.asm.
 * A=char_id (0xFF=all), X=amount, Y=mode. */
void recover_hp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0xFF) {
        for (int i = 0; i < game_state.player_controlled_party_count; i++) {
            uint8_t member = game_state.party_members[i];
            heal_character_hp((uint16_t)member, amount, mode);
        }
    } else {
        heal_character_hp(char_id, amount, mode);
    }
}

/* RECOVER_PP_AMTPERCENT: Port of asm/misc/recover_pp_amtpercent.asm. */
void recover_pp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0xFF) {
        for (int i = 0; i < game_state.player_controlled_party_count; i++) {
            uint8_t member = game_state.party_members[i];
            heal_character_pp((uint16_t)member, amount, mode);
        }
    } else {
        heal_character_pp(char_id, amount, mode);
    }
}

/* REDUCE_HP_AMTPERCENT: Port of asm/misc/reduce_hp_amtpercent.asm. */
void reduce_hp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0xFF) {
        for (int i = 0; i < game_state.player_controlled_party_count; i++) {
            uint8_t member = game_state.party_members[i];
            reduce_hp_target((uint16_t)member, amount, mode);
        }
    } else {
        reduce_hp_target(char_id, amount, mode);
    }
}

/* REDUCE_PP_AMTPERCENT: Port of asm/misc/reduce_pp_amtpercent.asm. */
void reduce_pp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0xFF) {
        for (int i = 0; i < game_state.player_controlled_party_count; i++) {
            uint8_t member = game_state.party_members[i];
            reduce_pp_target((uint16_t)member, amount, mode);
        }
    } else {
        reduce_pp_target(char_id, amount, mode);
    }
}

/* RESET_HPPP_ROLLING: Port of asm/misc/reset_hppp_rolling.asm.
 * For each active party member:
 *   - If afflictions[0] != 1 && current_hp == 0: set hp_target = 1
 *   - If HP fraction rolling && current_hp > hp_target: snap hp_target = current_hp
 *   - If PP fraction rolling && current_pp > pp_target: snap pp_target = current_pp
 * Then sets fastest_hppp_meter_speed = 1. */
void reset_hppp_rolling(void) {
    uint8_t count = game_state.player_controlled_party_count;
    for (int i = 0; i < count; i++) {
        uint8_t member_id = game_state.party_members[i];
        if (member_id == 0) continue;
        CharStruct *ch = &party_characters[member_id - 1];

        /* Assembly lines 28-35: check afflictions[0] and current_hp */
        if (ch->afflictions[0] != 1 && ch->current_hp == 0) {
            ch->current_hp_target = 1;
        }

        /* Assembly lines 37-50: if HP fraction active, snap target up */
        if (ch->current_hp_fraction != 0) {
            if (ch->current_hp > ch->current_hp_target) {
                ch->current_hp_target = ch->current_hp;
            }
        }

        /* Assembly lines 52-65: if PP fraction active, snap target up */
        if (ch->current_pp_fraction != 0) {
            if (ch->current_pp > ch->current_pp_target) {
                ch->current_pp_target = ch->current_pp;
            }
        }
    }
    /* Assembly lines 75-78: set fastest meter speed */
    fastest_hppp_meter_speed = 1;
}

/* Compute ADD checksum for a save block.
 * Port of CALC_SAVE_BLOCK_ADD_CHECKSUM (asm/system/saves/calc_save_block_checksum.asm):
 * sums every byte of the save data (excluding the header). */
static uint16_t compute_add_checksum(const SaveBlock *block) {
    uint16_t sum = 0;
    const uint8_t *data = (const uint8_t *)&block->game_state;
    size_t size = sizeof(SaveBlock) - sizeof(SaveHeader);
    for (size_t i = 0; i < size; i++) {
        sum += data[i];
    }
    return sum;
}

/* Compute XOR checksum for a save block.
 * Port of CALC_SAVE_BLOCK_XOR_CHECKSUM (asm/system/saves/calc_save_block_checksum_complement.asm):
 * XORs every 16-bit word of the save data (excluding the header). */
static uint16_t compute_xor_checksum(const SaveBlock *block) {
    uint16_t xor_sum = 0;
    const uint16_t *data = (const uint16_t *)&block->game_state;
    size_t word_count = (sizeof(SaveBlock) - sizeof(SaveHeader)) / 2;
    for (size_t i = 0; i < word_count; i++) {
        xor_sum ^= data[i];
    }
    return xor_sum;
}

bool save_game(int slot) {
    if (slot < 0 || slot >= SAVE_COUNT) return false;

    SaveBlock *block = &ert.save_scratch;
    memset(block, 0, sizeof(*block));

    /* Port of save_game_block.asm lines 22-23: TIMER → game_state.timer */
    game_state.timer = core.play_timer;

    /* Fill save block */
    memcpy(&block->game_state, &game_state, sizeof(GameState));
    memcpy(block->party_characters, party_characters, sizeof(party_characters));
    memcpy(block->event_flags, event_flags, sizeof(block->event_flags));

    /* Compute checksums — assembly uses ADD + XOR, NOT ADD + ~ADD.
     * See validate_save_block_checksums.asm: checksum = ADD, checksum_complement = XOR. */
    block->header.checksum = compute_add_checksum(block);
    block->header.checksum_complement = compute_xor_checksum(block);

    /* Write both copies */
    for (int copy = 0; copy < SAVE_COPY_COUNT; copy++) {
        size_t offset = (size_t)(slot * SAVE_COPY_COUNT + copy) * sizeof(SaveBlock);
        if (!platform_save_write(block, offset, sizeof(SaveBlock)))
            return false;
    }

    return true;
}

bool load_game(int slot) {
    if (slot < 0 || slot >= SAVE_COUNT) return false;

    SaveBlock *block = &ert.save_scratch;
    bool loaded = false;

    /* Try both copies */
    for (int copy = 0; copy < SAVE_COPY_COUNT; copy++) {
        size_t offset = (size_t)(slot * SAVE_COPY_COUNT + copy) * sizeof(SaveBlock);
        if (platform_save_read(block, offset, sizeof(SaveBlock)) != sizeof(SaveBlock))
            continue;

        uint16_t checksum = compute_add_checksum(block);
        uint16_t xor_checksum = compute_xor_checksum(block);
        if (block->header.checksum == checksum &&
            block->header.checksum_complement == xor_checksum) {
            loaded = true;
            break;
        }
    }

    if (!loaded) return false;

    memcpy(&game_state, &block->game_state, sizeof(GameState));
    memcpy(party_characters, block->party_characters, sizeof(party_characters));
    memcpy(event_flags, block->event_flags, sizeof(event_flags));

    /* Port of load_game_slot.asm lines 92-93: game_state.timer → TIMER */
    core.play_timer = game_state.timer;

    return true;
}

/* Port of ERASE_SAVE (erase_save_slot.asm) + ERASE_SAVE_BLOCK (erase_save_block.asm):
 * Zeroes both copies of a save slot (primary + backup).
 * Assembly memsets each 0x500-byte block to 0 then writes SRAM_SIGNATURE;
 * we just zero the blocks (signature is an SRAM-only concept). */
bool erase_save(int slot) {
    if (slot < 0 || slot >= SAVE_COUNT) return false;

    SaveBlock *block = &ert.save_scratch;
    memset(block, 0, sizeof(*block));

    for (int copy = 0; copy < SAVE_COPY_COUNT; copy++) {
        size_t offset = (size_t)(slot * SAVE_COPY_COUNT + copy) * sizeof(SaveBlock);
        if (!platform_save_write(block, offset, sizeof(SaveBlock)))
            return false;
    }

    return true;
}
