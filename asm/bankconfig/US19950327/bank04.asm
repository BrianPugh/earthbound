.SEGMENT "BANK04"
.INCLUDE "eventmacros.asm"
.INCLUDE "common.asm"
.INCLUDE "config.asm"
.INCLUDE "structs.asm"
.INCLUDE "symbols/bank00.inc.asm"
.INCLUDE "symbols/bank01.inc.asm"
.INCLUDE "symbols/bank02.inc.asm"
.INCLUDE "symbols/bank03.inc.asm"
.INCLUDE "symbols/bank04.inc.asm"
.INCLUDE "symbols/bank2f.inc.asm"
.INCLUDE "symbols/audiopacks.inc.asm"
.INCLUDE "symbols/doors.inc.asm"
.INCLUDE "symbols/globals.inc.asm"
.INCLUDE "symbols/map.inc.asm"
.INCLUDE "symbols/misc.inc.asm"
.INCLUDE "symbols/text.inc.asm"
LOCALEINCLUDE "flyovers.symbols.asm"

.INCLUDE "misc/write_inidisp.asm"

.INCLUDE "inventory/store/restore_inidisp.asm"

.INCLUDE "overworld/entity/reset_entity_animation.asm"

.INCLUDE "overworld/entity/set_entity_sleep_frames.asm"

.INCLUDE "text/vwf/upload_vwf_tile_to_vram.asm"

.INCLUDE "system/dma/alloc_bg2_tilemap_entry.asm"

.INCLUDE "data/events/script_pointers.asm"

.INCLUDE "system/init_error_screen.asm"

.INCLUDE "system/display_error_screen.asm"

.INCLUDE "data/map/footstep_sound_table.asm"

.INCLUDE "data/unknown/C40BE8.asm"

.INCLUDE "data/text/floating_sprite_table.asm"

.INCLUDE "data/events/scripts/785.asm"

.INCLUDE "data/events/entity_overlays.asm"

.INCLUDE "data/events/C40F18.asm"

.INCLUDE "data/events/C40F4A.asm"

.INCLUDE "data/events/C40F59.asm"

.INCLUDE "data/events/scripts/502.asm"

.INCLUDE "data/events/scripts/503.asm"

.INCLUDE "data/events/scripts/504.asm"

.INCLUDE "data/events/scripts/505.asm"

.INCLUDE "data/events/scripts/506.asm"

.INCLUDE "data/events/scripts/507.asm"

.INCLUDE "data/events/scripts/508.asm"

.INCLUDE "data/events/scripts/509.asm"

.INCLUDE "data/events/scripts/510.asm"

.INCLUDE "data/events/scripts/511.asm"

.INCLUDE "data/events/scripts/512.asm"

.INCLUDE "data/events/scripts/513.asm"

.INCLUDE "data/events/scripts/514.asm"

.INCLUDE "data/events/scripts/515.asm"

.INCLUDE "data/events/scripts/516.asm"

.INCLUDE "data/events/scripts/517.asm"

.INCLUDE "data/events/scripts/518.asm"

.INCLUDE "data/events/scripts/519.asm"

.INCLUDE "data/events/scripts/520.asm"

.INCLUDE "data/events/scripts/521.asm"

.INCLUDE "data/events/scripts/522.asm"

.INCLUDE "data/events/scripts/523.asm"

.INCLUDE "data/events/scripts/524.asm"

.INCLUDE "data/events/scripts/525.asm"

.INCLUDE "data/events/scripts/526.asm"

.INCLUDE "data/events/scripts/527.asm"

.INCLUDE "data/events/scripts/528.asm"

.INCLUDE "data/events/scripts/529.asm"

.INCLUDE "data/events/scripts/530.asm"

.INCLUDE "data/events/scripts/534.asm"

.INCLUDE "data/events/C41036.asm"

.INCLUDE "data/events/C4116C.asm"

.INCLUDE "data/events/C4119D.asm"

.INCLUDE "data/events/C411EB.asm"

.INCLUDE "data/events/C4121F.asm"

.INCLUDE "data/events/C41253.asm"

.INCLUDE "data/events/C41382.asm"

.INCLUDE "data/events/C413D6.asm"

.INCLUDE "data/events/C41402.asm"

.INCLUDE "data/events/C4144C.asm"

.INCLUDE "data/events/C4152A.asm"

.INCLUDE "data/events/C4154E.asm"

.INCLUDE "data/events/C4158A.asm"

.INCLUDE "data/events/C415BA.asm"

.INCLUDE "data/events/C415E7.asm"

.INCLUDE "data/events/C4160A.asm"

.INCLUDE "data/events/C4163F.asm"

.INCLUDE "data/events/C416AC.asm"

.INCLUDE "data/events/C4170E.asm"

.INCLUDE "data/events/C41822.asm"

