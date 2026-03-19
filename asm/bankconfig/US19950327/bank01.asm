.SEGMENT "BANK01"
.INCLUDE "common.asm"
.INCLUDE "config.asm"
.INCLUDE "structs.asm"
.INCLUDE "symbols/bank00.inc.asm"
.INCLUDE "symbols/bank01.inc.asm"
.INCLUDE "symbols/bank02.inc.asm"
.INCLUDE "symbols/bank03.inc.asm"
.INCLUDE "symbols/bank04.inc.asm"
.INCLUDE "symbols/bank2f.inc.asm"
.INCLUDE "symbols/globals.inc.asm"
.INCLUDE "symbols/misc.inc.asm"
.INCLUDE "symbols/text.inc.asm"

.INCLUDE "text/hp_pp_window/hide_hppp_windows_long.asm"

.INCLUDE "text/display_text_and_wait_for_fade.asm"

.INCLUDE "text/enable_blinking_triangle.asm"

.INCLUDE "text/clear_blinking_prompt.asm"

.INCLUDE "text/get_blinking_prompt.asm"

.INCLUDE "text/set_text_sound_mode.asm"

.INCLUDE "system/render_frame_tick.asm"

.INCLUDE "text/get_window_focus.asm"

.INCLUDE "text/set_window_focus.asm"

.INCLUDE "text/close_focus_window.asm"

.INCLUDE "text/window/close_all_windows.asm"

.INCLUDE "text/lock_input.asm"

.INCLUDE "text/unlock_input.asm"

.INCLUDE "text/hp_pp_window/tick_hppp_meter_n_frames.asm"

.INCLUDE "text/text_speed_delay.asm"

.INCLUDE "text/ccs/halt.asm"

.INCLUDE "text/wait_for_actionscript.asm"

.INCLUDE "text/get_active_window_address.asm"

.INCLUDE "text/transfer_active_mem_storage.asm"

.INCLUDE "text/transfer_storage_mem_active.asm"

.INCLUDE "text/get_argument_memory.asm"

.INCLUDE "text/get_secondary_memory.asm"

.INCLUDE "text/get_working_memory.asm"

.INCLUDE "text/increment_secondary_memory.asm"

.INCLUDE "text/set_secondary_memory.asm"

.INCLUDE "text/set_working_memory.asm"

.INCLUDE "text/set_argument_memory.asm"

.INCLUDE "text/get_text_x.asm"

.INCLUDE "text/get_text_y.asm"

.INCLUDE "text/create_window.asm"

.INCLUDE "text/hp_pp_window/copy_hppp_window_to_vram.asm"

.INCLUDE "text/window/render_window_frame.asm"

.INCLUDE "battle/enemy_flashing_off.asm"

.INCLUDE "battle/enemy_flashing_on.asm"

.INCLUDE "text/show_hppp_windows.asm"

.INCLUDE "text/hide_hppp_windows.asm"

.INCLUDE "text/window/write_tile_to_window.asm"

.INCLUDE "text/window/set_window_tile_attribute.asm"

.INCLUDE "text/ccs/clear_line.asm"

.INCLUDE "text/window/close_all_windows_redirect.asm"

.INCLUDE "text/menu/add_menu_item_far.asm"

.INCLUDE "text/menu/count_menu_option_chain_redirect.asm"

.INCLUDE "text/count_string_length_redirect.asm"

.INCLUDE "text/number_to_text_buffer_far.asm"

.INCLUDE "text/set_focus_text_cursor_redirect.asm"

.INCLUDE "text/print_newline_redirect.asm"

.INCLUDE "text/window/set_window_tile_attribute_redirect.asm"

.INCLUDE "text/print_letter_redirect.asm"

.INCLUDE "text/print_string_redirect.asm"

.INCLUDE "text/window/scroll_window_up_redirect.asm"

.INCLUDE "text/clear_window_tilemap.asm"

.INCLUDE "text/print_letter.asm"

.INCLUDE "text/set_tile_attribute_and_redraw.asm"

