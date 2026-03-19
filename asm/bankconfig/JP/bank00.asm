.SEGMENT "BANK00"
.INCLUDE "common.asm"
.INCLUDE "config.asm"
.INCLUDE "eventmacros.asm"
.INCLUDE "structs.asm"
.INCLUDE "symbols/bank00.inc.asm"
.INCLUDE "symbols/bank01.inc.asm"
.INCLUDE "symbols/bank02.inc.asm"
.INCLUDE "symbols/bank03.inc.asm"
.INCLUDE "symbols/bank04.inc.asm"
.INCLUDE "symbols/bank2f.inc.asm"
.INCLUDE "symbols/doors.inc.asm"
.INCLUDE "symbols/globals.inc.asm"
.INCLUDE "symbols/map.inc.asm"
.INCLUDE "symbols/misc.inc.asm"
.INCLUDE "symbols/sram.inc.asm"
.INCLUDE "symbols/text.inc.asm"

.INCLUDE "overworld/actionscript/clear_entity_draw_sorting_table.asm"

.INCLUDE "overworld/setup_vram.asm"

.INCLUDE "overworld/initialize.asm"

.INCLUDE "system/load_tileset_anim.asm"

.INCLUDE "system/animate_tileset.asm"

.INCLUDE "system/load_palette_anim.asm"

.INCLUDE "system/animate_palette.asm"

.INCLUDE "unused/C0035B.asm"

.INCLUDE "system/get_colour_average.asm"

.INCLUDE "overworld/adjust_single_colour.asm"

.INCLUDE "overworld/adjust_sprite_palettes_by_average.asm"

.INCLUDE "overworld/prepare_average_for_sprite_palettes.asm"

.INCLUDE "overworld/load_tile_collision.asm"

.INCLUDE "overworld/replace_block.asm"

.INCLUDE "overworld/load_map_block_event_changes.asm"

.INCLUDE "overworld/load_special_sprite_palette.asm"

.INCLUDE "overworld/load_map_palette.asm"

.INCLUDE "overworld/load_map_at_sector.asm"

.INCLUDE "overworld/load_sector_attributes.asm"

.INCLUDE "overworld/load_map_row.asm"

.INCLUDE "overworld/load_map_column.asm"

.INCLUDE "overworld/load_collision_row.asm"

.INCLUDE "overworld/load_collision_column.asm"

.INCLUDE "overworld/map/load_map_row_to_vram.asm"

.INCLUDE "overworld/map/load_map_column_to_vram.asm"

.INCLUDE "overworld/map/clear_map_row_vram.asm"

.INCLUDE "overworld/map/clear_map_column_vram.asm"

.INCLUDE "overworld/reload_map_at_position.asm"

.INCLUDE "overworld/load_map_at_position.asm"

.INCLUDE "overworld/refresh_map_at_position.asm"

.INCLUDE "overworld/camera/scroll_map_to_position.asm"

.INCLUDE "system/debug/debug_map_scroll_handler.asm"

.INCLUDE "overworld/reload_map.asm"

.INCLUDE "overworld/initialize_map.asm"

.INCLUDE "overworld/map/load_initial_map_data.asm"

.INCLUDE "overworld/map/load_map_row_to_vram_long.asm"

.INCLUDE "overworld/initialize_misc_object_data.asm"

.INCLUDE "overworld/entity/clear_overworld_spritemaps.asm"

.INCLUDE "overworld/find_free_space_7E4682.asm"

.INCLUDE "overworld/entity/clear_spritemap_slots.asm"

.INCLUDE "overworld/entity/allocate_sprite_vram.asm"

.INCLUDE "system/alloc_sprite_mem.asm"

.INCLUDE "overworld/entity/upload_sprite_to_vram.asm"

.INCLUDE "overworld/entity/load_overworld_spritemaps.asm"

.INCLUDE "overworld/entity/load_sprite_group_properties.asm"

.INCLUDE "overworld/create_entity.asm"

.INCLUDE "overworld/entity/deallocate_entity_sprite.asm"

.INCLUDE "overworld/remove_entity.asm"

.INCLUDE "overworld/clear_all_enemies.asm"

.INCLUDE "overworld/clear_map_entities.asm"

.INCLUDE "overworld/npc/spawn_npcs_at_sector-jp.asm"

.INCLUDE "overworld/npc/spawn_npcs_in_row.asm"

.INCLUDE "overworld/npc/spawn_npcs_in_column.asm"

.INCLUDE "overworld/get_map_enemy_placement.asm"

.INCLUDE "overworld/attempt_enemy_spawn.asm"

.INCLUDE "overworld/spawn_horizontal.asm"

.INCLUDE "overworld/spawn_vertical.asm"

.INCLUDE "overworld/velocity_store.asm"

.INCLUDE "overworld/update_mushroom_status.asm"

.INCLUDE "overworld/reset_mushroomized_walking.asm"

.INCLUDE "overworld/mushroomization_movement_swap.asm"

.INCLUDE "overworld/reset_party_state.asm"

.INCLUDE "overworld/adjust_position_horizontal.asm"

.INCLUDE "overworld/adjust_position_vertical.asm"

.INCLUDE "overworld/clear_character_afflictions.asm"

.INCLUDE "overworld/party/update_npc_party_lineup-jp.asm"

.INCLUDE "overworld/update_party-jp.asm"

.INCLUDE "overworld/add_party_member-jp.asm"

.INCLUDE "overworld/party/remove_party_member.asm"

.INCLUDE "overworld/party/snap_party_to_leader.asm"

.INCLUDE "overworld/initialize_party.asm"

.INCLUDE "overworld/party/load_party_at_map_position.asm"

.INCLUDE "system/palette/fade_to_map_music_at_leader.asm"

.INCLUDE "overworld/collision/get_collision_at_leader.asm"

