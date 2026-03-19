/*
 * Internal header shared between callroutine*.c files.
 * Not part of the public API — only #include from callroutine implementation files.
 */
#ifndef CALLROUTINE_INTERNAL_H
#define CALLROUTINE_INTERNAL_H

#include <stdint.h>
#include "data/event_script_data.h"

/* Bank index for script read helpers (sb/sw).
 * Set by callroutine_dispatch() before each callroutine invocation. */
extern int cr_bank_idx;

/* Convenience read helpers — read from the current script bank. */
static inline uint8_t sb(uint16_t pc) {
    return script_bank_read_byte(cr_bank_idx, pc);
}
static inline uint16_t sw(uint16_t pc) {
    return script_bank_read_word(cr_bank_idx, pc);
}

/* ---- Movement helpers (callroutine_movement.c) ---- */
void set_velocity_from_direction(int16_t ent, int16_t direction);
int16_t quantize_direction(int16_t raw_dir);
int16_t quantize_fine_direction(uint16_t fine_dir);
int16_t quantize_entity_direction(int16_t entity_offset, uint16_t fine_dir);
int16_t clamp_to_range(int16_t val, int16_t max);
void update_bubble_monkey_mode(int16_t char_id, uint16_t position_index);

int16_t cr_set_entity_direction_velocity(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_update_entity_animation(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc);
int16_t cr_clear_current_entity_collision(int16_t ent, int16_t scr,
                                           uint16_t pc, uint16_t *out_pc);

/* Inline movement cases extracted from the dispatch switch */
int16_t cr_set_entity_movement_speed(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc);
int16_t cr_set_entity_movement_speed_entry2(int16_t ent, int16_t scr,
                                             uint16_t pc, uint16_t *out_pc);
int16_t cr_get_entity_movement_speed(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc);
int16_t cr_set_direction8(int16_t ent, int16_t scr,
                           uint16_t pc, uint16_t *out_pc);
int16_t cr_set_direction(int16_t ent, int16_t scr,
                          uint16_t pc, uint16_t *out_pc);
int16_t cr_get_entity_direction(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_rand_high_byte(int16_t ent, int16_t scr,
                           uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_set_dir_velocity(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_get_entity_obstacle_flags(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc);
int16_t cr_get_entity_pathfinding_state(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_set_surface_flags(int16_t ent, int16_t scr,
                              uint16_t pc, uint16_t *out_pc);
int16_t cr_set_entity_velocity_from_dir(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_move_entity_distance(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_halve_entity_delta_y(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_load_current_map_block_events(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_reverse_direction_8(int16_t ent, int16_t scr,
                                uint16_t pc, uint16_t *out_pc);
int16_t cr_quantize_entity_direction(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc);
int16_t cr_calculate_direction_to_target(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_disable_entity_collision2(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_display_text(int16_t ent, int16_t scr,
                                  uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_play_sound(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_copy_sprite_pos(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_copy_leader_pos(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_store_offset_position(int16_t ent, int16_t scr,
                                           uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_get_event_flag(int16_t ent, int16_t scr,
                                        uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_set_event_flag(int16_t ent, int16_t scr,
                                        uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_queue_interaction(int16_t ent, int16_t scr,
                                       uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_store_npc_pos(int16_t ent, int16_t scr,
                                       uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_store_sprite_pos(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_face_toward_npc(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_face_toward_sprite(int16_t ent, int16_t scr,
                                            uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_set_bounding_box(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_set_pos_from_screen(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_set_npc_id(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_animate_pal_fade(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_setup_spotlight(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_apply_color_math(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_print_cast_name(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_print_cast_var0(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_print_cast_party(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_move_toward_no_sprite_cb(int16_t ent, int16_t scr,
                                     uint16_t pc, uint16_t *out_pc);
int16_t cr_move_toward_reversed_cb(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc);
int16_t cr_update_dir_velocity_cb(int16_t ent, int16_t scr,
                                   uint16_t pc, uint16_t *out_pc);
int16_t cr_update_dir_velocity_reversed_cb(int16_t ent, int16_t scr,
                                            uint16_t pc, uint16_t *out_pc);
int16_t cr_move_toward_target_cb(int16_t ent, int16_t scr,
                                  uint16_t pc, uint16_t *out_pc);
int16_t cr_update_mini_ghost_position(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc);

/* ---- Palette functions (callroutine_palette.c) ---- */
int16_t get_colour_fade_slope(int16_t current, int16_t target, int16_t frames);
void copy_fade_buffer_to_palettes(void);
void prepare_palette_fade_slopes(int16_t frames, uint16_t mask);
void update_map_palette_animation(void);
uint16_t fade_palette_color(uint16_t color, uint8_t style);
void load_palette_to_fade_buffer(uint8_t fade_style);
void finalize_palette_fade(void);
void setup_color_math_window(uint8_t cgadsub_val, uint8_t intensity);
int16_t clamp_color_channel(int16_t value);
void adjust_palette_brightness(int palette_index, int16_t brightness);
void apply_palette_brightness_all(int16_t brightness);
void setup_entity_color_math(void);

int16_t cr_cycle_entity_palette(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_set_state_paused(int16_t ent, int16_t scr,
                             uint16_t pc, uint16_t *out_pc);
int16_t cr_update_palette_anim(int16_t ent, int16_t scr,
                                uint16_t pc, uint16_t *out_pc);
int16_t cr_set_state_running(int16_t ent, int16_t scr,
                              uint16_t pc, uint16_t *out_pc);
int16_t cr_finalize_palette_fade(int16_t ent, int16_t scr,
                                  uint16_t pc, uint16_t *out_pc);
int16_t cr_load_file_select_palettes(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc);
int16_t cr_fill_palettes_white(int16_t ent, int16_t scr,
                                uint16_t pc, uint16_t *out_pc);
int16_t cr_fill_palettes_black(int16_t ent, int16_t scr,
                                uint16_t pc, uint16_t *out_pc);
int16_t cr_load_gas_station_palette(int16_t ent, int16_t scr,
                                     uint16_t pc, uint16_t *out_pc);
int16_t cr_load_gas_station_flash_palette(int16_t ent, int16_t scr,
                                           uint16_t pc, uint16_t *out_pc);

/* ---- Screen/display functions (callroutine_screen.c) ---- */
int16_t cr_decompress_title_data(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_load_title_palette(int16_t ent, int16_t scr,
                              uint16_t pc, uint16_t *out_pc);
int16_t cr_show_entity_sprite(int16_t ent, int16_t scr,
                              uint16_t pc, uint16_t *out_pc);
void render_entity_hdma_window(int16_t entity_offset,
                               uint8_t *wh_left_table,
                               uint8_t *wh_right_table);
void dispatch_tick_callback(uint32_t rom_addr, int16_t entity_offset);
int16_t cr_disable_obj_hdma(int16_t ent, int16_t scr,
                            uint16_t pc, uint16_t *out_pc);
int16_t cr_init_window_registers(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_decomp_itoi_production(int16_t ent, int16_t scr,
                                  uint16_t pc, uint16_t *out_pc);
int16_t cr_decomp_nintendo_presentation(int16_t ent, int16_t scr,
                                         uint16_t pc, uint16_t *out_pc);
int16_t cr_play_flyover_script(int16_t ent, int16_t scr,
                               uint16_t pc, uint16_t *out_pc);
int16_t cr_choose_random(int16_t ent, int16_t scr,
                         uint16_t pc, uint16_t *out_pc);
int16_t cr_test_player_in_area(int16_t ent, int16_t scr,
                               uint16_t pc, uint16_t *out_pc);
int16_t cr_make_party_look_at_entity(int16_t ent, int16_t scr,
                                     uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_calc_travel_frames(int16_t ent, int16_t scr,
                                           uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_cmd_return_2(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_is_entity_near_leader(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);

/* ---- Sprite functions (callroutine_sprite.c) ---- */
int16_t cr_set_entity_direction_and_frame(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_deallocate_entity_sprite(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc);
int16_t cr_is_x_less_than_entity(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_is_y_less_than_entity(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_clear_loop_counter(int16_t ent, int16_t scr,
                              uint16_t pc, uint16_t *out_pc);
int16_t cr_movement_loop(int16_t ent, int16_t scr,
                         uint16_t pc, uint16_t *out_pc);
int16_t cr_render_entity_sprite_me2(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc);
int16_t cr_render_entity_sprite_me1(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc);
int16_t cr_reset_entity_animation(int16_t ent, int16_t scr,
                                  uint16_t pc, uint16_t *out_pc);
int16_t cr_initialize_party_member_entity(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_update_follower_visuals(int16_t ent, int16_t scr,
                                   uint16_t pc, uint16_t *out_pc);
int16_t cr_sram_check_routine_checksum(int16_t ent, int16_t scr,
                                       uint16_t pc, uint16_t *out_pc);
int16_t cr_inflict_sunstroke_check(int16_t ent, int16_t scr,
                                   uint16_t pc, uint16_t *out_pc);
int16_t cr_check_entity_enemy_collision(int16_t ent, int16_t scr,
                                        uint16_t pc, uint16_t *out_pc);
int16_t cr_get_overworld_status(int16_t ent, int16_t scr,
                                uint16_t pc, uint16_t *out_pc);
int16_t cr_check_prospective_entity_collision(int16_t ent, int16_t scr,
                                              uint16_t pc, uint16_t *out_pc);
int16_t cr_render_entity_sprite_me3(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc);

/* ---- Action script functions (callroutine_action.c) ---- */
int16_t cr_actionscript_prepare_entity(int16_t ent, int16_t scr,
                                       uint16_t pc, uint16_t *out_pc);
int16_t cr_actionscript_prepare_at_leader(int16_t ent, int16_t scr,
                                          uint16_t pc, uint16_t *out_pc);
int16_t cr_actionscript_prepare_at_self(int16_t ent, int16_t scr,
                                        uint16_t pc, uint16_t *out_pc);
int16_t cr_actionscript_fade_out(int16_t ent, int16_t scr,
                                 uint16_t pc, uint16_t *out_pc);
int16_t cr_actionscript_fade_in(int16_t ent, int16_t scr,
                                uint16_t pc, uint16_t *out_pc);
int16_t cr_actionscript_get_party_member_pos(int16_t ent, int16_t scr,
                                             uint16_t pc, uint16_t *out_pc);
int16_t cr_prepare_entity_at_teleport_dest(int16_t ent, int16_t scr,
                                           uint16_t pc, uint16_t *out_pc);
int16_t cr_actionscript_fade_out_with_mosaic(int16_t ent, int16_t scr,
                                             uint16_t pc, uint16_t *out_pc);
int16_t cr_actionscript_prepare_at_teleport(int16_t ent, int16_t scr,
                                            uint16_t pc, uint16_t *out_pc);

#endif /* CALLROUTINE_INTERNAL_H */