.INCLUDE "text/number_to_text_buffer.asm"

.INCLUDE "text/print_number.asm"

.INCLUDE "text/window/set_window_number_padding.asm"

.INCLUDE "text/window/dispatch_window_border_animation.asm"

.INCLUDE "text/print_string.asm"

.INCLUDE "text/window/clear_window_content.asm"

.INCLUDE "text/window/clear_focus_window_content.asm"

.INCLUDE "text/change_current_window_font.asm"

.INCLUDE "text/window/set_window_palette_index.asm"

.INCLUDE "text/num_select_prompt.asm"

.INCLUDE "text/hp_pp_window/show_hppp_and_money_windows.asm"

.INCLUDE "text/menu/find_first_empty_menu_option.asm"

.INCLUDE "text/window/clear_focus_window_menu_options.asm"

.INCLUDE "text/menu/count_menu_option_chain.asm"

.INCLUDE "text/menu/add_menu_option.asm"

.INCLUDE "text/menu/add_positioned_menu_option.asm"

.INCLUDE "text/menu/add_menu_item.asm"

.INCLUDE "text/menu/add_menu_item_with_sound.asm"

.INCLUDE "text/menu/add_menu_item_no_position.asm"

.INCLUDE "text/print_menu_items.asm"

.INCLUDE "text/count_string_length.asm"

.INCLUDE "text/window/open_window_and_print_menu.asm"

.INCLUDE "text/menu/layout_and_print_menu_at_selection.asm"

.INCLUDE "text/menu/print_menu_at_selection.asm"

.INCLUDE "text/move_cursor.asm"

.INCLUDE "text/selection_menu.asm"

.INCLUDE "text/menu/set_cursor_move_callback.asm"

.INCLUDE "text/menu/clear_cursor_move_callback.asm"

.INCLUDE "battle/ui/get_battler_row_x_position.asm"

.INCLUDE "battle/ui/check_battle_action_targetable.asm"

.INCLUDE "battle/ui/find_next_battler_in_row.asm"

.INCLUDE "battle/ui/find_prev_battler_in_row.asm"

.INCLUDE "battle/ui/display_battle_target_text.asm"

.INCLUDE "battle/ui/select_battle_target.asm"

.INCLUDE "battle/ui/select_battle_row.asm"

.INCLUDE "battle/ui/select_battle_target_dispatch.asm"

.INCLUDE "text/party_character_selector.asm"

.INCLUDE "text/character_select_prompt.asm"

.INCLUDE "text/window/get_window_menu_option_count.asm"

.INCLUDE "text/window/animate_window_border.asm"

.INCLUDE "text/hp_pp_window/animate_window_border_with_hppp.asm"

.INCLUDE "text/number_to_ascii_buffer.asm"

.INCLUDE "audio/pause_music.asm"

.INCLUDE "misc/set_half_hppp_meter_speed.asm"

.INCLUDE "audio/resume_music.asm"

.INCLUDE "text/hp_pp_window/toggle_hppp_flipout_mode.asm"

.INCLUDE "text/window_tick.asm"

.INCLUDE "text/hp_pp_window/update_hppp_meter_and_render.asm"

.INCLUDE "system/debug/y_button_menu.asm"

.INCLUDE "overworld/talk_to.asm"

.INCLUDE "overworld/check.asm"

.INCLUDE "inventory/get_weapon_item_name.asm"

.INCLUDE "inventory/get_body_item_name.asm"

.INCLUDE "overworld/open_menu-proto.asm"

.INCLUDE "text/open_hppp_display.asm"

.INCLUDE "overworld/show_town_map.asm"

.INCLUDE "overworld/debug/y_button_flag.asm"

.INCLUDE "overworld/debug/y_button_guide.asm"

.INCLUDE "overworld/debug/set_char_level.asm"

.INCLUDE "overworld/debug/y_button_goods.asm"

.INCLUDE "text/push_text_stack_frame.asm"

.INCLUDE "text/pop_text_stack_frame.asm"

.INCLUDE "text/compare_strings.asm"

