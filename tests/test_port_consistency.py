"""Cross-checks between the C port and the disassembly.

Each test catches a class of bug where the C port silently disagrees with
the original: a ROM/WRAM address typed wrong, a script the port can't load,
a callroutine with no dispatch case, libc ``rand()`` in place of the game
RNG, or a text-pointer table packed without address remapping.

Tests that need the assembled ROM's linker map (``make``) or extracted
assets (``ebtools extract``) skip when those artifacts are missing.
"""

import re
import struct
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent
MAP_PATH = ROOT / "build" / "earthbound.map"
SCRIPT_POINTERS_PATH = ROOT / "asm" / "bin" / "US" / "events" / "event_script_pointers.bin"

needs_map = pytest.mark.skipif(not MAP_PATH.exists(), reason="build/earthbound.map missing (run make)")
needs_pointers = pytest.mark.skipif(
    not SCRIPT_POINTERS_PATH.exists(), reason="event_script_pointers.bin missing (run ebtools extract)"
)

# JP/prototype-only sources reference labels absent from the US build.
VERSION_SPECIFIC = re.compile(r"-(jp|proto)\.asm$")


def _map_labels() -> dict[str, int]:
    """Exported label -> 24-bit address, from the ld65 map's export list."""
    text = MAP_PATH.read_text()
    return {name: int(addr, 16) for name, addr in re.findall(r"(\w+)\s+([0-9A-F]{6}) RL", text)}


def _rom_addr_constants() -> dict[str, int]:
    """ROM_ADDR_<NAME> -> value from every C header under src/ (vendor excluded)."""
    consts = {}
    for path in (ROOT / "src").rglob("*.h"):
        if "vendor" in path.parts:
            continue
        for name, value in re.findall(r"#define\s+ROM_ADDR_(\w+)\s+0x([0-9A-Fa-f]{6})", path.read_text()):
            consts[name] = int(value, 16)
    return consts


def _event_asm_files() -> list[Path]:
    dirs = [ROOT / "asm" / "data" / "events", ROOT / "asm" / "overworld" / "actionscript"]
    return [p for d in dirs for p in d.rglob("*.asm") if not VERSION_SPECIFIC.search(p.name)]


def _strip_c_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


@needs_map
def test_rom_addr_constants_match_linker_map():
    labels = _map_labels()
    mismatched = {
        name: (hex(value), hex(labels[name]))
        for name, value in _rom_addr_constants().items()
        if name in labels and labels[name] != value
    }
    assert not mismatched, f"ROM_ADDR_* (constant, map) disagree: {mismatched}"


@needs_map
def test_script_callroutines_have_dispatch_cases():
    """Every routine an event script calls must reach a callroutine case.

    A missing case falls through to callroutine_dispatch's default, which
    leaves pc unchanged so the routine's parameter bytes run as opcodes.
    """
    labels = _map_labels()
    consts = _rom_addr_constants()

    # Event macros that wrap EVENT_CALLROUTINE <SYM>.
    wrapped: dict[str, set[str]] = {}
    current = None
    for line in (ROOT / "include" / "eventmacros.asm").read_text().splitlines():
        if m := re.match(r"\s*\.MACRO\s+(\w+)", line):
            current = m.group(1)
        elif re.match(r"\s*\.ENDMACRO", line):
            current = None
        elif current and (m := re.search(r"EVENT_CALLROUTINE\s+(?:\.LOWORD\()?(\w+)", line)):
            wrapped.setdefault(current, set()).add(m.group(1))

    used: set[str] = set()
    for path in _event_asm_files():
        for line in path.read_text().splitlines():
            line = line.split(";")[0]
            if m := re.search(r"EVENT_CALLROUTINE\s+(?:\.LOWORD\()?(\w+)", line):
                used.add(m.group(1))
            if (m := re.match(r"\s*(EVENT_\w+)", line)) and m.group(1) in wrapped:
                used |= wrapped[m.group(1)]
    assert len(used) > 100

    cases: set[int] = set()
    for path in (ROOT / "src" / "entity").glob("*.c"):
        text = path.read_text()
        cases |= {consts[n] for n in re.findall(r"case\s+ROM_ADDR_(\w+)\s*:", text) if n in consts}
        cases |= {int(v, 16) for v in re.findall(r"case\s+0x([0-9A-Fa-f]{6})\s*:", text)}

    missing = []
    for sym in sorted(used):
        # Non-exported labels resolve through their same-named ROM_ADDR_ constant.
        addr = labels.get(sym, consts.get(sym))
        if addr is None or addr not in cases:
            missing.append(sym)
    assert not missing, f"Script callroutines without a dispatch case: {missing}"


