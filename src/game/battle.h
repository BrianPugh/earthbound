/*
 * Battle system core — data structures, constants, and utility functions.
 *
 * Ports structs and enums from:
 *   include/structs.asm (battler, battle_action, enemy_data)
 *   include/constants/battle.asm (status groups, PSI, action types)
 *   include/config.asm (party counts, battler limits, damage constants)
 *
 * Ports utility functions from:
 *   asm/battle/set_hp.asm, set_pp.asm
 *   asm/battle/reduce_hp.asm, reduce_pp.asm
 *   asm/battle/recover_hp.asm, recover_pp.asm
 *   asm/battle/inflict_status.asm
 *   asm/battle/determine_dodge.asm
 *   asm/battle/success_255.asm, success_500.asm, success_speed.asm
 *   asm/battle/count_chars.asm
 *   asm/battle/ko_target.asm, revive_target.asm
 *   asm/battle/increase_offense_16th.asm, decrease_offense_16th.asm
 *   asm/battle/increase_defense_16th.asm, decrease_defense_16th.asm
 *   asm/battle/psi_shield_nullify.asm
 *   asm/battle/25_percent_variance.asm, 50_percent_variance.asm
 *   asm/battle/calc_psi_damage_modifiers.asm
 *   asm/battle/calc_psi_resistance_modifiers.asm
 */
#ifndef GAME_BATTLE_H
#define GAME_BATTLE_H

#include "core/types.h"
#include "include/binary.h"
#include "include/constants.h"

/* Set to 0 to disable the letterbox (black bars at top/bottom) during battle */
#ifndef BATTLE_LETTERBOX_ENABLED
#define BATTLE_LETTERBOX_ENABLED 1
#endif

/* ---- Party member IDs (from include/enums.asm) ---- */

enum PartyMemberId {
    PARTY_MEMBER_NONE              = 0,
    PARTY_MEMBER_NESS              = 1,
    PARTY_MEMBER_PAULA             = 2,
    PARTY_MEMBER_JEFF              = 3,
    PARTY_MEMBER_POO               = 4,
    PARTY_MEMBER_POKEY             = 5,
    PARTY_MEMBER_PICKY             = 6,
    PARTY_MEMBER_KING              = 7,
    PARTY_MEMBER_TONY              = 8,
    PARTY_MEMBER_BUBBLE_MONKEY     = 9,
    PARTY_MEMBER_DUNGEON_MAN       = 10,
    PARTY_MEMBER_FLYING_MAN1       = 11,
    PARTY_MEMBER_FLYING_MAN2       = 12,
    PARTY_MEMBER_FLYING_MAN3       = 13,
    PARTY_MEMBER_FLYING_MAN4       = 14,
    PARTY_MEMBER_FLYING_MAN5       = 15,
    PARTY_MEMBER_TEDDY_BEAR        = 16,
    PARTY_MEMBER_PLUSH_TEDDY_BEAR  = 17,
};

/* ---- Party / battler limits (from include/config.asm) ---- */

#define PLAYER_CHAR_COUNT    4
#define NONPLAYER_CHAR_COUNT 2
/* TOTAL_PARTY_COUNT already defined in game_state.h as 6 */
#define FIRST_ENEMY_INDEX    (TOTAL_PARTY_COUNT + 2) /* +2 for bad "party members" like tiny lil ghost */
#define BATTLER_COUNT        20
#define MAX_ENEMY_BATTLER_SLOTS (BATTLER_COUNT - FIRST_ENEMY_INDEX) /* 12 */
#define MAX_ENEMY_ENCOUNTER_SLOTS 23 /* max enemies in a battle group roster (Giygas phases) */

/* ---- Damage constants (from include/config.asm) ---- */

#define ROCKIN_ALPHA_DAMAGE   80
#define ROCKIN_BETA_DAMAGE   180
#define ROCKIN_GAMMA_DAMAGE  320
#define ROCKIN_OMEGA_DAMAGE  640

#define STARSTORM_ALPHA_DAMAGE 360
#define STARSTORM_OMEGA_DAMAGE 720

#define FIRE_ALPHA_DAMAGE     80
#define FIRE_BETA_DAMAGE     160
#define FIRE_GAMMA_DAMAGE    240
#define FIRE_OMEGA_DAMAGE    320

#define FREEZE_ALPHA_DAMAGE  180
#define FREEZE_BETA_DAMAGE   360
#define FREEZE_GAMMA_DAMAGE  540
#define FREEZE_OMEGA_DAMAGE  720

#define THUNDER_ALPHA_DAMAGE 120
#define THUNDER_ALPHA_HITS     1
#define THUNDER_BETA_DAMAGE  120
#define THUNDER_BETA_HITS      2
#define THUNDER_GAMMA_DAMAGE 200
#define THUNDER_GAMMA_HITS     3
#define THUNDER_OMEGA_DAMAGE 200
#define THUNDER_OMEGA_HITS     4

#define LIFEUP_ALPHA_HEALING 100
#define LIFEUP_BETA_HEALING  300
#define LIFEUP_GAMMA_HEALING 10000
#define LIFEUP_OMEGA_HEALING 400

#define BOTTLE_ROCKET_COUNT       1
#define BIG_BOTTLE_ROCKET_COUNT   5
#define MULTI_BOTTLE_ROCKET_COUNT 20

#define HANDBAG_STRAP_BASE_DAMAGE  100
#define MUMMY_WRAP_BASE_DAMAGE     400

/* Enemy IDs for Master Belch variants (from include/constants/enemies.asm) */
#define ENEMY_MASTER_BELCH_1  93
#define ENEMY_MASTER_BELCH_2 169
#define ENEMY_MASTER_BELCH_3 192

/* ---- Combat constants (from include/config.asm) ---- */

#define GUTS_FLOOR_FOR_SMAAAASH_CHANCE  25
#define SMAAAASH_FLASH_DURATION         (1 * FRAMES_PER_SECOND)
#define CHANCE_OF_WAKING_UP_WHEN_ATTACKED 128  /* out of 255 (~50%) */

/* ---- Timing constants (from include/enums.asm) ---- */

#define SIXTH_OF_A_SECOND   10
#define THIRD_OF_A_SECOND   20
#define HALF_OF_A_SECOND    30

/* ---- Status groups (from include/constants/battle.asm) ---- */

enum StatusGroup {
    STATUS_GROUP_PERSISTENT_EASYHEAL = 0,
    STATUS_GROUP_PERSISTENT_HARDHEAL = 1,
    STATUS_GROUP_TEMPORARY           = 2,
    STATUS_GROUP_STRANGENESS         = 3,
    STATUS_GROUP_CONCENTRATION       = 4,
    STATUS_GROUP_HOMESICKNESS        = 5,
    STATUS_GROUP_SHIELD              = 6,
};

/* Status values within STATUS_GROUP_PERSISTENT_EASYHEAL (group 0) */
enum Status0 {
    STATUS_0_UNCONSCIOUS = 1,
    STATUS_0_DIAMONDIZED = 2,
    STATUS_0_PARALYZED   = 3,
    STATUS_0_NAUSEOUS    = 4,
    STATUS_0_POISONED    = 5,
    STATUS_0_SUNSTROKE   = 6,
    STATUS_0_COLD        = 7,
};

/* Status values within STATUS_GROUP_PERSISTENT_HARDHEAL (group 1) */
enum Status1 {
    STATUS_1_MUSHROOMIZED = 1,
    STATUS_1_POSSESSED    = 2,
};

/* Status values within STATUS_GROUP_TEMPORARY (group 2) */
enum Status2 {
    STATUS_2_ASLEEP      = 1,
    STATUS_2_CRYING      = 2,
    STATUS_2_IMMOBILIZED = 3,
    STATUS_2_SOLIDIFIED  = 4,
};

/* Status values within STATUS_GROUP_STRANGENESS (group 3) */
enum Status3 {
    STATUS_3_STRANGE = 1,
};

/* Status values within STATUS_GROUP_CONCENTRATION (group 4) */
enum Status4 {
    STATUS_4_CANT_CONCENTRATE  = 1,
    STATUS_4_CANT_CONCENTRATE4 = 4,
};

