"""The ``pack-all`` command: pack all human-friendly assets back to binary.

Reads from src/assets/ and writes packed binaries to an output directory
that mirrors the asm/bin/ layout, so the embed-registry can use them as
overrides.
"""

import sys
from pathlib import Path
from typing import Annotated

from cyclopts import Parameter

from ebtools.config import CommonData, DumpDoc


def pack_all(
    assets_dir: Annotated[Path, Parameter(help="Human-friendly assets directory (e.g. src/assets/)")] = Path(
        "src/assets"
    ),
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binaries")] = Path("src/packed_assets"),
    *,
    bin_dir: Annotated[Path, Parameter(alias="-b", help="Extracted binary assets (e.g. asm/bin/)")] = Path("asm/bin"),
    yaml_config: Annotated[Path, Parameter(alias="-y", help="Dump doc YAML")] = Path("earthbound.yml"),
    commondata: Annotated[Path, Parameter(alias="-c", help="Common data definitions")] = Path("commondefs.yml"),
) -> None:
    """Pack all human-friendly assets from src/assets/ back to binary format.

    Output mirrors the asm/bin/ directory structure so it can be used as
    an override layer by the embed-registry.
    """
    from ebtools.config import load_common_data, load_dump_doc
    from ebtools.text_dsl.string_table import StringTableBuilder

    doc = load_dump_doc(yaml_config)
    common_data = load_common_data(commondata)

    # Build reverse text table and string table builder for inline text
    reverse_text_table: dict[str, int] = {char: code for code, char in doc.textTable.items()}
    string_table = StringTableBuilder(reverse_text_table)

    output_dir.mkdir(parents=True, exist_ok=True)
    packed = 0

    # --- JSON data tables ---
    json_packers = [
        ("items", "items/items.json", _pack_items, {"doc": doc, "string_table": string_table}),
        (
            "enemies",
            "enemies/enemies.json",
            _pack_enemies,
            {"doc": doc, "common_data": common_data, "string_table": string_table},
        ),
        ("npcs", "data/npc_config.json", _pack_npcs, {"common_data": common_data}),
        ("battle actions", "battle/battle_actions.json", _pack_battle_actions, {"string_table": string_table}),
        ("PSI abilities", "battle/psi_abilities.json", _pack_psi_abilities, {"string_table": string_table}),
        ("teleport destinations", "data/teleport_destinations.json", _pack_teleport, {"common_data": common_data}),
        (
            "PSI teleport destinations",
            "data/psi_teleport_destinations.json",
            _pack_psi_teleport,
            {"doc": doc},
        ),
        ("BG config", "battle/bg_config.json", _pack_bg_config, {}),
        ("EXP table", "data/exp_table.json", _pack_exp_table, {}),
        ("stores", "data/stores.json", _pack_stores, {"common_data": common_data}),
        ("music", "music_tracks.json", _pack_music, {}),
        ("swirls", "swirls/swirls.json", _pack_swirls, {}),
    ]

    # Simple data table packers (JSON -> single .bin)
    simple_packers = [
        (
            "attack type palettes",
            "data/attack_type_palettes.json",
            "data/attack_type_palettes.bin",
            _pack_simple("pack_attack_type_palettes"),
        ),
        (
            "enemy PSI colours",
            "data/enemy_psi_colours.json",
            "data/enemy_psi_colours.bin",
            _pack_simple("pack_enemy_psi_colours"),
        ),
        (
            "misc swirl colours",
            "data/misc_swirl_colours.json",
            "data/misc_swirl_colours.bin",
            _pack_simple("pack_misc_swirl_colours"),
        ),
        (
            "footstep sounds",
            "data/footstep_sounds.json",
            "data/footstep_sound_table.bin",
            _pack_simple("pack_footstep_sounds"),
        ),
        (
            "shake amplitudes",
            "data/shake_amplitudes.json",
            "data/vertical_shake_amplitude_table.bin",
            _pack_simple("pack_shake_amplitudes"),
        ),
        (
            "prayer noise",
            "data/prayer_noise.json",
            "data/final_giygas_prayer_noise_table.bin",
            _pack_simple("pack_prayer_noise"),
        ),
        (
            "stat gain modifiers",
            "data/stat_gain_modifiers.json",
            "data/stat_gain_modifier_table.bin",
            _pack_simple("pack_stat_gain_modifiers"),
        ),
        ("stats growth", "data/stats_growth.json", "data/stats_growth_vars.bin", _pack_simple("pack_stats_growth")),
        (
            "consolation items",
            "data/consolation_items.json",
            "data/consolation_item_table.bin",
            _pack_simple("pack_consolation_items"),
        ),
        ("tileset table", "data/tileset_table.json", "data/tileset_table.bin", _pack_simple("pack_tileset_table")),
        ("hotspots", "data/hotspots.json", "data/hotspot_coordinates.bin", _pack_simple("pack_hotspots")),
        (
            "timed delivery",
            "data/timed_delivery.json",
            "data/timed_delivery_table.bin",
            _pack_simple("pack_timed_delivery"),
        ),
        (
            "timed item transforms",
            "data/timed_item_transforms.json",
            "data/timed_item_transformation_table.bin",
            _pack_simple("pack_timed_item_transforms"),
        ),
        (
            "for-sale signs",
            "data/for_sale_signs.json",
            "data/for_sale_sign_sprite_table.bin",
            _pack_simple("pack_for_sale_signs"),
        ),
        ("battle entry BG", "data/btl_entry_bg.json", "data/btl_entry_bg_table.bin", _pack_simple("pack_btl_entry_bg")),
        ("NPC AI", "data/npc_ai.json", "data/npc_ai_table.bin", _pack_simple("pack_npc_ai")),
        (
            "movement palette",
            "data/movement_palette.json",
            "data/movement_text_string_palette.bin",
            _pack_simple("pack_movement_palette"),
        ),
        (
            "window border anim",
            "data/window_border_anim.json",
            "data/window_border_anim_tiles.bin",
            _pack_simple("pack_window_border_anim"),
        ),
        (
            "per-sector music",
            "data/per_sector_music.json",
            "data/per_sector_music.bin",
            _pack_simple("pack_per_sector_music"),
        ),
        (
            "per-sector attributes",
            "data/per_sector_attributes.json",
            "data/per_sector_attributes.bin",
            _pack_simple("pack_per_sector_u16"),
        ),
        (
            "per-sector town map",
            "data/per_sector_town_map.json",
            "data/per_sector_town_map.bin",
            _pack_simple("pack_per_sector_town_map"),
        ),
        (
            "tileset palette data",
            "data/tileset_palette_data.json",
            "data/global_map_tilesetpalette_data.bin",
            _pack_simple("pack_tileset_palette_data"),
        ),
        (
            "enemy battle groups",
            "data/enemy_battle_groups.json",
            "data/enemy_battle_groups_table.bin",
            _pack_simple("pack_enemy_battle_groups"),
        ),
        # Raw byte-list tables
        ("PSI anim config", "data/psi_anim_cfg.json", "data/psi_anim_cfg.bin", _pack_simple("pack_psi_anim_cfg")),
        (
            "battle sprite ptrs",
            "data/battle_sprites_pointers.json",
            "data/battle_sprites_pointers.bin",
            _pack_simple("pack_battle_sprites_pointers"),
        ),
        (
            "btl entry ptr table",
            "data/btl_entry_ptr_table.json",
            "data/btl_entry_ptr_table.bin",
            _pack_simple("pack_btl_entry_ptr_table"),
        ),
        (
            "sound stone config",
            "data/sound_stone_config.json",
            "data/sound_stone_config.bin",
            _pack_simple("pack_sound_stone_config"),
        ),
        (
            "sound stone melodies",
            "data/sound_stone_melodies.json",
            "data/sound_stone_melodies.bin",
            _pack_simple("pack_sound_stone_melodies"),
        ),
        (
            "status equip tiles",
            "data/status_equip_tile_tables.json",
            "data/status_equip_tile_tables.bin",
            _pack_simple("pack_status_equip_tile_tables"),
        ),
        (
            "status window text",
            "data/status_window_text.json",
            "data/status_window_text.bin",
            _pack_simple("pack_raw_byte_list"),
        ),
        (
            "attract mode txt",
            "data/attract_mode_txt.json",
            "data/attract_mode_txt.bin",
            _pack_simple("pack_attract_mode"),
        ),
        (
            "map enemy placement",
            "data/map_enemy_placement.json",
            "data/map_enemy_placement.bin",
            _pack_simple("pack_map_enemy_placement"),
        ),
        (
            "collision arrangement",
            "data/collision_arrangement_table.json",
            "data/map/collision_arrangement_table.bin",
            _pack_simple("pack_raw_byte_list"),
        ),
        (
            "collision pointers",
            "data/collision_pointers_blob.json",
            "data/map/collision_pointers_blob.bin",
            _pack_simple("pack_raw_byte_list"),
        ),
        (
            "BG distortion",
            "data/bg_distortion_table.json",
            "data/bg_distortion_table.bin",
            _pack_simple("pack_bg_distortion_table"),
        ),
        (
            "BG scrolling",
            "data/bg_scrolling_table.json",
            "data/bg_scrolling_table.bin",
            _pack_simple("pack_bg_scrolling_table"),
        ),
        (
            "Giygas death delays",
            "data/giygas_death_static_transition_delays.json",
            "data/giygas_death_static_transition_delays.bin",
            _pack_simple("pack_giygas_delays"),
        ),
    ]

    # Non-data misc assets (maps, ending, sprites, town map icons, loose files)
    _misc_packers = [
        ("maps/door_config_table.json", "maps/door_config_table.bin"),
        ("maps/door_data.json", "maps/door_data.bin"),
        ("maps/door_pointer_table.json", "maps/door_pointer_table.bin"),
        ("maps/event_control_ptr_table.json", "maps/event_control_ptr_table.bin"),
        ("maps/screen_transition_config.json", "maps/screen_transition_config.bin"),
        ("maps/tile_event_control_table.json", "maps/tile_event_control_table.bin"),
        ("maps/anim_pal_meta_table.json", "maps/anim_pal/meta_table.bin"),
        ("ending/credits_font_pal.json", "ending/credits_font.pal"),
        ("ending/E1E924.json", "ending/E1E924.bin"),
        ("ending/party_cast_tile_ids.json", "ending/party_cast_tile_ids.bin"),
        ("ending/photographer_cfg.json", "ending/photographer_cfg.bin"),
        ("overworld_sprites/entity_overlay_data.json", "overworld_sprites/entity_overlay_data.bin"),
        ("overworld_sprites/spritemap_config.json", "overworld_sprites/spritemap_config.bin"),
        ("town_maps/icon_animation_flags.json", "town_maps/icon_animation_flags.bin"),
        ("town_maps/icon_placement.json", "town_maps/icon_placement.bin"),
        ("town_maps/icon_spritemap_ptrs.json", "town_maps/icon_spritemap_ptrs.bin"),
        ("town_maps/icon_spritemaps.json", "town_maps/icon_spritemaps.bin"),
        ("town_maps/icon_pal.json", "town_maps/icon.pal"),
        ("town_maps/mapping.json", "town_maps/mapping.bin"),
        ("misc/sound_stone_pal.json", "sound_stone.pal"),
        ("misc/sprite_group_palettes.json", "sprite_group_palettes.pal"),
        ("misc/unknown_palette.json", "unknown_palette.pal.lzhal"),
        ("misc/debug_cursor_gfx.json", "debug_cursor.gfx"),
        ("misc/fonts_debug_gfx.json", "fonts/debug.gfx"),
        # Compressed assets
        ("misc/E1CFAF_gfx.json", "E1CFAF.gfx.lzhal"),
        ("misc/E1D4F4_pal.json", "E1D4F4.pal.lzhal"),
        ("misc/E1D5E8_arr.json", "E1D5E8.arr.lzhal"),
        ("ending/E1E94A.json", "ending/E1E94A.bin.lzhal"),
        ("graphics/flavoured_text_gfx.json", "graphics/flavoured_text.gfx.lzhal"),
        ("intro/gas_station2_pal.json", "intro/gas_station2.pal.lzhal"),
        ("intro/attract/nintendo_itoi_pal.json", "intro/attract/nintendo_itoi.pal.lzhal"),
        ("intro/attract/nintendo_presentation_arr.json", "intro/attract/nintendo_presentation.arr.lzhal"),
        ("intro/attract/nintendo_presentation_gfx.json", "intro/attract/nintendo_presentation.gfx.lzhal"),
        ("intro/attract/produced_by_itoi_arr.json", "intro/attract/produced_by_itoi.arr.lzhal"),
        ("intro/attract/produced_by_itoi_gfx.json", "intro/attract/produced_by_itoi.gfx.lzhal"),
    ]

    misc_count = 0
    for json_rel, bin_rel in _misc_packers:
        json_path = assets_dir / json_rel
        if json_path.exists():
            from ebtools.parsers.simple_tables import pack_raw_byte_list

            out = output_dir / bin_rel
            out.parent.mkdir(parents=True, exist_ok=True)
            pack_raw_byte_list(json_path, out)
            misc_count += 1
    if misc_count:
        packed += misc_count
        print(f"  Packed {misc_count} misc assets")

    # --- Locale-specific assets (go to US/ subdirectory) ---
    _locale_packers = [
        ("locale/E1AE7C.json", "US/E1AE7C.bin.lzhal"),
        ("locale/E1AE83.json", "US/E1AE83.bin.lzhal"),
        ("locale/E1AEFD.json", "US/E1AEFD.bin.lzhal"),
        ("locale/E1D6E1_gfx.json", "US/E1D6E1.gfx.lzhal"),
        ("locale/ending/cast_bg_palette.json", "US/ending/cast_bg_palette.pal"),
        ("locale/ending/cast_names_gfx.json", "US/ending/cast_names.gfx.lzhal"),
        ("locale/ending/cast_names_pal.json", "US/ending/cast_names.pal.lzhal"),
        ("locale/ending/cast_sequence_formatting.json", "US/ending/cast_sequence_formatting.bin"),
        ("locale/ending/credits_font_gfx.json", "US/ending/credits_font.gfx.lzhal"),
        ("locale/ending/staff_text.json", "US/ending/staff_text.bin"),
        ("locale/events/bank_c3_scripts.json", "US/events/bank_c3_scripts.bin"),
        ("locale/events/bank_c4_scripts.json", "US/events/bank_c4_scripts.bin"),
        ("locale/events/event_script_pointers.json", "US/events/event_script_pointers.bin"),
        ("locale/events/naming_screen_entities.json", "US/events/naming_screen_entities.bin"),
        ("locale/graphics/sound_stone_gfx.json", "US/graphics/sound_stone.gfx.lzhal"),
        ("locale/graphics/text_window_flavour_palettes.json", "US/graphics/text_window_flavour_palettes.pal"),
        ("locale/graphics/text_window_gfx.json", "US/graphics/text_window.gfx.lzhal"),
        ("locale/intro/title_screen_letters_gfx.json", "US/intro/title_screen_letters.gfx.lzhal"),
        ("locale/intro/title_screen_script_pointers.json", "US/intro/title_screen_script_pointers.bin"),
        ("locale/intro/title_screen_scripts.json", "US/intro/title_screen_scripts.bin"),
        ("locale/intro/title_screen_spritemaps.json", "US/intro/title_screen_spritemaps.bin"),
        ("locale/maps/palettes/1.json", "US/maps/palettes/1.pal"),
        ("locale/town_maps/label_gfx.json", "US/town_maps/label.gfx.lzhal"),
    ]

    locale_count = 0
    for json_rel, bin_rel in _locale_packers:
        json_path = assets_dir / json_rel
        if json_path.exists():
            from ebtools.parsers.simple_tables import pack_raw_byte_list

            out = output_dir / bin_rel
            out.parent.mkdir(parents=True, exist_ok=True)
            pack_raw_byte_list(json_path, out)
            locale_count += 1
    if locale_count:
        packed += locale_count
        print(f"  Packed {locale_count} locale assets")

    # --- Bulk indexed assets (audiopacks, maps, palettes, psianims, etc.) ---
    _bulk_packers = [
        ("audiopacks", "audiopacks", ".ebm"),
        ("overworld_sprites/palettes", "overworld_sprites/palettes", ".pal"),
        ("battle_sprites/palettes", "battle_sprites/palettes", ".pal"),
        ("maps/tiles", "maps/tiles", ".bin"),
        ("maps/palettes", "maps/palettes", ".pal"),
        ("maps/anim_gfx", "maps/anim_gfx", ".gfx.lzhal"),
        ("maps/anim_pal", "maps/anim_pal", ".pal.lzhal"),
        ("maps/anim_props", "maps/anim_props", ".bin"),
        ("psianims/gfx", "psianims/gfx", ".gfx.lzhal"),
        ("psianims/arrangements", "psianims/arrangements", ".arr.lzhal"),
        ("psianims/palettes", "psianims/palettes", ".pal"),
        ("maps/gfx", "maps/gfx", ".gfx.lzhal"),
        ("maps/arrangements", "maps/arrangements", ".arr.lzhal"),
        ("graphics/animations", "graphics/animations", ".anim.lzhal"),
        ("data/events", "data", ".bin"),
        ("errors", "errors", ""),
    ]

    bulk_count = 0
    for json_subdir, bin_subdir, ext in _bulk_packers:
        json_dir = assets_dir / json_subdir
        if not json_dir.exists():
            continue
        for json_file in sorted(json_dir.glob("*.json")):
            from ebtools.parsers.simple_tables import pack_raw_byte_list

            # Reverse the name mangling: foo_bar_ext.json -> foo_bar.ext
            stem = json_file.stem
            if ext:
                # Reconstruct original filename from stem
                # e.g. "0_ebm.json" with ext=".ebm" -> "0.ebm"
                ext_mangled = ext.replace(".", "_").lstrip("_")
                orig_name = stem[: -(len(ext_mangled) + 1)] + ext if stem.endswith("_" + ext_mangled) else stem + ext
            else:
                # For errors/ etc, reconstruct dotted name from underscores
                # This is lossy but best effort
                orig_name = stem
                # Common patterns: foo_gfx_lzhal -> foo.gfx.lzhal
                for suffix in [".gfx.lzhal", ".arr.lzhal", ".pal.lzhal", ".pal"]:
                    mangled = suffix.replace(".", "_")
                    if stem.endswith(mangled):
                        orig_name = stem[: -len(mangled)] + suffix
                        break

            out = output_dir / bin_subdir / orig_name
            out.parent.mkdir(parents=True, exist_ok=True)
            pack_raw_byte_list(json_file, out)
            bulk_count += 1
    if bulk_count:
        packed += bulk_count
        print(f"  Packed {bulk_count} bulk indexed assets")

    # --- PSI arrangement frame bundling ---
    packed += _pack_bundled_arrangements(bin_dir, output_dir)

    # --- Locale bulk assets ---
    _locale_bulk = [
        ("locale/maps/palettes", "US/maps/palettes", ".pal"),
        ("locale/fonts", "US/fonts", ""),
        ("locale/graphics/animations", "US/graphics/animations", ""),
        ("locale/errors", "US/errors", ""),
        ("locale/maps/gfx", "US/maps/gfx", ".gfx.lzhal"),
        ("locale/maps/arrangements", "US/maps/arrangements", ".arr.lzhal"),
        ("locale/maps/tiles", "US/maps/tiles", ".bin"),
        ("locale/town_maps", "US/town_maps", ".bin.lzhal"),
        ("locale/text", "US/text", ".bin"),
    ]

    locale_bulk = 0
    for json_subdir, bin_subdir, ext in _locale_bulk:
        json_dir = assets_dir / json_subdir
        if not json_dir.exists():
            continue
        for json_file in sorted(json_dir.glob("*.json")):
            from ebtools.parsers.simple_tables import pack_raw_byte_list

            stem = json_file.stem
            if ext:
                ext_mangled = ext.replace(".", "_").lstrip("_")
                orig_name = stem[: -(len(ext_mangled) + 1)] + ext if stem.endswith("_" + ext_mangled) else stem + ext
            else:
                orig_name = stem
                for suffix in [".gfx.lzhal", ".arr.lzhal", ".pal.lzhal", ".pal", ".anim.lzhal", ".gfx", ".bin"]:
                    mangled = suffix.replace(".", "_")
                    if stem.endswith(mangled):
                        orig_name = stem[: -len(mangled)] + suffix
                        break

            out = output_dir / bin_subdir / orig_name
            out.parent.mkdir(parents=True, exist_ok=True)
            pack_raw_byte_list(json_file, out)
            locale_bulk += 1

    # Locale misc (tea, mystery_sram)
    for json_rel, bin_rel in [
        ("locale/mystery_sram.json", "US/mystery_sram.bin.lzhal"),
        ("locale/tea.json", "US/tea.bin"),
    ]:
        json_path = assets_dir / json_rel
        if json_path.exists():
            from ebtools.parsers.simple_tables import pack_raw_byte_list

            out = output_dir / bin_rel
            out.parent.mkdir(parents=True, exist_ok=True)
            pack_raw_byte_list(json_path, out)
            locale_bulk += 1

    # Locale flyover + coffee
    for json_file in sorted((assets_dir / "locale").glob("flyover_*.json")):
        from ebtools.parsers.simple_tables import pack_raw_byte_list

        orig_name = json_file.stem.replace("_bin", ".bin")
        out = output_dir / "US" / orig_name
        out.parent.mkdir(parents=True, exist_ok=True)
        pack_raw_byte_list(json_file, out)
        locale_bulk += 1

    coffee_json = assets_dir / "locale" / "coffee.json"
    if coffee_json.exists():
        from ebtools.parsers.simple_tables import pack_raw_byte_list

        out = output_dir / "US" / "coffee.bin"
        out.parent.mkdir(parents=True, exist_ok=True)
        pack_raw_byte_list(coffee_json, out)
        locale_bulk += 1

    if locale_bulk:
        packed += locale_bulk
        print(f"  Packed {locale_bulk} locale bulk assets")

    for label, rel_path, out_rel, pack_fn in simple_packers:
        json_path = assets_dir / rel_path
        if json_path.exists():
            out = output_dir / out_rel
            out.parent.mkdir(parents=True, exist_ok=True)
            pack_fn(json_path, out)
            packed += 1
            print(f"  Packed {label}")

    # EB-text packers (need textTable for encode)
    from ebtools.parsers.simple_tables import (
        pack_eb_string,
        pack_guardian_text,
        pack_psi_names,
        pack_psi_suffixes,
        pack_status_equip_text,
        pack_status_equip_text_8_13,
        pack_telephone_contacts,
    )

    text_table = doc.textTable
    _text_packers = [
        ("PSI names", "data/psi_name_table.json", "data/psi_name_table.bin", pack_psi_names),
        ("PSI suffixes", "data/psi_suffixes.json", "data/psi_suffixes.bin", pack_psi_suffixes),
        ("phone call text", "data/phone_call_text.json", "data/phone_call_text.bin", pack_eb_string),
        (
            "status equip text 7",
            "data/status_equip_window_text_7.json",
            "data/status_equip_window_text_7.bin",
            pack_eb_string,
        ),
        (
            "status equip text 14",
            "data/status_equip_window_text_14.json",
            "data/status_equip_window_text_14.bin",
            pack_eb_string,
        ),
        ("status equip text", "data/status_equip_text.json", "data/status_equip_text.bin", pack_status_equip_text),
        (
            "status equip text 8-13",
            "data/status_equip_window_text_8_13.json",
            "data/status_equip_window_text_8_13.bin",
            pack_status_equip_text_8_13,
        ),
    ]
    for label, rel_path, out_rel, pack_fn in _text_packers:
        json_path = assets_dir / rel_path
        if json_path.exists():
            out = output_dir / out_rel
            out.parent.mkdir(parents=True, exist_ok=True)
            pack_fn(json_path, text_table, out)
            packed += 1
            print(f"  Packed {label}")

    # Locale EB-text packers
    lumine_json = assets_dir / "locale" / "data" / "text" / "lumine_hall_text.json"
    if lumine_json.exists():
        out = output_dir / "US" / "data" / "text" / "lumine_hall_text.bin"
        out.parent.mkdir(parents=True, exist_ok=True)
        pack_eb_string(lumine_json, text_table, out)
        packed += 1
        print("  Packed lumine hall text")
    guardian_json = assets_dir / "locale" / "ending" / "guardian_text.json"
    if guardian_json.exists():
        out = output_dir / "US" / "ending" / "guardian_text.bin"
        out.parent.mkdir(parents=True, exist_ok=True)
        pack_guardian_text(guardian_json, text_table, out)
        packed += 1
        print("  Packed guardian text")

    # --- Dialogue files (MUST run before config packers so addr_remap is available) ---
    addr_remap: dict[int, int] = {}  # original SNES addr → new compiled SNES addr
    dialogue_dir = assets_dir / "dialogue"
    if dialogue_dir.exists():
        addr_remap = _pack_dialogue(dialogue_dir, output_dir, doc, reverse_text_table, common_data)
        packed += 1

    for label, rel_path, func, kwargs in json_packers:
        json_path = assets_dir / rel_path
        if json_path.exists():
            func(json_path, output_dir, addr_remap=addr_remap, **kwargs)  # type: ignore[call-arg]
            packed += 1
            print(f"  Packed {label}")

    # Telephone contacts (text packer that also needs addr_remap for text pointers)
    _telephone_json = assets_dir / "data" / "telephone_contacts_table.json"
    if _telephone_json.exists():
        _telephone_out = output_dir / "data" / "telephone_contacts_table.bin"
        _telephone_out.parent.mkdir(parents=True, exist_ok=True)
        pack_telephone_contacts(_telephone_json, text_table, _telephone_out, addr_remap=addr_remap)
        packed += 1
        print("  Packed telephone contacts")

    # --- Compressed text dictionary (1 JSON -> 2 bin files) ---
    ct_json = assets_dir / "data" / "compressed_text.json"
    if ct_json.exists():
        from ebtools.parsers.simple_tables import pack_compressed_text

        data_out = output_dir / "data" / "compressed_text_data.bin"
        ptrs_out = output_dir / "data" / "compressed_text_ptrs.bin"
        pack_compressed_text(ct_json, text_table, data_out, ptrs_out)
        packed += 1
        print("  Packed compressed text dictionary")

    # --- Entity collision (5 bin files from 1 JSON) ---
    ec_json = assets_dir / "data" / "entity_collision.json"
    if ec_json.exists():
        from ebtools.parsers.simple_tables import pack_entity_collision

        pack_entity_collision(ec_json, output_dir / "data")
        packed += 1
        print("  Packed entity collision tables")

    # --- Pointer+data table pairs ---
    _ptr_pair_packers = [
        (
            "sprite placement",
            "data/sprite_placement.json",
            "data/sprite_placement_ptr_table.bin",
            "data/sprite_placement_table.bin",
            "pack_sprite_placement",
        ),
        (
            "event music",
            "data/event_music.json",
            "data/overworld_event_music_ptr_table.bin",
            "data/overworld_event_music_table.bin",
            "pack_event_music",
        ),
        (
            "enemy placement groups",
            "data/enemy_placement_groups.json",
            "data/enemy_placement_groups_ptr_table.bin",
            "data/enemy_placement_groups.bin",
            "pack_enemy_placement_groups",
        ),
    ]
    for label, rel_path, ptr_rel, data_rel, func_name in _ptr_pair_packers:
        json_path = assets_dir / rel_path
        if json_path.exists():
            import ebtools.parsers.simple_tables as st

            fn = getattr(st, func_name)
            ptr_out = output_dir / ptr_rel
            data_out = output_dir / data_rel
            ptr_out.parent.mkdir(parents=True, exist_ok=True)
            data_out.parent.mkdir(parents=True, exist_ok=True)
            fn(json_path, ptr_out, data_out)
            packed += 1
            print(f"  Packed {label}")

    # --- Fonts ---
    packed += _pack_all_fonts(assets_dir, output_dir)

    # --- Overworld sprites ---
    packed += _pack_all_sprites(assets_dir, bin_dir, output_dir)

    # --- Battle sprites ---
    packed += _pack_all_battle_sprites(assets_dir, bin_dir, output_dir)

    # --- Town maps ---
    packed += _pack_all_town_maps(assets_dir, output_dir)

    # --- Intro/ending graphics ---
    packed += _pack_all_intro_gfx(assets_dir, output_dir)

    # --- Battle backgrounds ---
    packed += _pack_all_battle_bgs(assets_dir, output_dir)

    # --- Inline string table ---
    string_table_data = string_table.build()
    if string_table_data:
        st_out = output_dir / "text" / "inline_strings.bin"
        st_out.parent.mkdir(parents=True, exist_ok=True)
        st_out.write_bytes(string_table_data)
        packed += 1
        print(f"  Packed inline string table ({len(string_table_data)} bytes)")

    # (Dialogue files were packed earlier — see _pack_dialogue call above)

    if packed == 0:
        print("Warning: no assets found to pack", file=sys.stderr)
    else:
        print(f"Packed {packed} asset groups to {output_dir}")


