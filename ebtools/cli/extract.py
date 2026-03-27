"""The ``extract`` command: pull binary assets from a donor ROM."""

import shutil
import sys
import tempfile
from pathlib import Path
from typing import Annotated

from cyclopts import Parameter

from ebtools.config import CommonData, DumpDoc, DumpInfo, load_common_data, load_dump_doc
from ebtools.hallz import decompress as hallz2_decompress
from ebtools.parsers import FULL_ROM_PARSERS, PARSERS
from ebtools.parsers.raw import write_raw
from ebtools.rom import detect


def dump_data(
    doc: DumpDoc,
    common_data: CommonData,
    rom: bytes,
    info: DumpInfo,
    out_path: Path,
    decompress: bool,
    temporary: Path,
    full_rom: bytes,
) -> list[str]:
    """Extract one dump entry from the ROM.

    Returns the list of output filenames (relative to out_path/info.subdir).
    """
    assert len(rom) == 0x300000, f"ROM size too small: Got {len(rom)}"
    assert info.offset <= 0x300000, f"Starting offset too high while attempting to write {info.subdir}/{info.name}"
    assert info.offset + info.size <= 0x300000, f"Size too high while attempting to write {info.subdir}/{info.name}"

    out_dir = out_path / info.subdir
    data = rom[info.offset : info.offset + info.size]
    offset = info.offset + 0xC00000

    files: list[str] = []

    if info.extension in FULL_ROM_PARSERS:
        parser = FULL_ROM_PARSERS[info.extension]
        files = parser(temporary, info.name, info.extension, data, offset, doc, common_data, full_rom)
    elif info.extension in PARSERS:
        parser = PARSERS[info.extension]
        files = parser(temporary, info.name, info.extension, data, offset, doc, common_data)
    else:
        # Default: raw binary
        if info.compressed:
            files += write_raw(
                temporary,
                info.name,
                info.extension + ".lzhal",
                data,
                offset,
                doc,
                common_data,
            )
            if decompress:
                decompressed = hallz2_decompress(data)
                filename = f"{info.name}.{info.extension}"
                (temporary / filename).write_bytes(decompressed)
                files.append(filename)
        else:
            files = write_raw(temporary, info.name, info.extension, data, offset, doc, common_data)

    # Copy changed files to output directory
    for file in files:
        target = out_dir / file
        temp_file = temporary / file
        if target.exists() and temp_file.read_bytes() == target.read_bytes():
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(temp_file, target)
        print(f"Dumped {target}")

    return files


