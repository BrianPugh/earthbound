"""Enemy stats table: packed JSON must match the ROM extraction.

The enemy JSON in ``src/assets/enemies`` is packed with the same function
``ebtools pack-all`` uses. Every byte must equal ``asm/bin`` except the 2
text pointers, which pack-all remaps to the port's relocated dialogue.
The ``EnemyData`` offsets in ``src/game/battle.h`` must also agree with the
asm ``enemy_data`` struct in ``include/structs.asm``.

Tests skip when ``asm/bin`` or the extracted assets are missing.
"""

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent
ROM_BIN = ROOT / "asm" / "bin" / "data" / "enemy_configuration_table.bin"
ENEMY_JSON = ROOT / "src" / "assets" / "enemies" / "enemies.json"
STRUCTS_ASM = ROOT / "include" / "structs.asm"
BATTLE_H = ROOT / "src" / "game" / "battle.h"

ENTRY_SIZE = 94
ENTRY_COUNT = 231
# encounter_text_ptr (u32 at 45) and death_text_ptr (u32 at 49).
REMAPPED = set(range(45, 53))

needs_assets = pytest.mark.skipif(
    not (ROM_BIN.exists() and ENEMY_JSON.exists()),
    reason="enemy table or enemies.json missing (run ebtools extract)",
)


def _field_at(offsets: dict[str, int], pos: int) -> str:
    name = "?"
    for field, off in sorted(offsets.items(), key=lambda kv: kv[1]):
        if off <= pos:
            name = field
    return name


def _asm_offsets() -> dict[str, int]:
    """Field -> offset from the USA branch of the asm enemy_data struct."""
    text = STRUCTS_ASM.read_text()
    body = re.search(r"\.STRUCT enemy_data\n(.*?)\.ENDSTRUCT", text, re.S).group(1)
    # Keep the USA branch: drop the .ELSE..ENDIF block and the .IFDEF/.ENDIF lines.
    body = re.sub(r"\.ELSE.*?\.ENDIF", "", body, flags=re.S)
    offsets = {}
    for m in re.finditer(r"^\s*(\w+)\s+\.\w+(?:\s+\d+)?\s*;(\d+)\s*$", body, re.M):
        offsets[m.group(1)] = int(m.group(2))
    return offsets


def _header_offsets() -> dict[str, int]:
    """Field -> offset from the ``/* NN */`` comments of the C EnemyData struct."""
    text = BATTLE_H.read_text()
    body = re.search(r"typedef struct \{\n((?:(?!\} ).*\n)*?)\} EnemyData;", text).group(1)
    offsets = {}
    for m in re.finditer(r"^\s*\w+\s+(\w+)(?:\[\w+\])?;\s*/\*\s*(\d+)\s*\*/", body, re.M):
        offsets[m.group(1)] = int(m.group(2))
    return offsets


@needs_assets
def test_packed_enemy_table_matches_rom(tmp_path):
    from ebtools.config import load_common_data, load_dump_doc
    from ebtools.parsers.enemy import pack_enemies

    doc = load_dump_doc(ROOT / "earthbound.yml")
    common_data = load_common_data(ROOT / "commondefs.yml")
    out = tmp_path / "enemy_configuration_table.bin"
    pack_enemies(ENEMY_JSON, doc.textTable, common_data, out)

    rom = ROM_BIN.read_bytes()
    packed = out.read_bytes()
    assert len(rom) == ENTRY_COUNT * ENTRY_SIZE
    assert len(packed) == len(rom)

    names = {e["id"]: e["name"] for e in __import__("json").loads(ENEMY_JSON.read_text())["enemies"]}
    offsets = _header_offsets()
    errors = []
    for i in range(ENTRY_COUNT):
        for off in range(ENTRY_SIZE):
            if off in REMAPPED:
                continue
            pos = i * ENTRY_SIZE + off
            if rom[pos] != packed[pos]:
                errors.append(
                    f"enemy {i} ({names.get(i)!r}) offset {off} ({_field_at(offsets, off)}): "
                    f"rom {rom[pos]:#04x} != packed {packed[pos]:#04x}"
                )
    assert not errors, "\n".join(errors[:20]) + f"\n{len(errors)} mismatching bytes"


def test_enemy_struct_offsets_match_asm():
    asm = _asm_offsets()
    hdr = _header_offsets()
    assert len(asm) > 40, f"asm parse found too few fields: {asm}"
    assert len(hdr) > 40, f"header parse found too few fields: {hdr}"
    assert max(asm.values()) == 93 and max(hdr.values()) == 93
    # Fields differ in name between the two sides for some entries, so compare the sorted offset lists.
    assert sorted(asm.values()) == sorted(hdr.values())