.INCLUDE "data/events/C41900.asm"

.INCLUDE "data/events/C41938.asm"

.INCLUDE "data/events/C41974.asm"

.INCLUDE "data/events/C4198D.asm"

.INCLUDE "data/events/C419B2.asm"

.INCLUDE "data/events/C419BF.asm"

.INCLUDE "data/events/C41A2A.asm"

.INCLUDE "data/events/C41A7D.asm"

.INCLUDE "system/decomp.asm"

.INCLUDE "text/vwf/render_vwf_glyph.asm"

.INCLUDE "data/unknown/C41EB9.asm"

.INCLUDE "data/unknown/C41EC9.asm"

.INCLUDE "data/unknown/C41ED9.asm"

.INCLUDE "text/vwf/update_vwf_dirty_min.asm"

.INCLUDE "text/vwf/update_vwf_dirty_max.asm"

.INCLUDE "misc/calculate_direction_from_positions.asm"

.INCLUDE "data/unknown/C41FC5.asm"

.INCLUDE "data/unknown/C41FDF.asm"

.INCLUDE "misc/calculate_velocity_components.asm"

.INCLUDE "data/unknown/C4205D.asm"

.INCLUDE "data/unknown/C420BD.asm"

.INCLUDE "misc/multiply_16bit_hardware.asm"

.INCLUDE "data/events/scripts/787.asm"

.INCLUDE "data/events/scripts/860.asm"

.INCLUDE "data/events/C4220E.asm"

.INCLUDE "data/events/scripts/789-proto.asm"

.INCLUDE "data/events/scripts/788.asm"

.INCLUDE "data/events/scripts/790.asm"

.INCLUDE "data/events/scripts/791.asm"

.INCLUDE "data/events/scripts/792.asm"

.INCLUDE "data/events/scripts/793.asm"

.INCLUDE "data/events/scripts/794.asm"

.INCLUDE "data/events/scripts/795.asm"

.INCLUDE "data/events/scripts/796.asm"

.INCLUDE "data/events/scripts/797.asm"

.INCLUDE "data/events/scripts/798.asm"

.INCLUDE "text/window/init_window_registers.asm"

.INCLUDE "text/window/setup_fullscreen_window_clipping.asm"

.INCLUDE "system/palette/apply_color_math_fixed_color.asm"

.INCLUDE "system/hdma/enable_obj_hdma.asm"

.INCLUDE "system/hdma/disable_obj_hdma.asm"

.INCLUDE "system/palette/setup_color_math_window.asm"

.INCLUDE "text/window/setup_dark_window_effect.asm"

.INCLUDE "text/window/setup_white_window_effect.asm"

.INCLUDE "system/hdma/setup_hdma_channel4_wh0.asm"

.INCLUDE "system/palette/set_colour_math_add.asm"

.INCLUDE "system/palette/set_colour_math_sub.asm"

.INCLUDE "system/hdma/disable_hdma_channel4_alt.asm"

.INCLUDE "text/window/setup_dual_dark_window_effect.asm"

.INCLUDE "system/hdma/setup_hdma_channel4_wh0_alt.asm"

.INCLUDE "system/hdma/disable_hdma_channel4.asm"

.INCLUDE "system/hdma/setup_hdma_channel5_wh2.asm"

.INCLUDE "system/hdma/disable_hdma_channel5.asm"

.INCLUDE "misc/init_transition_scroll_velocity.asm"

.INCLUDE "misc/update_transition_scroll.asm"

.INCLUDE "overworld/entity/update_entity_screen_positions.asm"

.INCLUDE "system/palette/update_map_palette_animation.asm"

.INCLUDE "data/events/scripts/859.asm"

.INCLUDE "data/events/C427E0.asm"

.INCLUDE "data/events/C42802.asm"

.INCLUDE "data/events/C42815.asm"

.INCLUDE "data/events/C42828.asm"

.INCLUDE "overworld/entity/copy_8dir_entity_sprite.asm"

.INCLUDE "overworld/entity/copy_4dir_entity_sprite.asm"

.INCLUDE "overworld/entity/copy_sprite_tile_data.asm"

.INCLUDE "overworld/entity/blend_sprite_tile_data.asm"

.INCLUDE "data/unknown/C42955.asm"

.INCLUDE "overworld/entity/merge_sprite_tile_pair.asm"

.INCLUDE "overworld/entity/upload_entity_sprite_to_vram.asm"

.INCLUDE "system/hdma/enable_letterbox_hdma.asm"

.INCLUDE "data/unknown/C42A1F.asm"

.INCLUDE "data/unknown/C42A41.asm"

.INCLUDE "data/unknown/C42A63.asm"

.INCLUDE "data/unknown/C42A85.asm"

.INCLUDE "data/unknown/C42AA7.asm"

