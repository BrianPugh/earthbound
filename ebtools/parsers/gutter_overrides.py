"""Wide-viewport gutter overrides: authored map blocks drawn only in the gutters.

The C port's wide viewport shows map tiles the SNES's 256x224 view never
reaches. Where those tiles lie in a sector with a different tileset than the
loaded one (or off the map), the port draws the loaded tileset's block 0, as
LOAD_MAP_ROW does. At some area edges that reads as a flat-coloured strip
next to scenery cut in half (e.g. half trees at the west edge of the Tenda
Village cave entrance). An override gives such a strip authored block ids of
the *loaded* tileset so the gutter continues the scene.

Source: ``src/custom_assets/maps/gutter_overrides.json`` (original content,
committed). Packed to ``maps/gutter_overrides.bin``::

    u16 count
    count x {
        u16 block_x, u16 block_y   # top-left, in 32x32-pixel map blocks
        u8  width, u8 height       # in blocks
        u8  tileset_combo          # only used while this combo is loaded
        u8  reserved (0)
        u16 blocks[height][width]  # block ids of that tileset, row-major
    }

All values little-endian. The C side (``src/game/map_loader.c``) applies an
entry only to tiles that would otherwise get the block-0 filler, and only
while the SNES-visible band doesn't overlap the entry, so the original view
and collision are unchanged.
"""

import json
import struct
from pathlib import Path

MAP_BLOCKS_WIDE = 256
MAP_BLOCKS_HIGH = 320
MAX_BLOCK_ID = 1023  # block ids are 10 bits (2-bit high field + low byte)
MAX_TILESET_COMBO = 31  # GLOBAL_MAP_TILESETPALETTE_DATA byte >> 3


def pack_gutter_overrides(json_path: Path, out_path: Path) -> int:
    """Pack the gutter override JSON to binary.

    Parameters
    ----------
    json_path
        Path to ``gutter_overrides.json``.
    out_path
        Destination ``gutter_overrides.bin``.

    Returns
    -------
    int
        Number of override entries packed.
    """
    doc = json.loads(json_path.read_text())
    entries = doc["overrides"]
    out = bytearray(struct.pack("<H", len(entries)))
    for entry in entries:
        name = entry.get("name", "?")
        bx = int(entry["block_x"])
        by = int(entry["block_y"])
        combo = int(entry["tileset_combo"])
        rows = entry["blocks"]
        height = len(rows)
        width = len(rows[0]) if rows else 0
        if not (1 <= width <= 255 and 1 <= height <= 255):
            raise ValueError(f"gutter override {name!r}: bad size {width}x{height}")
        if any(len(r) != width for r in rows):
            raise ValueError(f"gutter override {name!r}: rows differ in width")
        if bx < 0 or by < 0 or bx + width > MAP_BLOCKS_WIDE or by + height > MAP_BLOCKS_HIGH:
            raise ValueError(f"gutter override {name!r}: rect off the map")
        if not 0 <= combo <= MAX_TILESET_COMBO:
            raise ValueError(f"gutter override {name!r}: bad tileset_combo {combo}")
        out += struct.pack("<HHBBBB", bx, by, width, height, combo, 0)
        for r in rows:
            for block in r:
                if not 0 <= int(block) <= MAX_BLOCK_ID:
                    raise ValueError(f"gutter override {name!r}: bad block id {block}")
                out += struct.pack("<H", int(block))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(bytes(out))
    return len(entries)
