.SEGMENT "BANK2F"
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
.INCLUDE "symbols/map.inc.asm"
.INCLUDE "symbols/misc.inc.asm"
.INCLUDE "symbols/overworld_sprites.inc.asm"
.INCLUDE "symbols/sram.inc.asm"
.INCLUDE "symbols/text.inc.asm"

.INCLUDE "battle/enemy_flashing_off.asm"

.INCLUDE "battle/enemy_flashing_on.asm"

.INCLUDE "misc/clear_sprite_attribute_bits.asm"

.INCLUDE "misc/set_sprite_attribute_bits.asm"

.INCLUDE "text/clear_window_tilemap.asm"

.INCLUDE "misc/backup_selected_menu_option.asm"

.INCLUDE "text/check_text_fits_in_window.asm"

.INCLUDE "audio/pause_music.asm"

.INCLUDE "misc/set_half_hppp_meter_speed.asm"

.INCLUDE "audio/resume_music.asm"

.INCLUDE "misc/init_bubble_monkey.asm"

.INCLUDE "misc/update_bubble_monkey_mode.asm"

.INCLUDE "misc/update_party_follower_movement.asm"

.INCLUDE "intro/run_title_sequence.asm"

.INCLUDE "data/sram_signature.asm"

.INCLUDE "data/unknown/EF05A6.asm"

.INCLUDE "system/saves/erase_save_block.asm"

.INCLUDE "system/saves/check_block_signature.asm"

.INCLUDE "system/saves/check_all_blocks_signature.asm"

.INCLUDE "system/saves/copy_save_block.asm"

.INCLUDE "system/saves/calc_save_block_checksum.asm"

.INCLUDE "system/saves/calc_save_block_checksum_complement.asm"

.INCLUDE "system/saves/validate_save_block_checksums.asm"

.INCLUDE "system/saves/check_save_corruption.asm"

.INCLUDE "system/saves/save_game_block.asm"

.INCLUDE "system/saves/save_game_slot.asm"

.INCLUDE "system/saves/load_game_slot.asm"

.INCLUDE "system/saves/check_sram_integrity.asm"

.INCLUDE "system/saves/erase_save_slot.asm"

.INCLUDE "system/saves/copy_save_slot.asm"

.INCLUDE "system/saves/load_autosave_and_transition.asm"

.INCLUDE "overworld/get_delivery_attempts.asm"

.INCLUDE "overworld/reset_delivery_attempts.asm"

.INCLUDE "overworld/check_delivery_attempt_limit.asm"

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

.INCLUDE "data/map/tileset_table.asm"

.INCLUDE "data/map/tileset_graphics_pointer_table.asm"

.INCLUDE "data/map/tileset_arrangement_pointer_table.asm"

.INCLUDE "data/map/tileset_palette_pointer_table.asm"

.INCLUDE "data/map/tileset_collision_pointer_table.asm"

.INCLUDE "data/map/tileset_animation_pointer_table.asm"

.INCLUDE "data/map/tileset_animation_properties_pointer_table.asm"

.INCLUDE "data/map/tileset_animation_properties/00.asm"

.INCLUDE "data/map/tileset_animation_properties/01.asm"

.INCLUDE "data/map/tileset_animation_properties/02.asm"

.INCLUDE "data/map/tileset_animation_properties/03.asm"

.INCLUDE "data/map/tileset_animation_properties/04.asm"

.INCLUDE "data/map/tileset_animation_properties/05.asm"

.INCLUDE "data/map/tileset_animation_properties/06.asm"

.INCLUDE "data/map/tileset_animation_properties/07.asm"

.INCLUDE "data/map/tileset_animation_properties/08.asm"

.INCLUDE "data/map/tileset_animation_properties/09.asm"

.INCLUDE "data/map/tileset_animation_properties/10.asm"

.INCLUDE "data/map/tileset_animation_properties/11.asm"

.INCLUDE "data/map/tileset_animation_properties/12.asm"

.INCLUDE "data/map/tileset_animation_properties/13.asm"

.INCLUDE "data/map/tileset_animation_properties/14.asm"

.INCLUDE "data/map/tileset_animation_properties/15.asm"

.INCLUDE "data/map/tileset_animation_properties/16.asm"

.INCLUDE "data/map/tileset_animation_properties/17.asm"

.INCLUDE "data/map/tileset_animation_properties/18.asm"

.INCLUDE "data/map/tileset_animation_properties/19.asm"

.INCLUDE "data/sprite_grouping_pointers.asm"

.INCLUDE "data/sprite_grouping_data.asm"

.INCLUDE "data/unknown/EF4A40.asm"

LOCALEINCLUDE "text_data/EEXPLPSI.ebtxt"

LOCALEINCLUDE "text_data/E16DKFD.ebtxt"

LOCALEINCLUDE "text_data/E07GPFT.ebtxt"

LOCALEINCLUDE "text_data/EBATTLE5.ebtxt"

