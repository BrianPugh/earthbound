"""Cross-reference event flags against all data sources to aid renaming."""

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Annotated

from cyclopts import App, Parameter

from ebtools.config import CommonData, load_common_data

analyze_flags_app = App(
    name="analyze-flags",
    help="Cross-reference event flags against game data sources to suggest names for UNKNOWN flags.",
)

# ── Data structures ────────────────────────────────────────────────────────────


@dataclass
class FlagReference:
    source: str  # e.g. "npc_config", "teleport", "text:E01ONET0"
    context_type: str  # e.g. "npc_show", "npc_hide", "set", "clear", "test", "jump_if_set"
    details: dict = field(default_factory=dict)


@dataclass
class FlagInfo:
    id: int
    name: str
    is_unknown: bool
    references: list[FlagReference] = field(default_factory=list)


# ── Collectors ─────────────────────────────────────────────────────────────────


def collect_npc_config(assets_dir: Path, common_data: CommonData, flags: dict[int, FlagInfo]) -> None:
    """Collect event flag references from NPC configuration."""
    path = assets_dir / "data" / "npc_config.json"
    if not path.exists():
        return
    data = json.loads(path.read_text())
    npcs = data.get("npcs", data) if isinstance(data, dict) else data
    for npc in npcs:
        flag_id = npc.get("event_flag", 0)
        if flag_id == 0:
            continue
        if flag_id not in flags:
            continue
        sprite_id = npc.get("sprite_id", 0)
        sprite_name = common_data.sprites[sprite_id] if sprite_id < len(common_data.sprites) else f"SPRITE_{sprite_id}"
        show_cond = npc.get("show_condition", 0)
        context_type = "npc_hide" if show_cond == 1 else "npc_show"
        flags[flag_id].references.append(
            FlagReference(
                source="npc_config",
                context_type=context_type,
                details={
                    "npc_id": npc.get("id", "?"),
                    "type": npc.get("type", "?"),
                    "sprite_id": sprite_id,
                    "sprite_name": sprite_name,
                    "show_condition": show_cond,
                    "direction": npc.get("direction_name", ""),
                    "text_pointer": npc.get("text_pointer", ""),
                },
            )
        )


def collect_teleport_destinations(assets_dir: Path, flags: dict[int, FlagInfo]) -> None:
    """Collect event flag references from PSI Teleport destinations."""
    path = assets_dir / "data" / "psi_teleport_destinations.json"
    if not path.exists():
        return
    data = json.loads(path.read_text())
    destinations = data.get("destinations", data) if isinstance(data, dict) else data
    for dest in destinations:
        flag_id = dest.get("event_flag", 0)
        if flag_id == 0:
            continue
        if flag_id not in flags:
            continue
        flags[flag_id].references.append(
            FlagReference(
                source="teleport",
                context_type="teleport_unlock",
                details={
                    "destination_id": dest.get("id", "?"),
                    "name": dest.get("name", ""),
                    "x": dest.get("x", 0),
                    "y": dest.get("y", 0),
                },
            )
        )


def collect_timed_delivery(assets_dir: Path, common_data: CommonData, flags: dict[int, FlagInfo]) -> None:
    """Collect event flag references from timed delivery data."""
    path = assets_dir / "data" / "timed_delivery.json"
    if not path.exists():
        return
    data = json.loads(path.read_text())
    entries = data if isinstance(data, list) else data.get("deliveries", [])
    for i, entry in enumerate(entries):
        flag_id = entry.get("event_flag", 0)
        if flag_id == 0:
            continue
        if flag_id not in flags:
            continue
        sprite_id = entry.get("sprite_id", 0)
        sprite_name = common_data.sprites[sprite_id] if sprite_id < len(common_data.sprites) else f"SPRITE_{sprite_id}"
        flags[flag_id].references.append(
            FlagReference(
                source="timed_delivery",
                context_type="delivery",
                details={
                    "entry_index": i,
                    "sprite_id": sprite_id,
                    "sprite_name": sprite_name,
                    "delivery_time": entry.get("delivery_time", 0),
                },
            )
        )


