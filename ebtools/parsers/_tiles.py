"""Shared SNES tile encode/decode utilities.

Provides functions for converting between SNES 1BPP, 2BPP, and 4BPP tile
formats and pixel index grids, plus BGR555 palette loading and indexed PNG
creation.
"""

import json
import math
from pathlib import Path

from PIL import Image


class PackError(Exception):
    """Raised when a sprite pack operation encounters invalid input."""


def make_indexed_png(
    pixels_grid: list[list[int]],
    palette_rgba: list[tuple[int, int, int, int]],
    width: int,
    height: int,
) -> Image.Image:
    """Create a paletted (indexed) PNG from pixel indices and an RGBA palette."""
    img = Image.new("P", (width, height))

    # Set palette (RGB, only actual colors — no padding to 256)
    flat_palette: list[int] = []
    for r, g, b, _a in palette_rgba:
        flat_palette.extend([r, g, b])
    img.putpalette(flat_palette)

    # Set pixel data
    pixel_data = []
    for row in pixels_grid:
        pixel_data.extend(row)
    img.putdata(pixel_data)

    # Set transparency for color 0
    img.info["transparency"] = 0

    return img


def decode_1bpp_font_glyph(
    data: bytes,
    offset: int,
    byte_height: int,
) -> list[list[int]]:
    """Decode a 1bpp font glyph (inverted: 1=bg, 0=fg) to pixel indices.

    Parameters
    ----------
    data
        Raw font graphics data.
    offset
        Byte offset into data.
    byte_height
        Number of bytes (rows) per glyph (8, 16, or 32).

    Returns an 8×byte_height grid where 1=foreground, 0=background.
    """
    pixels = []
    for row in range(byte_height):
        b = data[offset + row] if offset + row < len(data) else 0xFF
        row_pixels = []
        for col in range(8):
            bit = 7 - col
            # Inverted: 0 bit = foreground (1), 1 bit = background (0)
            row_pixels.append(0 if (b >> bit) & 1 else 1)
        pixels.append(row_pixels)
    return pixels


def encode_1bpp_font_glyph(pixels: list[list[int]]) -> bytes:
    """Encode pixel indices back to inverted 1bpp font data.

    Each pixel: 1=foreground (stored as 0), 0=background (stored as 1).
    Returns one byte per row.
    """
    data = bytearray()
    for row in pixels:
        b = 0
        for col in range(8):
            bit = 7 - col
            # Foreground (1) -> 0 bit, Background (0) -> 1 bit
            if col < len(row) and row[col] == 0:
                b |= 1 << bit
        data.append(b)
    return bytes(data)


def decode_2bpp_tile(data: bytes, tile_offset: int = 0) -> list[list[int]]:
    """Decode one 8x8 SNES 2BPP interleaved tile (16 bytes) to pixel indices.

    Returns an 8x8 grid where each value is a 2-bit color index (0-3).
    """
    pixels = [[0] * 8 for _ in range(8)]
    for row in range(8):
        bp0 = data[tile_offset + row * 2] if tile_offset + row * 2 < len(data) else 0
        bp1 = data[tile_offset + row * 2 + 1] if tile_offset + row * 2 + 1 < len(data) else 0
        for col in range(8):
            bit = 7 - col
            pixel = (bp0 >> bit) & 1
            pixel |= ((bp1 >> bit) & 1) << 1
            pixels[row][col] = pixel
    return pixels


def encode_2bpp_tile(pixels: list[list[int]]) -> bytes:
    """Encode an 8x8 pixel index grid to SNES 2BPP interleaved tile (16 bytes).

    Each pixel value should be 0-3.
    """
    data = bytearray(16)
    for row in range(8):
        bp0 = 0
        bp1 = 0
        for col in range(8):
            bit = 7 - col
            p = pixels[row][col] & 0x03
            bp0 |= ((p >> 0) & 1) << bit
            bp1 |= ((p >> 1) & 1) << bit
        data[row * 2] = bp0
        data[row * 2 + 1] = bp1
    return bytes(data)


def decode_4bpp_tile(data: bytes, tile_offset: int = 0) -> list[list[int]]:
    """Decode one 8x8 SNES 4BPP interleaved tile (32 bytes) to pixel indices.

    Returns an 8x8 grid where each value is a 4-bit color index (0-15).
    """
    pixels = [[0] * 8 for _ in range(8)]
    for row in range(8):
        # Planes 0-1 at offset + row*2
        bp0 = data[tile_offset + row * 2] if tile_offset + row * 2 < len(data) else 0
        bp1 = data[tile_offset + row * 2 + 1] if tile_offset + row * 2 + 1 < len(data) else 0
        # Planes 2-3 at offset + 16 + row*2
        bp2 = data[tile_offset + 16 + row * 2] if tile_offset + 16 + row * 2 < len(data) else 0
        bp3 = data[tile_offset + 16 + row * 2 + 1] if tile_offset + 16 + row * 2 + 1 < len(data) else 0
        for col in range(8):
            bit = 7 - col
            pixel = (bp0 >> bit) & 1
            pixel |= ((bp1 >> bit) & 1) << 1
            pixel |= ((bp2 >> bit) & 1) << 2
            pixel |= ((bp3 >> bit) & 1) << 3
            pixels[row][col] = pixel
    return pixels


