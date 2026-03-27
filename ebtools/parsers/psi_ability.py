"""PSI ability table parser: 15-byte records -> JSON export and packing."""

import struct
from pathlib import Path
from typing import TYPE_CHECKING

from pydantic import BaseModel, Field

from ebtools.byte_reader import ByteReader
from ebtools.parsers.enemy import _decode_text

if TYPE_CHECKING:
    from ebtools.text_dsl.string_table import StringTableBuilder

RECORD_SIZE = 15
PSI_NAME_SIZE = 25


class PsiAbility(BaseModel):
    """A single PSI ability entry."""

    id: int = Field(ge=0)
    name_index: int = Field(ge=0, le=255)
    name: str = ""
    level: int = Field(ge=0, le=255)
    category: int = Field(ge=0, le=255)
    usability: int = Field(ge=0, le=255)
    battle_action: int = Field(ge=0, le=65535)
    ness_learn_level: int = Field(ge=0, le=255)
    paula_learn_level: int = Field(ge=0, le=255)
    poo_learn_level: int = Field(ge=0, le=255)
    menu_group: int = Field(ge=0, le=255)
    menu_position: int = Field(ge=0, le=255)
    description: str | None = None
    description_ref: str | None = None
    text_pointer: str | None = None  # hex string


class PsiAbilityTable(BaseModel):
    """Top-level psi_abilities.json schema."""

    abilities: list[PsiAbility]


def export_psi_abilities_json(
    data: bytes,
    text_table: dict[int, str],
    psi_names_data: bytes | None,
    output_path: Path,
) -> None:
    """Export PSI ability table binary to JSON."""
    # Parse PSI name table if available (17 entries x 25 bytes)
    psi_names: list[str] = []
    if psi_names_data:
        for i in range(len(psi_names_data) // PSI_NAME_SIZE):
            entry = psi_names_data[i * PSI_NAME_SIZE : (i + 1) * PSI_NAME_SIZE]
            psi_names.append(_decode_text(entry, text_table))

    abilities = []
    num = len(data) // RECORD_SIZE

    for idx in range(num):
        r = ByteReader(data[idx * RECORD_SIZE : (idx + 1) * RECORD_SIZE])
        name_index = r.read_byte()
        name = psi_names[name_index] if name_index < len(psi_names) else ""
        abilities.append(
            PsiAbility(
                id=idx,
                name_index=name_index,
                name=name,
                level=r.read_byte(),
                category=r.read_byte(),
                usability=r.read_byte(),
                battle_action=r.read_le16(),
                ness_learn_level=r.read_byte(),
                paula_learn_level=r.read_byte(),
                poo_learn_level=r.read_byte(),
                menu_group=r.read_byte(),
                menu_position=r.read_byte(),
                text_pointer=f"0x{r.read_le32():06X}",
            )
        )

    config = PsiAbilityTable(abilities=abilities)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as f:
        f.write(config.model_dump_json(indent=2))
        f.write("\n")


def pack_psi_abilities(
    json_path: Path,
    output_path: Path,
    string_table: StringTableBuilder | None = None,
    addr_remap: dict[int, int] | None = None,
) -> None:
    """Pack psi_abilities.json back to binary."""
    config = PsiAbilityTable.model_validate_json(json_path.read_bytes())
    buf = bytearray()

    for ability in config.abilities:
        buf.append(ability.name_index)
        buf.append(ability.level)
        buf.append(ability.category)
        buf.append(ability.usability)
        buf.extend(struct.pack("<H", ability.battle_action))
        buf.append(ability.ness_learn_level)
        buf.append(ability.paula_learn_level)
        buf.append(ability.poo_learn_level)
        buf.append(ability.menu_group)
        buf.append(ability.menu_position)
        if ability.description is not None and string_table is not None:
            ptr = string_table.add(ability.description)
        elif ability.text_pointer is not None:
            ptr = int(ability.text_pointer, 16)
        else:
            ptr = 0
        if addr_remap and ptr in addr_remap:
            ptr = addr_remap[ptr]
        buf.extend(struct.pack("<I", ptr))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buf))
