"""EXP table parser: 4 characters x 100 levels x 4-byte thresholds -> JSON export and packing."""

import struct
from pathlib import Path

from pydantic import BaseModel, Field

from ebtools.byte_reader import ByteReader

CHARACTERS = ["Ness", "Paula", "Jeff", "Poo"]
NUM_LEVELS = 100


class ExpTable(BaseModel):
    """Top-level exp_table.json schema.

    Each character has 100 cumulative EXP thresholds (level 1-99 + max).
    """

    characters: dict[str, list[int]]


def export_exp_table_json(data: bytes, output_path: Path) -> None:
    """Export EXP table binary to JSON."""
    characters: dict[str, list[int]] = {}
    r = ByteReader(data)

    for char_name in CHARACTERS:
        thresholds = []
        for _ in range(NUM_LEVELS):
            thresholds.append(r.read_le32())
        characters[char_name] = thresholds

    config = ExpTable(characters=characters)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def pack_exp_table(json_path: Path, output_path: Path) -> None:
    """Pack exp_table.json back to binary."""
    config = ExpTable.model_validate_json(json_path.read_bytes())
    buf = bytearray()

    for char_name in CHARACTERS:
        thresholds = config.characters[char_name]
        for val in thresholds:
            buf.extend(struct.pack("<I", val))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))