.INCLUDE "text/ccs/print_stat.asm"

.INCLUDE "text/ccs/set_number_padding.asm"

.INCLUDE "text/ccs/text_effects.asm"

.INCLUDE "text/ccs/jump.asm"

.INCLUDE "text/ccs/jump_multi.asm"

.INCLUDE "text/ccs/set_event_flag.asm"

.INCLUDE "text/ccs/clear_event_flag.asm"

.INCLUDE "text/ccs/jump_event_flag.asm"

.INCLUDE "text/ccs/get_event_flag.asm"

.INCLUDE "text/ccs/print_special_graphics.asm"

.INCLUDE "text/ccs/open_window.asm"

.INCLUDE "text/ccs/switch_to_window.asm"

.INCLUDE "text/ccs/call.asm"

.INCLUDE "text/ccs/create_number_selector.asm"

.INCLUDE "text/ccs/force_text_alignment.asm"

.INCLUDE "text/ccs/check_equal.asm"

.INCLUDE "text/ccs/check_not_equal.asm"

.INCLUDE "text/ccs/print_horizontal_strings.asm"

.INCLUDE "text/ccs/copy_to_argmem.asm"

.INCLUDE "text/ccs/set_secmem.asm"

.INCLUDE "text/ccs/party_selection_menu_uncancellable.asm"

.INCLUDE "text/ccs/party_selection_menu.asm"

.INCLUDE "text/ccs/print_item_name.asm"

.INCLUDE "text/ccs/print_teleport_destination_name.asm"

.INCLUDE "text/ccs/get_character_number.asm"

.INCLUDE "text/ccs/play_music.asm"

.INCLUDE "text/ccs/stop_music.asm"

.INCLUDE "text/ccs/play_sfx.asm"

.INCLUDE "text/ccs/get_letter_from_character_name.asm"

.INCLUDE "text/ccs/get_letter_from_stat.asm"

.INCLUDE "text/ccs/print_character.asm"

.INCLUDE "text/ccs/test_inventory_full.asm"

.INCLUDE "text/ccs/wallet_increase.asm"

.INCLUDE "text/ccs/wallet_decrease.asm"

.INCLUDE "text/ccs/recover_hp_by_percent.asm"

.INCLUDE "text/ccs/deplete_hp_by_percent.asm"

.INCLUDE "text/ccs/recover_hp_by_amount.asm"

.INCLUDE "text/ccs/deplete_hp_by_amount.asm"

.INCLUDE "text/ccs/recover_pp_by_percent.asm"

.INCLUDE "text/ccs/deplete_pp_by_percent.asm"

.INCLUDE "text/ccs/recover_pp_by_amount.asm"

.INCLUDE "text/ccs/deplete_pp_by_amount.asm"

.INCLUDE "text/ccs/give_item_to_character.asm"

.INCLUDE "text/ccs/take_item_from_character.asm"

.INCLUDE "text/ccs/test_inventory_not_full.asm"

.INCLUDE "text/ccs/test_character_doesnt_have_item.asm"

.INCLUDE "text/ccs/test_character_has_item.asm"

.INCLUDE "text/ccs/trigger_psi_teleport.asm"

.INCLUDE "text/ccs/trigger_teleport.asm"

.INCLUDE "text/ccs/pause.asm"

.INCLUDE "text/ccs/display_shop_menu.asm"

.INCLUDE "text/ccs/get_item_price.asm"

.INCLUDE "text/ccs/get_item_sell_price.asm"

.INCLUDE "text/ccs/test_character_can_equip_item.asm"

.INCLUDE "text/ccs/print_character_name.asm"

.INCLUDE "text/ccs/get_character_status.asm"

.INCLUDE "text/ccs/inflict_character_status.asm"

.INCLUDE "text/ccs/test_character_status.asm"

.INCLUDE "text/ccs/get_gender_etc.asm"

.INCLUDE "text/ccs/switch_gender_etc.asm"

.INCLUDE "text/ccs/test_equality.asm"

.INCLUDE "text/ccs/get_exp_for_next_level.asm"