/* Status values within STATUS_GROUP_HOMESICKNESS (group 5) */
enum Status5 {
    STATUS_5_HOMESICK = 1,
};

/* Status values within STATUS_GROUP_SHIELD (group 6) */
enum Status6 {
    STATUS_6_PSI_SHIELD_POWER = 1,
    STATUS_6_PSI_SHIELD       = 2,
    STATUS_6_SHIELD_POWER     = 3,
    STATUS_6_SHIELD           = 4,
};

/* ---- Action types (from include/constants/battle.asm) ---- */

enum ActionDirection {
    ACTION_DIRECTION_PARTY = 0,
    ACTION_DIRECTION_ENEMY = 1,
};

enum ActionTarget {
    ACTION_TARGET_NONE   = 0,
    ACTION_TARGET_ONE    = 1,
    ACTION_TARGET_RANDOM = 2,
    ACTION_TARGET_ROW    = 3,
    ACTION_TARGET_ALL    = 4,
};

enum ActionType {
    ACTION_TYPE_NOTHING           = 0,
    ACTION_TYPE_PHYSICAL          = 1,
    ACTION_TYPE_PIERCING_PHYSICAL = 2,
    ACTION_TYPE_PSI               = 3,
    ACTION_TYPE_ITEM              = 4,
    ACTION_TYPE_OTHER             = 5,
};

/* ---- Initiative (from include/constants/battle.asm) ---- */

enum Initiative {
    INITIATIVE_NORMAL       = 0,
    INITIATIVE_PARTY_FIRST  = 1,
    INITIATIVE_ENEMIES_FIRST = 2,
};

/* ---- Initial status for enemies (from include/constants/battle.asm) ---- */

enum InitialStatus {
    INITIAL_STATUS_NONE              = 0,
    INITIAL_STATUS_PSI_SHIELD        = 1,
    INITIAL_STATUS_PSI_SHIELD_POWER  = 2,
    INITIAL_STATUS_SHIELD            = 3,
    INITIAL_STATUS_SHIELD_POWER      = 4,
    INITIAL_STATUS_ASLEEP            = 5,
    INITIAL_STATUS_CANT_CONCENTRATE  = 6,
    INITIAL_STATUS_STRANGE           = 7,
};

/* ---- Giygas phases (from include/constants/battle.asm) ---- */

enum GiygasPhase {
    GIYGAS_BATTLE_STARTED        = 1,
    GIYGAS_DEVILS_MACHINE_OFF    = 2,
    GIYGAS_STARTS_ATTACKING      = 3,
    GIYGAS_START_PRAYING         = 4,
    GIYGAS_PRAYER_1_USED         = 5,
    GIYGAS_PRAYER_2_USED         = 6,
    GIYGAS_PRAYER_3_USED         = 7,
    GIYGAS_PRAYER_4_USED         = 8,
    GIYGAS_PRAYER_5_USED         = 9,
    GIYGAS_PRAYER_6_USED         = 10,
    GIYGAS_PRAYER_7_USED         = 11,
    GIYGAS_PRAYER_8_USED         = 12,
    GIYGAS_DEFEATED              = 0xFFFF,
};

/* ---- Battler struct (78 bytes, from include/structs.asm) ---- */

PACKED_STRUCT
typedef struct {
    uint16_t id;                       /*  0 */
    uint8_t  sprite;                   /*  2 */
    uint8_t  unknown03;                /*  3 */
    uint16_t current_action;           /*  4 */
    uint8_t  action_order_var;         /*  6 */
    uint8_t  action_item_slot;         /*  7 */
    uint8_t  current_action_argument;  /*  8 */
    uint8_t  action_targetting;        /*  9 */
    uint8_t  current_target;           /* 10 */
    uint8_t  the_flag;                 /* 11 */
    uint8_t  consciousness;            /* 12 */
    uint8_t  has_taken_turn;           /* 13 */
    uint8_t  ally_or_enemy;            /* 14 */
    uint8_t  npc_id;                   /* 15 */
    uint8_t  row;                      /* 16 */
    uint16_t hp;                       /* 17 */
    uint16_t hp_target;                /* 19 */
    uint16_t hp_max;                   /* 21 */
    uint16_t pp;                       /* 23 */
    uint16_t pp_target;                /* 25 */
    uint16_t pp_max;                   /* 27 */
    uint8_t  afflictions[AFFLICTION_GROUP_COUNT]; /* 29 */
    uint8_t  guarding;                 /* 36 */
    uint8_t  shield_hp;                /* 37 */
    uint16_t offense;                  /* 38 */
    uint16_t defense;                  /* 40 */
    uint16_t speed;                    /* 42 */
    uint16_t guts;                     /* 44 */
    uint16_t luck;                     /* 46 */
    uint8_t  vitality;                 /* 48 */
    uint8_t  iq;                       /* 49 */
    uint8_t  base_offense;             /* 50 */
    uint8_t  base_defense;             /* 51 */
    uint8_t  base_speed;               /* 52 */
    uint8_t  base_guts;                /* 53 */
    uint8_t  base_luck;                /* 54 */
    uint8_t  paralysis_resist;         /* 55 */
    uint8_t  freeze_resist;            /* 56 */
    uint8_t  flash_resist;             /* 57 */
    uint8_t  fire_resist;              /* 58 */
    uint8_t  brainshock_resist;        /* 59 */
    uint8_t  hypnosis_resist;          /* 60 */
    uint16_t money;                    /* 61 */
    uint32_t exp;                      /* 63 */
    uint8_t  vram_sprite_index;        /* 67 */
    uint8_t  sprite_x;                 /* 68 */
    uint8_t  sprite_y;                 /* 69 */
    uint8_t  initiative;               /* 70 */
    uint8_t  unknown71;                /* 71 */
    uint8_t  blink_timer;              /* 72: damage blink countdown */
    uint8_t  shake_timer;              /* 73: attack animation shake countdown */
    uint8_t  is_flash_target;          /* 74: 1 = this battler is the targeting flash target */
    uint8_t  use_alt_spritemap;        /* 75 */
    uint8_t  enemy_type_id;            /* 76: enemy config table index (8-bit) */
    uint8_t  id2;                      /* 77 */
} Battler;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(Battler, 78);

/* ---- Battle action struct (12 bytes, from include/structs.asm) ---- */

PACKED_STRUCT
typedef struct {
    uint8_t  direction;                /* 0: ACTION_DIRECTION_PARTY or ENEMY */
    uint8_t  target;                   /* 1: ACTION_TARGET_* */
    uint8_t  type;                     /* 2: ACTION_TYPE_* */
    uint8_t  pp_cost;                  /* 3 */
    uint32_t description_text_pointer; /* 4 */
    uint32_t battle_function_pointer;  /* 8 */
} BattleAction;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(BattleAction, 12);

/* ---- Enemy data struct (94 bytes USA, from include/structs.asm) ---- */

#define ENEMY_NAME_SIZE 25  /* USA */