def collect_telephone_contacts(assets_dir: Path, flags: dict[int, FlagInfo]) -> None:
    """Collect event flag references from telephone contacts table."""
    path = assets_dir / "data" / "telephone_contacts_table.json"
    if not path.exists():
        return
    data = json.loads(path.read_text())
    entries = data if isinstance(data, list) else data.get("contacts", [])
    for i, entry in enumerate(entries):
        flag_str = entry.get("event_flag", "0x0000")
        flag_id = int(flag_str, 16)
        if flag_id == 0:
            continue
        if flag_id not in flags:
            continue
        flags[flag_id].references.append(
            FlagReference(
                source="telephone",
                context_type="phone_contact",
                details={
                    "contact_index": i,
                    "label": entry.get("label", ""),
                },
            )
        )


# ── Text script binary scanner ─────────────────────────────────────────────────

# Argument byte counts for simple CC opcodes 0x00-0x17
_CC_SIMPLE_ARG_SIZES: dict[int, int] = {
    0x00: 0,
    0x01: 0,
    0x02: 0,
    0x03: 0,
    0x04: 2,
    0x05: 2,  # SET/CLEAR event flag
    0x06: 6,  # JUMP_IF_FLAG_SET: 2 (flag) + 4 (dest)
    0x07: 2,  # CHECK_EVENT_FLAG
    0x08: 4,
    0x0A: 4,
    # 0x09 is variable (JUMP_MULTI) — handled specially
    0x0B: 1,
    0x0C: 1,
    0x0D: 1,
    0x0E: 1,
    0x0F: 0,
    0x10: 1,
    0x11: 0,
    0x12: 0,
    0x13: 0,
    0x14: 0,
    0x15: 1,
    0x16: 1,
    0x17: 1,
}