.INCLUDE "data/unknown/C42AC9.asm"

.INCLUDE "data/unknown/C42AEB.asm"

.INCLUDE "data/unknown/C42B0D.asm"

.INCLUDE "data/unknown/C42B51.asm"

.INCLUDE "data/unknown/C42B5D.asm"

.INCLUDE "data/unknown/C42B73.asm"

.INCLUDE "data/unknown/C42B89.asm"

.INCLUDE "data/unknown/C42BA9.asm"

.INCLUDE "data/unknown/C42BBF.asm"

.INCLUDE "data/unknown/C42BE9.asm"

.INCLUDE "data/unknown/C42BFF.asm"

.INCLUDE "data/unknown/C42C29.asm"

.INCLUDE "data/unknown/C42C67.asm"

.INCLUDE "data/unknown/C42CA5.asm"

.INCLUDE "data/unknown/C42CC5.asm"

.INCLUDE "data/unknown/C42D03.asm"

.INCLUDE "data/unknown/C42D5F.asm"

.INCLUDE "data/unknown/C42DD9.asm"

.INCLUDE "data/unknown/C42E7B.asm"

.INCLUDE "overworld/set_party_tick_callbacks.asm"

.INCLUDE "data/map/tile_table_chunks_table.asm"

.INCLUDE "data/unknown/C42F8C.asm"

.INCLUDE "data/unknown/C4303C.asm"

.INCLUDE "overworld/velocity_store.asm"

.INCLUDE "misc/clear_all_status_effects.asm"

.INCLUDE "misc/init_chosen_four_pointers.asm"

.INCLUDE "text/set_overworld_status_suppression.asm"

.INCLUDE "misc/check_door_near_leader.asm"

.INCLUDE "misc/save_photo_state.asm"

.INCLUDE "data/item_use_menu_strings.asm"

.INCLUDE "battle/wait_and_update_battle_effects_far.asm"

.INCLUDE "text/clear_battle_menu_character_indicator-proto.asm"

.INCLUDE "battle/select_battle_menu_character.asm"

.INCLUDE "battle/clear_battler_flashing.asm"

.INCLUDE "battle/set_battler_flashing.asm"

.INCLUDE "text/window/fill_window_row.asm"

.INCLUDE "text/window/free_window_text_row.asm"

.INCLUDE "text/window/scroll_window_up.asm"

.INCLUDE "text/window/set_window_text_position.asm"

.INCLUDE "text/set_focus_text_cursor.asm"

.INCLUDE "text/print_newline.asm"

.INCLUDE "data/text/name_entry_grid_character_offset_table.asm"

.INCLUDE "data/unknown/C20958.asm"

.INCLUDE "data/text/locked_tiles.asm"

.INCLUDE "text/window/apply_window_text_attributes.asm"

.INCLUDE "misc/clear_sprite_attribute_bits.asm"

.INCLUDE "misc/set_sprite_attribute_bits.asm"

.INCLUDE "text/set_file_select_text_highlight.asm"

.INCLUDE "text/vwf/advance_vwf_tile.asm"

.INCLUDE "text/vwf/setup_menu_option_vwf.asm"

.INCLUDE "text/set_text_cursor_with_pixel_offset.asm"

.INCLUDE "text/set_text_pixel_position.asm"

.INCLUDE "text/advance_text_cursor_pixels.asm"

.INCLUDE "text/clear_menu_option_text.asm"

.INCLUDE "text/get_string_pixel_width.asm"

.INCLUDE "text/window/center_text_in_window.asm"

.INCLUDE "system/dma/init_used_bg2_tile_map.asm"

.INCLUDE "text/print_char_with_sound.asm"

.INCLUDE "text/get_character_at_cursor_position.asm"

.INCLUDE "text/vwf/initialize_existing_name_vwf-proto.asm"

.INCLUDE "text/vwf/initialize_keyboard_input_vwf.asm"

.INCLUDE "misc/register_keyboard_input_character.asm"

.INCLUDE "misc/render_keyboard_input_character.asm"

.INCLUDE "text/vwf/render_tiny_font_string.asm"

.INCLUDE "text/check_text_word_wrap.asm"

.INCLUDE "text/print_string_with_wordwrap.asm"

.INCLUDE "text/print_text_with_word_splitting-proto.asm"

.INCLUDE "system/dma/upload_text_tiles_to_vram.asm"

.INCLUDE "data/unknown/C44AD7.asm"

.INCLUDE "text/free_tile.asm"

.INCLUDE "text/vwf/blit_vwf_glyph.asm"

.INCLUDE "data/powers_of_two_16.asm"

.INCLUDE "text/window/write_char_to_window.asm"

.INCLUDE "text/vwf/flush_vwf_tiles_to_vram.asm"

.INCLUDE "text/clear_text_render_state.asm"