PACKED_STRUCT
typedef struct {
    uint8_t  the_flag;                          /*  0 */
    uint8_t  name[ENEMY_NAME_SIZE];             /*  1 */
    uint8_t  gender;                            /* 26 */
    uint8_t  type;                              /* 27 */
    uint16_t battle_sprite;                     /* 28 */
    uint16_t overworld_sprite;                  /* 30 */
    uint8_t  run_flag;                          /* 32 */
    uint16_t hp;                                /* 33 */
    uint16_t pp;                                /* 35 */
    uint32_t exp;                               /* 37 */
    uint16_t money;                             /* 41 */
    uint16_t event_script;                      /* 43 */
    uint32_t encounter_text_ptr;                /* 45 */
    uint32_t death_text_ptr;                    /* 49 */
    uint8_t  battle_sprite_palette;             /* 53 */
    uint8_t  level;                             /* 54 */
    uint8_t  music;                             /* 55 */
    uint16_t offense;                           /* 56 */
    uint16_t defense;                           /* 58 */
    uint8_t  speed;                             /* 60 */
    uint8_t  guts;                              /* 61 */
    uint8_t  luck;                              /* 62 */
    uint8_t  fire_vulnerability;                /* 63 */
    uint8_t  freeze_vulnerability;              /* 64 */
    uint8_t  flash_vulnerability;               /* 65 */
    uint8_t  paralysis_vulnerability;           /* 66 */
    uint8_t  hypnosis_brainshock_vulnerability; /* 67 */
    uint8_t  miss_rate;                         /* 68 */
    uint8_t  action_order;                      /* 69 */
    uint16_t actions[4];                        /* 70 */
    uint16_t final_action;                      /* 78 */
    uint8_t  action_args[4];                    /* 80 */
    uint8_t  final_action_arg;                  /* 84 */
    uint8_t  iq;                                /* 85 */
    uint8_t  boss;                              /* 86 */
    uint8_t  item_drop_rate;                    /* 87 */
    uint8_t  item_dropped;                      /* 88 */
    uint8_t  initial_status;                    /* 89 */
    uint8_t  death_type;                        /* 90 */
    uint8_t  row;                               /* 91 */
    uint8_t  max_called;                        /* 92 */
    uint8_t  mirror_success;                    /* 93 */
} EnemyData;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(EnemyData, 94);

/* ---- Enemy IDs (from include/constants/enemies.asm, subset) ----
 * Only enemies referenced by ID in C code logic are listed here.
 * Most enemy IDs live exclusively in ROM data (battle groups, encounter
 * tables, etc.) loaded at runtime from the donor ROM. Add new entries
 * as needed when code checks for a specific enemy. */

enum EnemyId {
    ENEMY_GIYGAS_1           = 34,
    ENEMY_TINY_LIL_GHOST     = 213,
    ENEMY_BUZZ_BUZZ          = 215,
    ENEMY_GIYGAS_2           = 218,
    ENEMY_GIYGAS_3           = 219,
    ENEMY_GIYGAS_4           = 220,
    ENEMY_GIYGAS_5           = 221,
    ENEMY_MAGIC_BUTTERFLY    = 225,
    ENEMY_GIYGAS_6           = 229,
};

/* (PARTY_MEMBER_TEDDY_BEAR and PARTY_MEMBER_PLUSH_TEDDY_BEAR are in PartyMemberId above) */

/* Battle spritemap buffer dimensions (needed by BattleState struct). */
#define BATTLE_SPRITEMAP_ENTRIES 16
#define BATTLE_SPRITE_SLOTS      4
#define BATTLE_SPRITEMAP_ENTRY_BYTES 5
#define BATTLE_SPRITEMAP_BUF_SIZE (BATTLE_SPRITEMAP_ENTRY_BYTES * BATTLE_SPRITEMAP_ENTRIES * BATTLE_SPRITE_SLOTS)

/* Mirror (metamorphose) system constant */
#define DEFAULT_MIRROR_TURN_COUNT 16

/* ---- Consolidated battle system runtime state ---- */
typedef struct {
    Battler battlers_table[BATTLER_COUNT];
    uint32_t battler_target_flags;
    uint16_t current_attacker;
    uint16_t current_target;
    uint32_t battle_exp_scratch;
    uint16_t battle_money_scratch;
    uint16_t giygas_phase;
    uint16_t special_defeat;
    uint16_t enemy_performing_final_attack;
    uint16_t skip_death_text_and_cleanup;
    uint16_t shield_has_nullified_damage;
    uint16_t damage_is_reflected;
    uint16_t is_smaaaash_attack;
    uint16_t item_dropped;
    uint16_t green_flash_duration;
    uint16_t red_flash_duration;
    uint16_t reflect_flash_duration;
    uint16_t green_background_flash_duration;
    uint16_t hp_pp_box_blink_duration;
    uint16_t hp_pp_box_blink_target;
    uint16_t vertical_shake_duration;
    uint16_t vertical_shake_hold_duration;
    uint16_t screen_effect_minimum_wait_frames;
    int16_t  screen_effect_vertical_offset;
    int16_t  screen_effect_horizontal_offset;
    uint16_t wobble_duration;
    uint16_t shake_duration;
    uint16_t background_brightness;
    uint16_t highest_enemy_level_in_battle;
    uint8_t  used_enemy_letters[26];
    uint16_t current_battle_group;
    uint16_t enemies_in_battle;
    uint16_t enemies_in_battle_ids[MAX_ENEMY_ENCOUNTER_SLOTS];
    uint16_t party_members_alive_overworld;
    int16_t  touched_enemy;
    uint16_t battle_initiative;
    uint16_t battle_mode_flag;
    uint8_t  battle_item_used;
    uint16_t party_members_with_selected_actions[6];
    uint16_t letterbox_effect_ending;
    uint16_t letterbox_top_end;
    uint16_t letterbox_bottom_start;
    uint16_t letterbox_effect_ending_top;
    uint16_t letterbox_effect_ending_bottom;
    uint16_t letterbox_visible_screen_value;
    uint16_t letterbox_nonvisible_screen_value;
    uint8_t  letterbox_hdma_table[18];
    uint16_t enable_background_darkening;
    uint16_t current_layer_config;
    uint16_t mirror_enemy;
    Battler  mirror_battler_backup;
    uint16_t mirror_turn_timer;
    int16_t  current_flashing_enemy;
    uint16_t current_flashing_enemy_row;
    uint16_t enemy_targetting_flashing;
    int16_t  current_flashing_row;
    uint8_t  front_row_battlers[8];
    uint8_t  back_row_battlers[8];
    uint16_t num_battlers_in_front_row;
    uint16_t num_battlers_in_back_row;
    uint8_t  battler_front_row_x_positions[8];
    uint8_t  battler_front_row_y_positions[8];
    uint8_t  battler_back_row_x_positions[8];
    uint8_t  battler_back_row_y_positions[8];
    /* Battle sprite allocation */
    uint16_t current_battle_sprites_allocated;
    uint16_t current_battle_sprite_enemy_ids[4];
    uint16_t current_battle_sprite_widths[4];
    uint16_t current_battle_sprite_heights[4];
    uint16_t current_battle_spritemaps_allocated;
    uint16_t battle_spritemap_allocation_counts[4];
    uint8_t  battle_spritemaps[BATTLE_SPRITEMAP_BUF_SIZE];
    uint8_t  alt_battle_spritemaps[BATTLE_SPRITEMAP_BUF_SIZE];
    /* HPPP meter speed */
    uint8_t  half_hppp_meter_speed;
    uint8_t  disable_hppp_rolling;
    int32_t  hp_meter_speed;
    uint16_t hppp_meter_flipout_mode;
    /* Sprite palette effect */
    uint16_t battle_sprite_palette_effect_speed;
    uint16_t battle_sprite_palette_effect_frames_left[4];
    int16_t  battle_sprite_palette_effect_deltas[64 * 3];
    int16_t  battle_sprite_palette_effect_counters[64 * 3];
    int16_t  battle_sprite_palette_effect_steps[64 * 3];
    /* PSI animation */
    uint16_t psi_animation_enemy_targets[4];
    int16_t  psi_animation_x_offset;
    int16_t  psi_animation_y_offset;
    /* PSI description cache */
    uint16_t last_selected_psi_description;
    /* Misc */
    uint32_t temp_function_pointer;
    uint16_t debugging_current_psi_animation;
    uint16_t debugging_current_swirl;
    uint16_t debugging_current_swirl_flags;
    /* Palette backup during battle sprite effects. */
    uint16_t background_colour_backup;
    /* Battle menu selection state (from anonymous struct). */
    uint8_t  battle_menu_user;
    uint8_t  battle_menu_param1;
    uint16_t battle_menu_selected_action;
    uint8_t  battle_menu_targetting;
    uint8_t  battle_menu_selected_target;
} BattleState;

extern BattleState bt;