.INCLUDE "overworld/get_on_bicycle.asm"

.INCLUDE "overworld/dismount_bicycle.asm"

.INCLUDE "overworld/party/initialize_party_member_entity.asm"

.INCLUDE "overworld/party/get_previous_party_position-jp.asm"

.INCLUDE "overworld/get_previous_position_index-jp.asm"

.INCLUDE "overworld/party/get_distance_to_party_member.asm"

.INCLUDE "overworld/party/adjust_party_member_visibility.asm"

.INCLUDE "overworld/party/init_party_position_buffer-jp.asm"

.INCLUDE "overworld/party/set_leader_position_and_load_party.asm"

.INCLUDE "system/center_screen.asm"

.INCLUDE "intro/start_demo_playback_far.asm"

.INCLUDE "intro/clear_demo_timer.asm"

.INCLUDE "overworld/map_input_to_direction.asm"

.INCLUDE "overworld/collision/check_directional_npc_collision.asm"

.INCLUDE "overworld/npc/find_adjacent_npc_interaction.asm"

.INCLUDE "overworld/find_nearby_checkable_tpt_entry.asm"

.INCLUDE "overworld/entity/set_entity_direction_from_leader.asm"

.INCLUDE "overworld/collision/check_collision_in_direction.asm"

.INCLUDE "overworld/find_clear_direction_for_leader.asm"

.INCLUDE "overworld/find_nearby_talkable_tpt_entry.asm"

.INCLUDE "overworld/update_overworld_player_input.asm"

.INCLUDE "overworld/camera/sync_camera_to_entity.asm"

.INCLUDE "overworld/door/update_escalator_movement.asm"

.INCLUDE "overworld/update_bicycle_movement-jp.asm"

.INCLUDE "overworld/camera/restore_camera_mode.asm"

.INCLUDE "overworld/camera/start_camera_shake.asm"

.INCLUDE "overworld/camera/update_camera_mode_3.asm"

.INCLUDE "overworld/camera/update_special_camera_mode.asm"

.INCLUDE "overworld/update_leader_movement.asm"

.INCLUDE "overworld/update_follower_state.asm"

.INCLUDE "system/palette/restore_bg_palette_and_enable_display.asm"

.INCLUDE "overworld/start_enemy_touch_flash.asm"

.INCLUDE "overworld/check_low_hp_alert.asm"

.INCLUDE "overworld/update_overworld_damage.asm"

.INCLUDE "overworld/update_overworld_frame.asm"

.INCLUDE "battle/init_common.asm"

.INCLUDE "overworld/party/fill_party_position_buffer.asm"

.INCLUDE "overworld/party/sum_alive_party_levels.asm"

.INCLUDE "overworld/collision/get_collision_tile_and_check_ladder.asm"

.INCLUDE "overworld/collision/check_collision_tiles_horizontal.asm"

.INCLUDE "overworld/collision/check_collision_tiles_vertical.asm"

.INCLUDE "overworld/collision/accumulate_collision_flags_vertical.asm"

.INCLUDE "overworld/collision/accumulate_collision_flags_horizontal.asm"

.INCLUDE "overworld/collision/check_collision_tile_pattern.asm"

.INCLUDE "overworld/collision/test_collision_north.asm"

.INCLUDE "overworld/collision/test_collision_south.asm"

.INCLUDE "overworld/collision/test_collision_west.asm"

.INCLUDE "overworld/collision/test_collision_east.asm"

.INCLUDE "overworld/collision/test_collision_diagonal.asm"

.INCLUDE "overworld/collision/check_directional_collision.asm"

.INCLUDE "overworld/collision/check_entity_collision.asm"

.INCLUDE "overworld/collision/lookup_entity_surface_flags.asm"

.INCLUDE "overworld/can_enemy_run_in_direction.asm"

.INCLUDE "overworld/collision/check_entity_obstacle_flags.asm"

.INCLUDE "overworld/collision/check_current_entity_obstacles.asm"

.INCLUDE "overworld/collision/check_enemy_movement_obstacles.asm"

.INCLUDE "overworld/collision/check_npc_player_obstacles.asm"

.INCLUDE "overworld/collision/lookup_surface_flags.asm"

.INCLUDE "overworld/collision/check_player_collision_at_position.asm"

.INCLUDE "overworld/collision/get_collision_at_pixel.asm"

.INCLUDE "overworld/npc_collision_check.asm"

.INCLUDE "overworld/collision/check_entity_collision_at_position.asm"

.INCLUDE "overworld/collision/check_entity_and_npc_collision.asm"

.INCLUDE "overworld/collision/check_prospective_npc_collision.asm"

.INCLUDE "overworld/collision/check_prospective_entity_collision.asm"

.INCLUDE "overworld/reset_queued_interactions.asm"

.INCLUDE "overworld/queue_interaction.asm"

.INCLUDE "overworld/get_current_interaction_type.asm"

.INCLUDE "text/get_queued_interaction_text.asm"

.INCLUDE "overworld/entity/queue_entity_creation.asm"

.INCLUDE "overworld/entity/flush_entity_creation_queue.asm"

.INCLUDE "overworld/door/check_door_in_direction.asm"

.INCLUDE "overworld/screen_transition.asm"

.INCLUDE "overworld/get_screen_transition_sound_effect.asm"

.INCLUDE "audio/resolve_map_sector_music.asm"

.INCLUDE "audio/apply_next_map_music.asm"

.INCLUDE "overworld/change_music_5DD6.asm"

.INCLUDE "audio/get_map_music_at_leader.asm"

.INCLUDE "audio/update_map_music_at_leader.asm"

.INCLUDE "overworld/door/check_door_event_flag.asm"

.INCLUDE "overworld/door/door_handler_nop.asm"

.INCLUDE "overworld/door/door_handler_type6.asm"

