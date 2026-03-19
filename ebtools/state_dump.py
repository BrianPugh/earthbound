"""Parse, inspect, and diff EarthBound C port state dump files.

State dumps are produced by the F4 debug hotkey in the C port. Each dump
is a binary file with tagged sections containing raw struct data.

Binary format:
  - Header: "EBSD" magic (4 bytes) + version u16 + frame_counter u16
  - Sections: u16 id + u32 size + <size> bytes of data
  - Terminator: u16 0xFFFF
"""

import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

from ebtools.state_dump_annotations import annotate_field, init_annotations

MAGIC = 0x44534245  # "EBSD" little-endian
SECTION_TERMINATOR = 0xFFFF

# Section ID -> name mapping (must match state_dump.c)
SECTION_NAMES: dict[int, str] = {
    0x0001: "CORE",
    0x0002: "GAME_STATE",
    0x0003: "PARTY_CHARACTERS",
    0x0004: "EVENT_FLAGS",
    0x0005: "OVERWORLD",
    0x0006: "BATTLE",
    0x0007: "DISPLAY_TEXT",
    0x0008: "WINDOW",
    0x0009: "MAP_LOADER",
    0x000A: "PPU",
    0x000B: "POSITION_BUFFER",
    0x000C: "DOOR",
    0x000D: "ENTITY_RUNTIME",
    0x000E: "ENTITY_SYSTEM",
    0x000F: "SCRIPTS",
    0x0010: "SPRITE_PRIORITY",
    0x0011: "FADE",
    0x0012: "RNG",
    0x0013: "AUDIO",
    0x0014: "PSI_ANIMATION",
}


@dataclass
class Section:
    id: int
    name: str
    data: bytes


@dataclass
class DumpFile:
    version: int
    frame: int
    sections: list[Section] = field(default_factory=list)
    total_size: int = 0


def parse_dump(path: Path) -> DumpFile:
    """Read a state dump file and return structured data."""
    raw = path.read_bytes()
    if len(raw) < 8:
        raise ValueError(f"File too small to be a state dump: {len(raw)} bytes")

    magic, version, frame = struct.unpack_from("<IHH", raw, 0)
    if magic != MAGIC:
        raise ValueError(f"Bad magic: expected 0x{MAGIC:08X} ('EBSD'), got 0x{magic:08X}")

    dump = DumpFile(version=version, frame=frame, total_size=len(raw))
    offset = 8

    while offset + 2 <= len(raw):
        (section_id,) = struct.unpack_from("<H", raw, offset)
        offset += 2

        if section_id == SECTION_TERMINATOR:
            break

        if offset + 4 > len(raw):
            raise ValueError(f"Truncated section header at offset {offset - 2}")

        (size,) = struct.unpack_from("<I", raw, offset)
        offset += 4

        if offset + size > len(raw):
            raise ValueError(f"Section 0x{section_id:04X} claims {size} bytes but only {len(raw) - offset} remain")

        name = SECTION_NAMES.get(section_id, f"UNKNOWN_0x{section_id:04X}")
        dump.sections.append(Section(id=section_id, name=name, data=raw[offset : offset + size]))
        offset += size

    return dump


def load_struct_info(path: Path | None) -> dict | None:
    """Load struct_info.json, trying auto-discovery if path is None."""
    candidates = []
    if path is not None:
        candidates.append(path)
    else:
        candidates.extend(
            [
                Path("src/build/struct_info.json"),
                Path("build/struct_info.json"),
            ]
        )

    for p in candidates:
        if p.is_file():
            with p.open() as f:
                return json.load(f)

    return None


def _parse_type(type_str: str) -> tuple[str, int | None]:
    """Parse a type string like 'u16[60]' into ('u16', 60) or ('u16', None)."""
    if "[" in type_str:
        base, rest = type_str.split("[", 1)
        count = int(rest.rstrip("]"))
        return base, count
    return type_str, None


_FMT_MAP = {
    "u8": ("<B", 1),
    "i8": ("<b", 1),
    "u16": ("<H", 2),
    "i16": ("<h", 2),
    "u32": ("<I", 4),
    "i32": ("<i", 4),
    "bool": ("<B", 1),
}


