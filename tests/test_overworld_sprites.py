"""Tests for overworld sprite PNG export and pack validation."""

import json
import struct

import pytest
from PIL import Image
from pydantic import ValidationError

from ebtools.parsers._tiles import (
    PackError,
    encode_bgr555_palette,
    encode_sprite_frame,
    load_bgr555_palette,
    load_jasc_pal,
    make_indexed_png,
    write_jasc_pal,
)
from ebtools.parsers.overworld_sprites import (
    Hitbox,
    SpriteGrouping,
    SpriteMetadata,
    export_sprite_png,
    pack_sprites,
    parse_sprite_groupings,
)

# ---------------------------------------------------------------------------
# Helpers for building synthetic sprite binary data
# ---------------------------------------------------------------------------

_DATA_ROM_BASE = 0x1A7F
_SPRITE_BANK_FIRST = 0x11


def _make_ptr_table(entries: list[int]) -> bytes:
    """Build a sprite_grouping_ptr_table from a list of data offsets."""
    buf = bytearray()
    for off in entries:
        rom_ptr = 0xEF0000 | (off + _DATA_ROM_BASE)
        buf += struct.pack("<I", rom_ptr)
    return bytes(buf)


def _make_grouping_entry(
    height: int = 3,
    width_byte: int = 0x20,
    size_byte: int = 0x00,
    palette_byte: int = 0x00,
    hitbox: tuple[int, int, int, int] = (8, 8, 8, 8),
    bank_byte: int = _SPRITE_BANK_FIRST,
    frame_pointers: list[int] | None = None,
) -> bytes:
    """Build a single sprite grouping entry (header + frame pointers)."""
    if frame_pointers is None:
        frame_pointers = [0x0000] * 8  # 4 directions × 2 frames
    header = bytes(
        [
            height,
            width_byte,
            size_byte,
            palette_byte,
            hitbox[0],
            hitbox[1],
            hitbox[2],
            hitbox[3],
            bank_byte,
        ]
    )
    ptrs = b"".join(struct.pack("<H", p) for p in frame_pointers)
    return header + ptrs


def _make_bank_data(
    tw: int = 2,
    th: int = 3,
    num_frames: int = 8,
) -> tuple[bytes, list[int]]:
    """Create synthetic bank data with sequential frames.

    Returns (bank_bytes, list_of_frame_offsets).
    """
    offsets = []
    buf = bytearray()
    for i in range(num_frames):
        offsets.append(len(buf))
        pixels = [[(i + r + c) % 16 for c in range(tw * 8)] for r in range(th * 8)]
        buf += encode_sprite_frame(pixels, tw, th)
    return bytes(buf), offsets


def _make_palette_file(num_colors: int = 16) -> bytes:
    """Create a BGR555 palette file."""
    buf = bytearray()
    for i in range(num_colors):
        val = (i * 2) & 0x1F  # simple ramp in red channel
        buf += struct.pack("<H", val)
    return bytes(buf)


def _make_test_sprite_dir(
    tmp_path,
    tw: int = 2,
    th: int = 3,
    num_directions: int = 4,
    bank_idx: int = 0,
):
    """Create a minimal asm/bin/overworld_sprites directory structure.

    Returns (bin_dir, sprite_id) where bin_dir is the equivalent of asm/bin/.
    """
    bank_byte = _SPRITE_BANK_FIRST + bank_idx
    num_frames = num_directions * 2
    bank_data, offsets = _make_bank_data(tw, th, num_frames)

    # Frame pointers: use raw offsets (8-dir uses mask 0xFFFE, 4-dir uses 0xFFF0)
    frame_pointers = offsets

    grouping_bytes = _make_grouping_entry(
        height=th,
        width_byte=(tw << 4),
        bank_byte=bank_byte,
        frame_pointers=frame_pointers,
    )

    # Sprite 0 = NONE placeholder, sprite 1 = our test sprite
    none_entry = _make_grouping_entry(height=0, width_byte=0, frame_pointers=[])
    # offset 0 = none entry, offset len(none_entry) = test sprite
    grouping_data = none_entry + grouping_bytes

    ptr_table = _make_ptr_table([0, len(none_entry)])

    # Write files
    sprite_dir = tmp_path / "overworld_sprites"
    sprite_dir.mkdir(parents=True)
    (sprite_dir / "sprite_grouping_ptr_table.bin").write_bytes(ptr_table)
    (sprite_dir / "sprite_grouping_data.bin").write_bytes(grouping_data)

    banks_dir = sprite_dir / "banks"
    banks_dir.mkdir()
    (banks_dir / f"{bank_byte:x}.bin").write_bytes(bank_data)

    pal_dir = sprite_dir / "palettes"
    pal_dir.mkdir()
    (pal_dir / "0.pal").write_bytes(_make_palette_file())

    return tmp_path, 1  # sprite_id=1