.INCLUDE "text/free_tile_safe.asm"

.INCLUDE "text/vwf/render_vwf_character.asm"

.INCLUDE "text/get_text_pixel_width.asm"

.INCLUDE "text/window/print_money_in_window-proto.asm"

.INCLUDE "text/menu/layout_menu_options.asm"

.INCLUDE "data/text/battle_to_text.asm"

.INCLUDE "data/text/battle_front_row_text.asm"

.INCLUDE "data/text/battle_back_row_text.asm"

.INCLUDE "data/text/CC_1C_01_data.asm"

.INCLUDE "data/powers_of_two_8.asm"

.INCLUDE "misc/find_item_in_inventory.asm"

.INCLUDE "misc/find_item_in_inventory2.asm"

.INCLUDE "misc/find_inventory_space.asm"

.INCLUDE "misc/find_inventory_space2.asm"

.INCLUDE "misc/change_equipped_weapon.asm"

.INCLUDE "misc/change_equipped_body.asm"

.INCLUDE "misc/change_equipped_arms.asm"

.INCLUDE "misc/change_equipped_other.asm"

.INCLUDE "data/item_usable_flags.asm"

.INCLUDE "misc/check_status_group.asm"

.INCLUDE "misc/inflict_status_nonbattle.asm"

.INCLUDE "data/battle/misc_target_text.asm"

.INCLUDE "data/text/phone_call_text.asm"

.INCLUDE "misc/get_required_exp.asm"

.INCLUDE "data/text/status_equip_window_text.asm"

.INCLUDE "data/homesickness_probabilities.asm"

.INCLUDE "text/vwf/render_mrsaturn_vwf_character.asm"

.INCLUDE "text/vwf/render_vwf_text_tile.asm"

.INCLUDE "text/vwf/reset_vwf_text_state.asm"

.INCLUDE "misc/check_if_psi_known.asm"

.INCLUDE "system/math/rand_mod.asm"

.INCLUDE "data/map/direction_matrix.asm"

.INCLUDE "overworld/get_direction_to.asm"

.INCLUDE "overworld/entity/find_entity_by_sprite_id.asm"

.INCLUDE "overworld/entity/find_entity_by_npc_id.asm"

.INCLUDE "overworld/entity/find_entity_for_character.asm"

.INCLUDE "overworld/entity/deactivate_npc_entity.asm"

.INCLUDE "overworld/entity/deactivate_sprite_entity.asm"

.INCLUDE "overworld/entity/set_tpt_entity_script.asm"

.INCLUDE "overworld/entity/set_sprite_entity_script.asm"

.INCLUDE "overworld/entity/find_entity_by_type.asm"

.INCLUDE "misc/get_direction_between_entities.asm"

.INCLUDE "overworld/npc/get_direction_between_npc_entities.asm"

.INCLUDE "overworld/entity/get_direction_between_sprite_entities.asm"

.INCLUDE "misc/get_direction_between_character_entities.asm"

.INCLUDE "overworld/npc/set_npc_direction.asm"

.INCLUDE "overworld/entity/set_entity_direction_by_sprite_id.asm"

.INCLUDE "overworld/entity/set_character_entity_direction.asm"

.INCLUDE "misc/set_all_party_directions.asm"

.INCLUDE "overworld/entity/hide_entity_sprites.asm"

.INCLUDE "overworld/entity/show_entity_sprites.asm"

.INCLUDE "overworld/create_prepared_entity_npc.asm"

.INCLUDE "overworld/create_prepared_entity_sprite.asm"

.INCLUDE "overworld/entity/create_entity_at_current_position.asm"

.INCLUDE "overworld/npc/disable_npc_movement.asm"

.INCLUDE "overworld/entity/disable_entity_by_sprite_id.asm"

.INCLUDE "misc/disable_character_movement.asm"

.INCLUDE "overworld/npc/enable_npc_movement.asm"

.INCLUDE "overworld/entity/enable_sprite_movement.asm"

.INCLUDE "misc/enable_character_movement.asm"

.INCLUDE "overworld/npc/set_camera_focus_by_npc_id.asm"

.INCLUDE "overworld/entity/set_camera_focus_by_sprite_id.asm"

.INCLUDE "misc/reset_camera_mode.asm"

.INCLUDE "misc/encounter_travelling_photographer.asm"

.INCLUDE "text/display_text_from_params.asm"

.INCLUDE "misc/disable_party_movement_and_hide.asm"

.INCLUDE "misc/enable_party_movement_and_show.asm"

.INCLUDE "overworld/npc/get_random_npc_delay.asm"

.INCLUDE "misc/get_random_screen_delay.asm"

.INCLUDE "misc/enable_tessie_leaves_entities.asm"

.INCLUDE "text/queue_npc_text_interaction.asm"