def extract(
    yaml_config: Path = Path("earthbound.yml"),
    rom_path: Path = Path("earthbound.sfc"),
    *,
    decompress: Annotated[bool, Parameter(alias="-z")] = False,
    commondata: Annotated[Path, Parameter(alias="-c")] = Path("commondefs.yml"),
    clear: Annotated[bool, Parameter(alias="-x")] = False,
    dump_path: Annotated[str, Parameter(alias="-d")] = "",
) -> None:
    """Extract binary assets from an EarthBound/Mother 2 ROM.

    Parameters
    ----------
    yaml_config
        Path to dump doc YAML.
    rom_path
        Path to ROM file.
    decompress
        Decompress compressed data.
    commondata
        Path to common data definitions.
    clear
        Clear temp data.
    dump_path
        Override defined dump path.
    """
    common_data = load_common_data(commondata)
    doc = load_dump_doc(yaml_config)

    rom = rom_path.read_bytes()

    if len(rom) < 0x300000:
        print("File too small to be an Earthbound ROM.", file=sys.stderr)
        sys.exit(2)

    detected = detect(rom, doc.romIdentifier)
    if not detected.matched:
        print("Non-matching ROM detected.", file=sys.stderr)
        sys.exit(3)

    if detected.header:
        rom = rom[0x200:]
        print("Correct ROM detected (with 512 byte header)")
    else:
        print("Correct ROM detected")

    full_rom = rom[:0x300000]

    with tempfile.TemporaryDirectory() as temporary:
        out_path = Path(dump_path) if dump_path else Path(doc.defaultDumpPath)
        binary_assets: list[str] = []
        for entry in doc.dumpEntries:
            files = dump_data(doc, common_data, full_rom, entry, out_path, decompress, Path(temporary), full_rom)
            # PARSERS produce assembly/text source; everything else is binary assets
            if entry.extension not in PARSERS:
                for f in files:
                    binary_assets.append(str(Path(entry.subdir) / f))

        # Write manifest of binary asset files for the C port's embedded asset pipeline
        manifest_path = out_path / "assets.manifest"
        manifest_path.write_text("\n".join(sorted(binary_assets)) + "\n")
        print(f"Wrote {manifest_path} ({len(binary_assets)} binary assets)")

    # Export developer-friendly PNG spritesheets (after temp dir is closed)
    from ebtools.parsers.overworld_sprites import export_all_sprites

    asset_output_dir = Path("port") / "assets"
    count = export_all_sprites(out_path, asset_output_dir, sprite_names=common_data.sprites)
    if count > 0:
        print(f"Exported {count} overworld sprite PNGs to {asset_output_dir / 'overworld_sprites'}")

    # Export battle sprite PNGs
    from ebtools.parsers.battle_sprites import build_sprite_palette_map, export_all_battle_sprites

    palette_map = build_sprite_palette_map(full_rom)
    bs_count = export_all_battle_sprites(out_path, asset_output_dir, palette_map)
    if bs_count > 0:
        print(f"Exported {bs_count} battle sprite PNGs to {asset_output_dir / 'battle_sprites'}")

    # Export item configuration JSON
    from ebtools.parsers.item import export_items_json

    item_bin_path = out_path / "data" / "item_configuration_table.bin"
    if item_bin_path.exists():
        items_json_path = asset_output_dir / "items" / "items.json"
        export_items_json(
            item_bin_path.read_bytes(),
            doc.textTable,
            common_data.items,
            common_data.itemFlags,
            items_json_path,
        )
        print(f"Exported item configuration to {items_json_path}")

    # Export intro/ending graphics as PNG
    from ebtools.parsers.intro_gfx import export_intro_ending_gfx

    intro_count = export_intro_ending_gfx(out_path, asset_output_dir)
    if intro_count > 0:
        print(f"Exported {intro_count} intro/ending graphics to {asset_output_dir / 'intro'}")

    # Export town maps as PNG
    from ebtools.parsers.town_map import export_all_town_maps

    tm_count = export_all_town_maps(out_path, asset_output_dir)
    if tm_count > 0:
        print(f"Exported {tm_count} town maps to {asset_output_dir / 'town_maps'}")

    # Export swirl effects as JSON
    from ebtools.parsers.swirl import export_swirls_json

    swirl_json_path = asset_output_dir / "swirls" / "swirls.json"
    swirl_count = export_swirls_json(out_path, swirl_json_path)
    if swirl_count > 0:
        print(f"Exported {swirl_count} swirl effects to {swirl_json_path}")

    # Export battle backgrounds as PNG
    from ebtools.parsers.battle_bg import export_battle_bgs

    bgc_bin_path = out_path / "data" / "bg_data_table.bin"
    if bgc_bin_path.exists():
        bg_count = export_battle_bgs(out_path, bgc_bin_path.read_bytes(), asset_output_dir)
        if bg_count > 0:
            print(f"Exported {bg_count} battle backgrounds to {asset_output_dir / 'battle_bgs'}")

    # Export fonts as PNG + JSON
    from ebtools.parsers.font import export_all_fonts

    font_count = export_all_fonts(out_path, asset_output_dir)
    if font_count > 0:
        print(f"Exported {font_count} fonts to {asset_output_dir / 'fonts'}")

    # Export enemy configuration JSON
    from ebtools.parsers.enemy import export_enemies_json

    enemy_bin_path = out_path / "data" / "enemy_configuration_table.bin"
    if enemy_bin_path.exists():
        enemies_json_path = asset_output_dir / "enemies" / "enemies.json"
        export_enemies_json(
            enemy_bin_path.read_bytes(),
            doc.textTable,
            common_data,
            enemies_json_path,
        )
        print(f"Exported enemy configuration to {enemies_json_path}")

    # Export NPC configuration JSON
    from ebtools.parsers.npc import export_npcs_json

    npc_bin_path = out_path / "data" / "npc_config_table.bin"
    if npc_bin_path.exists():
        npcs_json_path = asset_output_dir / "data" / "npc_config.json"
        export_npcs_json(
            npc_bin_path.read_bytes(),
            common_data,
            npcs_json_path,
        )
        print(f"Exported NPC configuration to {npcs_json_path}")

    # Export battle action table JSON
    from ebtools.parsers.battle_action import export_battle_actions_json

    ba_bin_path = out_path / "data" / "battle_action_table.bin"
    if ba_bin_path.exists():
        ba_json_path = asset_output_dir / "battle" / "battle_actions.json"
        export_battle_actions_json(ba_bin_path.read_bytes(), ba_json_path)
        print(f"Exported battle actions to {ba_json_path}")

    # Export PSI ability table JSON
    from ebtools.parsers.psi_ability import export_psi_abilities_json

    psi_bin_path = out_path / "data" / "psi_ability_table.bin"
    if psi_bin_path.exists():
        psi_json_path = asset_output_dir / "battle" / "psi_abilities.json"
        psi_name_bin_path = out_path / "data" / "psi_name_table.bin"
        psi_names_data = psi_name_bin_path.read_bytes() if psi_name_bin_path.exists() else None
        export_psi_abilities_json(
            psi_bin_path.read_bytes(),
            doc.textTable,
            psi_names_data,
            psi_json_path,
        )
        print(f"Exported PSI abilities to {psi_json_path}")

    # Export teleport destination table JSON
    from ebtools.parsers.teleport import export_teleport_json

    tp_bin_path = out_path / "data" / "teleport_destination_table.bin"
    if tp_bin_path.exists():
        tp_json_path = asset_output_dir / "data" / "teleport_destinations.json"
        export_teleport_json(tp_bin_path.read_bytes(), common_data, tp_json_path)
        print(f"Exported teleport destinations to {tp_json_path}")

    # Export PSI teleport destination table JSON
    from ebtools.parsers.teleport import export_psi_teleport_json

    ptp_bin_path = out_path / "data" / "psi_teleport_dest_table.bin"
    if ptp_bin_path.exists():
        ptp_json_path = asset_output_dir / "data" / "psi_teleport_destinations.json"
        export_psi_teleport_json(ptp_bin_path.read_bytes(), doc.textTable, ptp_json_path)
        print(f"Exported PSI teleport destinations to {ptp_json_path}")

    # Export BG config table JSON
    from ebtools.parsers.bg_config import export_bg_config_json

    bgc_bin_path = out_path / "data" / "bg_data_table.bin"
    if bgc_bin_path.exists():
        bgc_json_path = asset_output_dir / "battle" / "bg_config.json"
        export_bg_config_json(bgc_bin_path.read_bytes(), bgc_json_path)
        print(f"Exported BG config to {bgc_json_path}")

    # Export EXP table JSON
    from ebtools.parsers.exp_table import export_exp_table_json

    exp_bin_path = out_path / "data" / "exp_table.bin"
    if exp_bin_path.exists():
        exp_json_path = asset_output_dir / "data" / "exp_table.json"
        export_exp_table_json(exp_bin_path.read_bytes(), exp_json_path)
        print(f"Exported EXP table to {exp_json_path}")

    # Export store/shop table JSON
    from ebtools.parsers.store import export_stores_json

    store_bin_path = out_path / "data" / "store_table.bin"
    if store_bin_path.exists():
        store_json_path = asset_output_dir / "data" / "stores.json"
        export_stores_json(store_bin_path.read_bytes(), common_data, store_json_path)
        print(f"Exported store table to {store_json_path}")

    # Export simple data tables as JSON
    from ebtools.parsers.simple_tables import (
        export_attack_type_palettes_json,
        export_attract_mode_json,
        export_battle_sprites_pointers_json,
        export_bg_distortion_table_json,
        export_bg_scrolling_table_json,
        export_btl_entry_bg_json,
        export_btl_entry_ptr_table_json,
        export_consolation_items_json,
        export_eb_string_json,
        export_enemy_battle_groups_json,
        export_enemy_placement_groups_json,
        export_enemy_psi_colours_json,
        export_entity_collision_json,
        export_event_music_json,
        export_footstep_sounds_json,
        export_for_sale_signs_json,
        export_giygas_delays_json,
        export_guardian_text_json,
        export_hotspots_json,
        export_map_enemy_placement_json,
        export_misc_swirl_colours_json,
        export_movement_palette_json,
        export_npc_ai_json,
        export_per_sector_music_json,
        export_per_sector_town_map_json,
        export_per_sector_u16_json,
        export_prayer_noise_json,
        export_psi_anim_cfg_json,
        export_psi_names_json,
        export_psi_suffixes_json,
        export_raw_byte_list_json,
        export_shake_amplitudes_json,
        export_sound_stone_config_json,
        export_sound_stone_melodies_json,
        export_sprite_placement_json,
        export_stat_gain_modifiers_json,
        export_stats_growth_json,
        export_status_equip_text_8_13_json,
        export_status_equip_text_json,
        export_status_equip_tile_tables_json,
        export_telephone_contacts_json,
        export_tileset_palette_data_json,
        export_tileset_table_json,
        export_timed_delivery_json,
        export_timed_item_transforms_json,
        export_window_border_anim_json,
    )

    data_dir = out_path / "data"
    data_json_dir = asset_output_dir / "data"

    # Simple binary -> JSON exports
    _simple_exports = [
        ("attack_type_palettes.bin", "attack_type_palettes.json", export_attack_type_palettes_json),
        ("enemy_psi_colours.bin", "enemy_psi_colours.json", export_enemy_psi_colours_json),
        ("misc_swirl_colours.bin", "misc_swirl_colours.json", export_misc_swirl_colours_json),
        ("footstep_sound_table.bin", "footstep_sounds.json", export_footstep_sounds_json),
        ("vertical_shake_amplitude_table.bin", "shake_amplitudes.json", export_shake_amplitudes_json),
        ("final_giygas_prayer_noise_table.bin", "prayer_noise.json", export_prayer_noise_json),
        ("stat_gain_modifier_table.bin", "stat_gain_modifiers.json", export_stat_gain_modifiers_json),
        ("stats_growth_vars.bin", "stats_growth.json", export_stats_growth_json),
        ("consolation_item_table.bin", "consolation_items.json", export_consolation_items_json),
        ("tileset_table.bin", "tileset_table.json", export_tileset_table_json),
        ("hotspot_coordinates.bin", "hotspots.json", export_hotspots_json),
        ("timed_delivery_table.bin", "timed_delivery.json", export_timed_delivery_json),
        ("timed_item_transformation_table.bin", "timed_item_transforms.json", export_timed_item_transforms_json),
        ("for_sale_sign_sprite_table.bin", "for_sale_signs.json", export_for_sale_signs_json),
        ("btl_entry_bg_table.bin", "btl_entry_bg.json", export_btl_entry_bg_json),
        ("npc_ai_table.bin", "npc_ai.json", export_npc_ai_json),
        ("movement_text_string_palette.bin", "movement_palette.json", export_movement_palette_json),
        ("window_border_anim_tiles.bin", "window_border_anim.json", export_window_border_anim_json),
        ("per_sector_music.bin", "per_sector_music.json", export_per_sector_music_json),
        ("per_sector_attributes.bin", "per_sector_attributes.json", export_per_sector_u16_json),
        ("per_sector_town_map.bin", "per_sector_town_map.json", export_per_sector_town_map_json),
        ("global_map_tilesetpalette_data.bin", "tileset_palette_data.json", export_tileset_palette_data_json),
        ("psi_anim_cfg.bin", "psi_anim_cfg.json", export_psi_anim_cfg_json),
        ("battle_sprites_pointers.bin", "battle_sprites_pointers.json", export_battle_sprites_pointers_json),
        ("btl_entry_ptr_table.bin", "btl_entry_ptr_table.json", export_btl_entry_ptr_table_json),
        ("sound_stone_config.bin", "sound_stone_config.json", export_sound_stone_config_json),
        ("sound_stone_melodies.bin", "sound_stone_melodies.json", export_sound_stone_melodies_json),
        ("bg_distortion_table.bin", "bg_distortion_table.json", export_bg_distortion_table_json),
        ("bg_scrolling_table.bin", "bg_scrolling_table.json", export_bg_scrolling_table_json),
        (
            "giygas_death_static_transition_delays.bin",
            "giygas_death_static_transition_delays.json",
            export_giygas_delays_json,
        ),
        ("attract_mode_txt.bin", "attract_mode_txt.json", export_attract_mode_json),
        ("status_equip_tile_tables.bin", "status_equip_tile_tables.json", export_status_equip_tile_tables_json),
        ("map_enemy_placement.bin", "map_enemy_placement.json", export_map_enemy_placement_json),
        # Truly opaque tables (display_text CC bytecode, collision data)
        ("status_window_text.bin", "status_window_text.json", export_raw_byte_list_json),
        ("map/collision_arrangement_table.bin", "collision_arrangement_table.json", export_raw_byte_list_json),
        ("map/collision_pointers_blob.bin", "collision_pointers_blob.json", export_raw_byte_list_json),
    ]

    simple_count = 0
    for bin_name, json_name, func in _simple_exports:
        bin_path = data_dir / bin_name
        if bin_path.exists():
            func(bin_path.read_bytes(), data_json_dir / json_name)
            simple_count += 1

    # EB-text exports (need textTable for decode)
    _text_exports = [
        ("psi_name_table.bin", "psi_name_table.json", export_psi_names_json),
        ("psi_suffixes.bin", "psi_suffixes.json", export_psi_suffixes_json),
        ("phone_call_text.bin", "phone_call_text.json", export_eb_string_json),
        ("status_equip_window_text_7.bin", "status_equip_window_text_7.json", export_eb_string_json),
        ("status_equip_window_text_14.bin", "status_equip_window_text_14.json", export_eb_string_json),
        ("status_equip_text.bin", "status_equip_text.json", export_status_equip_text_json),
        (
            "status_equip_window_text_8_13.bin",
            "status_equip_window_text_8_13.json",
            export_status_equip_text_8_13_json,
        ),
        ("telephone_contacts_table.bin", "telephone_contacts_table.json", export_telephone_contacts_json),
    ]
    for bin_name, json_name, func in _text_exports:
        bin_path = data_dir / bin_name
        if bin_path.exists():
            func(bin_path.read_bytes(), doc.textTable, data_json_dir / json_name)
            simple_count += 1

    # Compressed text dictionary (2 bin files -> 1 JSON of decoded strings)
    ct_data_path = data_dir / "compressed_text_data.bin"
    ct_ptrs_path = data_dir / "compressed_text_ptrs.bin"
    if ct_data_path.exists() and ct_ptrs_path.exists():
        from ebtools.parsers.simple_tables import export_compressed_text_json

        export_compressed_text_json(
            ct_data_path.read_bytes(),
            ct_ptrs_path.read_bytes(),
            doc.textTable,
            data_json_dir / "compressed_text.json",
        )
        simple_count += 1

    # Entity collision tables (5 files -> 1 JSON)
    if (data_dir / "entity_collision_left_x.bin").exists():
        export_entity_collision_json(data_dir, data_json_dir / "entity_collision.json")
        simple_count += 1

    # Pointer+data table pairs
    ptr_pairs = [
        (
            "sprite_placement_ptr_table.bin",
            "sprite_placement_table.bin",
            "sprite_placement.json",
            export_sprite_placement_json,
        ),
        (
            "overworld_event_music_ptr_table.bin",
            "overworld_event_music_table.bin",
            "event_music.json",
            export_event_music_json,
        ),
        (
            "enemy_placement_groups_ptr_table.bin",
            "enemy_placement_groups.bin",
            "enemy_placement_groups.json",
            export_enemy_placement_groups_json,
        ),
    ]
    for ptr_name, data_name, json_name, func in ptr_pairs:
        ptr_path = data_dir / ptr_name
        tbl_path = data_dir / data_name
        if ptr_path.exists() and tbl_path.exists():
            func(ptr_path.read_bytes(), tbl_path.read_bytes(), data_json_dir / json_name)
            simple_count += 1

    # Enemy battle groups (single file, variable length)
    ebg_path = data_dir / "enemy_battle_groups_table.bin"
    if ebg_path.exists():
        export_enemy_battle_groups_json(ebg_path.read_bytes(), data_json_dir / "enemy_battle_groups.json")
        simple_count += 1

    # Non-data binary assets: maps, ending, loose files, town map icons, etc.
    _misc_exports = [
        # Maps
        ("maps/door_config_table.bin", "maps/door_config_table.json"),
        ("maps/door_data.bin", "maps/door_data.json"),
        ("maps/door_pointer_table.bin", "maps/door_pointer_table.json"),
        ("maps/event_control_ptr_table.bin", "maps/event_control_ptr_table.json"),
        ("maps/screen_transition_config.bin", "maps/screen_transition_config.json"),
        ("maps/tile_event_control_table.bin", "maps/tile_event_control_table.json"),
        ("maps/anim_pal/meta_table.bin", "maps/anim_pal_meta_table.json"),
        # Ending
        ("ending/credits_font.pal", "ending/credits_font_pal.json"),
        ("ending/E1E924.bin", "ending/E1E924.json"),
        ("ending/party_cast_tile_ids.bin", "ending/party_cast_tile_ids.json"),
        ("ending/photographer_cfg.bin", "ending/photographer_cfg.json"),
        # Overworld sprites (non-gfx)
        ("overworld_sprites/entity_overlay_data.bin", "overworld_sprites/entity_overlay_data.json"),
        ("overworld_sprites/spritemap_config.bin", "overworld_sprites/spritemap_config.json"),
        # Town map icons
        ("town_maps/icon_animation_flags.bin", "town_maps/icon_animation_flags.json"),
        ("town_maps/icon_placement.bin", "town_maps/icon_placement.json"),
        ("town_maps/icon_spritemap_ptrs.bin", "town_maps/icon_spritemap_ptrs.json"),
        ("town_maps/icon_spritemaps.bin", "town_maps/icon_spritemaps.json"),
        ("town_maps/icon.pal", "town_maps/icon_pal.json"),
        ("town_maps/mapping.bin", "town_maps/mapping.json"),
        # Loose files
        ("sound_stone.pal", "misc/sound_stone_pal.json"),
        ("sprite_group_palettes.pal", "misc/sprite_group_palettes.json"),
        # More loose/misc files
        ("unknown_palette.pal.lzhal", "misc/unknown_palette.json"),
        ("debug_cursor.gfx", "misc/debug_cursor_gfx.json"),
        ("fonts/debug.gfx", "misc/fonts_debug_gfx.json"),
        # Compressed assets (stored as raw byte lists of the compressed data)
        ("E1CFAF.gfx.lzhal", "misc/E1CFAF_gfx.json"),
        ("E1D4F4.pal.lzhal", "misc/E1D4F4_pal.json"),
        ("E1D5E8.arr.lzhal", "misc/E1D5E8_arr.json"),
        ("ending/E1E94A.bin.lzhal", "ending/E1E94A.json"),
        ("graphics/flavoured_text.gfx.lzhal", "graphics/flavoured_text_gfx.json"),
        ("intro/gas_station2.pal.lzhal", "intro/gas_station2_pal.json"),
        ("intro/attract/nintendo_itoi.pal.lzhal", "intro/attract/nintendo_itoi_pal.json"),
        ("intro/attract/nintendo_presentation.arr.lzhal", "intro/attract/nintendo_presentation_arr.json"),
        ("intro/attract/nintendo_presentation.gfx.lzhal", "intro/attract/nintendo_presentation_gfx.json"),
        ("intro/attract/produced_by_itoi.arr.lzhal", "intro/attract/produced_by_itoi_arr.json"),
        ("intro/attract/produced_by_itoi.gfx.lzhal", "intro/attract/produced_by_itoi_gfx.json"),
    ]

    # Locale-specific assets (in US/ subdirectory)
    us_dir = out_path / "US"
    _locale_exports = [
        ("E1AE7C.bin.lzhal", "locale/E1AE7C.json"),
        ("E1AE83.bin.lzhal", "locale/E1AE83.json"),
        ("E1AEFD.bin.lzhal", "locale/E1AEFD.json"),
        ("E1D6E1.gfx.lzhal", "locale/E1D6E1_gfx.json"),
        ("ending/cast_bg_palette.pal", "locale/ending/cast_bg_palette.json"),
        ("ending/cast_names.gfx.lzhal", "locale/ending/cast_names_gfx.json"),
        ("ending/cast_names.pal.lzhal", "locale/ending/cast_names_pal.json"),
        ("ending/cast_sequence_formatting.bin", "locale/ending/cast_sequence_formatting.json"),
        ("ending/credits_font.gfx.lzhal", "locale/ending/credits_font_gfx.json"),
        ("ending/staff_text.bin", "locale/ending/staff_text.json"),
        ("events/bank_c3_scripts.bin", "locale/events/bank_c3_scripts.json"),
        ("events/bank_c4_scripts.bin", "locale/events/bank_c4_scripts.json"),
        ("events/event_script_pointers.bin", "locale/events/event_script_pointers.json"),
        ("events/naming_screen_entities.bin", "locale/events/naming_screen_entities.json"),
        ("graphics/sound_stone.gfx.lzhal", "locale/graphics/sound_stone_gfx.json"),
        ("graphics/text_window_flavour_palettes.pal", "locale/graphics/text_window_flavour_palettes.json"),
        ("graphics/text_window.gfx.lzhal", "locale/graphics/text_window_gfx.json"),
        ("intro/title_screen_letters.gfx.lzhal", "locale/intro/title_screen_letters_gfx.json"),
        ("intro/title_screen_script_pointers.bin", "locale/intro/title_screen_script_pointers.json"),
        ("intro/title_screen_scripts.bin", "locale/intro/title_screen_scripts.json"),
        ("intro/title_screen_spritemaps.bin", "locale/intro/title_screen_spritemaps.json"),
        ("maps/palettes/1.pal", "locale/maps/palettes/1.json"),
        ("town_maps/label.gfx.lzhal", "locale/town_maps/label_gfx.json"),
    ]
    for bin_rel, json_rel in _locale_exports:
        bin_path = us_dir / bin_rel
        if bin_path.exists():
            json_out = asset_output_dir / json_rel
            export_raw_byte_list_json(bin_path.read_bytes(), json_out)
            simple_count += 1

    # Locale EB-text exports (need textTable)
    lumine_bin = us_dir / "data" / "text" / "lumine_hall_text.bin"
    if lumine_bin.exists():
        export_eb_string_json(
            lumine_bin.read_bytes(),
            doc.textTable,
            asset_output_dir / "locale" / "data" / "text" / "lumine_hall_text.json",
        )
        simple_count += 1
    guardian_bin = us_dir / "ending" / "guardian_text.bin"
    if guardian_bin.exists():
        export_guardian_text_json(
            guardian_bin.read_bytes(), doc.textTable, asset_output_dir / "locale" / "ending" / "guardian_text.json"
        )
        simple_count += 1
    for bin_rel, json_rel in _misc_exports:
        bin_path = out_path / bin_rel
        if bin_path.exists():
            json_out = asset_output_dir / json_rel
            export_raw_byte_list_json(bin_path.read_bytes(), json_out)
            simple_count += 1

    # Bulk export: indexed binary files (audiopacks, maps, palettes, psianims, etc.)
    _bulk_exports = [
        ("audiopacks", "*.ebm", "audiopacks"),
        ("overworld_sprites/palettes", "*.pal", "overworld_sprites/palettes"),
        ("battle_sprites/palettes", "*.pal", "battle_sprites/palettes"),
        ("maps/tiles", "*.bin", "maps/tiles"),
        ("maps/palettes", "*.pal", "maps/palettes"),
        ("maps/anim_gfx", "*.gfx.lzhal", "maps/anim_gfx"),
        ("maps/anim_pal", "*.pal.lzhal", "maps/anim_pal"),
        ("maps/anim_props", "*.bin", "maps/anim_props"),
        ("psianims/gfx", "*.gfx.lzhal", "psianims/gfx"),
        ("psianims/arrangements", "*.arr.lzhal", "psianims/arrangements"),
        ("psianims/palettes", "*.pal", "psianims/palettes"),
        ("maps/gfx", "*.gfx.lzhal", "maps/gfx"),
        ("maps/arrangements", "*.arr.lzhal", "maps/arrangements"),
        ("graphics/animations", "*.anim.lzhal", "graphics/animations"),
        ("data", "E*.bin", "data/events"),
        ("data", "EBATTLE*.bin", "data/events"),
        ("data", "EBGMESS.bin", "data/events"),
        ("data", "EDEBUG.bin", "data/events"),
        ("data", "EEVENT*.bin", "data/events"),
        ("data", "EEXPL*.bin", "data/events"),
        ("data", "EGLOBAL.bin", "data/events"),
        ("data", "EGOODS*.bin", "data/events"),
        ("data", "EHINT.bin", "data/events"),
        ("data", "ENEWS.bin", "data/events"),
        ("data", "ESHOP*.bin", "data/events"),
        ("data", "ESYSTEM.bin", "data/events"),
        ("data", "KEYBOARD.bin", "data/events"),
        ("data", "DOOR_SCRIPTS.bin", "data/events"),
        ("data", "DEBUG_TEXT.bin", "data/events"),
        ("data", "UNKNOWN_*.bin", "data/events"),
    ]
    import glob as glob_mod

    for src_subdir, pattern, out_subdir in _bulk_exports:
        src_path = out_path / src_subdir
        if not src_path.exists():
            continue
        for f in sorted(src_path.glob(pattern)):
            stem = f.name.replace(".", "_").replace("-", "_")
            json_out = asset_output_dir / out_subdir / f"{stem}.json"
            export_raw_byte_list_json(f.read_bytes(), json_out)
            simple_count += 1

    # US-specific: misc locale files
    _us_misc = [
        ("mystery_sram.bin.lzhal", "locale/mystery_sram.json"),
        ("tea.bin", "locale/tea.json"),
        ("text/floating_sprite_table.bin", "locale/text/floating_sprite_table.json"),
    ]
    for bin_rel, json_rel in _us_misc:
        bin_path = us_dir / bin_rel
        if bin_path.exists():
            export_raw_byte_list_json(bin_path.read_bytes(), asset_output_dir / json_rel)
            simple_count += 1

    # US-specific: town maps (locale-specific compressed maps)
    us_town_maps = us_dir / "town_maps"
    if us_town_maps.exists():
        for f in sorted(us_town_maps.glob("*")):
            if f.is_file():
                stem = f.name.replace(".", "_")
                json_out = asset_output_dir / "locale" / "town_maps" / f"{stem}.json"
                export_raw_byte_list_json(f.read_bytes(), json_out)
                simple_count += 1

    # US-specific locale maps (gfx, arrangements, tiles)
    for subdir in ["maps/gfx", "maps/arrangements", "maps/tiles"]:
        us_sub = out_path / "US" / subdir
        if us_sub.exists():
            for f in sorted(us_sub.glob("*")):
                if f.is_file():
                    stem = f.name.replace(".", "_")
                    json_out = asset_output_dir / "locale" / subdir / f"{stem}.json"
                    export_raw_byte_list_json(f.read_bytes(), json_out)
                    simple_count += 1

    # US-specific locale palettes
    us_maps_pal = out_path / "US" / "maps" / "palettes"
    if us_maps_pal.exists():
        for f in sorted(us_maps_pal.glob("*.pal")):
            stem = f.name.replace(".", "_")
            json_out = asset_output_dir / "locale" / "maps" / "palettes" / f"{stem}.json"
            export_raw_byte_list_json(f.read_bytes(), json_out)
            simple_count += 1

    # US fonts (romaji)
    us_fonts = out_path / "US" / "fonts"
    if us_fonts.exists():
        for f in sorted(us_fonts.glob("*")):
            if f.is_file():
                stem = f.name.replace(".", "_")
                json_out = asset_output_dir / "locale" / "fonts" / f"{stem}.json"
                export_raw_byte_list_json(f.read_bytes(), json_out)
                simple_count += 1

    # US graphics/animations
    us_gfx_anims = out_path / "US" / "graphics" / "animations"
    if us_gfx_anims.exists():
        for f in sorted(us_gfx_anims.glob("*")):
            if f.is_file():
                stem = f.name.replace(".", "_")
                json_out = asset_output_dir / "locale" / "graphics" / "animations" / f"{stem}.json"
                export_raw_byte_list_json(f.read_bytes(), json_out)
                simple_count += 1

    # US flyover data
    for f in sorted((out_path / "US").glob("flyover_*.bin")):
        stem = f.name.replace(".", "_")
        json_out = asset_output_dir / "locale" / f"{stem}.json"
        export_raw_byte_list_json(f.read_bytes(), json_out)
        simple_count += 1

    # US coffee
    coffee = out_path / "US" / "coffee.bin"
    if coffee.exists():
        export_raw_byte_list_json(coffee.read_bytes(), asset_output_dir / "locale" / "coffee.json")
        simple_count += 1

    # US errors
    us_errors = out_path / "US" / "errors"
    if us_errors.exists():
        for f in sorted(us_errors.glob("*")):
            if f.is_file():
                stem = f.name.replace(".", "_")
                json_out = asset_output_dir / "locale" / "errors" / f"{stem}.json"
                export_raw_byte_list_json(f.read_bytes(), json_out)
                simple_count += 1

    # Common errors
    errors_dir = out_path / "errors"
    if errors_dir.exists():
        for f in sorted(errors_dir.glob("*")):
            if f.is_file():
                stem = f.name.replace(".", "_")
                json_out = asset_output_dir / "errors" / f"{stem}.json"
                export_raw_byte_list_json(f.read_bytes(), json_out)
                simple_count += 1

    if simple_count > 0:
        print(f"Exported {simple_count} data tables to {data_json_dir}")

    # Export music_tracks.json (track names + dataset table pack assignments)
    from ebtools.parsers.music import export_music_json

    music_asm_path = yaml_config.parent / "include" / "constants" / "music.asm"
    dataset_bin_path = out_path / "music" / "dataset_table.bin"
    if music_asm_path.exists() and dataset_bin_path.exists():
        music_json_path = asset_output_dir / "music_tracks.json"
        music_count = export_music_json(music_asm_path, dataset_bin_path, music_json_path)
        if music_count > 0:
            print(f"Exported {music_count} music tracks to {music_json_path}")

    # Export dialogue YAML files from raw text block binaries.
    # Decodes text bytecode into human-readable YAML DSL with symbolic labels,
    # event flag names, item names, etc.  Also migrates text pointer fields in
    # items.json, enemies.json, etc. to inline text or dialogue references.
    from ebtools.cli.migrate_text import export_dialogue_yaml

    export_dialogue_yaml(
        assets_dir=asset_output_dir,
        bin_dir=out_path,
        doc=doc,
        common_data=common_data,
    )
