"""Human-readable annotations for state dump field values.

Provides enum decode tables, EB text decoding, and a field annotation
registry so that analyze-dump output shows meaningful names instead of
raw numbers.

Enum tables are loaded at init time from:
  1. struct_info.json "enums" section (C-defined enums, authoritative)
  2. Assembly .ENUM blocks in include/constants/ (items, music)
"""

import struct
from pathlib import Path

# ---------------------------------------------------------------------------
# EB text decoder (stable algorithm, not data — kept here)
# ---------------------------------------------------------------------------

# 96-entry table: EB codes 0x50-0xAF -> ASCII (from text.c:2804)
_EB_TO_ASCII = (
    " !&{$%}'"  # 0x50-0x57
    "()*+,-./"  # 0x58-0x5F
    "0123456789:;<=>?"  # 0x60-0x6F
    "@ABCDEFGHIJKLMNO"  # 0x70-0x7F
    "PQRSTUVWXYZ~^[]#"  # 0x80-0x8F
    "_abcdefghijklmno"  # 0x90-0x9F
    "pqrstuvwxyz?|???"  # 0xA0-0xAF
)


def decode_eb_text(data: bytes) -> str:
    """Decode EB-encoded bytes to a quoted ASCII string. Stops at 0x00."""
    chars = []
    for b in data:
        if b == 0x00:
            break
        if 0x50 <= b <= 0xAF:
            chars.append(_EB_TO_ASCII[b - 0x50])
        else:
            chars.append(f"\\x{b:02X}")
    return '"' + "".join(chars) + '"'


# ---------------------------------------------------------------------------
# ASM enum parser
# ---------------------------------------------------------------------------


def _find_repo_root() -> Path | None:
    """Walk up from this file looking for the repo root."""
    p = Path(__file__).resolve().parent
    for _ in range(10):
        if (p / ".git").exists() or (p / "pyproject.toml").exists():
            return p
        p = p.parent
    return None


def _parse_asm_enum(path: Path, enum_name: str) -> dict[int, str]:
    """Parse a ca65 .ENUM block into {value: display_name}.

    Handles decimal and $hex values.  Converts UPPER_SNAKE to Title Case
    display names (e.g. TEDDY_BEAR -> "Teddy Bear").
    """
    result: dict[int, str] = {}
    inside = False
    for line in path.read_text().splitlines():
        stripped = line.strip()
        if stripped == f".ENUM {enum_name}":
            inside = True
            continue
        if stripped == ".ENDENUM":
            if inside:
                break
            continue
        if not inside:
            continue
        if "=" not in stripped:
            continue
        name_part, val_part = stripped.split("=", 1)
        name_part = name_part.strip()
        val_part = val_part.strip()
        value = int(val_part[1:], 16) if val_part.startswith("$") else int(val_part)
        # Convert UPPER_SNAKE_CASE to Title Case
        display = " ".join(w.capitalize() for w in name_part.split("_"))
        result[value] = display
    return result


# ---------------------------------------------------------------------------
# Enum table registry (populated by init_annotations)
# ---------------------------------------------------------------------------

_enum_tables: dict[str, dict[int, str]] = {}
_initialized = False


def init_annotations(struct_info: dict | None) -> None:
    """Build enum tables from struct_info JSON and ASM files.

    Must be called once before annotate_field().
    """
    global _enum_tables, _initialized
    if _initialized:
        return
    _initialized = True

    # 1. Load C-defined enums from struct_info.json
    if struct_info is not None:
        enums = struct_info.get("enums", {})
        for enum_name, entries in enums.items():
            _enum_tables[enum_name] = {int(k): v for k, v in entries.items()}

    # 2. Parse item and music enums from assembly (supplementary)
    repo_root = _find_repo_root()
    if repo_root is not None:
        items_path = repo_root / "include" / "constants" / "items.asm"
        if items_path.exists() and "item_id" not in _enum_tables:
            _enum_tables["item_id"] = _parse_asm_enum(items_path, "ITEM")

        music_path = repo_root / "include" / "constants" / "music.asm"
        if music_path.exists() and "music_track" not in _enum_tables:
            _enum_tables["music_track"] = _parse_asm_enum(music_path, "MUSIC")


