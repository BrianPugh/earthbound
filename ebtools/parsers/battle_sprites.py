"""Battle sprite PNG export and binary pack utilities.

Reads the battle_sprites_pointers.bin table and per-sprite .gfx.lzhal files
to generate indexed PNG images with JSON metadata.  Also provides the inverse:
packing PNGs back to compressed LZHAL binary files.
"""

import struct
import sys
from collections import Counter
from pathlib import Path

from PIL import Image
from pydantic import BaseModel, ConfigDict, Field, ValidationError, computed_field

from ebtools.hallz import compress, decompress
from ebtools.parsers._tiles import (
    PackError,
    decode_4bpp_tile,
    encode_4bpp_tile,
    encode_bgr555_palette,
    load_bgr555_palette,
    make_indexed_png,
    write_jasc_pal,
)

# Battle sprite size enum -> (width_tiles, height_tiles)
SPRITE_SIZES: dict[int, tuple[int, int]] = {
    1: (4, 4),  # 32x32
    2: (8, 4),  # 64x32
    3: (4, 8),  # 32x64
    4: (8, 8),  # 64x64
    5: (16, 8),  # 128x64
    6: (16, 16),  # 128x128
}

# Number of battle sprite palettes
_NUM_PALETTES = 32

# Locale-specific sprite IDs (have US/ overrides)
_LOCALE_SPRITE_IDS = {23, 62}

# Enemy configuration table: ROM address and record layout
_ENEMY_CONFIG_ROM_ADDR = 0xD59589
_ENEMY_CONFIG_ROM_OFFSET = _ENEMY_CONFIG_ROM_ADDR - 0xC00000  # = 0x159589
_ENEMY_CONFIG_RECORD_SIZE = 94
_ENEMY_CONFIG_NUM_ENTRIES = 231
_ENEMY_CONFIG_BATTLE_SPRITE_OFFSET = 28  # uint16 LE
_ENEMY_CONFIG_PALETTE_OFFSET = 53  # uint8