@needs_map
def test_script_wram_writes_are_resolved():
    """Every WRAM symbol event scripts write must be mapped by resolve_wram()."""
    labels = _map_labels()
    opcodes = (ROOT / "src" / "entity" / "opcodes.c").read_text()
    handled = {int(a, 16) for a in re.findall(r"WRAM_\w+\(\s*0x([0-9A-Fa-f]+)", opcodes)}
    # Special-cased in the WRITE_*_WRAM handlers via `addr == WRAM_<NAME>`.
    special = set(re.findall(r"addr == WRAM_(\w+)", opcodes))

    used: set[str] = set()
    for path in _event_asm_files():
        used |= set(re.findall(r"EVENT_\w*WRAM\w*\s+\.LOWORD\((\w+)\)", path.read_text()))
    assert used

    unresolved = [
        sym
        for sym in sorted(used)
        if (labels.get(sym, -1) & 0xFFFF) not in handled
        # Special-case names may abbreviate the symbol or split it (_LO/_HI).
        and not any(sym.startswith(s) or s.startswith(sym + "_") for s in special)
        and not sym.startswith("UNREAD_")
    ]
    assert not unresolved, f"Script WRAM writes resolve_wram() doesn't map: {unresolved}"


def _yml_entry(name: str) -> tuple[int, int]:
    text = (ROOT / "earthbound.yml").read_text()
    m = re.search(rf"name: '?{name}'?\n\s+offset: (0x[0-9A-Fa-f]+)\n\s+size: (0x[0-9A-Fa-f]+|\d+)", text)
    assert m, f"{name} missing from earthbound.yml"
    return int(m.group(1), 16), int(m.group(2), 0)


@needs_pointers
def test_event_script_pointers_resolve_to_loaded_banks():
    """Every script ID must land in a script bank event_script_data.c loads."""
    banks = [
        _yml_entry(name)
        for name in ("title_screen_scripts", "bank_c4_scripts", "bank_c3_scripts_combined", "bank_c0_scripts")
    ]
    ranges = [(0xC00000 + offset, 0xC00000 + offset + size) for offset, size in banks]
    data = SCRIPT_POINTERS_PATH.read_bytes()
    allowed = {0}  # EVENT_0: debug cursor in bank C2, unused by the port
    unresolved = []
    for script_id in range(len(data) // 3):
        addr = int.from_bytes(data[script_id * 3 : script_id * 3 + 3], "little")
        if script_id not in allowed and not any(lo <= addr < hi for lo, hi in ranges):
            unresolved.append((script_id, hex(addr)))
    assert not unresolved, f"Script IDs outside every loaded script bank: {unresolved}"


def test_no_libc_rand_in_game_code():
    """Gameplay randomness must come from the serialized game RNG (RAND)."""
    offenders = []
    for path in (ROOT / "src").rglob("*.c"):
        if {"vendor", "tools"} & set(path.parts):
            continue
        code = _strip_c_comments(path.read_text())
        if re.search(r"(?<![\w.>])s?rand\s*\(", code):
            offenders.append(str(path.relative_to(ROOT)))
    assert not offenders, f"libc rand()/srand() in game code: {offenders}"


def test_timed_delivery_pack_remaps_text_pointers(tmp_path):
    from ebtools.parsers.simple_tables import pack_timed_delivery

    entry = {
        "sprite_id": 1,
        "event_flag": 2,
        "attempt_limit": 3,
        "time_value": 4,
        "delivery_time": 5,
        "success_text_ptr": "0xC7A542",
        "failure_text_ptr": "0xC7A7D9",
        "enter_speed": 6,
        "exit_speed": 7,
    }
    json_path = tmp_path / "timed_delivery.json"
    json_path.write_text(__import__("json").dumps([entry]))
    out = tmp_path / "timed_delivery_table.bin"
    pack_timed_delivery(json_path, out, addr_remap={0xC7A542: 0x14D015, 0xC7A7D9: 0x14D40C})

    packed = out.read_bytes()
    assert len(packed) == 20
    success = struct.unpack_from("<H", packed, 10)[0] | packed[12] << 16
    failure = struct.unpack_from("<H", packed, 13)[0] | packed[15] << 16
    assert (success, failure) == (0x14D015, 0x14D40C)
