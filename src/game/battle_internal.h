/*
 * Battle system internal header.
 *
 * Shared declarations for battle sub-files (battle.c, battle_actions.c).
 * NOT for external consumers — use battle.h instead.
 */
#ifndef GAME_BATTLE_INTERNAL_H
#define GAME_BATTLE_INTERNAL_H

#include "game/battle.h"
#include "core/math.h"

/* ---- RNG helpers ---- */
static inline uint8_t rand_byte(void) {
    return rng_next_byte();
}

/* RAND_LIMIT equivalent: (rand_byte * limit) >> 8, giving [0, limit) */
static inline uint16_t rand_limit(uint16_t limit) {
    return (uint16_t)(((uint32_t)rand_byte() * limit) >> 8);
}

/* ---- Targeting mode constants ---- */
#define TARGETTED_ALLIES  0
#define TARGETTED_SINGLE  1
#define TARGETTED_ROW     2
#define TARGETTED_ALL     4
#define TARGETTED_ENEMIES 16

/* ---- EB character encoding constants ---- */
#define EB_CHAR_SPACE     0x50
#define EB_CHAR_A_MINUS_1 0x70  /* CHAR::A_ - 1 = 0x71 - 1 */

/* ---- Shared data tables ---- */
extern const uint16_t dead_targettable_actions[];
extern const uint8_t *npc_ai_table;

/* ---- Sound effect IDs used across battle sub-files ---- */
#define SFX_RECOVER_HP         36

/* ---- Functions defined in battle.c, called by battle_actions.c ---- */

/* Battle flow helpers */
void display_in_battle_text_addr(uint32_t addr);
void display_text_with_prompt(const uint8_t *text, size_t size);
void display_text_with_prompt_addr(uint32_t snes_addr);
void display_battle_cutscene_text(uint16_t group, uint16_t music, uint32_t text_addr);
void play_giygas_weakened_sequence(uint16_t music, uint32_t text_addr);
void battle_wait(uint16_t frames);

/* Battle scene/setup */
void load_battle_scene(uint16_t battle_group, uint16_t music_id);
void load_battle_sprite(uint16_t sprite_id);
void setup_battle_enemy_sprites(void);
uint16_t layout_enemy_battle_positions(void);
void initialize_battle_party(uint16_t param);

/* Display/palette helpers */
void load_attack_palette(uint16_t type);
void set_coldata(uint8_t red, uint8_t green, uint8_t blue);
void set_colour_addsub_mode(uint8_t cgwsel_val, uint8_t cgadsub_val);
void restore_bg_palette_and_enable_display(void);
void set_palette_upload_mode(uint16_t mode);
void build_letterbox_hdma_table(void);

/* Battler helpers */
void set_battler_target(uint16_t attacker_offset, uint16_t target_index);
void set_battler_pp_from_target(uint16_t attacker_offset, uint16_t pp_cost);
void check_dead_players(void);
uint16_t copy_enemy_name(const uint8_t *src, uint8_t *dest, uint16_t length, uint16_t dest_size);
void consume_used_battle_item(void);
void clear_battle_visual_effects(void);

/* Far wrappers */
void redirect_show_hppp_windows(void);
void redirect_close_focus_window(void);
void select_battle_menu_character_far(uint16_t party_slot);
void clear_battle_menu_character_indicator_far(void);
void set_current_item_far(uint8_t item);
uint16_t enemy_select_mode(uint16_t current_group);
void close_all_windows_and_hide_hppp(void);


/* ---- Dispatch table type ---- */
typedef struct {
    uint32_t rom_addr;
    void (*func)(void);
} BattleActionEntry;

/* ---- Functions defined in battle_calc.c ---- */

/* Success/probability checks */
uint16_t battle_success_255(uint16_t threshold);
uint16_t battle_success_500(uint16_t threshold);
uint16_t battle_success_speed(uint16_t base_chance);
uint16_t battle_success_luck40(void);
uint16_t battle_success_luck80(void);

/* Damage variance */
uint16_t battle_25pct_variance(uint16_t value);
uint16_t battle_50pct_variance(uint16_t value);

/* PSI resistance modifiers */
uint8_t battle_calc_psi_dmg_modifier(uint8_t resist_level);
uint8_t battle_calc_psi_res_modifier(uint8_t resist_level);

/* Stat modification */
void battle_increase_offense(Battler *target);
void battle_decrease_offense(Battler *target);
void battle_increase_defense(Battler *target);
void battle_decrease_defense(Battler *target);

/* Shield handling */
uint16_t battle_psi_shield_nullify(void);
void battle_weaken_shield(void);
uint16_t battle_shields_common(Battler *target, uint16_t shield_type);
uint16_t battle_get_shield_targeting(uint16_t action);

/* Dodge/miss/smash */
uint16_t battle_determine_dodge(void);
uint16_t battle_miss_calc(uint16_t miss_message_type);
uint16_t battle_smaaaash(void);