#define ENEMY_GROUP_BOSS_START 448
#define ENEMY_GROUP_BOSS_GIYGAS_PHASE_1_ENTRY 475
#define ENEMY_GROUP_BOSS_GIYGAS_PHASE_1 476
#define ENEMY_GROUP_BOSS_GIYGAS_PHASE_2 477
#define ENEMY_GROUP_BOSS_GIYGAS_DURING_PRAYER_1 478
#define ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_1 479
#define ENEMY_GROUP_BOSS_GIYGAS_AFTER_PRAYER_7 480
#define ENEMY_GROUP_BOSS_GIYGAS_PHASE_FINAL 483

/* ---- Battle action table (loaded from ROM) ---- */

extern const BattleAction *battle_action_table;

/* ---- Enemy configuration table (loaded from ROM) ---- */

extern const EnemyData *enemy_config_table;

/* ---- Battler access helpers ---- */

/* Convert a byte offset to a Battler pointer.
 * In assembly, battler pointers are byte offsets from BATTLERS_TABLE start. */
static inline Battler *battler_from_offset(uint16_t offset) {
    return (Battler *)((uint8_t *)bt.battlers_table + offset);
}

/* Convert a Battler pointer back to byte offset */
static inline uint16_t battler_to_offset(const Battler *b) {
    return (uint16_t)((const uint8_t *)b - (const uint8_t *)bt.battlers_table);
}

/* ---- HP / PP management ---- */

/* SET_HP: Clamp new_hp to [0, hp_max], update hp_target.
 * For player characters, also updates char_struct::current_hp_target.
 * For NPC allies, also updates game_state::party_npc_X_hp.
 * For enemies, directly sets hp and hp_target. */
void battle_set_hp(Battler *target, uint16_t new_hp);

/* SET_PP: Clamp new_pp to [0, pp_max], update pp_target.
 * Same char_struct sync rules as SET_HP. */
void battle_set_pp(Battler *target, uint16_t new_pp);

/* REDUCE_HP: Subtract damage from hp_target, floor at 0, apply via SET_HP. */
void battle_reduce_hp(Battler *target, uint16_t damage);

/* REDUCE_PP: Subtract cost from pp_target, floor at 0, apply via SET_PP. */
void battle_reduce_pp(Battler *target, uint16_t cost);

/* RECOVER_HP: Add heal_amount to hp_target, cap at hp_max, apply via SET_HP.
 * Blocks if target is unconscious or has CANT_CONCENTRATE affliction (group 0 == 1).
 * Displays appropriate battle message. */
void battle_recover_hp(Battler *target, uint16_t heal_amount);

/* RECOVER_PP: Add amount to pp_target, cap at pp_max, apply via SET_PP.
 * Blocks if target is unconscious or has CANT_CONCENTRATE affliction.
 * Displays PP recovery battle message. */
void battle_recover_pp(Battler *target, uint16_t amount);

/* ---- Status effects ---- */

/* INFLICT_STATUS_BATTLE: Apply status to target.
 * Returns 1 if status was applied, 0 if blocked (NPC or already active). */
uint16_t battle_inflict_status(Battler *target, uint16_t status_group, uint16_t status_value);

/* ---- Combat calculations ---- */

/* DETERMINE_DODGE: Check if current target dodges current attacker's attack.
 * Returns 1 if dodged, 0 if not. Paralyzed/asleep/immobilized/solidified targets can't dodge. */
uint16_t battle_determine_dodge(void);

/* SUCCESS_255: Random chance with 0-255 threshold.
 * Returns 1 if rand_byte() < threshold, 0 otherwise. Probability = threshold/256. */
uint16_t battle_success_255(uint16_t threshold);

/* SUCCESS_500: Random chance with 0-500 threshold.
 * Returns 1 if rand_limit(500) < threshold, 0 otherwise. Probability = threshold/500. */
uint16_t battle_success_500(uint16_t threshold);

/* SUCCESS_SPEED: Speed-modified random check.
 * Uses difference between target speed*2 and attacker speed.
 * Returns 1 on success, 0 on failure. */
uint16_t battle_success_speed(uint16_t base_chance);

/* COUNT_CHARS: Count alive, non-NPC, non-unconscious/diamondized battlers on a side.
 * side: 0=party, 1=enemies. */
uint16_t battle_count_chars(uint16_t side);

/* ---- Damage variance ---- */

/* TWENTY_FIVE_PERCENT_VARIANCE: Apply +/- 12.5% random variance to a value.
 * Uses two random numbers, picks the one closer to center (triangular distribution). */
uint16_t battle_25pct_variance(uint16_t value);

/* FIFTY_PERCENT_VARIANCE: Apply +/- 25% random variance to a value.
 * Same triangular distribution method but double the magnitude. */
uint16_t battle_50pct_variance(uint16_t value);

/* ---- PSI resistance modifiers ---- */

/* CALC_PSI_DMG_MODIFIERS: Returns damage multiplier (0-255) based on resistance level.
 * 0=none(255), 1=low(179), 2=med(102), 3=high(13). Multiply damage × result / 255. */
uint8_t battle_calc_psi_dmg_modifier(uint8_t resist_level);

/* CALC_PSI_RES_MODIFIERS: Returns success multiplier (0-255) based on resistance level.
 * 0=none(255), 1=low(128), 2=med(26), 3=high(0). */
uint8_t battle_calc_psi_res_modifier(uint8_t resist_level);

/* ---- Stat modification ---- */

/* INCREASE_OFFENSE_16TH: Increase target's offense by 1/16th, clamp to base*5/4. */
void battle_increase_offense(Battler *target);

/* HEXADECIMATE_OFFENSE: Decrease target's offense by 1/16th, clamp to base*3/4. */
void battle_decrease_offense(Battler *target);

/* INCREASE_DEFENSE_16TH: Increase target's defense by 1/16th, clamp to base*5/4. */
void battle_increase_defense(Battler *target);

/* HEXADECIMATE_DEFENSE: Decrease target's defense by 1/16th, clamp to base*3/4. */
void battle_decrease_defense(Battler *target);

/* ---- PSI shield ---- */

/* PSI_SHIELD_NULLIFY: Check if current target's PSI shield blocks/reflects the attack.
 * Sets SHIELD_HAS_NULLIFIED_DAMAGE, may swap attacker/target for reflection.
 * Returns 1 if attack was nullified, 0 if it proceeds normally. */
uint16_t battle_psi_shield_nullify(void);

/* ---- KO / Revive ---- */

/* KO_TARGET: Kill a battler. Handles:
 *   - Player chars: set affliction to UNCONSCIOUS, zero hp_target, display text
 *   - Enemies: death animation, final attack, reward accumulation, death_type processing
 *   - Possession/ghost mechanics for Tiny Lil Ghost */
void battle_ko_target(Battler *target);

/* REVIVE_TARGET: Revive a KO'd battler with specified HP.
 * Clears all afflictions, displays revive message, handles palette animation. */
void battle_revive_target(Battler *target, uint16_t hp);

/* ---- Damage application ---- */

/* GET_BATTLE_ACTION_TYPE: Look up the action type (physical/PSI/etc.) for a given action ID. */
uint16_t battle_get_action_type(uint16_t action_id);

/* CALC_DAMAGE: Apply damage to a target, display damage text, handle Giygas
 * redirect, guts save, screen shake, and KO trigger.
 * Returns 1 always (signals damage was attempted). */
uint16_t battle_calc_damage(uint16_t target_offset, uint16_t damage);

/* CALC_RESIST_DAMAGE: Apply damage with guard/shield modifiers, handle KO,
 * shield reflection, and sleep wake chance.
 * damage: raw damage amount, resist_modifier: 0-255 resistance factor.
 * Returns final damage dealt. */
uint16_t battle_calc_resist_damage(uint16_t damage, uint16_t resist_modifier);

/* MISS_CALC: Check if an attack misses based on miss rate.
 * miss_message_type: 0 for physical miss text, 1 for gun miss text.
 * Returns 1 if attack missed, 0 if it hits. */
uint16_t battle_miss_calc(uint16_t miss_message_type);

/* SMAAAASH: Check for critical hit (SMAAAASH attack).
 * Uses attacker's guts stat vs SUCCESS_500.
 * On success, deals 4x offense damage and weakens shields.
 * Returns 1 if SMAAAASH occurred, 0 otherwise. */
