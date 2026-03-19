"""Intro/ending graphics export/pack: logo and title screen assets.

Export format: tileset PNG + arrangement JSON + JASC palette.
Each asset has separate .gfx.lzhal, .arr.lzhal, .pal.lzhal files.
"""

from pathlib import Path

from ebtools.hallz import compress, decompress
from ebtools.parsers._tiles import (
    TILESET_COLS,
    decode_2bpp_tile,
    decode_4bpp_tile,
    encode_2bpp_tile,
    encode_4bpp_tile,
    encode_arrangement_entries,
    encode_bgr555_palette,
    indexed_png_to_tiles,
    load_bgr555_palette,
    load_jasc_pal,
    parse_arrangement_entries,
    read_arrangement_json,
    tiles_to_indexed_png,
    write_arrangement_json,
    write_jasc_pal,
)

TILEMAP_WIDTH = 32
TILEMAP_HEIGHT = 32


def _try_decompress(path: Path) -> bytes | None:
    """Read and decompress a .lzhal file, returning None if missing."""
    if not path.exists():
        return None
    try:
        return decompress(path.read_bytes())
    except Exception:
        return None


def _export_tilemap_asset(
    gfx_data: bytes,
    arr_data: bytes,
    pal_data: bytes,
    bpp: int,
    out_dir: Path,
    name: str,
    width_tiles: int = TILEMAP_WIDTH,
    height_tiles: int = TILEMAP_HEIGHT,
) -> None:
    """Export a single tilemap asset as tileset PNG + arrangement JSON + palette."""
    tile_bytes = 16 if bpp == 2 else 32
    num_tiles = len(gfx_data) // tile_bytes

    tiles = []
    for t in range(num_tiles):
        if bpp == 2:
            tiles.append(decode_2bpp_tile(gfx_data, t * 16))
        else:
            tiles.append(decode_4bpp_tile(gfx_data, t * 32))

    palette_rgba = load_bgr555_palette(pal_data)

    out_dir.mkdir(parents=True, exist_ok=True)

    # Tileset PNG
    img = tiles_to_indexed_png(tiles, palette_rgba)
    img.save(out_dir / f"{name}_tileset.png")

    # Arrangement JSON
    entries = parse_arrangement_entries(arr_data, width_tiles * height_tiles)
    write_arrangement_json(
        out_dir / f"{name}_arrangement.json",
        width_tiles,
        height_tiles,
        entries,
        tile_count=len(tiles),
    )

    # JASC palette
    palette_rgb = [(c[0], c[1], c[2]) for c in palette_rgba]
    write_jasc_pal(palette_rgb, out_dir / f"{name}.pal")


def export_intro_ending_gfx(bin_dir: Path, output_dir: Path) -> int:
    """Export intro and ending graphics as tileset PNG + arrangement JSON + palette.

    Returns the number of images exported.
    """
    count = 0

    # Logo assets (locale-independent + US-specific)
    logos = [
        ("ape_logo", "intro/logos/ape", "intro/logos/ape", "intro/logos/ape", 4),
        ("halken_logo", "intro/logos/halken", "intro/logos/halken", "intro/logos/halken", 4),
        ("nintendo_logo", "US/intro/logos/nintendo", "US/intro/logos/nintendo", "intro/logos/nintendo", 4),
    ]

    for name, gfx_stem, arr_stem, pal_stem, bpp in logos:
        gfx = _try_decompress(bin_dir / f"{gfx_stem}.gfx.lzhal")
        arr = _try_decompress(bin_dir / f"{arr_stem}.arr.lzhal")
        pal = _try_decompress(bin_dir / f"{pal_stem}.pal.lzhal")

        if gfx is None or arr is None or pal is None:
            continue

        out_dir = output_dir / "intro"
        _export_tilemap_asset(gfx, arr, pal, bpp, out_dir, name)
        count += 1

    # Title screen
    title_gfx = _try_decompress(bin_dir / "US/intro/title_screen.gfx.lzhal")
    title_arr = _try_decompress(bin_dir / "US/intro/title_screen.arr.lzhal")
    title_pal = _try_decompress(bin_dir / "US/intro/title_screen.pal.lzhal")

    if title_gfx and title_arr and title_pal:
        out_dir = output_dir / "intro"
        _export_tilemap_asset(title_gfx, title_arr, title_pal, 4, out_dir, "title_screen")
        count += 1

    # Gas station
    gas_gfx = _try_decompress(bin_dir / "US/intro/gas_station.gfx.lzhal")
    gas_arr = _try_decompress(bin_dir / "US/intro/gas_station.arr.lzhal")
    gas_pal = _try_decompress(bin_dir / "intro/gas_station.pal.lzhal")

    if gas_gfx and gas_arr and gas_pal:
        out_dir = output_dir / "intro"
        _export_tilemap_asset(gas_gfx, gas_arr, gas_pal, 4, out_dir, "gas_station")
        count += 1

    return count


# Asset definitions mapping name → (gfx_stem, arr_stem, pal_stem, bpp)
INTRO_ASSETS = {
    "ape_logo": ("intro/logos/ape", "intro/logos/ape", "intro/logos/ape", 4),
    "halken_logo": ("intro/logos/halken", "intro/logos/halken", "intro/logos/halken", 4),
    "nintendo_logo": ("US/intro/logos/nintendo", "US/intro/logos/nintendo", "intro/logos/nintendo", 4),
    "title_screen": ("US/intro/title_screen", "US/intro/title_screen", "US/intro/title_screen", 4),
    "gas_station": ("US/intro/gas_station", "US/intro/gas_station", "intro/gas_station", 4),
}


def pack_intro_ending_asset(
    tileset_png: Path,
    arrangement_json: Path,
    pal_path: Path,
    bpp: int,
    bin_dir: Path,
    gfx_stem: str,
    arr_stem: str,
    pal_stem: str,
) -> None:
    """Pack a single intro/ending tilemap asset back to compressed binary files.

    Writes .gfx.lzhal, .arr.lzhal, .pal.lzhal to the appropriate paths under bin_dir.
    """
    from PIL import Image

    # Read palette
    colors = load_jasc_pal(pal_path)
    pal_bytes = encode_bgr555_palette(colors)

    # Read tileset
    img = Image.open(tileset_png)
    tiles = indexed_png_to_tiles(img, TILESET_COLS)

    # Encode tiles
    tile_data = bytearray()
    encode_fn = encode_2bpp_tile if bpp == 2 else encode_4bpp_tile
    for tile in tiles:
        tile_data.extend(encode_fn(tile))

    # Read arrangement
    _w, _h, entries, _tpr, tile_count = read_arrangement_json(arrangement_json)

    # Trim padding tiles from PNG grid alignment
    if tile_count is not None:
        tiles = tiles[:tile_count]
        tile_data = bytearray()
        for tile in tiles:
            tile_data.extend(encode_fn(tile))

    arr_bytes = encode_arrangement_entries(entries)

    # Compress and write
    gfx_out = bin_dir / f"{gfx_stem}.gfx.lzhal"
    arr_out = bin_dir / f"{arr_stem}.arr.lzhal"
    pal_out = bin_dir / f"{pal_stem}.pal.lzhal"

    for p in (gfx_out, arr_out, pal_out):
        p.parent.mkdir(parents=True, exist_ok=True)

    gfx_out.write_bytes(compress(bytes(tile_data)))
    arr_out.write_bytes(compress(arr_bytes))
    pal_out.write_bytes(compress(pal_bytes))