# Subcode argument sizes for CC 0x18-0x1F
# Maps (major_opcode, sub_opcode) -> arg_bytes_after_subcode
# -1 means variable length (handled specially)
_CC_SUB_ARG_SIZES: dict[int, dict[int, int]] = {
    0x18: {
        0x00: 0,
        0x01: 1,
        0x02: 0,
        0x03: 1,
        0x04: 0,
        0x05: 2,
        0x06: 0,
        0x07: 5,
        0x08: 3,
        0x09: 1,
        0x0A: 0,
    },
    0x19: {
        0x02: -1,  # variable: null-terminated string
        0x04: 0,
        0x05: 3,
        0x10: 1,
        0x11: 1,
        0x14: 0,
        0x16: 2,
        0x18: 1,
        0x19: 2,
        0x1A: 1,
        0x1B: 1,
        0x1C: 2,
        0x1D: 2,
        0x1E: 0,
        0x1F: 0,
        0x20: 0,
        0x21: 1,
        0x22: 4,
        0x23: 5,
        0x24: 4,
        0x25: 1,
        0x26: 1,
        0x27: 1,
        0x28: 1,
    },
    0x1A: {
        0x01: 17,
        0x05: 2,
        0x06: 1,
        0x07: 0,
        0x0A: 0,
    },
    0x1B: {
        0x00: 0,
        0x01: 0,
        0x02: 4,
        0x03: 4,
        0x04: 0,
        0x05: 0,
        0x06: 0,
    },
    0x1C: {
        0x00: 1,
        0x01: 1,
        0x02: 1,
        0x03: 1,
        0x04: 0,
        0x05: 1,
        0x06: 1,
        0x07: 1,
        0x08: 1,
        0x09: 0,
        0x0A: 4,
        0x0B: 4,
        0x0C: 1,
        0x0D: 0,
        0x0E: 0,
        0x0F: 0,
        0x11: 1,
        0x12: 1,
        0x13: 2,
        0x14: 1,
        0x15: 1,
    },
    0x1D: {
        0x00: 2,
        0x01: 2,
        0x02: 1,
        0x03: 1,
        0x04: 2,
        0x05: 2,
        0x06: 4,
        0x07: 4,
        0x08: 2,
        0x09: 2,
        0x0A: 1,
        0x0B: 1,
        0x0C: 2,
        0x0D: 3,
        0x0E: 2,
        0x0F: 2,
        0x10: 2,
        0x11: 2,
        0x12: 2,
        0x13: 2,
        0x14: 4,
        0x15: 2,
        0x17: 4,
        0x18: 1,
        0x19: 1,
        0x20: 0,
        0x21: 1,
        0x22: 0,
        0x23: 1,
        0x24: 1,
    },
    0x1E: {
        0x00: 2,
        0x01: 2,
        0x02: 2,
        0x03: 2,
        0x04: 2,
        0x05: 2,
        0x06: 2,
        0x07: 2,
        0x08: 2,
        0x09: 4,
        0x0A: 3,
        0x0B: 3,
        0x0C: 3,
        0x0D: 3,
        0x0E: 3,
    },
    0x1F: {
        0x00: 2,
        0x01: 1,
        0x02: 1,
        0x03: 0,
        0x04: 1,
        0x05: 0,
        0x06: 0,
        0x07: 1,
        0x11: 1,
        0x12: 1,
        0x13: 2,
        0x14: 1,
        0x15: 5,
        0x16: 3,
        0x17: 5,
        0x1A: 3,
        0x1B: 2,
        0x1C: 2,
        0x1D: 1,
        0x1E: 3,
        0x1F: 3,
        0x20: 2,
        0x21: 1,
        0x23: 2,
        0x30: 0,
        0x31: 0,
        0x41: 1,
        0x50: 0,
        0x51: 0,
        0x52: 1,
        0x60: 0,
        0x61: 0,
        0x62: 1,
        0x63: 4,
        0x64: 0,
        0x65: 0,
        0x66: 6,
        0x67: 1,
        0x68: 0,
        0x69: 0,
        0x71: 2,
        0x83: 2,
        0x90: 0,
        0xA0: 0,
        0xA1: 0,
        0xA2: 0,
        0xB0: 0,
        0xC0: -1,  # variable: JUMP_MULTI2
        0xD0: 1,
        0xD1: 0,
        0xD2: 1,
        0xD3: 1,
        0xE1: 3,
        0xE4: 3,
        0xE5: 1,
        0xE6: 2,
        0xE7: 2,
        0xE8: 1,
        0xE9: 2,
        0xEA: 2,
        0xEB: 2,
        0xEC: 2,
        0xED: 0,
        0xEE: 2,
        0xEF: 2,
        0xF0: 0,
        0xF1: 4,
        0xF2: 4,
        0xF3: 3,
        0xF4: 2,
    },
}

# Flag-related CC opcodes and their context types
_FLAG_OPCODES = {
    0x04: "set",
    0x05: "clear",
    0x06: "jump_if_set",
    0x07: "test",
}


