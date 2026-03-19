"""Music track utilities: export/pack dataset table JSON, generate C header.

The music_tracks.json combines:
  - Track names from include/constants/music.asm
  - Pack assignments from music/dataset_table.bin (3 bytes/track:
    primary_sample_pack, secondary_sample_pack, sequence_pack)
"""

import json
import re
from pathlib import Path

DATASET_ENTRY_SIZE = 3


def export_music_json(
    asm_path: Path,
    dataset_bin_path: Path,
    output_path: Path,
) -> int:
    """Export music_tracks.json with track names and pack assignments.

    Returns the number of tracks exported.
    """
    # Parse track names from assembly enum
    track_names: dict[int, tuple[str, str]] = {}
    with asm_path.open() as f:
        in_enum = False
        for line in f:
            stripped = line.strip()
            if stripped == ".ENUM MUSIC":
                in_enum = True
                continue
            if stripped == ".ENDENUM":
                break
            if not in_enum:
                continue
            m = re.match(r"(\w+)\s*=\s*(\d+)", stripped)
            if not m:
                continue
            symbol = m.group(1)
            value = int(m.group(2))
            track_names[value] = (f"MUSIC_{symbol}", _snake_to_display(symbol))

    # Read dataset table
    dataset = dataset_bin_path.read_bytes()
    num_tracks = len(dataset) // DATASET_ENTRY_SIZE

    tracks: dict[str, dict] = {}
    for i in range(num_tracks):
        off = i * DATASET_ENTRY_SIZE
        primary = dataset[off]
        secondary = dataset[off + 1]
        sequence = dataset[off + 2]

        name, display = track_names.get(i, (f"MUSIC_UNKNOWN_{i}", f"Unknown {i}"))
        tracks[str(i)] = {
            "name": name,
            "display": display,
            "primary_sample_pack": primary,
            "secondary_sample_pack": secondary,
            "sequence_pack": sequence,
        }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        json.dump(tracks, f, indent=4)
        f.write("\n")

    return len(tracks)


def pack_music_dataset(json_path: Path, output_path: Path) -> None:
    """Pack music_tracks.json back to binary dataset_table.bin.

    Writes 3 bytes per track: primary_sample_pack, secondary_sample_pack, sequence_pack.
    """
    with json_path.open() as f:
        tracks = json.load(f)

    max_id = max(int(k) for k in tracks)
    buf = bytearray((max_id + 1) * DATASET_ENTRY_SIZE)

    for key, entry in tracks.items():
        idx = int(key)
        off = idx * DATASET_ENTRY_SIZE
        buf[off] = entry["primary_sample_pack"]
        buf[off + 1] = entry["secondary_sample_pack"]
        buf[off + 2] = entry["sequence_pack"]

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(buf)


def generate_music_header(json_path: Path, header_path: Path) -> None:
    """Generate music_generated.h from music_tracks.json.

    Produces #defines for each track and a static MUSIC_TRACK_NAMES array
    (gated by MUSIC_INCLUDE_NAMES) for gen_struct_info.
    """
    with json_path.open() as f:
        tracks = json.load(f)

    # Find max key to size the array
    max_id = max(int(k) for k in tracks)
    count = max_id + 1

    lines = [
        "/* Auto-generated from music_tracks.json — do not edit */",
        "#ifndef MUSIC_GENERATED_H",
        "#define MUSIC_GENERATED_H",
        "",
    ]

    # Emit #defines (sorted numerically)
    max_name_len = max(len(v["name"]) for v in tracks.values())
    for key in sorted(tracks, key=int):
        entry = tracks[key]
        name = entry["name"]
        padding = " " * (max_name_len - len(name) + 1)
        lines.append(f"#define {name}{padding}{key}")

    lines.append("")
    lines.append(f"#define MUSIC_TRACK_COUNT {count}")
    lines.append("")

    # Names array — only compiled when MUSIC_INCLUDE_NAMES is defined
    # (used by gen_struct_info.c, not the game binary)
    lines.append("#ifdef MUSIC_INCLUDE_NAMES")
    lines.append("static const char *MUSIC_TRACK_NAMES[MUSIC_TRACK_COUNT] = {")
    for i in range(count):
        key = str(i)
        if key in tracks:
            display = tracks[key]["display"]
            # Escape any quotes in display names
            display = display.replace("\\", "\\\\").replace('"', '\\"')
            lines.append(f'    [{i}] = "{display}",')
    lines.append("};")
    lines.append("#endif /* MUSIC_INCLUDE_NAMES */")
    lines.append("")
    lines.append("#endif /* MUSIC_GENERATED_H */")
    lines.append("")

    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text("\n".join(lines))


def _snake_to_display(name: str) -> str:
    """Convert UPPER_SNAKE_CASE to Title Case, preserving trailing digits.

    Examples
    --------
        GAS_STATION -> "Gas Station"
        YOU_WON1 -> "You Won 1"
        NONE -> "None"
    """
    parts = name.split("_")
    result = []
    for part in parts:
        m = re.match(r"^([A-Za-z]+)(\d+)$", part)
        if m:
            result.append(m.group(1).capitalize())
            result.append(m.group(2))
        else:
            result.append(part.capitalize())
    return " ".join(result)


# Keep for backwards compat with any external callers
def bootstrap_music_json(asm_path: Path, output_path: Path) -> None:
    """Deprecated: use export_music_json instead."""
    track_names: dict[int, tuple[str, str]] = {}
    with asm_path.open() as f:
        in_enum = False
        for line in f:
            stripped = line.strip()
            if stripped == ".ENUM MUSIC":
                in_enum = True
                continue
            if stripped == ".ENDENUM":
                break
            if not in_enum:
                continue
            m = re.match(r"(\w+)\s*=\s*(\d+)", stripped)
            if not m:
                continue
            symbol = m.group(1)
            value = int(m.group(2))
            track_names[value] = (f"MUSIC_{symbol}", _snake_to_display(symbol))

    tracks: dict[str, dict[str, str]] = {}
    for value, (name, display) in sorted(track_names.items()):
        tracks[str(value)] = {"name": name, "display": display}

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        json.dump(tracks, f, indent=4)
        f.write("\n")
    print(f"Wrote {len(tracks)} music tracks to {output_path}")