.INCLUDE "text/ccs/print_number.asm"

.INCLUDE "text/ccs/text_speed_delay.asm"

.INCLUDE "text/ccs/show_character_inventory.asm"

.INCLUDE "text/ccs/selection_menu_allow_cancel.asm"

.INCLUDE "text/ccs/selection_menu_no_cancel.asm"

.INCLUDE "text/ccs/print_money_amount.asm"

.INCLUDE "text/ccs/give_item_to_character_2.asm"

.INCLUDE "text/ccs/take_item_from_character_2.asm"

.INCLUDE "text/ccs/check_item_equipped.asm"

.INCLUDE "text/ccs/check_item_usable_by.asm"

.INCLUDE "text/ccs/equip_character_from_inventory.asm"

.INCLUDE "text/ccs/escargo_express_move_item.asm"

.INCLUDE "text/ccs/deliver_escargo_express_item.asm"

.INCLUDE "text/ccs/get_item_number.asm"

.INCLUDE "text/ccs/test_has_enough_money.asm"

.INCLUDE "text/ccs/get_escargo_express_item.asm"

.INCLUDE "text/ccs/display_status.asm"

.INCLUDE "text/ccs/print_vertical_strings.asm"

.INCLUDE "text/ccs/set_argmem.asm"

.INCLUDE "text/ccs/get_menu_option_count.asm"

.INCLUDE "text/ccs/learn_special_psi.asm"

.INCLUDE "text/ccs/atm_increase.asm"

.INCLUDE "text/ccs/atm_decrease.asm"

.INCLUDE "text/ccs/test_atm_has_enough_money.asm"

.INCLUDE "text/ccs/party_member_add.asm"

.INCLUDE "text/ccs/party_member_remove.asm"

.INCLUDE "inventory/queue_item_for_character.asm"

.INCLUDE "text/ccs/transfer_item_to_queue.asm"

.INCLUDE "text/ccs/get_queued_item_data.asm"

.INCLUDE "text/ccs/escargo_express_store.asm"

.INCLUDE "text/ccs/test_item_is_drink.asm"

.INCLUDE "text/ccs/test_party_enough_characters.asm"

.INCLUDE "text/ccs/print_psi_name.asm"

.INCLUDE "text/ccs/get_random_number.asm"

.INCLUDE "text/cc_display_text_with_ongosub.asm"

.INCLUDE "text/ccs/jump_multi2.asm"

.INCLUDE "text/ccs/try_fixing_items.asm"

.INCLUDE "text/ccs/set_character_direction.asm"

.INCLUDE "text/ccs/set_party_direction.asm"

.INCLUDE "text/ccs/set_tpt_direction.asm"

.INCLUDE "text/ccs/create_entity_tpt.asm"

.INCLUDE "text/ccs/dummy_1F_18.asm"

.INCLUDE "text/ccs/dummy_1F_19.asm"

.INCLUDE "text/ccs/create_floating_sprite_at_tpt_entity.asm"

.INCLUDE "text/ccs/delete_floating_sprite_at_tpt_entity.asm"

.INCLUDE "text/ccs/create_floating_sprite_at_character.asm"

.INCLUDE "text/ccs/delete_floating_sprite_at_character.asm"

.INCLUDE "text/ccs/set_map_palette.asm"

.INCLUDE "text/ccs/create_entity_sprite.asm"

.INCLUDE "text/ccs/delete_entity_tpt.asm"

.INCLUDE "text/ccs/delete_entity_sprite.asm"

.INCLUDE "text/ccs/get_direction_from_character_to_entity.asm"

.INCLUDE "text/ccs/get_direction_from_tpt_entity_to_entity.asm"

.INCLUDE "text/ccs/enable_blinking_triangle.asm"

.INCLUDE "text/ccs/set_character_level.asm"

.INCLUDE "text/ccs/get_direction_from_sprite_entity_to_entity.asm"

.INCLUDE "text/ccs/set_entity_direction_sprite.asm"