.INCLUDE "text/freeze_and_queue_text_interaction.asm"

.INCLUDE "misc/get_pad_press.asm"

.INCLUDE "overworld/entity/is_x_less_than_entity.asm"

.INCLUDE "overworld/entity/is_y_less_than_entity.asm"

.INCLUDE "misc/is_below_leader_y.asm"

.INCLUDE "overworld/npc/get_npc_default_direction.asm"

.INCLUDE "overworld/entity/set_entity_direction.asm"

.INCLUDE "overworld/entity/face_entity_toward_npc.asm"

.INCLUDE "overworld/entity/face_entity_toward_sprite.asm"

.INCLUDE "data/unknown/C46A5E.asm"

.INCLUDE "misc/get_leader_direction_offset.asm"

.INCLUDE "data/unknown/C46A7A.asm"

.INCLUDE "data/unknown/C46A8A.asm"

.INCLUDE "misc/get_cardinal_direction.asm"

.INCLUDE "overworld/entity/map_direction_to_sprite_group.asm"

.INCLUDE "misc/get_direction_to_script_target.asm"

.INCLUDE "misc/calculate_direction_to_target.asm"

.INCLUDE "overworld/entity/quantize_entity_direction.asm"

.INCLUDE "misc/scale_direction_distance.asm"

.INCLUDE "misc/reverse_direction_8.asm"

.INCLUDE "data/unknown/C46B41.asm"

.INCLUDE "misc/quantize_direction.asm"

.INCLUDE "overworld/entity/store_leader_pos_to_entity_vars.asm"

.INCLUDE "inventory/store/store_prepared_position_to_vars.asm"

.INCLUDE "overworld/npc/store_npc_position_to_script_vars.asm"

.INCLUDE "overworld/entity/save_sprite_position_to_vars.asm"

.INCLUDE "overworld/get_position_of_party_member.asm"

.INCLUDE "overworld/entity/store_entity_position_to_vars.asm"

.INCLUDE "overworld/entity/store_entity_offset_position.asm"

.INCLUDE "overworld/entity/restore_entity_position_from_vars.asm"

.INCLUDE "overworld/entity/copy_leader_position_to_entity.asm"

.INCLUDE "overworld/entity/copy_entity_position_by_sprite_id.asm"

.INCLUDE "overworld/entity/entity_screen_to_world.asm"

.INCLUDE "overworld/entity/set_entity_random_screen_x.asm"

.INCLUDE "misc/set_photographer_position.asm"

.INCLUDE "overworld/prepare_new_entity_at_existing_entity_location.asm"

.INCLUDE "overworld/prepare_new_entity_at_teleport_destination.asm"

.INCLUDE "overworld/prepare_new_entity.asm"

.INCLUDE "misc/set_actionscript_state_running.asm"

.INCLUDE "misc/queue_interaction_from_params.asm"

.INCLUDE "overworld/actionscript/test_player_in_area.asm"

.INCLUDE "overworld/entity/is_entity_in_range_of_leader.asm"

.INCLUDE "overworld/entity/navigate_entity_to_target.asm"

.INCLUDE "overworld/entity/set_entity_velocity_from_direction.asm"

.INCLUDE "overworld/entity/move_entity_toward_target.asm"

.INCLUDE "overworld/entity/set_entity_bounding_box.asm"

.INCLUDE "overworld/entity/get_entity_boundary_side.asm"

.INCLUDE "overworld/entity/update_entity_direction_and_velocity.asm"

.INCLUDE "overworld/entity/halve_entity_delta_y.asm"

.INCLUDE "misc/get_party_count.asm"

.INCLUDE "overworld/map/load_current_map_block_events.asm"

.INCLUDE "overworld/map/load_map_row_at_scroll_pos.asm"

.INCLUDE "overworld/map/load_initial_map_data_far.asm"

.INCLUDE "system/load_background_animation.asm"

.INCLUDE "system/palette/clamp_color_channel.asm"

.INCLUDE "system/palette/adjust_palette_brightness.asm"

.INCLUDE "system/palette/apply_palette_brightness_all.asm"

.INCLUDE "overworld/entity/apply_entity_palette_brightness.asm"

.INCLUDE "overworld/entity/setup_entity_color_math.asm"

.INCLUDE "data/unknown/C474F6.asm"

.INCLUDE "overworld/entity/render_entity_hdma_window.asm"

.INCLUDE "overworld/entity/setup_entity_hdma_window_ch4.asm"

.INCLUDE "overworld/entity/setup_entity_hdma_window_ch5.asm"

.INCLUDE "system/hdma/setup_spotlight_hdma_window.asm"

.INCLUDE "misc/clamp_value_to_range.asm"

.INCLUDE "system/hdma/write_window_hdma_entry.asm"

.INCLUDE "system/hdma/setup_rect_window_hdma.asm"