def encode_4bpp_tile(pixels: list[list[int]]) -> bytes:
    """Encode an 8x8 pixel index grid to SNES 4BPP interleaved tile (32 bytes).

    Each pixel value should be 0-15.
    """
    data = bytearray(32)
    for row in range(8):
        bp0 = 0
        bp1 = 0
        bp2 = 0
        bp3 = 0
        for col in range(8):
            bit = 7 - col
            p = pixels[row][col] & 0x0F
            bp0 |= ((p >> 0) & 1) << bit
            bp1 |= ((p >> 1) & 1) << bit
            bp2 |= ((p >> 2) & 1) << bit
            bp3 |= ((p >> 3) & 1) << bit
        data[row * 2] = bp0
        data[row * 2 + 1] = bp1
        data[16 + row * 2] = bp2
        data[16 + row * 2 + 1] = bp3
    return bytes(data)


def decode_sprite_frame(
    bank_data: bytes,
    data_offset: int,
    tile_width: int,
    tile_height: int,
) -> list[list[int]]:
    """Decode a sprite frame from bank data to a pixel index grid.

    The frame data is stored as tile_height rows, each containing tile_width
    concatenated 32-byte 4BPP tiles.

    Returns a (tile_height*8) x (tile_width*8) pixel index grid.
    """
    byte_width = tile_width * 32  # bytes per tile row
    px_w = tile_width * 8
    px_h = tile_height * 8
    pixels = [[0] * px_w for _ in range(px_h)]

    for trow in range(tile_height):
        row_offset = data_offset + trow * byte_width
        for tcol in range(tile_width):
            tile_offset = row_offset + tcol * 32
            tile_pixels = decode_4bpp_tile(bank_data, tile_offset)
            for py in range(8):
                for px in range(8):
                    pixels[trow * 8 + py][tcol * 8 + px] = tile_pixels[py][px]

    return pixels


def encode_sprite_frame(
    pixels: list[list[int]],
    tile_width: int,
    tile_height: int,
) -> bytes:
    """Encode a pixel index grid to sprite frame binary data.

    Returns tile_height rows of tile_width concatenated 32-byte 4BPP tiles.
    """
    byte_width = tile_width * 32
    data = bytearray(tile_height * byte_width)

    for trow in range(tile_height):
        for tcol in range(tile_width):
            # Extract 8x8 block
            tile_pixels = []
            for py in range(8):
                row = []
                for px in range(8):
                    y = trow * 8 + py
                    x = tcol * 8 + px
                    row.append(pixels[y][x] if y < len(pixels) and x < len(pixels[0]) else 0)
                tile_pixels.append(row)
            tile_bytes = encode_4bpp_tile(tile_pixels)
            off = trow * byte_width + tcol * 32
            data[off : off + 32] = tile_bytes

    return bytes(data)


def encode_bgr555_palette(colors: list[tuple[int, int, int]]) -> bytes:
    """Convert a list of RGB tuples (8-bit) to SNES BGR555 palette data.

    Each channel is quantized via ``>> 3`` to recover the 5-bit value.
    Returns 2 bytes per color (little-endian BGR555).
    """
    buf = bytearray()
    for r, g, b in colors:
        r5 = (r >> 3) & 0x1F
        g5 = (g >> 3) & 0x1F
        b5 = (b >> 3) & 0x1F
        word = r5 | (g5 << 5) | (b5 << 10)
        buf.append(word & 0xFF)
        buf.append((word >> 8) & 0xFF)
    return bytes(buf)


def write_jasc_pal(colors: list[tuple[int, int, int]], path: Path) -> None:
    """Write a JASC-PAL palette file (text format used by Aseprite/Paint Shop Pro)."""
    lines = ["JASC-PAL", "0100", str(len(colors))]
    for r, g, b in colors:
        lines.append(f"{r} {g} {b}")
    path.write_text("\n".join(lines) + "\n")


def load_jasc_pal(path: Path) -> list[tuple[int, int, int]]:
    """Read a JASC-PAL palette file and return a list of RGB tuples."""
    text = path.read_text()
    lines = text.strip().splitlines()
    if len(lines) < 3 or lines[0].strip() != "JASC-PAL":
        raise ValueError(f"Not a valid JASC-PAL file: {path}")
    count = int(lines[2].strip())
    colors: list[tuple[int, int, int]] = []
    for i in range(count):
        parts = lines[3 + i].strip().split()
        colors.append((int(parts[0]), int(parts[1]), int(parts[2])))
    return colors