def format_field_value(data: bytes, field_info: dict) -> str:
    """Decode bytes to a human-readable string for a single field."""
    decoder = field_info.get("decoder")
    if decoder:
        annotated = annotate_field(decoder, data, field_info)
        if annotated is not None:
            return annotated

    offset = field_info["offset"]
    size = field_info["size"]
    type_str = field_info["type"]

    base_type, count = _parse_type(type_str)

    if base_type == "blob":
        return f"<{size} bytes>"

    fmt_info = _FMT_MAP.get(base_type)
    if fmt_info is None:
        return f"<unknown type {type_str}>"

    fmt, elem_size = fmt_info

    if count is not None:
        # Array type
        values = []
        for i in range(min(count, size // elem_size)):
            if offset + i * elem_size + elem_size > len(data):
                break
            (val,) = struct.unpack_from(fmt, data, offset + i * elem_size)
            values.append(val)

        if len(values) <= 16:
            if base_type in ("u8", "i8"):
                return "[" + ", ".join(f"0x{v & 0xFF:02X}" for v in values) + "]"
            return "[" + ", ".join(str(v) for v in values) + "]"
        # Summarize large arrays
        non_zero = sum(1 for v in values if v != 0)
        return f"[{len(values)} elements, {non_zero} non-zero]"

    # Scalar
    if offset + elem_size > len(data):
        return "<truncated>"

    (val,) = struct.unpack_from(fmt, data, offset)

    if base_type == "bool":
        return "true" if val else "false"
    if base_type in ("u16", "u32") and val > 255:
        return f"0x{val:X} ({val})"
    if base_type in ("i16", "i32") and (val > 255 or val < -255):
        return f"0x{val & ((1 << (elem_size * 8)) - 1):X} ({val})"
    return str(val)


def _find_section_info(struct_info: dict, section_name: str) -> dict | None:
    """Find section metadata by name."""
    sections = struct_info.get("sections", {})
    return sections.get(section_name)


def print_info(dump: DumpFile, struct_info: dict | None) -> None:
    """Print a summary of a dump file with decoded field values."""
    init_annotations(struct_info)
    print(f"EBSD v{dump.version}  frame={dump.frame}  ({dump.total_size:,} bytes)")
    print()

    for section in dump.sections:
        print(f"{section.name} ({len(section.data):,} bytes):")

        if struct_info is None:
            print("  (no struct_info.json — field decoding unavailable)")
            print()
            continue

        sec_info = _find_section_info(struct_info, section.name)
        if sec_info is None:
            print("  (section not in struct_info.json)")
            print()
            continue

        element_count = sec_info.get("element_count")
        element_size = sec_info.get("element_size")
        element_labels = sec_info.get("element_labels", {})
        fields = sec_info.get("fields", [])

        if element_count and element_size:
            # Array section (e.g., CharStruct[6])
            for idx in range(element_count):
                elem_offset = idx * element_size
                label = element_labels.get(str(idx))
                if label:
                    print(f"  [{idx}] ({label}):")
                else:
                    print(f"  [{idx}]:")
                for f in fields:
                    shifted = {**f, "offset": f["offset"] + elem_offset}
                    val = format_field_value(section.data, shifted)
                    print(f"    {f['name']:40s} = {val}")
        else:
            for f in fields:
                val = format_field_value(section.data, f)
                print(f"  {f['name']:42s} = {val}")

        print()


def print_diff(
    dump_a: DumpFile,
    dump_b: DumpFile,
    struct_info: dict | None,
    max_diffs: int = 32,
) -> None:
    """Print field-level differences between two dumps."""
    init_annotations(struct_info)
    print(f"Comparing frame {dump_a.frame} vs frame {dump_b.frame}")
    print()

    sections_a = {s.name: s for s in dump_a.sections}
    sections_b = {s.name: s for s in dump_b.sections}
    all_names = list(dict.fromkeys(s.name for s in dump_a.sections + dump_b.sections))

    changed_count = 0
    identical_count = 0
    total_field_diffs = 0

    for name in all_names:
        sec_a = sections_a.get(name)
        sec_b = sections_b.get(name)

        if sec_a is None:
            print(f"{name}: only in second dump")
            changed_count += 1
            print()
            continue
        if sec_b is None:
            print(f"{name}: only in first dump")
            changed_count += 1
            print()
            continue

        if sec_a.data == sec_b.data:
            identical_count += 1
            continue

        changed_count += 1
        sec_info = _find_section_info(struct_info, name) if struct_info else None

        if sec_info is None:
            byte_diffs = sum(1 for a, b in zip(sec_a.data, sec_b.data) if a != b)
            print(f"{name}: CHANGED ({byte_diffs} bytes differ)")
            print()
            continue

        print(f"{name}: CHANGED")
        fields = sec_info.get("fields", [])
        element_count = sec_info.get("element_count")
        element_size = sec_info.get("element_size")
        element_labels = sec_info.get("element_labels", {})
        section_diffs = 0

        if element_count and element_size:
            for idx in range(element_count):
                elem_offset = idx * element_size
                label = element_labels.get(str(idx))
                idx_str = f"[{idx}] ({label})" if label else f"[{idx}]"
                for f in fields:
                    shifted = {**f, "offset": f["offset"] + elem_offset}
                    off = shifted["offset"]
                    sz = shifted["size"]
                    chunk_a = sec_a.data[off : off + sz]
                    chunk_b = sec_b.data[off : off + sz]
                    if chunk_a != chunk_b:
                        val_a = format_field_value(sec_a.data, shifted)
                        val_b = format_field_value(sec_b.data, shifted)
                        print(f"  {idx_str}.{f['name']}: {val_a} -> {val_b}")
                        section_diffs += 1
                        total_field_diffs += 1
                        if total_field_diffs >= max_diffs:
                            print(f"  ... (truncated at {max_diffs} diffs)")
                            print()
                            break
                if total_field_diffs >= max_diffs:
                    break
        else:
            for f in fields:
                off = f["offset"]
                sz = f["size"]
                chunk_a = sec_a.data[off : off + sz]
                chunk_b = sec_b.data[off : off + sz]
                if chunk_a != chunk_b:
                    val_a = format_field_value(sec_a.data, f)
                    val_b = format_field_value(sec_b.data, f)
                    print(f"  {f['name']}: {val_a} -> {val_b}")
                    section_diffs += 1
                    total_field_diffs += 1
                    if total_field_diffs >= max_diffs:
                        print(f"  ... (truncated at {max_diffs} diffs)")
                        break

        if section_diffs == 0:
            # Changes in padding or unmapped bytes
            byte_diffs = sum(1 for a, b in zip(sec_a.data, sec_b.data) if a != b)
            print(f"  ({byte_diffs} bytes differ in unmapped regions)")

        print()

        if total_field_diffs >= max_diffs:
            print(f"(stopped after {max_diffs} field diffs)")
            break

    print(f"Summary: {changed_count} sections changed, {identical_count} sections identical")


def print_hexdump(section: Section) -> None:
    """Print a standard 16-byte-wide hex dump of a section."""
    print(f"Section {section.name} ({len(section.data):,} bytes):")
    data = section.data
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"{i:08X}  {hex_part:<48s} |{ascii_part}|")
