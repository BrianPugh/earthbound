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
    game_state.sound_setting = 1; /* stereo (1-based, like the asm's menu result) */
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
 * asm A = char_id (1-indexed), X = status_group (1-7 = affliction index,
 * 8 = party_status); the C takes them as (status_group, char_id).
 * Returns affliction_value+1 (so 0 means "not in party", 1 means "affliction==0"). */
uint16_t check_status_group(uint16_t status_group, uint16_t char_id) {
    /* Assembly line 15: CPY #8 tests the group (TXY) → party_status+1 */
    if (status_group == 8) {
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

/* SRAM_SIGNATURE (asm/data/sram_signature.asm, earthbound.yml data/
 * sram_signature): the block signature string as ASCIIZ (the asset includes
 * the NUL). ERASE_SAVE_BLOCK copies it over the zeroed block header, and
 * CHECK_BLOCK_SIGNATURE STRCMPs the header against it. */
#define SRAM_BLOCK_SIGNATURE ASSET_DATA(ASSET_DATA_SRAM_SIGNATURE_BIN)
#define SRAM_BLOCK_SIGNATURE_SIZE ASSET_SIZE(ASSET_DATA_SRAM_SIGNATURE_BIN)

/* Byte offset (within the SRAM image / .srm file) of the 16-bit SRAM version
 * word, SAVE_BASE + $1FFE (CHECK_SRAM_INTEGRITY). SAVE_BASE is file offset 0. */
#define SRAM_VERSION_WORD_OFFSET 0x1FFE
/* Full on-cartridge SRAM size (CHECK_SRAM_INTEGRITY's MEMSET24 length). The 6
 * save blocks cover the first 7680 bytes; the rest stays zero apart from the
 * version word. snes9x also won't load a .srm shorter than this. */
#define SRAM_IMAGE_SIZE 0x2000

/* CORRUPTION_CHECK_RESULTS: one SAVE_SLOT_BIT_FLAGS bit per slot whose 2 copies
 * both failed their checksums at boot. Set by check_sram_integrity(), shown and
 * cleared by the file-select list (file_select.c, corruption_check). */
uint8_t corruption_check_results;

/* Platform save I/O note: every routine below goes through platform_save_read/
 * write, one 1280-byte block (or the 512-byte SRAM tail) per write, using
 * ert.save_scratch as the only buffer. On the flash backends each write is a
 * sector erase + program, so the boot check writes nothing when SRAM is intact;
 * repairs only happen when a block is actually bad, and the setup menus write
 * only on a button press (never per frame). */

static size_t sram_block_offset(int block) {
    return (size_t)block * sizeof(SaveBlock);
}

/* Read one save block (0-5). A short read (a .srm shorter than the block) leaves
 * the missing bytes zero, like the zeroed SRAM it stands for. */
static void sram_read_block(int block, SaveBlock *buf) {
    memset(buf, 0, sizeof(*buf));
    platform_save_read(buf, sram_block_offset(block), sizeof(*buf));
}

static bool sram_write_block(int block, const SaveBlock *buf) {
    return platform_save_write(buf, sram_block_offset(block), sizeof(*buf));
}

/* ERASE_SAVE_BLOCK (asm/system/saves/erase_save_block.asm): MEMSET24 the block
 * to 0, then MEMCPY24 STRLEN(SRAM_SIGNATURE) bytes of the signature over it.
 * The asset's NUL lands on a byte that is already 0. */
static void make_erased_block(SaveBlock *buf) {
    memset(buf, 0, sizeof(*buf));
    memcpy(buf->header.signature, SRAM_BLOCK_SIGNATURE, SRAM_BLOCK_SIGNATURE_SIZE);
}

static bool erase_save_block(int block) {
    SaveBlock *buf = &ert.save_scratch;
    make_erased_block(buf);
    return sram_write_block(block, buf);
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

/* VALIDATE_SAVE_BLOCK_CHECKSUMS (validate_save_block_checksums.asm), as a bool
 * (the asm returns 0 for valid, -1 for a mismatch). An erased block (all-zero
 * data, checksums 0) is valid. */
static bool block_checksums_valid(const SaveBlock *block) {
    return block->header.checksum == compute_add_checksum(block) &&
           block->header.checksum_complement == compute_xor_checksum(block);
}

/* STRCMP(SRAM_SIGNATURE, block): equal through the signature's NUL. */
static bool block_signature_valid(const SaveBlock *block) {
    return memcmp(block->header.signature, SRAM_BLOCK_SIGNATURE, SRAM_BLOCK_SIGNATURE_SIZE) == 0;
}

/* CHECK_SAVE_CORRUPTION (asm/system/saves/check_save_corruption.asm) for one
 * 0-based slot: a bad primary is erased and refilled from the backup (COPY_SAVE_
 * BLOCK); a bad backup is erased and refilled from the primary; when both are
 * bad, both stay erased and the slot's bit is set in CORRUPTION_CHECK_RESULTS.
 * The asm erases a block before copying over it; the copy rewrites all 1280
 * bytes, so only the copy is written here (same final SRAM, one flash write). */
static void check_save_corruption(int slot) {
    SaveBlock *buf = &ert.save_scratch;
    int primary = slot * SAVE_COPY_COUNT;
    int backup = primary + 1;

    sram_read_block(primary, buf);
    if (!block_checksums_valid(buf)) {
        sram_read_block(backup, buf);
        if (block_checksums_valid(buf)) {
            sram_write_block(primary, buf);     /* @RESTORE_FROM_BACKUP */
            return; /* @CHECK_BACKUP_BLOCK re-validates the good backup: no-op */
        }
        erase_save_block(primary);
        erase_save_block(backup);
        corruption_check_results |= (uint8_t)(1u << slot);   /* SAVE_SLOT_BIT_FLAGS */
        return;
    }
    /* @CHECK_BACKUP_BLOCK, then COPY_SAVE_BLOCK(backup <- primary) if bad. One
     * scratch block only (the Pico's core-0 stack has no room for a second). */
    sram_read_block(backup, buf);
    if (!block_checksums_valid(buf)) {
        sram_read_block(primary, buf);
        sram_write_block(backup, buf);
    }
}

/* Port of CHECK_SRAM_INTEGRITY (asm/system/saves/check_sram_integrity.asm), the
 * first thing GAME_INIT does:
 *   1. version word at $1FFE != SRAM_VERSION -> MEMSET24 all 8 KB to 0;
 *   2. CHECK_ALL_BLOCKS_SIGNATURE: CHECK_BLOCK_SIGNATURE erases (ERASE_SAVE_BLOCK)
 *      every block whose header STRCMPs unequal to SRAM_SIGNATURE;
 *   3. CORRUPTION_CHECK_RESULTS = 0, CHECK_SAVE_CORRUPTION on slots 0-2;
 *   4. store SRAM_VERSION at $1FFE.
 * The wipe is written in its final form: the 6 zeroed blocks fail step 2 and are
 * erased (signed), so they are written signed straight away, and the zeroed tail
 * is written last together with the version word. When the version word is
 * already right, step 4 rewrites the same value and is skipped, so an intact SRAM
 * costs no write at all (the flash backends erase a sector per write). */
void check_sram_integrity(void) {
    uint8_t ver[2] = { 0, 0 };
    bool wipe = platform_save_read(ver, SRAM_VERSION_WORD_OFFSET, sizeof(ver)) != sizeof(ver) ||
                (uint16_t)(ver[0] | (ver[1] << 8)) != SRAM_VERSION;

    for (int block = 0; block < SAVE_COUNT * SAVE_COPY_COUNT; block++) {
        if (!wipe) {
            sram_read_block(block, &ert.save_scratch);
            if (block_signature_valid(&ert.save_scratch))
                continue;
        }
        erase_save_block(block);
    }

    corruption_check_results = 0;
    for (int slot = 0; slot < SAVE_COUNT; slot++)
        check_save_corruption(slot);

    if (wipe) {
        uint8_t *tail = (uint8_t *)&ert.save_scratch;
        size_t tail_size = SRAM_IMAGE_SIZE - SAVE_FILE_SIZE;
        _Static_assert(SRAM_IMAGE_SIZE - SAVE_FILE_SIZE <= sizeof(SaveBlock),
                       "SRAM tail must fit the save scratch block");
        memset(tail, 0, tail_size);
        tail[SRAM_VERSION_WORD_OFFSET - SAVE_FILE_SIZE] = (uint8_t)(SRAM_VERSION & 0xFF);
        tail[SRAM_VERSION_WORD_OFFSET - SAVE_FILE_SIZE + 1] = (uint8_t)(SRAM_VERSION >> 8);
        platform_save_write(tail, SAVE_FILE_SIZE, tail_size);
    }
}

/* SAVE_GAME_SLOT (save_game_slot.asm) + SAVE_GAME_BLOCK (save_game_block.asm):
 * writes the game into both blocks of a 0-based slot. SAVE_GAME_BLOCK writes
 * only game_state, the 6 char_structs and event_flags into SRAM; the header
 * signature (from the boot check or an erase) and the trailing padding keep
 * what the block held, and the ADD/XOR checksums are taken over the block as
 * it then stands, padding included. So each copy is read back, patched and
 * rewritten. */
bool save_game(int slot) {
    if (slot < 0 || slot >= SAVE_COUNT) return false;

    SaveBlock *block = &ert.save_scratch;

    /* Port of save_game_block.asm lines 22-23: TIMER → game_state.timer */
    game_state.timer = core.play_timer;

    for (int copy = 0; copy < SAVE_COPY_COUNT; copy++) {
        int b = slot * SAVE_COPY_COUNT + copy;
        sram_read_block(b, block);

        memcpy(&block->game_state, &game_state, sizeof(GameState));
        memcpy(block->party_characters, party_characters, sizeof(party_characters));
        memcpy(block->event_flags, event_flags, sizeof(block->event_flags));

        /* Checksums — assembly uses ADD + XOR, NOT ADD + ~ADD.
         * See validate_save_block_checksums.asm: checksum = ADD, checksum_complement = XOR. */
        block->header.checksum = compute_add_checksum(block);
        block->header.checksum_complement = compute_xor_checksum(block);

        if (!sram_write_block(b, block))
            return false;
    }
    return true;
}

/* Port of LOAD_GAME_SLOT (asm/system/saves/load_game_slot.asm): copies the
 * slot's primary block (at slot * $A00) into game_state, party_characters and
 * event_flags without checking it. The boot check (check_sram_integrity) has
 * already repaired a bad primary from the backup, or erased both copies, so the
 * primary is always valid by the time anything loads it. Returns false only when
 * the slot number is out of range. */
bool load_game(int slot) {
    if (slot < 0 || slot >= SAVE_COUNT) return false;

    SaveBlock *block = &ert.save_scratch;
    sram_read_block(slot * SAVE_COPY_COUNT, block);

    memcpy(&game_state, &block->game_state, sizeof(GameState));
    memcpy(party_characters, block->party_characters, sizeof(party_characters));
    memcpy(event_flags, block->event_flags, sizeof(event_flags));

    /* Port of load_game_slot.asm lines 92-93: game_state.timer → TIMER */
    core.play_timer = game_state.timer;

    /* Save compatibility: ports before 2026-10-31 stored sound_setting 0-based
     * (0 = Stereo, 1 = Mono); the asm stores the 1-based menu result (1 = Stereo,
     * 2 = Mono), which is all the original ever writes. A 0 can only come from
     * such an old port save, so read it as Stereo instead of letting
     * FILE_SELECT_INIT's DEC turn it into SET_AUDIO_CHANNELS(-1) (mono). An old
     * port Mono (1) reads as Stereo; it can't be told apart from a new Stereo. */
    if (game_state.sound_setting == 0)
        game_state.sound_setting = 1;

    return true;
}

/* Port of ERASE_SAVE (erase_save_slot.asm): ERASE_SAVE_BLOCK on both copies of
 * a 0-based slot, which zeroes them and signs them again. */
bool erase_save(int slot) {
    if (slot < 0 || slot >= SAVE_COUNT) return false;

    for (int copy = 0; copy < SAVE_COPY_COUNT; copy++) {
        if (!erase_save_block(slot * SAVE_COPY_COUNT + copy))
            return false;
    }
    return true;
}

/* Port of COPY_SAVE (asm/system/saves/copy_save_slot.asm) + COPY_SAVE_BLOCK
 * (asm/system/saves/copy_save_block.asm): raw-copies both SRAM blocks (primary +
 * backup) of slot `from` over slot `to`, header and checksums included, with no
 * re-validation. Slots are 0-based. */
bool copy_save(int to, int from) {
    if (to < 0 || to >= SAVE_COUNT || from < 0 || from >= SAVE_COUNT) return false;

    SaveBlock *block = &ert.save_scratch;
    for (int copy = 0; copy < SAVE_COPY_COUNT; copy++) {
        sram_read_block(from * SAVE_COPY_COUNT + copy, block);
        if (!sram_write_block(to * SAVE_COPY_COUNT + copy, block))
            return false;
    }
    return true;
}
