"""Code-generation commands: produce C headers and bootstrap JSON from source data."""

import sys
from pathlib import Path
from typing import Annotated

from cyclopts import App, Parameter

generate_app = App(name="generate", help="Generate C headers and bootstrap data files.")


@generate_app.command(name="items-header")
def generate_items_header_cmd(
    items_json: Annotated[Path, Parameter(help="Path to items.json")],
    output: Annotated[Path, Parameter(help="Path for generated header file")],
) -> None:
    """Generate items_generated.h from items.json."""
    from ebtools.parsers.item import generate_items_header

    if not items_json.exists():
        print(f"Error: {items_json} not found", file=sys.stderr)
        sys.exit(1)

    generate_items_header(items_json, output)


@generate_app.command(name="music-header")
def generate_music_header_cmd(
    music_json: Annotated[Path, Parameter(help="Path to music_tracks.json")],
    output: Annotated[Path, Parameter(help="Path for generated header file")],
) -> None:
    """Generate music_generated.h from music_tracks.json."""
    from ebtools.parsers.music import generate_music_header

    if not music_json.exists():
        print(f"Error: {music_json} not found", file=sys.stderr)
        sys.exit(1)

    generate_music_header(music_json, output)


@generate_app.command(name="bootstrap-music-json")
def bootstrap_music_json_cmd(
    asm_path: Annotated[Path, Parameter(help="Path to include/constants/music.asm")],
    output: Annotated[Path, Parameter(help="Path for output music_tracks.json")],
) -> None:
    """One-time bootstrap: parse music.asm enum into music_tracks.json."""
    from ebtools.parsers.music import bootstrap_music_json

    if not asm_path.exists():
        print(f"Error: {asm_path} not found", file=sys.stderr)
        sys.exit(1)

    bootstrap_music_json(asm_path, output)


@generate_app.command(name="enemies-header")
def generate_enemies_header_cmd(
    enemies_json: Annotated[Path, Parameter(help="Path to enemies.json")],
    output: Annotated[Path, Parameter(help="Path for generated header file")],
) -> None:
    """Generate enemies_generated.h from enemies.json."""
    from ebtools.parsers.enemy import generate_enemies_header

    if not enemies_json.exists():
        print(f"Error: {enemies_json} not found", file=sys.stderr)
        sys.exit(1)

    generate_enemies_header(enemies_json, output)
