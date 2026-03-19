/*
 * gen_struct_info.c — Build-time tool that emits JSON struct metadata.
 *
 * Compiled and run by CMake to produce struct_info.json, which the
 * ebtools analyze-dump command uses to decode state dump files.
 *
 * Uses offsetof()/sizeof() to capture exact field layouts, including
 * any compiler padding.
 */

/* Enable name arrays in generated headers (must be before any #include
   that transitively pulls in constants.h → items_generated.h). */
#define ITEMS_INCLUDE_NAMES
#define MUSIC_INCLUDE_NAMES

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

#include "core/types.h"
#include "core/memory.h"
#include "core/math.h"
#include "game/game_state.h"
#include "game/overworld.h"
#include "game/battle.h"
#include "game/display_text.h"
#include "game/window.h"
#include "game/map_loader.h"
#include "game/fade.h"
#include "game/door.h"
#include "game/audio.h"
#include "game/oval_window.h"
#include "game/position_buffer.h"
#include "snes/ppu.h"
#include "entity/entity.h"
#include "music_generated.h"

/* Macros to emit JSON field entries */
#define FIELD(stype, fname, tstr) \
    printf("        {\"name\": \"%s\", \"offset\": %zu, \"size\": %zu, \"type\": \"%s\"}", \
           #fname, offsetof(stype, fname), sizeof(((stype *)0)->fname), tstr)

/* Field with decoder annotation */
#define AFIELD(stype, fname, tstr, decoder) \
    printf("        {\"name\": \"%s\", \"offset\": %zu, \"size\": %zu, " \
           "\"type\": \"%s\", \"decoder\": \"%s\"}", \
           #fname, offsetof(stype, fname), sizeof(((stype *)0)->fname), tstr, decoder)

#define BLOB_FIELD(stype, fname) \
    printf("        {\"name\": \"%s\", \"offset\": %zu, \"size\": %zu, \"type\": \"blob[%zu]\"}", \
           #fname, offsetof(stype, fname), sizeof(((stype *)0)->fname), sizeof(((stype *)0)->fname))

/*
 * LAST_FIELD — compile-time assertion that fname is near the end of stype.
 * Catches missing fields when a struct grows.
 */
#define LAST_FIELD_TOLERANCE 7

#define LAST_FIELD(stype, fname, tstr) \
    _Static_assert(offsetof(stype, fname) + sizeof(((stype *)0)->fname) + \
        LAST_FIELD_TOLERANCE >= sizeof(stype), \
        #stype "." #fname " is not the last field — update gen_struct_info.c"); \
    FIELD(stype, fname, tstr)

#define LAST_AFIELD(stype, fname, tstr, decoder) \
    _Static_assert(offsetof(stype, fname) + sizeof(((stype *)0)->fname) + \
        LAST_FIELD_TOLERANCE >= sizeof(stype), \
        #stype "." #fname " is not the last field — update gen_struct_info.c"); \
    AFIELD(stype, fname, tstr, decoder)

#define LAST_BLOB_FIELD(stype, fname) \
    _Static_assert(offsetof(stype, fname) + sizeof(((stype *)0)->fname) + \
        LAST_FIELD_TOLERANCE >= sizeof(stype), \
        #stype "." #fname " is not the last field — update gen_struct_info.c"); \
    BLOB_FIELD(stype, fname)

/* Macro for emitting enum entries in the "enums" JSON section */
#define ENUM_ENTRY(val) printf("        \"%d\": \"%s\"", val, #val)
#define ENUM_ENTRY_NAMED(val, display) printf("        \"%d\": \"%s\"", val, display)

#define BEGIN_SECTION(name, id, stype) \
    printf("    \"%s\": {\n      \"id\": %d,\n      \"struct_size\": %zu,\n      \"fields\": [\n", \
           name, id, sizeof(stype))

#define END_SECTION() printf("\n      ]\n    }")

#define SEP() printf(",\n")

int main(void) {
    printf("{\n  \"version\": 1,\n  \"sections\": {\n");

    /* ================================================================
     * SECTION_CORE (0x0001) — CoreState
     * ================================================================ */
    BEGIN_SECTION("CORE", 1, CoreState);
    FIELD(CoreState, pad1_raw, "u16"); SEP();
    FIELD(CoreState, pad1_pressed, "u16"); SEP();
    FIELD(CoreState, pad1_held, "u16"); SEP();
    FIELD(CoreState, pad1_autorepeat, "u16"); SEP();
    FIELD(CoreState, pad_timer, "u16"); SEP();
    FIELD(CoreState, nmi_count, "u16"); SEP();
    FIELD(CoreState, frame_counter, "u16"); SEP();
    FIELD(CoreState, play_timer, "u32"); SEP();
    FIELD(CoreState, force_blank, "u8"); SEP();
    FIELD(CoreState, screen_brightness, "u8"); SEP();
    FIELD(CoreState, mosaic_register, "u8"); SEP();
    FIELD(CoreState, game_mode, "u16"); SEP();
    FIELD(CoreState, game_submode, "u16"); SEP();
    LAST_FIELD(CoreState, wait_for_nmi, "u8");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_GAME_STATE (0x0002) — GameState (packed)
     * ================================================================ */
    BEGIN_SECTION("GAME_STATE", 2, GameState);
    AFIELD(GameState, mother2_playername, "u8[12]", "eb_text"); SEP();
    AFIELD(GameState, earthbound_playername, "u8[24]", "eb_text"); SEP();
    AFIELD(GameState, pet_name, "u8[6]", "eb_text"); SEP();
    AFIELD(GameState, favourite_food, "u8[6]", "eb_text"); SEP();
    AFIELD(GameState, favourite_thing, "u8[12]", "eb_text"); SEP();
    FIELD(GameState, money_carried, "u32"); SEP();
    FIELD(GameState, bank_balance, "u32"); SEP();
    FIELD(GameState, party_psi, "u8"); SEP();
    AFIELD(GameState, party_npc_1, "u8", "party_member_id"); SEP();
    AFIELD(GameState, party_npc_2, "u8", "party_member_id"); SEP();
    FIELD(GameState, party_npc_1_hp, "u16"); SEP();
    FIELD(GameState, party_npc_2_hp, "u16"); SEP();
    FIELD(GameState, party_status, "u8"); SEP();
    AFIELD(GameState, party_npc_1_id_copy, "u8", "party_member_id"); SEP();
    AFIELD(GameState, party_npc_2_id_copy, "u8", "party_member_id"); SEP();
    FIELD(GameState, party_npc_1_hp_copy, "u16"); SEP();
    FIELD(GameState, party_npc_2_hp_copy, "u16"); SEP();
    FIELD(GameState, wallet_backup, "u32"); SEP();
    AFIELD(GameState, escargo_express_items, "u8[36]", "array:item_id"); SEP();
    AFIELD(GameState, party_members, "u8[6]", "array:party_member_id"); SEP();
    FIELD(GameState, leader_x_frac, "u16"); SEP();
    FIELD(GameState, leader_x_coord, "u16"); SEP();
    FIELD(GameState, leader_y_frac, "u16"); SEP();
    FIELD(GameState, leader_y_coord, "u16"); SEP();
    FIELD(GameState, position_buffer_index, "u16"); SEP();
    AFIELD(GameState, leader_direction, "u16", "direction"); SEP();
    FIELD(GameState, trodden_tile_type, "u16"); SEP();
    AFIELD(GameState, walking_style, "u16", "walking_style"); SEP();
    FIELD(GameState, leader_moved, "u16"); SEP();
    AFIELD(GameState, character_mode, "u16", "character_mode"); SEP();
    FIELD(GameState, current_party_members, "u16"); SEP();
    AFIELD(GameState, party_order, "u8[6]", "array:party_member_id"); SEP();
    FIELD(GameState, player_controlled_party_members, "u8[6]"); SEP();
    FIELD(GameState, party_entity_slots, "u8[12]"); SEP();
    FIELD(GameState, party_count, "u8"); SEP();
    FIELD(GameState, player_controlled_party_count, "u8"); SEP();
    FIELD(GameState, camera_mode, "u16"); SEP();
    FIELD(GameState, auto_move_frames_left, "u16"); SEP();
    AFIELD(GameState, auto_move_saved_walking_style, "u16", "walking_style"); SEP();
    FIELD(GameState, unknownB6, "u8[3]"); SEP();
    FIELD(GameState, unknownB8, "u8[3]"); SEP();
    FIELD(GameState, auto_fight_enable, "u8"); SEP();
    FIELD(GameState, exit_mouse_x_coord, "u16"); SEP();
    FIELD(GameState, exit_mouse_y_coord, "u16"); SEP();
    FIELD(GameState, text_speed, "u8"); SEP();
    FIELD(GameState, sound_setting, "u8"); SEP();
    FIELD(GameState, unknownC3, "u8"); SEP();
    FIELD(GameState, unknownC4, "u8[4]"); SEP();
    FIELD(GameState, active_hotspot_modes, "u8[2]"); SEP();
    FIELD(GameState, active_hotspot_ids, "u8[2]"); SEP();
    FIELD(GameState, active_hotspot_pointers, "u8[8]"); SEP();
    BLOB_FIELD(GameState, saved_photo_states); SEP();
    FIELD(GameState, timer, "u32"); SEP();
    LAST_AFIELD(GameState, text_flavour, "u8", "text_flavour");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_PARTY_CHARACTERS (0x0003) — CharStruct[6] (packed)
     * Emit fields for a single CharStruct; the section contains 6.
     * ================================================================ */
    {
        printf("    \"PARTY_CHARACTERS\": {\n");
        printf("      \"id\": 3,\n");
        printf("      \"struct_size\": %zu,\n", sizeof(CharStruct) * 6);
        printf("      \"element_size\": %zu,\n", sizeof(CharStruct));
        printf("      \"element_count\": 6,\n");
        printf("      \"element_labels\": {\"0\": \"Ness\", \"1\": \"Paula\", "
               "\"2\": \"Jeff\", \"3\": \"Poo\", \"4\": \"NPC1\", \"5\": \"NPC2\"},\n");
        printf("      \"fields\": [\n");
        AFIELD(CharStruct, name, "u8[5]", "eb_text"); SEP();
        FIELD(CharStruct, level, "u8"); SEP();
        FIELD(CharStruct, exp, "u32"); SEP();
        FIELD(CharStruct, max_hp, "u16"); SEP();
        FIELD(CharStruct, max_pp, "u16"); SEP();
        AFIELD(CharStruct, afflictions, "u8[7]", "afflictions"); SEP();
        FIELD(CharStruct, offense, "u8"); SEP();
        FIELD(CharStruct, defense, "u8"); SEP();
        FIELD(CharStruct, speed, "u8"); SEP();
        FIELD(CharStruct, guts, "u8"); SEP();
        FIELD(CharStruct, luck, "u8"); SEP();
        FIELD(CharStruct, vitality, "u8"); SEP();
        FIELD(CharStruct, iq, "u8"); SEP();
        FIELD(CharStruct, base_offense, "u8"); SEP();
        FIELD(CharStruct, base_defense, "u8"); SEP();
        FIELD(CharStruct, base_speed, "u8"); SEP();
        FIELD(CharStruct, base_guts, "u8"); SEP();
        FIELD(CharStruct, base_luck, "u8"); SEP();
        FIELD(CharStruct, base_vitality, "u8"); SEP();
        FIELD(CharStruct, base_iq, "u8"); SEP();
        AFIELD(CharStruct, items, "u8[14]", "array:item_id"); SEP();
        AFIELD(CharStruct, equipment, "u8[4]", "array:item_id"); SEP();
        FIELD(CharStruct, unknown53, "u16"); SEP();
        AFIELD(CharStruct, previous_walking_style, "u16", "walking_style"); SEP();
        FIELD(CharStruct, unknown57, "u16"); SEP();
        FIELD(CharStruct, unknown59, "u16"); SEP();
        FIELD(CharStruct, position_index, "u16"); SEP();
        FIELD(CharStruct, unknown63, "u16"); SEP();
        AFIELD(CharStruct, buffer_walking_style, "u16", "walking_style"); SEP();
        FIELD(CharStruct, current_hp_fraction, "u16"); SEP();
        FIELD(CharStruct, current_hp, "u16"); SEP();
        FIELD(CharStruct, current_hp_target, "u16"); SEP();
        FIELD(CharStruct, current_pp_fraction, "u16"); SEP();
        FIELD(CharStruct, current_pp, "u16"); SEP();
        FIELD(CharStruct, current_pp_target, "u16"); SEP();
        FIELD(CharStruct, hp_pp_window_options, "u16"); SEP();
        FIELD(CharStruct, miss_rate, "u8"); SEP();
        FIELD(CharStruct, fire_resist, "u8"); SEP();
        FIELD(CharStruct, freeze_resist, "u8"); SEP();
        FIELD(CharStruct, flash_resist, "u8"); SEP();
        FIELD(CharStruct, paralysis_resist, "u8"); SEP();
        FIELD(CharStruct, hypnosis_brainshock_resist, "u8"); SEP();
        FIELD(CharStruct, boosted_speed, "u8"); SEP();
        FIELD(CharStruct, boosted_guts, "u8"); SEP();
        FIELD(CharStruct, boosted_vitality, "u8"); SEP();
        FIELD(CharStruct, boosted_iq, "u8"); SEP();
        FIELD(CharStruct, boosted_luck, "u8"); SEP();
        FIELD(CharStruct, unused_92, "u8"); SEP();
        FIELD(CharStruct, unused_93, "u8"); SEP();
        LAST_FIELD(CharStruct, unknown94, "u8");
        END_SECTION();
    }

    SEP();

    /* ================================================================
     * SECTION_EVENT_FLAGS (0x0004) — uint8_t[128]
     * ================================================================ */
    {
        printf("    \"EVENT_FLAGS\": {\n");
        printf("      \"id\": 4,\n");
        printf("      \"struct_size\": 128,\n");
        printf("      \"fields\": [\n");
        printf("        {\"name\": \"flags\", \"offset\": 0, \"size\": 128, \"type\": \"blob[128]\"}");
        END_SECTION();
    }

    SEP();

    /* ================================================================
     * SECTION_OVERWORLD (0x0005) — OverworldState
     * ================================================================ */
    BEGIN_SECTION("OVERWORLD", 5, OverworldState);
    FIELD(OverworldState, npc_spawns_enabled, "u8"); SEP();
    FIELD(OverworldState, enemy_spawns_enabled, "u8"); SEP();
    FIELD(OverworldState, player_movement_flags, "u8"); SEP();
    FIELD(OverworldState, player_intangibility_frames, "u16"); SEP();
    FIELD(OverworldState, player_has_moved_since_map_load, "u8"); SEP();
    FIELD(OverworldState, player_has_done_something_this_frame, "u8"); SEP();
    FIELD(OverworldState, camera_focus_entity, "i16"); SEP();
    FIELD(OverworldState, disable_music_changes, "u8"); SEP();
    FIELD(OverworldState, enable_auto_sector_music_changes, "u8"); SEP();
    FIELD(OverworldState, footstep_sound_id, "u16"); SEP();
    FIELD(OverworldState, footstep_sound_id_override, "u16"); SEP();
    FIELD(OverworldState, footstep_sound_ignore_entity, "i16"); SEP();
    FIELD(OverworldState, entity_prepared_x, "i16"); SEP();
    FIELD(OverworldState, entity_prepared_y, "i16"); SEP();
    AFIELD(OverworldState, entity_prepared_direction, "i16", "direction"); SEP();
    AFIELD(OverworldState, auto_movement_direction, "u16", "direction"); SEP();
    AFIELD(OverworldState, stairs_direction, "u16", "direction"); SEP();
    AFIELD(OverworldState, escalator_entrance_direction, "u16", "direction"); SEP();
    AFIELD(OverworldState, final_movement_direction, "i16", "direction"); SEP();
    FIELD(OverworldState, not_moving_in_same_direction_faced, "u8"); SEP();
    FIELD(OverworldState, entity_movement_prospective_x, "i16"); SEP();
    FIELD(OverworldState, entity_movement_prospective_y, "i16"); SEP();
    FIELD(OverworldState, checked_collision_left_x, "u16"); SEP();
    FIELD(OverworldState, checked_collision_top_y, "u16"); SEP();
    FIELD(OverworldState, ladder_stairs_tile_x, "i16"); SEP();
    FIELD(OverworldState, ladder_stairs_tile_y, "i16"); SEP();
    FIELD(OverworldState, overworld_status_suppression, "u8"); SEP();
    FIELD(OverworldState, demo_frames_left, "u16"); SEP();
    FIELD(OverworldState, demo_recording_flags, "u16"); SEP();
    FIELD(OverworldState, mushroomized_walking_flag, "u8"); SEP();
    FIELD(OverworldState, mushroomization_timer, "u16"); SEP();
    FIELD(OverworldState, mushroomization_modifier, "u16"); SEP();
    FIELD(OverworldState, debug_flag, "u8"); SEP();
    FIELD(OverworldState, debug_mode_number, "u8"); SEP();
    FIELD(OverworldState, respawn_x, "u16"); SEP();
    FIELD(OverworldState, respawn_y, "u16"); SEP();
    FIELD(OverworldState, current_teleport_destination_x, "u16"); SEP();
    FIELD(OverworldState, current_teleport_destination_y, "u16"); SEP();
    FIELD(OverworldState, loaded_map_tile_combo, "i16"); SEP();
    FIELD(OverworldState, loaded_map_palette, "i16"); SEP();
    FIELD(OverworldState, show_npc_flag, "u8"); SEP();
    BLOB_FIELD(OverworldState, active_hotspots); SEP();
    FIELD(OverworldState, last_sector_x, "u16"); SEP();
    FIELD(OverworldState, last_sector_y, "u16"); SEP();
    AFIELD(OverworldState, current_leader_direction, "u16", "direction"); SEP();
    FIELD(OverworldState, current_leading_party_member_entity, "u16"); SEP();
    FIELD(OverworldState, mini_ghost_entity_id, "i16"); SEP();
    FIELD(OverworldState, possessed_player_count, "u16"); SEP();
    FIELD(OverworldState, hp_alert_shown, "u8[6]"); SEP();
    FIELD(OverworldState, overworld_damage_countdown_frames, "u8[6]"); SEP();
    FIELD(OverworldState, moving_party_member_entity_id, "i16"); SEP();
    FIELD(OverworldState, battle_mode, "u16"); SEP();
    FIELD(OverworldState, input_disable_frame_counter, "u16"); SEP();
    FIELD(OverworldState, overworld_enemy_count, "u16"); SEP();
    FIELD(OverworldState, overworld_enemy_maximum, "u16"); SEP();
    FIELD(OverworldState, magic_butterfly_spawned, "u8"); SEP();
    FIELD(OverworldState, enemy_spawn_range_width, "u16"); SEP();
    FIELD(OverworldState, enemy_spawn_range_height, "u16"); SEP();
    FIELD(OverworldState, enemy_spawn_counter, "u16"); SEP();
    FIELD(OverworldState, enemy_spawn_too_many_failures, "u16"); SEP();
    FIELD(OverworldState, enemy_spawn_encounter_id, "u16"); SEP();
    FIELD(OverworldState, enemy_spawn_remaining_count, "u16"); SEP();
    FIELD(OverworldState, enemy_spawn_chance, "u16"); SEP();
    FIELD(OverworldState, spawning_enemy_group, "u16"); SEP();
    FIELD(OverworldState, spawning_enemy_sprite, "u16"); SEP();
    FIELD(OverworldState, spawning_enemy_name, "u16"); SEP();
    FIELD(OverworldState, battle_swirl_countdown, "u16"); SEP();
    FIELD(OverworldState, enemy_has_been_touched, "u8"); SEP();
    FIELD(OverworldState, pending_interactions, "u16"); SEP();
    BLOB_FIELD(OverworldState, queued_interactions); SEP();
    FIELD(OverworldState, current_queued_interaction, "u16"); SEP();
    FIELD(OverworldState, next_queued_interaction, "u16"); SEP();
    FIELD(OverworldState, current_queued_interaction_type, "u16"); SEP();
    FIELD(OverworldState, interacting_npc_id, "u16"); SEP();
    FIELD(OverworldState, map_object_text, "u32"); SEP();
    FIELD(OverworldState, interacting_npc_entity, "u16"); SEP();
    FIELD(OverworldState, dad_phone_timer, "u16"); SEP();
    FIELD(OverworldState, dad_phone_queued, "u8"); SEP();
    FIELD(OverworldState, pajama_flag, "u8"); SEP();
    FIELD(OverworldState, disabled_transitions, "u8"); SEP();
    FIELD(OverworldState, render_hppp_windows, "u8"); SEP();
    FIELD(OverworldState, redraw_all_windows, "u8"); SEP();
    FIELD(OverworldState, currently_drawn_hppp_windows, "u8"); SEP();
    FIELD(OverworldState, psi_teleport_speed_int, "u16"); SEP();
    AFIELD(OverworldState, psi_teleport_style, "u16", "teleport_style"); SEP();
    FIELD(OverworldState, psi_teleport_destination, "u16"); SEP();
    FIELD(OverworldState, psi_teleport_state, "u16"); SEP();
    FIELD(OverworldState, psi_teleport_beta_angle, "u16"); SEP();
    FIELD(OverworldState, psi_teleport_beta_progress, "u16"); SEP();
    FIELD(OverworldState, psi_teleport_better_progress, "u16"); SEP();
    FIELD(OverworldState, psi_teleport_beta_x_adjustment, "u16"); SEP();
    FIELD(OverworldState, psi_teleport_beta_y_adjustment, "u16"); SEP();
    FIELD(OverworldState, psi_teleport_speed, "i32"); SEP();
    FIELD(OverworldState, psi_teleport_speed_x, "i32"); SEP();
    FIELD(OverworldState, psi_teleport_speed_y, "i32"); SEP();
    FIELD(OverworldState, psi_teleport_next_x, "i32"); SEP();
    FIELD(OverworldState, psi_teleport_next_y, "i32"); SEP();
    FIELD(OverworldState, psi_teleport_success_screen_speed_x, "i16"); SEP();
    FIELD(OverworldState, psi_teleport_success_screen_x, "i16"); SEP();
    FIELD(OverworldState, psi_teleport_success_screen_speed_y, "i16"); SEP();
    FIELD(OverworldState, psi_teleport_success_screen_y, "i16"); SEP();
    FIELD(OverworldState, entity_fade_entity, "i16"); SEP();
    BLOB_FIELD(OverworldState, entity_creation_queue); SEP();
    FIELD(OverworldState, entity_creation_queue_length, "u16"); SEP();
    FIELD(OverworldState, delivery_timers, "i16[10]"); SEP();
    FIELD(OverworldState, delivery_attempts, "i16[10]"); SEP();
    FIELD(OverworldState, spawning_travelling_photographer_id, "u16"); SEP();
    LAST_BLOB_FIELD(OverworldState, post_teleport_callback);
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_BATTLE (0x0006) — BattleState
     * ================================================================ */
    BEGIN_SECTION("BATTLE", 6, BattleState);
    BLOB_FIELD(BattleState, battlers_table); SEP();
    FIELD(BattleState, battler_target_flags, "u32"); SEP();
    FIELD(BattleState, current_attacker, "u16"); SEP();
    FIELD(BattleState, current_target, "u16"); SEP();
    FIELD(BattleState, battle_exp_scratch, "u32"); SEP();
    FIELD(BattleState, battle_money_scratch, "u16"); SEP();
    FIELD(BattleState, giygas_phase, "u16"); SEP();
    FIELD(BattleState, special_defeat, "u16"); SEP();
    FIELD(BattleState, enemy_performing_final_attack, "u16"); SEP();
    FIELD(BattleState, skip_death_text_and_cleanup, "u16"); SEP();
    FIELD(BattleState, shield_has_nullified_damage, "u16"); SEP();
    FIELD(BattleState, damage_is_reflected, "u16"); SEP();
    FIELD(BattleState, is_smaaaash_attack, "u16"); SEP();
    FIELD(BattleState, item_dropped, "u16"); SEP();
    FIELD(BattleState, green_flash_duration, "u16"); SEP();
    FIELD(BattleState, red_flash_duration, "u16"); SEP();
    FIELD(BattleState, reflect_flash_duration, "u16"); SEP();
    FIELD(BattleState, green_background_flash_duration, "u16"); SEP();
    FIELD(BattleState, hp_pp_box_blink_duration, "u16"); SEP();
    FIELD(BattleState, hp_pp_box_blink_target, "u16"); SEP();
    FIELD(BattleState, vertical_shake_duration, "u16"); SEP();
    FIELD(BattleState, vertical_shake_hold_duration, "u16"); SEP();
    FIELD(BattleState, screen_effect_minimum_wait_frames, "u16"); SEP();
    FIELD(BattleState, screen_effect_vertical_offset, "i16"); SEP();
    FIELD(BattleState, screen_effect_horizontal_offset, "i16"); SEP();
    FIELD(BattleState, wobble_duration, "u16"); SEP();
    FIELD(BattleState, shake_duration, "u16"); SEP();
    FIELD(BattleState, background_brightness, "u16"); SEP();
    FIELD(BattleState, highest_enemy_level_in_battle, "u16"); SEP();
    FIELD(BattleState, used_enemy_letters, "u8[26]"); SEP();
    FIELD(BattleState, current_battle_group, "u16"); SEP();
    FIELD(BattleState, enemies_in_battle, "u16"); SEP();
    FIELD(BattleState, enemies_in_battle_ids, "u16[23]"); SEP();
    FIELD(BattleState, party_members_alive_overworld, "u16"); SEP();
    FIELD(BattleState, touched_enemy, "i16"); SEP();
    FIELD(BattleState, battle_initiative, "u16"); SEP();
    FIELD(BattleState, battle_mode_flag, "u16"); SEP();
    AFIELD(BattleState, battle_item_used, "u8", "item_id"); SEP();
    FIELD(BattleState, party_members_with_selected_actions, "u16[6]"); SEP();
    FIELD(BattleState, letterbox_effect_ending, "u16"); SEP();
    FIELD(BattleState, letterbox_top_end, "u16"); SEP();
    FIELD(BattleState, letterbox_bottom_start, "u16"); SEP();
    FIELD(BattleState, letterbox_effect_ending_top, "u16"); SEP();
    FIELD(BattleState, letterbox_effect_ending_bottom, "u16"); SEP();
    FIELD(BattleState, letterbox_visible_screen_value, "u16"); SEP();
    FIELD(BattleState, letterbox_nonvisible_screen_value, "u16"); SEP();
    FIELD(BattleState, letterbox_hdma_table, "u8[18]"); SEP();
    FIELD(BattleState, enable_background_darkening, "u16"); SEP();
    FIELD(BattleState, current_layer_config, "u16"); SEP();
    FIELD(BattleState, mirror_enemy, "u16"); SEP();
    BLOB_FIELD(BattleState, mirror_battler_backup); SEP();
    FIELD(BattleState, mirror_turn_timer, "u16"); SEP();
    FIELD(BattleState, current_flashing_enemy, "i16"); SEP();
    FIELD(BattleState, current_flashing_enemy_row, "u16"); SEP();
    FIELD(BattleState, enemy_targetting_flashing, "u16"); SEP();
    FIELD(BattleState, current_flashing_row, "i16"); SEP();
    FIELD(BattleState, front_row_battlers, "u8[8]"); SEP();
    FIELD(BattleState, back_row_battlers, "u8[8]"); SEP();
    FIELD(BattleState, num_battlers_in_front_row, "u16"); SEP();
    FIELD(BattleState, num_battlers_in_back_row, "u16"); SEP();
    FIELD(BattleState, battler_front_row_x_positions, "u8[8]"); SEP();
    FIELD(BattleState, battler_front_row_y_positions, "u8[8]"); SEP();
    FIELD(BattleState, battler_back_row_x_positions, "u8[8]"); SEP();
    FIELD(BattleState, battler_back_row_y_positions, "u8[8]"); SEP();
    FIELD(BattleState, current_battle_sprites_allocated, "u16"); SEP();
    FIELD(BattleState, current_battle_sprite_enemy_ids, "u16[4]"); SEP();
    FIELD(BattleState, current_battle_sprite_widths, "u16[4]"); SEP();
    FIELD(BattleState, current_battle_sprite_heights, "u16[4]"); SEP();
    FIELD(BattleState, current_battle_spritemaps_allocated, "u16"); SEP();
    FIELD(BattleState, battle_spritemap_allocation_counts, "u16[4]"); SEP();
    BLOB_FIELD(BattleState, battle_spritemaps); SEP();
    BLOB_FIELD(BattleState, alt_battle_spritemaps); SEP();
    FIELD(BattleState, half_hppp_meter_speed, "u8"); SEP();
    FIELD(BattleState, disable_hppp_rolling, "u8"); SEP();
    FIELD(BattleState, hp_meter_speed, "i32"); SEP();
    FIELD(BattleState, hppp_meter_flipout_mode, "u16"); SEP();
    FIELD(BattleState, battle_sprite_palette_effect_speed, "u16"); SEP();
    FIELD(BattleState, battle_sprite_palette_effect_frames_left, "u16[4]"); SEP();
    BLOB_FIELD(BattleState, battle_sprite_palette_effect_deltas); SEP();
    BLOB_FIELD(BattleState, battle_sprite_palette_effect_counters); SEP();
    BLOB_FIELD(BattleState, battle_sprite_palette_effect_steps); SEP();
    FIELD(BattleState, psi_animation_enemy_targets, "u16[4]"); SEP();
    FIELD(BattleState, psi_animation_x_offset, "i16"); SEP();
    FIELD(BattleState, psi_animation_y_offset, "i16"); SEP();
    FIELD(BattleState, last_selected_psi_description, "u16"); SEP();
    FIELD(BattleState, temp_function_pointer, "u32"); SEP();
    FIELD(BattleState, debugging_current_psi_animation, "u16"); SEP();
    FIELD(BattleState, debugging_current_swirl, "u16"); SEP();
    FIELD(BattleState, debugging_current_swirl_flags, "u16"); SEP();
    FIELD(BattleState, background_colour_backup, "u16"); SEP();
    FIELD(BattleState, battle_menu_user, "u8"); SEP();
    FIELD(BattleState, battle_menu_param1, "u8"); SEP();
    FIELD(BattleState, battle_menu_selected_action, "u16"); SEP();
    FIELD(BattleState, battle_menu_targetting, "u8"); SEP();
    LAST_FIELD(BattleState, battle_menu_selected_target, "u8");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_DISPLAY_TEXT (0x0007) — DisplayTextState
     * ================================================================ */
    BEGIN_SECTION("DISPLAY_TEXT", 7, DisplayTextState);
    FIELD(DisplayTextState, text_speed_based_wait, "u16"); SEP();
    FIELD(DisplayTextState, text_prompt_waiting_for_input, "u16"); SEP();
    FIELD(DisplayTextState, text_sound_mode, "u16"); SEP();
    FIELD(DisplayTextState, instant_printing, "u8"); SEP();
    FIELD(DisplayTextState, early_tick_exit, "u8"); SEP();
    FIELD(DisplayTextState, blinking_triangle_flag, "u16"); SEP();
    FIELD(DisplayTextState, battle_attacker_name, "u8[30]"); SEP();
    FIELD(DisplayTextState, battle_target_name, "u8[28]"); SEP();
    FIELD(DisplayTextState, cnum, "u32"); SEP();
    FIELD(DisplayTextState, citem, "u8"); SEP();
    FIELD(DisplayTextState, force_left_text_alignment, "u16"); SEP();
    FIELD(DisplayTextState, pagination_window, "u16"); SEP();
    FIELD(DisplayTextState, pagination_animation_frame, "i16"); SEP();
    FIELD(DisplayTextState, enable_word_wrap, "u16"); SEP();
    FIELD(DisplayTextState, allow_text_overflow, "u8"); SEP();
    FIELD(DisplayTextState, last_printed_character, "u8"); SEP();
    FIELD(DisplayTextState, current_interacting_event_flag, "u16"); SEP();
    FIELD(DisplayTextState, upcoming_word_length, "u16"); SEP();
    FIELD(DisplayTextState, party_member_selection_scripts, "u32[4]"); SEP();
    FIELD(DisplayTextState, attacker_enemy_id, "i16"); SEP();
    FIELD(DisplayTextState, target_enemy_id, "i16"); SEP();
    FIELD(DisplayTextState, print_attacker_article, "u8"); SEP();
    FIELD(DisplayTextState, print_target_article, "u8"); SEP();
    FIELD(DisplayTextState, text_main_register_backup, "u32"); SEP();
    FIELD(DisplayTextState, text_sub_register_backup, "u32"); SEP();
    FIELD(DisplayTextState, text_loop_register_backup, "u8"); SEP();
    FIELD(DisplayTextState, g_cc18_attrs_saved, "u8"); SEP();
    BLOB_FIELD(DisplayTextState, window_text_attrs_backup); SEP();
    FIELD(DisplayTextState, hppp_meter_flipout_mode_hp_backups, "u16[4]"); SEP();
    FIELD(DisplayTextState, hppp_meter_flipout_mode_pp_backups, "u16[4]"); SEP();
    LAST_FIELD(DisplayTextState, last_party_member_status_last_check, "u16");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_WINDOW (0x0008) — WindowSystemState
     * ================================================================ */
    BEGIN_SECTION("WINDOW", 8, WindowSystemState);
    FIELD(WindowSystemState, current_focus_window, "u16"); SEP();
    BLOB_FIELD(WindowSystemState, bg2_buffer); SEP();
    FIELD(WindowSystemState, battle_menu_current_character_id, "i16"); SEP();
    FIELD(WindowSystemState, hppp_meter_area_needs_update, "u16"); SEP();
    BLOB_FIELD(WindowSystemState, hppp_window_buffer); SEP();
    FIELD(WindowSystemState, hppp_window_digit_buffer, "u8[3]"); SEP();
    FIELD(WindowSystemState, upload_hppp_meter_tiles, "u8"); SEP();
    FIELD(WindowSystemState, used_bg2_tile_map, "u16[32]"); SEP();
    BLOB_FIELD(WindowSystemState, windows); SEP();
    FIELD(WindowSystemState, titled_windows, "u16[5]"); SEP();
    FIELD(WindowSystemState, menu_backup_text_x, "u16"); SEP();
    FIELD(WindowSystemState, menu_backup_text_y, "u16"); SEP();
    FIELD(WindowSystemState, menu_backup_current_option, "u16"); SEP();
    LAST_FIELD(WindowSystemState, menu_backup_selected_option, "u16");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_MAP_LOADER (0x0009) — MapLoaderState
     * ================================================================ */
    BEGIN_SECTION("MAP_LOADER", 9, MapLoaderState);
    FIELD(MapLoaderState, current_sector_attributes, "u16"); SEP();
    AFIELD(MapLoaderState, current_map_music_track, "u16", "music_track"); SEP();
    AFIELD(MapLoaderState, next_map_music_track, "u16", "music_track"); SEP();
    FIELD(MapLoaderState, do_map_music_fade, "u16"); SEP();
    BLOB_FIELD(MapLoaderState, map_palette_backup); SEP();
    FIELD(MapLoaderState, map_palette_animation_loaded, "u16"); SEP();
    BLOB_FIELD(MapLoaderState, loaded_collision_tiles); SEP();
    FIELD(MapLoaderState, loaded_animated_tile_count, "u16"); SEP();
    FIELD(MapLoaderState, loaded_tileset_combo, "i16"); SEP();
    FIELD(MapLoaderState, loaded_palette_index, "i16"); SEP();
    BLOB_FIELD(MapLoaderState, overworld_palette_anim); SEP();
    BLOB_FIELD(MapLoaderState, overworld_tileset_anim); SEP();
    BLOB_FIELD(MapLoaderState, animated_map_palette_buffer); SEP();
    BLOB_FIELD(MapLoaderState, animated_tileset_buffer); SEP();
    FIELD(MapLoaderState, screen_left_x, "i16"); SEP();
    FIELD(MapLoaderState, screen_top_y, "i16"); SEP();
    BLOB_FIELD(MapLoaderState, tile_collision_buffer); SEP();
    FIELD(MapLoaderState, tile_collision_loaded, "bool"); SEP();
    FIELD(MapLoaderState, loaded_map_music_entry_offset, "u16"); SEP();
    FIELD(MapLoaderState, saved_colour_average_red, "u16"); SEP();
    FIELD(MapLoaderState, saved_colour_average_green, "u16"); SEP();
    FIELD(MapLoaderState, saved_colour_average_blue, "u16"); SEP();
    FIELD(MapLoaderState, next_your_sanctuary_location_tile_index, "u16"); SEP();
    FIELD(MapLoaderState, total_your_sanctuary_loaded_tileset_tiles, "u16"); SEP();
    FIELD(MapLoaderState, your_sanctuary_loaded_tileset_tiles, "u16"); SEP();
    LAST_FIELD(MapLoaderState, loaded_your_sanctuary_locations, "u16[8]");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_PPU (0x000A) — PPUState
     * ================================================================ */
    BEGIN_SECTION("PPU", 10, PPUState);
    BLOB_FIELD(PPUState, vram); SEP();
    BLOB_FIELD(PPUState, cgram); SEP();
    BLOB_FIELD(PPUState, oam); SEP();
    BLOB_FIELD(PPUState, oam_hi); SEP();
    BLOB_FIELD(PPUState, oam_full_x); SEP();
    BLOB_FIELD(PPUState, oam_full_y); SEP();
    FIELD(PPUState, inidisp, "u8"); SEP();
    FIELD(PPUState, obsel, "u8"); SEP();
    FIELD(PPUState, bgmode, "u8"); SEP();
    FIELD(PPUState, mosaic, "u8"); SEP();
    FIELD(PPUState, bg_sc, "u8[4]"); SEP();
    FIELD(PPUState, bg_nba, "u8[2]"); SEP();
    FIELD(PPUState, bg_hofs, "u16[4]"); SEP();
    FIELD(PPUState, bg_vofs, "u16[4]"); SEP();
    FIELD(PPUState, tm, "u8"); SEP();
    FIELD(PPUState, ts, "u8"); SEP();
    FIELD(PPUState, tmw, "u8"); SEP();
    FIELD(PPUState, tsw, "u8"); SEP();
    FIELD(PPUState, cgwsel, "u8"); SEP();
    FIELD(PPUState, cgadsub, "u8"); SEP();
    FIELD(PPUState, coldata_r, "u8"); SEP();
    FIELD(PPUState, coldata_g, "u8"); SEP();
    FIELD(PPUState, coldata_b, "u8"); SEP();
    FIELD(PPUState, w12sel, "u8"); SEP();
    FIELD(PPUState, w34sel, "u8"); SEP();
    FIELD(PPUState, wobjsel, "u8"); SEP();
    FIELD(PPUState, wh0, "u8"); SEP();
    FIELD(PPUState, wh1, "u8"); SEP();
    FIELD(PPUState, wh2, "u8"); SEP();
    FIELD(PPUState, wh3, "u8"); SEP();
    FIELD(PPUState, wbglog, "u8"); SEP();
    FIELD(PPUState, wobjlog, "u8"); SEP();
    FIELD(PPUState, vram_addr, "u16"); SEP();
    FIELD(PPUState, vmain, "u8"); SEP();
    FIELD(PPUState, oam_addr, "u16"); SEP();
    BLOB_FIELD(PPUState, wh0_table); SEP();
    BLOB_FIELD(PPUState, wh1_table); SEP();
    FIELD(PPUState, window_hdma_active, "bool"); SEP();
    FIELD(PPUState, bg_viewport_fill, "u32[4]"); SEP();
    FIELD(PPUState, sprite_x_offset, "i16"); SEP();
    LAST_FIELD(PPUState, sprite_y_offset, "i16");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_POSITION_BUFFER (0x000B) — PositionBufferState
     * ================================================================ */
    BEGIN_SECTION("POSITION_BUFFER", 11, PositionBufferState);
    BLOB_FIELD(PositionBufferState, player_position_buffer); SEP();
    BLOB_FIELD(PositionBufferState, h_speeds); SEP();
    BLOB_FIELD(PositionBufferState, v_speeds); SEP();
    FIELD(PositionBufferState, camera_mode_3_frames_left, "u16"); SEP();
    FIELD(PositionBufferState, camera_mode_backup, "u16"); SEP();
    LAST_FIELD(PositionBufferState, bicycle_diagonal_turn_counter, "u16");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_DOOR (0x000C) — DoorState
     * ================================================================ */
    BEGIN_SECTION("DOOR", 12, DoorState);
    FIELD(DoorState, door_found, "u16"); SEP();
    FIELD(DoorState, door_found_type, "u16"); SEP();
    FIELD(DoorState, using_door, "u16"); SEP();
    FIELD(DoorState, wipe_palettes_on_map_load, "u16"); SEP();
    FIELD(DoorState, transition_x_velocity, "i32"); SEP();
    FIELD(DoorState, transition_y_velocity, "i32"); SEP();
    FIELD(DoorState, transition_x_accum, "i32"); SEP();
    FIELD(DoorState, transition_y_accum, "i32"); SEP();
    LAST_FIELD(DoorState, door_interactions, "u32[5]");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_ENTITY_RUNTIME (0x000D) — EntityRuntimeState
     * ================================================================ */
    BEGIN_SECTION("ENTITY_RUNTIME", 13, EntityRuntimeState);
    FIELD(EntityRuntimeState, current_entity_offset, "i16"); SEP();
    FIELD(EntityRuntimeState, current_entity_slot, "i16"); SEP();
    FIELD(EntityRuntimeState, current_script_offset, "i16"); SEP();
    FIELD(EntityRuntimeState, current_script_slot, "i16"); SEP();
    FIELD(EntityRuntimeState, next_active_entity, "i16"); SEP();
    FIELD(EntityRuntimeState, actionscript_current_script, "i16"); SEP();
    FIELD(EntityRuntimeState, actionscript_state, "u16"); SEP();
    FIELD(EntityRuntimeState, disable_actionscript, "u16"); SEP();
    FIELD(EntityRuntimeState, last_allocated_script, "i16"); SEP();
    FIELD(EntityRuntimeState, entity_bg_h_offset_lo, "i16[8]"); SEP();
    FIELD(EntityRuntimeState, entity_bg_v_offset_lo, "i16[8]"); SEP();
    FIELD(EntityRuntimeState, entity_bg_h_offset_hi, "i16[8]"); SEP();
    FIELD(EntityRuntimeState, entity_bg_v_offset_hi, "i16[8]"); SEP();
    FIELD(EntityRuntimeState, entity_bg_h_velocity_lo, "i16[8]"); SEP();
    FIELD(EntityRuntimeState, entity_bg_v_velocity_lo, "i16[8]"); SEP();
    FIELD(EntityRuntimeState, entity_bg_h_velocity_hi, "i16[8]"); SEP();
    FIELD(EntityRuntimeState, entity_bg_v_velocity_hi, "i16[8]"); SEP();
    FIELD(EntityRuntimeState, actionscript_backup_x, "i16"); SEP();
    FIELD(EntityRuntimeState, actionscript_backup_y, "i16"); SEP();
    BLOB_FIELD(EntityRuntimeState, entity_callback_flags_backup); SEP();
    BLOB_FIELD(EntityRuntimeState, entity_path_points); SEP();
    BLOB_FIELD(EntityRuntimeState, entity_path_point_counts); SEP();
    FIELD(EntityRuntimeState, pathfinding_target_centre_x, "i16"); SEP();
    FIELD(EntityRuntimeState, pathfinding_target_centre_y, "i16"); SEP();
    FIELD(EntityRuntimeState, pathfinding_target_width, "i16"); SEP();
    FIELD(EntityRuntimeState, pathfinding_target_height, "i16"); SEP();
    BLOB_FIELD(EntityRuntimeState, delivery_paths); SEP();
    FIELD(EntityRuntimeState, pathfinding_enemy_ids, "i16[4]"); SEP();
    FIELD(EntityRuntimeState, pathfinding_enemy_counts, "i16[4]"); SEP();
    FIELD(EntityRuntimeState, enemy_pathfinding_target_entity, "i16"); SEP();
    FIELD(EntityRuntimeState, spritemap_bank, "u8"); SEP();
    FIELD(EntityRuntimeState, current_sprite_drawing_priority, "u16"); SEP();
    FIELD(EntityRuntimeState, oam_write_index, "u16"); SEP();
    FIELD(EntityRuntimeState, new_entity_pos_z, "i16"); SEP();
    FIELD(EntityRuntimeState, new_entity_var, "i16[8]"); SEP();
    FIELD(EntityRuntimeState, new_entity_priority, "i16"); SEP();
    FIELD(EntityRuntimeState, entity_fade_states_buffer, "u16"); SEP();
    FIELD(EntityRuntimeState, entity_fade_states_length, "u16"); SEP();
    FIELD(EntityRuntimeState, title_screen_quick_mode, "u16"); SEP();
    FIELD(EntityRuntimeState, wait_for_naming_screen_actionscript, "u16"); SEP();
    FIELD(EntityRuntimeState, palette_upload_mode, "u8"); SEP();
    BLOB_FIELD(EntityRuntimeState, palettes); SEP();
    LAST_BLOB_FIELD(EntityRuntimeState, buffer);
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_ENTITY_SYSTEM (0x000E) — EntitySystem
     * ================================================================ */
    BEGIN_SECTION("ENTITY_SYSTEM", 14, EntitySystem);
    FIELD(EntitySystem, first_entity, "i16"); SEP();
    FIELD(EntitySystem, last_entity, "i16"); SEP();
    FIELD(EntitySystem, next_entity, "i16[60]"); SEP();
    FIELD(EntitySystem, script_table, "i16[60]"); SEP();
    FIELD(EntitySystem, script_index, "i16[60]"); SEP();
    FIELD(EntitySystem, abs_x, "i16[60]"); SEP();
    FIELD(EntitySystem, abs_y, "i16[60]"); SEP();
    FIELD(EntitySystem, abs_z, "i16[60]"); SEP();
    FIELD(EntitySystem, frac_x, "u16[60]"); SEP();
    FIELD(EntitySystem, frac_y, "u16[60]"); SEP();
    FIELD(EntitySystem, frac_z, "u16[60]"); SEP();
    FIELD(EntitySystem, delta_x, "i16[60]"); SEP();
    FIELD(EntitySystem, delta_y, "i16[60]"); SEP();
    FIELD(EntitySystem, delta_z, "i16[60]"); SEP();
    FIELD(EntitySystem, delta_frac_x, "u16[60]"); SEP();
    FIELD(EntitySystem, delta_frac_y, "u16[60]"); SEP();
    FIELD(EntitySystem, delta_frac_z, "u16[60]"); SEP();
    FIELD(EntitySystem, screen_x, "i16[60]"); SEP();
    FIELD(EntitySystem, screen_y, "i16[60]"); SEP();
    FIELD(EntitySystem, animation_frame, "i16[60]"); SEP();
    FIELD(EntitySystem, current_displayed_sprites, "u16[60]"); SEP();
    FIELD(EntitySystem, draw_priority, "i16[60]"); SEP();
    FIELD(EntitySystem, spritemap_ptr_lo, "u16[60]"); SEP();
    FIELD(EntitySystem, spritemap_ptr_hi, "u16[60]"); SEP();
    FIELD(EntitySystem, move_callback, "u16[60]"); SEP();
    FIELD(EntitySystem, screen_pos_callback, "u16[60]"); SEP();
    FIELD(EntitySystem, draw_callback, "u16[60]"); SEP();
    FIELD(EntitySystem, tick_callback_lo, "u16[60]"); SEP();
    FIELD(EntitySystem, tick_callback_hi, "u16[60]"); SEP();
    BLOB_FIELD(EntitySystem, var); SEP();
    FIELD(EntitySystem, sprite_ids, "u16[60]"); SEP();
    FIELD(EntitySystem, vram_address, "u16[60]"); SEP();
    FIELD(EntitySystem, byte_widths, "u16[60]"); SEP();
    FIELD(EntitySystem, tile_heights, "u16[60]"); SEP();
    FIELD(EntitySystem, graphics_ptr_lo, "u16[60]"); SEP();
    FIELD(EntitySystem, graphics_ptr_hi, "u16[60]"); SEP();
    FIELD(EntitySystem, graphics_sprite_bank, "u16[60]"); SEP();
    FIELD(EntitySystem, spritemap_sizes, "u16[60]"); SEP();
    FIELD(EntitySystem, spritemap_begin_idx, "u16[60]"); SEP();
    FIELD(EntitySystem, sizes, "u16[60]"); SEP();
    FIELD(EntitySystem, hitbox_enabled, "u16[60]"); SEP();
    FIELD(EntitySystem, hitbox_ud_widths, "u16[60]"); SEP();
    FIELD(EntitySystem, hitbox_ud_heights, "u16[60]"); SEP();
    FIELD(EntitySystem, hitbox_lr_widths, "u16[60]"); SEP();
    FIELD(EntitySystem, hitbox_lr_heights, "u16[60]"); SEP();
    FIELD(EntitySystem, upper_lower_body_divides, "u16[60]"); SEP();
    AFIELD(EntitySystem, moving_directions, "i16[60]", "array:direction"); SEP();
    AFIELD(EntitySystem, directions, "i16[60]", "array:direction"); SEP();
    FIELD(EntitySystem, movement_speeds, "u16[60]"); SEP();
    FIELD(EntitySystem, npc_ids, "u16[60]"); SEP();
    FIELD(EntitySystem, enemy_ids, "i16[60]"); SEP();
    FIELD(EntitySystem, enemy_spawn_tiles, "i16[60]"); SEP();
    FIELD(EntitySystem, collided_objects, "i16[60]"); SEP();
    FIELD(EntitySystem, surface_flags, "u16[60]"); SEP();
    FIELD(EntitySystem, obstacle_flags, "u16[60]"); SEP();
    FIELD(EntitySystem, pathfinding_states, "u16[60]"); SEP();
    AFIELD(EntitySystem, walking_styles, "u16[60]", "array:walking_style"); SEP();
    FIELD(EntitySystem, animation_fingerprints, "u16[60]"); SEP();
    FIELD(EntitySystem, overlay_flags, "u8[60]"); SEP();
    FIELD(EntitySystem, use_8dir_sprites, "u16[60]"); SEP();
    FIELD(EntitySystem, butterfly_orbit_direction, "i16[60]"); SEP();
    FIELD(EntitySystem, ripple_overlay_ptrs, "u16[60]"); SEP();
    FIELD(EntitySystem, ripple_next_update, "u16[60]"); SEP();
    FIELD(EntitySystem, ripple_spritemaps, "u16[60]"); SEP();
    FIELD(EntitySystem, big_ripple_overlay_ptrs, "u16[60]"); SEP();
    FIELD(EntitySystem, big_ripple_next_update, "u16[60]"); SEP();
    FIELD(EntitySystem, big_ripple_spritemaps, "u16[60]"); SEP();
    FIELD(EntitySystem, weak_enemy_value, "u16[60]"); SEP();
    FIELD(EntitySystem, sweating_overlay_ptrs, "u16[60]"); SEP();
    FIELD(EntitySystem, sweating_next_update, "u16[60]"); SEP();
    FIELD(EntitySystem, sweating_spritemaps, "u16[60]"); SEP();
    FIELD(EntitySystem, mushroomized_overlay_ptrs, "u16[60]"); SEP();
    FIELD(EntitySystem, mushroomized_next_update, "u16[60]"); SEP();
    FIELD(EntitySystem, mushroomized_spritemaps, "u16[60]"); SEP();
    FIELD(EntitySystem, alloc_min_slot, "u16"); SEP();
    LAST_FIELD(EntitySystem, alloc_max_slot, "u16");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_SCRIPTS (0x000F) — ScriptSystem
     * ================================================================ */
    BEGIN_SECTION("SCRIPTS", 15, ScriptSystem);
    FIELD(ScriptSystem, next_script, "i8[70]"); SEP();
    FIELD(ScriptSystem, sleep_frames, "i16[140]"); SEP();
    FIELD(ScriptSystem, pc, "u16[140]"); SEP();
    FIELD(ScriptSystem, pc_bank, "u8[70]"); SEP();
    FIELD(ScriptSystem, stack_offset, "u8[70]"); SEP();
    FIELD(ScriptSystem, tempvar, "i16[140]"); SEP();
    LAST_BLOB_FIELD(ScriptSystem, stack);
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_SPRITE_PRIORITY (0x0010) — SpritePriorityQueue[4]
     * ================================================================ */
    {
        printf("    \"SPRITE_PRIORITY\": {\n");
        printf("      \"id\": 16,\n");
        printf("      \"struct_size\": %zu,\n", sizeof(SpritePriorityQueue) * 4);
        printf("      \"element_size\": %zu,\n", sizeof(SpritePriorityQueue));
        printf("      \"element_count\": 4,\n");
        printf("      \"fields\": [\n");
        FIELD(SpritePriorityQueue, spritemaps, "u16[64]"); SEP();
        FIELD(SpritePriorityQueue, sprite_x, "i16[64]"); SEP();
        FIELD(SpritePriorityQueue, sprite_y, "i16[64]"); SEP();
        FIELD(SpritePriorityQueue, spritemap_banks, "u16[64]"); SEP();
        LAST_FIELD(SpritePriorityQueue, offset, "u16");
        END_SECTION();
    }

    SEP();

    /* ================================================================
     * SECTION_FADE (0x0011) — FadeState
     * ================================================================ */
    BEGIN_SECTION("FADE", 17, FadeState);
    FIELD(FadeState, target, "u8"); SEP();
    FIELD(FadeState, step, "i8"); SEP();
    FIELD(FadeState, delay, "u8"); SEP();
    FIELD(FadeState, counter, "u8"); SEP();
    FIELD(FadeState, fading, "bool"); SEP();
    LAST_FIELD(FadeState, updated_this_frame, "bool");
    END_SECTION();

    SEP();

    /* ================================================================
     * SECTION_RNG (0x0012) — RNGState
     * ================================================================ */
    BEGIN_SECTION("RNG", 18, RNGState);
    FIELD(RNGState, a, "u16"); SEP();
    LAST_FIELD(RNGState, b, "u16");
    END_SECTION();

    SEP();

#ifdef ENABLE_AUDIO
    /* ================================================================
     * SECTION_AUDIO (0x0013) — AudioState
     * ================================================================ */
    BEGIN_SECTION("AUDIO", 19, AudioState);
    AFIELD(AudioState, current_music_track, "u16", "music_track"); SEP();
    FIELD(AudioState, current_primary_sample_pack, "u8"); SEP();
    FIELD(AudioState, current_secondary_sample_pack, "u8"); SEP();
    FIELD(AudioState, current_sequence_pack, "u8"); SEP();
    LAST_FIELD(AudioState, unknown_7eb543, "u8");
    END_SECTION();

    SEP();
#endif

    /* ================================================================
     * SECTION_PSI_ANIMATION (0x0014) — PsiAnimationState
     * ================================================================ */
    BEGIN_SECTION("PSI_ANIMATION", 20, PsiAnimationState);
    FIELD(PsiAnimationState, time_until_next_frame, "u8"); SEP();
    FIELD(PsiAnimationState, frame_hold_frames, "u8"); SEP();
    FIELD(PsiAnimationState, total_frames, "u8"); SEP();
    FIELD(PsiAnimationState, frame_data, "u32"); SEP();
    FIELD(PsiAnimationState, palette_animation_lower_index, "u8"); SEP();
    FIELD(PsiAnimationState, palette_animation_upper_index, "u8"); SEP();
    FIELD(PsiAnimationState, palette_animation_current_index, "u8"); SEP();
    FIELD(PsiAnimationState, palette_animation_frames, "u8"); SEP();
    FIELD(PsiAnimationState, palette_animation_time_until_next_frame, "u8"); SEP();
    FIELD(PsiAnimationState, palette, "u16[16]"); SEP();
    FIELD(PsiAnimationState, displayed_palette, "u16"); SEP();
    FIELD(PsiAnimationState, enemy_colour_change_start_frames_left, "u16"); SEP();
    FIELD(PsiAnimationState, enemy_colour_change_frames_left, "u16"); SEP();
    FIELD(PsiAnimationState, enemy_colour_change_red, "u16"); SEP();
    FIELD(PsiAnimationState, enemy_colour_change_green, "u16"); SEP();
    FIELD(PsiAnimationState, enemy_colour_change_blue, "u16"); SEP();
    LAST_FIELD(PsiAnimationState, arr_bundle_buf, "skip");
    END_SECTION();

    /* ================================================================
     * Enums section — emitted from C constants so they stay in sync
     * ================================================================ */
    printf("\n  },\n  \"enums\": {\n");

    /* party_member_id */
    printf("    \"party_member_id\": {\n");
    ENUM_ENTRY_NAMED(PARTY_MEMBER_NONE, "None"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_NESS, "Ness"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_PAULA, "Paula"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_JEFF, "Jeff"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_POO, "Poo"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_POKEY, "Pokey"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_PICKY, "Picky"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_KING, "King"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_TONY, "Tony"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_BUBBLE_MONKEY, "Bubble Monkey"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_DUNGEON_MAN, "Dungeon Man"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_FLYING_MAN1, "Flying Man 1"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_FLYING_MAN2, "Flying Man 2"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_FLYING_MAN3, "Flying Man 3"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_FLYING_MAN4, "Flying Man 4"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_FLYING_MAN5, "Flying Man 5"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_TEDDY_BEAR, "Teddy Bear"); SEP();
    ENUM_ENTRY_NAMED(PARTY_MEMBER_PLUSH_TEDDY_BEAR, "Plush Teddy Bear");
    printf("\n    },\n");

    /* direction */
    printf("    \"direction\": {\n");
    ENUM_ENTRY_NAMED(DIRECTION_UP, "UP"); SEP();
    ENUM_ENTRY_NAMED(DIRECTION_UP_RIGHT, "UP_RIGHT"); SEP();
    ENUM_ENTRY_NAMED(DIRECTION_RIGHT, "RIGHT"); SEP();
    ENUM_ENTRY_NAMED(DIRECTION_DOWN_RIGHT, "DOWN_RIGHT"); SEP();
    ENUM_ENTRY_NAMED(DIRECTION_DOWN, "DOWN"); SEP();
    ENUM_ENTRY_NAMED(DIRECTION_DOWN_LEFT, "DOWN_LEFT"); SEP();
    ENUM_ENTRY_NAMED(DIRECTION_LEFT, "LEFT"); SEP();
    ENUM_ENTRY_NAMED(DIRECTION_UP_LEFT, "UP_LEFT"); SEP();
    ENUM_ENTRY_NAMED(DIRECTION_NONE, "NONE");
    printf("\n    },\n");

    /* walking_style */
    printf("    \"walking_style\": {\n");
    ENUM_ENTRY_NAMED(WALKING_STYLE_NORMAL, "NORMAL"); SEP();
    ENUM_ENTRY_NAMED(WALKING_STYLE_BICYCLE, "BICYCLE"); SEP();
    ENUM_ENTRY_NAMED(WALKING_STYLE_GHOST, "GHOST"); SEP();
    ENUM_ENTRY_NAMED(WALKING_STYLE_SLOWER, "SLOWER"); SEP();
    ENUM_ENTRY_NAMED(WALKING_STYLE_LADDER, "LADDER"); SEP();
    ENUM_ENTRY_NAMED(WALKING_STYLE_ROPE, "ROPE"); SEP();
    ENUM_ENTRY_NAMED(WALKING_STYLE_SLOWEST, "SLOWEST"); SEP();
    ENUM_ENTRY_NAMED(WALKING_STYLE_ESCALATOR, "ESCALATOR"); SEP();
    ENUM_ENTRY_NAMED(WALKING_STYLE_STAIRS, "STAIRS");
    printf("\n    },\n");

    /* character_mode */
    printf("    \"character_mode\": {\n");
    ENUM_ENTRY_NAMED(CHARACTER_MODE_NORMAL, "NORMAL"); SEP();
    ENUM_ENTRY_NAMED(CHARACTER_MODE_SMALL, "SMALL"); SEP();
    ENUM_ENTRY_NAMED(CHARACTER_MODE_GHOST, "GHOST"); SEP();
    ENUM_ENTRY_NAMED(CHARACTER_MODE_ROBOT, "ROBOT"); SEP();
    ENUM_ENTRY_NAMED(CHARACTER_MODE_BICYCLE, "BICYCLE");
    printf("\n    },\n");

    /* teleport_style */
    printf("    \"teleport_style\": {\n");
    ENUM_ENTRY_NAMED(TELEPORT_STYLE_NONE, "NONE"); SEP();
    ENUM_ENTRY_NAMED(TELEPORT_STYLE_PSI_ALPHA, "PSI_ALPHA"); SEP();
    ENUM_ENTRY_NAMED(TELEPORT_STYLE_PSI_BETA, "PSI_BETA"); SEP();
    ENUM_ENTRY_NAMED(TELEPORT_STYLE_INSTANT, "INSTANT"); SEP();
    ENUM_ENTRY_NAMED(TELEPORT_STYLE_PSI_BETTER, "PSI_BETTER"); SEP();
    ENUM_ENTRY_NAMED(TELEPORT_STYLE_STAR_MASTER, "STAR_MASTER");
    printf("\n    },\n");

    /* text_flavour */
    printf("    \"text_flavour\": {\n");
    ENUM_ENTRY_NAMED(TEXT_FLAVOUR_NONE, "None"); SEP();
    ENUM_ENTRY_NAMED(TEXT_FLAVOUR_PLAIN, "Plain"); SEP();
    ENUM_ENTRY_NAMED(TEXT_FLAVOUR_MINT, "Mint"); SEP();
    ENUM_ENTRY_NAMED(TEXT_FLAVOUR_STRAWBERRY, "Strawberry"); SEP();
    ENUM_ENTRY_NAMED(TEXT_FLAVOUR_BANANA, "Banana"); SEP();
    ENUM_ENTRY_NAMED(TEXT_FLAVOUR_PEANUT, "Peanut");
    printf("\n    },\n");

    /* status_group */
    printf("    \"status_group\": {\n");
    ENUM_ENTRY_NAMED(STATUS_GROUP_PERSISTENT_EASYHEAL, "EasyHeal"); SEP();
    ENUM_ENTRY_NAMED(STATUS_GROUP_PERSISTENT_HARDHEAL, "HardHeal"); SEP();
    ENUM_ENTRY_NAMED(STATUS_GROUP_TEMPORARY, "Temporary"); SEP();
    ENUM_ENTRY_NAMED(STATUS_GROUP_STRANGENESS, "Strange"); SEP();
    ENUM_ENTRY_NAMED(STATUS_GROUP_CONCENTRATION, "Concentration"); SEP();
    ENUM_ENTRY_NAMED(STATUS_GROUP_HOMESICKNESS, "Homesick"); SEP();
    ENUM_ENTRY_NAMED(STATUS_GROUP_SHIELD, "Shield");
    printf("\n    },\n");

    /* status_0 — persistent easy heal */
    printf("    \"status_0\": {\n");
    ENUM_ENTRY_NAMED(0, "OK"); SEP();
    ENUM_ENTRY_NAMED(STATUS_0_UNCONSCIOUS, "Unconscious"); SEP();
    ENUM_ENTRY_NAMED(STATUS_0_DIAMONDIZED, "Diamondized"); SEP();
    ENUM_ENTRY_NAMED(STATUS_0_PARALYZED, "Paralyzed"); SEP();
    ENUM_ENTRY_NAMED(STATUS_0_NAUSEOUS, "Nauseous"); SEP();
    ENUM_ENTRY_NAMED(STATUS_0_POISONED, "Poisoned"); SEP();
    ENUM_ENTRY_NAMED(STATUS_0_SUNSTROKE, "Sunstroke"); SEP();
    ENUM_ENTRY_NAMED(STATUS_0_COLD, "Cold");
    printf("\n    },\n");

    /* status_1 — persistent hard heal */
    printf("    \"status_1\": {\n");
    ENUM_ENTRY_NAMED(0, "OK"); SEP();
    ENUM_ENTRY_NAMED(STATUS_1_MUSHROOMIZED, "Mushroomized"); SEP();
    ENUM_ENTRY_NAMED(STATUS_1_POSSESSED, "Possessed");
    printf("\n    },\n");

    /* status_2 — temporary */
    printf("    \"status_2\": {\n");
    ENUM_ENTRY_NAMED(0, "OK"); SEP();
    ENUM_ENTRY_NAMED(STATUS_2_ASLEEP, "Asleep"); SEP();
    ENUM_ENTRY_NAMED(STATUS_2_CRYING, "Crying"); SEP();
    ENUM_ENTRY_NAMED(STATUS_2_IMMOBILIZED, "Immobilized"); SEP();
    ENUM_ENTRY_NAMED(STATUS_2_SOLIDIFIED, "Solidified");
    printf("\n    },\n");

    /* status_3 — strangeness */
    printf("    \"status_3\": {\n");
    ENUM_ENTRY_NAMED(0, "OK"); SEP();
    ENUM_ENTRY_NAMED(STATUS_3_STRANGE, "Strange");
    printf("\n    },\n");

    /* status_4 — concentration */
    printf("    \"status_4\": {\n");
    ENUM_ENTRY_NAMED(0, "OK"); SEP();
    ENUM_ENTRY_NAMED(STATUS_4_CANT_CONCENTRATE, "CantConcentrate");
    printf("\n    },\n");

    /* status_5 — homesickness */
    printf("    \"status_5\": {\n");
    ENUM_ENTRY_NAMED(0, "OK"); SEP();
    ENUM_ENTRY_NAMED(STATUS_5_HOMESICK, "Homesick");
    printf("\n    },\n");

    /* status_6 — shield */
    printf("    \"status_6\": {\n");
    ENUM_ENTRY_NAMED(0, "None"); SEP();
    ENUM_ENTRY_NAMED(STATUS_6_PSI_SHIELD_POWER, "PSI Shield Power"); SEP();
    ENUM_ENTRY_NAMED(STATUS_6_PSI_SHIELD, "PSI Shield"); SEP();
    ENUM_ENTRY_NAMED(STATUS_6_SHIELD_POWER, "Shield Power"); SEP();
    ENUM_ENTRY_NAMED(STATUS_6_SHIELD, "Shield");
    printf("\n    },\n");

    /* item_id — generated from items.json */
    printf("    \"item_id\": {\n");
    {
        int first = 1;
        for (int i = 0; i < ITEM_ID_COUNT; i++) {
            if (ITEM_ID_NAMES[i] != NULL) {
                if (!first) SEP();
                first = 0;
                printf("        \"%d\": \"%s\"", i, ITEM_ID_NAMES[i]);
            }
        }
    }
    printf("\n    },\n");

    /* music_track — generated from music_tracks.json */
    printf("    \"music_track\": {\n");
    {
        int first = 1;
        for (int i = 0; i < MUSIC_TRACK_COUNT; i++) {
            if (MUSIC_TRACK_NAMES[i] != NULL) {
                if (!first) SEP();
                first = 0;
                printf("        \"%d\": \"%s\"", i, MUSIC_TRACK_NAMES[i]);
            }
        }
    }
    printf("\n    }\n");

    printf("  }\n}\n");
    return 0;
}