.INCLUDE "overworld/entity/apply_entity_rect_window.asm"

.INCLUDE "text/window/center_camera_with_rect_window.asm"

.INCLUDE "overworld/entity/reflect_entity_y_at_target.asm"

.INCLUDE "misc/load_animation_sequence_frame.asm"

.INCLUDE "misc/display_animation_sequence_frame.asm"

.INCLUDE "system/load_window_gfx.asm"

.INCLUDE "system/palette/load_character_window_palette.asm"

.INCLUDE "text/undraw_flyover_text.asm"

.INCLUDE "data/text/lumine_hall.asm"

.INCLUDE "system/dma/decode_planar_tilemap.asm"

.INCLUDE "text/vwf/render_font_glyph.asm"

.INCLUDE "intro/render_character_name_display.asm"

.INCLUDE "misc/load_character_portrait_screen.asm"

.INCLUDE "system/dma/advance_tilemap_animation_frame.asm"

.INCLUDE "text/menu/setup_psi_teleport_departure.asm"

.INCLUDE "overworld/actionscript/make_party_look_at_active_entity.asm"

.INCLUDE "overworld/actionscript/animated_background_callback.asm"

.INCLUDE "overworld/actionscript/simple_screen_position_callback.asm"

.INCLUDE "overworld/actionscript/simple_screen_position_callback_offset.asm"

.INCLUDE "overworld/actionscript/centre_screen_on_entity_callback.asm"

.INCLUDE "overworld/actionscript/centre_screen_on_entity_callback_offset.asm"

.INCLUDE "data/unknown/C48C59.asm"

.INCLUDE "misc/clear_auto_movement_buffer.asm"

.INCLUDE "misc/record_auto_movement_step.asm"

.INCLUDE "data/unknown/C48D38.asm"

.INCLUDE "misc/calculate_movement_path_steps.asm"

.INCLUDE "misc/record_repeated_auto_movement.asm"

.INCLUDE "misc/queue_auto_movement_step.asm"

.INCLUDE "overworld/is_valid_item_transformation.asm"

.INCLUDE "overworld/initialize_item_transformation.asm"

.INCLUDE "inventory/cancel_item_transformation.asm"

.INCLUDE "overworld/process_item_transformations.asm"

.INCLUDE "overworld/get_distance_to_magic_truffle.asm"

.INCLUDE "system/get_colour_fade_slope.asm"

.INCLUDE "overworld/initialize_map_palette_fade.asm"

.INCLUDE "system/palette/update_map_palette_fade.asm"

.INCLUDE "system/palette/load_map_palette.asm"

.INCLUDE "system/palette/fade_palette_color.asm"

.INCLUDE "system/palette/load_palette_to_fade_buffer.asm"

.INCLUDE "system/palette/prepare_palette_fade_slopes.asm"

.INCLUDE "system/palette/prepare_palette_fade.asm"

.INCLUDE "system/palette/prepare_map_palette_fade.asm"

.INCLUDE "system/palette/copy_fade_buffer_to_palettes.asm"

.INCLUDE "system/palette/finalize_palette_fade.asm"

.INCLUDE "system/palette/restore_map_palette.asm"

.INCLUDE "system/palette/animate_palette_fade_to_map.asm"

.INCLUDE "text/load_text_layer_tilemap.asm"

.INCLUDE "text/window/init_oval_window_far.asm"

.INCLUDE "text/vwf/invert_vwf_buffer.asm"

.INCLUDE "text/vwf/blit_font_to_vwf_buffer.asm"

.INCLUDE "text/vwf/render_large_font_character.asm"

.INCLUDE "battle/wait_and_update_battle_effects.asm"

.INCLUDE "text/init_flyover_text_screen.asm"

.INCLUDE "text/vwf/upload_vwf_buffer_to_vram.asm"

.INCLUDE "text/scroll_flyover_text.asm"

.INCLUDE "misc/advance_flyover_pixel_offset.asm"

.INCLUDE "intro/render_party_member_name-proto.asm"

.INCLUDE "text/render_flyover_text_character.asm"

.INCLUDE "text/advance_text_line_position.asm"

.INCLUDE "text/coffee_tea_scene.asm"

.INCLUDE "data/text/flyover_text_pointers.asm"

.INCLUDE "misc/play_flyover_script.asm"

.INCLUDE "data/text/battle_menu_text.asm"

.INCLUDE "data/battle/dead_targettable_actions.asm"

.INCLUDE "battle/autohealing.asm"

.INCLUDE "battle/autolifeup.asm"

.INCLUDE "data/battle/battle_window_sizes.asm"

.INCLUDE "battle/check_if_valid_target.asm"

.INCLUDE "battle/set_battler_target.asm"

.INCLUDE "data/powers_of_two_32.asm"

