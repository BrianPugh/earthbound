"""Tests for battle sprite PNG export and pack validation."""

import json
import struct

import pytest
from PIL import Image
from pydantic import ValidationError

from ebtools.hallz import compress, decompress
from ebtools.parsers._tiles import PackError, encode_4bpp_tile, load_bgr555_palette, make_indexed_png
from ebtools.parsers.battle_sprites import (
    SPRITE_SIZES,
    BattleSprite,
    BattleSpriteMetadata,
    _decode_battle_sprite,
    _encode_battle_sprite,
    _generate_arrangement,
    _parse_pointer_table,
    export_all_battle_sprites,
    pack_battle_sprites,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_pointer_table(entries: list[tuple[int, int]]) -> bytes:
    """Build a battle_sprites_pointers.bin from (rom_ptr, size_enum) pairs."""
    buf = bytearray()
    for ptr, size_enum in entries:
        buf += struct.pack("<I", ptr)
        buf.append(size_enum)
    return bytes(buf)


def _make_palette_file(num_colors: int = 16) -> bytes:
    """Create a BGR555 palette file."""
    buf = bytearray()
    for i in range(num_colors):
        val = (i * 2) & 0x1F
        buf += struct.pack("<H", val)
    return bytes(buf)


def _make_tile_data(width_tiles: int, height_tiles: int, pixel_value: int = 5) -> bytes:
    """Create uncompressed tile data for a battle sprite.

    Fills all pixels with the given value.
    """
    num_tiles = width_tiles * height_tiles
    pixels = [[pixel_value] * 8 for _ in range(8)]
    tile_bytes = encode_4bpp_tile(pixels)
    return tile_bytes * num_tiles


def _make_test_battle_sprite_dir(
    tmp_path,
    sprite_ids: list[int] | None = None,
    size_enum: int = 1,
) -> tuple:
    """Create a minimal asm/bin/ directory structure for battle sprites.

    Returns (bin_dir, sprite_ids_created).
    """
    if sprite_ids is None:
        sprite_ids = [0]

    bin_dir = tmp_path / "bin"
    sprite_dir = bin_dir / "battle_sprites"
    sprite_dir.mkdir(parents=True)

    # Create palette files
    pal_dir = sprite_dir / "palettes"
    pal_dir.mkdir()
    for i in range(32):
        (pal_dir / f"{i}.pal").write_bytes(_make_palette_file())

    # Create pointer table
    data_dir = bin_dir / "data"
    data_dir.mkdir(parents=True)

    wt, ht = SPRITE_SIZES[size_enum]
    tile_data = _make_tile_data(wt, ht)

    entries: list[tuple[int, int]] = []
    for sid in sprite_ids:
        # ROM pointer doesn't matter for export (we read .gfx.lzhal files directly)
        entries.append((0xCE0000 + sid * 0x100, size_enum))

        # Create compressed sprite file
        compressed = compress(tile_data)
        (sprite_dir / f"{sid}.gfx.lzhal").write_bytes(compressed)

    (data_dir / "battle_sprites_pointers.bin").write_bytes(_make_pointer_table(entries))

    return bin_dir, sprite_ids


def _make_valid_battle_png_and_json(
    png_dir,
    sprite_id: int = 0,
    name: str = "000",
    size_enum: int = 1,
    palette_index: int = 0,
    pixel_value: int = 5,
):
    """Create a valid indexed PNG + JSON pair for a battle sprite."""
    wt, ht = SPRITE_SIZES[size_enum]
    px_w = wt * 8
    px_h = ht * 8

    img = Image.new("P", (px_w, px_h))
    palette = [0] * 768
    for i in range(16):
        palette[i * 3] = i * 16
        palette[i * 3 + 1] = i * 8
        palette[i * 3 + 2] = i * 4
    img.putpalette(palette)
    img.putdata([pixel_value] * (px_w * px_h))
    img.save(str(png_dir / f"{name}.png"), "PNG")

    meta = {
        "sprite_id": sprite_id,
        "name": name,
        "size_enum": size_enum,
        "tile_width": wt,
        "tile_height": ht,
        "pixel_width": px_w,
        "pixel_height": px_h,
        "palette_index": palette_index,
    }
    (png_dir / f"{name}.json").write_text(json.dumps(meta))
    return meta


# ---------------------------------------------------------------------------
# Tests: LZHAL compress round-trip
# ---------------------------------------------------------------------------


class TestLzhalCompress:
    def test_empty_data(self):
        compressed = compress(b"")
        assert compressed == b"\xff"  # just the terminator
        assert decompress(compressed) == b""

    def test_small_data(self):
        data = b"hello world"
        compressed = compress(data)
        assert decompress(compressed) == data

    def test_exactly_32_bytes(self):
        data = bytes(range(32))
        compressed = compress(data)
        assert decompress(compressed) == data

    def test_33_bytes_uses_extended(self):
        data = bytes(range(33))
        compressed = compress(data)
        assert decompress(compressed) == data
        # Should use extended format (first byte has high bits set)
        assert (compressed[0] & 0xE0) == 0xE0

    def test_large_data(self):
        data = bytes(range(256)) * 10  # 2560 bytes
        compressed = compress(data)
        assert decompress(compressed) == data

    def test_exactly_1024_bytes(self):
        data = bytes([0xAB]) * 1024
        compressed = compress(data)
        assert decompress(compressed) == data

    def test_over_1024_bytes(self):
        data = bytes(range(256)) * 5  # 1280 bytes
        compressed = compress(data)
        assert decompress(compressed) == data

    def test_real_tile_data_round_trip(self):
        """Round-trip with realistic 4BPP tile data."""
        tile_data = _make_tile_data(4, 4)  # 32x32 sprite
        compressed = compress(tile_data)
        assert decompress(compressed) == tile_data


# ---------------------------------------------------------------------------
# Tests: BattleSprite model
# ---------------------------------------------------------------------------


class TestBattleSprite:
    def test_computed_dimensions(self):
        bs = BattleSprite(sprite_id=0, size_enum=1, rom_pointer=0xCE0000)
        assert bs.tile_width == 4
        assert bs.tile_height == 4
        assert bs.pixel_width == 32
        assert bs.pixel_height == 32

    def test_size_enum_4(self):
        bs = BattleSprite(sprite_id=5, size_enum=4, rom_pointer=0xCE0000)
        assert bs.tile_width == 8
        assert bs.tile_height == 8
        assert bs.pixel_width == 64
        assert bs.pixel_height == 64

    def test_size_enum_6(self):
        bs = BattleSprite(sprite_id=10, size_enum=6, rom_pointer=0xCE0000)
        assert bs.tile_width == 16
        assert bs.tile_height == 16
        assert bs.pixel_width == 128
        assert bs.pixel_height == 128

    def test_invalid_size_enum(self):
        bs = BattleSprite(sprite_id=0, size_enum=99, rom_pointer=0xCE0000)
        assert bs.tile_width == 0
        assert bs.tile_height == 0

    def test_model_dump_excludes_rom_pointer(self):
        bs = BattleSprite(sprite_id=42, size_enum=2, rom_pointer=0xCE0000)
        d = bs.model_dump()
        assert d["sprite_id"] == 42
        assert d["size_enum"] == 2
        assert d["tile_width"] == 8
        assert d["tile_height"] == 4
        assert "rom_pointer" not in d  # excluded field

    def test_frozen(self):
        bs = BattleSprite(sprite_id=0, size_enum=1, rom_pointer=0xCE0000)
        with pytest.raises(ValidationError):
            bs.sprite_id = 1


# ---------------------------------------------------------------------------
# Tests: BattleSpriteMetadata validation
# ---------------------------------------------------------------------------


class TestBattleSpriteMetadata:
    def test_valid(self):
        meta = BattleSpriteMetadata(
            sprite_id=0,
            name="test",
            size_enum=1,
            tile_width=4,
            tile_height=4,
            pixel_width=32,
            pixel_height=32,
            palette_index=0,
        )
        assert meta.sprite_id == 0

    def test_invalid_size_enum(self):
        with pytest.raises(ValidationError, match="size_enum"):
            BattleSpriteMetadata(
                sprite_id=0,
                size_enum=7,
                tile_width=4,
                tile_height=4,
                pixel_width=32,
                pixel_height=32,
                palette_index=0,
            )

    def test_invalid_palette_index(self):
        with pytest.raises(ValidationError, match="palette_index"):
            BattleSpriteMetadata(
                sprite_id=0,
                size_enum=1,
                tile_width=4,
                tile_height=4,
                pixel_width=32,
                pixel_height=32,
                palette_index=32,
            )

    def test_negative_sprite_id(self):
        with pytest.raises(ValidationError, match="sprite_id"):
            BattleSpriteMetadata(
                sprite_id=-1,
                size_enum=1,
                tile_width=4,
                tile_height=4,
                pixel_width=32,
                pixel_height=32,
                palette_index=0,
            )

    def test_missing_required_fields(self):
        with pytest.raises(ValidationError):
            BattleSpriteMetadata(sprite_id=0)


# ---------------------------------------------------------------------------
# Tests: _generate_arrangement + encode/decode round-trip
# ---------------------------------------------------------------------------


class TestArrangement:
    def test_4x4_arrangement_length(self):
        arr = _generate_arrangement(4, 4)
        assert len(arr) == 16

    def test_8x8_arrangement_length(self):
        arr = _generate_arrangement(8, 8)
        assert len(arr) == 64

    def test_arrangement_is_permutation(self):
        """Each tile index should appear exactly once."""
        for size_enum, (w, h) in SPRITE_SIZES.items():
            arr = _generate_arrangement(w, h)
            assert sorted(arr) == list(range(w * h)), f"Failed for size_enum {size_enum}"

    def test_encode_decode_round_trip(self):
        """Encoding then decoding a sprite should recover the original pixels."""
        for size_enum, (w, h) in SPRITE_SIZES.items():
            # Create a unique pixel pattern
            pixels = [[(y * w * 8 + x) % 16 for x in range(w * 8)] for y in range(h * 8)]
            encoded = _encode_battle_sprite(pixels, w, h)
            decoded = _decode_battle_sprite(encoded, w, h)
            assert decoded == pixels, f"Round-trip failed for size_enum {size_enum}"


# ---------------------------------------------------------------------------
# Tests: _parse_pointer_table
# ---------------------------------------------------------------------------


class TestParsePointerTable:
    def test_basic_parse(self):
        data = _make_pointer_table([(0xCE606D, 1), (0xCE4E6D, 2)])
        sprites = _parse_pointer_table(data)
        assert len(sprites) == 2
        assert sprites[0].sprite_id == 0
        assert sprites[0].size_enum == 1
        assert sprites[0].rom_pointer == 0xCE606D
        assert sprites[1].sprite_id == 1
        assert sprites[1].size_enum == 2

    def test_empty_table(self):
        sprites = _parse_pointer_table(b"")
        assert sprites == []


# ---------------------------------------------------------------------------
# Tests: export_all_battle_sprites
# ---------------------------------------------------------------------------


class TestExportBattleSprites:
    def test_basic_export(self, tmp_path):
        bin_dir, sprite_ids = _make_test_battle_sprite_dir(tmp_path, sprite_ids=[0, 1])
        out_dir = tmp_path / "assets"
        palette_map = {0: 0, 1: 1}

        count = export_all_battle_sprites(bin_dir, out_dir, palette_map)
        assert count == 2

        # Check PNG files
        for sid in sprite_ids:
            png_path = out_dir / "battle_sprites" / f"{sid:03d}.png"
            assert png_path.exists()
            img = Image.open(png_path)
            assert img.mode == "P"
            assert img.width == 32  # size_enum 1 = 32x32
            assert img.height == 32

        # Check JSON files
        for sid in sprite_ids:
            json_path = out_dir / "battle_sprites" / f"{sid:03d}.json"
            assert json_path.exists()
            data = json.loads(json_path.read_text())
            assert data["sprite_id"] == sid
            assert data["size_enum"] == 1

    def test_export_with_missing_ptr_table(self, tmp_path):
        bin_dir = tmp_path / "bin"
        bin_dir.mkdir()
        out_dir = tmp_path / "assets"
        count = export_all_battle_sprites(bin_dir, out_dir, {})
        assert count == 0

    def test_palette_assignment(self, tmp_path):
        bin_dir, _ = _make_test_battle_sprite_dir(tmp_path, sprite_ids=[0])
        out_dir = tmp_path / "assets"
        palette_map = {0: 5}

        export_all_battle_sprites(bin_dir, out_dir, palette_map)
        data = json.loads((out_dir / "battle_sprites" / "000.json").read_text())
        assert data["palette_index"] == 5

    def test_jasc_palette_export(self, tmp_path):
        bin_dir, _ = _make_test_battle_sprite_dir(tmp_path, sprite_ids=[0])
        out_dir = tmp_path / "assets"

        export_all_battle_sprites(bin_dir, out_dir, {0: 0})

        # Check JASC palette files were created
        for i in range(32):
            pal_path = out_dir / "battle_sprites" / "palettes" / f"{i}.pal"
            assert pal_path.exists()
            text = pal_path.read_text()
            assert text.startswith("JASC-PAL")


# ---------------------------------------------------------------------------
# Tests: pack_battle_sprites validation
# ---------------------------------------------------------------------------


class TestPackBattleSprites:
    def _setup(self, tmp_path):
        bin_dir, sprite_ids = _make_test_battle_sprite_dir(tmp_path, sprite_ids=[0])
        return bin_dir, sprite_ids

    def test_non_indexed_png(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        img = Image.new("RGB", (32, 32), (255, 0, 0))
        img.save(str(png_dir / "000.png"))
        meta = {
            "sprite_id": 0,
            "size_enum": 1,
            "tile_width": 4,
            "tile_height": 4,
            "pixel_width": 32,
            "pixel_height": 32,
            "palette_index": 0,
        }
        (png_dir / "000.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_battle_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_wrong_dimensions(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        img = Image.new("P", (64, 64))
        img.putpalette([0] * 768)
        img.save(str(png_dir / "000.png"))
        meta = {
            "sprite_id": 0,
            "size_enum": 1,
            "tile_width": 4,
            "tile_height": 4,
            "pixel_width": 32,
            "pixel_height": 32,
            "palette_index": 0,
        }
        (png_dir / "000.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_battle_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_pixel_values_out_of_range(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        img = Image.new("P", (32, 32))
        img.putpalette([0] * 768)
        img.putdata([20] * (32 * 32))
        img.save(str(png_dir / "000.png"))
        meta = {
            "sprite_id": 0,
            "size_enum": 1,
            "tile_width": 4,
            "tile_height": 4,
            "pixel_width": 32,
            "pixel_height": 32,
            "palette_index": 0,
        }
        (png_dir / "000.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_battle_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_json_without_png(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        meta = {
            "sprite_id": 0,
            "size_enum": 1,
            "tile_width": 4,
            "tile_height": 4,
            "pixel_width": 32,
            "pixel_height": 32,
            "palette_index": 0,
        }
        (png_dir / "orphan.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_battle_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_invalid_json(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        img = Image.new("P", (32, 32))
        img.putpalette([0] * 768)
        img.save(str(png_dir / "000.png"))
        (png_dir / "000.json").write_text("{invalid json")

        with pytest.raises(PackError, match="1 error"):
            pack_battle_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_invalid_sprite_id(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        img = Image.new("P", (32, 32))
        img.putpalette([0] * 768)
        img.save(str(png_dir / "000.png"))
        meta = {
            "sprite_id": 9999,
            "size_enum": 1,
            "tile_width": 4,
            "tile_height": 4,
            "pixel_width": 32,
            "pixel_height": 32,
            "palette_index": 0,
        }
        (png_dir / "000.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_battle_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_size_enum_mismatch(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        img = Image.new("P", (32, 32))
        img.putpalette([0] * 768)
        img.save(str(png_dir / "000.png"))
        meta = {
            "sprite_id": 0,
            "size_enum": 4,
            "tile_width": 8,
            "tile_height": 8,
            "pixel_width": 64,
            "pixel_height": 64,
            "palette_index": 0,
        }
        (png_dir / "000.json").write_text(json.dumps(meta))

        with pytest.raises(PackError, match="1 error"):
            pack_battle_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_multiple_errors_all_reported(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        # Bad file 1: RGB PNG
        img = Image.new("RGB", (32, 32))
        img.save(str(png_dir / "bad1.png"))
        (png_dir / "bad1.json").write_text(
            json.dumps(
                {
                    "sprite_id": 0,
                    "size_enum": 1,
                    "tile_width": 4,
                    "tile_height": 4,
                    "pixel_width": 32,
                    "pixel_height": 32,
                    "palette_index": 0,
                }
            )
        )

        # Bad file 2: orphan JSON
        (png_dir / "bad2.json").write_text(
            json.dumps(
                {
                    "sprite_id": 0,
                    "size_enum": 1,
                    "tile_width": 4,
                    "tile_height": 4,
                    "pixel_width": 32,
                    "pixel_height": 32,
                    "palette_index": 0,
                }
            )
        )

        with pytest.raises(PackError, match="2 error"):
            pack_battle_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_no_json_files_no_error(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        pack_battle_sprites(png_dir, bin_dir, tmp_path / "out")

    def test_valid_sprite_packs_successfully(self, tmp_path):
        bin_dir, _ = self._setup(tmp_path)
        png_dir = tmp_path / "custom"
        png_dir.mkdir()

        _make_valid_battle_png_and_json(png_dir, sprite_id=0)
        out_dir = tmp_path / "out"
        pack_battle_sprites(png_dir, bin_dir, out_dir)

        # Check output file was created
        assert (out_dir / "0.gfx.lzhal").exists()

    def test_full_round_trip(self, tmp_path):
        """Export -> edit (no-op) -> pack -> decompress should produce identical tile data."""
        bin_dir, _ = _make_test_battle_sprite_dir(tmp_path, sprite_ids=[0], size_enum=1)
        export_dir = tmp_path / "assets"
        palette_map = {0: 0}

        # Export
        export_all_battle_sprites(bin_dir, export_dir, palette_map)

        # Copy exported files to "custom" dir
        png_dir = tmp_path / "custom"
        png_dir.mkdir()
        export_sprite_dir = export_dir / "battle_sprites"
        for f in export_sprite_dir.glob("000.*"):
            (png_dir / f.name).write_bytes(f.read_bytes())

        # Pack
        out_dir = tmp_path / "out"
        pack_battle_sprites(png_dir, bin_dir, out_dir)

        # Compare: decompress original and repacked
        orig_compressed = (bin_dir / "battle_sprites" / "0.gfx.lzhal").read_bytes()
        new_compressed = (out_dir / "0.gfx.lzhal").read_bytes()

        orig_decompressed = decompress(orig_compressed)
        new_decompressed = decompress(new_compressed)
        assert orig_decompressed == new_decompressed


# ---------------------------------------------------------------------------
# Tests: make_indexed_png
# ---------------------------------------------------------------------------


class TestMakeIndexedPng:
    def test_palette_has_16_entries(self):
        palette_rgba = [(i * 16, i * 8, i * 4, 0 if i == 0 else 255) for i in range(16)]
        pixels = [[0] * 16 for _ in range(16)]
        img = make_indexed_png(pixels, palette_rgba, 16, 16)
        assert img.mode == "P"
        raw_pal = img.getpalette()
        assert raw_pal is not None
        our_colors = raw_pal[: 16 * 3]
        expected = []
        for r, g, b, _a in palette_rgba:
            expected.extend([r, g, b])
        assert our_colors == expected