.INCLUDE "overworld/door/set_walking_style_stairs.asm"

.INCLUDE "overworld/door/try_activate_door.asm"

.INCLUDE "overworld/spawn_buzz_buzz.asm"

.INCLUDE "overworld/door/process_door_interactions.asm"

.INCLUDE "overworld/door_transition.asm"

.INCLUDE "data/unknown/C06E02.asm"

.INCLUDE "overworld/reset_movement_state.asm"

.INCLUDE "overworld/door/start_escalator_movement.asm"

.INCLUDE "overworld/door/finish_escalator_movement.asm"

.INCLUDE "overworld/door/handle_escalator_movement-jp.asm"

.INCLUDE "overworld/door/handle_stairs_enter.asm"

.INCLUDE "overworld/door/handle_stairs_leave.asm"

.INCLUDE "overworld/door/get_stairs_movement_direction.asm"

.INCLUDE "overworld/door/handle_stairs_movement.asm"

.INCLUDE "overworld/disable_hotspot.asm"

.INCLUDE "overworld/reload_hotspots.asm"

.INCLUDE "overworld/activate_hotspot.asm"

.INCLUDE "overworld/check_hotspot_exit.asm"

.INCLUDE "overworld/door/find_door_at_position.asm"

.INCLUDE "overworld/door/process_door_at_tile.asm"

.INCLUDE "overworld/process_queued_interactions.asm"

.INCLUDE "overworld/party/initialize_party_member_animations.asm"

.INCLUDE "overworld/party/schedule_party_animation_reset.asm"

.INCLUDE "overworld/entity/create_mini_ghost_entity.asm"

.INCLUDE "overworld/entity/destroy_mini_ghost_entity.asm"

.INCLUDE "overworld/update_mini_ghost_position.asm"

.INCLUDE "overworld/party/get_party_member_sprite_id.asm"

.INCLUDE "overworld/entity/get_sprite_variant_from_flags.asm"

.INCLUDE "overworld/apply_possession_overlay_flag.asm"

.INCLUDE "overworld/party/update_party_entity_graphics.asm"

.INCLUDE "overworld/party/refresh_party_entities.asm"

.INCLUDE "overworld/party/clear_party_sprite_hide_flags.asm"

.SEGMENT "BANK00B"

.INCLUDE "system/reset.asm"

.INCLUDE "system/reset_vector.asm"

.INCLUDE "system/nmi_vector.asm"

.INCLUDE "system/irq_vector.asm"

.INCLUDE "system/irq_nmi.asm"

.INCLUDE "system/test_sram_size.asm"

.INCLUDE "intro/clear_demo_recording_flags.asm"

.INCLUDE "intro/start_demo_recording.asm"

.INCLUDE "intro/start_demo_playback.asm"

.INCLUDE "system/read_joypad.asm"

.INCLUDE "intro/record_demo_input.asm"

.INCLUDE "overworld/update_joypad_state.asm"

.INCLUDE "system/process_sfx_queue.asm"

.INCLUDE "system/execute_irq_callback.asm"

.INCLUDE "system/default_irq_callback.asm"

.INCLUDE "system/set_irq_callback.asm"

.INCLUDE "system/reset_irq_callback.asm"

.INCLUDE "system/palette/transfer_palette_to_ram.asm"

.INCLUDE "system/palette/set_palette_upload_mode.asm"

.INCLUDE "system/dma/process_vram_transfer_list.asm"

.INCLUDE "system/transfer_to_vram.asm"

.INCLUDE "system/prepare_vram_copy.asm"

.INCLUDE "system/copy_to_vram_redirect.asm"

.INCLUDE "system/copy_to_vram.asm"

.INCLUDE "system/sbrk.asm"

.INCLUDE "system/enable_nmi_joypad.asm"

.INCLUDE "overworld/force_blank_and_wait_vblank.asm"

.INCLUDE "overworld/blank_screen_and_wait_vblank.asm"

.INCLUDE "system/wait_until_next_frame.asm"

.INCLUDE "overworld/wait_frames.asm"

.INCLUDE "system/set_inidisp_far.asm"

.INCLUDE "system/set_inidisp.asm"

.INCLUDE "system/palette/update_mosaic_from_brightness_redirect.asm"

.INCLUDE "system/palette/update_mosaic_from_brightness.asm"

.INCLUDE "system/fade_in_with_mosaic.asm"

.INCLUDE "system/fade_out_with_mosaic.asm"

.INCLUDE "system/fade_in.asm"

.INCLUDE "system/fade_out.asm"

.INCLUDE "system/palette/wait_for_fade_complete.asm"

.INCLUDE "overworld/entity/swap_spritemap_bank.asm"

.INCLUDE "system/oam_clear.asm"

.INCLUDE "overworld/clear_oam_and_update_screen.asm"

.INCLUDE "overworld/entity/render_all_priority_sprites.asm"

.INCLUDE "overworld/entity/priority_sprite_hook.asm"

.INCLUDE "overworld/redirect_c08c58.asm"

.INCLUDE "overworld/entity/dispatch_sprite_draw_by_priority.asm"

.INCLUDE "data/C08C58_jumps.asm"

.INCLUDE "overworld/entity/queue_priority_0_sprite.asm"

.INCLUDE "overworld/entity/queue_priority_1_sprite.asm"

.INCLUDE "overworld/entity/queue_priority_2_sprite.asm"

.INCLUDE "overworld/entity/queue_priority_3_sprite.asm"

.INCLUDE "overworld/entity/write_spritemap_to_oam.asm"

.INCLUDE "overworld/set_bgmode.asm"

.INCLUDE "system/set_oam_size.asm"

.INCLUDE "system/set_bg1_vram_location.asm"

.INCLUDE "system/set_bg2_vram_location.asm"

.INCLUDE "system/set_bg3_vram_location.asm"

