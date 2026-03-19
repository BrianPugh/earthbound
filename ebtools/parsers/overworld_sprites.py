"""Overworld sprite PNG export and binary pack utilities.

Reads sprite_grouping_ptr_table.bin, sprite_grouping_data.bin, bank data,
and palette files to generate indexed PNG spritesheets with JSON metadata.
Also provides the inverse: packing PNG spritesheets back to binary.
"""

import json
import struct
import sys
from pathlib import Path
from typing import ClassVar, Literal

from PIL import Image
from pydantic import BaseModel, ConfigDict, Field, ValidationError, computed_field

from ebtools.parsers._tiles import (
    PackError,
    decode_sprite_frame,
    encode_bgr555_palette,
    encode_sprite_frame,
    load_bgr555_palette,
    make_indexed_png,
    write_jasc_pal,
)

# Sprite grouping header: 8 bytes of metadata + 1 bank byte
_HEADER_SIZE = 9  # matches SPRITE_GROUPING_HEADER_SIZE in C port

# ROM base address of sprite_grouping_data
_DATA_ROM_BASE = 0x1A7F

# Number of sprite palettes
_NUM_PALETTES = 8

# Sprite bank range
_SPRITE_BANK_FIRST = 0x11
_SPRITE_BANK_COUNT = 5

# Direction labels for JSON metadata (memory pointer order)
_DIR_LABELS_4 = ["up", "right", "down", "left"]
_DIR_LABELS_8 = [
    "up",
    "right",
    "down",
    "left",
    "up_right",
    "down_right",
    "down_left",
    "up_left",
]


def _read16(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8)


def _read32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


class Hitbox(BaseModel):
    """Sprite hitbox dimensions (all values 0-255)."""

    model_config = ConfigDict(frozen=True)

    width_ud: int = Field(ge=0, le=255)
    height_ud: int = Field(ge=0, le=255)
    width_lr: int = Field(ge=0, le=255)
    height_lr: int = Field(ge=0, le=255)


class SpriteGrouping(BaseModel):
    """Parsed sprite grouping entry."""

    model_config = ConfigDict(frozen=True)

    FRAMES_PER_DIRECTION: ClassVar[int] = 2

    sprite_id: int
    data_offset: int = Field(exclude=True)
    tile_height: int
    width_byte: int
    size_byte: int
    palette_byte: int
    hitbox: Hitbox
    bank_byte: int
    frame_pointers: list[int] = Field(default_factory=list)  # raw 16-bit values (with flag bits)

    # -- Computed fields (included in model_dump / JSON) --

    @computed_field
    @property
    def tile_width(self) -> int:
        return (self.width_byte >> 4) & 0x0F

    @computed_field
    @property
    def pixel_width(self) -> int:
        return self.tile_width * 8

    @computed_field
    @property
    def pixel_height(self) -> int:
        return self.tile_height * 8

    @computed_field
    @property
    def direction_count(self) -> int:
        return len(self.frame_pointers) // 2

    @computed_field
    @property
    def frames_per_direction(self) -> int:
        return self.FRAMES_PER_DIRECTION

    @computed_field
    @property
    def directions(self) -> list[str]:
        labels = _DIR_LABELS_8 if self.is_8dir else _DIR_LABELS_4
        return labels[: self.direction_count]

    @computed_field
    @property
    def palette_index(self) -> int:
        return (self.palette_byte >> 1) & 0x07

    @computed_field
    @property
    def bank(self) -> int:
        return self.bank_byte & 0x3F

    # -- Plain properties (internal use only, not in JSON) --

    @property
    def byte_width(self) -> int:
        return self.tile_width * 32

    @property
    def is_8dir(self) -> bool:
        return self.direction_count >= 8

    @property
    def bank_index(self) -> int:
        return self.bank - _SPRITE_BANK_FIRST

    @property
    def frame_data_mask(self) -> int:
        return 0xFFFE if self.is_8dir else 0xFFF0

    def to_json_dict(self, name: str) -> dict:
        """Serialize to the JSON metadata format used for sprite sidecars."""
        d = self.model_dump()
        d["name"] = name
        return d