# --- Simple table pack helper ---


def _pack_simple(func_name: str):
    """Return a (json_path, output_path) callable that lazily loads from simple_tables."""

    def _do_pack(json_path: Path, output_path: Path) -> None:
        import ebtools.parsers.simple_tables as st

        fn = getattr(st, func_name)
        fn(json_path, output_path)

    return _do_pack


def _validate_dialogue(
    all_dialogue: list[tuple[Path, dict[str, list[dict]]]],
    flat_label_offsets: dict[str, int],
) -> list[str]:
    """Validate deserialized dialogue data for common errors.

    Checks for:
    1. Unknown opcode names (not in OPCODE_BY_NAME, "text", or "unknown").
    2. Broken label references (string values in LABEL/JUMP_TABLE arg positions
       that don't exist in *flat_label_offsets*).

    Returns a list of human-readable error strings.  An empty list means
    everything is valid.
    """
    from ebtools.text_dsl.opcodes import OPCODE_BY_NAME, ArgType

    errors: list[str] = []
    label_arg_types = {ArgType.LABEL, ArgType.JUMP_TABLE}

    for yaml_file, messages in all_dialogue:
        fname = yaml_file.name
        for label, ops in messages.items():
            for i, entry in enumerate(ops):
                op_name = entry.get("op", "")

                # 1. Unknown opcode names
                if op_name not in ("text", "unknown") and op_name not in OPCODE_BY_NAME:
                    errors.append(f"{fname}: label {label!r}, entry {i}: unknown opcode {op_name!r}")
                    continue  # can't check args for unknown opcodes

                if op_name in ("text", "unknown"):
                    continue

                # 2. Broken label references
                spec = OPCODE_BY_NAME[op_name]
                for arg_spec in spec.args:
                    if arg_spec.type not in label_arg_types:
                        continue
                    value = entry.get(arg_spec.name)
                    if value is None:
                        continue
                    if arg_spec.type == ArgType.JUMP_TABLE:
                        # JUMP_TABLE is a list of label targets
                        if isinstance(value, list):
                            for j, target in enumerate(value):
                                if isinstance(target, str) and target not in flat_label_offsets:
                                    errors.append(
                                        f"{fname}: label {label!r}, entry {i}: "
                                        f"unresolved label {target!r} in {op_name}.{arg_spec.name}[{j}]"
                                    )
                    elif isinstance(value, str) and value not in flat_label_offsets:
                        errors.append(
                            f"{fname}: label {label!r}, entry {i}: "
                            f"unresolved label {value!r} in {op_name}.{arg_spec.name}"
                        )

    return errors