.INCLUDE "system/set_bg4_vram_location.asm"

.INCLUDE "system/math/rand.asm"

.INCLUDE "system/memcpy16.asm"

.INCLUDE "system/memcpy24.asm"

.INCLUDE "system/memset16.asm"

.INCLUDE "system/memset24.asm"

.INCLUDE "system/strlen.asm"

.INCLUDE "system/strcmp.asm"

.INCLUDE "system/setjmp.asm"

.INCLUDE "system/longjmp.asm"

.INCLUDE "data/palette_dma_parameters.asm"

.INCLUDE "data/dma_table.asm"

.INCLUDE "data/unknown/C08FC2.asm"

.INCLUDE "system/math/mult8.asm"

.INCLUDE "system/math/mult168.asm"

.INCLUDE "system/math/mult16.asm"

.INCLUDE "system/math/mult32.asm"

.INCLUDE "system/math/division8.asm"

.INCLUDE "system/math/division16.asm"

.INCLUDE "system/math/division32.asm"

.INCLUDE "system/math/division8s.asm"

.INCLUDE "system/math/division16s.asm"

.INCLUDE "system/math/division32s.asm"

.INCLUDE "system/math/modulus8s.asm"

.INCLUDE "system/math/modulus16s.asm"

.INCLUDE "system/math/modulus32s.asm"

.INCLUDE "system/math/modulus8.asm"

.INCLUDE "system/math/modulus16.asm"

.INCLUDE "system/math/modulus32.asm"

.INCLUDE "system/math/asl16.asm"

.INCLUDE "system/math/asl32.asm"

.INCLUDE "system/math/asr8.asm"

.INCLUDE "system/math/asr16.asm"

.INCLUDE "system/math/asr32.asm"

.INCLUDE "overworld/jump_temp_function_pointer.asm"

.INCLUDE "overworld/init_entity_system-jp.asm"

.INCLUDE "overworld/init_entity.asm"

.INCLUDE "overworld/disable_all_entities.asm"

.INCLUDE "overworld/enable_all_entities.asm"

.INCLUDE "overworld/actionscript/run_actionscript_frame.asm"

.INCLUDE "overworld/entity/run_entity_scripts_and_tick.asm"

.INCLUDE "overworld/execute_movement_script.asm"

.INCLUDE "data/movement_control_codes_pointer_table.asm"

.INCLUDE "overworld/actionscript/script/00.asm"

.INCLUDE "overworld/actionscript/script/01.asm"

.INCLUDE "overworld/actionscript/script/24.asm"

.INCLUDE "overworld/actionscript/script/02.asm"

.INCLUDE "overworld/actionscript/script/19.asm"

.INCLUDE "overworld/actionscript/script/03.asm"

.INCLUDE "overworld/actionscript/script/1A.asm"

.INCLUDE "overworld/actionscript/script/1B.asm"

.INCLUDE "overworld/actionscript/script/04.asm"

.INCLUDE "overworld/actionscript/script/05.asm"

.INCLUDE "overworld/actionscript/script/06.asm"

.INCLUDE "overworld/actionscript/script/3B_45.asm"

.INCLUDE "overworld/actionscript/script/28.asm"

.INCLUDE "overworld/actionscript/script/29.asm"

.INCLUDE "overworld/actionscript/script/2A.asm"

.INCLUDE "overworld/actionscript/script/3F_49.asm"

.INCLUDE "overworld/actionscript/script/40_4A.asm"

.INCLUDE "overworld/actionscript/script/41_4B.asm"

.INCLUDE "overworld/actionscript/script/2E.asm"

.INCLUDE "overworld/actionscript/script/2F.asm"

.INCLUDE "overworld/actionscript/script/30.asm"

.INCLUDE "overworld/actionscript/script/31.asm"

.INCLUDE "overworld/actionscript/script/32.asm"

.INCLUDE "overworld/actionscript/script/33.asm"

.INCLUDE "overworld/actionscript/script/34.asm"

.INCLUDE "overworld/actionscript/script/35.asm"

.INCLUDE "overworld/actionscript/script/36.asm"

.INCLUDE "overworld/actionscript/script/2B.asm"

.INCLUDE "overworld/actionscript/script/2C.asm"

.INCLUDE "overworld/actionscript/script/2D.asm"

.INCLUDE "overworld/actionscript/script/37.asm"

.INCLUDE "overworld/actionscript/script/38.asm"

.INCLUDE "overworld/actionscript/script/39.asm"

.INCLUDE "overworld/entity/clear_entity_delta_motion.asm"

.INCLUDE "overworld/actionscript/script/3A.asm"

.INCLUDE "overworld/actionscript/script/43.asm"

.INCLUDE "overworld/actionscript/script/42_4C.asm"

.INCLUDE "overworld/actionscript/script/0A.asm"

.INCLUDE "overworld/actionscript/script/0B.asm"

.INCLUDE "overworld/actionscript/script/10.asm"

.INCLUDE "overworld/actionscript/script/11.asm"

.INCLUDE "overworld/actionscript/script/0C.asm"

.INCLUDE "overworld/actionscript/script/07.asm"

.INCLUDE "overworld/actionscript/script/13.asm"

.INCLUDE "overworld/actionscript/script/08.asm"

.INCLUDE "overworld/actionscript/script/09.asm"

.INCLUDE "overworld/actionscript/script/3C_46.asm"

.INCLUDE "overworld/actionscript/script/3D_47.asm"

.INCLUDE "overworld/actionscript/script/3E_48.asm"

.INCLUDE "overworld/actionscript/script/18.asm"

.INCLUDE "overworld/actionscript/script/14.asm"

.INCLUDE "overworld/actionscript/script/27.asm"

.INCLUDE "overworld/actionscript/script/0D.asm"

.INCLUDE "data/unknown/C09ABD.asm"

