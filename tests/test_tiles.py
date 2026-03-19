"""Tests for the shared SNES 4BPP tile codec."""

import pytest

from ebtools.parsers._tiles import (
    decode_4bpp_tile,
    decode_sprite_frame,
    encode_4bpp_tile,
    encode_sprite_frame,
    load_bgr555_palette,
)


class TestDecode4bppTile:
    def test_all_zeros(self):
        data = bytes(32)
        pixels = decode_4bpp_tile(data)
        assert pixels == [[0] * 8 for _ in range(8)]

    def test_all_ones_index_15(self):
        data = bytes([0xFF] * 32)
        pixels = decode_4bpp_tile(data)
        assert pixels == [[15] * 8 for _ in range(8)]

    def test_single_pixel_top_left(self):
        """Set only the top-left pixel (bit 7) to color index 1 (plane 0 only)."""
        data = bytearray(32)
        data[0] = 0x80  # row 0, plane 0: bit 7 set
        pixels = decode_4bpp_tile(bytes(data))
        assert pixels[0][0] == 1
        assert pixels[0][1] == 0
        assert pixels[1][0] == 0

    def test_single_pixel_plane2(self):
        """Set only plane 2 for top-left pixel → color index 4."""
        data = bytearray(32)
        data[16] = 0x80  # row 0, plane 2: bit 7 set
        pixels = decode_4bpp_tile(bytes(data))
        assert pixels[0][0] == 4

    def test_all_planes_single_pixel(self):
        """All 4 planes set for top-left pixel → color index 15."""
        data = bytearray(32)
        data[0] = 0x80  # plane 0
        data[1] = 0x80  # plane 1
        data[16] = 0x80  # plane 2
        data[17] = 0x80  # plane 3
        pixels = decode_4bpp_tile(bytes(data))
        assert pixels[0][0] == 15

    def test_with_offset(self):
        prefix = bytes(10)
        tile_data = bytearray(32)
        tile_data[0] = 0x80  # plane 0, row 0, bit 7
        data = prefix + bytes(tile_data)
        pixels = decode_4bpp_tile(data, tile_offset=10)
        assert pixels[0][0] == 1

    def test_truncated_data_pads_zero(self):
        """Short data should not crash; missing bytes treated as 0."""
        data = bytes(16)  # only planes 0-1
        pixels = decode_4bpp_tile(data)
        # Should succeed without error (planes 2-3 default to 0)
        assert len(pixels) == 8
        assert len(pixels[0]) == 8


class TestEncode4bppTile:
    def test_all_zeros(self):
        pixels = [[0] * 8 for _ in range(8)]
        assert encode_4bpp_tile(pixels) == bytes(32)

    def test_all_fifteen(self):
        pixels = [[15] * 8 for _ in range(8)]
        assert encode_4bpp_tile(pixels) == bytes([0xFF] * 32)

    def test_roundtrip_single_pixel(self):
        pixels = [[0] * 8 for _ in range(8)]
        pixels[3][5] = 9  # arbitrary pixel, arbitrary index
        encoded = encode_4bpp_tile(pixels)
        decoded = decode_4bpp_tile(encoded)
        assert decoded == pixels

    def test_roundtrip_all_values(self):
        """Each row has 8 pixels with distinct values 0-7, rows vary."""
        pixels = [[(row + col) % 16 for col in range(8)] for row in range(8)]
        encoded = encode_4bpp_tile(pixels)
        decoded = decode_4bpp_tile(encoded)
        assert decoded == pixels

    def test_masks_to_4bit(self):
        """Values > 15 should be masked to lower 4 bits."""
        pixels = [[0x1F] * 8 for _ in range(8)]  # 0x1F & 0x0F = 15
        encoded = encode_4bpp_tile(pixels)
        decoded = decode_4bpp_tile(encoded)
        assert decoded == [[15] * 8 for _ in range(8)]


class TestSpriteFrame:
    def test_roundtrip_1x1(self):
        """Single tile (8x8) round-trip."""
        pixels = [[(r * 8 + c) % 16 for c in range(8)] for r in range(8)]
        encoded = encode_sprite_frame(pixels, tile_width=1, tile_height=1)
        assert len(encoded) == 32
        decoded = decode_sprite_frame(encoded, 0, tile_width=1, tile_height=1)
        assert decoded == pixels

    def test_roundtrip_2x3(self):
        """Ness-sized sprite (16x24) round-trip."""
        tw, th = 2, 3
        px_w, px_h = tw * 8, th * 8
        pixels = [[(r + c) % 16 for c in range(px_w)] for r in range(px_h)]
        encoded = encode_sprite_frame(pixels, tw, th)
        assert len(encoded) == tw * th * 32
        decoded = decode_sprite_frame(encoded, 0, tw, th)
        assert decoded == pixels

    def test_with_offset(self):
        """Decode from a non-zero offset within larger data."""
        tw, th = 1, 1
        pixels = [[7] * 8 for _ in range(8)]
        frame_bytes = encode_sprite_frame(pixels, tw, th)
        padded = bytes(64) + frame_bytes + bytes(64)
        decoded = decode_sprite_frame(padded, 64, tw, th)
        assert decoded == pixels


class TestLoadBgr555Palette:
    def test_single_color_red(self):
        # BGR555: red = bits 0-4 = 31 → 0x001F
        pal = bytes([0x1F, 0x00])
        colors = load_bgr555_palette(pal)
        assert len(colors) == 1
        r, g, b, a = colors[0]
        assert r == (31 << 3) | (31 >> 2)  # 0xF8 | 0x07 = 0xFF
        assert g == 0
        assert b == 0
        assert a == 0  # color 0 is transparent

    def test_second_color_opaque(self):
        pal = bytes([0x00, 0x00, 0x1F, 0x00])
        colors = load_bgr555_palette(pal)
        assert len(colors) == 2
        assert colors[0][3] == 0  # color 0 transparent
        assert colors[1][3] == 255  # color 1 opaque

    def test_green(self):
        # BGR555: green = bits 5-9 = 31 → (31 << 5) = 0x03E0
        pal = bytes([0xE0, 0x03])
        colors = load_bgr555_palette(pal)
        _r, g, _b, _a = colors[0]
        assert g == (31 << 3) | (31 >> 2)

    def test_blue(self):
        # BGR555: blue = bits 10-14 = 31 → (31 << 10) = 0x7C00
        pal = bytes([0x00, 0x7C])
        colors = load_bgr555_palette(pal)
        _r, _g, b, _a = colors[0]
        assert b == (31 << 3) | (31 >> 2)

    def test_empty(self):
        assert load_bgr555_palette(b"") == []

    def test_odd_length_ignores_trailing(self):
        pal = bytes([0x00, 0x00, 0xFF])  # 3 bytes: 1 full color + 1 trailing
        colors = load_bgr555_palette(pal)
        assert len(colors) == 1