def _pack_dialogue(
    dialogue_dir: Path,
    output_dir: Path,
    doc: DumpDoc,
    reverse_text_table: dict[str, int],
    common_data: CommonData | None = None,
) -> dict[int, int]:
    """Compile all dialogue YAML into a single flat blob.

    All dialogue blocks are concatenated into one ``dialogue/dialogue.bin``
    file.  Every label gets a flat byte offset (guaranteed < 0xC00000) so
    the C port can distinguish new-format offsets from legacy SNES addresses.

    Returns a dict mapping original SNES addresses to flat offsets so config
    packers can update text pointer fields.
    """
    from ebtools.text_dsl.compiler import build_reverse_names, compile_text_block
    from ebtools.text_dsl.yaml_io import deserialize_dialogue_file

    # Build reverse name lookup for symbolic names in dialogue YAML.
    reverse_names = build_reverse_names(common_data) if common_data is not None else None

    # Build label→original SNES address map from dump config.
    original_label_addrs: dict[str, int] = {}
    for entry in doc.dumpEntries:
        if entry.extension == "ebtxt":
            block_base = entry.offset + 0xC00000
            for label_offset, label_name in doc.renameLabels.get(entry.name, {}).items():
                original_label_addrs[label_name] = block_base + label_offset

    yaml_files = sorted(dialogue_dir.glob("*.yaml")) + sorted(dialogue_dir.glob("*.yml"))

    # The C port uses dialogue_blob_base (0x100000) to distinguish dialogue
    # offsets from inline string offsets.  Label offsets used during compilation
    # must include this base so cross-references within the blob are correct.
    dialogue_blob_base = 0x100000

    # Phase 1: Load all files, compute flat byte offsets for every label
    # across ALL blocks in one continuous address space.
    all_dialogue: list[tuple[Path, dict[str, list[dict]]]] = []
    flat_label_offsets: dict[str, int] = {}
    global_byte_pos = 0

    for yaml_file in yaml_files:
        messages = deserialize_dialogue_file(yaml_file.read_text())
        all_dialogue.append((yaml_file, messages))

        # Dummy offsets for size measurement (LABEL args always compile to 4 bytes)
        dummy_offsets = {**original_label_addrs, **dict.fromkeys(messages, 0)}
        for label, ops in messages.items():
            flat_label_offsets[label] = dialogue_blob_base + global_byte_pos
            try:
                compiled = compile_text_block(
                    ops, reverse_text_table, label_offsets=dummy_offsets, reverse_names=reverse_names
                )
            except (ValueError, KeyError, TypeError) as exc:
                raise SystemExit(f"Error compiling dialogue {yaml_file.name}, label {label!r}: {exc}") from exc
            global_byte_pos += len(compiled)

    # Merge original SNES label addresses so cross-block references resolve.
    all_label_offsets = {**original_label_addrs, **flat_label_offsets}

    # Validate after Phase 1 (all labels known).
    validation_errors = _validate_dialogue(all_dialogue, all_label_offsets)
    if validation_errors:
        print("Dialogue validation errors:", file=sys.stderr)
        for err in validation_errors:
            print(f"  {err}", file=sys.stderr)
        raise SystemExit(f"Dialogue validation failed with {len(validation_errors)} error(s)")

    # Phase 2: Compile everything into one flat blob with resolved labels
    blob = bytearray()
    for _yaml_file, messages in all_dialogue:
        for _label, ops in messages.items():
            try:
                compiled = compile_text_block(
                    ops, reverse_text_table, label_offsets=flat_label_offsets, reverse_names=reverse_names
                )
            except (ValueError, KeyError, TypeError) as exc:
                raise SystemExit(f"Error compiling dialogue {_yaml_file.name}, label {_label!r}: {exc}") from exc
            blob.extend(compiled)

    if blob:
        out_path = output_dir / "dialogue" / "dialogue.bin"
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(blob)
        print(f"  Packed {len(all_dialogue)} dialogue files into dialogue.bin ({len(blob)} bytes)")

    # Build address remap: original SNES addr → flat offset (already includes
    # dialogue_blob_base from the label offset computation above).
    addr_remap: dict[int, int] = {}
    for label_name, original_addr in original_label_addrs.items():
        flat_offset = flat_label_offsets.get(label_name)
        if flat_offset is not None:
            addr_remap[original_addr] = flat_offset

    print(f"  Built address remap table ({len(addr_remap)} entries)")

    # Generate C header with all label → flat offset mappings.
    # Write to src/data/ (source tree), derived from dialogue_dir (src/assets/dialogue).
    src_data_dir = dialogue_dir.parent.parent / "data"
    _generate_text_refs_header(flat_label_offsets, src_data_dir)

    # Emit binary remap table: sorted array of (snes_addr_u32, blob_offset_u32) pairs.
    # Used by the C port to resolve SNES addresses embedded in binary ROM data
    # (e.g., movement script CC_0F text display) without the legacy TextBlock registry.
    _emit_addr_remap_bin(addr_remap, output_dir)

    return addr_remap