.INCLUDE "overworld/entity/entity_var_op_and.asm"

.INCLUDE "overworld/entity/entity_var_op_or.asm"

.INCLUDE "overworld/entity/entity_var_op_add.asm"

.INCLUDE "overworld/entity/entity_var_op_xor.asm"

.INCLUDE "overworld/actionscript/script/0E.asm"

.INCLUDE "data/events/entity_script_var_tables.asm"

.INCLUDE "overworld/actionscript/script/0F.asm"

.INCLUDE "overworld/actionscript/script/12.asm"

.INCLUDE "overworld/actionscript/script/15.asm"

.INCLUDE "overworld/actionscript/script/16.asm"

.INCLUDE "overworld/actionscript/script/17.asm"

.INCLUDE "overworld/actionscript/script/1C.asm"

.INCLUDE "overworld/actionscript/script/1D.asm"

.INCLUDE "overworld/actionscript/script/1E.asm"

.INCLUDE "overworld/actionscript/script/1F.asm"

.INCLUDE "overworld/actionscript/script/20.asm"

.INCLUDE "overworld/actionscript/script/44.asm"

.INCLUDE "overworld/actionscript/script/21.asm"

.INCLUDE "overworld/actionscript/script/26.asm"

.INCLUDE "overworld/actionscript/script/22.asm"

.INCLUDE "overworld/actionscript/script/23.asm"

.INCLUDE "overworld/actionscript/script/25.asm"

.INCLUDE "overworld/allocate_entity_slot.asm"

.INCLUDE "overworld/entity/deactivate_entity_far.asm"

.INCLUDE "overworld/deactivate_entity.asm"

.INCLUDE "overworld/link_entity_to_list.asm"

.INCLUDE "overworld/unlink_entity_from_list.asm"

.INCLUDE "overworld/append_entity_to_list.asm"

.INCLUDE "overworld/free_entity_scripts.asm"

.INCLUDE "overworld/entity/find_entity_predecessor.asm"

.INCLUDE "overworld/entity/rebuild_entity_free_list.asm"

.INCLUDE "overworld/allocate_entity_script_slot.asm"

.INCLUDE "overworld/free_action_script.asm"

.INCLUDE "overworld/unlink_action_script.asm"

.INCLUDE "overworld/find_previous_action_script.asm"

.INCLUDE "overworld/get_action_script_index.asm"

.INCLUDE "overworld/get_last_action_script.asm"

.INCLUDE "overworld/actionscript/script/read8.asm"

.INCLUDE "overworld/actionscript/script/read8_copy.asm"

.INCLUDE "overworld/actionscript/script/read16.asm"

.INCLUDE "overworld/actionscript/script/read16_copy.asm"

.INCLUDE "overworld/actionscript/jump_to_loaded_movement_pointer.asm"

.INCLUDE "overworld/actionscript/clear_sprite_tick_callback.asm"

.INCLUDE "overworld/entity/create_entity_from_stream.asm"

.INCLUDE "overworld/entity/spawn_entity.asm"

.INCLUDE "overworld/entity/deactivate_entity_from_var.asm"

.INCLUDE "overworld/deactivate_all_entities_except_current.asm"

.INCLUDE "overworld/deactivate_entities_by_script_id.asm"

.INCLUDE "overworld/entity/assign_entity_script_from_stream.asm"

.INCLUDE "overworld/calculate_prospective_position.asm"

.INCLUDE "overworld/entity/disable_all_entity_callbacks.asm"

.INCLUDE "overworld/entity/restore_entity_callback_flags.asm"

.INCLUDE "overworld/actionscript/choose_random.asm"

.INCLUDE "overworld/rand_high_byte.asm"

.INCLUDE "overworld/actionscript/fade_in.asm"

.INCLUDE "overworld/actionscript/fade_out.asm"

.INCLUDE "overworld/entity/apply_entity_delta_position_entry2.asm"

.INCLUDE "overworld/entity/apply_entity_delta_position_3d.asm"

.INCLUDE "overworld/entity/update_entity_z_position.asm"

.INCLUDE "overworld/entity/entity_screen_coords_bg1.asm"

.INCLUDE "overworld/entity/entity_screen_coords_bg1_with_z.asm"

.INCLUDE "overworld/entity/entity_world_to_screen.asm"

.INCLUDE "overworld/entity/entity_coords_relative_to_bg3.asm"

.INCLUDE "overworld/camera/entity_add_camera_offset.asm"

.INCLUDE "overworld/entity/entity_screen_coords_bg3_with_z.asm"

.INCLUDE "overworld/entity/copy_entity_abs_to_screen.asm"

.INCLUDE "overworld/entity/call_entity_draw_callback.asm"

.INCLUDE "overworld/entity/draw_entity.asm"

.INCLUDE "intro/draw_title_letter.asm"

.INCLUDE "system/check_hardware.asm"

.INCLUDE "overworld/map/get_map_block_cached_redirect.asm"

.INCLUDE "overworld/map/get_map_block_cached.asm"

.INCLUDE "data/unknown/C0A1AE.asm"

.INCLUDE "overworld/get_map_tile_attribute.asm"

.INCLUDE "system/palette/load_bg_palette.asm"

.INCLUDE "data/unknown/C0A20C.asm"

.INCLUDE "overworld/npc/find_entity_by_npc_id_linked.asm"

.INCLUDE "overworld/entity/add_entity_position_offset.asm"

.INCLUDE "overworld/entity/entity_screen_coords_by_id.asm"

.INCLUDE "overworld/party/update_party_sprite_position.asm"

.INCLUDE "data/unknown/C0A2AB.asm"

.INCLUDE "overworld/party/check_follower_vertical_distance.asm"

.INCLUDE "overworld/party/check_follower_horizontal_distance.asm"