.INCLUDE "data/battle/prayer_list.asm"

.INCLUDE "data/battle/prayer_text_pointers.asm"

.INCLUDE "data/battle/giygas_death_static_transition_delays.asm"

.INCLUDE "data/battle/final_giygas_prayer_noise_table.asm"

.INCLUDE "misc/setup_gas_station_background.asm"

.INCLUDE "data/unknown/C4A591.asm"

.INCLUDE "data/unknown/C4A5CE.asm"

.INCLUDE "data/unknown/C4A5FA.asm"

.INCLUDE "data/unknown/C4A626.asm"

.INCLUDE "data/unknown/C4A652.asm"

.INCLUDE "misc/init_swirl_effect.asm"

.INCLUDE "misc/update_swirl_effect.asm"

.INCLUDE "data/unknown/C4AC57.asm"

.INCLUDE "data/sound_stone_unknown1.asm"

.INCLUDE "data/sound_stone_unknown2.asm"

.INCLUDE "data/sound_stone_unknown3.asm"

.INCLUDE "data/sound_stone_unknown4.asm"

.INCLUDE "data/sound_stone_unknown5.asm"

.INCLUDE "data/sound_stone_unknown6.asm"

.INCLUDE "data/music/sound_stone_music.asm"

.INCLUDE "data/sound_stone_unknown7.asm"

.INCLUDE "data/sound_stone_unknown8.asm"

.INCLUDE "data/sound_stone_melody_flags.asm"

.INCLUDE "overworld/use_sound_stone.asm"

.INCLUDE "overworld/entity/load_sprite_tiles_to_vram.asm"

.INCLUDE "overworld/load_overlay_sprites.asm"

.INCLUDE "overworld/entity/update_floating_sprite_offset.asm"

.INCLUDE "text/spawn_floating_sprite.asm"

.INCLUDE "misc/remove_associated_entities.asm"

.INCLUDE "overworld/entity/spawn_floating_sprite_for_character.asm"

.INCLUDE "misc/remove_character_entities.asm"

.INCLUDE "overworld/entity/spawn_floating_sprite_for_npc.asm"

.INCLUDE "overworld/npc/remove_npc_entities.asm"

.INCLUDE "overworld/entity/spawn_floating_sprite_for_sprite.asm"

.INCLUDE "overworld/entity/remove_sprite_entities.asm"

.INCLUDE "overworld/entity/spawn_leader_floating_sprite.asm"

.INCLUDE "overworld/entity/remove_leader_floating_sprites.asm"

.INCLUDE "misc/path_heap_alloc.asm"

.INCLUDE "misc/get_path_heap_used_size.asm"

.INCLUDE "overworld/pathfinding/initialize_pathfinder.asm"

.INCLUDE "misc/init_path_matrix_borders.asm"

.INCLUDE "misc/sort_path_nodes.asm"

.INCLUDE "misc/populate_path_matrix.asm"

.INCLUDE "overworld/pathfinding/path_bfs_search.asm"

.INCLUDE "misc/trace_path_route.asm"

.INCLUDE "misc/compress_path_waypoints.asm"

.INCLUDE "data/text/file_select_text.asm"

.INCLUDE "misc/initialize_game_over_screen.asm"

.INCLUDE "system/palette/load_map_palette_animation_frame.asm"

.INCLUDE "system/palette/animate_map_palette_change.asm"

.INCLUDE "text/skippable_pause.asm"

.INCLUDE "system/palette/fade_palette_to_white.asm"

.INCLUDE "system/palette/animate_palette_fade_with_rendering.asm"

.INCLUDE "misc/play_comeback_sequence.asm"

.INCLUDE "overworld/spawn.asm"

.INCLUDE "overworld/entity/init_entity_fade_states_buffer.asm"

.INCLUDE "overworld/entity/double_entity_fade_state.asm"

.INCLUDE "misc/clear_buffer_range.asm"

.INCLUDE "overworld/entity/init_entity_fade_state.asm"

.INCLUDE "overworld/entity/clear_fade_entity_flags.asm"

.INCLUDE "overworld/entity/update_fade_entity_sprites.asm"

.INCLUDE "overworld/entity/hide_fade_entity_frames.asm"

.INCLUDE "misc/null/C4CC2C.asm"

.INCLUDE "overworld/entity/animate_entity_tile_copy.asm"

.INCLUDE "overworld/entity/animate_entity_tile_blend.asm"

.INCLUDE "overworld/entity/clear_spritemap_buffer.asm"

.INCLUDE "overworld/entity/animate_entity_tile_merge.asm"

.INCLUDE "misc/transliterate_consonant_vowel.asm"

.INCLUDE "text/process_name_input_string.asm"

.INCLUDE "overworld/get_town_map_id.asm"

.INCLUDE "system/palette/cycle_map_icon_palette.asm"