uint16_t battle_smaaaash(void);

/* ---- Attack damage calculations ---- */

/* BTLACT_LEVEL_1_ATK: Standard physical attack.
 * damage = attacker.offense - target.defense, with 25% variance.
 * Includes miss/smaaaash/dodge checks and heals strangeness. */
void battle_level_1_attack(void);

/* BTLACT_LEVEL_2_ATK: Doubled physical attack.
 * damage = (attacker.offense * 2) - target.defense, with 25% variance. */
void battle_level_2_attack(void);

/* ---- Status helpers ---- */

/* HEAL_STRANGENESS: Remove STRANGENESS status from current target if present. */
void battle_heal_strangeness(void);

/* WEAKEN_SHIELD: After PSI reflection, decrement shield HP and remove if depleted. */
void battle_weaken_shield(void);

/* SHIELDS_COMMON: Apply or refresh a shield on a battler.
 * shield_type: STATUS_6_SHIELD, STATUS_6_SHIELD_POWER, etc.
 * Returns 0 if new shield applied, 1 if existing shield refreshed. */
uint16_t battle_shields_common(Battler *target, uint16_t shield_type);

/* ---- Battle action IDs (from include/constants/actions.asm, subset) ---- */

enum BattleActionId {
    BATTLE_ACTION_PSI_LIFEUP_ALPHA     = 32,
    BATTLE_ACTION_PSI_LIFEUP_BETA      = 33,
    BATTLE_ACTION_PSI_LIFEUP_GAMMA     = 34,
    BATTLE_ACTION_PSI_LIFEUP_OMEGA     = 35,
    BATTLE_ACTION_PSI_HEALING_ALPHA    = 36,
    BATTLE_ACTION_PSI_HEALING_BETA     = 37,
    BATTLE_ACTION_PSI_HEALING_GAMMA    = 38,
    BATTLE_ACTION_PSI_HEALING_OMEGA    = 39,
    BATTLE_ACTION_PSI_SHIELD_SIGMA     = 42,
    BATTLE_ACTION_PSI_SHIELD_OMEGA     = 43,
    BATTLE_ACTION_PSI_PSI_SHIELD_SIGMA = 46,
    BATTLE_ACTION_PSI_PSI_SHIELD_OMEGA = 47,
    BATTLE_ACTION_ACTION_135           = 135,
    BATTLE_ACTION_ACTION_136           = 136,
    BATTLE_ACTION_ACTION_137           = 137,
    BATTLE_ACTION_ACTION_138           = 138,
    BATTLE_ACTION_ACTION_139           = 139,
    BATTLE_ACTION_HAND_AID             = 140,
    BATTLE_ACTION_ACTION_141           = 141,
    BATTLE_ACTION_ACTION_142           = 142,
    BATTLE_ACTION_ACTION_143           = 143,
    BATTLE_ACTION_ACTION_144           = 144,
    BATTLE_ACTION_ACTION_145           = 145,
    BATTLE_ACTION_ACTION_146           = 146,
    BATTLE_ACTION_ACTION_147           = 147,
    BATTLE_ACTION_ACTION_148           = 148,
    BATTLE_ACTION_WET_TOWEL            = 149,
    BATTLE_ACTION_REFRESHING_HERB      = 150,
    BATTLE_ACTION_SECRET_HERB          = 151,
    BATTLE_ACTION_FULL_STATUS_HEAL     = 152,
    BATTLE_ACTION_ACTION_153           = 153,
    BATTLE_ACTION_ACTION_154           = 154,
    BATTLE_ACTION_ACTION_155           = 155,
    BATTLE_ACTION_ACTION_156           = 156,
    BATTLE_ACTION_ACTION_157           = 157,
    BATTLE_ACTION_ACTION_158           = 158,
};

/* ---- Targeting ---- */

/* TARGET_BATTLER: Set the bit for battler_index in battler_target_flags. */
void battle_target_battler(uint16_t battler_index);

/* REMOVE_TARGET: Clear the bit for battler_index in battler_target_flags. */
void battle_remove_target(uint16_t battler_index);

/* IS_CHAR_TARGETTED: Check if battler_index's bit is set in battler_target_flags. */
uint16_t battle_is_char_targeted(uint16_t battler_index);

/* TARGET_ALL: Set bits for all conscious battlers in battler_target_flags. */
void battle_target_all(void);

/* TARGET_ALL_ENEMIES: Set bits for all conscious enemies in battler_target_flags. */
void battle_target_all_enemies(void);

/* RANDOM_TARGETTING: Given a 32-bit target mask, randomly select one set bit.
 * Returns a mask with only the selected bit set. Returns 0 if input is 0. */
uint32_t battle_random_targeting(uint32_t target_mask);

/* TARGET_ROW: Set target flags based on parameter:
 * 0=all allies, 1=enemies in row 0, 2=enemies in row 1. */
void battle_target_row(uint16_t param);

/* REMOVE_DEAD_TARGETTING: Clear target bits for any unconscious battlers. */
void battle_remove_dead_targeting(void);

/* CHECK_IF_VALID_TARGET: Check if battler at index is conscious, non-NPC,
 * non-unconscious, non-diamondized. Returns 1 if valid, 0 if not. */
uint16_t battle_check_if_valid_target(uint16_t battler_index);

/* REMOVE_STATUS_UNTARGETTABLE_TARGETS: Remove unconscious/diamondized targets
 * unless current action can target dead battlers (healing, revival). */
void battle_remove_status_untargettable_targets(void);

/* SET_BATTLER_TARGETS_BY_ACTION: Set battler_target_flags based on action targeting type. */
void set_battler_targets_by_action(uint16_t attacker_offset);

/* ---- Enemy lookup ---- */

/* GET_ENEMY_TYPE: Returns the type byte for enemy at given index. */
uint16_t battle_get_enemy_type(uint16_t enemy_id);

/* ---- Luck-based success checks ---- */

/* SUCCESS_LUCK40: Random check vs target's luck with range 40.
 * Returns 1 if rand(40) >= target.luck, 0 otherwise. */
uint16_t battle_success_luck40(void);

/* SUCCESS_LUCK80: Random check vs target's luck with range 80.
 * Returns 1 if rand(80) >= target.luck, 0 otherwise. */
uint16_t battle_success_luck80(void);

/* ---- NPC attack check ---- */

/* FAIL_ATTACK_ON_NPCS: Returns 1 if current target is an NPC (attack fails). */
uint16_t battle_fail_attack_on_npcs(void);

/* ---- Status HP loss ---- */

/* LOSE_HP_STATUS: Reduce target's HP by amount (status damage like poison).
 * Subtracts directly from hp_target, floors at 0. */
void battle_lose_hp_status(Battler *target, uint16_t amount);

/* ---- Shield targeting ---- */

/* GET_SHIELD_TARGETTING: Returns 1 if the shield action targets a single ally
 * (sigma/omega variants), 0 for all-ally targeting (alpha/beta). */
uint16_t battle_get_shield_targeting(uint16_t action);

/* ---- Additional targeting ---- */

/* TARGET_ALLIES: Set bits for all party members and NPC allies. */
void battle_target_allies(void);

/* REMOVE_NPC_TARGETTING: Clear target bits for NPC battlers. */
void battle_remove_npc_targeting(void);

/* FEELING_STRANGE_RETARGETTING: Re-roll targeting for a confused battler. */
void battle_feeling_strange_retargeting(void);

/* ---- Post-battle ---- */

/* RESET_POST_BATTLE_STATS: Clear temporary afflictions from party after battle. */
void battle_reset_post_battle_stats(void);

/* BOSS_BATTLE_CHECK: Returns 0 if any conscious enemy is a boss, 1 otherwise. */
uint16_t battle_boss_battle_check(void);

/* RETURN_BATTLE_ATTACKER_ADDRESS: Returns pointer to battle_attacker_name buffer. */
char *return_battle_attacker_address(void);

/* RETURN_BATTLE_TARGET_ADDRESS: Returns pointer to battle_target_name buffer. */
char *return_battle_target_address(void);

