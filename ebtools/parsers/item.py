"""Item config parser: 39-byte records -> assembly, JSON export, and packing.

Ported from app.d:parseItemConfig.
"""

import json
import struct
from pathlib import Path

from pydantic import BaseModel, Field

from ebtools.byte_reader import ByteReader
from ebtools.config import CommonData, DumpDoc
from ebtools.parsers._common import format_pointer
from ebtools.parsers.enemy import _decode_text

RECORD_SIZE = 39
NAME_BYTES = 25  # first 24 are text, byte 25 is padding/null


class ItemParams(BaseModel):
    """Item effect parameters (4 bytes).

    These four bytes are a generic parameter block whose meaning is
    overloaded by item type:

    Equipment (weapons/armor):
        strength — offense/defense modifier (non-Poo characters)
        epi      — offense/defense modifier (Poo only; signed)
        ep       — secondary stat modifier: guts (weapon), speed (body),
                   luck (arms/other); NOT character-dependent
        special  — miss rate (weapon) or elemental resistance (armor)

    Food / consumables:
        strength — effect type (0=HP, 1=PP, 2=HP+PP, 3=random stat, ...)
        epi      — recovery amount (non-Poo); 0 = full recovery
        ep       — recovery amount (Poo)
        special  — (unused for most food)

    Broken items (Jeff repair):
        strength — (unused)
        epi      — minimum IQ required for Jeff to repair
        ep       — item ID of the repaired result
        special  — (unused)
    """

    strength: int = Field(ge=0, le=255)
    epi: int = Field(ge=0, le=255)
    ep: int = Field(ge=0, le=255)
    special: int = Field(ge=0, le=255)


class Item(BaseModel):
    """A single item configuration entry."""

    id: int = Field(ge=0, le=253)
    symbol: str
    name: str
    type: int = Field(ge=0, le=255)
    cost: int = Field(ge=0, le=65535)
    flags: int = Field(ge=0, le=255)
    flag_names: list[str] = Field(default_factory=list)
    effect: int = Field(ge=0, le=65535)
    params: ItemParams
    help_text_pointer: str  # hex string, e.g. "0xC539C4"


class ItemConfig(BaseModel):
    """Top-level items.json schema."""

    items: list[Item]


