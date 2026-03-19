#ifndef GAME_STATE_H
#define GAME_STATE_H

#include "core/types.h"
#include "include/constants.h"

/* Character struct - 95 bytes (US retail), matches char_struct from structs.asm */
PACKED_STRUCT
typedef struct {
    uint8_t  name[5];           /* 0 */
    uint8_t  level;             /* 5 */
    uint32_t exp;               /* 6 */
    uint16_t max_hp;            /* 10 */
    uint16_t max_pp;            /* 12 */
    uint8_t  afflictions[AFFLICTION_GROUP_COUNT]; /* 14 */
    uint8_t  offense;           /* 21 */
    uint8_t  defense;           /* 22 */
    uint8_t  speed;             /* 23 */
    uint8_t  guts;              /* 24 */
    uint8_t  luck;              /* 25 */
    uint8_t  vitality;          /* 26 */
    uint8_t  iq;                /* 27 */
    uint8_t  base_offense;      /* 28 */
    uint8_t  base_defense;      /* 29 */
    uint8_t  base_speed;        /* 30 */
    uint8_t  base_guts;         /* 31 */
    uint8_t  base_luck;         /* 32 */
    uint8_t  base_vitality;     /* 33 */
    uint8_t  base_iq;           /* 34 */
    uint8_t  items[14];         /* 35 */
    uint8_t  equipment[4];      /* 49 */
    uint16_t unknown53;         /* 53 */
    uint16_t previous_walking_style; /* 55 */
    uint16_t unknown57;         /* 57 */
    uint16_t unknown59;         /* 59 */
    uint16_t position_index;    /* 61 */
    uint16_t unknown63;         /* 63 */
    uint16_t buffer_walking_style; /* 65 */
    uint16_t current_hp_fraction;  /* 67 */
    uint16_t current_hp;        /* 69 */
    uint16_t current_hp_target; /* 71 */
    uint16_t current_pp_fraction;  /* 73 */
    uint16_t current_pp;        /* 75 */
    uint16_t current_pp_target; /* 77 */
    uint16_t hp_pp_window_options; /* 79 */
    uint8_t  miss_rate;         /* 81 */
    uint8_t  fire_resist;       /* 82 */
    uint8_t  freeze_resist;     /* 83 */
    uint8_t  flash_resist;      /* 84 */
    uint8_t  paralysis_resist;  /* 85 */
    uint8_t  hypnosis_brainshock_resist; /* 86 */
    uint8_t  boosted_speed;     /* 87 */
    uint8_t  boosted_guts;      /* 88 */
    uint8_t  boosted_vitality;  /* 89 */
    uint8_t  boosted_iq;        /* 90 */
    uint8_t  boosted_luck;      /* 91 */
    uint8_t  unused_92;         /* 92: write-only dead storage (never read) */
    uint8_t  unused_93;         /* 93: write-only dead storage (never read) */
    uint8_t  unknown94;         /* 94 */
} CharStruct;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(CharStruct, 95);

/* Photo state entry - 8 bytes */
PACKED_STRUCT
typedef struct {
    uint16_t unknown;
    uint8_t  party[6];
} PhotoState;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(PhotoState, 8);