.INCLUDE "data/unknown/C0A30B.asm"

.INCLUDE "overworld/party/check_follower_diagonal_distance.asm"

.INCLUDE "data/unknown/C0A350.asm"

.INCLUDE "overworld/collision/entity_physics_with_collision.asm"

.INCLUDE "overworld/collision/entity_physics_simple_collision.asm"

.INCLUDE "overworld/entity/draw_entity_sprite.asm"

.INCLUDE "overworld/render_entity_sprite.asm"

.INCLUDE "system/dma/prepare_vram_copy_row_safe.asm"

.INCLUDE "data/sprite_direction_mapping_4_direction.asm"

.INCLUDE "data/sprite_direction_mapping_8_direction.asm"

.INCLUDE "system/math/rand_0_3.asm"

.INCLUDE "system/math/rand_0_7.asm"

.INCLUDE "overworld/movement/movement_cmd_set_npc_id.asm"

.INCLUDE "overworld/actionscript/set_direction8.asm"

.INCLUDE "overworld/actionscript/set_direction.asm"

.INCLUDE "overworld/entity/set_current_entity_direction.asm"

.INCLUDE "overworld/entity/get_entity_direction.asm"

.INCLUDE "overworld/actionscript/set_surface_flags.asm"

.INCLUDE "overworld/entity/set_entity_movement_speed.asm"

.INCLUDE "overworld/entity/get_entity_movement_speed.asm"

.INCLUDE "overworld/movement/movement_cmd_set_direction_velocity.asm"

.INCLUDE "overworld/movement/movement_cmd_calculate_travel_frames.asm"

.INCLUDE "overworld/entity/move_entity_distance.asm"

.INCLUDE "overworld/collision/is_entity_collision_enabled.asm"

.INCLUDE "overworld/collision/get_entity_obstacle_flags.asm"

.INCLUDE "overworld/pathfinding/get_entity_pathfinding_state.asm"

.INCLUDE "overworld/actionscript/disable_current_entity_collision.asm"

.INCLUDE "overworld/actionscript/clear_current_entity_collision.asm"

.INCLUDE "overworld/entity/update_entity_animation.asm"

.INCLUDE "overworld/update_entity_sprite.asm"

.INCLUDE "overworld/entity/render_entity_sprite_8dir.asm"

.INCLUDE "overworld/actionscript/disable_current_entity_collision2.asm"

.INCLUDE "overworld/actionscript/clear_current_entity_collision2.asm"

.INCLUDE "overworld/movement/movement_cmd_play_sound.asm"

.INCLUDE "overworld/movement/movement_cmd_get_event_flag.asm"

.INCLUDE "overworld/movement/movement_cmd_set_event_flag.asm"

.INCLUDE "overworld/movement/movement_cmd_copy_leader_pos.asm"

.INCLUDE "overworld/movement/movement_cmd_copy_sprite_pos.asm"

.INCLUDE "overworld/movement_set_position_from_screen.asm"

.INCLUDE "overworld/movement_queue_interaction.asm"

.INCLUDE "overworld/movement_display_text.asm"

.INCLUDE "overworld/movement_store_offset_position.asm"

.INCLUDE "overworld/move_toward_target_callback.asm"

.INCLUDE "overworld/move_toward_target_reversed_callback.asm"

.INCLUDE "overworld/entity/move_toward_target_no_sprite_callback.asm"

.INCLUDE "overworld/update_direction_velocity_callback.asm"

.INCLUDE "overworld/update_direction_velocity_reversed_callback.asm"

.INCLUDE "overworld/actionscript/prepare_new_entity_at_self.asm"

.INCLUDE "overworld/actionscript/prepare_new_entity_at_party_leader.asm"

.INCLUDE "overworld/actionscript/prepare_new_entity_at_teleport_destination.asm"

.INCLUDE "overworld/actionscript/prepare_new_entity.asm"

.INCLUDE "overworld/movement/movement_cmd_store_npc_position.asm"

.INCLUDE "overworld/movement/movement_cmd_store_sprite_position.asm"

.INCLUDE "overworld/actionscript/get_position_of_party_member.asm"

.INCLUDE "overworld/movement/movement_cmd_face_toward_npc.asm"

.INCLUDE "overworld/movement/movement_cmd_face_toward_sprite.asm"

.INCLUDE "overworld/movement_set_bounding_box.asm"

.INCLUDE "battle/load_battlebg_movement.asm"

.INCLUDE "overworld/movement/movement_cmd_create_entity.asm"

.INCLUDE "overworld/movement/movement_cmd_create_entity_bg3.asm"

.INCLUDE "overworld/movement/movement_cmd_print_cast_name.asm"

.INCLUDE "overworld/party/movement_cmd_print_cast_name_party.asm"

.INCLUDE "overworld/movement/movement_cmd_print_cast_name_var0.asm"

.INCLUDE "overworld/actionscript/fade_out_with_mosaic.asm"

.INCLUDE "overworld/movement/movement_cmd_setup_spotlight.asm"

.INCLUDE "overworld/movement/movement_cmd_apply_color_math_fixed.asm"

.INCLUDE "overworld/entity/set_entity_direction_and_frame.asm"

.INCLUDE "overworld/entity/force_render_entity_sprite.asm"

.INCLUDE "overworld/movement/movement_cmd_animate_palette_fade.asm"

.INCLUDE "overworld/movement/movement_cmd_return_2.asm"

.INCLUDE "overworld/movement/movement_cmd_return_4.asm"

.INCLUDE "overworld/movement/movement_cmd_loop.asm"

.INCLUDE "overworld/movement/movement_cmd_clear_loop_counter.asm"

.INCLUDE "audio/load_spc700_data.asm"

.INCLUDE "audio/wait_for_spc700.asm"

.INCLUDE "overworld/write_apu_port0.asm"

