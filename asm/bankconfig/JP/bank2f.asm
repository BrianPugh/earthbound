.SEGMENT "BANK2F"
.INCLUDE "common.asm"
.INCLUDE "config.asm"
.INCLUDE "structs.asm"
.INCLUDE "symbols/bank00.inc.asm"
.INCLUDE "symbols/bank01.inc.asm"
.INCLUDE "symbols/bank02.inc.asm"
.INCLUDE "symbols/bank04.inc.asm"
.INCLUDE "symbols/bank2f.inc.asm"
.INCLUDE "symbols/audiopacks.inc.asm"
.INCLUDE "symbols/globals.inc.asm"
.INCLUDE "symbols/map.inc.asm"
.INCLUDE "symbols/overworld_sprites.inc.asm"
.INCLUDE "symbols/sram.inc.asm"

INSERT_AUDIO_PACK 23
INSERT_AUDIO_PACK 11
INSERT_AUDIO_PACK 142
INSERT_AUDIO_PACK 160
INSERT_AUDIO_PACK 101
INSERT_AUDIO_PACK 103
INSERT_AUDIO_PACK 51
INSERT_AUDIO_PACK 18
INSERT_AUDIO_PACK 29
INSERT_AUDIO_PACK 93
INSERT_AUDIO_PACK 95
INSERT_AUDIO_PACK 164
INSERT_AUDIO_PACK 151
INSERT_AUDIO_PACK 143
INSERT_AUDIO_PACK 12
INSERT_AUDIO_PACK 135
INSERT_AUDIO_PACK 83
INSERT_AUDIO_PACK 88
INSERT_AUDIO_PACK 155
INSERT_AUDIO_PACK 31
INSERT_AUDIO_PACK 129
INSERT_AUDIO_PACK 22
INSERT_AUDIO_PACK 17
INSERT_AUDIO_PACK 91
INSERT_AUDIO_PACK 81
INSERT_AUDIO_PACK 147
INSERT_AUDIO_PACK 152
INSERT_AUDIO_PACK 159
INSERT_AUDIO_PACK 49
INSERT_AUDIO_PACK 9
INSERT_AUDIO_PACK 145
INSERT_AUDIO_PACK 69
INSERT_AUDIO_PACK 167
INSERT_AUDIO_PACK 130
INSERT_AUDIO_PACK 168
INSERT_AUDIO_PACK 75
INSERT_AUDIO_PACK 137
INSERT_AUDIO_PACK 59
INSERT_AUDIO_PACK 41
INSERT_AUDIO_PACK 7

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

.INCLUDE "data/sound_stone_melodies.asm"



.INCLUDE "data/map/per_sector_town_map_data.asm"

.INCLUDE "data/map/town_map_mapping.asm"

.INCLUDE "data/debug/sound_menu_option_strings.asm"

.INCLUDE "misc/draw_hex_byte_to_vram.asm"

.INCLUDE "system/debug/setup_debug_sound_menu-jp.asm"

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

.INCLUDE "system/debug/run_debug_overworld_mode-jp.asm"

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

.INCLUDE "system/saves/load_replay_save_slot-jp.asm"

.INCLUDE "system/saves/update_replay_save_slot.asm"

.INCLUDE "misc/start_replay_mode.asm"

.INCLUDE "misc/disable_replay_mode.asm"

.INCLUDE "misc/delay_during_force_blank.asm"

.INCLUDE "system/debug/setup_debug_check_position_window-jp.asm"

.INCLUDE "data/debug/debug_position_window_hdma_table.asm"

.INCLUDE "text/disable_hdma_and_init_window.asm"

.INCLUDE "data/debug/debug_tilemap_data.asm"

DEBUG_MENU_FONT:
	BINARY "fonts/debug.gfx"

.INCLUDE "data/debug/debug_font_mask_data.asm"

.INCLUDE "data/debug/debug_font_palette.asm"

DEBUG_CURSOR_GRAPHICS:
	BINARY "debug_cursor.gfx"

.INCLUDE "data/debug/debug_menu_zero_data.asm"

.INCLUDE "data/debug/debug_menu_palettes.asm"

.INCLUDE "data/system/compiler_version_string.asm"

.INCLUDE "data/unused/EFF3DB.asm"

.INCLUDE "data/unused/EFF511.asm"

.INCLUDE "data/unused/EFF53B.asm"

.INCLUDE "data/debug/debug_cursor_spritemap.asm"