/* Game state - 473 bytes (US retail), matches game_state from structs.asm */
PACKED_STRUCT
typedef struct {
    uint8_t  mother2_playername[12];   /* 0 */
    uint8_t  earthbound_playername[24]; /* 12 */
    uint8_t  pet_name[6];             /* 36 */
    uint8_t  favourite_food[6];       /* 42 */
    uint8_t  favourite_thing[12];     /* 48 */
    uint32_t money_carried;           /* 60 */
    uint32_t bank_balance;            /* 64 */
    uint8_t  party_psi;              /* 68 */
    uint8_t  party_npc_1;            /* 69 */
    uint8_t  party_npc_2;            /* 70 */
    uint16_t party_npc_1_hp;         /* 71 */
    uint16_t party_npc_2_hp;         /* 73 */
    uint8_t  party_status;           /* 75 */
    uint8_t  party_npc_1_id_copy;    /* 76 */
    uint8_t  party_npc_2_id_copy;    /* 77 */
    uint16_t party_npc_1_hp_copy;    /* 78 */
    uint16_t party_npc_2_hp_copy;    /* 80 */
    uint32_t wallet_backup;          /* 82 */
    uint8_t  escargo_express_items[36]; /* 86 */
    uint8_t  party_members[6];       /* 122 */
    uint16_t leader_x_frac;          /* 128 */
    uint16_t leader_x_coord;         /* 130 */
    uint16_t leader_y_frac;          /* 132 */
    uint16_t leader_y_coord;         /* 134 */
    uint16_t position_buffer_index;  /* 136 */
    uint16_t leader_direction;       /* 138 */
    uint16_t trodden_tile_type;      /* 140 */
    uint16_t walking_style;          /* 142 */
    uint16_t leader_moved;           /* 144 */
    uint16_t character_mode;         /* 146: see CharacterMode enum */
    uint16_t current_party_members;  /* 148 */
    uint8_t  party_order[6];           /* 150 */
    uint8_t  player_controlled_party_members[6]; /* 156 */
    uint8_t  party_entity_slots[12]; /* 162 */
    uint8_t  party_count;            /* 174 */
    uint8_t  player_controlled_party_count; /* 175 */
    uint16_t camera_mode;            /* 176 */
    uint16_t auto_move_frames_left;  /* 178 */
    uint16_t auto_move_saved_walking_style; /* 180 */
    uint8_t  unknownB6[3];           /* 182 */
    uint8_t  unknownB8[3];           /* 185 */
    uint8_t  auto_fight_enable;      /* 188 */
    uint16_t exit_mouse_x_coord;     /* 189 */
    uint16_t exit_mouse_y_coord;     /* 191 */
    uint8_t  text_speed;             /* 193 */
    uint8_t  sound_setting;          /* 194 */
    uint8_t  unknownC3;              /* 195 */
    uint8_t  unknownC4[4];           /* 196 */
    uint8_t  active_hotspot_modes[2]; /* 200 */
    uint8_t  active_hotspot_ids[2];  /* 202 */
    uint8_t  active_hotspot_pointers[8]; /* 204 */
    PhotoState saved_photo_states[NUM_PHOTOS]; /* 212 */
    uint32_t timer;                  /* 468 */
    uint8_t  text_flavour;           /* 472 */
} GameState;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(GameState, 473);

/* Save header - 32 bytes */
PACKED_STRUCT
typedef struct {
    uint8_t  signature[28];
    uint16_t checksum;
    uint16_t checksum_complement;
} SaveHeader;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(SaveHeader, 32);

/* Character mode values (game_state.character_mode).
 * Set from low 3 bits of sector attributes; controls sprite selection
 * and party spacing in get_party_member_sprite_id / position buffer. */
enum CharacterMode {
    CHARACTER_MODE_NORMAL  = 0,
    CHARACTER_MODE_SMALL   = 3,  /* Lost Underworld / small party */
    CHARACTER_MODE_GHOST   = 4,
    CHARACTER_MODE_ROBOT   = 5,
    CHARACTER_MODE_BICYCLE = 6,
};

/* Total party count (4 PCs + 2 NPCs) */
#define TOTAL_PARTY_COUNT 6

/* Save block - 1280 bytes, matches save_block from structs.asm */
PACKED_STRUCT
typedef struct {
    SaveHeader header;                        /* 0: 32 bytes */
    GameState  game_state;                    /* 32: 473 bytes */
    CharStruct party_characters[TOTAL_PARTY_COUNT]; /* 505: 570 bytes */
    uint8_t    event_flags[EVENT_FLAG_COUNT / 8];   /* 1075: 128 bytes */
    uint8_t    padding[1280 - 32 - 473 - 95*6 - 128]; /* remaining padding */
} SaveBlock;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(SaveBlock, 1280);

/* Save file: 3 slots x 2 copies = 6 blocks x 1280 bytes = 7680 bytes */
#define SAVE_FILE_SIZE (SAVE_COUNT * SAVE_COPY_COUNT * sizeof(SaveBlock))

/* Global game state */
extern GameState game_state;
extern CharStruct party_characters[TOTAL_PARTY_COUNT];
extern uint8_t event_flags[EVENT_FLAG_COUNT / 8];

/* Initialize game state to defaults */
void game_state_init(void);

/* Event flag manipulation */
bool event_flag_get(uint16_t flag);
void event_flag_set(uint16_t flag);
void event_flag_clear(uint16_t flag);

/* Clear all entity surface flags and party member afflictions.
 * Port of CLEAR_ALL_STATUS_EFFECTS (asm/misc/clear_all_status_effects.asm). */
void clear_all_status_effects(void);

/* --- Party query functions --- */

/* CHECK_CHARACTER_IN_PARTY: Port of asm/battle/check_character_in_party.asm.
 * Returns char_id if found in party_members, 0 otherwise. */