LOCALEINCLUDE "text_data/EBATTLE4.ebtxt"

LOCALEINCLUDE "text_data/EBATTLE8.ebtxt"

LOCALEINCLUDE "text_data/EBATTLE2.ebtxt"

LOCALEINCLUDE "text_data/EBATTLE0.ebtxt"

LOCALEINCLUDE "text_data/EBATTLE3.ebtxt"

LOCALEINCLUDE "text_data/EBATTLE9.ebtxt"

LOCALEINCLUDE "text_data/E04GRFD.ebtxt"

LOCALEINCLUDE "text_data/EBATTLE1.ebtxt"

LOCALEINCLUDE "text_data/EGOODS2.ebtxt"

LOCALEINCLUDE "text_data/UNKNOWN_EFA2FA.ebtxt"

.INCLUDE "data/command_window_text.asm"

.INCLUDE "data/status_window_text.asm"

LOCALEINCLUDE "text_data/KEYBOARD.ebtxt"

.INCLUDE "data/name_input_window_selection_layout_pointers.asm"

LOCALEINCLUDE "text_data/DEBUG_TEXT.ebtxt"

.INCLUDE "data/map/per_sector_town_map_data.asm"

.INCLUDE "data/map/town_map_mapping.asm"

.INCLUDE "data/unknown/EFC51B.asm"

.INCLUDE "data/unknown/EFCD1B.asm"

.INCLUDE "data/debug/sound_menu_option_strings.asm"

.INCLUDE "misc/draw_hex_byte_to_vram.asm"

.INCLUDE "system/debug/setup_debug_sound_menu.asm"

.INCLUDE "system/debug/run_debug_sound_test.asm"

.INCLUDE "data/debug/menu_option_strings.asm"

.INCLUDE "system/debug/load_debug_mode_graphics.asm"

.INCLUDE "system/debug/init_debug_mode_display.asm"

.INCLUDE "system/debug/init_debug_menu_screen.asm"

.INCLUDE "system/debug/draw_debug_menu_text_row.asm"

.INCLUDE "system/debug/display_menu_options.asm"

.INCLUDE "system/debug/integer_to_hex_debug_tiles.asm"

.INCLUDE "system/debug/integer_to_decimal_debug_tiles.asm"

.INCLUDE "system/debug/integer_to_binary_debug_tiles.asm"

.INCLUDE "system/debug/display_check_position_debug_overlay.asm"

.INCLUDE "system/debug/display_view_character_debug_overlay.asm"

.INCLUDE "system/debug/get_debug_collision_tile.asm"

.INCLUDE "system/debug/update_debug_collision_row.asm"

.INCLUDE "system/debug/update_debug_collision_column.asm"

.INCLUDE "system/debug/update_debug_collision_display.asm"

.INCLUDE "system/debug/run_debug_overworld_mode.asm"

.INCLUDE "system/debug/load_debug_cursor_graphics.asm"

.INCLUDE "system/debug/handle_cursor_movement.asm"

.INCLUDE "system/debug/process_command_selection.asm"

.INCLUDE "system/debug/load_menu.asm"

.INCLUDE "system/debug/check_debug_mode_1.asm"

.INCLUDE "system/debug/clamp_to_debug_max.asm"

.INCLUDE "system/debug/check_debug_exit_button.asm"

.INCLUDE "system/debug/check_view_character_mode.asm"

.INCLUDE "system/debug/check_debug_enemies_enabled.asm"

.INCLUDE "system/saves/save_replay_save_slot.asm"

.INCLUDE "misc/restore_rng_if_bad_sram.asm"

.INCLUDE "misc/backup_rng_if_bad_sram.asm"

.INCLUDE "system/saves/load_replay_save_slot.asm"

.INCLUDE "system/saves/update_replay_save_slot.asm"

.INCLUDE "misc/start_replay_mode.asm"

.INCLUDE "misc/disable_replay_mode.asm"

.INCLUDE "misc/delay_during_force_blank.asm"

.INCLUDE "system/debug/setup_debug_check_position_window.asm"

.INCLUDE "data/unknown/EFEB1D.asm"

.INCLUDE "text/disable_hdma_and_init_window.asm"

.INCLUDE "data/unknown/EFEB3D.asm"

DEBUG_MENU_FONT:
	BINARY "fonts/debug.gfx"

.INCLUDE "data/unknown/EFEF70.asm"

.INCLUDE "data/debug/debug_font_palette.asm"

DEBUG_CURSOR_GRAPHICS:
	BINARY "debug_cursor.gfx"

.INCLUDE "data/unknown/EFF0D7.asm"

.INCLUDE "data/unknown/EFF1BB.asm"

.INCLUDE "data/unknown_version_string.asm"

.INCLUDE "data/unused/EFF3DB.asm"

.INCLUDE "data/unused/EFF511.asm"

.INCLUDE "data/unused/EFF53B.asm"

.INCLUDE "data/debug/debug_cursor_spritemap.asm"