def _emit_addr_remap_bin(addr_remap: dict[int, int], output_dir: Path) -> None:
    """Emit ``dialogue/addr_remap.bin`` — sorted (snes_addr, blob_offset) pairs."""
    import struct

    sorted_pairs = sorted(addr_remap.items())
    buf = bytearray()
    for snes_addr, blob_offset in sorted_pairs:
        buf.extend(struct.pack("<II", snes_addr, blob_offset))

    out_path = output_dir / "dialogue" / "addr_remap.bin"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(bytes(buf))
    print(f"  Emitted addr_remap.bin ({len(sorted_pairs)} entries, {len(buf)} bytes)")


def _generate_text_refs_header(
    flat_label_offsets: dict[str, int],
    output_dir: Path,
) -> None:
    """Generate ``text_refs.h`` — C defines mapping label names to dialogue blob offsets."""
    header_path = output_dir / "text_refs.h"

    lines: list[str] = [
        "/*",
        " * text_refs.h — Auto-generated dialogue label offsets.",
        " *",
        " * Generated by: ebtools pack-all",
        " * Each define maps a dialogue label to its byte offset in the dialogue blob.",
        " * Use with display_text_from_addr() (offsets >= 0x100000 route to the blob).",
        " *",
        " * DO NOT EDIT — regenerate with: ebtools pack-all",
        " */",
        "#ifndef DATA_TEXT_REFS_H",
        "#define DATA_TEXT_REFS_H",
        "",
    ]

    # Group by YAML file prefix (everything before the first underscore after MSG_)
    # For readability, just sort alphabetically.
    for name, offset in sorted(flat_label_offsets.items()):
        lines.append(f"#define {name:60s} 0x{offset:06X}u")

    lines.append("")
    lines.append("#endif /* DATA_TEXT_REFS_H */")
    lines.append("")

    header_path.write_text("\n".join(lines))
    print(f"  Generated {header_path} ({len(flat_label_offsets)} defines)")