uint16_t check_character_in_party(uint16_t char_id);

/* CHECK_STATUS_GROUP: Port of asm/misc/check_status_group.asm.
 * status_group 1-7 = affliction slot, 8 = party_status.
 * Returns affliction_value+1, or 0 if character not in party. */
uint16_t check_status_group(uint16_t status_group, uint16_t char_id);

/* INFLICT_STATUS_NONBATTLE: Port of asm/misc/inflict_status_nonbattle.asm.
 * Sets afflictions[status_group-1] = value-1 for char_id.
 * If status_group==8, sets party_status = value-1.
 * Returns char_id on success, 0 if not in party. */
uint16_t inflict_status_nonbattle(uint16_t char_id, uint16_t status_group, uint16_t value);

/* LEARN_SPECIAL_PSI: Port of asm/misc/learn_special_psi.asm.
 * psi_type: 1=TELEPORT_ALPHA, 2=STARSTORM_ALPHA, 3=STARSTORM_OMEGA, 4=TELEPORT_BETA.
 * Sets the corresponding bit in game_state.party_psi. */
void learn_special_psi(uint16_t psi_type);

/* CHECK_IF_PSI_KNOWN: Port of asm/misc/check_if_psi_known.asm.
 * Returns 1 if character knows the PSI ability, 0 if not.
 * char_id: 1=Ness, 2=Paula, 4=Poo (Jeff/others always return 0).
 * psi_ability_id: index into PSI_ABILITY_TABLE. */
uint16_t check_if_psi_known(uint16_t char_id, uint16_t psi_ability_id);

/* RESET_HPPP_ROLLING: Port of asm/misc/reset_hppp_rolling.asm.
 * Snaps HP/PP rolling targets to current values for all party members.
 * Sets fastest_hppp_meter_speed = 1. */
void reset_hppp_rolling(void);

/* --- HP/PP target modification functions --- */

/* HEAL_CHARACTER_HP: Port of asm/battle/heal_character_hp.asm.
 * Adds heal amount to current_hp_target, clamps to max_hp.
 * If current_hp is 0 and char exists, sets current_hp = 1 (wake from KO).
 * char_id: 1-indexed. amount: heal value. mode: 0=percent of max, non-zero=absolute. */
void heal_character_hp(uint16_t char_id, uint16_t amount, uint16_t mode);

/* HEAL_CHARACTER_PP: Port of asm/battle/heal_character_pp.asm.
 * Adds heal amount to current_pp_target, clamps to max_pp.
 * char_id: 1-indexed. amount: heal value. mode: 0=percent of max, non-zero=absolute. */
void heal_character_pp(uint16_t char_id, uint16_t amount, uint16_t mode);

/* REDUCE_HP_TARGET: Port of asm/battle/reduce_hp_target.asm.
 * Subtracts damage from current_hp_target, floors at 0.
 * char_id: 1-indexed. amount: damage value. mode: 0=percent of max, non-zero=absolute. */
void reduce_hp_target(uint16_t char_id, uint16_t amount, uint16_t mode);

/* REDUCE_PP_TARGET: Port of asm/battle/reduce_pp_target.asm.
 * Subtracts cost from current_pp_target, floors at 0.
 * char_id: 1-indexed. amount: cost value. mode: 0=percent of max, non-zero=absolute. */
void reduce_pp_target(uint16_t char_id, uint16_t amount, uint16_t mode);

/* RECOVER_HP_AMTPERCENT: Port of asm/misc/recover_hp_amtpercent.asm.
 * If char_id == 0xFF, applies heal to all player-controlled party members.
 * Otherwise applies to single character. */
void recover_hp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode);

/* RECOVER_PP_AMTPERCENT: Port of asm/misc/recover_pp_amtpercent.asm. */
void recover_pp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode);

/* REDUCE_HP_AMTPERCENT: Port of asm/misc/reduce_hp_amtpercent.asm. */
void reduce_hp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode);

/* REDUCE_PP_AMTPERCENT: Port of asm/misc/reduce_pp_amtpercent.asm. */
void reduce_pp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode);

/* HP/PP meter speed globals (from ram.asm) */
extern uint8_t fastest_hppp_meter_speed;

/* Save/load */
extern uint8_t current_save_slot; /* 1-indexed: 1, 2, or 3. Port of CURRENT_SAVE_SLOT BSS. */
bool save_game(int slot);
bool load_game(int slot);
bool erase_save(int slot);

#endif /* GAME_STATE_H */
