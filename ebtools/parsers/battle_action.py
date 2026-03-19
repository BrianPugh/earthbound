"""Battle action table parser: 12-byte records -> JSON export and packing."""

import struct
from pathlib import Path

from pydantic import BaseModel, Field

from ebtools.byte_reader import ByteReader

RECORD_SIZE = 12


class BattleAction(BaseModel):
    """A single battle action entry."""

    id: int = Field(ge=0)
    direction: int = Field(ge=0, le=255)
    target: int = Field(ge=0, le=255)
    type: int = Field(ge=0, le=255)
    pp_cost: int = Field(ge=0, le=255)
    description_pointer: str  # hex string
    function_pointer: str  # hex string


class BattleActionTable(BaseModel):
    """Top-level battle_actions.json schema."""

    actions: list[BattleAction]


def export_battle_actions_json(data: bytes, output_path: Path) -> None:
    """Export battle action table binary to JSON."""
    actions = []
    num = len(data) // RECORD_SIZE

    for idx in range(num):
        r = ByteReader(data[idx * RECORD_SIZE : (idx + 1) * RECORD_SIZE])
        actions.append(
            BattleAction(
                id=idx,
                direction=r.read_byte(),
                target=r.read_byte(),
                type=r.read_byte(),
                pp_cost=r.read_byte(),
                description_pointer=f"0x{r.read_le32():06X}",
                function_pointer=f"0x{r.read_le32():06X}",
            )
        )

    config = BattleActionTable(actions=actions)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def pack_battle_actions(json_path: Path, output_path: Path) -> None:
    """Pack battle_actions.json back to binary."""
    config = BattleActionTable.model_validate_json(json_path.read_bytes())
    buf = bytearray()

    for action in config.actions:
        buf.append(action.direction)
        buf.append(action.target)
        buf.append(action.type)
        buf.append(action.pp_cost)
        buf.extend(struct.pack("<I", int(action.description_pointer, 16)))
        buf.extend(struct.pack("<I", int(action.function_pointer, 16)))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))