# --- Individual pack helpers ---


def _pack_items(json_path: Path, output_dir: Path, *, doc, string_table=None, addr_remap=None) -> None:
    from ebtools.parsers.item import pack_items

    pack_items(
        json_path,
        doc.textTable,
        output_dir / "data" / "item_configuration_table.bin",
        string_table=string_table,
        addr_remap=addr_remap,
    )


def _pack_enemies(json_path: Path, output_dir: Path, *, doc, common_data, string_table=None, addr_remap=None) -> None:
    from ebtools.parsers.enemy import pack_enemies

    pack_enemies(
        json_path,
        doc.textTable,
        common_data,
        output_dir / "data" / "enemy_configuration_table.bin",
        string_table=string_table,
        addr_remap=addr_remap,
    )


def _pack_npcs(json_path: Path, output_dir: Path, *, common_data, addr_remap=None) -> None:
    from ebtools.parsers.npc import pack_npcs

    pack_npcs(json_path, common_data, output_dir / "data" / "npc_config_table.bin", addr_remap=addr_remap)


def _pack_battle_actions(json_path: Path, output_dir: Path, *, string_table=None, addr_remap=None) -> None:
    from ebtools.parsers.battle_action import pack_battle_actions

    pack_battle_actions(
        json_path, output_dir / "data" / "battle_action_table.bin", string_table=string_table, addr_remap=addr_remap
    )