def _scan_text_script(data: list[int], script_name: str, flags: dict[int, FlagInfo]) -> None:
    """Scan a text script binary for CC opcodes that reference event flags."""
    pos = 0
    n = len(data)

    while pos < n:
        byte = data[pos]
        pos += 1

        # Regular text characters (0x20-0xFF range covers printable + extended)
        if byte >= 0x20:
            continue

        # Flag-related opcodes
        if byte in _FLAG_OPCODES:
            if pos + 1 >= n:
                break
            flag_id = data[pos] | (data[pos + 1] << 8)
            offset = pos - 1
            pos += _CC_SIMPLE_ARG_SIZES[byte]
            if flag_id in flags:
                context_type = _FLAG_OPCODES[byte]
                flags[flag_id].references.append(
                    FlagReference(
                        source=f"text:{script_name}",
                        context_type=context_type,
                        details={"offset": offset},
                    )
                )
            continue

        # JUMP_MULTI (variable length)
        if byte == 0x09:
            if pos >= n:
                break
            count = data[pos]
            pos += 1 + count * 4
            continue

        # Simple opcodes with fixed arg sizes
        if byte in _CC_SIMPLE_ARG_SIZES:
            pos += _CC_SIMPLE_ARG_SIZES[byte]
            continue

        # Subcoded opcodes 0x18-0x1F
        if 0x18 <= byte <= 0x1F:
            if pos >= n:
                break
            sub = data[pos]
            pos += 1
            sub_table = _CC_SUB_ARG_SIZES.get(byte, {})
            arg_size = sub_table.get(sub)

            if arg_size is None:
                # Unknown subcode — can't reliably continue parsing this script
                break
            elif arg_size == -1:
                # Variable length
                if byte == 0x19 and sub == 0x02:
                    # LOAD_STRING_TO_MEMORY: read until terminator (0x00 or 0x01/0x02)
                    while pos < n:
                        x = data[pos]
                        pos += 1
                        if x == 0:
                            break
                        if x == 1:
                            pos += 4  # LE32 destination
                            break
                        if x == 2:
                            break
                elif (byte == 0x09) or (byte == 0x1F and sub == 0xC0):
                    # JUMP_MULTI / JUMP_MULTI2
                    if pos >= n:
                        break
                    count = data[pos]
                    pos += 1 + count * 4
            else:
                pos += arg_size
            continue

        # Bytes in 0x01-0x03 range that aren't CC opcodes we recognize
        # (these are text control chars like LINE_BREAK etc.)
        # Already handled above in the _CC_SIMPLE_ARG_SIZES table


def collect_text_scripts(assets_dir: Path, flags: dict[int, FlagInfo]) -> None:
    """Scan all text script binaries for event flag references."""
    events_dir = assets_dir / "data" / "events"
    if not events_dir.exists():
        return
    for path in sorted(events_dir.glob("*_bin.json")):
        script_name = path.stem.replace("_bin", "")
        data = json.loads(path.read_text())
        if not isinstance(data, list):
            continue
        _scan_text_script(data, script_name, flags)


def collect_event_scripts_asm(repo_root: Path, flags: dict[int, FlagInfo]) -> None:
    """Grep ASM event scripts for flag macro references."""
    events_dir = repo_root / "asm" / "data" / "events"
    if not events_dir.exists():
        return

    flag_pattern = re.compile(r"EVENT_(?:TEST_EVENT_FLAG|MOVEMENT_CMD_SET_EVENT_FLAG)\s+EVENT_FLAG::(\w+)")

    for asm_path in sorted(events_dir.rglob("*.asm")):
        lines = asm_path.read_text().splitlines()
        rel_path = asm_path.relative_to(repo_root)
        for line_no, line in enumerate(lines, 1):
            m = flag_pattern.search(line)
            if not m:
                continue
            flag_name = m.group(1)
            # Find the flag ID by name
            flag_id = None
            for fid, finfo in flags.items():
                if finfo.name == flag_name:
                    flag_id = fid
                    break
            if flag_id is None:
                continue
            # Gather context (up to 3 lines before/after)
            start = max(0, line_no - 4)
            end = min(len(lines), line_no + 3)
            context_lines = lines[start:end]
            macro_name = "TEST_EVENT_FLAG" if "TEST_EVENT_FLAG" in line else "SET_EVENT_FLAG"
            flags[flag_id].references.append(
                FlagReference(
                    source=f"event_asm:{rel_path}",
                    context_type="asm_test" if "TEST" in macro_name else "asm_set",
                    details={
                        "file": str(rel_path),
                        "line": line_no,
                        "context": context_lines,
                    },
                )
            )


_TOWN_MAP_NAMES = ["Onett", "Twoson", "Threed", "Fourside", "Scaraba", "Summers"]