/* ENEMY_FLASHING_OFF: Turn off the currently flashing enemy sprite during targeting. */
void enemy_flashing_off(void);

/* ---- Battle action handlers ---- */

/* Individual battle actions (called via action dispatch table) */
void btlact_bash(void);
void btlact_shoot(void);
void btlact_spy(void);
void btlact_level_1_attack(void);
void btlact_healing_alpha(void);
void btlact_healing_beta(void);
void btlact_healing_gamma(void);
void btlact_healing_omega(void);
void btlact_shield_alpha(void);
void btlact_shield_beta(void);
void btlact_psi_shield_alpha(void);
void btlact_psi_shield_beta(void);
void btlact_psi_fire_common(uint16_t base_damage);
void btlact_psi_rockin_common(uint16_t base_damage);
void btlact_psi_starstorm_common(uint16_t base_damage);

/* HP/PP recovery actions (from asm/battle/actions/) */
void btlact_hp_recovery_10(void);
void btlact_hp_recovery_50(void);
void btlact_hp_recovery_100(void);
void btlact_hp_recovery_200(void);
void btlact_hp_recovery_300(void);
void btlact_hp_recovery_1d4(void);
void btlact_hp_recovery_10000(void);
void btlact_pp_recovery_20(void);
void btlact_pp_recovery_80(void);

/* Simple wrapper actions */
void btlact_double_bash(void);

/* Status effect actions */
void btlact_poison(void);
void btlact_nauseate(void);
void btlact_feel_strange(void);
void btlact_immobilize(void);
void btlact_crying(void);
void btlact_crying2(void);
void btlact_solidify(void);
void btlact_solidify_2(void);
void btlact_mushroomize(void);
void btlact_paralysis_alpha(void);
void btlact_hypnosis_alpha(void);
void btlact_brainshock_alpha(void);

/* PSI Flash sub-effects (called from PSI Flash handler, not via action dispatch) */
void flash_inflict_crying(void);
void flash_inflict_paralysis(void);
void flash_inflict_feeling_strange(void);

/* Stat modification actions */
void btlact_offense_up_alpha(void);
void btlact_defense_down_alpha(void);
void btlact_speed_up_1d4(void);
void btlact_guts_up_1d4(void);
void btlact_reduce_offense(void);
void btlact_reduce_offense_defense(void);
void btlact_sudden_guts_pill(void);
void btlact_defense_spray(void);
void btlact_defense_shower(void);
void btlact_cut_guts(void);

/* Null/empty actions */
void btlact_null(void);
void btlact_enemy_extend(void);
void btlact_null2(void);
void btlact_null3(void);
void btlact_null4(void);
void btlact_null5(void);
void btlact_null6(void);
void btlact_null7(void);
void btlact_null8(void);
void btlact_null9(void);
void btlact_null10(void);
void btlact_null11(void);
void btlact_null12(void);

/* Redirect copies */
void redirect_btlact_brainshock_a_copy(void);
void redirect_btlact_hypnosis_a_copy(void);

/* Physical attack levels */
void battle_level_3_attack(void);
void battle_level_4_attack(void);
void btlact_level_2_attack_poison(void);
void btlact_level_2_attack_diamondize(void);

/* PSI common functions */
void btlact_psi_freeze_common(uint16_t base_damage);

/* PSI wrappers */
void btlact_psi_fire_alpha(void);
void btlact_psi_fire_beta(void);
void btlact_psi_fire_gamma(void);
void btlact_psi_fire_omega(void);
void btlact_psi_freeze_alpha(void);
void btlact_psi_freeze_beta(void);
void btlact_psi_freeze_gamma(void);
void btlact_psi_freeze_omega(void);
void btlact_psi_rockin_alpha(void);
void btlact_psi_rockin_beta(void);
void btlact_psi_rockin_gamma(void);
void btlact_psi_rockin_omega(void);
void btlact_psi_starstorm_alpha(void);
void btlact_psi_starstorm_omega(void);
void btlact_psi_thunder_alpha(void);
void btlact_psi_thunder_beta(void);
void btlact_psi_thunder_gamma(void);
void btlact_psi_thunder_omega(void);

/* Lifeup */
void lifeup_common(uint16_t base_healing);
void btlact_lifeup_alpha(void);
void btlact_lifeup_beta(void);
void btlact_lifeup_gamma(void);
void btlact_lifeup_omega(void);

/* Bottle rockets */
void bottle_rocket_common(uint16_t count);
void btlact_bottle_rocket(void);
void btlact_big_bottle_rocket(void);
void btlact_multi_bottle_rocket(void);

/* Item damage (spray/bomb) */
void insect_spray_common(uint16_t base_damage);
void rust_spray_common(uint16_t base_damage);
void btlact_insecticide_spray(void);
void btlact_xterminator_spray(void);
void btlact_rust_promoter(void);
void btlact_rust_promoter_dx(void);
void btlact_bomb(void);
void btlact_super_bomb(void);
void btlact_350_fire_damage(void);
void btlact_bag_of_dragonite(void);

/* Item/misc actions */
void btlact_yogurt_dispenser(void);
void btlact_snake(void);
/* Status effect actions (additional) */
void btlact_cold(void);
void btlact_inflict_poison(void);
void btlact_paralyze(void);
void btlact_inflict_solidification(void);
void btlact_counter_psi(void);
void btlact_distract(void);
void btlact_neutralize(void);
void apply_neutralize_to_all(void);
void btlact_heal_poison(void);
void btlact_shield_killer(void);

/* Enemy actions */
void btlact_call_for_help(void);
void btlact_sow_seeds(void);
void btlact_hungry_hp_sucker(void);

/* Stealable item system */
uint16_t find_stealable_items(void);
uint16_t select_stealable_item(void);
uint16_t is_item_stealable(uint16_t item_id);

/* Mirror (metamorphose) */
void battle_copy_mirror_data(Battler *dest, const Battler *source);
/* btlact_mirror is static */
/* btlact_rainbow_of_colours is static */
/* btlact_teleport_box is static */

/* Freeze time (multi-hit bash) */
void btlact_freezetime(void);

/* Battle sprite screen clamping */
void clamp_enemies_to_screen_width(void);

/* Redirects (enemy reuse of player PSI) */
void redirect_btlact_brainshock_alpha(void);
void redirect_btlact_hypnosis_alpha(void);
void redirect_btlact_paralysis_alpha(void);
void redirect_btlact_offense_up_alpha(void);
void redirect_btlact_defense_down_alpha(void);
void redirect_btlact_shield_alpha(void);
void redirect_btlact_shield_beta(void);
void redirect_btlact_psi_shield_alpha(void);
void redirect_btlact_psi_shield_beta(void);
/* Additional status/combat actions */
void btlact_diamondize(void);
void btlact_possess(void);
void btlact_steal(void);
void btlact_reduce_pp(void);
void btlact_magnet_a(void);
void btlact_magnet_o(void);
void btlact_random_stat_up_1d4(void);

/* Physical+status combo attacks */
void btlact_handbag_strap(void);
void btlact_mummy_wrap(void);

/* Special item actions */
void btlact_fly_honey(void);

/* PSI Flash */
uint16_t flash_immunity_test(void);
void btlact_psi_flash_alpha(void);
void btlact_psi_flash_beta(void);
void btlact_psi_flash_gamma(void);
void btlact_psi_flash_omega(void);

/* Special enemy death handlers */
void btlact_clumsydeath(void);

/* Character stat recalculation */
void recalc_character_miss_rate(uint16_t character_id);

/* ---- Battle initialization ---- */

/* FIND_NEXT_ENEMY_LETTER (asm/battle/find_next_enemy_letter.asm)
 * Scans battlers_table for enemies with the same ID, marks their letters as used,
 * and returns the next available letter (1='A', 2='B', ..., 26='Z', 0=none left). */
uint8_t find_next_enemy_letter(uint16_t enemy_id);

/* BATTLE_INIT_ENEMY_STATS (asm/battle/init_enemy_stats.asm)
 * Initialize a battler from the enemy configuration table. Sets all stats,
 * PSI resistances, initial status effects, money, and experience. */