# ---------------------------------------------------------------------------
# Format helpers (duplicated from state_dump.py to avoid circular import)
# ---------------------------------------------------------------------------

_FMT_MAP = {
    "u8": ("<B", 1),
    "i8": ("<b", 1),
    "u16": ("<H", 2),
    "i16": ("<h", 2),
    "u32": ("<I", 4),
    "i32": ("<i", 4),
    "bool": ("<B", 1),
}


def _parse_type(type_str: str) -> tuple[str, int | None]:
    if "[" in type_str:
        base, rest = type_str.split("[", 1)
        count = int(rest.rstrip("]"))
        return base, count
    return type_str, None


def _decode_scalar(value: int, table: dict[int, str]) -> str | None:
    name = table.get(value)
    if name is not None:
        return f"{value} ({name})"
    return None


# ---------------------------------------------------------------------------
# Affliction decoder
# ---------------------------------------------------------------------------


def _decode_afflictions(data: bytes, offset: int, size: int) -> str:
    """Decode 7-byte affliction array into named groups."""
    group_table = _enum_tables.get("status_group", {})
    parts = []
    for i in range(min(7, size)):
        if offset + i >= len(data):
            break
        val = data[offset + i]
        group = group_table.get(i, f"Group{i}")
        status_table = _enum_tables.get(f"status_{i}", {})
        status = status_table.get(val, f"?{val}")
        parts.append(f"{group}:{status}")
    return "[" + ", ".join(parts) + "]"


# ---------------------------------------------------------------------------
# Main decoder dispatch
# ---------------------------------------------------------------------------


def annotate_field(
    decoder_key: str,
    data: bytes,
    field_info: dict,
) -> str | None:
    """Produce an annotated string for a field. Returns None to fall through.

    decoder_key comes from field_info["decoder"] (set by AFIELD in gen_struct_info.c).
    """
    offset = field_info["offset"]
    size = field_info["size"]
    type_str = field_info["type"]
    base_type, count = _parse_type(type_str)

    # EB text
    if decoder_key == "eb_text":
        if offset + size <= len(data):
            return decode_eb_text(data[offset : offset + size])
        return None

    # Afflictions
    if decoder_key == "afflictions":
        return _decode_afflictions(data, offset, size)

    # Array with annotation
    if decoder_key.startswith("array:"):
        table_key = decoder_key[6:]
        table = _enum_tables.get(table_key)
        if table is None:
            return None

        fmt_info = _FMT_MAP.get(base_type)
        if fmt_info is None or count is None:
            return None
        fmt, elem_size = fmt_info

        values = []
        for i in range(min(count, size // elem_size)):
            if offset + i * elem_size + elem_size > len(data):
                break
            (val,) = struct.unpack_from(fmt, data, offset + i * elem_size)
            values.append(val)

        if len(values) <= 16:
            parts = []
            for v in values:
                lookup_v = v & 0xFF if elem_size == 1 else v
                name = table.get(lookup_v)
                if name is not None:
                    if elem_size == 1:
                        parts.append(f"0x{v & 0xFF:02X}({name})")
                    else:
                        parts.append(f"{v}({name})")
                else:
                    if elem_size == 1:
                        parts.append(f"0x{v & 0xFF:02X}")
                    else:
                        parts.append(str(v))
            return "[" + ", ".join(parts) + "]"
        else:
            # Sparse format for large arrays
            parts = []
            default = 0
            for i, v in enumerate(values):
                if v != default:
                    name = table.get(v)
                    if name is not None:
                        parts.append(f"{i}:{v}({name})")
                    else:
                        parts.append(f"{i}:{v}")
            if not parts:
                return f"[{len(values)} elements, all zero]"
            return "{" + ", ".join(parts) + "}"

    # Scalar enum
    table = _enum_tables.get(decoder_key)
    if table is None:
        return None

    fmt_info = _FMT_MAP.get(base_type)
    if fmt_info is None:
        return None
    fmt, elem_size = fmt_info

    if offset + elem_size > len(data):
        return None

    (val,) = struct.unpack_from(fmt, data, offset)
    return _decode_scalar(val, table)