def collect_town_map_icons(repo_root: Path, flags: dict[int, FlagInfo]) -> None:
    """Collect event flag references from town map icon placement data."""
    path = repo_root / "asm" / "data" / "map" / "town_map_icon_placement_data.asm"
    if not path.exists():
        return
    text = path.read_text()

    # Parse ENTRY blocks: TOWN_MAP_ICON_PLACEMENT_ENTRY_N
    entry_pattern = re.compile(r"TOWN_MAP_ICON_PLACEMENT_ENTRY_(\d+):")
    flag_pattern = re.compile(
        r"\.BYTE\s+\$([0-9A-Fa-f]{2})\s*\n\s*"
        r"\.BYTE\s+\$([0-9A-Fa-f]{2})\s*\n\s*"
        r"\.BYTE\s+\$([0-9A-Fa-f]{2})\s*\n\s*"
        r"\.WORD\s+(?:EVENT_FLAG_UNSET\s*\|\s*)?EVENT_FLAG::(\w+)",
        re.MULTILINE,
    )

    # Split by entry labels
    entries = list(entry_pattern.finditer(text))
    for idx, entry_match in enumerate(entries):
        entry_num = int(entry_match.group(1))
        town_name = _TOWN_MAP_NAMES[entry_num] if entry_num < len(_TOWN_MAP_NAMES) else f"Town{entry_num}"
        start = entry_match.end()
        end = entries[idx + 1].start() if idx + 1 < len(entries) else len(text)
        chunk = text[start:end]

        icon_idx = 0
        for m in flag_pattern.finditer(chunk):
            x_pos = int(m.group(1), 16)
            y_pos = int(m.group(2), 16)
            icon_type = int(m.group(3), 16)
            flag_name = m.group(4)

            # Find flag ID by name
            flag_id = None
            for fid, finfo in flags.items():
                if finfo.name == flag_name:
                    flag_id = fid
                    break
            if flag_id is None:
                icon_idx += 1
                continue

            is_hint = "EVENT_FLAG_UNSET" in m.group(0)
            flags[flag_id].references.append(
                FlagReference(
                    source="town_map_icons",
                    context_type="hint_icon" if is_hint else "map_icon",
                    details={
                        "town": town_name,
                        "entry_index": entry_num,
                        "icon_index": icon_idx,
                        "icon_type": icon_type,
                        "x": x_pos,
                        "y": y_pos,
                    },
                )
            )
            icon_idx += 1


def collect_c_constants(repo_root: Path, flags: dict[int, FlagInfo]) -> None:
    """Collect EVENT_FLAG_* #defines from C port constants.h."""
    path = repo_root / "src" / "include" / "constants.h"
    if not path.exists():
        return
    pattern = re.compile(r"#define\s+EVENT_FLAG_(\w+)\s+(\d+)")
    for line in path.read_text().splitlines():
        m = pattern.search(line)
        if not m:
            continue
        c_name = m.group(1)
        flag_id = int(m.group(2))
        if c_name == "COUNT":
            continue
        if flag_id not in flags:
            continue
        flags[flag_id].references.append(
            FlagReference(
                source="c_constant",
                context_type="c_define",
                details={"c_name": f"EVENT_FLAG_{c_name}", "line": line.strip()},
            )
        )


# ── Report generation ──────────────────────────────────────────────────────────


def _confidence(flag_info: FlagInfo) -> str:
    """Determine confidence level for a flag based on its references."""
    sources = {r.source.split(":")[0] for r in flag_info.references}
    if (
        "npc_config" in sources
        or "teleport" in sources
        or "timed_delivery" in sources
        or "telephone" in sources
        or "town_map_icons" in sources
    ):
        return "HIGH"
    if any(s.startswith("text") for s in sources):
        return "MEDIUM"
    if any(s.startswith("event_asm") for s in sources):
        return "LOW"
    if "c_constant" in sources:
        return "LOW"
    return "NONE"