def _make_valid_png_and_json(
    png_dir,
    sprite_id: int = 1,
    name: str = "test_sprite",
    tw: int = 2,
    th: int = 3,
    num_directions: int = 4,
    pixel_value: int = 5,
):
    """Create a valid indexed PNG + JSON pair in png_dir."""
    px_w = tw * 8
    px_h = th * 8
    sheet_w = px_w * 2
    sheet_h = px_h * num_directions

    img = Image.new("P", (sheet_w, sheet_h))
    palette = [0] * 768
    for i in range(16):
        palette[i * 3] = i * 16
        palette[i * 3 + 1] = i * 8
        palette[i * 3 + 2] = i * 4
    img.putpalette(palette)
    img.putdata([pixel_value] * (sheet_w * sheet_h))
    img.save(str(png_dir / f"{name}.png"), "PNG")

    dir_labels = ["up", "right", "down", "left", "up_right", "down_right", "down_left", "up_left"][:num_directions]
    meta = {
        "sprite_id": sprite_id,
        "name": name,
        "tile_width": tw,
        "tile_height": th,
        "pixel_width": px_w,
        "pixel_height": px_h,
        "direction_count": num_directions,
        "frames_per_direction": 2,
        "directions": dir_labels,
        "palette_index": 0,
        "palette_byte": 0,
        "size_byte": 0,
        "width_byte": (tw << 4),
        "bank": _SPRITE_BANK_FIRST,
        "bank_byte": _SPRITE_BANK_FIRST,
        "hitbox": {"width_ud": 8, "height_ud": 8, "width_lr": 8, "height_lr": 8},
        "frame_pointers": [0] * (num_directions * 2),
    }
    (png_dir / f"{name}.json").write_text(json.dumps(meta))

    return meta


# ---------------------------------------------------------------------------
# Tests: SpriteGrouping properties
# ---------------------------------------------------------------------------