.INCLUDE "audio/stop_music.asm"

.INCLUDE "audio/play_sound.asm"

.INCLUDE "overworld/write_apu_port1.asm"

.INCLUDE "overworld/read_apu_io0.asm"

.INCLUDE "data/stereo_mono_data.asm"

.INCLUDE "overworld/write_apu_port2.asm"

.INCLUDE "overworld/entity/draw_entity_overlays.asm"

.INCLUDE "overworld/execute_overlay_animation_script.asm"

.INCLUDE "data/events/scripts/786.asm"

.INCLUDE "overworld/write_bg3_y_scroll.asm"

.INCLUDE "misc/battlebgs/do_battlebg_dma.asm"

.INCLUDE "data/dma_flags.asm"

.INCLUDE "data/dma_target_registers.asm"

.INCLUDE "data/unknown/C0AE26.asm"

.INCLUDE "data/unknown/C0AE2D.asm"

.INCLUDE "system/dma/mask_hdma_channel.asm"

.INCLUDE "data/unknown/C0AE44.asm"

.INCLUDE "misc/battlebgs/load_bg_offset_parameters.asm"

.INCLUDE "misc/battlebgs/load_bg_offset_parameters2.asm"

.INCLUDE "misc/battlebgs/prepare_bg_offset_tables.asm"

.INCLUDE "system/palette/set_color_math_from_table.asm"

.INCLUDE "data/unknown/C0AFF1.asm"

.INCLUDE "system/set_coldata.asm"

.INCLUDE "system/set_colour_addsub_mode.asm"

.INCLUDE "system/set_window_mask.asm"

.INCLUDE "data/unknown/C0B0A6.asm"

.INCLUDE "text/disable_windows.asm"

.INCLUDE "system/dma/setup_hdma_channel.asm"

.INCLUDE "system/dma/setup_swirl_window_hdma.asm"

.INCLUDE "text/generate_oval_window_data.asm"

.INCLUDE "data/unknown/C0B2FF.asm"

.INCLUDE "data/unknown/C0B3FF.asm"

.INCLUDE "system/math/cosine_sine.asm"

.INCLUDE "data/sine_table.asm"

.INCLUDE "system/file_select_init.asm"

.INCLUDE "overworld/place_leader_at_position.asm"

.INCLUDE "overworld/initialize_overworld_state.asm"

.INCLUDE "battle/init_overworld.asm"

.INCLUDE "system/main.asm"

.INCLUDE "system/game_init.asm"

.INCLUDE "overworld/pathfinding/calculate_pathfinding_targets.asm"

.INCLUDE "overworld/pathfinding/initialize_pathfinding_for_entities.asm"

.INCLUDE "misc/find_path_to_party.asm"

.INCLUDE "overworld/pathfinding/pathfind_to_party_leader.asm"

.INCLUDE "overworld/pathfinding/pathfind_to_current_entity.asm"

.INCLUDE "overworld/setup_delivery_path_to_leader.asm"

.INCLUDE "overworld/setup_delivery_path_reverse.asm"

.INCLUDE "overworld/entity/setup_delivery_path_from_entity.asm"

.INCLUDE "overworld/npc/set_npc_direction_from_event_flag.asm"

.INCLUDE "overworld/npc/set_current_npc_direction_from_event_flag.asm"

.INCLUDE "overworld/get_leader_moved_flag.asm"

.INCLUDE "overworld/entity/classify_entity_leader_distance.asm"

.INCLUDE "overworld/entity/classify_entity_leader_distance_short.asm"

.INCLUDE "overworld/entity/check_entity_can_pursue.asm"

.INCLUDE "overworld/entity/check_entity_can_pursue_short.asm"

.INCLUDE "data/unknown/C0C4CF.asm"

.INCLUDE "data/map/opposite_directions.asm"

.INCLUDE "overworld/get_direction_from_player_to_entity.asm"

.INCLUDE "overworld/check_enemy_should_flee.asm"

.INCLUDE "overworld/get_opposite_direction_from_player_to_entity.asm"

.INCLUDE "overworld/entity/choose_entity_direction_to_player.asm"

.INCLUDE "overworld/calculate_direction_to_leader.asm"

.INCLUDE "overworld/actionscript/get_direction_rotated_clockwise.asm"

.INCLUDE "overworld/actionscript/get_direction_turned_randomly_left_or_right.asm"

.INCLUDE "overworld/is_entity_near_leader.asm"

.INCLUDE "overworld/is_entity_on_screen.asm"

.INCLUDE "overworld/entity/check_entity_threshold_distance.asm"

.INCLUDE "overworld/collision/predict_surface_flags.asm"

.INCLUDE "overworld/collision/update_entity_surface_flags.asm"

.INCLUDE "overworld/collision/update_entity_surface_flags_3d.asm"

.INCLUDE "overworld/entity/set_entity_direction_velocity.asm"

.INCLUDE "overworld/calculate_travel_frames.asm"

.INCLUDE "overworld/calculate_movement_duration.asm"

.INCLUDE "overworld/calculate_travel_time_to_target.asm"

.INCLUDE "overworld/init_butterfly_movement.asm"

.INCLUDE "overworld/update_butterfly_movement-jp.asm"

.INCLUDE "overworld/pathfinding/steer_entity_toward_direction.asm"

.INCLUDE "data/unknown/C0CF58.asm"

.INCLUDE "overworld/collision/check_entity_collision_path.asm"

.INCLUDE "overworld/collision/check_entity_collision_ahead.asm"

.INCLUDE "overworld/pathfinding/advance_entity_toward_leader.asm"

.INCLUDE "overworld/collision/check_entity_enemy_collision.asm"

.INCLUDE "overworld/stub_return_zero.asm"

.INCLUDE "overworld/initiate_enemy_encounter-jp.asm"