def _format_reference_md(ref: FlagReference) -> str:
    """Format a single reference as markdown."""
    d = ref.details
    if ref.source == "npc_config":
        cond = "hides when set" if ref.context_type == "npc_hide" else "shows when set"
        return f"  - **NPC Config**: NPC #{d['npc_id']} (sprite: {d['sprite_name']}, type: {d['type']}, {cond})"
    if ref.source == "teleport":
        return f"  - **Teleport**: {d['name']} (destination #{d['destination_id']})"
    if ref.source == "timed_delivery":
        return f"  - **Timed Delivery**: entry #{d['entry_index']} (sprite: {d['sprite_name']}, time: {d['delivery_time']})"
    if ref.source == "telephone":
        return f"  - **Telephone**: {d['label']} (contact #{d['contact_index']})"
    if ref.source == "town_map_icons":
        return f"  - **Town Map Icon**: {d['town']} — icon #{d['icon_index']} (type {d['icon_type']}, pos {d['x']},{d['y']}, {ref.context_type})"
    if ref.source.startswith("text:"):
        script = ref.source.split(":", 1)[1]
        return f"  - **Text Script**: {script} — {ref.context_type.upper()} at offset 0x{d['offset']:04X}"
    if ref.source.startswith("event_asm:"):
        return f"  - **Event ASM**: {d['file']}:{d['line']} — {ref.context_type}"
    if ref.source == "c_constant":
        return f"  - **C Constant**: `{d['c_name']}` ({d['line']})"
    return f"  - **{ref.source}**: {ref.context_type} — {d}"


def generate_markdown_report(flags: dict[int, FlagInfo], include_named: bool) -> str:
    """Generate a markdown cross-reference report."""
    lines: list[str] = []
    lines.append("# Event Flag Cross-Reference Report\n")

    # Summary stats
    total = len(flags)
    named = sum(1 for f in flags.values() if not f.is_unknown)
    unknown_with_refs = sum(1 for f in flags.values() if f.is_unknown and f.references)
    unreferenced = sum(1 for f in flags.values() if f.is_unknown and not f.references)
    lines.append("## Summary\n")
    lines.append(
        f"- Total: {total} | Named: {named} | Unknown with refs: {unknown_with_refs} | Unreferenced unknown: {unreferenced}\n"
    )

    # Group flags by confidence
    buckets: dict[str, list[FlagInfo]] = {"HIGH": [], "MEDIUM": [], "LOW": [], "NONE": []}
    for finfo in flags.values():
        if not include_named and not finfo.is_unknown:
            continue
        conf = _confidence(finfo)
        buckets[conf].append(finfo)

    for conf_level, label in [
        ("HIGH", "HIGH Confidence (NPC/Teleport/Delivery/Telephone/Town Map cross-refs)"),
        ("MEDIUM", "MEDIUM Confidence (Text script location context)"),
        ("LOW", "LOW Confidence (ASM/C-constant-only references)"),
    ]:
        bucket = buckets[conf_level]
        if not bucket:
            continue
        lines.append(f"\n## {label}\n")
        for finfo in sorted(bucket, key=lambda f: f.id):
            lines.append(f"### {finfo.name} (Flag {finfo.id} / 0x{finfo.id:03X})\n")
            for ref in finfo.references:
                lines.append(_format_reference_md(ref))
            lines.append("")

    # Unreferenced
    unreferenced_flags = sorted(buckets["NONE"], key=lambda f: f.id)
    if unreferenced_flags:
        lines.append("\n## Unreferenced (no references in any data source)\n")
        # Compress into ranges
        ranges: list[tuple[int, int]] = []
        for finfo in unreferenced_flags:
            if ranges and finfo.id == ranges[-1][1] + 1:
                ranges[-1] = (ranges[-1][0], finfo.id)
            else:
                ranges.append((finfo.id, finfo.id))
        for start, end in ranges:
            if start == end:
                flag_name = flags[start].name
                lines.append(f"- {flag_name} (Flag {start})")
            else:
                start_name = flags[start].name
                end_name = flags[end].name
                lines.append(f"- {start_name}–{end_name} (Flags {start}–{end}, {end - start + 1} flags)")
        lines.append("")

    return "\n".join(lines)


