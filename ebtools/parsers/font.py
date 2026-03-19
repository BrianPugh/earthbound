"""Font parser: 1bpp glyph data + width table -> PNG grid + JSON metadata.

EarthBound has 5 fonts, each with 96 glyphs (EB character codes 0x50-0xAF).
Font data is inverted 1bpp (1=background, 0=foreground), one byte per row.
"""

from pathlib import Path

from PIL import Image
from pydantic import BaseModel

from ebtools.parsers._tiles import decode_1bpp_font_glyph, encode_1bpp_font_glyph

# 96 glyphs per font (EB codes 0x50-0xAF)
GLYPH_COUNT = 96
# Grid layout: 16 columns × 6 rows = 96 glyphs
GRID_COLS = 16
GRID_ROWS = 6


class FontMetadata(BaseModel):
    """JSON sidecar metadata for a font PNG."""

    font_name: str
    glyph_count: int
    byte_stride: int
    grid_columns: int
    grid_rows: int
    widths: list[int]


# Font definitions: name, gfx file, width file, bytes per glyph
# pixel_height = stride (1 byte per row, stride bytes per glyph)
FONTS = {
    "normal": {"gfx": "US/fonts/main.gfx", "widths": "US/fonts/main.bin", "stride": 32},
    "battle": {"gfx": "US/fonts/battle.gfx", "widths": "US/fonts/battle.bin", "stride": 16},
    "tiny": {"gfx": "US/fonts/tiny.gfx", "widths": "US/fonts/tiny.bin", "stride": 8},
    "large": {"gfx": "US/fonts/large.gfx", "widths": "US/fonts/large.bin", "stride": 32},
    "mrsaturn": {"gfx": "US/fonts/mrsaturn.gfx", "widths": "US/fonts/mrsaturn.bin", "stride": 32},
}


def export_font_png(
    gfx_data: bytes,
    width_data: bytes,
    font_name: str,
    stride: int,
    output_dir: Path,
) -> None:
    """Export a font to a PNG grid image and JSON metadata.

    The PNG is a 16×6 grid of 8×stride cells. 1 byte per row, stride rows per glyph.
    Color 0 = background (black), color 1 = foreground (white).
    """
    cell_w = 8
    cell_h = stride  # pixel height = byte stride (1 byte per row)
    img_w = GRID_COLS * cell_w
    img_h = GRID_ROWS * cell_h

    # Create 1-bit indexed PNG (palette: 0=black bg, 1=white fg)
    img = Image.new("P", (img_w, img_h))
    img.putpalette([0, 0, 0, 255, 255, 255] + [0] * (256 * 3 - 6))

    for glyph_idx in range(GLYPH_COUNT):
        col = glyph_idx % GRID_COLS
        row = glyph_idx // GRID_COLS

        # Decode glyph pixels (stride rows)
        pixels = decode_1bpp_font_glyph(gfx_data, glyph_idx * stride, stride)

        # Place in grid
        for py in range(cell_h):
            for px in range(cell_w):
                img.putpixel(
                    (col * cell_w + px, row * cell_h + py),
                    pixels[py][px] if py < len(pixels) else 0,
                )

    output_dir.mkdir(parents=True, exist_ok=True)
    png_path = output_dir / f"{font_name}.png"
    img.save(png_path, transparency=None)

    # Write JSON sidecar with widths and metadata
    metadata = FontMetadata(
        font_name=font_name,
        glyph_count=GLYPH_COUNT,
        byte_stride=stride,
        grid_columns=GRID_COLS,
        grid_rows=GRID_ROWS,
        widths=list(width_data[:GLYPH_COUNT]),
    )
    json_path = output_dir / f"{font_name}.json"
    json_path.write_text(metadata.model_dump_json(indent=2) + "\n")


def export_all_fonts(bin_dir: Path, output_dir: Path) -> int:
    """Export all 5 fonts to PNG + JSON in output_dir/fonts/.

    Returns the number of fonts exported.
    """
    count = 0
    fonts_dir = output_dir / "fonts"

    for font_name, info in FONTS.items():
        gfx_path = bin_dir / info["gfx"]
        width_path = bin_dir / info["widths"]

        if not gfx_path.exists() or not width_path.exists():
            continue

        export_font_png(
            gfx_path.read_bytes(),
            width_path.read_bytes(),
            font_name,
            info["stride"],
            fonts_dir,
        )
        count += 1

    return count


def pack_font(
    png_path: Path,
    json_path: Path,
) -> tuple[bytes, bytes]:
    """Pack a font PNG + JSON back to binary gfx + width data.

    Returns (gfx_bytes, width_bytes).
    """
    metadata = FontMetadata.model_validate_json(json_path.read_bytes())
    stride = metadata.byte_stride
    widths = metadata.widths

    img = Image.open(png_path)
    if img.mode != "P":
        img = img.convert("P")

    cell_w = 8
    cell_h = stride

    gfx_buf = bytearray()
    for glyph_idx in range(GLYPH_COUNT):
        col = glyph_idx % GRID_COLS
        row = glyph_idx // GRID_COLS

        # Extract pixel grid from image
        pixels = []
        for py in range(stride):
            row_pixels = []
            for px in range(cell_w):
                p = img.getpixel((col * cell_w + px, row * cell_h + py))
                row_pixels.append(1 if p else 0)
            pixels.append(row_pixels)

        # Encode to 1bpp (inverted)
        glyph_bytes = encode_1bpp_font_glyph(pixels)
        gfx_buf.extend(glyph_bytes)

    width_buf = bytes(widths)
    return bytes(gfx_buf), width_buf