class TestSpriteGrouping:
    def test_tile_dimensions(self):
        sg = SpriteGrouping(
            sprite_id=1,
            data_offset=0,
            tile_height=3,
            width_byte=0x20,
            size_byte=0,
            palette_byte=0,
            hitbox=Hitbox(width_ud=8, height_ud=8, width_lr=8, height_lr=8),
            bank_byte=_SPRITE_BANK_FIRST,
            frame_pointers=[0] * 8,
        )
        assert sg.tile_width == 2
        assert sg.tile_height == 3
        assert sg.pixel_width == 16
        assert sg.pixel_height == 24

    def test_direction_count_4dir(self):
        sg = SpriteGrouping(
            sprite_id=1,
            data_offset=0,
            tile_height=3,
            width_byte=0x20,
            size_byte=0,
            palette_byte=0,
            hitbox=Hitbox(width_ud=8, height_ud=8, width_lr=8, height_lr=8),
            bank_byte=_SPRITE_BANK_FIRST,
            frame_pointers=[0] * 8,
        )
        assert sg.direction_count == 4
        assert not sg.is_8dir
        assert sg.frame_data_mask == 0xFFF0

    def test_direction_count_8dir(self):
        sg = SpriteGrouping(
            sprite_id=1,
            data_offset=0,
            tile_height=3,
            width_byte=0x20,
            size_byte=0,
            palette_byte=0,
            hitbox=Hitbox(width_ud=8, height_ud=8, width_lr=8, height_lr=8),
            bank_byte=_SPRITE_BANK_FIRST,
            frame_pointers=[0] * 16,
        )
        assert sg.direction_count == 8
        assert sg.is_8dir
        assert sg.frame_data_mask == 0xFFFE

    def test_palette_index(self):
        sg = SpriteGrouping(
            sprite_id=1,
            data_offset=0,
            tile_height=3,
            width_byte=0x20,
            size_byte=0,
            palette_byte=0x0A,
            hitbox=Hitbox(width_ud=8, height_ud=8, width_lr=8, height_lr=8),
            bank_byte=_SPRITE_BANK_FIRST,
            frame_pointers=[],
        )
        assert sg.palette_index == 5  # (0x0A >> 1) & 0x07

    def test_bank_index(self):
        sg = SpriteGrouping(
            sprite_id=1,
            data_offset=0,
            tile_height=3,
            width_byte=0x20,
            size_byte=0,
            palette_byte=0,
            hitbox=Hitbox(width_ud=8, height_ud=8, width_lr=8, height_lr=8),
            bank_byte=0x13,
            frame_pointers=[],
        )
        assert sg.bank_index == 2  # 0x13 - 0x11


# ---------------------------------------------------------------------------
# Tests: parse_sprite_groupings
# ---------------------------------------------------------------------------


class TestParseGroupings:
    def test_basic_parse(self):
        entry = _make_grouping_entry(height=3, width_byte=0x20, frame_pointers=[0x100, 0x200])
        ptr_table = _make_ptr_table([0])
        groupings = parse_sprite_groupings(ptr_table, entry)
        assert 0 in groupings
        sg = groupings[0]
        assert sg.tile_height == 3
        assert sg.tile_width == 2
        assert sg.frame_pointers == [0x100, 0x200]

    def test_multiple_sprites_same_offset(self):
        """Two sprite IDs sharing the same grouping data."""
        entry = _make_grouping_entry(height=2, width_byte=0x10, frame_pointers=[0, 0, 0, 0])
        ptr_table = _make_ptr_table([0, 0])  # both point to offset 0
        groupings = parse_sprite_groupings(ptr_table, entry)
        assert 0 in groupings
        assert 1 in groupings
        assert groupings[0].tile_height == groupings[1].tile_height


# ---------------------------------------------------------------------------
# Tests: export_sprite_png
# ---------------------------------------------------------------------------


class TestExportSpritePng:
    def test_basic_export(self):
        tw, th = 2, 3
        bank_data, offsets = _make_bank_data(tw, th, num_frames=8)
        sg = SpriteGrouping(
            sprite_id=1,
            data_offset=0,
            tile_height=th,
            width_byte=(tw << 4),
            size_byte=0,
            palette_byte=0,
            hitbox=Hitbox(width_ud=8, height_ud=8, width_lr=8, height_lr=8),
            bank_byte=_SPRITE_BANK_FIRST,
            frame_pointers=offsets,
        )
        palette = [(i * 16, i * 8, i * 4, 0 if i == 0 else 255) for i in range(16)]
        img = export_sprite_png(sg, bank_data, palette)
        assert img is not None
        assert img.mode == "P"
        assert img.width == tw * 8 * 2
        assert img.height == th * 8 * 4  # 4 directions

    def test_zero_dimensions_returns_none(self):
        sg = SpriteGrouping(
            sprite_id=0,
            data_offset=0,
            tile_height=0,
            width_byte=0x00,
            size_byte=0,
            palette_byte=0,
            hitbox=Hitbox(width_ud=0, height_ud=0, width_lr=0, height_lr=0),
            bank_byte=_SPRITE_BANK_FIRST,
            frame_pointers=[],
        )
        assert export_sprite_png(sg, b"", []) is None

    def test_no_directions_returns_none(self):
        sg = SpriteGrouping(
            sprite_id=0,
            data_offset=0,
            tile_height=3,
            width_byte=0x20,
            size_byte=0,
            palette_byte=0,
            hitbox=Hitbox(width_ud=0, height_ud=0, width_lr=0, height_lr=0),
            bank_byte=_SPRITE_BANK_FIRST,
            frame_pointers=[],
        )
        assert export_sprite_png(sg, b"", []) is None