class SpriteMetadata(BaseModel):
    """Validation schema for sprite JSON sidecar files (used by pack_sprites)."""

    sprite_id: int = Field(ge=0)
    name: str = ""
    tile_width: int
    tile_height: int
    pixel_width: int
    pixel_height: int
    direction_count: int
    frames_per_direction: int
    directions: list[Literal["up", "right", "down", "left", "up_right", "down_right", "down_left", "up_left"]]
    palette_index: int
    palette_byte: int
    size_byte: int
    width_byte: int
    bank: int
    bank_byte: int
    hitbox: Hitbox
    frame_pointers: list[int]


def parse_sprite_groupings(
    ptr_table: bytes,
    grouping_data: bytes,
) -> dict[int, SpriteGrouping]:
    """Parse all sprite grouping entries from binary data.

    Returns a dict mapping sprite_id -> SpriteGrouping.
    """
    num_sprites = len(ptr_table) // 4

    # Collect all unique data offsets and sort them to compute entry sizes
    offset_to_ids: dict[int, list[int]] = {}
    offsets_list: list[int] = []

    for sprite_id in range(num_sprites):
        rom_ptr = _read32(ptr_table, sprite_id * 4)
        data_off = (rom_ptr & 0xFFFF) - _DATA_ROM_BASE

        if data_off not in offset_to_ids:
            offset_to_ids[data_off] = []
            offsets_list.append(data_off)
        offset_to_ids[data_off].append(sprite_id)

    sorted_offsets = sorted(set(offsets_list))

    # Compute entry size for each unique offset
    entry_sizes: dict[int, int] = {}
    for i, off in enumerate(sorted_offsets):
        if i + 1 < len(sorted_offsets):
            entry_sizes[off] = sorted_offsets[i + 1] - off
        else:
            entry_sizes[off] = len(grouping_data) - off

    # Parse each unique grouping
    groupings: dict[int, SpriteGrouping] = {}

    for off in sorted_offsets:
        if off + _HEADER_SIZE > len(grouping_data):
            continue

        tile_height = grouping_data[off + 0]
        width_byte = grouping_data[off + 1]
        size_byte = grouping_data[off + 2]
        palette_byte = grouping_data[off + 3]
        hitbox = Hitbox(
            width_ud=grouping_data[off + 4],
            height_ud=grouping_data[off + 5],
            width_lr=grouping_data[off + 6],
            height_lr=grouping_data[off + 7],
        )
        bank_byte = grouping_data[off + 8]

        # Read frame pointers: entry_size - 9 header bytes = pointer array bytes
        ptr_array_size = entry_sizes[off] - _HEADER_SIZE
        num_pointers = ptr_array_size // 2
        frame_pointers = []
        for i in range(num_pointers):
            ptr_off = off + _HEADER_SIZE + i * 2
            if ptr_off + 2 <= len(grouping_data):
                frame_pointers.append(_read16(grouping_data, ptr_off))

        # Create SpriteGrouping for each sprite_id that uses this offset
        for sprite_id in offset_to_ids[off]:
            groupings[sprite_id] = SpriteGrouping(
                sprite_id=sprite_id,
                data_offset=off,
                tile_height=tile_height,
                width_byte=width_byte,
                size_byte=size_byte,
                palette_byte=palette_byte,
                hitbox=hitbox,
                bank_byte=bank_byte,
                frame_pointers=list(frame_pointers),
            )

    return groupings


def export_sprite_png(
    sg: SpriteGrouping,
    bank_data: bytes,
    palette_rgba: list[tuple[int, int, int, int]],
) -> Image.Image | None:
    """Export a single sprite grouping as an indexed PNG spritesheet.

    Layout: columns = frames (2), rows = directions.
    Returns None if sprite data can't be decoded.
    """
    tw = sg.tile_width
    th = sg.tile_height
    if tw == 0 or th == 0:
        return None

    num_dirs = sg.direction_count
    if num_dirs == 0:
        return None

    px_w = tw * 8  # pixel width per frame
    px_h = th * 8  # pixel height per frame
    sheet_w = px_w * 2  # 2 frames per direction
    sheet_h = px_h * num_dirs

    sheet_pixels = [[0] * sheet_w for _ in range(sheet_h)]

    mask = sg.frame_data_mask
    bank_size = len(bank_data)

    for dir_idx in range(num_dirs):
        for frame_idx in range(2):
            ptr_idx = dir_idx * 2 + frame_idx
            if ptr_idx >= len(sg.frame_pointers):
                continue

            raw_ptr = sg.frame_pointers[ptr_idx]
            data_off = raw_ptr & mask

            if data_off + th * sg.byte_width > bank_size:
                continue

            frame_pixels = decode_sprite_frame(bank_data, data_off, tw, th)

            # Place in spritesheet
            x_off = frame_idx * px_w
            y_off = dir_idx * px_h
            for y in range(px_h):
                for x in range(px_w):
                    sheet_pixels[y_off + y][x_off + x] = frame_pixels[y][x]

    return make_indexed_png(sheet_pixels, palette_rgba, sheet_w, sheet_h)