.INCLUDE "system/palette/desaturate_palettes.asm"

.INCLUDE "overworld/is_battle_pending.asm"

.INCLUDE "overworld/handle_enemy_contact.asm"

.INCLUDE "overworld/entity/disable_other_entity_updates.asm"

.INCLUDE "overworld/entity/save_entity_position.asm"

.INCLUDE "overworld/entity/restore_entity_actionscript_position.asm"

.INCLUDE "overworld/pathfinding/reset_entity_pathfinding.asm"

.INCLUDE "overworld/pathfinding/entity_pathfinding_step.asm"

.INCLUDE "overworld/pathfinding/advance_entity_path_point.asm"

.INCLUDE "overworld/entity/sort_entity_draw_order.asm"

.INCLUDE "overworld/entity/build_entity_draw_list.asm"

.INCLUDE "overworld/schedule_overworld_task.asm"

.INCLUDE "overworld/clear_overworld_task.asm"

.INCLUDE "overworld/process_overworld_tasks.asm"

.INCLUDE "overworld/load_dad_phone.asm"

.INCLUDE "system/palette/run_frames_until_fade_done.asm"

.INCLUDE "overworld/wait_frames_with_updates.asm"

.INCLUDE "overworld/set_teleport_state.asm"

.INCLUDE "overworld/teleport/load_teleport_destination.asm"

.INCLUDE "overworld/teleport/setup_teleport_entity_flags.asm"

.INCLUDE "overworld/teleport/init_psi_teleport_beta.asm"

.INCLUDE "overworld/teleport/reset_entities_after_teleport.asm"

.INCLUDE "overworld/collision/get_combined_surface_flags.asm"

.INCLUDE "overworld/teleport/update_psi_teleport_speed.asm"

.INCLUDE "overworld/record_player_position.asm"

.INCLUDE "overworld/teleport/update_teleport_party_visibility.asm"

.INCLUDE "overworld/teleport/set_teleport_entity_speed_vars.asm"

.INCLUDE "overworld/teleport/psi_teleport_alpha_tick.asm"

.INCLUDE "overworld/party/update_party_entity_from_buffer.asm"

.INCLUDE "overworld/teleport/update_teleport_beta_input.asm"

.INCLUDE "overworld/teleport/set_teleport_velocity_by_direction.asm"

.INCLUDE "overworld/teleport/psi_teleport_beta_tick.asm"

.INCLUDE "overworld/teleport/psi_teleport_success_tick.asm"

.INCLUDE "overworld/teleport/update_party_entity_teleport_tick.asm"

.INCLUDE "overworld/teleport/psi_teleport_decelerate_tick.asm"

.INCLUDE "overworld/teleport/init_teleport_arrival.asm"

.INCLUDE "overworld/teleport/init_teleport_departure.asm"

.INCLUDE "overworld/tick_callback_nop.asm"

.INCLUDE "overworld/entity/update_entity_surface_and_graphics.asm"

.INCLUDE "overworld/teleport/run_teleport_failure_sequence.asm"

.INCLUDE "misc/teleport_freezeobjects.asm"

.INCLUDE "misc/teleport_freezeobjects2.asm"

.INCLUDE "misc/teleport_mainloop.asm"

.INCLUDE "overworld/fill_tile_grid_16x16-jp.asm"

.INCLUDE "intro/load_title_screen_graphics-jp.asm"

.INCLUDE "overworld/entity/cycle_entity_palette_frame-jp.asm"

.INCLUDE "overworld/set_default_tm.asm"

.INCLUDE "overworld/entity/show_current_entity_sprite.asm"

.INCLUDE "intro/show_title_screen-jp.asm"

.INCLUDE "intro/logo_screen_load.asm"

.INCLUDE "overworld/wait_frames_or_until_pressed.asm"

.INCLUDE "intro/logo_screen.asm"

.INCLUDE "intro/gas_station_load.asm"

.INCLUDE "system/palette/prepare_palette_fade_from_current.asm"

.INCLUDE "overworld/run_gas_station_credits.asm"

.INCLUDE "intro/gas_station.asm"

.INCLUDE "intro/load_gas_station_flash_palette.asm"

.INCLUDE "intro/load_gas_station_palette.asm"

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

.INCLUDE "ending/credits_scroll_frame-jp.asm"

.SEGMENT "HEADER"
snes_header:
	.BYTE "01MB  " ;Game Code
	.WORD $0000 ; Padding
	.WORD $0000 ; Padding
	.WORD $0000 ; Padding
	.WORD $0000 ; Padding
	.WORD $0000 ; Padding
	.BYTE "MOTHER-2             " ; Title
	.BYTE $31		; HiROM + FastROM
	.BYTE $02		; ROM only
	.BYTE $0C		; ROM Size = 0x300000
	.BYTE $03		; RAM Size
	.BYTE $00		; Japan
	.BYTE $33		; Licensee Code
	.BYTE $00		; Version
	.WORD $5D3C	; Checksum Complement
	.WORD $A2C3	; Checksum
	.WORD $0000	; Unused
	.WORD $0000	; Unused
	.WORD $5FFF	; Native-mode COP
	.WORD $5FFF	; Native-mode BRK
	.WORD $5FFF	; Native-mode ABORT
	.ADDR NMI_VECTOR	; Native-mode NMI
	.WORD $0000	; Native-mode RESET
	.ADDR IRQ_VECTOR	; Native-mode IRQ
	.WORD $0000	; Unused
	.WORD $0000	; Unused
	.WORD	$5FFF	; Emulation-mode COP
	.WORD $0000	; Unused
	.WORD $5FFF	; Emulation-mode ABORT
	.WORD $5FFF	; Emulation-mode NMI
	.ADDR RESET_VECTOR ; Emulation-mode RESET
	.WORD $5FFF	; Emulation-mode IRQ
