"""Teleport destination table parsers: export and packing for both tables."""

import struct
from pathlib import Path

from pydantic import BaseModel, Field

from ebtools.byte_reader import ByteReader
from ebtools.config import CommonData
from ebtools.parsers.enemy import _decode_text, _encode_text

TELEPORT_RECORD_SIZE = 8
PSI_TELEPORT_RECORD_SIZE = 31
PSI_TELEPORT_NAME_SIZE = 25


class TeleportDestination(BaseModel):
    """A single teleport destination entry (8 bytes)."""

    id: int = Field(ge=0)
    x: int = Field(ge=0, le=65535)
    y: int = Field(ge=0, le=65535)
    direction: int = Field(ge=0, le=255)
    direction_name: str = ""
    screen_transition: int = Field(ge=0, le=255)
    unknown_6: int = Field(ge=0, le=255)
    unknown_7: int = Field(ge=0, le=255)


class TeleportDestinationTable(BaseModel):
    """Top-level teleport_destinations.json schema."""

    destinations: list[TeleportDestination]


class PsiTeleportDestination(BaseModel):
    """A single PSI teleport destination entry (31 bytes)."""

    id: int = Field(ge=0)
    name: str
    event_flag: int = Field(ge=0, le=65535)
    x: int = Field(ge=0, le=65535)
    y: int = Field(ge=0, le=65535)


class PsiTeleportDestinationTable(BaseModel):
    """Top-level psi_teleport_destinations.json schema."""

    destinations: list[PsiTeleportDestination]


def export_teleport_json(
    data: bytes,
    common_data: CommonData,
    output_path: Path,
) -> None:
    """Export teleport destination table binary to JSON."""
    destinations = []
    num = len(data) // TELEPORT_RECORD_SIZE

    for idx in range(num):
        r = ByteReader(data[idx * TELEPORT_RECORD_SIZE : (idx + 1) * TELEPORT_RECORD_SIZE])
        x = r.read_le16()
        y = r.read_le16()
        direction = r.read_byte()
        dir_name = common_data.directions[direction] if direction < len(common_data.directions) else ""
        destinations.append(
            TeleportDestination(
                id=idx,
                x=x,
                y=y,
                direction=direction,
                direction_name=dir_name,
                screen_transition=r.read_byte(),
                unknown_6=r.read_byte(),
                unknown_7=r.read_byte(),
            )
        )

    config = TeleportDestinationTable(destinations=destinations)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def pack_teleport(json_path: Path, common_data: CommonData, output_path: Path) -> None:
    """Pack teleport_destinations.json back to binary."""
    config = TeleportDestinationTable.model_validate_json(json_path.read_bytes())
    buf = bytearray()

    for dest in config.destinations:
        buf.extend(struct.pack("<H", dest.x))
        buf.extend(struct.pack("<H", dest.y))
        buf.append(dest.direction)
        buf.append(dest.screen_transition)
        buf.append(dest.unknown_6)
        buf.append(dest.unknown_7)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


def export_psi_teleport_json(
    data: bytes,
    text_table: dict[int, str],
    output_path: Path,
) -> None:
    """Export PSI teleport destination table binary to JSON."""
    destinations = []
    num = len(data) // PSI_TELEPORT_RECORD_SIZE

    for idx in range(num):
        entry = data[idx * PSI_TELEPORT_RECORD_SIZE : (idx + 1) * PSI_TELEPORT_RECORD_SIZE]
        name = _decode_text(entry[:PSI_TELEPORT_NAME_SIZE], text_table)
        r = ByteReader(entry[PSI_TELEPORT_NAME_SIZE:])
        destinations.append(
            PsiTeleportDestination(
                id=idx,
                name=name,
                event_flag=r.read_le16(),
                x=r.read_le16(),
                y=r.read_le16(),
            )
        )

    config = PsiTeleportDestinationTable(destinations=destinations)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def pack_psi_teleport(
    json_path: Path,
    text_table: dict[int, str],
    output_path: Path,
) -> None:
    """Pack psi_teleport_destinations.json back to binary."""
    reverse_table: dict[str, int] = {}
    for code, char in text_table.items():
        if char not in reverse_table:
            reverse_table[char] = code

    config = PsiTeleportDestinationTable.model_validate_json(json_path.read_bytes())
    buf = bytearray()

    for dest in config.destinations:
        buf.extend(_encode_text(dest.name, reverse_table, PSI_TELEPORT_NAME_SIZE))
        buf.extend(struct.pack("<H", dest.event_flag))
        buf.extend(struct.pack("<H", dest.x))
        buf.extend(struct.pack("<H", dest.y))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))
