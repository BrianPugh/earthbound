"""Town map tilemap export/pack.

Each town map is 18496 bytes decompressed:
  - 64 bytes: BGR555 palette (32 colors)
  - 16384 bytes: 512 × 4bpp tiles (32 bytes each)
  - 2048 bytes: 32×32 tilemap arrangement (1024 × 16-bit entries)

There are 6 town maps split between locale-specific and common directories.

Export format: tileset PNG + arrangement JSON + JASC palette.
"""

from pathlib import Path

from ebtools.hallz import compress, decompress
from ebtools.parsers._tiles import (
    TILESET_COLS,
    decode_4bpp_tile,
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

PAL_SIZE = 64
TILE_DATA_SIZE = 16384
ARR_SIZE = 2048
TOTAL_SIZE = PAL_SIZE + TILE_DATA_SIZE + ARR_SIZE  # 18496

MAP_TILES = 32  # 32×32


def _parse_town_map(data: bytes) -> tuple[bytes, bytes, bytes]:
    """Split decompressed town map data into (pal, tiles, arr)."""
    return (
        data[:PAL_SIZE],
        data[PAL_SIZE : PAL_SIZE + TILE_DATA_SIZE],
        data[PAL_SIZE + TILE_DATA_SIZE : PAL_SIZE + TILE_DATA_SIZE + ARR_SIZE],
    )


def export_all_town_maps(bin_dir: Path, output_dir: Path) -> int:
    """Export all 6 town maps as tileset PNG + arrangement JSON + palette.

    Returns the number of maps exported.
    """
    maps_dir = output_dir / "town_maps"
    maps_dir.mkdir(parents=True, exist_ok=True)
    count = 0

    for i in range(6):
        us_path = bin_dir / f"US/town_maps/{i}.bin.lzhal"
        common_path = bin_dir / f"town_maps/{i}.bin.lzhal"
        path = us_path if us_path.exists() else common_path

        if not path.exists():
            continue

        try:
            data = decompress(path.read_bytes())
        except Exception:  # noqa: S112
            continue

        if len(data) < TOTAL_SIZE:
            continue

        pal_data, tile_data, arr_data = _parse_town_map(data)

        palette_rgba = load_bgr555_palette(pal_data)

        # Decode tiles
        tiles = []
        for t in range(len(tile_data) // 32):
            tiles.append(decode_4bpp_tile(tile_data, t * 32))

        # Tileset PNG
        img = tiles_to_indexed_png(tiles, palette_rgba)
        img.save(maps_dir / f"{i}_tileset.png")

        # Arrangement JSON
        entries = parse_arrangement_entries(arr_data, MAP_TILES * MAP_TILES)
        write_arrangement_json(
            maps_dir / f"{i}_arrangement.json",
            MAP_TILES,
            MAP_TILES,
            entries,
            tile_count=len(tiles),
        )

        # JASC palette
        palette_rgb = [(c[0], c[1], c[2]) for c in palette_rgba]
        write_jasc_pal(palette_rgb, maps_dir / f"{i}.pal")

        count += 1

    return count


def pack_town_map(tileset_png: Path, arrangement_json: Path, pal_path: Path, output_path: Path) -> None:
    """Pack a town map from tileset PNG + arrangement JSON + palette to compressed binary.

    Output is HALLZ2-compressed binary: palette(64B) + tiles(16384B) + arrangement(2048B).
    """
    from PIL import Image

    # Read palette
    colors = load_jasc_pal(pal_path)
    pal_bytes = encode_bgr555_palette(colors)

    # Read tileset
    img = Image.open(tileset_png)
    tiles = indexed_png_to_tiles(img, TILESET_COLS)

    # Encode tiles to 4bpp
    tile_data = bytearray()
    for tile in tiles:
        tile_data.extend(encode_4bpp_tile(tile))

    # Pad or truncate to expected size
    if len(tile_data) < TILE_DATA_SIZE:
        tile_data.extend(b"\x00" * (TILE_DATA_SIZE - len(tile_data)))
    else:
        tile_data = tile_data[:TILE_DATA_SIZE]

    # Read arrangement
    _w, _h, entries, _tpr, tile_count = read_arrangement_json(arrangement_json)

    # Trim padding tiles from PNG grid alignment
    if tile_count is not None:
        tiles = tiles[:tile_count]
        tile_data = bytearray()
        for tile in tiles:
            tile_data.extend(encode_4bpp_tile(tile))
    arr_bytes = encode_arrangement_entries(entries)

    # Pad arrangement
    arr_bytes = arr_bytes + b"\x00" * (ARR_SIZE - len(arr_bytes)) if len(arr_bytes) < ARR_SIZE else arr_bytes[:ARR_SIZE]

    # Combine and compress
    raw = bytes(pal_bytes) + bytes(tile_data) + bytes(arr_bytes)
    compressed = compress(raw)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(compressed)
