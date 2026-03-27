"""NPC config parser: 17-byte records -> assembly, JSON export, and packing.

Ported from app.d:parseNPCConfig.
"""

import struct
from pathlib import Path

from pydantic import BaseModel, Field

from ebtools.config import CommonData, DumpDoc
from ebtools.parsers._common import format_pointer

NPC_TYPES = {1: "PERSON", 2: "ITEM_BOX", 3: "OBJECT"}
NPC_TYPE_IDS = {"PERSON": 1, "ITEM_BOX": 2, "OBJECT": 3}

RECORD_SIZE = 17


class NpcEntry(BaseModel):
    """A single NPC configuration entry (17 bytes)."""

    id: int = Field(ge=0)
    type: str
    sprite_id: int = Field(ge=0, le=65535)
    direction: int = Field(ge=0, le=255)
    direction_name: str = ""
    movement_type: int = Field(ge=0, le=255)
    appearance_style: int = Field(ge=0, le=255)
    event_flag: int = Field(ge=0, le=65535)
    event_flag_name: str = ""
    show_condition: int = Field(ge=0, le=255)
    dialogue_ref: str | None = None
    text_pointer: str | None = None  # hex string (32-bit .DWORD, bytes 9-12)
    # Bytes 13-16: secondary data (type-dependent)
    secondary_pointer: str = ""  # hex string for OBJECT type (32-bit .DWORD)
    secondary_bytes: list[int] = Field(default_factory=list)  # for non-OBJECT types (4 bytes)


class NpcConfig(BaseModel):
    """Top-level npc_config.json schema."""

    npcs: list[NpcEntry]


def export_npcs_json(
    data: bytes,
    common_data: CommonData,
    output_path: Path,
) -> None:
    """Export NPC config table binary to JSON."""
    npcs = []
    num = len(data) // RECORD_SIZE

    for idx in range(num):
        entry = data[idx * RECORD_SIZE : (idx + 1) * RECORD_SIZE]
        npc_type_id = entry[0]
        npc_type = NPC_TYPES.get(npc_type_id, f"UNKNOWN_{npc_type_id}")
        is_object = npc_type_id == 3

        sprite_id = entry[1] | (entry[2] << 8)
        direction = entry[3]
        dir_name = common_data.directions[direction] if direction < len(common_data.directions) else ""
        movement_type = entry[4]
        appearance_style = entry[5]
        event_flag = entry[6] | (entry[7] << 8)
        flag_name = common_data.eventFlags[event_flag] if event_flag < len(common_data.eventFlags) else ""
        show_condition = entry[8]
        # Bytes 9-12: text pointer as 32-bit LE (.DWORD)
        text_ptr = entry[9] | (entry[10] << 8) | (entry[11] << 16) | (entry[12] << 24)

        if is_object:
            # Bytes 13-16: secondary pointer as 32-bit LE (.DWORD)
            sec_ptr = entry[13] | (entry[14] << 8) | (entry[15] << 16) | (entry[16] << 24)
            sec_bytes: list[int] = []
            sec_ptr_str = f"0x{sec_ptr:08X}"
        else:
            sec_ptr_str = ""
            sec_bytes = [entry[13], entry[14], entry[15], entry[16]]

        npcs.append(
            NpcEntry(
                id=idx,
                type=npc_type,
                sprite_id=sprite_id,
                direction=direction,
                direction_name=dir_name,
                movement_type=movement_type,
                appearance_style=appearance_style,
                event_flag=event_flag,
                event_flag_name=flag_name,
                show_condition=show_condition,
                text_pointer=f"0x{text_ptr:08X}",
                secondary_pointer=sec_ptr_str,
                secondary_bytes=sec_bytes,
            )
        )

    config = NpcConfig(npcs=npcs)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def pack_npcs(
    json_path: Path,
    common_data: CommonData,
    output_path: Path,
    addr_remap: dict[int, int] | None = None,
) -> None:
    """Pack npc_config.json back to binary."""
    config = NpcConfig.model_validate_json(json_path.read_bytes())
    buf = bytearray()

    for npc in config.npcs:
        # Type (u8)
        type_id = NPC_TYPE_IDS.get(npc.type)
        if type_id is None:
            raise ValueError(f"NPC {npc.id}: unknown type '{npc.type}'")
        buf.append(type_id)
        # Sprite ID (u16 LE)
        buf.extend(struct.pack("<H", npc.sprite_id))
        # Direction (u8)
        buf.append(npc.direction)
        # Movement type (u8)
        buf.append(npc.movement_type)
        # Appearance style (u8)
        buf.append(npc.appearance_style)
        # Event flag (u16 LE)
        buf.extend(struct.pack("<H", npc.event_flag))
        # Show condition (u8)
        buf.append(npc.show_condition)
        # Text pointer (32-bit LE, bytes 9-12)
        ptr = int(npc.text_pointer, 16)
        if addr_remap and ptr in addr_remap:
            ptr = addr_remap[ptr]
        buf.extend(struct.pack("<I", ptr))
        # Secondary data (bytes 13-16)
        if npc.type == "OBJECT" and npc.secondary_pointer:
            buf.extend(struct.pack("<I", int(npc.secondary_pointer, 16)))
        else:
            for b in npc.secondary_bytes:
                buf.append(b)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))


def parse_npc_config(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Parse NPC configuration (17-byte records) to assembly."""
    filename = f"{base_name}.{extension}"

    with (dir / filename).open("w") as out:
        for i in range(0, len(source), RECORD_SIZE):
            entry = source[i : i + RECORD_SIZE]
            if len(entry) < RECORD_SIZE:
                break

            npc_type = NPC_TYPES.get(entry[0], "ERROR")
            secondary_pointer = entry[0] == 3

            out.write(f"  .BYTE NPC_TYPE::{npc_type}\n")
            out.write(f"  .WORD ${entry[1] + (entry[2] << 8):04X}\n")
            out.write(f"  .BYTE DIRECTION::{common_data.directions[entry[3]]}\n")
            out.write(f"  .BYTE ${entry[4]:02X}\n")
            out.write(f"  .BYTE ${entry[5]:02X}\n")
            out.write(f"  .WORD EVENT_FLAG::{common_data.eventFlags[entry[6] + (entry[7] << 8)]}\n")
            out.write(f"  .BYTE ${entry[8]:02X}\n")
            out.write(format_pointer(entry[9] + (entry[10] << 8) + (entry[11] << 16)))
            if secondary_pointer:
                out.write(format_pointer(entry[13] + (entry[14] << 8) + (entry[15] << 16)))
            else:
                out.write(f"  .BYTE ${entry[13]:02X}, ${entry[14]:02X}, ${entry[15]:02X}, ${entry[16]:02X}\n")
            out.write("\n")

    return [filename]