def export_all_sprites(
    bin_path: Path,
    asset_output_dir: Path,
    sprite_names: list[str] | None = None,
) -> int:
    """Export all overworld sprites as indexed PNG spritesheets with JSON metadata.

    Parameters
    ----------
    bin_path
        Path to extracted binary assets (e.g. asm/bin/).
    asset_output_dir
        Output directory for PNGs and JSON (e.g. src/assets/).
    sprite_names
        Optional list of sprite names from common_data.sprites.

    Returns the number of sprites exported.
    """
    sprite_dir = bin_path / "overworld_sprites"
    out_dir = asset_output_dir / "overworld_sprites"

    # Load binary data
    ptr_table_path = sprite_dir / "sprite_grouping_ptr_table.bin"
    data_path = sprite_dir / "sprite_grouping_data.bin"
    if not ptr_table_path.exists() or not data_path.exists():
        print(f"  Skipping sprite export: missing {ptr_table_path} or {data_path}")
        return 0

    ptr_table = ptr_table_path.read_bytes()
    grouping_data = data_path.read_bytes()

    # Load sprite banks
    banks: dict[int, bytes] = {}
    for i in range(_SPRITE_BANK_COUNT):
        bank_num = _SPRITE_BANK_FIRST + i
        bank_path = sprite_dir / "banks" / f"{bank_num:x}.bin"
        if bank_path.exists():
            banks[i] = bank_path.read_bytes()

    # Load palettes
    palettes: dict[int, list[tuple[int, int, int, int]]] = {}
    for i in range(_NUM_PALETTES):
        pal_path = sprite_dir / "palettes" / f"{i}.pal"
        if pal_path.exists():
            palettes[i] = load_bgr555_palette(pal_path.read_bytes())

    # Export JASC-PAL palette files for artist use
    jasc_dir = out_dir / "palettes"
    jasc_dir.mkdir(parents=True, exist_ok=True)
    for pal_idx, pal_rgba in palettes.items():
        rgb_colors = [(r, g, b) for r, g, b, _a in pal_rgba]
        write_jasc_pal(rgb_colors, jasc_dir / f"{pal_idx}.pal")

    # Parse all groupings
    groupings = parse_sprite_groupings(ptr_table, grouping_data)

    out_dir.mkdir(parents=True, exist_ok=True)
    count = 0

    num_sprites = len(ptr_table) // 4
    for sprite_id in range(1, num_sprites):  # skip NONE (0)
        sg = groupings.get(sprite_id)
        if sg is None:
            continue

        # Get sprite name
        name = sprite_names[sprite_id].lower() if sprite_names and sprite_id < len(sprite_names) else f"{sprite_id:04d}"

        # Get bank data
        bank_idx = sg.bank_index
        if bank_idx < 0 or bank_idx >= _SPRITE_BANK_COUNT:
            continue
        bank_data = banks.get(bank_idx)
        if bank_data is None:
            continue

        # Get palette
        pal_idx = sg.palette_index
        palette = palettes.get(pal_idx)
        if palette is None:
            # Fallback to palette 0
            palette = palettes.get(0)
        if palette is None:
            # Use a default grayscale palette
            palette = [(i * 17, i * 17, i * 17, 0 if i == 0 else 255) for i in range(16)]

        # Export PNG
        img = export_sprite_png(sg, bank_data, palette)
        if img is None:
            continue

        png_path = out_dir / f"{name}.png"
        img.save(str(png_path), "PNG", transparency=0)

        # Export JSON
        json_data = sg.to_json_dict(name)
        json_path = out_dir / f"{name}.json"
        json_path.write_text(json.dumps(json_data, indent=2) + "\n")

        count += 1

    return count


