"""Opcode definitions for the EarthBound text bytecode VM.

Each opcode maps a human-readable YAML name to a byte sequence and argument list.
Compressed text opcodes (0x15-0x17) are handled separately by the decoder/compiler
and are not included here.
"""

from dataclasses import dataclass
from enum import Enum, auto


class ArgType(Enum):
    """Argument types for text bytecode opcodes."""

    U8 = auto()
    U16 = auto()
    U24 = auto()
    U32 = auto()
    LABEL = auto()
    FLAG = auto()
    ITEM = auto()
    WINDOW = auto()
    PARTY = auto()
    MUSIC = auto()
    SFX = auto()
    SPRITE = auto()
    MOVEMENT = auto()
    STATUS_GROUP = auto()
    ENEMY_GROUP = auto()
    STRING = auto()
    JUMP_TABLE = auto()


@dataclass(frozen=True)
class ArgSpec:
    """Specification for a single opcode argument."""

    name: str
    type: ArgType


@dataclass(frozen=True)
class OpcodeSpec:
    """Specification for a single text bytecode opcode."""

    yaml_name: str
    bytes: tuple[int, ...] = ()
    args: tuple[ArgSpec, ...] = ()


def _a(name: str, type: ArgType) -> ArgSpec:
    """Shorthand for creating an ArgSpec."""
    return ArgSpec(name=name, type=type)