# ---------------------------------------------------------------------------
# Tests: SpriteGrouping.to_json_dict
# ---------------------------------------------------------------------------


class TestToJsonDict:
    def test_fields(self):
        sg = SpriteGrouping(
            sprite_id=42,
            data_offset=0,
            tile_height=3,
            width_byte=0x20,
            size_byte=5,
            palette_byte=0x06,
            hitbox=Hitbox(width_ud=10, height_ud=12, width_lr=14, height_lr=16),
            bank_byte=0x13,
            frame_pointers=[0x100, 0x200, 0x300, 0x400],
        )
        j = sg.to_json_dict("my_sprite")
        assert j["sprite_id"] == 42
        assert j["name"] == "my_sprite"
        assert j["tile_width"] == 2
        assert j["tile_height"] == 3
        assert j["direction_count"] == 2
        assert j["palette_index"] == 3  # (0x06 >> 1) & 7
        assert j["hitbox"] == {"width_ud": 10, "height_ud": 12, "width_lr": 14, "height_lr": 16}
        assert j["frame_pointers"] == [0x100, 0x200, 0x300, 0x400]


# ---------------------------------------------------------------------------
# Tests: _validate_hitbox
# ---------------------------------------------------------------------------


class TestHitboxValidation:
    def test_valid_hitbox(self):
        hb = Hitbox(width_ud=8, height_ud=8, width_lr=8, height_lr=8)
        assert hb.width_ud == 8

    def test_out_of_range(self):
        with pytest.raises(ValidationError, match="width_ud"):
            Hitbox(width_ud=256, height_ud=0, width_lr=0, height_lr=0)

    def test_negative(self):
        with pytest.raises(ValidationError, match="height_lr"):
            Hitbox(width_ud=0, height_ud=0, width_lr=0, height_lr=-1)

    def test_non_integer(self):
        with pytest.raises(ValidationError, match="width_ud"):
            Hitbox(width_ud="big", height_ud=0, width_lr=0, height_lr=0)

    def test_boundary_values(self):
        hb = Hitbox(width_ud=0, height_ud=255, width_lr=0, height_lr=0)
        assert hb.width_ud == 0
        assert hb.height_ud == 255

    def test_sprite_metadata_requires_fields(self):
        with pytest.raises(ValidationError):
            SpriteMetadata(sprite_id=1)  # missing required fields


# ---------------------------------------------------------------------------
# Tests: pack_sprites validation errors
# ---------------------------------------------------------------------------