def _pack_psi_abilities(json_path: Path, output_dir: Path, *, string_table=None, addr_remap=None) -> None:
    from ebtools.parsers.psi_ability import pack_psi_abilities

    pack_psi_abilities(
        json_path, output_dir / "data" / "psi_ability_table.bin", string_table=string_table, addr_remap=addr_remap
    )


def _pack_teleport(json_path: Path, output_dir: Path, *, common_data, addr_remap=None) -> None:
    from ebtools.parsers.teleport import pack_teleport

    pack_teleport(json_path, common_data, output_dir / "data" / "teleport_destination_table.bin")


def _pack_psi_teleport(json_path: Path, output_dir: Path, *, doc, addr_remap=None) -> None:
    from ebtools.parsers.teleport import pack_psi_teleport

    pack_psi_teleport(json_path, doc.textTable, output_dir / "data" / "psi_teleport_dest_table.bin")


def _pack_bg_config(json_path: Path, output_dir: Path, *, addr_remap=None) -> None:
    from ebtools.parsers.bg_config import pack_bg_config

    pack_bg_config(json_path, output_dir / "data" / "bg_data_table.bin")


def _pack_exp_table(json_path: Path, output_dir: Path, *, addr_remap=None) -> None:
    from ebtools.parsers.exp_table import pack_exp_table

    pack_exp_table(json_path, output_dir / "data" / "exp_table.bin")