# fmt: off
OPCODES: list[OpcodeSpec] = [
    # === Primary codes 0x00-0x14 ===
    OpcodeSpec("line_break", (0x00,)),
    OpcodeSpec("start_new_line", (0x01,)),
    OpcodeSpec("end_block", (0x02,)),
    OpcodeSpec("halt_with_prompt", (0x03,)),
    OpcodeSpec("set_event_flag", (0x04,), (_a("flag", ArgType.FLAG),)),
    OpcodeSpec("clear_event_flag", (0x05,), (_a("flag", ArgType.FLAG),)),
    OpcodeSpec("jump_if_flag_set", (0x06,), (_a("flag", ArgType.FLAG), _a("dest", ArgType.LABEL))),
    OpcodeSpec("check_event_flag", (0x07,), (_a("flag", ArgType.FLAG),)),
    OpcodeSpec("call_text", (0x08,), (_a("dest", ArgType.LABEL),)),
    OpcodeSpec("jump_multi", (0x09,), (_a("targets", ArgType.JUMP_TABLE),)),
    OpcodeSpec("jump", (0x0A,), (_a("dest", ArgType.LABEL),)),
    OpcodeSpec("test_if_workmem_true", (0x0B,), (_a("value", ArgType.U8),)),
    OpcodeSpec("test_if_workmem_false", (0x0C,), (_a("value", ArgType.U8),)),
    OpcodeSpec("copy_to_argmem", (0x0D,), (_a("value", ArgType.U8),)),
    OpcodeSpec("store_to_argmem", (0x0E,), (_a("value", ArgType.U8),)),
    OpcodeSpec("increment_workmem", (0x0F,)),
    OpcodeSpec("pause", (0x10,), (_a("frames", ArgType.U8),)),
    OpcodeSpec("create_selection_menu", (0x11,)),
    OpcodeSpec("clear_text_line", (0x12,)),
    OpcodeSpec("halt_without_prompt", (0x13,)),
    OpcodeSpec("halt_with_prompt_always", (0x14,)),

    # === 0x18 xx: Window operations ===
    OpcodeSpec("close_window", (0x18, 0x00)),
    OpcodeSpec("open_window", (0x18, 0x01), (_a("window", ArgType.WINDOW),)),
    OpcodeSpec("save_window_text_attributes", (0x18, 0x02)),
    OpcodeSpec("switch_to_window", (0x18, 0x03), (_a("window_id", ArgType.U8),)),
    OpcodeSpec("close_all_windows", (0x18, 0x04)),
    OpcodeSpec("force_text_alignment", (0x18, 0x05), (_a("x", ArgType.U8), _a("y", ArgType.U8))),
    OpcodeSpec("clear_window", (0x18, 0x06)),
    OpcodeSpec("check_for_inequality", (0x18, 0x07), (_a("address", ArgType.U32), _a("value", ArgType.U8))),
    OpcodeSpec("selection_menu_no_cancel", (0x18, 0x08), (_a("value", ArgType.U24),)),
    OpcodeSpec("selection_menu_allow_cancel", (0x18, 0x09), (_a("value", ArgType.U8),)),
    OpcodeSpec("show_wallet_window", (0x18, 0x0A)),

    # === 0x19 xx: Memory/string operations ===
    OpcodeSpec("load_string_to_memory", (0x19, 0x02), (_a("payload", ArgType.STRING),)),
    OpcodeSpec("clear_loaded_strings", (0x19, 0x04)),
    OpcodeSpec("inflict_status", (0x19, 0x05), (_a("party_member", ArgType.PARTY), _a("status_group", ArgType.STATUS_GROUP), _a("status", ArgType.U8))),
    OpcodeSpec("get_character_number", (0x19, 0x10), (_a("value", ArgType.U8),)),
    OpcodeSpec("get_character_name_letter", (0x19, 0x11), (_a("value", ArgType.U8),)),
    OpcodeSpec("get_escargo_express_item", (0x19, 0x14)),
    OpcodeSpec("get_character_status", (0x19, 0x16), (_a("character", ArgType.U8), _a("status", ArgType.U8))),
    OpcodeSpec("get_exp_for_next_level", (0x19, 0x18), (_a("character", ArgType.U8),)),
    OpcodeSpec("add_item_id_to_work_memory", (0x19, 0x19), (_a("slot", ArgType.U8), _a("index", ArgType.U8))),
    OpcodeSpec("get_escargo_express_item_by_slot", (0x19, 0x1A), (_a("slot", ArgType.U8),)),
    OpcodeSpec("get_window_menu_option_count", (0x19, 0x1B), (_a("value", ArgType.U8),)),
    OpcodeSpec("transfer_item_to_queue", (0x19, 0x1C), (_a("arg1", ArgType.U8), _a("arg2", ArgType.U8))),
    OpcodeSpec("get_queued_item_data", (0x19, 0x1D), (_a("arg1", ArgType.U8), _a("arg2", ArgType.U8))),
    OpcodeSpec("get_current_number", (0x19, 0x1E)),
    OpcodeSpec("get_current_inventory_item", (0x19, 0x1F)),
    OpcodeSpec("get_player_controlled_party_count", (0x19, 0x20)),
    OpcodeSpec("is_item_drink", (0x19, 0x21), (_a("value", ArgType.U8),)),
    OpcodeSpec("get_direction_of_object_from_character", (0x19, 0x22), (_a("character", ArgType.U8), _a("object_type", ArgType.U8), _a("object_id", ArgType.U16))),
    OpcodeSpec("get_direction_of_object_from_npc", (0x19, 0x23), (_a("npc_x", ArgType.U16), _a("npc_y", ArgType.U16), _a("direction", ArgType.U8))),
    OpcodeSpec("get_direction_of_object_from_sprite", (0x19, 0x24), (_a("sprite_x", ArgType.U16), _a("sprite_y", ArgType.U16))),
    OpcodeSpec("is_item_condiment", (0x19, 0x25), (_a("value", ArgType.U8),)),
    OpcodeSpec("set_respawn_point", (0x19, 0x26), (_a("point", ArgType.U8),)),
    OpcodeSpec("resolve_cc_table_data", (0x19, 0x27), (_a("index", ArgType.U8),)),
    OpcodeSpec("get_letter_from_stat", (0x19, 0x28), (_a("stat", ArgType.U8),)),

    # === 0x1A xx: UI menus ===
    OpcodeSpec("party_member_selection_menu_uncancellable", (0x1A, 0x01), (_a("dest1", ArgType.LABEL), _a("dest2", ArgType.LABEL), _a("dest3", ArgType.LABEL), _a("dest4", ArgType.LABEL), _a("arg", ArgType.U8))),
    OpcodeSpec("show_character_inventory", (0x1A, 0x05), (_a("character", ArgType.U8), _a("mode", ArgType.U8))),
    OpcodeSpec("display_shop_menu", (0x1A, 0x06), (_a("shop_id", ArgType.U8),)),
    OpcodeSpec("select_escargo_express_item", (0x1A, 0x07)),
    OpcodeSpec("open_phone_menu", (0x1A, 0x0A)),

    # === 0x1B xx: Memory operations ===
    OpcodeSpec("copy_active_memory_to_storage", (0x1B, 0x00)),
    OpcodeSpec("copy_storage_memory_to_active", (0x1B, 0x01)),
    OpcodeSpec("jump_if_false", (0x1B, 0x02), (_a("dest", ArgType.LABEL),)),
    OpcodeSpec("jump_if_true", (0x1B, 0x03), (_a("dest", ArgType.LABEL),)),
    OpcodeSpec("swap_working_and_arg_memory", (0x1B, 0x04)),
    OpcodeSpec("copy_active_memory_to_working_memory", (0x1B, 0x05)),
    OpcodeSpec("copy_working_memory_to_active_memory", (0x1B, 0x06)),

    # === 0x1C xx: Printing ===
    OpcodeSpec("text_colour_effects", (0x1C, 0x00), (_a("effect", ArgType.U8),)),
    OpcodeSpec("print_stat", (0x1C, 0x01), (_a("stat", ArgType.U8),)),
    OpcodeSpec("print_char_name", (0x1C, 0x02), (_a("character", ArgType.U8),)),
    OpcodeSpec("print_char", (0x1C, 0x03), (_a("char_code", ArgType.U8),)),
    OpcodeSpec("open_hp_pp_windows", (0x1C, 0x04)),
    OpcodeSpec("print_item_name", (0x1C, 0x05), (_a("item", ArgType.ITEM),)),
    OpcodeSpec("print_teleport_destination_name", (0x1C, 0x06), (_a("destination", ArgType.U8),)),
    OpcodeSpec("print_horizontal_text_string", (0x1C, 0x07), (_a("string_id", ArgType.U8),)),
    OpcodeSpec("print_special_gfx", (0x1C, 0x08), (_a("gfx_id", ArgType.U8),)),
    OpcodeSpec("set_number_padding", (0x1C, 0x09)),
    OpcodeSpec("print_number", (0x1C, 0x0A), (_a("address", ArgType.U32),)),
    OpcodeSpec("print_money_amount", (0x1C, 0x0B), (_a("address", ArgType.U32),)),
    OpcodeSpec("print_vertical_text_string", (0x1C, 0x0C), (_a("string_id", ArgType.U8),)),
    OpcodeSpec("print_action_user_name", (0x1C, 0x0D)),
    OpcodeSpec("print_action_target_name", (0x1C, 0x0E)),
    OpcodeSpec("print_action_amount", (0x1C, 0x0F)),
    OpcodeSpec("hint_new_line", (0x1C, 0x11), (_a("width", ArgType.U8),)),
    OpcodeSpec("print_psi_name", (0x1C, 0x12), (_a("psi_id", ArgType.U8),)),
    OpcodeSpec("display_psi_animation", (0x1C, 0x13), (_a("psi_id", ArgType.U8), _a("level", ArgType.U8))),
    OpcodeSpec("load_special", (0x1C, 0x14), (_a("special_id", ArgType.U8),)),
    OpcodeSpec("load_special_for_jump_multi", (0x1C, 0x15), (_a("special_id", ArgType.U8),)),

    # === 0x1D xx: Items/money ===
    OpcodeSpec("give_item_to_character", (0x1D, 0x00), (_a("character", ArgType.U8), _a("item", ArgType.ITEM))),
    OpcodeSpec("take_item_from_character", (0x1D, 0x01), (_a("character", ArgType.U8), _a("item", ArgType.ITEM))),
    OpcodeSpec("get_player_has_inventory_full", (0x1D, 0x02), (_a("character", ArgType.U8),)),
    OpcodeSpec("get_player_has_inventory_room", (0x1D, 0x03), (_a("character", ArgType.U8),)),
    OpcodeSpec("check_if_character_doesnt_have_item", (0x1D, 0x04), (_a("character", ArgType.U8), _a("item", ArgType.ITEM))),
    OpcodeSpec("check_if_character_has_item", (0x1D, 0x05), (_a("character", ArgType.U8), _a("item", ArgType.ITEM))),
    OpcodeSpec("add_to_atm", (0x1D, 0x06), (_a("amount", ArgType.U32),)),
    OpcodeSpec("take_from_atm", (0x1D, 0x07), (_a("amount", ArgType.U32),)),
    OpcodeSpec("add_to_wallet", (0x1D, 0x08), (_a("amount", ArgType.U16),)),
    OpcodeSpec("take_from_wallet", (0x1D, 0x09), (_a("amount", ArgType.U16),)),
    OpcodeSpec("get_buy_price_of_item", (0x1D, 0x0A), (_a("item", ArgType.ITEM),)),
    OpcodeSpec("get_sell_price_of_item", (0x1D, 0x0B), (_a("item", ArgType.ITEM),)),
    OpcodeSpec("escargo_express_item_status", (0x1D, 0x0C), (_a("value", ArgType.U16),)),
    OpcodeSpec("character_has_ailment", (0x1D, 0x0D), (_a("character", ArgType.U8), _a("status_group", ArgType.STATUS_GROUP), _a("status", ArgType.U8))),
    OpcodeSpec("give_item_to_character_b", (0x1D, 0x0E), (_a("character", ArgType.U8), _a("item", ArgType.ITEM))),
    OpcodeSpec("take_item_from_character_2", (0x1D, 0x0F), (_a("value", ArgType.U16),)),
    OpcodeSpec("check_item_equipped", (0x1D, 0x10), (_a("value", ArgType.U16),)),
    OpcodeSpec("check_item_usable_by_slot", (0x1D, 0x11), (_a("value", ArgType.U16),)),
    OpcodeSpec("escargo_express_move", (0x1D, 0x12), (_a("value", ArgType.U16),)),
    OpcodeSpec("deliver_escargo_express_item", (0x1D, 0x13), (_a("value", ArgType.U16),)),
    OpcodeSpec("have_enough_money", (0x1D, 0x14), (_a("amount", ArgType.U32),)),
    OpcodeSpec("put_val_in_argmem", (0x1D, 0x15), (_a("value", ArgType.U16),)),
    OpcodeSpec("have_enough_money_in_atm", (0x1D, 0x17), (_a("amount", ArgType.U32),)),
    OpcodeSpec("escargo_express_store", (0x1D, 0x18), (_a("value", ArgType.U8),)),
    OpcodeSpec("have_x_party_members", (0x1D, 0x19), (_a("count", ArgType.U8),)),
    OpcodeSpec("test_is_user_targetting_self", (0x1D, 0x20)),
    OpcodeSpec("generate_random_number", (0x1D, 0x21), (_a("max_value", ArgType.U8),)),
    OpcodeSpec("test_if_exit_mouse_usable", (0x1D, 0x22)),
    OpcodeSpec("get_item_category", (0x1D, 0x23), (_a("value", ArgType.U8),)),
    OpcodeSpec("get_game_state_c4", (0x1D, 0x24), (_a("value", ArgType.U8),)),

    # === 0x1E xx: Stat modifications ===
    OpcodeSpec("recover_hp_percent", (0x1E, 0x00), (_a("character", ArgType.U8), _a("percent", ArgType.U8))),
    OpcodeSpec("deplete_hp_percent", (0x1E, 0x01), (_a("character", ArgType.U8), _a("percent", ArgType.U8))),
    OpcodeSpec("recover_hp_amount", (0x1E, 0x02), (_a("character", ArgType.U8), _a("amount", ArgType.U8))),
    OpcodeSpec("deplete_hp_amount", (0x1E, 0x03), (_a("character", ArgType.U8), _a("amount", ArgType.U8))),
    OpcodeSpec("recover_pp_percent", (0x1E, 0x04), (_a("character", ArgType.U8), _a("percent", ArgType.U8))),
    OpcodeSpec("deplete_pp_percent", (0x1E, 0x05), (_a("character", ArgType.U8), _a("percent", ArgType.U8))),
    OpcodeSpec("recover_pp_amount", (0x1E, 0x06), (_a("character", ArgType.U8), _a("amount", ArgType.U8))),
    OpcodeSpec("deplete_pp_amount", (0x1E, 0x07), (_a("character", ArgType.U8), _a("amount", ArgType.U8))),
    OpcodeSpec("set_character_level", (0x1E, 0x08), (_a("character", ArgType.U8), _a("level", ArgType.U8))),
    OpcodeSpec("give_experience", (0x1E, 0x09), (_a("character", ArgType.U8), _a("amount", ArgType.U24))),
    OpcodeSpec("boost_iq", (0x1E, 0x0A), (_a("character", ArgType.U8), _a("amount", ArgType.U16))),
    OpcodeSpec("boost_guts", (0x1E, 0x0B), (_a("character", ArgType.U8), _a("amount", ArgType.U16))),
    OpcodeSpec("boost_speed", (0x1E, 0x0C), (_a("character", ArgType.U8), _a("amount", ArgType.U16))),
    OpcodeSpec("boost_vitality", (0x1E, 0x0D), (_a("character", ArgType.U8), _a("amount", ArgType.U16))),
    OpcodeSpec("boost_luck", (0x1E, 0x0E), (_a("character", ArgType.U8), _a("amount", ArgType.U16))),

    # === 0x1F xx: Entity/world operations ===
    OpcodeSpec("play_music", (0x1F, 0x00), (_a("flag", ArgType.U8), _a("track", ArgType.MUSIC))),
    OpcodeSpec("stop_music", (0x1F, 0x01), (_a("fade_time", ArgType.U8),)),
    OpcodeSpec("play_sound", (0x1F, 0x02), (_a("sfx", ArgType.SFX),)),
    OpcodeSpec("restore_default_music", (0x1F, 0x03)),
    OpcodeSpec("set_text_printing_sound", (0x1F, 0x04), (_a("sound", ArgType.U8),)),
    OpcodeSpec("disable_sector_music_change", (0x1F, 0x05)),
    OpcodeSpec("enable_sector_music_change", (0x1F, 0x06)),
    OpcodeSpec("apply_music_effect", (0x1F, 0x07), (_a("effect", ArgType.U8),)),
    OpcodeSpec("add_party_member", (0x1F, 0x11), (_a("member", ArgType.PARTY),)),
    OpcodeSpec("remove_party_member", (0x1F, 0x12), (_a("member", ArgType.PARTY),)),
    OpcodeSpec("change_character_direction", (0x1F, 0x13), (_a("character", ArgType.U8), _a("direction", ArgType.U8))),
    OpcodeSpec("change_party_direction", (0x1F, 0x14), (_a("direction", ArgType.U8),)),
    OpcodeSpec("generate_active_sprite", (0x1F, 0x15), (_a("sprite", ArgType.SPRITE), _a("movement", ArgType.MOVEMENT), _a("arg", ArgType.U8))),
    OpcodeSpec("change_tpt_entry_direction", (0x1F, 0x16), (_a("tpt_entry", ArgType.U16), _a("direction", ArgType.U8))),
    OpcodeSpec("create_entity", (0x1F, 0x17), (_a("entity_id", ArgType.U16), _a("movement", ArgType.MOVEMENT), _a("arg", ArgType.U8))),
    OpcodeSpec("create_floating_sprite_near_tpt_entry", (0x1F, 0x1A), (_a("tpt_entry", ArgType.U16), _a("sprite_type", ArgType.U8))),
    OpcodeSpec("delete_floating_sprite_near_tpt_entry", (0x1F, 0x1B), (_a("tpt_entry", ArgType.U16),)),
    OpcodeSpec("create_floating_sprite_near_character", (0x1F, 0x1C), (_a("character", ArgType.U8), _a("sprite_type", ArgType.U8))),
    OpcodeSpec("delete_floating_sprite_near_character", (0x1F, 0x1D), (_a("character", ArgType.U8),)),
    OpcodeSpec("delete_tpt_instance", (0x1F, 0x1E), (_a("tpt_entry", ArgType.U16), _a("arg", ArgType.U8))),
    OpcodeSpec("delete_generated_sprite", (0x1F, 0x1F), (_a("sprite", ArgType.SPRITE), _a("arg", ArgType.U8))),
    OpcodeSpec("trigger_psi_teleport", (0x1F, 0x20), (_a("type", ArgType.U8), _a("destination", ArgType.U8))),
    OpcodeSpec("teleport_to", (0x1F, 0x21), (_a("destination", ArgType.U8),)),
    OpcodeSpec("trigger_battle", (0x1F, 0x23), (_a("enemy_group", ArgType.ENEMY_GROUP),)),
    OpcodeSpec("use_normal_font", (0x1F, 0x30)),
    OpcodeSpec("use_mr_saturn_font", (0x1F, 0x31)),
    OpcodeSpec("trigger_event", (0x1F, 0x41), (_a("event", ArgType.U8),)),
    OpcodeSpec("disable_controller_input", (0x1F, 0x50)),
    OpcodeSpec("enable_controller_input", (0x1F, 0x51)),
    OpcodeSpec("create_number_selector", (0x1F, 0x52), (_a("max_digits", ArgType.U8),)),
    OpcodeSpec("text_speed_delay", (0x1F, 0x60)),
    OpcodeSpec("trigger_movement_code", (0x1F, 0x61)),
    OpcodeSpec("enable_blinking_triangle", (0x1F, 0x62), (_a("enabled", ArgType.U8),)),
    OpcodeSpec("screen_reload_ptr", (0x1F, 0x63), (_a("dest", ArgType.LABEL),)),
    OpcodeSpec("delete_all_npcs", (0x1F, 0x64)),
    OpcodeSpec("delete_first_npc", (0x1F, 0x65)),
    OpcodeSpec("activate_hotspot", (0x1F, 0x66), (_a("slot", ArgType.U8), _a("hotspot", ArgType.U8), _a("callback", ArgType.LABEL))),
    OpcodeSpec("deactivate_hotspot", (0x1F, 0x67), (_a("slot", ArgType.U8),)),
    OpcodeSpec("store_coordinates_to_memory", (0x1F, 0x68)),
    OpcodeSpec("teleport_to_stored_coordinates", (0x1F, 0x69)),
    OpcodeSpec("realize_psi", (0x1F, 0x71), (_a("character", ArgType.U8), _a("psi_id", ArgType.U8))),
    OpcodeSpec("equip_item_to_character", (0x1F, 0x83), (_a("character", ArgType.U8), _a("item_slot", ArgType.U8))),
    OpcodeSpec("set_tpt_direction_up", (0x1F, 0xA0)),
    OpcodeSpec("set_tpt_direction_down", (0x1F, 0xA1)),
    OpcodeSpec("get_interacting_event_flag", (0x1F, 0xA2)),
    OpcodeSpec("save_game", (0x1F, 0xB0)),
    OpcodeSpec("jump_multi2", (0x1F, 0xC0), (_a("targets", ArgType.JUMP_TABLE),)),
    OpcodeSpec("try_fix_item", (0x1F, 0xD0), (_a("value", ArgType.U8),)),
    OpcodeSpec("get_direction_of_nearby_truffle", (0x1F, 0xD1)),
    OpcodeSpec("summon_wandering_photographer", (0x1F, 0xD2), (_a("value", ArgType.U8),)),
    OpcodeSpec("trigger_timed_event", (0x1F, 0xD3), (_a("event", ArgType.U8),)),
    OpcodeSpec("change_map_palette", (0x1F, 0xE1), (_a("palette_id", ArgType.U16), _a("fade_speed", ArgType.U8))),
    OpcodeSpec("change_generated_sprite_direction", (0x1F, 0xE4), (_a("sprite_id", ArgType.U16), _a("direction", ArgType.U8))),
    OpcodeSpec("set_player_lock", (0x1F, 0xE5), (_a("lock", ArgType.U8),)),
    OpcodeSpec("delay_tpt_appearance", (0x1F, 0xE6), (_a("tpt_entry", ArgType.U16),)),
    OpcodeSpec("disable_sprite_movement", (0x1F, 0xE7), (_a("sprite_id", ArgType.U16),)),
    OpcodeSpec("restrict_player_movement_when_camera_repositioned", (0x1F, 0xE8), (_a("value", ArgType.U8),)),
    OpcodeSpec("enable_npc_movement", (0x1F, 0xE9), (_a("tpt_entry", ArgType.U16),)),
    OpcodeSpec("enable_sprite_movement", (0x1F, 0xEA), (_a("sprite_id", ArgType.U16),)),
    OpcodeSpec("make_invisible", (0x1F, 0xEB), (_a("type", ArgType.U8), _a("id", ArgType.U8))),
    OpcodeSpec("make_visible", (0x1F, 0xEC), (_a("type", ArgType.U8), _a("id", ArgType.U8))),
    OpcodeSpec("restore_movement", (0x1F, 0xED)),
    OpcodeSpec("warp_party_to_tpt_entry", (0x1F, 0xEE), (_a("tpt_entry", ArgType.U16),)),
    OpcodeSpec("set_camera_focus_by_sprite_id", (0x1F, 0xEF), (_a("sprite_id", ArgType.U16),)),
    OpcodeSpec("ride_bicycle", (0x1F, 0xF0)),
    OpcodeSpec("set_tpt_movement_code", (0x1F, 0xF1), (_a("tpt_entry", ArgType.U16), _a("movement", ArgType.MOVEMENT))),
    OpcodeSpec("set_sprite_movement_code", (0x1F, 0xF2), (_a("sprite", ArgType.SPRITE), _a("movement", ArgType.MOVEMENT))),
    OpcodeSpec("create_floating_sprite_near_entity", (0x1F, 0xF3), (_a("entity_id", ArgType.U16), _a("sprite_type", ArgType.U8))),
    OpcodeSpec("delete_floating_sprite_near_entity", (0x1F, 0xF4), (_a("entity_id", ArgType.U16),)),
]
# fmt: on


def _build_lookup_by_name() -> dict[str, OpcodeSpec]:
    """Build a lookup dict mapping yaml_name -> OpcodeSpec."""
    result: dict[str, OpcodeSpec] = {}
    for op in OPCODES:
        if op.yaml_name in result:
            raise ValueError(f"Duplicate opcode name: {op.yaml_name}")
        result[op.yaml_name] = op
    return result


def _build_lookup_by_bytes() -> dict[tuple[int, ...], OpcodeSpec]:
    """Build a lookup dict mapping byte tuple -> OpcodeSpec."""
    result: dict[tuple[int, ...], OpcodeSpec] = {}
    for op in OPCODES:
        if op.bytes in result:
            raise ValueError(f"Duplicate byte sequence: {op.bytes} ({op.yaml_name} vs {result[op.bytes].yaml_name})")
        result[op.bytes] = op
    return result


OPCODE_BY_NAME: dict[str, OpcodeSpec] = _build_lookup_by_name()
OPCODE_BY_BYTES: dict[tuple[int, ...], OpcodeSpec] = _build_lookup_by_bytes()