class TestPackSpritesValidation:
    """Test that pack_sprites raises PackError for invalid inputs."""

    def _setup_bin_dir(self, tmp_path):
        bin_dir, sprite_id = _make_test_sprite_dir(tmp_path / "bin")
        return bin_dir, sprite_id

    def test_non_indexed_png(self, tmp_path):
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        # Create RGB (non-indexed) PNG
        img = Image.new("RGB", (32, 96), (255, 0, 0))
        img.save(str(png_dir / "bad.png"))
        meta = {
            "sprite_id": sprite_id,
            "name": "bad",
            "hitbox": {"width_ud": 8, "height_ud": 8, "width_lr": 8, "height_lr": 8},
            "frame_pointers": [0] * 8,
        }
        (png_dir / "bad.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_wrong_dimensions(self, tmp_path):
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        # Correct sprite is 2×3 tiles, 4 dirs → 32×96, but we give 32×32
        img = Image.new("P", (32, 32))
        img.putpalette([0] * 768)
        img.save(str(png_dir / "bad.png"))
        meta = {
            "sprite_id": sprite_id,
            "hitbox": {"width_ud": 8, "height_ud": 8, "width_lr": 8, "height_lr": 8},
            "frame_pointers": [0] * 8,
        }
        (png_dir / "bad.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_pixel_values_out_of_range(self, tmp_path):
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        # Create indexed PNG with pixels > 15
        img = Image.new("P", (32, 96))
        palette = [0] * 768
        for i in range(768):
            palette[i] = i % 256
        img.putpalette(palette)
        img.putdata([20] * (32 * 96))  # all pixels at index 20
        img.save(str(png_dir / "bad.png"))
        meta = {
            "sprite_id": sprite_id,
            "hitbox": {"width_ud": 8, "height_ud": 8, "width_lr": 8, "height_lr": 8},
            "frame_pointers": [0] * 8,
        }
        (png_dir / "bad.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_json_without_png(self, tmp_path):
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        meta = {
            "sprite_id": sprite_id,
            "hitbox": {"width_ud": 8, "height_ud": 8, "width_lr": 8, "height_lr": 8},
            "frame_pointers": [0] * 8,
        }
        (png_dir / "orphan.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_invalid_json(self, tmp_path):
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        img = Image.new("P", (32, 96))
        img.putpalette([0] * 768)
        img.save(str(png_dir / "bad.png"))
        (png_dir / "bad.json").write_text("{invalid json")

        with pytest.raises(PackError, match="1 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_missing_sprite_id(self, tmp_path):
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        img = Image.new("P", (32, 96))
        img.putpalette([0] * 768)
        img.save(str(png_dir / "bad.png"))
        (png_dir / "bad.json").write_text(json.dumps({"name": "oops"}))

        with pytest.raises(PackError, match="1 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_invalid_sprite_id(self, tmp_path):
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        img = Image.new("P", (32, 96))
        img.putpalette([0] * 768)
        img.save(str(png_dir / "bad.png"))
        (png_dir / "bad.json").write_text(json.dumps({"sprite_id": 9999}))

        with pytest.raises(PackError, match="1 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_hitbox_out_of_range(self, tmp_path):
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        _make_valid_png_and_json(png_dir, sprite_id=sprite_id)
        # Overwrite JSON with bad hitbox
        meta = json.loads((png_dir / "test_sprite.json").read_text())
        meta["hitbox"]["width_ud"] = 300
        (png_dir / "test_sprite.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_multiple_errors_all_reported(self, tmp_path):
        """Multiple bad files should all be reported in a single PackError."""
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        # Bad file 1: RGB PNG
        img = Image.new("RGB", (32, 96))
        img.save(str(png_dir / "bad1.png"))
        (png_dir / "bad1.json").write_text(
            json.dumps(
                {
                    "sprite_id": sprite_id,
                    "hitbox": {"width_ud": 8, "height_ud": 8, "width_lr": 8, "height_lr": 8},
                }
            )
        )

        # Bad file 2: orphan JSON
        (png_dir / "bad2.json").write_text(
            json.dumps(
                {
                    "sprite_id": sprite_id,
                    "hitbox": {"width_ud": 8, "height_ud": 8, "width_lr": 8, "height_lr": 8},
                }
            )
        )

        with pytest.raises(PackError, match="2 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_no_json_files_no_error(self, tmp_path):
        """Empty png_dir should return silently (no error)."""
        bin_dir, _ = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        # No files → should print warning and return, not raise
        pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_valid_sprite_packs_successfully(self, tmp_path):
        """A correctly formatted sprite should pack without errors."""
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        _make_valid_png_and_json(png_dir, sprite_id=sprite_id)

        out_dir = tmp_path / "out"
        pack_sprites(png_dir, bin_dir, out_dir)

        # Check output files were created
        assert (out_dir / "sprite_grouping_data.bin").exists()
        assert (out_dir / "sprite_grouping_ptr_table.bin").exists()
        assert (out_dir / "banks").is_dir()


# ---------------------------------------------------------------------------
# Tests: encode_bgr555_palette round-trip
# ---------------------------------------------------------------------------


class TestEncodeBgr555Palette:
    def test_round_trip(self):
        """Encoding then decoding should recover the original colors (within quantization)."""
        original_colors = [(r, g, b) for r in (0, 8, 248) for g in (0, 8, 248) for b in (0, 8, 248)]
        # Pad to 16
        while len(original_colors) < 16:
            original_colors.append((0, 0, 0))
        original_colors = original_colors[:16]

        encoded = encode_bgr555_palette(original_colors)
        assert len(encoded) == 32  # 16 colors × 2 bytes

        decoded = load_bgr555_palette(encoded)
        assert len(decoded) == 16

        for i, (r, g, b) in enumerate(original_colors):
            dr, dg, db, _a = decoded[i]
            # Each channel quantized to 5 bits (>> 3) then expanded (<<3 | >>5)
            # The round-trip tolerance is up to 7 per channel
            assert abs(dr - r) <= 7
            assert abs(dg - g) <= 7
            assert abs(db - b) <= 7

    def test_exact_round_trip_for_snes_values(self):
        """Colors that are exact SNES values (multiples of 8 with bit replication) survive losslessly."""
        # SNES palette from _make_palette_file: simple ramp in red channel
        pal_bytes = _make_palette_file(16)
        decoded = load_bgr555_palette(pal_bytes)
        rgb_colors = [(r, g, b) for r, g, b, _a in decoded]
        re_encoded = encode_bgr555_palette(rgb_colors)
        assert re_encoded == pal_bytes


# ---------------------------------------------------------------------------
# Tests: JASC-PAL round-trip
# ---------------------------------------------------------------------------


class TestJascPal:
    def test_round_trip(self, tmp_path):
        colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (128, 128, 128)]
        pal_path = tmp_path / "test.pal"
        write_jasc_pal(colors, pal_path)
        loaded = load_jasc_pal(pal_path)
        assert loaded == colors

    def test_16_colors(self, tmp_path):
        colors = [(i * 16, i * 8, i * 4) for i in range(16)]
        pal_path = tmp_path / "test16.pal"
        write_jasc_pal(colors, pal_path)
        loaded = load_jasc_pal(pal_path)
        assert loaded == colors

    def test_file_format(self, tmp_path):
        colors = [(10, 20, 30), (40, 50, 60)]
        pal_path = tmp_path / "test.pal"
        write_jasc_pal(colors, pal_path)
        text = pal_path.read_text()
        lines = text.strip().splitlines()
        assert lines[0] == "JASC-PAL"
        assert lines[1] == "0100"
        assert lines[2] == "2"
        assert lines[3] == "10 20 30"
        assert lines[4] == "40 50 60"


# ---------------------------------------------------------------------------
# Tests: make_indexed_png palette size
# ---------------------------------------------------------------------------


class TestMakeIndexedPngPaletteSize:
    def test_palette_has_16_entries(self):
        """PNG palette should have exactly 16 entries, not 256."""
        palette_rgba = [(i * 16, i * 8, i * 4, 0 if i == 0 else 255) for i in range(16)]
        pixels = [[0] * 16 for _ in range(16)]
        img = make_indexed_png(pixels, palette_rgba, 16, 16)
        raw_pal = img.getpalette()
        assert raw_pal is not None
        # Pillow getpalette() returns 768 values (256 * 3) regardless,
        # but only the first 16*3 should have our data
        our_colors = raw_pal[: 16 * 3]
        expected = []
        for r, g, b, _a in palette_rgba:
            expected.extend([r, g, b])
        assert our_colors == expected


# ---------------------------------------------------------------------------
# Tests: pack_sprites palette extraction
# ---------------------------------------------------------------------------


class TestPackPaletteExtraction:
    def _setup_bin_dir(self, tmp_path):
        bin_dir, sprite_id = _make_test_sprite_dir(tmp_path / "bin")
        return bin_dir, sprite_id

    def test_pack_extracts_palette(self, tmp_path):
        """PNG with a modified palette should produce an updated .pal file."""
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        _make_valid_png_and_json(png_dir, sprite_id=sprite_id)

        # Modify the PNG palette to something different from the original
        png_path = png_dir / "test_sprite.png"
        img = Image.open(png_path)
        new_palette = [0] * 48
        for i in range(16):
            new_palette[i * 3] = (i * 17) % 256  # different from original
            new_palette[i * 3 + 1] = (i * 5) % 256
            new_palette[i * 3 + 2] = (i * 11) % 256
        img.putpalette(new_palette)
        img.save(str(png_path), "PNG")

        out_dir = tmp_path / "out"
        pack_sprites(png_dir, bin_dir, out_dir)

        # Should have written a palette file
        pal_path = out_dir / "palettes" / "0.pal"
        assert pal_path.exists()
        pal_data = pal_path.read_bytes()
        assert len(pal_data) == 32  # 16 colors × 2 bytes

    def test_pack_palette_conflict(self, tmp_path):
        """Two sprites on the same palette_index with different palettes should error."""
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        # Create first sprite
        _make_valid_png_and_json(png_dir, sprite_id=sprite_id, name="sprite_a")

        # Create second sprite with same sprite_id but different palette
        # We need a second sprite that shares the same palette_index
        # Since our test has only 1 sprite, we create two JSONs pointing to same sprite_id
        # but with different palettes in the PNGs
        _make_valid_png_and_json(png_dir, sprite_id=sprite_id, name="sprite_b")

        # Modify palette of sprite_b to be different
        png_b = png_dir / "sprite_b.png"
        img = Image.open(png_b)
        different_palette = [255] * 48  # all white
        img.putpalette(different_palette)
        img.save(str(png_b), "PNG")

        with pytest.raises(PackError, match="1 error"):
            pack_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_pack_palette_unchanged(self, tmp_path):
        """Palette matching original should not produce a .pal output file."""
        bin_dir, sprite_id = self._setup_bin_dir(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        # Read the original palette and use it in the PNG
        orig_pal_path = bin_dir / "overworld_sprites" / "palettes" / "0.pal"
        orig_pal_bytes = orig_pal_path.read_bytes()
        orig_colors = load_bgr555_palette(orig_pal_bytes)

        # Create PNG with the original palette colors
        tw, th = 2, 3
        num_dirs = 4
        px_w = tw * 8
        px_h = th * 8
        sheet_w = px_w * 2
        sheet_h = px_h * num_dirs

        img = Image.new("P", (sheet_w, sheet_h))
        flat_pal = []
        for r, g, b, _a in orig_colors:
            flat_pal.extend([r, g, b])
        img.putpalette(flat_pal)
        img.putdata([5] * (sheet_w * sheet_h))
        img.save(str(png_dir / "test_sprite.png"), "PNG")

        meta = {
            "sprite_id": sprite_id,
            "name": "test_sprite",
            "tile_width": tw,
            "tile_height": th,
            "pixel_width": px_w,
            "pixel_height": px_h,
            "direction_count": num_dirs,
            "frames_per_direction": 2,
            "directions": ["up", "right", "down", "left"],
            "palette_index": 0,
            "palette_byte": 0,
            "size_byte": 0,
            "width_byte": (tw << 4),
            "bank": _SPRITE_BANK_FIRST,
            "bank_byte": _SPRITE_BANK_FIRST,
            "hitbox": {"width_ud": 8, "height_ud": 8, "width_lr": 8, "height_lr": 8},
            "frame_pointers": [0] * (num_dirs * 2),
        }
        (png_dir / "test_sprite.json").write_text(json.dumps(meta))

        out_dir = tmp_path / "out"
        pack_sprites(png_dir, bin_dir, out_dir)

        # Palette should NOT have been written since it's unchanged
        assert not (out_dir / "palettes").exists()