def _pack_stores(json_path: Path, output_dir: Path, *, common_data, addr_remap=None) -> None:
    from ebtools.parsers.store import pack_stores

    pack_stores(json_path, common_data, output_dir / "data" / "store_table.bin")


def _pack_music(json_path: Path, output_dir: Path, *, addr_remap=None) -> None:
    from ebtools.parsers.music import pack_music_dataset

    pack_music_dataset(json_path, output_dir / "music" / "dataset_table.bin")


def _pack_swirls(json_path: Path, output_dir: Path, *, addr_remap=None) -> None:
    from ebtools.parsers.swirl import pack_swirls

    pack_swirls(json_path, output_dir)


def _pack_all_fonts(assets_dir: Path, output_dir: Path) -> int:
    from ebtools.parsers.font import FONTS, pack_font

    fonts_dir = assets_dir / "fonts"
    count = 0
    for name, info in FONTS.items():
        png = fonts_dir / f"{name}.png"
        json_p = fonts_dir / f"{name}.json"
        if png.exists() and json_p.exists():
            gfx_bytes, width_bytes = pack_font(png, json_p)
            gfx_out = output_dir / info["gfx"]
            width_out = output_dir / info["widths"]
            gfx_out.parent.mkdir(parents=True, exist_ok=True)
            width_out.parent.mkdir(parents=True, exist_ok=True)
            gfx_out.write_bytes(gfx_bytes)
            width_out.write_bytes(width_bytes)
            count += 1
    if count:
        print(f"  Packed {count} fonts")
    return count


