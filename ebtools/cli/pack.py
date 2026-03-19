"""Pack commands: convert edited assets back to binary formats."""

import sys
from pathlib import Path
from typing import Annotated

from cyclopts import App, Parameter

from ebtools.config import load_common_data, load_dump_doc

pack_app = App(name="pack", help="Pack modified assets back to binary formats.")


@pack_app.command(name="sprites")
def pack_sprites_cmd(
    png_dir: Annotated[Path, Parameter(help="Directory containing modified PNG + JSON sprite files")],
    bin_dir: Annotated[Path, Parameter(help="Path to original extracted binary assets (e.g. asm/bin/)")],
    output_dir: Annotated[Path, Parameter(help="Output directory for generated binary overrides")],
) -> None:
    """Pack modified PNG spritesheets back to binary overworld sprite format.

    Reads indexed PNG spritesheets and JSON metadata, encodes them as SNES 4BPP
    tile data, and writes binary bank files suitable for use as custom_assets
    overrides in the C port build.

    Parameters
    ----------
    png_dir
        Directory containing .png and .json files (e.g. src/custom_assets/overworld_sprites/png/).
    bin_dir
        Path to original extracted binary assets (e.g. asm/bin/).
    output_dir
        Output directory for binary overrides (e.g. src/custom_assets/overworld_sprites/).
    """
    from ebtools.parsers._tiles import PackError
    from ebtools.parsers.overworld_sprites import pack_sprites

    if not png_dir.is_dir():
        print(f"Error: {png_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    try:
        pack_sprites(png_dir, bin_dir, output_dir)
    except PackError:
        sys.exit(1)


@pack_app.command(name="battle-sprites")
def pack_battle_sprites_cmd(
    png_dir: Annotated[Path, Parameter(help="Directory containing modified PNG + JSON battle sprite files")],
    bin_dir: Annotated[Path, Parameter(help="Path to original extracted binary assets (e.g. asm/bin/)")],
    output_dir: Annotated[Path, Parameter(help="Output directory for generated binary overrides")],
) -> None:
    """Pack modified PNG images back to compressed LZHAL battle sprite format.

    Reads indexed PNG images and JSON metadata, encodes them as SNES 4BPP
    tile data with battle sprite arrangement, compresses with LZHAL, and
    writes .gfx.lzhal files suitable for use as custom_assets overrides
    in the C port build.

    Parameters
    ----------
    png_dir
        Directory containing .png and .json files (e.g. src/custom_assets/battle_sprites/png/).
    bin_dir
        Path to original extracted binary assets (e.g. asm/bin/).
    output_dir
        Output directory for binary overrides (e.g. src/custom_assets/battle_sprites/).
    """
    from ebtools.parsers._tiles import PackError
    from ebtools.parsers.battle_sprites import pack_battle_sprites

    if not png_dir.is_dir():
        print(f"Error: {png_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    try:
        pack_battle_sprites(png_dir, bin_dir, output_dir)
    except PackError:
        sys.exit(1)


@pack_app.command(name="items")
def pack_items_cmd(
    items_json: Annotated[Path, Parameter(help="Path to items.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
    *,
    yaml_config: Annotated[Path, Parameter(alias="-y")] = Path("earthbound.yml"),
) -> None:
    """Pack items.json back to binary item configuration table.

    Parameters
    ----------
    items_json
        Path to the edited items.json.
    output_dir
        Output directory (binary written to output_dir/data/item_configuration_table.bin).
    yaml_config
        Path to dump doc YAML (for text table).
    """
    from ebtools.parsers.item import pack_items

    if not items_json.exists():
        print(f"Error: {items_json} not found", file=sys.stderr)
        sys.exit(1)

    doc = load_dump_doc(yaml_config)
    output_path = output_dir / "data" / "item_configuration_table.bin"
    pack_items(items_json, doc.textTable, output_path)
    print(f"Packed items to {output_path}")


@pack_app.command(name="enemies")
def pack_enemies_cmd(
    enemies_json: Annotated[Path, Parameter(help="Path to enemies.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
    *,
    yaml_config: Annotated[Path, Parameter(alias="-y")] = Path("earthbound.yml"),
    commondata: Annotated[Path, Parameter(alias="-c")] = Path("commondefs.yml"),
) -> None:
    """Pack enemies.json back to binary enemy configuration table."""
    from ebtools.parsers.enemy import pack_enemies

    if not enemies_json.exists():
        print(f"Error: {enemies_json} not found", file=sys.stderr)
        sys.exit(1)

    doc = load_dump_doc(yaml_config)
    common_data = load_common_data(commondata)
    output_path = output_dir / "data" / "enemy_configuration_table.bin"
    pack_enemies(enemies_json, doc.textTable, common_data, output_path)
    print(f"Packed enemies to {output_path}")


@pack_app.command(name="npcs")
def pack_npcs_cmd(
    npcs_json: Annotated[Path, Parameter(help="Path to npc_config.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
    *,
    commondata: Annotated[Path, Parameter(alias="-c")] = Path("commondefs.yml"),
) -> None:
    """Pack npc_config.json back to binary NPC configuration table."""
    from ebtools.parsers.npc import pack_npcs

    if not npcs_json.exists():
        print(f"Error: {npcs_json} not found", file=sys.stderr)
        sys.exit(1)

    common_data = load_common_data(commondata)
    output_path = output_dir / "data" / "npc_config_table.bin"
    pack_npcs(npcs_json, common_data, output_path)
    print(f"Packed NPCs to {output_path}")


@pack_app.command(name="battle-actions")
def pack_battle_actions_cmd(
    json_path: Annotated[Path, Parameter(help="Path to battle_actions.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
) -> None:
    """Pack battle_actions.json back to binary battle action table."""
    from ebtools.parsers.battle_action import pack_battle_actions

    if not json_path.exists():
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)

    output_path = output_dir / "data" / "battle_action_table.bin"
    pack_battle_actions(json_path, output_path)
    print(f"Packed battle actions to {output_path}")


@pack_app.command(name="psi-abilities")
def pack_psi_abilities_cmd(
    json_path: Annotated[Path, Parameter(help="Path to psi_abilities.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
) -> None:
    """Pack psi_abilities.json back to binary PSI ability table."""
    from ebtools.parsers.psi_ability import pack_psi_abilities

    if not json_path.exists():
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)

    output_path = output_dir / "data" / "psi_ability_table.bin"
    pack_psi_abilities(json_path, output_path)
    print(f"Packed PSI abilities to {output_path}")


@pack_app.command(name="teleport-destinations")
def pack_teleport_cmd(
    json_path: Annotated[Path, Parameter(help="Path to teleport_destinations.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
    *,
    commondata: Annotated[Path, Parameter(alias="-c")] = Path("commondefs.yml"),
) -> None:
    """Pack teleport_destinations.json back to binary teleport table."""
    from ebtools.parsers.teleport import pack_teleport

    if not json_path.exists():
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)

    common_data = load_common_data(commondata)
    output_path = output_dir / "data" / "teleport_destination_table.bin"
    pack_teleport(json_path, common_data, output_path)
    print(f"Packed teleport destinations to {output_path}")


@pack_app.command(name="psi-teleport-destinations")
def pack_psi_teleport_cmd(
    json_path: Annotated[Path, Parameter(help="Path to psi_teleport_destinations.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
    *,
    yaml_config: Annotated[Path, Parameter(alias="-y")] = Path("earthbound.yml"),
) -> None:
    """Pack psi_teleport_destinations.json back to binary PSI teleport table."""
    from ebtools.parsers.teleport import pack_psi_teleport

    if not json_path.exists():
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)

    doc = load_dump_doc(yaml_config)
    output_path = output_dir / "data" / "psi_teleport_dest_table.bin"
    pack_psi_teleport(json_path, doc.textTable, output_path)
    print(f"Packed PSI teleport destinations to {output_path}")


@pack_app.command(name="bg-config")
def pack_bg_config_cmd(
    json_path: Annotated[Path, Parameter(help="Path to bg_config.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
) -> None:
    """Pack bg_config.json back to binary BG data table."""
    from ebtools.parsers.bg_config import pack_bg_config

    if not json_path.exists():
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)

    output_path = output_dir / "data" / "bg_data_table.bin"
    pack_bg_config(json_path, output_path)
    print(f"Packed BG config to {output_path}")


@pack_app.command(name="exp-table")
def pack_exp_table_cmd(
    json_path: Annotated[Path, Parameter(help="Path to exp_table.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
) -> None:
    """Pack exp_table.json back to binary EXP table."""
    from ebtools.parsers.exp_table import pack_exp_table

    if not json_path.exists():
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)

    output_path = output_dir / "data" / "exp_table.bin"
    pack_exp_table(json_path, output_path)
    print(f"Packed EXP table to {output_path}")


@pack_app.command(name="stores")
def pack_stores_cmd(
    json_path: Annotated[Path, Parameter(help="Path to stores.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
    *,
    commondata: Annotated[Path, Parameter(alias="-c")] = Path("commondefs.yml"),
) -> None:
    """Pack stores.json back to binary store table."""
    from ebtools.parsers.store import pack_stores

    if not json_path.exists():
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)

    common_data = load_common_data(commondata)
    output_path = output_dir / "data" / "store_table.bin"
    pack_stores(json_path, common_data, output_path)
    print(f"Packed stores to {output_path}")


@pack_app.command(name="town-map")
def pack_town_map_cmd(
    tileset_png: Annotated[Path, Parameter(help="Path to tileset PNG")],
    arrangement_json: Annotated[Path, Parameter(help="Path to arrangement JSON")],
    pal_path: Annotated[Path, Parameter(help="Path to JASC palette file")],
    output_path: Annotated[Path, Parameter(help="Output .bin.lzhal path")],
) -> None:
    """Pack a town map from tileset PNG + arrangement JSON + palette to compressed binary."""
    from ebtools.parsers.town_map import pack_town_map

    for p in (tileset_png, arrangement_json, pal_path):
        if not p.exists():
            print(f"Error: {p} not found", file=sys.stderr)
            sys.exit(1)

    pack_town_map(tileset_png, arrangement_json, pal_path, output_path)
    print(f"Packed town map to {output_path}")


@pack_app.command(name="intro-gfx")
def pack_intro_gfx_cmd(
    name: Annotated[
        str, Parameter(help="Asset name (ape_logo, halken_logo, nintendo_logo, title_screen, gas_station)")
    ],
    tileset_png: Annotated[Path, Parameter(help="Path to tileset PNG")],
    arrangement_json: Annotated[Path, Parameter(help="Path to arrangement JSON")],
    pal_path: Annotated[Path, Parameter(help="Path to JASC palette file")],
    bin_dir: Annotated[Path, Parameter(help="Output binary asset directory (e.g. asm/bin/)")],
) -> None:
    """Pack an intro/ending graphic from tileset PNG + arrangement JSON + palette."""
    from ebtools.parsers.intro_gfx import INTRO_ASSETS, pack_intro_ending_asset

    if name not in INTRO_ASSETS:
        print(f"Error: unknown asset '{name}'. Valid: {', '.join(INTRO_ASSETS)}", file=sys.stderr)
        sys.exit(1)

    for p in (tileset_png, arrangement_json, pal_path):
        if not p.exists():
            print(f"Error: {p} not found", file=sys.stderr)
            sys.exit(1)

    gfx_stem, arr_stem, pal_stem, bpp = INTRO_ASSETS[name]
    pack_intro_ending_asset(tileset_png, arrangement_json, pal_path, bpp, bin_dir, gfx_stem, arr_stem, pal_stem)
    print(f"Packed {name} to {bin_dir}")


@pack_app.command(name="battle-bg")
def pack_battle_bg_cmd(
    tileset_png: Annotated[Path, Parameter(help="Path to tileset PNG")],
    arrangement_json: Annotated[Path, Parameter(help="Path to arrangement JSON")],
    pal_path: Annotated[Path, Parameter(help="Path to JASC palette file")],
    metadata_json: Annotated[Path, Parameter(help="Path to metadata JSON")],
    bin_dir: Annotated[Path, Parameter(help="Output binary asset directory (e.g. asm/bin/)")],
) -> None:
    """Pack a battle background from tileset PNG + arrangement JSON + palette + metadata."""
    from ebtools.parsers.battle_bg import pack_battle_bg

    for p in (tileset_png, arrangement_json, pal_path, metadata_json):
        if not p.exists():
            print(f"Error: {p} not found", file=sys.stderr)
            sys.exit(1)

    pack_battle_bg(tileset_png, arrangement_json, pal_path, metadata_json, bin_dir)
    print(f"Packed battle background to {bin_dir}")


@pack_app.command(name="font")
def pack_font_cmd(
    name: Annotated[str, Parameter(help="Font name (normal, battle, tiny, large, mrsaturn)")],
    png_path: Annotated[Path, Parameter(help="Path to font PNG")],
    json_path: Annotated[Path, Parameter(help="Path to font metadata JSON")],
    bin_dir: Annotated[Path, Parameter(help="Output binary asset directory (e.g. asm/bin/)")],
) -> None:
    """Pack a font PNG + JSON back to binary gfx + width files."""
    from ebtools.parsers.font import FONTS, pack_font

    if name not in FONTS:
        print(f"Error: unknown font '{name}'. Valid: {', '.join(FONTS)}", file=sys.stderr)
        sys.exit(1)

    for p in (png_path, json_path):
        if not p.exists():
            print(f"Error: {p} not found", file=sys.stderr)
            sys.exit(1)

    gfx_bytes, width_bytes = pack_font(png_path, json_path)

    info = FONTS[name]
    gfx_out = bin_dir / info["gfx"]
    width_out = bin_dir / info["widths"]
    gfx_out.parent.mkdir(parents=True, exist_ok=True)
    width_out.parent.mkdir(parents=True, exist_ok=True)
    gfx_out.write_bytes(gfx_bytes)
    width_out.write_bytes(width_bytes)
    print(f"Packed font '{name}' to {gfx_out} and {width_out}")


@pack_app.command(name="swirls")
def pack_swirls_cmd(
    json_path: Annotated[Path, Parameter(help="Path to swirls.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed swirl files")],
) -> None:
    """Pack swirls.json back to individual .swirl binary files."""
    from ebtools.parsers.swirl import pack_swirls

    if not json_path.exists():
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)

    pack_swirls(json_path, output_dir)
    print(f"Packed swirls to {output_dir}")


@pack_app.command(name="music")
def pack_music_cmd(
    json_path: Annotated[Path, Parameter(help="Path to music_tracks.json")],
    output_dir: Annotated[Path, Parameter(help="Output directory for packed binary")],
) -> None:
    """Pack music_tracks.json back to binary dataset table."""
    from ebtools.parsers.music import pack_music_dataset

    if not json_path.exists():
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)

    output_path = output_dir / "music" / "dataset_table.bin"
    pack_music_dataset(json_path, output_path)
    print(f"Packed music dataset table to {output_path}")


@pack_app.command(name="arrangements-bundled")
def pack_arrangements_bundled_cmd(
    input_dir: Annotated[Path, Parameter(help="Directory containing .arr.lzhal files")],
    output_dir: Annotated[Path, Parameter(help="Output directory for .arr.bundled files")],
) -> None:
    """Bundle PSI arrangement frames into independently-compressed 8-frame chunks.

    Reads .arr.lzhal files from input_dir, splits each into 8-frame bundles,
    compresses each bundle independently with HALLZ2, and writes .arr.bundled
    files to output_dir.

    Parameters
    ----------
    input_dir
        Directory containing N.arr.lzhal files (e.g. asm/bin/psianims/arrangements/).
    output_dir
        Output directory for N.arr.bundled files.
    """
    from ebtools.parsers.psi_arrangements import pack_bundled_arrangements

    if not input_dir.is_dir():
        print(f"Error: {input_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    count = pack_bundled_arrangements(input_dir, output_dir)
    print(f"Packed {count} bundled PSI arrangements to {output_dir}")