.INCLUDE "overworld/map/update_town_map_player_icon.asm"

.INCLUDE "overworld/map/render_town_map_icons.asm"

.INCLUDE "overworld/load_town_map_data.asm"

.INCLUDE "overworld/display_town_map.asm"

.INCLUDE "text/menu/run_town_map_menu.asm"

.INCLUDE "intro/display_animated_naming_sprite.asm"

.INCLUDE "intro/init_naming_screen_events.asm"

.INCLUDE "overworld/entity/create_file_select_party_sprites.asm"

.INCLUDE "misc/run_attract_mode.asm"

.INCLUDE "intro/init_intro.asm"

.INCLUDE "system/dma/set_buffer_tilemap_priority.asm"

.INCLUDE "intro/decomp_itoi_production.asm"

.INCLUDE "intro/decomp_nintendo_presentation.asm"

.INCLUDE "data/unknown/C4DE78.asm"

.INCLUDE "overworld/initialize_your_sanctuary_display.asm"

.INCLUDE "overworld/enable_your_sanctuary_display.asm"

.INCLUDE "overworld/prepare_your_sanctuary_location_palette_data.asm"

.INCLUDE "overworld/prepare_your_sanctuary_location_tile_arrangement_data.asm"

.INCLUDE "overworld/prepare_your_sanctuary_location_tileset_data.asm"

.INCLUDE "overworld/load_your_sanctuary_location_data.asm"

.INCLUDE "overworld/load_your_sanctuary_location.asm"

.INCLUDE "overworld/display_your_sanctuary_location.asm"

.INCLUDE "overworld/test_your_sanctuary_display.asm"

.INCLUDE "system/strcat.asm"

.INCLUDE "ending/load_cast_scene-proto.asm"

.INCLUDE "ending/set_cast_scroll_threshold.asm"

.INCLUDE "ending/check_cast_scroll_threshold.asm"

.INCLUDE "ending/handle_cast_scrolling.asm"

.INCLUDE "ending/render_cast_name_text-proto.asm"

.INCLUDE "data/character_guardian_text.asm"

.INCLUDE "ending/prepare_dynamic_cast_name_text.asm"

.INCLUDE "ending/prepare_cast_name_tilemap.asm"

.INCLUDE "ending/copy_cast_name_tilemap.asm"

.INCLUDE "ending/print_cast_name.asm"

.INCLUDE "ending/print_cast_name_party.asm"

.INCLUDE "ending/print_cast_name_entity_var0.asm"

.INCLUDE "ending/upload_special_cast_palette.asm"

.INCLUDE "ending/create_entity_at_v01_plus_bg3y.asm"

.INCLUDE "ending/is_entity_still_on_cast_screen.asm"

.INCLUDE "ending/play_cast_scene.asm"

.INCLUDE "unused/C4EDA3.asm"

.INCLUDE "unused/C4EE9D.asm"

.INCLUDE "ending/enqueue_credits_dma.asm"

.INCLUDE "ending/process_credits_dma_queue.asm"

.INCLUDE "ending/initialize_credits_scene.asm"

.INCLUDE "ending/try_rendering_photograph.asm"

.INCLUDE "ending/count_photo_flags.asm"

.INCLUDE "ending/slide_credits_photograph.asm"

.INCLUDE "ending/play_credits.asm"

.INCLUDE "overworld/get_delivery_attempts.asm"

.INCLUDE "overworld/reset_delivery_attempts.asm"

.INCLUDE "overworld/check_delivery_attempt_limit-proto.asm"

.INCLUDE "overworld/get_timed_delivery_time.asm"

.INCLUDE "overworld/set_delivery_timer.asm"

.INCLUDE "overworld/decrement_delivery_timer.asm"

.INCLUDE "overworld/queue_delivery_success_interaction.asm"

.INCLUDE "overworld/queue_delivery_failure_interaction.asm"

.INCLUDE "overworld/get_timed_delivery_enter_speed.asm"

.INCLUDE "overworld/get_timed_delivery_exit_speed.asm"

.INCLUDE "overworld/get_delivery_sprite_and_placeholder.asm"

.INCLUDE "overworld/spawn_delivery_entities.asm"

.INCLUDE "misc/is_player_busy.asm"

.INCLUDE "overworld/init_delivery_sequence.asm"

.INCLUDE "misc/restore_overworld_state.asm"

.INCLUDE "data/music/dataset_table.asm"

.INCLUDE "data/music/pack_pointer_table.asm"

.INCLUDE "audio/get_audio_bank.asm"

.INCLUDE "audio/initialize_music_subsystem.asm"

.INCLUDE "audio/change_music.asm"

.INCLUDE "audio/set_num_channels.asm"

.INCLUDE "overworld/set_auto_sector_music_changes.asm"