def generate_json_report(flags: dict[int, FlagInfo], include_named: bool) -> str:
    """Generate a JSON cross-reference report."""
    result = []
    for finfo in sorted(flags.values(), key=lambda f: f.id):
        if not include_named and not finfo.is_unknown:
            continue
        if not finfo.references and not include_named:
            # Still include unreferenced unknowns
            pass
        entry = {
            "id": finfo.id,
            "name": finfo.name,
            "is_unknown": finfo.is_unknown,
            "confidence": _confidence(finfo),
            "references": [
                {
                    "source": r.source,
                    "context_type": r.context_type,
                    "details": {k: v for k, v in r.details.items() if k != "context"},
                }
                for r in finfo.references
            ],
        }
        result.append(entry)
    return json.dumps(result, indent=2)


# ── CLI command ────────────────────────────────────────────────────────────────


@analyze_flags_app.default
def analyze_flags(
    *,
    commondefs: Annotated[Path, Parameter(alias="-c")] = Path("commondefs.yml"),
    assets_dir: Annotated[Path, Parameter(alias="-a")] = Path("src/assets"),
    output: Annotated[Path | None, Parameter(alias="-o")] = None,
    json_output: Annotated[Path | None, Parameter(alias="-j", name="--json")] = None,
    flag_id: Annotated[int | None, Parameter(name="--flag-id")] = None,
    include_named: Annotated[bool, Parameter(name="--include-named")] = False,
) -> None:
    """Cross-reference event flags against all game data sources.

    Scans NPC configs, teleport destinations, timed deliveries, telephone contacts,
    town map icon placement, text script binaries, event script ASM, and C port
    constants to find all references to each event flag. Outputs a report with
    confidence levels.
    """
    # Load flag definitions
    common_data = load_common_data(commondefs)

    # Build flag info registry
    flags: dict[int, FlagInfo] = {}
    for i, name in enumerate(common_data.eventFlags):
        is_unknown = name.startswith("UNKNOWN_")
        if flag_id is not None and i != flag_id:
            continue
        flags[i] = FlagInfo(id=i, name=name, is_unknown=is_unknown)

    # Detect repo root (parent of src/assets)
    repo_root = assets_dir.parent.parent
    if not (repo_root / "asm").exists():
        # Try CWD
        repo_root = Path()

    # Run all collectors
    print("Scanning NPC config...")
    collect_npc_config(assets_dir, common_data, flags)

    print("Scanning teleport destinations...")
    collect_teleport_destinations(assets_dir, flags)

    print("Scanning timed deliveries...")
    collect_timed_delivery(assets_dir, common_data, flags)

    print("Scanning telephone contacts...")
    collect_telephone_contacts(assets_dir, flags)

    print("Scanning text script binaries...")
    collect_text_scripts(assets_dir, flags)

    print("Scanning town map icon placement...")
    collect_town_map_icons(repo_root, flags)

    print("Scanning event script ASM...")
    collect_event_scripts_asm(repo_root, flags)

    print("Scanning C port constants...")
    collect_c_constants(repo_root, flags)

    # Generate reports
    report_include_named = include_named or flag_id is not None

    md_report = generate_markdown_report(flags, report_include_named)

    if output:
        output.write_text(md_report)
        print(f"\nMarkdown report written to {output}")
    else:
        print("\n" + md_report)

    if json_output:
        jr = generate_json_report(flags, report_include_named)
        json_output.write_text(jr)
        print(f"JSON report written to {json_output}")

    # Print summary
    total_refs = sum(len(f.references) for f in flags.values())
    flags_with_refs = sum(1 for f in flags.values() if f.references)
    unknown_with_refs = sum(1 for f in flags.values() if f.is_unknown and f.references)
    print(
        f"\nFound {total_refs} references across {flags_with_refs} flags ({unknown_with_refs} unknown flags with references)"
    )