.INCLUDE "text/ccs/set_player_movement_lock.asm"

.INCLUDE "text/ccs/set_tpt_entity_delay.asm"

.INCLUDE "text/ccs/disable_sprite_movement.asm"

.INCLUDE "text/ccs/set_player_movement_lock_if_camera_refocused.asm"

.INCLUDE "text/ccs/enable_npc_movement.asm"

.INCLUDE "text/ccs/enable_sprite_movement.asm"

.INCLUDE "text/ccs/set_character_invisibility.asm"

.INCLUDE "text/ccs/set_character_visibility.asm"

.INCLUDE "text/ccs/teleport_party_to_tpt_entity.asm"

.INCLUDE "text/ccs/set_camera_focus_sprite.asm"

.INCLUDE "text/ccs/screen_reload_pointer.asm"

.INCLUDE "text/ccs/set_tpt_entity_movement.asm"

.INCLUDE "text/ccs/set_sprite_entity_movement.asm"

.INCLUDE "text/ccs/test_item_is_condiment.asm"

.INCLUDE "text/ccs/trigger_battle.asm"

.INCLUDE "text/ccs/set_respawn_point.asm"

.INCLUDE "text/ccs/check_escargo_express_item_flags.asm"

.INCLUDE "text/ccs/activate_hotspot.asm"

.INCLUDE "text/ccs/deactivate_hotspot.asm"

.INCLUDE "text/ccs/toggle_text_printing_sound.asm"

.INCLUDE "text/ccs/get_set_game_state_c4.asm"

.INCLUDE "text/ccs/nop_1f_40.asm"

.INCLUDE "text/ccs/trigger_special_event.asm"

.INCLUDE "text/ccs/trigger_photographer_event.asm"

.INCLUDE "text/ccs/create_floating_sprite_at_sprite_entity.asm"

.INCLUDE "text/ccs/delete_floating_sprite_at_sprite_entity.asm"

.INCLUDE "text/ccs/display_battle_animation.asm"

.INCLUDE "text/ccs/set_music_effect.asm"

.INCLUDE "text/ccs/trigger_timed_event.asm"

.INCLUDE "text/ccs/increase_character_experience.asm"

.INCLUDE "text/ccs/increase_character_iq.asm"

.INCLUDE "text/ccs/increase_character_guts.asm"

.INCLUDE "text/ccs/increase_character_speed.asm"

.INCLUDE "text/ccs/increase_character_vitality.asm"

.INCLUDE "text/ccs/increase_character_luck.asm"

.INCLUDE "text/ccs/get_item_category.asm"

.INCLUDE "text/ccs/resolve_cc_table_data.asm"

.INCLUDE "text/menu/cc_add_menu_option_with_callback.asm"

.INCLUDE "text/menu/cc_gather_menu_option_text.asm"

.INCLUDE "text/ccs/load_string.asm"

.INCLUDE "text/ccs/tree_18.asm"

.INCLUDE "text/ccs/tree_19.asm"

.INCLUDE "text/ccs/tree_1A.asm"

.INCLUDE "text/ccs/tree_1B.asm"

.INCLUDE "text/ccs/tree_1C.asm"

.INCLUDE "text/ccs/tree_1D.asm"

.INCLUDE "text/ccs/tree_1E.asm"

.INCLUDE "text/ccs/tree_1F.asm"

.INCLUDE "text/init_text_state.asm"

.INCLUDE "inventory/store/restore_text_state_attributes.asm"

.INCLUDE "text/display_text.asm"

.INCLUDE "misc/give_item_to_specific_character.asm"

.INCLUDE "misc/give_item_to_character.asm"

.INCLUDE "misc/remove_item_from_inventory.asm"

.INCLUDE "misc/take_item_from_specific_character.asm"

.INCLUDE "misc/take_item_from_character.asm"

.INCLUDE "misc/reduce_hp_amtpercent.asm"

.INCLUDE "misc/recover_hp_amtpercent.asm"

.INCLUDE "misc/reduce_pp_amtpercent.asm"

