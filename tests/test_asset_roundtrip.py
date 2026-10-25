"""Round trip of every extracted asset with a human-editable form.

``ebtools extract`` writes each ROM asset to ``asm/bin/`` and, for many of
them, a JSON/PNG form to ``src/assets/``. ``ebtools pack-all`` turns the
latter back into binaries that override the former in the C port. For an
unedited extraction, packing must reproduce the extracted binary; a mismatch
means the packer (or the exporter) loses or reshapes data.

The test runs pack-all once into a temporary directory and compares every
packed file that has an ``asm/bin`` twin. Documented differences:

* Dialogue is not compiled (``_pack_dialogue`` is replaced by an empty
  ``addr_remap``), so the tables whose text pointers pack-all remaps (door
  data, NPC config, telephone contacts, timed delivery, attract mode, the
  bank C3/C4 event scripts) keep their original ROM addresses.
* Tables that store their text inline in the JSON (battle actions, enemies,
  items, PSI abilities) get a fresh pointer into the inline string table, so
  those pointer fields are masked out of the comparison.
* ``.lzhal`` files packed from a PNG are recompressed by ebtools' own
  compressor, so they are compared after decompression.
* The authored overrides in ``src/custom_assets`` are left out (they are
  edits by design).

Skips when the assets have not been extracted.
"""

import atexit
import contextlib
import io
import shutil
import tempfile
from functools import cache
from pathlib import Path
from unittest import mock

import pytest

ROOT = Path(__file__).parent.parent
BIN_DIR = ROOT / "asm" / "bin"
ASSETS_DIR = ROOT / "src" / "assets"
MANIFEST = BIN_DIR / "assets.manifest"

# Packed tables whose text-pointer field is rebuilt from inline JSON text:
# relative path -> (entry size, field offset). The field is a 4-byte LE
# pointer. Entry sizes match the packed C structs (BattleAction 12,
# EnemyData 94, ItemConfig 39, PsiAbility 15).
INLINE_TEXT_POINTER_FIELDS = {
    "data/battle_action_table.bin": (12, 4),  # description
    "data/enemy_configuration_table.bin": (94, 49),  # death text
    "data/item_configuration_table.bin": (39, 35),  # help text
    "data/psi_ability_table.bin": (15, 11),  # description
}

# Assets the PNG form can't carry exactly: relative path -> (reason, number of
# trailing zero bytes of the decompressed original the PNG form drops).
TRAILING_PAD_DROPPED = {
    "US/intro/gas_station.gfx.lzhal": (
        "decompresses to 0xC000 tile bytes plus one trailing 0x00; the game copies only 0xC000 to VRAM "
        "and the PNG holds whole tiles",
        1,
    ),
}


def _assets_extracted() -> bool:
    return MANIFEST.exists() and (ASSETS_DIR / "items" / "items.json").exists()


@cache
def _packed_dir() -> Path:
    """Run pack-all (without dialogue compilation) once per session."""
    from ebtools.cli import pack_all as pack_all_mod

    out = Path(tempfile.mkdtemp(prefix="eb_roundtrip_"))
    atexit.register(shutil.rmtree, out, ignore_errors=True)
    empty_custom = out / "_no_custom_assets"
    empty_custom.mkdir()
    with (
        mock.patch.object(pack_all_mod, "_pack_dialogue", return_value={}),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        pack_all_mod.pack_all(
            ASSETS_DIR,
            out / "packed",
            bin_dir=BIN_DIR,
            yaml_config=ROOT / "earthbound.yml",
            commondata=ROOT / "commondefs.yml",
            text_refs_dir=out / "_text_refs",
            custom_dir=empty_custom,
        )
    return out / "packed"


def _roundtrip_paths() -> list[str]:
    packed = _packed_dir()
    return sorted(
        rel
        for p in packed.rglob("*")
        if p.is_file() and (BIN_DIR / (rel := p.relative_to(packed).as_posix())).is_file()
    )


def pytest_generate_tests(metafunc: pytest.Metafunc) -> None:
    if "asset_path" not in metafunc.fixturenames:
        return
    if not _assets_extracted():
        metafunc.parametrize(
            "asset_path",
            [pytest.param(None, marks=pytest.mark.skip(reason="assets not extracted (run ebtools extract)"))],
        )
        return
    metafunc.parametrize("asset_path", _roundtrip_paths())


def _mask(data: bytes, entry_size: int, offset: int) -> bytes:
    out = bytearray(data)
    for base in range(0, len(out), entry_size):
        out[base + offset : base + offset + 4] = bytes(min(4, max(0, len(out) - base - offset)))
    return bytes(out)


def _first_diff(a: bytes, b: bytes) -> str:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return f"first difference at byte {i}"
    return f"sizes differ ({len(a)} vs {len(b)} bytes)"


def test_pack_reproduces_extracted_binary(asset_path: str) -> None:
    packed = (_packed_dir() / asset_path).read_bytes()
    original = (BIN_DIR / asset_path).read_bytes()
    if packed == original:
        return

    if asset_path in INLINE_TEXT_POINTER_FIELDS:
        entry_size, offset = INLINE_TEXT_POINTER_FIELDS[asset_path]
        assert len(packed) == len(original)
        packed, original = _mask(packed, entry_size, offset), _mask(original, entry_size, offset)
    elif asset_path.endswith(".lzhal"):
        from ebtools.hallz import decompress

        packed, original = decompress(packed), decompress(original)
        if asset_path in TRAILING_PAD_DROPPED:
            _reason, pad = TRAILING_PAD_DROPPED[asset_path]
            assert original[-pad:] == bytes(pad)
            original = original[:-pad]

    assert packed == original, f"{asset_path}: {_first_diff(packed, original)}"


@pytest.mark.skipif(not _assets_extracted(), reason="assets not extracted (run ebtools extract)")
def test_documented_exceptions_still_packed() -> None:
    """Every exception listed above names a file pack-all still produces."""
    paths = set(_roundtrip_paths())
    for rel in (*INLINE_TEXT_POINTER_FIELDS, *TRAILING_PAD_DROPPED):
        assert rel in paths, f"{rel} is no longer packed; drop its exception"