void battle_init_enemy_stats(Battler *battler, uint16_t enemy_id);

/* BATTLE_INIT_PLAYER_STATS (asm/battle/init_player_stats.asm)
 * Initialize a battler from a party character's stats. Copies HP, PP, stats,
 * afflictions, and calculates PSI resistances. character is 1-based. */
void battle_init_player_stats(uint16_t character, Battler *target);

/* ---- Battle entry points (asm/battle/init_*.asm) ---- */

/* INIT_BATTLE_COMMON (asm/battle/init_common.asm)
 * Shared battle setup: fades out, runs BATTLE_ROUTINE, updates party.
 * Returns battle result from BATTLE_ROUTINE. */
uint16_t init_battle_common(void);

/* INIT_BATTLE_OVERWORLD (asm/battle/init_overworld.asm)
 * Entry point for random encounters triggered from overworld.
 * Handles instant win, battle, post-battle map reload, entity reset. */
void init_battle_overworld(void);

/* INIT_BATTLE_SCRIPTED (asm/battle/init_scripted.asm)
 * Entry point for scripted battles (from event scripts).
 * Parses enemy group, plays swirl, runs battle, handles post-battle.
 * battle_group: index into BTL_ENTRY_PTR_TABLE.
 * Returns 0=normal, 1=party defeated/special. */
uint16_t init_battle_scripted(uint16_t battle_group);

/* ---- External dependencies (implemented elsewhere) ---- */

/* Battle text display — port of asm/text/display_in_battle_text.asm */
void display_in_battle_text(const uint8_t *text, size_t size);

/* SNES address variants — resolve MSG_BTL_* address, then display with battle wrappers.
 * Use these with #define constants from data/battle_text_data.h. */
void display_text_wait_addr(uint32_t snes_addr, uint32_t param);
void display_in_battle_text_addr(uint32_t snes_addr);
extern void swap_attacker_with_target(void);
extern void set_current_item(uint8_t item);
extern void reset_hppp_rolling(void);

/* Battle sprite palette effects */
extern void setup_battle_sprite_palette_effect(uint16_t palette_index,
                                                uint16_t r, uint16_t g, uint16_t b);
extern void set_battle_sprite_palette_effect_speed(uint16_t speed);
extern void reverse_battle_sprite_palette_effect(uint16_t frames, uint16_t palette_group);
extern void update_battle_sprite_palette_anim(void);
void render_all_battle_sprites(void);
extern void update_battle_screen_effects(void);
extern void wait_and_update_battle_effects(void);
void upload_text_tiles_to_vram(uint16_t param);

/* Targeting helpers */
extern void choose_target(uint16_t attacker_offset);
extern void set_battler_targets_by_action(uint16_t attacker_offset);
extern void fix_attacker_name(uint16_t param);
extern void fix_target_name(void);
extern void set_target_if_targeted(void);
typedef void (*battle_action_fn)(void);
extern void apply_action_to_targets(battle_action_fn action);

/* Battle utility functions */
extern uint16_t is_row_valid(void);
extern uint16_t pick_random_enemy_target(uint16_t attacker_offset);
extern uint16_t find_first_unconscious_party_slot(void);
extern uint16_t find_first_alive_party_member(void);
extern void replace_boss_battler(uint16_t new_enemy_id);
extern void enemy_flashing_on(uint16_t row, uint16_t enemy);
extern void giygas_hurt_prayer(uint16_t base_damage);
extern void instant_win_pp_recovery(void);
extern uint16_t autohealing(uint16_t status_group, uint16_t status_id);
extern uint16_t autolifeup(void);
extern void btlact_vitality_up_1d4(void);
extern void btlact_iq_up_1d4(void);
extern void btlact_luck_up_1d4(void);
extern void btlact_pray_subtle(void);
extern void btlact_pray_warm(void);
extern void btlact_pray_mysterious(void);
extern void btlact_pray_golden(void);
extern void btlact_pray_aroma(void);
extern void btlact_pray_rainbow(void);
extern void btlact_pray_rending_sound(void);

/* Battle sprite OAM data buffer constants (also used by BattleState struct above). */

/* Battle sprite pointer table (loaded from ROM, 110 entries × 5 bytes each) */
#define BATTLE_SPRITES_ENTRY_SIZE 5
#define BATTLE_SPRITES_COUNT 110
extern const uint8_t *battle_sprites_pointers_data;


/* FIND_BATTLE_SPRITE_FOR_ENEMY: Search for enemy in sprite slots, return index or 0. */
uint16_t find_battle_sprite_for_enemy(uint16_t enemy_id);

/* CHECK_ALL_HPPP_METERS_STABLE: Returns 1 if all party HP/PP meters are at target. */
uint16_t check_all_hppp_meters_stable(void);

/* GET_EFFECTIVE_HPPP_METER_SPEED: Returns hp_meter_speed, halved if half mode is on. */
int32_t get_effective_hppp_meter_speed(void);

/* RESET_HPPP_METER_SPEED_IF_STABLE: Clear fastest speed if all meters are stable. */
void reset_hppp_meter_speed_if_stable(void);

/* HP_PP_ROLLER: Port of asm/misc/hp_pp_roller.asm.
 * Called each frame. Processes one party member per frame (cycling via frame_counter).
 * Animates HP/PP values toward their targets using 16.16 fixed-point arithmetic.
 * In flipout mode, oscillates HP between 999 and 1, PP between 999 and 0. */
void hp_pp_roller(void);

/* WAIT_FOR_FADE_WITH_TICK: Loop until fade completes, ticking windows each frame. */
void wait_for_fade_with_tick(void);

/* GET_BATTLE_SPRITE_WIDTH: Return width in 8px tile units for sprite ID (1-based). */
uint16_t get_battle_sprite_width(uint16_t sprite_id);

/* GET_BATTLE_SPRITE_HEIGHT: Return height in 8px tile units for sprite ID (1-based). */
uint16_t get_battle_sprite_height(uint16_t sprite_id);

/* CALCULATE_BATTLER_ROW_WIDTH: Sum widths of all conscious enemies in back row. */
uint16_t calculate_battler_row_width(void);

/* GET_BATTLER_ROW_X_POSITION: Return x position for battler at index in row (0=front, 1=back). */
uint16_t get_battler_row_x_position(uint16_t row, uint16_t index);

/* SORT_BATTLERS_INTO_ROWS: Port of asm/battle/sort_battlers_into_rows.asm.
 * Counts conscious, non-unconscious enemies in each row, then selection-sorts
 * them left-to-right by sprite_x into front_row_battlers/back_row_battlers.
 * Also fills the x/y position arrays (x = sprite_x >> 3, y = baseline - height). */
void sort_battlers_into_rows(void);

/* Port of RENDER_AND_DISABLE_ENTITIES (asm/battle/render_and_disable_entities.asm).
 * Updates party, refreshes entities, renders one frame, then disables all entities
 * (except the entity_fade_entity, which keeps its tick/move callbacks active). */
void render_and_disable_entities(void);

/* INITIALIZE_BATTLE_UI_STATE (asm/battle/initialize_battle_ui_state.asm).
 * Resets all window, text, VWF, and battle UI state to initial values. */
void initialize_battle_ui_state(void);

/* BATTLE_SWIRL_SEQUENCE (asm/overworld/battle_swirl_sequence.asm).
 * Configures and starts the battle swirl screen transition effect. */
void battle_swirl_sequence(void);

/* PSI animation system — visual effects for PSI attacks in battle.
 * PSI_ANIMATION_ENEMY_TARGETS tracks which enemy sprite slots are affected.
 * PSI_ANIMATION_X/Y_OFFSET positions the animation relative to the target. */

/* SHOW_PSI_ANIMATION (asm/battle/show_psi_animation.asm).
 * Initializes PSI visual effect: loads graphics/palette, sets animation state,
 * marks target enemies, darkens BG palettes. */
void show_psi_animation(uint16_t anim_id);

/* UPDATE_PSI_ANIMATION (asm/battle/psi_animation_fill_data.asm).
 * Per-frame updater: advances animation frames, rotates palettes,
 * applies/restores enemy color change effects. */