/* Damage calculation pipeline */
uint16_t battle_get_action_type(uint16_t action_id);
uint16_t battle_calc_damage(uint16_t target_offset, uint16_t damage);
uint16_t battle_calc_resist_damage(uint16_t damage, uint16_t resist_modifier);

/* Physical attack levels */
void battle_level_1_attack(void);
void battle_level_2_attack(void);
void battle_level_3_attack(void);
void battle_level_4_attack(void);

/* Status/HP helpers */
void battle_heal_strangeness(void);
void battle_lose_hp_status(Battler *target, uint16_t amount);
uint16_t battle_fail_attack_on_npcs(void);
void recalc_character_miss_rate(uint16_t character_id);

/* ---- Functions defined in battle_targeting.c ---- */

/* Target selection UI */
uint16_t determine_targetting(uint16_t action_id, uint16_t char_id);
void choose_target(uint16_t attacker_offset);
void set_target_if_targeted(void);
bool check_battle_target_type(uint16_t ally_effect, uint16_t enemy_effect);
uint16_t pick_random_enemy_target(uint16_t attacker_offset);
uint16_t is_row_valid(void);

uint16_t select_battle_target_dispatch(uint16_t mode, uint16_t allow_cancel,
                                              uint16_t action_param);

/* Mask-based targeting operations */
void battle_target_battler(uint16_t battler_index);
void battle_remove_target(uint16_t battler_index);
uint16_t battle_is_char_targeted(uint16_t battler_index);
void battle_target_all(void);
void battle_target_all_enemies(void);
uint32_t battle_random_targeting(uint32_t target_mask);
void battle_target_row(uint16_t param);
void battle_remove_dead_targeting(void);
uint16_t battle_check_if_valid_target(uint16_t battler_index);
void battle_remove_status_untargettable_targets(void);
void set_battler_targets_by_action(uint16_t attacker_offset);
void battle_target_allies(void);
void battle_remove_npc_targeting(void);
void battle_feeling_strange_retargeting(void);

uint16_t determine_battle_item_target(void);

/* ---- Functions defined in battle_psi.c ---- */

bool ensure_battle_psi_table(void);
uint16_t check_character_has_psi_ability(uint16_t char_id,
                                        uint16_t usability,
                                        uint16_t category);
uint16_t check_psi_category_available(uint16_t category, uint16_t char_id);
void generate_battle_psi_list_callback(uint16_t category);
void display_character_psi_list(uint16_t char_id);
void display_psi_target_and_cost(uint16_t ability_id);
void display_psi_description(uint16_t ability_id);
void show_psi_animation(uint16_t anim_id);
void update_psi_animation(void);
void apply_psi_battle_effect(uint16_t effect_id);
uint16_t battle_psi_menu(void);

/* ---- Functions defined in battle_ui.c ---- */

/* Sprite palette effects */
void setup_battle_sprite_palette_effect(uint16_t palette_index,
                                         uint16_t r, uint16_t g, uint16_t b);
void set_battle_sprite_palette_effect_speed(uint16_t speed);
void reverse_battle_sprite_palette_effect(uint16_t frames, uint16_t palette_group);
void update_battle_sprite_palette_anim(void);

/* Battle sprite rendering */
void render_all_battle_sprites(void);
uint16_t find_battle_sprite_for_enemy(uint16_t enemy_id);
uint16_t get_battle_sprite_width(uint16_t sprite_id);
uint16_t get_battle_sprite_height(uint16_t sprite_id);
uint16_t calculate_battler_row_width(void);
void sort_battlers_into_rows(void);
uint16_t get_battler_row_x_position(uint16_t row, uint16_t index);
void clamp_enemies_to_screen_width(void);

/* Enemy flashing */
void enemy_flashing_off(void);
void enemy_flashing_on(uint16_t row, uint16_t enemy);

/* Focus window */
void clear_focus_window_content_far(void);

/* Scene loading and setup */
void force_blank_and_wait_vblank(void);
void set_color_math_from_table(uint16_t index);
void load_enemy_battle_sprites(void);
void upload_text_tiles_to_vram(uint16_t param);
void desaturate_palettes(void);
void blank_screen_and_wait_vblank(void);
void initialize_battle_ui_state(void);

/* Screen effects */
void update_battle_screen_effects(void);
void wait_and_update_battle_effects(void);

/* ---- Functions defined in battle_actions.c, called by battle.c ---- */

/* Action dispatch */
void jump_temp_function_pointer(void);

/* Stealable item shared state */
#define MAX_STEALABLE_ITEMS (14 * 4)
extern uint8_t stealable_item_candidates[MAX_STEALABLE_ITEMS];

/* Stealable item helpers (used by perform_action) */
uint16_t select_stealable_item(void);
uint16_t is_item_stealable(uint16_t item_id);

/* Auto-healing (used by perform_action) */
uint16_t autohealing(uint16_t status_group, uint16_t status_id);
uint16_t autolifeup(void);

/* Flash immunity (used by battle.c) */
uint16_t flash_immunity_test(void);

/* Stealable items (used by battle.c) */
uint16_t find_stealable_items(void);

#endif /* GAME_BATTLE_INTERNAL_H */
