"""Battle background export/pack: gfx + arrangement + palette.

Renders the 32×32 tile arrangement (256×256 pixels) using the specified
graphics and palette. Supports both 2bpp and 4bpp tile formats.

Export format: tileset PNG + arrangement JSON + JASC palette + metadata JSON.
"""

from pathlib import Path

from pydantic import BaseModel

from ebtools.byte_reader import ByteReader
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
TILE_SIZE = 8
IMAGE_WIDTH = TILEMAP_WIDTH * TILE_SIZE  # 256
IMAGE_HEIGHT = TILEMAP_HEIGHT * TILE_SIZE  # 256


class BattleBackgroundMetadata(BaseModel):
    """JSON sidecar metadata for a battle background."""

    config_index: int
    graphics_index: int
    arrangement_index: int
    palette_index: int
    bits_per_pixel: int


def export_battle_bgs(
    bin_dir: Path,
    bg_config_data: bytes,
    output_dir: Path,
) -> int:
    """Export all battle backgrounds as tileset PNG + arrangement JSON + palette.

    Returns the number of backgrounds exported.
    """
    gfx_dir = bin_dir / "battle_bgs" / "graphics"
    arr_dir = bin_dir / "battle_bgs" / "arrangements"
    pal_dir = bin_dir / "battle_bgs" / "palettes"

    if not gfx_dir.exists() or not arr_dir.exists() or not pal_dir.exists():
        return 0

    bgs_dir = output_dir / "battle_bgs"
    bgs_dir.mkdir(parents=True, exist_ok=True)

    rendered: set[tuple[int, int, int, int]] = set()
    count = 0
    num_entries = len(bg_config_data) // 17

    for idx in range(num_entries):
        r = ByteReader(bg_config_data[idx * 17 : (idx + 1) * 17])
        gfx_idx = r.read_byte()
        pal_idx = r.read_byte()
        bpp = r.read_byte()
        _ = r.read_byte()  # palette_animation_type
        _ = r.read_byte()  # palette_animation_param_1
        _ = r.read_byte()  # palette_animation_param_2
        _ = r.read_byte()  # scroll_type
        _ = r.read_byte()  # scroll_speed_h
        _ = r.read_byte()  # scroll_speed_v
        _ = r.read_byte()  # distortion_type
        _ = r.read_byte()  # distortion_param
        arr_idx = r.read_byte()

        key = (gfx_idx, arr_idx, pal_idx, bpp)
        if key in rendered:
            continue

        gfx_path = gfx_dir / f"{gfx_idx}.gfx.lzhal"
        arr_path = arr_dir / f"{arr_idx}.arr.lzhal"
        pal_path = pal_dir / f"{pal_idx}.pal"

        if not gfx_path.exists() or not arr_path.exists() or not pal_path.exists():
            continue

        try:
            gfx_data = decompress(gfx_path.read_bytes())
            arr_data = decompress(arr_path.read_bytes())
        except Exception:  # noqa: S112
            continue

        pal_data = pal_path.read_bytes()

        tile_bytes = 16 if bpp == 2 else 32
        num_tiles = len(gfx_data) // tile_bytes

        # Decode tiles
        tiles = []
        for t in range(num_tiles):
            if bpp == 2:
                tiles.append(decode_2bpp_tile(gfx_data, t * 16))
            else:
                tiles.append(decode_4bpp_tile(gfx_data, t * 32))

        palette_rgba = load_bgr555_palette(pal_data)

        prefix = f"{idx:03d}"

        # Tileset PNG
        img = tiles_to_indexed_png(tiles, palette_rgba)
        img.save(bgs_dir / f"{prefix}_tileset.png")

        # Arrangement JSON
        entries = parse_arrangement_entries(arr_data, TILEMAP_WIDTH * TILEMAP_HEIGHT)
        write_arrangement_json(
            bgs_dir / f"{prefix}_arrangement.json",
            TILEMAP_WIDTH,
            TILEMAP_HEIGHT,
            entries,
            tile_count=len(tiles),
        )

        # JASC palette
        palette_rgb = [(c[0], c[1], c[2]) for c in palette_rgba]
        write_jasc_pal(palette_rgb, bgs_dir / f"{prefix}.pal")

        # Metadata JSON
        meta = BattleBackgroundMetadata(
            config_index=idx,
            graphics_index=gfx_idx,
            arrangement_index=arr_idx,
            palette_index=pal_idx,
            bits_per_pixel=bpp,
        )
        (bgs_dir / f"{prefix}.json").write_text(meta.model_dump_json(indent=2) + "\n")

        rendered.add(key)
        count += 1

    return count


def pack_battle_bg(
    tileset_png: Path,
    arrangement_json: Path,
    pal_path: Path,
    metadata_json: Path,
    bin_dir: Path,
) -> None:
    """Pack a single battle background from tileset PNG + arrangement JSON + palette.

    Reads the metadata JSON to determine asset indices and bpp, then writes
    compressed .gfx.lzhal, .arr.lzhal, and raw .pal files under bin_dir.
    """
    import json

    from PIL import Image

    meta = BattleBackgroundMetadata(**json.loads(metadata_json.read_text()))

    # Read palette
    colors = load_jasc_pal(pal_path)
    pal_bytes = encode_bgr555_palette(colors)

    # Read tileset
    img = Image.open(tileset_png)
    tiles = indexed_png_to_tiles(img, TILESET_COLS)

    # Encode tiles
    encode_fn = encode_2bpp_tile if meta.bits_per_pixel == 2 else encode_4bpp_tile
    tile_data = bytearray()
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

    # Write outputs
    gfx_dir = bin_dir / "battle_bgs" / "graphics"
    arr_dir = bin_dir / "battle_bgs" / "arrangements"
    pal_dir = bin_dir / "battle_bgs" / "palettes"

    for d in (gfx_dir, arr_dir, pal_dir):
        d.mkdir(parents=True, exist_ok=True)

    gfx_dir_file = gfx_dir / f"{meta.graphics_index}.gfx.lzhal"
    arr_dir_file = arr_dir / f"{meta.arrangement_index}.arr.lzhal"
    pal_dir_file = pal_dir / f"{meta.palette_index}.pal"

    gfx_dir_file.write_bytes(compress(bytes(tile_data)))
    arr_dir_file.write_bytes(compress(arr_bytes))
    pal_dir_file.write_bytes(pal_bytes)