TILESET_COLS = 16  # tiles per row in tileset PNGs


def tiles_to_indexed_png(
    tiles: list[list[list[int]]],
    palette_rgba: list[tuple[int, int, int, int]],
    cols: int = TILESET_COLS,
) -> Image.Image:
    """Arrange decoded tiles into an indexed PNG tileset image.

    Tiles are laid out in a grid *cols* tiles wide.
    """
    num_tiles = len(tiles)
    rows = math.ceil(num_tiles / cols) if num_tiles else 1
    img_w = cols * 8
    img_h = rows * 8
    pixels = [[0] * img_w for _ in range(img_h)]

    for i, tile in enumerate(tiles):
        tx = (i % cols) * 8
        ty = (i // cols) * 8
        for py in range(8):
            for px in range(8):
                pixels[ty + py][tx + px] = tile[py][px]

    return make_indexed_png(pixels, palette_rgba, img_w, img_h)


def indexed_png_to_tiles(
    img: Image.Image,
    cols: int = TILESET_COLS,
) -> list[list[list[int]]]:
    """Split an indexed PNG tileset image back into 8x8 tile pixel grids."""
    if img.mode != "P":
        raise PackError(f"Tileset image must be indexed/palette mode, got {img.mode}")
    w, h = img.size
    pixel_data = list(img.getdata())
    tiles: list[list[list[int]]] = []
    rows = h // 8
    actual_cols = w // 8

    for tr in range(rows):
        for tc in range(actual_cols):
            tile: list[list[int]] = []
            for py in range(8):
                row: list[int] = []
                for px in range(8):
                    x = tc * 8 + px
                    y = tr * 8 + py
                    row.append(pixel_data[y * w + x])
                tile.append(row)
            tiles.append(tile)

    return tiles


def write_arrangement_json(
    path: Path,
    width: int,
    height: int,
    entries: list[int],
    tiles_per_row: int = TILESET_COLS,
    tile_count: int | None = None,
) -> None:
    """Write a tilemap arrangement as JSON.

    Each entry is a raw SNES 16-bit tilemap word:
      bits 0-9: tile index, bit 14: hflip, bit 15: vflip.
    ``tile_count`` records the actual number of tiles in the tileset
    (the PNG may have padding tiles from grid alignment).
    """
    data: dict = {
        "width": width,
        "height": height,
        "tiles_per_row": tiles_per_row,
        "entries": entries,
    }
    if tile_count is not None:
        data["tile_count"] = tile_count
    path.write_text(json.dumps(data, separators=(",", ":")) + "\n")


def read_arrangement_json(path: Path) -> tuple[int, int, list[int], int, int | None]:
    """Read a tilemap arrangement JSON.

    Returns (width, height, entries, tiles_per_row, tile_count).
    tile_count is None if not present.
    """
    data = json.loads(path.read_text())
    return (
        data["width"],
        data["height"],
        data["entries"],
        data.get("tiles_per_row", TILESET_COLS),
        data.get("tile_count"),
    )


def parse_arrangement_entries(arr_data: bytes, count: int) -> list[int]:
    """Parse raw tilemap arrangement bytes into a list of 16-bit entry values."""
    entries: list[int] = []
    for i in range(count):
        off = i * 2
        if off + 1 < len(arr_data):
            entries.append(arr_data[off] | (arr_data[off + 1] << 8))
        else:
            entries.append(0)
    return entries


def encode_arrangement_entries(entries: list[int]) -> bytes:
    """Encode a list of 16-bit tilemap entries back to raw bytes."""
    buf = bytearray(len(entries) * 2)
    for i, e in enumerate(entries):
        buf[i * 2] = e & 0xFF
        buf[i * 2 + 1] = (e >> 8) & 0xFF
    return bytes(buf)


def load_bgr555_palette(pal_bytes: bytes) -> list[tuple[int, int, int, int]]:
    """Convert SNES BGR555 palette data to a list of RGBA tuples.

    Each color is 2 bytes (little-endian BGR555).
    Color 0 is set to transparent (alpha=0).
    """
    colors: list[tuple[int, int, int, int]] = []
    for i in range(0, len(pal_bytes), 2):
        if i + 1 >= len(pal_bytes):
            break
        word = pal_bytes[i] | (pal_bytes[i + 1] << 8)
        r = (word & 0x1F) << 3
        g = ((word >> 5) & 0x1F) << 3
        b = ((word >> 10) & 0x1F) << 3
        # Brighten: replicate top 3 bits into bottom 3
        r |= r >> 5
        g |= g >> 5
        b |= b >> 5
        alpha = 0 if len(colors) == 0 else 255
        colors.append((r, g, b, alpha))
    return colors
