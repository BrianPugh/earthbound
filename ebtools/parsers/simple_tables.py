"""Parsers for simple binary data tables -> JSON export and packing.

Covers small, fixed-format tables that are trivially representable as JSON:
- Entity collision tables (5 files)
- Hotspot coordinates
- Footstep sounds
- Battle entry BG table
- Consolation items
- Stat growth parameters
- Tileset table
- Timed delivery/transformation tables
- Various small lookup tables (shake amplitudes, prayer noise, etc.)
- Color tables (attack type palettes, PSI colours, swirl colours, etc.)
"""

import json
import struct
from pathlib import Path

from ebtools.byte_reader import ByteReader

CHARACTERS = ["Ness", "Paula", "Jeff", "Poo"]


def _write_json(path: Path, obj: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")


def _read_json(path: Path) -> object:
    return json.loads(path.read_bytes())


# ---------------------------------------------------------------------------
# Entity collision tables (5 files, all 17 entries x uint16_t LE)
# ---------------------------------------------------------------------------

ENTITY_COLLISION_TABLES = {
    "entity_collision_left_x": "left_x",
    "entity_collision_top_y": "top_y",
    "entity_collision_width": "width",
    "entity_collision_tile_count": "tile_count",
    "entity_collision_height_offset": "height_offset",
}
ENTITY_COLLISION_COUNT = 17


def export_entity_collision_json(bin_dir: Path, output_path: Path) -> None:
    entries = []
    tables: dict[str, list[int]] = {}
    for bin_name, field in ENTITY_COLLISION_TABLES.items():
        data = (bin_dir / f"{bin_name}.bin").read_bytes()
        values = [struct.unpack_from("<h", data, i * 2)[0] for i in range(ENTITY_COLLISION_COUNT)]
        tables[field] = values

    for i in range(ENTITY_COLLISION_COUNT):
        entries.append({field: tables[field][i] for field in ENTITY_COLLISION_TABLES.values()})

    _write_json(output_path, entries)


def pack_entity_collision(json_path: Path, output_dir: Path) -> None:
    entries = _read_json(json_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    for bin_name, field in ENTITY_COLLISION_TABLES.items():
        buf = bytearray()
        for entry in entries:
            buf.extend(struct.pack("<h", entry[field]))
        (output_dir / f"{bin_name}.bin").write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Hotspot coordinates: 56 entries x 8 bytes (x1, y1, x2, y2 as uint16_t)
# ---------------------------------------------------------------------------

HOTSPOT_COUNT = 56


def export_hotspots_json(data: bytes, output_path: Path) -> None:
    r = ByteReader(data)
    entries = []
    for _ in range(HOTSPOT_COUNT):
        entries.append(
            {
                "x1": r.read_le16(),
                "y1": r.read_le16(),
                "x2": r.read_le16(),
                "y2": r.read_le16(),
            }
        )
    _write_json(output_path, entries)


def pack_hotspots(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(struct.pack("<HHHH", e["x1"], e["y1"], e["x2"], e["y2"]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Footstep sound table: 10 entries x uint16_t (SFX IDs)
# ---------------------------------------------------------------------------

FOOTSTEP_COUNT = 10


def export_footstep_sounds_json(data: bytes, output_path: Path) -> None:
    entries = [struct.unpack_from("<H", data, i * 2)[0] for i in range(FOOTSTEP_COUNT)]
    _write_json(output_path, entries)


def pack_footstep_sounds(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for sfx_id in entries:
        buf.extend(struct.pack("<H", sfx_id))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Vertical shake amplitude table: 61 signed bytes
# ---------------------------------------------------------------------------


def export_shake_amplitudes_json(data: bytes, output_path: Path) -> None:
    entries = [struct.unpack_from("b", data, i)[0] for i in range(len(data))]
    _write_json(output_path, entries)


def pack_shake_amplitudes(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for v in entries:
        buf.extend(struct.pack("b", v))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Final Giygas prayer noise table: pairs of (sfx_id, delay)
# ---------------------------------------------------------------------------


def export_prayer_noise_json(data: bytes, output_path: Path) -> None:
    entries = []
    for i in range(0, len(data), 2):
        entries.append({"sfx_id": data[i], "delay": data[i + 1]})
    _write_json(output_path, entries)


def pack_prayer_noise(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(bytes([e["sfx_id"], e["delay"]]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Stat gain modifier table: 4 bytes
# ---------------------------------------------------------------------------


def export_stat_gain_modifiers_json(data: bytes, output_path: Path) -> None:
    _write_json(output_path, list(data))


def pack_stat_gain_modifiers(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(entries))


# ---------------------------------------------------------------------------
# Stats growth vars: 4 characters x 7 stats = 28 bytes
# ---------------------------------------------------------------------------

STAT_NAMES = ["offense", "defense", "speed", "guts", "vitality", "iq", "luck"]


def export_stats_growth_json(data: bytes, output_path: Path) -> None:
    result = {}
    for i, char in enumerate(CHARACTERS):
        base = i * 7
        result[char] = {name: data[base + j] for j, name in enumerate(STAT_NAMES)}
    _write_json(output_path, result)


def pack_stats_growth(json_path: Path, output_path: Path) -> None:
    obj = _read_json(json_path)
    buf = bytearray()
    for char in CHARACTERS:
        for name in STAT_NAMES:
            buf.append(obj[char][name])
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Consolation item table: 2 entries x 9 bytes (enemy_id + 8 item IDs)
# ---------------------------------------------------------------------------

CONSOLATION_ENTRY_SIZE = 9
CONSOLATION_ITEM_SLOTS = 8


def export_consolation_items_json(data: bytes, output_path: Path) -> None:
    entries = []
    count = len(data) // CONSOLATION_ENTRY_SIZE
    for i in range(count):
        base = i * CONSOLATION_ENTRY_SIZE
        entries.append(
            {
                "enemy_id": data[base],
                "items": list(data[base + 1 : base + 1 + CONSOLATION_ITEM_SLOTS]),
            }
        )
    _write_json(output_path, entries)


def pack_consolation_items(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.append(e["enemy_id"])
        buf.extend(bytes(e["items"]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Tileset table: 32 entries x uint16_t (tileset_combo -> tileset_id)
# ---------------------------------------------------------------------------


def export_tileset_table_json(data: bytes, output_path: Path) -> None:
    count = len(data) // 2
    entries = [struct.unpack_from("<H", data, i * 2)[0] for i in range(count)]
    _write_json(output_path, entries)


def pack_tileset_table(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for v in entries:
        buf.extend(struct.pack("<H", v))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Timed item transformation table: N entries x 5 bytes
# ---------------------------------------------------------------------------

ITEM_TRANSFORM_ENTRY_SIZE = 5


def export_timed_item_transforms_json(data: bytes, output_path: Path) -> None:
    entries = []
    count = len(data) // ITEM_TRANSFORM_ENTRY_SIZE
    for i in range(count):
        base = i * ITEM_TRANSFORM_ENTRY_SIZE
        entries.append(
            {
                "item_id": data[base],
                "sfx": data[base + 1],
                "sfx_frequency": data[base + 2],
                "target_item": data[base + 3],
                "transformation_time": data[base + 4],
            }
        )
    _write_json(output_path, entries)


def pack_timed_item_transforms(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(
            bytes(
                [
                    e["item_id"],
                    e["sfx"],
                    e["sfx_frequency"],
                    e["target_item"],
                    e["transformation_time"],
                ]
            )
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Timed delivery table: 10 entries x 20 bytes
# Layout: sprite_id(2) + event_flag(2) + attempt_limit(2) + time_value(2)
#         + delivery_time(2) + success_text_ptr(3) + failure_text_ptr(3)
#         + enter_speed(2) + exit_speed(2)
# ---------------------------------------------------------------------------

DELIVERY_ENTRY_SIZE = 20


def export_timed_delivery_json(data: bytes, output_path: Path) -> None:
    entries = []
    count = len(data) // DELIVERY_ENTRY_SIZE
    for i in range(count):
        base = i * DELIVERY_ENTRY_SIZE
        success_addr = struct.unpack_from("<H", data, base + 10)[0]
        success_bank = data[base + 12]
        failure_addr = struct.unpack_from("<H", data, base + 13)[0]
        failure_bank = data[base + 15]
        entries.append(
            {
                "sprite_id": struct.unpack_from("<H", data, base)[0],
                "event_flag": struct.unpack_from("<H", data, base + 2)[0],
                "attempt_limit": struct.unpack_from("<H", data, base + 4)[0],
                "time_value": struct.unpack_from("<H", data, base + 6)[0],
                "delivery_time": struct.unpack_from("<H", data, base + 8)[0],
                "success_text_ptr": f"0x{(success_bank << 16) | success_addr:06X}",
                "failure_text_ptr": f"0x{(failure_bank << 16) | failure_addr:06X}",
                "enter_speed": struct.unpack_from("<H", data, base + 16)[0],
                "exit_speed": struct.unpack_from("<H", data, base + 18)[0],
            }
        )
    _write_json(output_path, entries)


def pack_timed_delivery(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(struct.pack("<H", e["sprite_id"]))
        buf.extend(struct.pack("<H", e["event_flag"]))
        buf.extend(struct.pack("<H", e["attempt_limit"]))
        buf.extend(struct.pack("<H", e["time_value"]))
        buf.extend(struct.pack("<H", e["delivery_time"]))
        # success_text_ptr: 24-bit SNES addr → 2 byte addr + 1 byte bank
        sptr = int(e["success_text_ptr"], 16)
        buf.extend(struct.pack("<H", sptr & 0xFFFF))
        buf.append((sptr >> 16) & 0xFF)
        # failure_text_ptr: 24-bit SNES addr → 2 byte addr + 1 byte bank
        fptr = int(e["failure_text_ptr"], 16)
        buf.extend(struct.pack("<H", fptr & 0xFFFF))
        buf.append((fptr >> 16) & 0xFF)
        buf.extend(struct.pack("<H", e["enter_speed"]))
        buf.extend(struct.pack("<H", e["exit_speed"]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# For-sale sign sprite table: 4 entries x uint16_t
# ---------------------------------------------------------------------------


def export_for_sale_signs_json(data: bytes, output_path: Path) -> None:
    count = len(data) // 2
    entries = [struct.unpack_from("<H", data, i * 2)[0] for i in range(count)]
    _write_json(output_path, entries)


def pack_for_sale_signs(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for v in entries:
        buf.extend(struct.pack("<H", v))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Battle entry BG table: N entries x 4 bytes (layer1_id, layer2_id as uint16_t)
# ---------------------------------------------------------------------------

BTL_ENTRY_BG_ENTRY_SIZE = 4


def export_btl_entry_bg_json(data: bytes, output_path: Path) -> None:
    count = len(data) // BTL_ENTRY_BG_ENTRY_SIZE
    entries = []
    for i in range(count):
        base = i * BTL_ENTRY_BG_ENTRY_SIZE
        entries.append(
            {
                "layer1": struct.unpack_from("<H", data, base)[0],
                "layer2": struct.unpack_from("<H", data, base + 2)[0],
            }
        )
    _write_json(output_path, entries)


def pack_btl_entry_bg(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(struct.pack("<HH", e["layer1"], e["layer2"]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# NPC AI table: 19 entries x 2 bytes (flags, replacement_enemy_id)
# ---------------------------------------------------------------------------

NPC_AI_COUNT = 19


def export_npc_ai_json(data: bytes, output_path: Path) -> None:
    entries = []
    count = len(data) // 2
    for i in range(count):
        entries.append(
            {
                "flags": data[i * 2],
                "replacement_enemy_id": data[i * 2 + 1],
            }
        )
    _write_json(output_path, entries)


def pack_npc_ai(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(bytes([e["flags"], e["replacement_enemy_id"]]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Enemy PSI colours: 11 entries x 3 bytes (R, G, B for COLDATA)
# ---------------------------------------------------------------------------


def export_enemy_psi_colours_json(data: bytes, output_path: Path) -> None:
    entries = []
    count = len(data) // 3
    for i in range(count):
        base = i * 3
        entries.append({"r": data[base], "g": data[base + 1], "b": data[base + 2]})
    _write_json(output_path, entries)


def pack_enemy_psi_colours(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(bytes([e["r"], e["g"], e["b"]]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Misc swirl colours: 5 entries x 3 bytes (R, G, B for COLDATA)
# ---------------------------------------------------------------------------


def export_misc_swirl_colours_json(data: bytes, output_path: Path) -> None:
    export_enemy_psi_colours_json(data, output_path)  # Same format


def pack_misc_swirl_colours(json_path: Path, output_path: Path) -> None:
    pack_enemy_psi_colours(json_path, output_path)


# ---------------------------------------------------------------------------
# Attack type palettes: 3 entries x 32 bytes (16 uint16_t SNES colors each)
# ---------------------------------------------------------------------------

ATTACK_PALETTE_SIZE = 32
ATTACK_TYPE_NAMES = ["physical", "fire", "freeze"]


def export_attack_type_palettes_json(data: bytes, output_path: Path) -> None:
    result = {}
    for i, name in enumerate(ATTACK_TYPE_NAMES):
        base = i * ATTACK_PALETTE_SIZE
        colors = [struct.unpack_from("<H", data, base + j * 2)[0] for j in range(16)]
        result[name] = colors
    _write_json(output_path, result)


def pack_attack_type_palettes(json_path: Path, output_path: Path) -> None:
    obj = _read_json(json_path)
    buf = bytearray()
    for name in ATTACK_TYPE_NAMES:
        for c in obj[name]:
            buf.extend(struct.pack("<H", c))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Movement text string palette: 4 SNES colors (uint16_t x 4)
# ---------------------------------------------------------------------------


def export_movement_palette_json(data: bytes, output_path: Path) -> None:
    entries = [struct.unpack_from("<H", data, i * 2)[0] for i in range(len(data) // 2)]
    _write_json(output_path, entries)


def pack_movement_palette(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for v in entries:
        buf.extend(struct.pack("<H", v))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Window border animation tiles: ROW1 (9 tiles + null term = 20 bytes) + ROW2 (9 tiles = 18 bytes)
# ---------------------------------------------------------------------------

BORDER_ANIM_ROW1_COUNT = 10  # 9 tiles + 1 null terminator
BORDER_ANIM_ROW2_COUNT = 9


def export_window_border_anim_json(data: bytes, output_path: Path) -> None:
    row1 = [struct.unpack_from("<H", data, i * 2)[0] for i in range(BORDER_ANIM_ROW1_COUNT)]
    row2 = [struct.unpack_from("<H", data, 20 + i * 2)[0] for i in range(BORDER_ANIM_ROW2_COUNT)]
    _write_json(output_path, {"row1": row1, "row2": row2})


def pack_window_border_anim(json_path: Path, output_path: Path) -> None:
    obj = _read_json(json_path)
    buf = bytearray()
    for v in obj["row1"]:
        buf.extend(struct.pack("<H", v))
    for v in obj["row2"]:
        buf.extend(struct.pack("<H", v))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Per-sector data tables (large grid tables: 80x32 sectors)
# ---------------------------------------------------------------------------

SECTOR_ROWS = 32
SECTOR_COLS = 80
SECTOR_COUNT = SECTOR_ROWS * SECTOR_COLS  # 2560


def export_per_sector_music_json(data: bytes, output_path: Path) -> None:
    """Per-sector music: 2560 bytes, 1 byte per sector (music zone ID)."""
    grid = []
    for row in range(SECTOR_ROWS):
        grid.append(list(data[row * SECTOR_COLS : (row + 1) * SECTOR_COLS]))
    _write_json(output_path, grid)


def pack_per_sector_music(json_path: Path, output_path: Path) -> None:
    grid = _read_json(json_path)
    buf = bytearray()
    for row in grid:
        buf.extend(bytes(row))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


def export_per_sector_u16_json(data: bytes, output_path: Path) -> None:
    """Per-sector uint16 tables (attributes, town_map): 5120 bytes, 2 bytes per sector."""
    grid = []
    for row in range(SECTOR_ROWS):
        row_data = []
        for col in range(SECTOR_COLS):
            offset = (row * SECTOR_COLS + col) * 2
            row_data.append(struct.unpack_from("<H", data, offset)[0])
        grid.append(row_data)
    _write_json(output_path, grid)


def pack_per_sector_u16(json_path: Path, output_path: Path) -> None:
    grid = _read_json(json_path)
    buf = bytearray()
    for row in grid:
        for v in row:
            buf.extend(struct.pack("<H", v))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Sprite placement table + pointer table
# ---------------------------------------------------------------------------


def export_sprite_placement_json(ptr_data: bytes, table_data: bytes, output_path: Path) -> None:
    """Store sprite placement ptr+data as raw byte lists for lossless round-trip."""
    _write_json(
        output_path,
        {
            "pointer_table": list(ptr_data),
            "placement_data": list(table_data),
        },
    )


def pack_sprite_placement(json_path: Path, ptr_output: Path, table_output: Path) -> None:
    obj = _read_json(json_path)
    ptr_output.parent.mkdir(parents=True, exist_ok=True)
    table_output.parent.mkdir(parents=True, exist_ok=True)
    ptr_output.write_bytes(bytes(obj["pointer_table"]))
    table_output.write_bytes(bytes(obj["placement_data"]))


# ---------------------------------------------------------------------------
# Overworld event music table + pointer table
# ---------------------------------------------------------------------------


def export_event_music_json(ptr_data: bytes, table_data: bytes, output_path: Path) -> None:
    """Store event music ptr+data as raw byte lists for lossless round-trip."""
    _write_json(
        output_path,
        {
            "pointer_table": list(ptr_data),
            "music_data": list(table_data),
        },
    )


def pack_event_music(json_path: Path, ptr_output: Path, table_output: Path) -> None:
    obj = _read_json(json_path)
    ptr_output.parent.mkdir(parents=True, exist_ok=True)
    table_output.parent.mkdir(parents=True, exist_ok=True)
    ptr_output.write_bytes(bytes(obj["pointer_table"]))
    table_output.write_bytes(bytes(obj["music_data"]))


# ---------------------------------------------------------------------------
# Enemy battle groups table (variable-length, 0xFF terminated)
# ---------------------------------------------------------------------------


def export_enemy_battle_groups_json(data: bytes, output_path: Path) -> None:
    """Parse variable-length enemy battle groups.

    Format: sequences of 2-byte enemy IDs terminated by 0xFF byte.
    Groups are concatenated; the pointer table indexes into them.
    We store the raw bytes as a flat list of groups.
    """
    groups: list[list[int]] = []
    i = 0
    while i < len(data):
        group: list[int] = []
        while i < len(data):
            if data[i] == 0xFF:
                i += 1
                break
            if i + 1 < len(data):
                enemy_id = struct.unpack_from("<H", data, i)[0]
                group.append(enemy_id)
                i += 2
            else:
                break
        groups.append(group)
    _write_json(output_path, groups)


def pack_enemy_battle_groups(json_path: Path, output_path: Path) -> None:
    groups = _read_json(json_path)
    buf = bytearray()
    for group in groups:
        for enemy_id in group:
            buf.extend(struct.pack("<H", enemy_id))
        buf.append(0xFF)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Enemy placement groups + pointer table
# ---------------------------------------------------------------------------


def export_enemy_placement_groups_json(ptr_data: bytes, group_data: bytes, output_path: Path) -> None:
    """Parse enemy placement groups (variable-length records)."""
    # 24-bit ROM pointers stored as 4 bytes (3 used + 1 padding? or 3-byte ptrs)
    # Actually these are within-bank 16-bit pointers based on the size (812 = 203*4)
    # Let me check: 812/4 = 203 entries of 4 bytes = likely 24-bit ptr + pad
    # But 812/2 = 406 entries. Let me check the actual format.

    # From the research: "203 entries × 4 bytes (24-bit ROM pointers to placement data)"
    # Actually re-reading: the ptr table has within-bank offsets.
    # 812 bytes / 203 entries = 4 bytes each.
    # These may be 32-bit absolute pointers or 16-bit ptrs with other data.
    # For safety, store as raw hex list with the raw group data.
    _write_json(
        output_path,
        {
            "pointer_table": list(ptr_data),
            "group_data": list(group_data),
        },
    )


def pack_enemy_placement_groups(json_path: Path, ptr_output: Path, group_output: Path) -> None:
    obj = _read_json(json_path)
    ptr_output.parent.mkdir(parents=True, exist_ok=True)
    group_output.parent.mkdir(parents=True, exist_ok=True)
    ptr_output.write_bytes(bytes(obj["pointer_table"]))
    group_output.write_bytes(bytes(obj["group_data"]))


# ---------------------------------------------------------------------------
# Map enemy placement: raw binary stored as byte list
# ---------------------------------------------------------------------------


def export_raw_byte_list_json(data: bytes, output_path: Path) -> None:
    """Generic export for binary data as a JSON byte array.

    For tables where we don't yet understand the full structure but want
    them tracked as JSON for completeness.
    """
    _write_json(output_path, list(data))


def pack_raw_byte_list(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(entries))


# ---------------------------------------------------------------------------
# Global map tileset/palette data: 2560 bytes (80x32 grid of packed bytes)
# ---------------------------------------------------------------------------


def export_tileset_palette_data_json(data: bytes, output_path: Path) -> None:
    """Export as 32x80 grid matching sector layout."""
    grid = []
    for row in range(SECTOR_ROWS):
        grid.append(list(data[row * SECTOR_COLS : (row + 1) * SECTOR_COLS]))
    _write_json(output_path, grid)


def pack_tileset_palette_data(json_path: Path, output_path: Path) -> None:
    grid = _read_json(json_path)
    buf = bytearray()
    for row in grid:
        buf.extend(bytes(row))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# EB text encoding helpers
# ---------------------------------------------------------------------------

TextTable = dict[int, str]


def _decode_eb_string(data: bytes, text_table: TextTable) -> str:
    """Decode EB-encoded bytes to a string using the text table.  Stops at 0x00."""
    return "".join(text_table.get(b, f"\\x{b:02X}") for b in data if b != 0x00 and b in text_table)


def _encode_eb_string(text: str, text_table: TextTable, pad_to: int) -> bytearray:
    """Encode a string to EB bytes, null-padded to *pad_to* bytes."""
    reverse: dict[str, int] = {}
    for code, char in text_table.items():
        if char not in reverse:
            reverse[char] = code
    buf = bytearray()
    for ch in text:
        if ch not in reverse:
            raise ValueError(f"Unmappable character '{ch}'")
        buf.append(reverse[ch])
    if len(buf) > pad_to:
        raise ValueError(f"String too long ({len(buf)} > {pad_to} bytes)")
    buf.extend(b"\x00" * (pad_to - len(buf)))
    return buf


# ---------------------------------------------------------------------------
# PSI name table: 17 entries × 25 bytes, null-padded EB text
# ---------------------------------------------------------------------------

PSI_NAME_ENTRY_SIZE = 25
PSI_NAME_COUNT = 17


def export_psi_names_json(data: bytes, text_table: TextTable, output_path: Path) -> None:
    names = []
    for i in range(PSI_NAME_COUNT):
        entry = data[i * PSI_NAME_ENTRY_SIZE : (i + 1) * PSI_NAME_ENTRY_SIZE]
        names.append(_decode_eb_string(entry, text_table))
    _write_json(output_path, names)


def pack_psi_names(json_path: Path, text_table: TextTable, output_path: Path) -> None:
    names = _read_json(json_path)
    buf = bytearray()
    for name in names:
        buf.extend(_encode_eb_string(name, text_table, PSI_NAME_ENTRY_SIZE))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# PSI suffixes: 5 entries × 2 bytes, null-padded EB text
# ---------------------------------------------------------------------------

PSI_SUFFIX_ENTRY_SIZE = 2
PSI_SUFFIX_COUNT = 5


def export_psi_suffixes_json(data: bytes, text_table: TextTable, output_path: Path) -> None:
    suffixes = []
    for i in range(PSI_SUFFIX_COUNT):
        entry = data[i * PSI_SUFFIX_ENTRY_SIZE : (i + 1) * PSI_SUFFIX_ENTRY_SIZE]
        suffixes.append(_decode_eb_string(entry, text_table))
    _write_json(output_path, suffixes)


def pack_psi_suffixes(json_path: Path, text_table: TextTable, output_path: Path) -> None:
    suffixes = _read_json(json_path)
    buf = bytearray()
    for s in suffixes:
        buf.extend(_encode_eb_string(s, text_table, PSI_SUFFIX_ENTRY_SIZE))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Single EB-text strings (phone_call_text, status_equip_window_text_7/14)
# ---------------------------------------------------------------------------


def export_eb_string_json(data: bytes, text_table: TextTable, output_path: Path) -> None:
    """Export a single null-terminated EB-encoded string as a JSON string."""
    _write_json(output_path, _decode_eb_string(data, text_table))


def pack_eb_string(json_path: Path, text_table: TextTable, output_path: Path) -> None:
    """Pack a JSON string back to null-terminated EB-encoded bytes."""
    text = _read_json(json_path)
    reverse: dict[str, int] = {}
    for code, char in text_table.items():
        if char not in reverse:
            reverse[char] = code
    buf = bytearray()
    for ch in text:
        if ch not in reverse:
            raise ValueError(f"Unmappable character '{ch}'")
        buf.append(reverse[ch])
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Guardian text (ending): 3 null-terminated EB strings, sequential
# ---------------------------------------------------------------------------

GUARDIAN_TEXT_OFFSETS = [0, 7, 14]  # offsets of each string
GUARDIAN_TEXT_SIZES = [7, 7, 10]  # max size of each string (incl null)


def export_guardian_text_json(data: bytes, text_table: TextTable, output_path: Path) -> None:
    strings = []
    for off, sz in zip(GUARDIAN_TEXT_OFFSETS, GUARDIAN_TEXT_SIZES):
        strings.append(_decode_eb_string(data[off : off + sz], text_table))
    _write_json(output_path, strings)


def pack_guardian_text(json_path: Path, text_table: TextTable, output_path: Path) -> None:
    strings = _read_json(json_path)
    buf = bytearray()
    for text, sz in zip(strings, GUARDIAN_TEXT_SIZES):
        buf.extend(_encode_eb_string(text, text_table, sz))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Status equip text: multi-section EB text
#   TEXT_4 (offset 0, 35 bytes): PSI info prompt
#   TEXT_5 (offset 35, 9×16 bytes): affliction names
#   TEXT_6 (offset 179, 16 bytes): homesick text
# ---------------------------------------------------------------------------

_STATUS_TEXT4_SIZE = 35
_STATUS_TEXT5_OFFSET = 35
_STATUS_TEXT5_COUNT = 9
_STATUS_TEXT5_ENTRY_SIZE = 16
_STATUS_TEXT6_OFFSET = 179
_STATUS_TEXT6_SIZE = 16


def export_status_equip_text_json(data: bytes, text_table: TextTable, output_path: Path) -> None:
    psi_prompt = _decode_eb_string(data[0:_STATUS_TEXT4_SIZE], text_table)
    afflictions = []
    for i in range(_STATUS_TEXT5_COUNT):
        off = _STATUS_TEXT5_OFFSET + i * _STATUS_TEXT5_ENTRY_SIZE
        afflictions.append(_decode_eb_string(data[off : off + _STATUS_TEXT5_ENTRY_SIZE], text_table))
    homesick = _decode_eb_string(data[_STATUS_TEXT6_OFFSET : _STATUS_TEXT6_OFFSET + _STATUS_TEXT6_SIZE], text_table)
    _write_json(
        output_path,
        {
            "psi_prompt": psi_prompt,
            "afflictions": afflictions,
            "homesick": homesick,
        },
    )


def pack_status_equip_text(json_path: Path, text_table: TextTable, output_path: Path) -> None:
    obj = _read_json(json_path)
    buf = bytearray()
    buf.extend(_encode_eb_string(obj["psi_prompt"], text_table, _STATUS_TEXT4_SIZE))
    for aff in obj["afflictions"]:
        buf.extend(_encode_eb_string(aff, text_table, _STATUS_TEXT5_ENTRY_SIZE))
    buf.extend(_encode_eb_string(obj["homesick"], text_table, _STATUS_TEXT6_SIZE))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Status equip window text 8-13: multi-section EB text
#   ETEXT_8  (offset 0,  8 bytes): "Offense:" label
#   ETEXT_9  (offset 8,  8 bytes): "Defense:" label
#   ETEXT_10 (offset 16, 4×11 bytes): equipment slot labels
#   ETEXT_11 (offset 60, 4×8 bytes): equipment category titles
#   ETEXT_12 (offset 92, 10 bytes): "(Nothing)" text
#   ETEXT_13 (offset 102, 5 bytes): "None" text
# ---------------------------------------------------------------------------

_ETEXT8_SIZE = 8
_ETEXT9_SIZE = 8
_ETEXT10_STRIDE = 11
_ETEXT10_COUNT = 4
_ETEXT11_STRIDE = 8
_ETEXT11_COUNT = 4
_ETEXT12_SIZE = 10
_ETEXT13_SIZE = 5


def export_status_equip_text_8_13_json(data: bytes, text_table: TextTable, output_path: Path) -> None:
    offense = _decode_eb_string(data[0:_ETEXT8_SIZE], text_table)
    defense = _decode_eb_string(data[8 : 8 + _ETEXT9_SIZE], text_table)
    slot_labels = []
    for i in range(_ETEXT10_COUNT):
        off = 16 + i * _ETEXT10_STRIDE
        slot_labels.append(_decode_eb_string(data[off : off + _ETEXT10_STRIDE], text_table))
    category_titles = []
    for i in range(_ETEXT11_COUNT):
        off = 60 + i * _ETEXT11_STRIDE
        category_titles.append(_decode_eb_string(data[off : off + _ETEXT11_STRIDE], text_table))
    nothing = _decode_eb_string(data[92 : 92 + _ETEXT12_SIZE], text_table)
    none = _decode_eb_string(data[102 : 102 + _ETEXT13_SIZE], text_table)
    _write_json(
        output_path,
        {
            "offense_label": offense,
            "defense_label": defense,
            "equipment_slot_labels": slot_labels,
            "equipment_category_titles": category_titles,
            "nothing_text": nothing,
            "none_text": none,
        },
    )


def pack_status_equip_text_8_13(json_path: Path, text_table: TextTable, output_path: Path) -> None:
    obj = _read_json(json_path)
    buf = bytearray()
    buf.extend(_encode_eb_string(obj["offense_label"], text_table, _ETEXT8_SIZE))
    buf.extend(_encode_eb_string(obj["defense_label"], text_table, _ETEXT9_SIZE))
    for label in obj["equipment_slot_labels"]:
        buf.extend(_encode_eb_string(label, text_table, _ETEXT10_STRIDE))
    for title in obj["equipment_category_titles"]:
        buf.extend(_encode_eb_string(title, text_table, _ETEXT11_STRIDE))
    buf.extend(_encode_eb_string(obj["nothing_text"], text_table, _ETEXT12_SIZE))
    buf.extend(_encode_eb_string(obj["none_text"], text_table, _ETEXT13_SIZE))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Telephone contacts: 7 entries × 31 bytes
#   label[25] (EB text, null-padded) + event_flag[2] (LE16) + text_ptr[4] (LE32)
# ---------------------------------------------------------------------------

TELEPHONE_CONTACT_ENTRY_SIZE = 31
TELEPHONE_CONTACT_LABEL_LEN = 25


def export_telephone_contacts_json(data: bytes, text_table: TextTable, output_path: Path) -> None:
    num_entries = len(data) // TELEPHONE_CONTACT_ENTRY_SIZE
    entries = []
    for i in range(num_entries):
        off = i * TELEPHONE_CONTACT_ENTRY_SIZE
        entry = data[off : off + TELEPHONE_CONTACT_ENTRY_SIZE]
        label = _decode_eb_string(entry[:TELEPHONE_CONTACT_LABEL_LEN], text_table)
        event_flag = struct.unpack_from("<H", entry, TELEPHONE_CONTACT_LABEL_LEN)[0]
        text_ptr = struct.unpack_from("<I", entry, TELEPHONE_CONTACT_LABEL_LEN + 2)[0]
        entries.append(
            {
                "label": label,
                "event_flag": f"0x{event_flag:04X}",
                "text_ptr": f"0x{text_ptr:08X}",
            }
        )
    _write_json(output_path, entries)


def pack_telephone_contacts(
    json_path: Path,
    text_table: TextTable,
    output_path: Path,
    addr_remap: dict[int, int] | None = None,
) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for entry in entries:
        buf.extend(_encode_eb_string(entry["label"], text_table, TELEPHONE_CONTACT_LABEL_LEN))
        buf.extend(struct.pack("<H", int(entry["event_flag"], 16)))
        ptr = int(entry["text_ptr"], 16)
        if addr_remap and ptr in addr_remap:
            ptr = addr_remap[ptr]
        buf.extend(struct.pack("<I", ptr))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# PSI animation config: 34 entries × 12 bytes
# ---------------------------------------------------------------------------

PSI_ANIM_CFG_ENTRY_SIZE = 12
PSI_ANIM_COUNT = 34

_PSI_TARGET_NAMES = {0: "single", 1: "row", 2: "all_enemies", 3: "random"}
_PSI_TARGET_VALUES = {v: k for k, v in _PSI_TARGET_NAMES.items()}


def export_psi_anim_cfg_json(data: bytes, output_path: Path) -> None:
    entries = []
    for i in range(PSI_ANIM_COUNT):
        off = i * PSI_ANIM_CFG_ENTRY_SIZE
        e = data[off : off + PSI_ANIM_CFG_ENTRY_SIZE]
        gfx_loword = struct.unpack_from("<H", e, 0)[0]
        color_rgb = struct.unpack_from("<H", e, 10)[0]
        entries.append(
            {
                "gfx_loword": f"0x{gfx_loword:04X}",
                "frame_hold": e[2],
                "pal_anim_frames": e[3],
                "pal_anim_lower": e[4],
                "pal_anim_upper": e[5],
                "total_frames": e[6],
                "target_type": _PSI_TARGET_NAMES.get(e[7], str(e[7])),
                "color_change_start": e[8],
                "color_change_duration": e[9],
                "color_rgb": f"0x{color_rgb:04X}",
            }
        )
    _write_json(output_path, entries)


def pack_psi_anim_cfg(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(struct.pack("<H", int(e["gfx_loword"], 16)))
        buf.append(e["frame_hold"])
        buf.append(e["pal_anim_frames"])
        buf.append(e["pal_anim_lower"])
        buf.append(e["pal_anim_upper"])
        buf.append(e["total_frames"])
        target = e["target_type"]
        buf.append(_PSI_TARGET_VALUES[target] if target in _PSI_TARGET_VALUES else int(target))
        buf.append(e["color_change_start"])
        buf.append(e["color_change_duration"])
        buf.extend(struct.pack("<H", int(e["color_rgb"], 16)))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Battle sprites pointers: 110 entries × 5 bytes
# ---------------------------------------------------------------------------

BATTLE_SPRITES_ENTRY_SIZE = 5
BATTLE_SPRITES_COUNT = 110

_SPRITE_SIZE_NAMES = {
    1: "32x32",
    2: "64x32",
    3: "32x64",
    4: "64x64",
    5: "128x64",
    6: "128x128",
}
_SPRITE_SIZE_VALUES = {v: k for k, v in _SPRITE_SIZE_NAMES.items()}


def export_battle_sprites_pointers_json(data: bytes, output_path: Path) -> None:
    entries = []
    for i in range(BATTLE_SPRITES_COUNT):
        off = i * BATTLE_SPRITES_ENTRY_SIZE
        gfx_ptr = struct.unpack_from("<I", data, off)[0] & 0xFFFFFF
        size_enum = data[off + 4]
        entries.append(
            {
                "gfx_pointer": f"0x{gfx_ptr:06X}",
                "size": _SPRITE_SIZE_NAMES.get(size_enum, str(size_enum)),
            }
        )
    _write_json(output_path, entries)


def pack_battle_sprites_pointers(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        ptr = int(e["gfx_pointer"], 16)
        buf.extend(struct.pack("<I", ptr))
        size = e["size"]
        buf.append(_SPRITE_SIZE_VALUES[size] if size in _SPRITE_SIZE_VALUES else int(size))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Battle entry pointer table: 484 entries × 8 bytes
# ---------------------------------------------------------------------------

BTL_ENTRY_PTR_ENTRY_SIZE = 8


def export_btl_entry_ptr_table_json(data: bytes, output_path: Path) -> None:
    num_entries = len(data) // BTL_ENTRY_PTR_ENTRY_SIZE
    entries = []
    for i in range(num_entries):
        off = i * BTL_ENTRY_PTR_ENTRY_SIZE
        e = data[off : off + BTL_ENTRY_PTR_ENTRY_SIZE]
        rom_ptr = e[0] | (e[1] << 8) | (e[2] << 16)
        enemy_id = struct.unpack_from("<H", e, 3)[0]
        entries.append(
            {
                "group_pointer": f"0x{rom_ptr:06X}",
                "enemy_id": enemy_id,
                "unknown5": e[5],
                "unknown6": e[6],
                "letterbox_style": e[7],
            }
        )
    _write_json(output_path, entries)


def pack_btl_entry_ptr_table(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        ptr = int(e["group_pointer"], 16)
        buf.append(ptr & 0xFF)
        buf.append((ptr >> 8) & 0xFF)
        buf.append((ptr >> 16) & 0xFF)
        buf.extend(struct.pack("<H", e["enemy_id"]))
        buf.append(e["unknown5"])
        buf.append(e["unknown6"])
        buf.append(e["letterbox_style"])
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# BG distortion table: 135 entries × 17 bytes
# ---------------------------------------------------------------------------

BG_DISTORTION_ENTRY_SIZE = 17


def export_bg_distortion_table_json(data: bytes, output_path: Path) -> None:
    num_entries = len(data) // BG_DISTORTION_ENTRY_SIZE
    entries = []
    for i in range(num_entries):
        off = i * BG_DISTORTION_ENTRY_SIZE
        e = data[off : off + BG_DISTORTION_ENTRY_SIZE]
        entries.append(
            {
                "duration": struct.unpack_from("<H", e, 0)[0],
                "type": e[2],
                "ripple_frequency": struct.unpack_from("<h", e, 3)[0],
                "ripple_amplitude": struct.unpack_from("<h", e, 5)[0],
                "speed": e[7],
                "compression_rate": struct.unpack_from("<h", e, 8)[0],
                "freq_accel": struct.unpack_from("<h", e, 10)[0],
                "amp_accel": struct.unpack_from("<h", e, 12)[0],
                "speed_accel": struct.unpack_from("<b", e, 14)[0],
                "comp_accel": struct.unpack_from("<h", e, 15)[0],
            }
        )
    _write_json(output_path, entries)


def pack_bg_distortion_table(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(struct.pack("<H", e["duration"]))
        buf.append(e["type"])
        buf.extend(struct.pack("<h", e["ripple_frequency"]))
        buf.extend(struct.pack("<h", e["ripple_amplitude"]))
        buf.append(e["speed"])
        buf.extend(struct.pack("<h", e["compression_rate"]))
        buf.extend(struct.pack("<h", e["freq_accel"]))
        buf.extend(struct.pack("<h", e["amp_accel"]))
        buf.extend(struct.pack("<b", e["speed_accel"]))
        buf.extend(struct.pack("<h", e["comp_accel"]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# BG scrolling table: 120 entries × 10 bytes
# ---------------------------------------------------------------------------

BG_SCROLLING_ENTRY_SIZE = 10


def export_bg_scrolling_table_json(data: bytes, output_path: Path) -> None:
    num_entries = len(data) // BG_SCROLLING_ENTRY_SIZE
    entries = []
    for i in range(num_entries):
        off = i * BG_SCROLLING_ENTRY_SIZE
        e = data[off : off + BG_SCROLLING_ENTRY_SIZE]
        entries.append(
            {
                "duration": struct.unpack_from("<H", e, 0)[0],
                "h_velocity": struct.unpack_from("<h", e, 2)[0],
                "v_velocity": struct.unpack_from("<h", e, 4)[0],
                "h_accel": struct.unpack_from("<h", e, 6)[0],
                "v_accel": struct.unpack_from("<h", e, 8)[0],
            }
        )
    _write_json(output_path, entries)


def pack_bg_scrolling_table(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for e in entries:
        buf.extend(struct.pack("<H", e["duration"]))
        buf.extend(struct.pack("<h", e["h_velocity"]))
        buf.extend(struct.pack("<h", e["v_velocity"]))
        buf.extend(struct.pack("<h", e["h_accel"]))
        buf.extend(struct.pack("<h", e["v_accel"]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Giygas death static transition delays: uint16_t LE array
# ---------------------------------------------------------------------------


def export_giygas_delays_json(data: bytes, output_path: Path) -> None:
    num_entries = len(data) // 2
    entries = []
    for i in range(num_entries):
        entries.append(struct.unpack_from("<H", data, i * 2)[0])
    _write_json(output_path, entries)


def pack_giygas_delays(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for v in entries:
        buf.extend(struct.pack("<H", v))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Attract mode text pointers: 10 × uint32_t LE ROM addresses
# ---------------------------------------------------------------------------

ATTRACT_MODE_SCENE_COUNT = 10


def export_attract_mode_json(data: bytes, output_path: Path) -> None:
    entries = []
    for i in range(ATTRACT_MODE_SCENE_COUNT):
        addr = struct.unpack_from("<I", data, i * 4)[0]
        entries.append(f"0x{addr:08X}")
    _write_json(output_path, entries)


def pack_attract_mode(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for addr_str in entries:
        buf.extend(struct.pack("<I", int(addr_str, 16)))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Per-sector town map: 7680 bytes (32×80 grid, 3 bytes per sector)
# Row stride = 32 * 3 = 96 bytes. Access: data[y * 96 + x * 3 + byte_offset]
# ---------------------------------------------------------------------------

_TOWN_MAP_COLS = 32  # x-sectors per row
_TOWN_MAP_ROWS = 80  # y-sector rows
_TOWN_MAP_BYTES_PER_SECTOR = 3
_TOWN_MAP_ROW_STRIDE = _TOWN_MAP_COLS * _TOWN_MAP_BYTES_PER_SECTOR


def export_per_sector_town_map_json(data: bytes, output_path: Path) -> None:
    """Export as 80×32 grid where each cell is [byte0, byte1, byte2]."""
    grid = []
    for y in range(_TOWN_MAP_ROWS):
        row = []
        for x in range(_TOWN_MAP_COLS):
            off = y * _TOWN_MAP_ROW_STRIDE + x * _TOWN_MAP_BYTES_PER_SECTOR
            row.append(list(data[off : off + _TOWN_MAP_BYTES_PER_SECTOR]))
        grid.append(row)
    _write_json(output_path, grid)


def pack_per_sector_town_map(json_path: Path, output_path: Path) -> None:
    grid = _read_json(json_path)
    buf = bytearray()
    for row in grid:
        for cell in row:
            buf.extend(bytes(cell))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Sound stone config: 119-byte fixed blob with embedded sub-tables
# ---------------------------------------------------------------------------


def export_sound_stone_config_json(data: bytes, output_path: Path) -> None:
    # 9 × 4-byte ROM pointers
    melody_ptrs = []
    for i in range(9):
        ptr = struct.unpack_from("<I", data, i * 4)[0]
        melody_ptrs.append(f"0x{ptr:08X}")
    # 8-entry sub-tables starting at offset 36
    idle_x = list(data[36:44])
    idle_y = list(data[44:52])
    idle_tiles = list(data[52:60])
    idle_pal = list(data[60:68])
    orbit_tiles = list(data[68:76])
    orbit_pal = list(data[76:84])
    # 9 music IDs at offset 84
    music_ids = list(data[84:93])
    # 9 × uint16_t timing values at offset 93
    timing = []
    for i in range(9):
        timing.append(struct.unpack_from("<H", data, 93 + i * 2)[0])
    # 8 melody flags at offset 111
    melody_flags = list(data[111:119])
    _write_json(
        output_path,
        {
            "melody_pointers": melody_ptrs,
            "idle_x": idle_x,
            "idle_y": idle_y,
            "idle_tiles": idle_tiles,
            "idle_palette": idle_pal,
            "orbit_tiles": orbit_tiles,
            "orbit_palette": orbit_pal,
            "music_ids": music_ids,
            "timing": timing,
            "melody_flags": melody_flags,
        },
    )


def pack_sound_stone_config(json_path: Path, output_path: Path) -> None:
    obj = _read_json(json_path)
    buf = bytearray()
    for ptr_str in obj["melody_pointers"]:
        buf.extend(struct.pack("<I", int(ptr_str, 16)))
    buf.extend(bytes(obj["idle_x"]))
    buf.extend(bytes(obj["idle_y"]))
    buf.extend(bytes(obj["idle_tiles"]))
    buf.extend(bytes(obj["idle_palette"]))
    buf.extend(bytes(obj["orbit_tiles"]))
    buf.extend(bytes(obj["orbit_palette"]))
    buf.extend(bytes(obj["music_ids"]))
    for v in obj["timing"]:
        buf.extend(struct.pack("<H", v))
    buf.extend(bytes(obj["melody_flags"]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Sound stone melodies: 992 bytes of melody curve data
# Variable-length per melody, but contiguous. Export as array of signed bytes.
# ---------------------------------------------------------------------------


def export_sound_stone_melodies_json(data: bytes, output_path: Path) -> None:
    """Export as array of signed byte values (pitch curves)."""
    entries = []
    for b in data:
        entries.append(struct.unpack("b", bytes([b]))[0])
    _write_json(output_path, entries)


def pack_sound_stone_melodies(json_path: Path, output_path: Path) -> None:
    entries = _read_json(json_path)
    buf = bytearray()
    for v in entries:
        buf.extend(struct.pack("b", v))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Status equip tile tables: 3 tables × 7 rows × 7 columns × uint16 LE
# Table 0 (TEXT_1): mode=1 tile indices
# Table 1 (TEXT_2): mode=0 tile indices
# Table 2 (TEXT_3): palette multipliers
# Total: 294 bytes
# ---------------------------------------------------------------------------

STATUS_TILE_TABLE_ROWS = 7  # affliction groups
STATUS_TILE_TABLE_COLS = 7  # max affliction values per group
STATUS_TILE_TABLE_NAMES = ["mode1_tiles", "mode0_tiles", "palette_multipliers"]


def export_status_equip_tile_tables_json(data: bytes, output_path: Path) -> None:
    """Export 3 tables of 7×7 uint16 LE values as named 2D arrays."""
    table_size = STATUS_TILE_TABLE_ROWS * STATUS_TILE_TABLE_COLS * 2  # 98 bytes
    result = {}
    for t, name in enumerate(STATUS_TILE_TABLE_NAMES):
        base = t * table_size
        rows = []
        for r in range(STATUS_TILE_TABLE_ROWS):
            row = []
            for c in range(STATUS_TILE_TABLE_COLS):
                off = base + (r * STATUS_TILE_TABLE_COLS + c) * 2
                row.append(struct.unpack_from("<H", data, off)[0])
            rows.append(row)
        result[name] = rows
    _write_json(output_path, result)


def pack_status_equip_tile_tables(json_path: Path, output_path: Path) -> None:
    obj = _read_json(json_path)
    buf = bytearray()
    for name in STATUS_TILE_TABLE_NAMES:
        table = obj[name]
        for row in table:
            for val in row:
                buf.extend(struct.pack("<H", val))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Map enemy placement: 128 columns × 160 rows × uint16 LE encounter IDs
# Row stride = 256 bytes (128 words). Total = 40960 bytes.
# Most entries are 0 (no encounter). Export as 2D grid [row][col].
# ---------------------------------------------------------------------------

ENEMY_MAP_COLS = 128
ENEMY_MAP_ROWS = 160


def export_map_enemy_placement_json(data: bytes, output_path: Path) -> None:
    """Export as 160-row × 128-col grid of encounter IDs."""
    grid = []
    for y in range(ENEMY_MAP_ROWS):
        row = []
        for x in range(ENEMY_MAP_COLS):
            off = y * 256 + x * 2
            row.append(struct.unpack_from("<H", data, off)[0])
        grid.append(row)
    _write_json(output_path, grid)


def pack_map_enemy_placement(json_path: Path, output_path: Path) -> None:
    grid = _read_json(json_path)
    buf = bytearray()
    for row in grid:
        for val in row:
            buf.extend(struct.pack("<H", val))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# Compressed text dictionary: CC 0x15/0x16/0x17 text substitution system
#
# Two binary files work together:
#   compressed_text_data.bin  — 768 null-terminated EB-encoded string fragments
#                               packed contiguously (e.g. " in the \0 that \0...")
#   compressed_text_ptrs.bin  — 767 × 4-byte LE SNES ROM addresses, each pointing
#                               to the start of a fragment in the data blob
#
# The data blob has 768 entries (0-767) but the pointer table only has 767
# (0-766).  Entry 767 exists in ASM but is unreachable via the extracted
# pointer table.  We export all 768 as a JSON string array and on pack
# rebuild both files: all 768 go into the data blob, first 767 get pointers.
# ---------------------------------------------------------------------------

COMPRESSED_TEXT_PTR_COUNT = 767  # entries in pointer table
COMPRESSED_TEXT_TOTAL_COUNT = 768  # entries in data blob (includes trailing)
COMPRESSED_TEXT_SNES_BASE = 0xC8BC2D


def export_compressed_text_json(data: bytes, ptrs: bytes, text_table: TextTable, output_path: Path) -> None:
    """Decode 768 null-terminated EB fragments into a JSON string array.

    Uses the pointer table for the first 767 entries, then reads the
    trailing entry that follows the last pointed-to fragment.
    """
    entries = []
    for i in range(COMPRESSED_TEXT_PTR_COUNT):
        snes_addr = struct.unpack_from("<I", ptrs, i * 4)[0]
        offset = snes_addr - COMPRESSED_TEXT_SNES_BASE
        end = data.index(0x00, offset)
        fragment = data[offset:end]
        entries.append(_decode_eb_string(fragment, text_table))

    # Trailing entry 767: starts right after entry 766's null terminator
    last_snes = struct.unpack_from("<I", ptrs, (COMPRESSED_TEXT_PTR_COUNT - 1) * 4)[0]
    last_offset = last_snes - COMPRESSED_TEXT_SNES_BASE
    trailing_start = data.index(0x00, last_offset) + 1
    if trailing_start < len(data):
        end = data.index(0x00, trailing_start)
        entries.append(_decode_eb_string(data[trailing_start:end], text_table))

    _write_json(output_path, entries)


def pack_compressed_text(json_path: Path, text_table: TextTable, data_output: Path, ptrs_output: Path) -> None:
    """Pack JSON string array back to data + pointer table binaries."""
    entries = _read_json(json_path)
    if len(entries) != COMPRESSED_TEXT_TOTAL_COUNT:
        raise ValueError(f"Expected {COMPRESSED_TEXT_TOTAL_COUNT} entries, got {len(entries)}")

    reverse: dict[str, int] = {}
    for code, char in text_table.items():
        if char not in reverse:
            reverse[char] = code

    # Build data blob: all 768 strings encoded to EB bytes + null terminator
    data_buf = bytearray()
    offsets: list[int] = []
    for text in entries:
        offsets.append(len(data_buf))
        for ch in text:
            if ch not in reverse:
                raise ValueError(f"Unmappable character '{ch}'")
            data_buf.append(reverse[ch])
        data_buf.append(0x00)  # null terminator

    # Build pointer table: only first 767 entries get pointers
    ptrs_buf = bytearray()
    for off in offsets[:COMPRESSED_TEXT_PTR_COUNT]:
        ptrs_buf.extend(struct.pack("<I", off))

    data_output.parent.mkdir(parents=True, exist_ok=True)
    ptrs_output.parent.mkdir(parents=True, exist_ok=True)
    data_output.write_bytes(bytes(data_buf))
    ptrs_output.write_bytes(bytes(ptrs_buf))