.INCLUDE "misc/recover_pp_amtpercent.asm"

.INCLUDE "misc/equip_item.asm"

.INCLUDE "text/get_character_by_party_position.asm"

.INCLUDE "text/is_escargo_express_full.asm"

.INCLUDE "misc/escargo_express_store.asm"

.INCLUDE "misc/escargo_express_move.asm"

.INCLUDE "inventory/remove_escargo_express_item.asm"

.INCLUDE "inventory/deliver_escargo_express_item.asm"

.INCLUDE "text/print_item_type.asm"

.INCLUDE "text/print_cc_table_value.asm"

.INCLUDE "text/print_character_name.asm"

.INCLUDE "text/menu/display_menu_header_text.asm"

.INCLUDE "text/window/close_menu_header_window.asm"

.INCLUDE "text/menu/open_telephone_menu.asm"

.INCLUDE "text/window/display_status_window.asm"

.INCLUDE "misc/inventory_get_item_name.asm"

.INCLUDE "text/menu/selection_menu_with_focus.asm"

.INCLUDE "inventory/select_escargo_express_item-proto.asm"

.INCLUDE "text/set_hppp_window_mode_item.asm"

.INCLUDE "text/window/initialize_window_flavour_palette.asm"

.INCLUDE "text/hp_pp_window/reset_hppp_options_and_load_palette.asm"

.INCLUDE "inventory/store/open_store_menu.asm"

.INCLUDE "misc/get_item_type.asm"

.INCLUDE "inventory/equipment/display_equipment_menu.asm"

.INCLUDE "inventory/equipment/display_character_equipment_stats.asm"

.INCLUDE "inventory/equipment/show_equipment_and_stats.asm"

.INCLUDE "inventory/equipment/equipment_change_menu.asm"

.INCLUDE "text/window/display_money_window.asm"

.INCLUDE "inventory/equipment/open_equipment_menu.asm"

.INCLUDE "text/menu/open_teleport_destination_menu.asm"

.INCLUDE "text/display_telephone_contact_text.asm"

.INCLUDE "battle/ui/set_battle_attacker_name.asm"

.INCLUDE "battle/return_battle_attacker_address.asm"

.INCLUDE "battle/ui/set_battle_target_name.asm"

.INCLUDE "battle/return_battle_target_address.asm"

.INCLUDE "inventory/set_current_item.asm"

.INCLUDE "inventory/get_current_item.asm"

.INCLUDE "text/set_cnum.asm"

.INCLUDE "text/get_cnum.asm"

.INCLUDE "text/get_nearby_npc_config_type.asm"

.INCLUDE "inventory/get_sector_item_type.asm"

.INCLUDE "battle/determine_targetting.asm"

.INCLUDE "overworld/use_item.asm"

.INCLUDE "text/menu/overworld_psi_menu-proto.asm"

.INCLUDE "text/menu/display_psi_description.asm"

.INCLUDE "text/menu/open_status_menu.asm"

.INCLUDE "overworld/teleport.asm"

.INCLUDE "overworld/attempt_homesickness.asm"

.INCLUDE "overworld/get_off_bicycle.asm"

.INCLUDE "text/dispatch_special_event.asm"

.INCLUDE "text/print_mrsaturn_letter.asm"

.INCLUDE "text/check_psi_affliction_block.asm"

.INCLUDE "text/check_character_has_psi_ability.asm"

.INCLUDE "text/check_character_psi_availability.asm"

.INCLUDE "text/check_character_psi_availability_far.asm"

.INCLUDE "text/find_first_character_with_psi.asm"

.INCLUDE "text/count_characters_with_psi.asm"

.INCLUDE "text/get_psi_name.asm"

.INCLUDE "battle/generate_psi_list.asm"

.INCLUDE "text/menu/display_character_psi_list.asm"

.INCLUDE "text/menu/display_psi_target_and_cost.asm"

.INCLUDE "text/print_psi_ability_name.asm"

.INCLUDE "text/menu/display_psi_ability_details-jp.asm"

.INCLUDE "battle/ui/generate_battle_psi_list.asm"