def parse_item_config(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Parse item configuration (39-byte records)."""
    filename = f"{base_name}.{extension}"

    with (dir / filename).open("w") as out:
        for i in range(0, len(source), RECORD_SIZE):
            entry = source[i : i + RECORD_SIZE]
            if len(entry) < RECORD_SIZE:
                break

            r = ByteReader(entry)

            out.write(f'  PADDEDEBTEXT "{_decode_text(entry[0:24], doc.textTable)}", 25\n')
            r.pos = 25
            out.write(f"  .BYTE ${r.read_byte():02X} ;Type\n")
            out.write(f"  .WORD {r.read_le16()} ;Cost\n")

            flags = r.read_byte()
            if flags == 0:
                out.write("  .BYTE $00 ;Flags\n")
            else:
                flag_names = [f"ITEM_FLAGS::{common_data.itemFlags[bit]}" for bit in range(8) if flags & (1 << bit)]
                out.write(f"  .BYTE {' | '.join(flag_names)}\n")

            out.write(f"  .WORD ${r.read_le16():04X} ;Effect\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Strength\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;EPI\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;EP\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Special\n")
            out.write(format_pointer(r.read_le32()))
            out.write("\n")

    return [filename]


def _decode_item(
    idx: int,
    entry: bytes,
    text_table: dict[int, str],
    item_names: list[str],
    item_flags: list[str],
) -> Item:
    """Decode a single 39-byte item record into an Item model."""
    r = ByteReader(entry)
    name = _decode_text(entry[0:24], text_table)

    r.pos = 25
    item_type = r.read_byte()
    cost = r.read_le16()
    flags = r.read_byte()
    effect = r.read_le16()
    strength = r.read_byte()
    epi = r.read_byte()
    ep = r.read_byte()
    special = r.read_byte()
    help_text_ptr = r.read_le32()

    symbol = item_names[idx] if idx < len(item_names) else f"UNKNOWN_{idx}"
    flag_name_list = [item_flags[bit] for bit in range(8) if flags & (1 << bit)]

    return Item(
        id=idx,
        symbol=symbol,
        name=name,
        type=item_type,
        cost=cost,
        flags=flags,
        flag_names=flag_name_list,
        effect=effect,
        params=ItemParams(strength=strength, epi=epi, ep=ep, special=special),
        help_text_pointer=f"0x{help_text_ptr:06X}",
    )


def export_items_json(
    item_data: bytes,
    text_table: dict[int, str],
    item_names: list[str],
    item_flags: list[str],
    output_path: Path,
) -> None:
    """Export item configuration binary to an editable JSON file.

    Parameters
    ----------
    item_data
        Raw binary item table (254 × 39 bytes).
    text_table
        EB character code → string mapping.
    item_names
        Symbolic item names from commondefs.yml (254 entries, index 0 = NONE).
    item_flags
        Flag bit names from commondefs.yml (8 entries).
    output_path
        Path to write items.json.
    """
    items = []
    num_items = len(item_data) // RECORD_SIZE

    for idx in range(num_items):
        entry = item_data[idx * RECORD_SIZE : (idx + 1) * RECORD_SIZE]
        items.append(_decode_item(idx, entry, text_table, item_names, item_flags))

    config = ItemConfig(items=items)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def _load_item_config(items_json_path: Path) -> ItemConfig:
    """Load and validate items.json."""
    return ItemConfig.model_validate_json(items_json_path.read_bytes())


def generate_items_header(items_json_path: Path, header_path: Path) -> None:
    """Generate items_generated.h from items.json.

    Produces an enum ItemId and a static ITEM_ID_NAMES array for gen_struct_info.
    """
    config = _load_item_config(items_json_path)
    items = config.items
    max_id = max(item.id for item in items) if items else 0
    count = max_id + 1

    lines = [
        "/* Auto-generated from items.json — do not edit */",
        "#ifndef ITEMS_GENERATED_H",
        "#define ITEMS_GENERATED_H",
        "",
        "enum ItemId {",
    ]

    for item in items:
        lines.append(f"    ITEM_{item.symbol} = 0x{item.id:02X},")

    lines.append("};")
    lines.append("")
    lines.append(f"#define ITEM_ID_COUNT {count}")
    lines.append("")

    # Names array — only compiled when ITEMS_INCLUDE_NAMES is defined
    # (used by gen_struct_info.c, not the game binary)
    lines.append("#ifdef ITEMS_INCLUDE_NAMES")
    lines.append("static const char *ITEM_ID_NAMES[ITEM_ID_COUNT] = {")
    for item in items:
        # Use in-game name if non-empty, otherwise title-case the symbol
        display = item.name if item.name else item.symbol.replace("_", " ").title()
        display = display.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'    [0x{item.id:02X}] = "{display}",')

    lines.append("};")
    lines.append("#endif /* ITEMS_INCLUDE_NAMES */")
    lines.append("")
    lines.append("#endif /* ITEMS_GENERATED_H */")
    lines.append("")

    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text("\n".join(lines))


def pack_items(items_json_path: Path, text_table: dict[int, str], output_path: Path) -> None:
    """Pack items.json back to a 254 × 39-byte binary.

    Parameters
    ----------
    items_json_path
        Path to items.json.
    text_table
        EB character code → string mapping (int key → str value).
    output_path
        Path to write the binary file.
    """
    # Build reverse text table: character → EB byte
    reverse_table: dict[str, int] = {}
    for code, char in text_table.items():
        if char not in reverse_table:
            reverse_table[char] = code

    config = _load_item_config(items_json_path)
    buf = bytearray()

    for item in config.items:
        # Encode name to EB text bytes (max 24 chars, padded to 25 with 0x00)
        name_bytes = bytearray()
        for ch in item.name:
            if ch not in reverse_table:
                raise ValueError(f"Item {item.id} ({item.symbol}): unmappable character '{ch}'")
            name_bytes.append(reverse_table[ch])

        if len(name_bytes) > 24:
            raise ValueError(f"Item {item.id} ({item.symbol}): name too long ({len(name_bytes)} > 24 bytes)")

        # Pad to 25 bytes with 0x00
        name_bytes.extend(b"\x00" * (NAME_BYTES - len(name_bytes)))
        buf.extend(name_bytes)

        # Type (u8)
        buf.append(item.type)
        # Cost (u16 LE)
        buf.extend(struct.pack("<H", item.cost))
        # Flags (u8)
        buf.append(item.flags)
        # Effect (u16 LE)
        buf.extend(struct.pack("<H", item.effect))
        # Params (4 × u8)
        buf.append(item.params.strength)
        buf.append(item.params.epi)
        buf.append(item.params.ep)
        buf.append(item.params.special)
        # Help text pointer (u32 LE)
        ptr = int(item.help_text_pointer, 16)
        buf.extend(struct.pack("<I", ptr))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))