def _generate_arrangement(width: int, height: int) -> list[int]:
    """Generate tile arrangement matching D source's generateArr.

    Battle sprites store tiles in 4x4 blocks in a specific interleaved order.
    """
    arr = [[0] * width for _ in range(height)]
    num_blocks = (width * height) // 16
    for i in range(num_blocks):
        start_x = ((i * 4) // width) * 4
        start_y = (i * 4) % width
        for x in range(4):
            for y in range(4):
                arr[start_x + x][start_y + y] = x * 4 + y + i * 16
    # Flatten
    return [tile for row in arr for tile in row]


# ---------------------------------------------------------------------------
# Pydantic models
# ---------------------------------------------------------------------------


class BattleSprite(BaseModel):
    """Parsed battle sprite pointer table entry."""

    model_config = ConfigDict(frozen=True)

    sprite_id: int
    size_enum: int
    rom_pointer: int = Field(exclude=True)

    @computed_field
    @property
    def tile_width(self) -> int:
        if self.size_enum in SPRITE_SIZES:
            return SPRITE_SIZES[self.size_enum][0]
        return 0

    @computed_field
    @property
    def tile_height(self) -> int:
        if self.size_enum in SPRITE_SIZES:
            return SPRITE_SIZES[self.size_enum][1]
        return 0

    @computed_field
    @property
    def pixel_width(self) -> int:
        return self.tile_width * 8

    @computed_field
    @property
    def pixel_height(self) -> int:
        return self.tile_height * 8


class BattleSpriteMetadata(BaseModel):
    """Validation schema for battle sprite JSON sidecar files (used by pack)."""

    sprite_id: int = Field(ge=0)
    name: str = ""
    size_enum: int = Field(ge=1, le=6)
    tile_width: int
    tile_height: int
    pixel_width: int
    pixel_height: int
    palette_index: int = Field(ge=0, le=31)


# ---------------------------------------------------------------------------
# Sprite <-> palette mapping from enemy config
# ---------------------------------------------------------------------------


def build_sprite_palette_map(full_rom: bytes) -> dict[int, int]:
    """Parse the enemy configuration table to find the most common palette per battle sprite.

    Returns dict mapping sprite_id -> palette_index.
    """
    sprite_palettes: dict[int, list[int]] = {}

    for i in range(_ENEMY_CONFIG_NUM_ENTRIES):
        record_offset = _ENEMY_CONFIG_ROM_OFFSET + i * _ENEMY_CONFIG_RECORD_SIZE
        if record_offset + _ENEMY_CONFIG_RECORD_SIZE > len(full_rom):
            break

        battle_sprite = struct.unpack_from("<H", full_rom, record_offset + _ENEMY_CONFIG_BATTLE_SPRITE_OFFSET)[0]
        palette = full_rom[record_offset + _ENEMY_CONFIG_PALETTE_OFFSET]

        if battle_sprite == 0:
            continue

        if battle_sprite not in sprite_palettes:
            sprite_palettes[battle_sprite] = []
        sprite_palettes[battle_sprite].append(palette)

    # Pick the most common palette for each sprite
    result: dict[int, int] = {}
    for sprite_id, palettes in sprite_palettes.items():
        counter = Counter(palettes)
        result[sprite_id] = counter.most_common(1)[0][0]

    return result


# ---------------------------------------------------------------------------
# Pointer table parsing
# ---------------------------------------------------------------------------


def _parse_pointer_table(ptr_data: bytes) -> list[BattleSprite]:
    """Parse battle_sprites_pointers.bin into BattleSprite models.

    Each entry is 5 bytes: uint32 LE ROM pointer + uint8 size_enum.
    """
    num_entries = len(ptr_data) // 5
    sprites: list[BattleSprite] = []
    for idx in range(num_entries):
        base = idx * 5
        ptr = struct.unpack_from("<I", ptr_data, base)[0]
        size_enum = ptr_data[base + 4]
        sprites.append(BattleSprite(sprite_id=idx, size_enum=size_enum, rom_pointer=ptr))
    return sprites


# ---------------------------------------------------------------------------
# PNG export
# ---------------------------------------------------------------------------


def _decode_battle_sprite(
    decompressed: bytes,
    width_tiles: int,
    height_tiles: int,
) -> list[list[int]]:
    """Decode decompressed battle sprite data to a pixel grid.

    Uses the 4x4 block tile arrangement.
    """
    arrangement = _generate_arrangement(width_tiles, height_tiles)
    width_px = width_tiles * 8
    height_px = height_tiles * 8
    pixels = [[0] * width_px for _ in range(height_px)]

    for tile_y in range(height_tiles):
        for tile_x in range(width_tiles):
            tile_idx = arrangement[tile_y * width_tiles + tile_x]
            tile_pixels = decode_4bpp_tile(decompressed, tile_idx * 32)
            for py in range(8):
                for px in range(8):
                    pixels[tile_y * 8 + py][tile_x * 8 + px] = tile_pixels[py][px]

    return pixels


def export_all_battle_sprites(
    bin_path: Path,
    asset_output_dir: Path,
    sprite_palette_map: dict[int, int],
) -> int:
    """Export all battle sprites as indexed PNG images with JSON metadata.

    Parameters
    ----------
    bin_path
        Path to extracted binary assets (e.g. asm/bin/).
    asset_output_dir
        Output directory for PNGs and JSON (e.g. src/assets/).
    sprite_palette_map
        Mapping of sprite_id -> palette_index from enemy config.

    Returns the number of sprites exported.
    """
    sprite_dir = bin_path / "battle_sprites"
    out_dir = asset_output_dir / "battle_sprites"

    # Load pointer table
    ptr_path = bin_path / "data" / "battle_sprites_pointers.bin"
    if not ptr_path.exists():
        print(f"  Skipping battle sprite export: missing {ptr_path}")
        return 0

    ptr_data = ptr_path.read_bytes()
    sprites = _parse_pointer_table(ptr_data)

    # Load palettes
    palettes: dict[int, list[tuple[int, int, int, int]]] = {}
    for i in range(_NUM_PALETTES):
        pal_path = sprite_dir / "palettes" / f"{i}.pal"
        if pal_path.exists():
            palettes[i] = load_bgr555_palette(pal_path.read_bytes())

    # Export JASC-PAL palette files
    jasc_dir = out_dir / "palettes"
    jasc_dir.mkdir(parents=True, exist_ok=True)
    for pal_idx, pal_rgba in palettes.items():
        rgb_colors = [(r, g, b) for r, g, b, _a in pal_rgba]
        write_jasc_pal(rgb_colors, jasc_dir / f"{pal_idx}.pal")

    out_dir.mkdir(parents=True, exist_ok=True)
    count = 0

    for bs in sprites:
        if bs.size_enum not in SPRITE_SIZES:
            continue

        # Load compressed data (check locale-specific first)
        lzhal_path = None
        if bs.sprite_id in _LOCALE_SPRITE_IDS:
            locale_path = bin_path / "US" / "battle_sprites" / f"{bs.sprite_id}.gfx.lzhal"
            if locale_path.exists():
                lzhal_path = locale_path
        if lzhal_path is None:
            lzhal_path = sprite_dir / f"{bs.sprite_id}.gfx.lzhal"

        if not lzhal_path.exists():
            continue

        compressed = lzhal_path.read_bytes()
        decompressed = decompress(compressed)

        pixels = _decode_battle_sprite(decompressed, bs.tile_width, bs.tile_height)

        # Get palette
        pal_idx = sprite_palette_map.get(bs.sprite_id, 0)
        palette = palettes.get(pal_idx)
        if palette is None:
            palette = palettes.get(0)
        if palette is None:
            palette = [(i * 17, i * 17, i * 17, 0 if i == 0 else 255) for i in range(16)]

        img = make_indexed_png(pixels, palette, bs.pixel_width, bs.pixel_height)

        name = f"{bs.sprite_id:03d}"
        png_path = out_dir / f"{name}.png"
        img.save(str(png_path), "PNG", transparency=0)

        meta = BattleSpriteMetadata(
            sprite_id=bs.sprite_id,
            name=name,
            size_enum=bs.size_enum,
            tile_width=bs.tile_width,
            tile_height=bs.tile_height,
            pixel_width=bs.pixel_width,
            pixel_height=bs.pixel_height,
            palette_index=pal_idx,
        )
        json_path = out_dir / f"{name}.json"
        json_path.write_text(meta.model_dump_json(indent=2) + "\n")

        count += 1

    return count


# ---------------------------------------------------------------------------
# PNG pack (repack)
# ---------------------------------------------------------------------------


def _encode_battle_sprite(
    pixels: list[list[int]],
    width_tiles: int,
    height_tiles: int,
) -> bytes:
    """Encode a pixel grid to battle sprite tile data (4x4 block arrangement).

    Inverse of _decode_battle_sprite: reads pixels in display order and
    writes tiles in the interleaved storage order.
    """
    num_tiles = width_tiles * height_tiles
    tile_data = bytearray(num_tiles * 32)
    arrangement = _generate_arrangement(width_tiles, height_tiles)

    for tile_y in range(height_tiles):
        for tile_x in range(width_tiles):
            # Extract 8x8 pixel block
            tile_pixels = []
            for py in range(8):
                row = []
                for px in range(8):
                    y = tile_y * 8 + py
                    x = tile_x * 8 + px
                    row.append(pixels[y][x] if y < len(pixels) and x < len(pixels[0]) else 0)
                tile_pixels.append(row)

            # Get the storage index for this tile position
            storage_idx = arrangement[tile_y * width_tiles + tile_x]
            encoded = encode_4bpp_tile(tile_pixels)
            offset = storage_idx * 32
            tile_data[offset : offset + 32] = encoded

    return bytes(tile_data)


def pack_battle_sprites(
    png_dir: Path,
    bin_path: Path,
    output_dir: Path,
) -> None:
    """Pack modified PNG images back to compressed LZHAL battle sprite format.

    Reads PNG + JSON from png_dir, encodes as 4BPP tile data with the battle
    sprite arrangement, compresses with LZHAL, and writes .gfx.lzhal files
    to output_dir.

    Raises PackError if any input files are invalid.

    Parameters
    ----------
    png_dir
        Directory containing modified PNG + JSON files.
    bin_path
        Path to original extracted binary assets (asm/bin/).
    output_dir
        Output directory for generated binary overrides.
    """
    sprite_dir = bin_path / "battle_sprites"

    # Load pointer table for validation
    ptr_path = bin_path / "data" / "battle_sprites_pointers.bin"
    ptr_data = ptr_path.read_bytes()
    sprites = _parse_pointer_table(ptr_data)
    sprite_by_id = {bs.sprite_id: bs for bs in sprites}

    # Load original palettes for comparison
    original_palettes: dict[int, bytes] = {}
    for i in range(_NUM_PALETTES):
        pal_path = sprite_dir / "palettes" / f"{i}.pal"
        if pal_path.exists():
            original_palettes[i] = pal_path.read_bytes()

    # Find all JSON files
    json_files = sorted(png_dir.glob("*.json"))
    if not json_files:
        print("No battle sprite JSON files found in", png_dir)
        return

    errors: list[str] = []
    # Track palette changes: palette_index -> (rgb_colors, source_filename)
    extracted_palettes: dict[int, tuple[list[tuple[int, int, int]], str]] = {}
    packed_sprites: list[tuple[str, bytes, bool]] = []  # (filename, data, is_locale)

    for json_path in json_files:
        png_path = json_path.with_suffix(".png")
        fname = json_path.stem

        # --- Validate JSON ---
        if not png_path.exists():
            errors.append(f"{json_path.name}: no matching PNG file ({png_path.name})")
            continue

        try:
            meta = BattleSpriteMetadata.model_validate_json(json_path.read_bytes())
        except ValidationError as e:
            errors.append(f"{json_path.name}: {e}")
            continue

        sprite_id = meta.sprite_id
        bs = sprite_by_id.get(sprite_id)
        if bs is None:
            errors.append(f"{json_path.name}: sprite_id {sprite_id} not found in pointer table")
            continue

        if meta.size_enum != bs.size_enum:
            errors.append(
                f"{json_path.name}: size_enum {meta.size_enum} doesn't match pointer table size_enum {bs.size_enum}"
            )
            continue

        # --- Validate PNG ---
        img = Image.open(png_path)

        if img.mode != "P":
            errors.append(
                f"{png_path.name}: must be indexed/paletted PNG (mode 'P'), "
                f"got mode '{img.mode}'. Re-save as an indexed PNG with 16 colors."
            )
            continue

        expected_w = bs.pixel_width
        expected_h = bs.pixel_height
        if img.width != expected_w or img.height != expected_h:
            errors.append(
                f"{png_path.name}: wrong dimensions {img.width}x{img.height}, expected {expected_w}x{expected_h}"
            )
            continue

        img_data = list(img.getdata())
        oob_pixels = [v for v in img_data if v > 15]
        if oob_pixels:
            max_val = max(oob_pixels)
            count = len(oob_pixels)
            errors.append(
                f"{png_path.name}: {count} pixel(s) have color index > 15 (max found: {max_val}). "
                f"SNES 4bpp sprites only support indices 0-15. Reduce to a 16-color palette."
            )
            continue

        # --- Extract palette from PNG ---
        raw_pal = img.getpalette()
        if raw_pal is not None:
            pal_values = raw_pal[: 16 * 3]
            png_colors = [(pal_values[i], pal_values[i + 1], pal_values[i + 2]) for i in range(0, len(pal_values), 3)]
            pal_idx = meta.palette_index

            if pal_idx in extracted_palettes:
                prev_colors, prev_file = extracted_palettes[pal_idx]
                if png_colors != prev_colors:
                    errors.append(
                        f"{fname}: palette {pal_idx} conflicts with {prev_file} "
                        f"(both use palette {pal_idx} but have different colors)"
                    )
            else:
                extracted_palettes[pal_idx] = (png_colors, fname)

        # --- Encode sprite ---
        width_px = bs.pixel_width
        pixels = [[0] * width_px for _ in range(bs.pixel_height)]
        for y in range(bs.pixel_height):
            for x in range(width_px):
                pixels[y][x] = img_data[y * width_px + x]

        tile_data = _encode_battle_sprite(pixels, bs.tile_width, bs.tile_height)
        compressed = compress(tile_data)

        is_locale = sprite_id in _LOCALE_SPRITE_IDS
        lzhal_name = f"{sprite_id}.gfx.lzhal"
        packed_sprites.append((lzhal_name, compressed, is_locale))

        print(f"  Packed battle sprite {sprite_id} ({meta.name or '?'})")

    # Abort if any errors were found
    if errors:
        print("\npack-battle-sprites failed with errors:", file=sys.stderr)
        for err in errors:
            print(f"  ERROR: {err}", file=sys.stderr)
        raise PackError(f"{len(errors)} error(s) found in custom battle sprite files")

    # Write output files.
    # output_dir corresponds to the battle_sprites/ subdirectory within custom_assets,
    # so files are written directly (no extra battle_sprites/ prefix).
    output_dir.mkdir(parents=True, exist_ok=True)

    for lzhal_name, compressed, is_locale in packed_sprites:
        # Locale-specific sprites go under US/battle_sprites/ relative to
        # the custom_assets root (one level above output_dir).
        out_path = output_dir.parent / "US" / "battle_sprites" / lzhal_name if is_locale else output_dir / lzhal_name
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(compressed)
        print(f"  Wrote {out_path}")

    # Write updated palette files if changed
    for pal_idx, (rgb_colors, source_file) in extracted_palettes.items():
        new_pal_bytes = encode_bgr555_palette(rgb_colors)
        original = original_palettes.get(pal_idx)
        if original is not None and new_pal_bytes != original:
            print(f"  WARNING: palette {pal_idx} changed (from {source_file})")
        if original is None or new_pal_bytes != original:
            pal_out_dir = output_dir / "palettes"
            pal_out_dir.mkdir(parents=True, exist_ok=True)
            pal_out_path = pal_out_dir / f"{pal_idx}.pal"
            pal_out_path.write_bytes(new_pal_bytes)
            print(f"  Wrote {pal_out_path}")