.INCLUDE "text/check_psi_category_available.asm"

.INCLUDE "battle/battle_psi_menu.asm"

.INCLUDE "battle/ui/determine_battle_item_target.asm"

.INCLUDE "battle/ui/battle_item_menu.asm"

.INCLUDE "inventory/get_item_ep.asm"

.INCLUDE "text/calculate_stat_gain.asm"

.INCLUDE "misc/level_up_char.asm"

.INCLUDE "misc/reset_char_level_one.asm"

.INCLUDE "misc/gain_exp.asm"

.INCLUDE "misc/find_condiment.asm"

.INCLUDE "overworld/show_hp_alert.asm"

.INCLUDE "text/display_in_battle_text.asm"

.INCLUDE "text/display_text_wait.asm"

.INCLUDE "battle/ui/initialize_battle_party.asm"

.INCLUDE "text/show_hppp_windows_redirect.asm"

.INCLUDE "text/hide_hppp_windows_redirect.asm"

.INCLUDE "text/create_window_redirect.asm"

.INCLUDE "text/set_window_focus_redirect.asm"

.INCLUDE "text/window/clear_focus_window_content_redirect.asm"

.INCLUDE "text/close_focus_window_redirect.asm"

.INCLUDE "text/hp_pp_window/close_all_windows_and_hide_hppp.asm"

.INCLUDE "battle/ui/set_battle_attacker_name_redirect.asm"

.INCLUDE "battle/ui/set_battle_target_name_redirect.asm"

.INCLUDE "inventory/set_current_item_redirect.asm"

.INCLUDE "text/set_cnum_far.asm"

.INCLUDE "text/display_text_with_prompt.asm"

.INCLUDE "misc/remove_item_from_inventory_redirect.asm"

.INCLUDE "battle/select_battle_menu_character_redirect.asm"

.INCLUDE "text/clear_battle_menu_character_indicator_redirect.asm"

.INCLUDE "text/selection_menu_setup.asm"

.INCLUDE "text/print_menu_items_redirect.asm"

.INCLUDE "text/selection_menu_redirect.asm"

.INCLUDE "battle/ui/battle_item_menu_redirect.asm"

.INCLUDE "battle/ui/select_battle_target_dispatch_redirect.asm"

.INCLUDE "battle/battle_psi_menu_redirect.asm"

.INCLUDE "battle/actions/switch_weapon.asm"

.INCLUDE "battle/actions/switch_armor.asm"

.INCLUDE "misc/null/C1E1A2.asm"

.INCLUDE "battle/enemy_select_mode.asm"

.INCLUDE "text/render_naming_character.asm"

.INCLUDE "text/render_dont_care_name-proto.asm"

.INCLUDE "text/text_input_dialog.asm"

.INCLUDE "text/enter_your_name_please.asm"

.INCLUDE "intro/name_a_character.asm"

.INCLUDE "text/window/preview_window_flavour.asm"

.INCLUDE "text/window/preview_window_flavour_hibyte.asm"

.INCLUDE "system/saves/corruption_check.asm"

.INCLUDE "intro/file_select_menu.asm"

.INCLUDE "intro/file_select/show_file_select_menu.asm"

.INCLUDE "text/show_file_copy_dialog.asm"

.INCLUDE "text/confirm_file_delete.asm"

.INCLUDE "intro/file_select/open_text_speed_menu.asm"

.INCLUDE "intro/file_select/file_select_text_speed_menu.asm"

.INCLUDE "intro/file_select/open_sound_menu.asm"

.INCLUDE "intro/file_select/file_select_sound_mode_menu.asm"

.INCLUDE "intro/file_select/open_flavour_menu.asm"

.INCLUDE "intro/file_select_menu_loop.asm"

.INCLUDE "text/check_last_member_status_change.asm"

.INCLUDE "text/menu/run_file_menu.asm"

.INCLUDE "text/center_vwf_for_string.asm"

.INCLUDE "system/antipiracy/sram_check_routine_checksum.asm"