void update_psi_animation(void);

/* APPLY_PSI_BATTLE_EFFECT (asm/battle/apply_psi_battle_effect.asm).
 * Dispatches battle animation by effect_id:
 *   <35 → SHOW_PSI_ANIMATION, 35-45 → darken+swirl with enemy colours,
 *   46 → wobble, 47 → shake, 49-53 → misc swirl. */
void apply_psi_battle_effect(uint16_t effect_id);

/* CHECK_BATTLE_TARGET_TYPE (asm/battle/check_battle_target_type.asm).
 * Checks current_target: if ghost → return true (no effect).
 * If ally → apply ally_effect, return false.
 * If enemy → apply enemy_effect, return true. */
bool check_battle_target_type(uint16_t ally_effect, uint16_t enemy_effect);

/* DESATURATE_PALETTES (asm/system/palette/desaturate_palettes.asm).
 * Converts first 128 palette entries to greyscale (average R+G+B).
 * Backs up palettes to buffer+0x2000, then replaces with grey. */
void desaturate_palettes(void);

/* ---- PSI constants (from constants/battle.asm) ---- */

/* PSI category flags (PSI_CATEGORY bitmask) */
#define PSI_CAT_OFFENSE   1
#define PSI_CAT_RECOVER   2
#define PSI_CAT_ASSIST    4
#define PSI_CAT_OTHER     8

/* PSI usability flags (PSI_USABILITY bitmask) */
#define PSI_USE_OVERWORLD  1
#define PSI_USE_BATTLE     2

/* Condiment table entry (7 bytes, packed to match ROM binary layout).
 * CONDIMENT_TABLE (asm/data/condiment_table.asm): 43 data entries + 1 zero terminator.
 * Each entry pairs a food item with up to two compatible condiments and
 * the enhanced item_parameters that result from the combination. */
PACKED_STRUCT
typedef struct {
    uint8_t food_id;       /* 0: item ID of the food */
    uint8_t condiment1_id; /* 1: item ID of primary compatible condiment */
    uint8_t condiment2_id; /* 2: item ID of secondary compatible condiment */
    uint8_t strength;      /* 3: effect type (0=HP, 1=PP, 2=full HP, 3=all stats) */
    uint8_t epi;           /* 4: effect-per-item (used by non-Poo characters) */
    uint8_t ep;            /* 5: effect points (used by Poo) */
    uint8_t special;       /* 6: special flag (e.g. Skip Sandwich speed boost) */
} CondimentEntry;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(CondimentEntry, 7);

/* PSI ability table entry (15 bytes, packed to match ROM binary layout).
 * 54 entries in the table. */
#define PSI_MAX_ENTRIES        54

PACKED_STRUCT
typedef struct {
    uint8_t  name;           /*  0: PSI name ID (into PSI_NAME_TABLE) */
    uint8_t  level;          /*  1: PSI suffix level (1=α, 2=β, 3=γ, 4=Σ, 5=Ω) */
    uint8_t  category;       /*  2: PSI_CATEGORY bitmask */
    uint8_t  usability;      /*  3: PSI_USABILITY bitmask */
    uint16_t battle_action;  /*  4: index into battle action table (LE) */
    uint8_t  ness_level;     /*  6: level at which Ness learns this */
    uint8_t  paula_level;    /*  7: level at which Paula learns this */
    uint8_t  poo_level;      /*  8: level at which Poo learns this */
    uint8_t  menu_x;         /*  9: X position in PSI menu */
    uint8_t  menu_y;         /* 10: Y position in PSI menu */
    uint32_t text;           /* 11: SNES address of description text (LE) */
} PsiAbility;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(PsiAbility, 15);

/* PSI ability IDs (from PSI enum in constants/battle.asm) */
#define PSI_TELEPORT_ALPHA   51
#define PSI_TELEPORT_BETA    52

/* ---- PSI ability table (loaded from ROM binary asset) ---- */

/* Pointer to the PSI ability table.  Loaded lazily by
 * ensure_battle_psi_table(). */
extern const PsiAbility *battle_psi_table;
bool ensure_battle_psi_table(void);

/* ---- PSI availability checks ---- */

/* CHECK_CHARACTER_HAS_PSI_ABILITY: Port of asm/text/check_character_has_psi_ability.asm.
 * Returns 1 if char_id has any PSI ability matching usability and category masks. */
uint16_t check_character_has_psi_ability(uint16_t char_id,
                                        uint16_t usability,
                                        uint16_t category);

/* CHECK_PSI_CATEGORY_AVAILABLE: Port of asm/text/check_psi_category_available.asm.
 * Returns nonzero if the character has PSI abilities in the given category (1-3). */
uint16_t check_psi_category_available(uint16_t category, uint16_t char_id);

/* ---- PSI menu shared functions (used by status menu in text.c) ---- */

/* GENERATE_BATTLE_PSI_LIST: Port of asm/battle/ui/generate_battle_psi_list.asm.
 * Cursor move callback that generates the PSI ability list for a category.
 * Reads battle_menu_current_character_id to determine which character. */
void generate_battle_psi_list_callback(uint16_t category);

/* DISPLAY_PSI_TARGET_AND_COST: Port of asm/text/menu/display_psi_target_and_cost.asm.
 * Cursor move callback that shows target type and PP cost for a PSI ability. */
void display_psi_target_and_cost(uint16_t ability_id);

/* DISPLAY_CHARACTER_PSI_LIST: Port of asm/text/menu/display_character_psi_list.asm.
 * Creates TEXT_STANDARD window, sets character name as title,
 * generates the full overworld PSI ability list. */
void display_character_psi_list(uint16_t char_id);

/* DISPLAY_PSI_DESCRIPTION: Port of asm/text/menu/display_psi_description.asm.
 * Cursor move callback for PSI ability selection. Shows target/cost and
 * description text. Caches last_selected_psi_description to avoid redraws. */
void display_psi_description(uint16_t ability_id);


/* ---- Targeting ---- */

/* DETERMINE_TARGETTING (asm/battle/determine_targetting.asm).
 * Core targeting logic for battle actions (items, PSI, etc.).
 * Returns packed (targeting_mode << 8) | target_index, or 0 if cancelled. */
uint16_t determine_targetting(uint16_t action_id, uint16_t char_id);

/* JUMP_TEMP_FUNCTION_POINTER (asm/overworld/jump_temp_function_pointer.asm).
 * Dispatches the battle action at the ROM address stored in
 * temp_function_pointer.  Looks up the address in the btlact_dispatch_table
 * and calls the corresponding C function. */
void jump_temp_function_pointer(void);

/* --- Shared utility functions (promoted from static for sound stone, etc.) --- */

/* FORCE_BLANK_AND_WAIT_VBLANK (asm/system/force_blank_and_wait_vblank.asm)
 * Sets force blank, disables HDMA, cancels fade, waits one frame. */
void force_blank_and_wait_vblank(void);

/* BLANK_SCREEN_AND_WAIT_VBLANK — like above but keeps HDMA and fade. */
void blank_screen_and_wait_vblank(void);

/* LOAD_ENEMY_BATTLE_SPRITES (asm/battle/load_enemy_battle_sprites.asm)
 * Sets PPU to mode 1 + BG3 priority, configures BG/OBJ VRAM locations. */
void load_enemy_battle_sprites(void);

/* SET_COLOR_MATH_FROM_TABLE (asm/system/palette/set_color_math_from_table.asm)
 * Configures PPU color math registers from preset table.
 * index: layer configuration mode (0-10). */
void set_color_math_from_table(uint16_t index);

/* LOAD_BATTLE_BG (asm/battle/load_battle_bg.asm)
 * Loads two battle BG layers with letterbox configuration. */
void load_battle_bg(uint16_t layer1_id, uint16_t layer2_id, uint16_t letterbox_style);

/* RELOAD_MAP (asm/overworld/reload_map.asm)
 * Restores overworld map after battle/menu screens. */
void reload_map(void);

#endif /* GAME_BATTLE_H */