def pack_sprites(
    png_dir: Path,
    bin_path: Path,
    output_dir: Path,
) -> None:
    """Pack modified PNG spritesheets back to binary format.

    Reads PNG + JSON from png_dir, merges with original binary data from bin_path,
    and writes updated binary files to output_dir.

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
    sprite_dir = bin_path / "overworld_sprites"

    # Load original binary data
    ptr_table = (sprite_dir / "sprite_grouping_ptr_table.bin").read_bytes()
    grouping_data = bytearray((sprite_dir / "sprite_grouping_data.bin").read_bytes())

    # Load original sprite banks as mutable bytearrays
    original_banks: dict[int, bytearray] = {}
    for i in range(_SPRITE_BANK_COUNT):
        bank_num = _SPRITE_BANK_FIRST + i
        bank_path = sprite_dir / "banks" / f"{bank_num:x}.bin"
        if bank_path.exists():
            original_banks[i] = bytearray(bank_path.read_bytes())

    # Parse groupings from original data
    groupings = parse_sprite_groupings(ptr_table, bytes(grouping_data))

    # Find all PNG files with matching JSON
    json_files = sorted(png_dir.glob("*.json"))
    if not json_files:
        print("No sprite JSON files found in", png_dir)
        return

    # Load original palettes for comparison
    original_palettes: dict[int, bytes] = {}
    for i in range(_NUM_PALETTES):
        pal_path = sprite_dir / "palettes" / f"{i}.pal"
        if pal_path.exists():
            original_palettes[i] = pal_path.read_bytes()

    # Build a count of how many sprites use each palette (across ALL sprites, not just custom)
    all_groupings = parse_sprite_groupings(ptr_table, bytes(grouping_data))
    sprites_per_palette: dict[int, int] = {}
    for sg_any in all_groupings.values():
        pi = sg_any.palette_index
        sprites_per_palette[pi] = sprites_per_palette.get(pi, 0) + 1

    # Collect all errors so the user sees every problem at once
    errors: list[str] = []
    modified_banks: set[int] = set()
    # Track extracted palettes: palette_index -> (rgb_colors, source_filename)
    extracted_palettes: dict[int, tuple[list[tuple[int, int, int]], str]] = {}

    for json_path in json_files:
        png_path = json_path.with_suffix(".png")
        fname = json_path.stem

        # --- Validate JSON ---
        if not png_path.exists():
            errors.append(f"{json_path.name}: no matching PNG file ({png_path.name})")
            continue

        try:
            raw = json.loads(json_path.read_text())
        except json.JSONDecodeError as e:
            errors.append(f"{json_path.name}: invalid JSON: {e}")
            continue

        try:
            meta = SpriteMetadata.model_validate(raw)
        except ValidationError as e:
            errors.append(f"{json_path.name}: {e}")
            continue

        sprite_id = meta.sprite_id

        sg = groupings.get(sprite_id)
        if sg is None:
            errors.append(f"{json_path.name}: sprite_id {sprite_id} not found in grouping data")
            continue

        bank_idx = sg.bank_index
        if bank_idx not in original_banks:
            errors.append(f"{fname}: bank index {bank_idx} (bank 0x{sg.bank:x}) not loaded")
            continue

        bank_data = original_banks[bank_idx]

        # --- Validate PNG ---
        img = Image.open(png_path)

        if img.mode != "P":
            errors.append(
                f"{png_path.name}: must be indexed/paletted PNG (mode 'P'), "
                f"got mode '{img.mode}'. Re-save as an indexed PNG with 16 colors."
            )
            continue

        tw = sg.tile_width
        th = sg.tile_height
        px_w = tw * 8
        px_h = th * 8
        expected_w = px_w * 2  # 2 frames
        expected_h = px_h * sg.direction_count

        if img.width != expected_w or img.height != expected_h:
            errors.append(
                f"{png_path.name}: wrong dimensions {img.width}x{img.height}, "
                f"expected {expected_w}x{expected_h} "
                f"({px_w}px × 2 frames, {px_h}px × {sg.direction_count} directions)"
            )
            continue

        # Check for out-of-range pixel indices (4bpp supports 0-15)
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
            # Take first 16 colors (48 values)
            pal_values = raw_pal[: 16 * 3]
            png_colors = [(pal_values[i], pal_values[i + 1], pal_values[i + 2]) for i in range(0, len(pal_values), 3)]
            pal_idx = sg.palette_index

            if pal_idx in extracted_palettes:
                prev_colors, prev_file = extracted_palettes[pal_idx]
                if png_colors != prev_colors:
                    errors.append(
                        f"{fname}: palette {pal_idx} conflicts with {prev_file} "
                        f"(both use palette {pal_idx} but have different colors)"
                    )
            else:
                extracted_palettes[pal_idx] = (png_colors, fname)

        mask = sg.frame_data_mask
        sheet_w = img.width

        for dir_idx in range(sg.direction_count):
            for frame_idx in range(2):
                ptr_idx = dir_idx * 2 + frame_idx
                if ptr_idx >= len(sg.frame_pointers):
                    continue

                raw_ptr = sg.frame_pointers[ptr_idx]
                data_off = raw_ptr & mask

                frame_size = th * tw * 32
                if data_off + frame_size > len(bank_data):
                    errors.append(
                        f"{fname}: frame data at offset 0x{data_off:04X} + {frame_size} bytes "
                        f"exceeds bank size ({len(bank_data)} bytes)"
                    )
                    continue

                # Extract frame pixels from spritesheet
                x_off = frame_idx * px_w
                y_off = dir_idx * px_h
                frame_pixels = [[0] * px_w for _ in range(px_h)]
                for y in range(px_h):
                    for x in range(px_w):
                        idx = (y_off + y) * sheet_w + (x_off + x)
                        frame_pixels[y][x] = img_data[idx]

                # Encode back to 4bpp
                frame_bytes = encode_sprite_frame(frame_pixels, tw, th)

                # Write into bank data
                bank_data[data_off : data_off + len(frame_bytes)] = frame_bytes
                modified_banks.add(bank_idx)

        # Update grouping metadata from JSON
        off = sg.data_offset
        grouping_data[off + 3] = meta.palette_byte
        grouping_data[off + 4] = meta.hitbox.width_ud
        grouping_data[off + 5] = meta.hitbox.height_ud
        grouping_data[off + 6] = meta.hitbox.width_lr
        grouping_data[off + 7] = meta.hitbox.height_lr

        print(f"  Packed sprite {sprite_id} ({meta.name or '?'})")

    # Abort if any errors were found
    if errors:
        print("\npack-sprites failed with errors:", file=sys.stderr)
        for err in errors:
            print(f"  ERROR: {err}", file=sys.stderr)
        raise PackError(f"{len(errors)} error(s) found in custom sprite files")

    # Write output files
    output_dir.mkdir(parents=True, exist_ok=True)

    # Write modified banks
    banks_dir = output_dir / "banks"
    banks_dir.mkdir(parents=True, exist_ok=True)
    for bank_idx in modified_banks:
        bank_num = _SPRITE_BANK_FIRST + bank_idx
        out_path = banks_dir / f"{bank_num:x}.bin"
        out_path.write_bytes(original_banks[bank_idx])
        print(f"  Wrote {out_path}")

    # Write updated palette files if palettes changed
    for pal_idx, (rgb_colors, source_file) in extracted_palettes.items():
        new_pal_bytes = encode_bgr555_palette(rgb_colors)
        original = original_palettes.get(pal_idx)
        if original is not None and new_pal_bytes != original:
            n = sprites_per_palette.get(pal_idx, 0)
            print(
                f"  WARNING: palette {pal_idx} changed (from {source_file}) "
                f"— this affects all {n} sprite(s) using this palette"
            )
        if original is None or new_pal_bytes != original:
            pal_out_dir = output_dir / "palettes"
            pal_out_dir.mkdir(parents=True, exist_ok=True)
            pal_out_path = pal_out_dir / f"{pal_idx}.pal"
            pal_out_path.write_bytes(new_pal_bytes)
            print(f"  Wrote {pal_out_path}")

    # Write updated grouping data and pointer table
    out_data = output_dir / "sprite_grouping_data.bin"
    out_data.write_bytes(grouping_data)
    print(f"  Wrote {out_data}")

    out_ptrs = output_dir / "sprite_grouping_ptr_table.bin"
    out_ptrs.write_bytes(ptr_table)
    print(f"  Wrote {out_ptrs}")