def _pack_all_sprites(assets_dir: Path, bin_dir: Path, output_dir: Path) -> int:
    sprites_dir = assets_dir / "overworld_sprites"
    pngs = list(sprites_dir.glob("*.png")) if sprites_dir.exists() else []
    if not pngs:
        return 0

    from ebtools.parsers.overworld_sprites import pack_sprites

    # pack_sprites writes to output_dir/{banks,palettes,sprite_grouping_*.bin}
    # which matches the overworld_sprites/ layout in the manifest.
    try:
        pack_sprites(sprites_dir, bin_dir, output_dir / "overworld_sprites")
    except Exception as e:
        print(f"  Warning: sprite packing failed: {e}", file=sys.stderr)
        return 0
    else:
        print(f"  Packed {len(pngs)} overworld sprites")
        return 1


def _pack_all_battle_sprites(assets_dir: Path, bin_dir: Path, output_dir: Path) -> int:
    sprites_dir = assets_dir / "battle_sprites"
    pngs = list(sprites_dir.glob("*.png")) if sprites_dir.exists() else []
    if not pngs:
        return 0

    from ebtools.parsers.battle_sprites import pack_battle_sprites

    # pack_battle_sprites writes common sprites to output_dir/
    # and locale-specific to output_dir.parent/"US"/"battle_sprites"/
    # So pass output_dir/battle_sprites — locale files go to output_dir/US/battle_sprites/
    try:
        pack_battle_sprites(sprites_dir, bin_dir, output_dir / "battle_sprites")
    except Exception as e:
        print(f"  Warning: battle sprite packing failed: {e}", file=sys.stderr)
        return 0
    else:
        print(f"  Packed {len(pngs)} battle sprites")
        return 1


def _pack_all_town_maps(assets_dir: Path, output_dir: Path) -> int:
    from ebtools.parsers.town_map import pack_town_map

    maps_dir = assets_dir / "town_maps"
    count = 0
    for i in range(6):
        tileset = maps_dir / f"{i}_tileset.png"
        arrangement = maps_dir / f"{i}_arrangement.json"
        pal = maps_dir / f"{i}.pal"
        if tileset.exists() and arrangement.exists() and pal.exists():
            out = output_dir / "town_maps" / f"{i}.bin.lzhal"
            pack_town_map(tileset, arrangement, pal, out)
            count += 1
    if count:
        print(f"  Packed {count} town maps")
    return count


def _pack_all_intro_gfx(assets_dir: Path, output_dir: Path) -> int:
    from ebtools.parsers.intro_gfx import INTRO_ASSETS, pack_intro_ending_asset

    intro_dir = assets_dir / "intro"
    count = 0
    for name, (gfx_stem, arr_stem, pal_stem, bpp) in INTRO_ASSETS.items():
        tileset = intro_dir / f"{name}_tileset.png"
        arrangement = intro_dir / f"{name}_arrangement.json"
        pal = intro_dir / f"{name}.pal"
        if tileset.exists() and arrangement.exists() and pal.exists():
            pack_intro_ending_asset(tileset, arrangement, pal, bpp, output_dir, gfx_stem, arr_stem, pal_stem)
            count += 1
    if count:
        print(f"  Packed {count} intro/ending graphics")
    return count


def _pack_all_battle_bgs(assets_dir: Path, output_dir: Path) -> int:
    from ebtools.parsers.battle_bg import pack_battle_bg

    bgs_dir = assets_dir / "battle_bgs"
    if not bgs_dir.exists():
        return 0

    count = 0
    for meta_path in sorted(bgs_dir.glob("*.json")):
        idx_str = meta_path.stem
        tileset = bgs_dir / f"{idx_str}_tileset.png"
        arrangement = bgs_dir / f"{idx_str}_arrangement.json"
        pal = bgs_dir / f"{idx_str}.pal"
        if tileset.exists() and arrangement.exists() and pal.exists():
            pack_battle_bg(tileset, arrangement, pal, meta_path, output_dir)
            count += 1
    if count:
        print(f"  Packed {count} battle backgrounds")
    return count


def _pack_bundled_arrangements(bin_dir: Path, output_dir: Path) -> int:
    from ebtools.parsers.psi_arrangements import pack_bundled_arrangements

    # Use packed .arr.lzhal if available (edited assets), else fall back to
    # the original extracted binaries.
    packed_arr_dir = output_dir / "psianims" / "arrangements"
    extracted_arr_dir = bin_dir / "psianims" / "arrangements"

    if packed_arr_dir.exists() and any(packed_arr_dir.glob("*.arr.lzhal")):
        input_dir = packed_arr_dir
    elif extracted_arr_dir.exists():
        input_dir = extracted_arr_dir
    else:
        return 0

    bundled_out = output_dir / "psianims" / "arrangements"
    count = pack_bundled_arrangements(input_dir, bundled_out)
    if count:
        print(f"  Packed {count} bundled PSI arrangements")
    return count
