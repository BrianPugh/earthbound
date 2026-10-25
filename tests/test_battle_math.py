"""Battle math in the C port vs the original ROM (tests/c, CODE-QUALITY.md L2).

``tests/c/battle_cases.txt`` holds cases recorded from the original game by
``tests/c/battle_oracle.py`` (each asm routine called in the playtest emulator
with chosen inputs). ``test_port_matches_rom_cases`` builds the host-only
``battle_math_test`` (libgame + a no-op platform) and replays them; it needs the
extracted assets libgame embeds. ``test_cases_file_is_current`` re-records the
cases in the emulator and checks the committed file is what the ROM produces;
it needs the emulator core, ``make`` and the story saves.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent
C_DIR = ROOT / "tests" / "c"
BUILD_DIR = C_DIR / "build"
CASES = C_DIR / "battle_cases.txt"
MANIFEST = ROOT / "asm" / "bin" / "assets.manifest"
CORE = Path(os.environ.get("EB_EMU_CORE", ROOT / "playtest" / "emu" / "build" / "snes9x_libretro.dylib"))
SAVE = ROOT / "playtest" / "saves" / "26-Dalaam.srm"
# battle_oracle.SCENE_SAVES: the scene cases come from these saves too
SCENE_SAVES = [
    ROOT / "playtest" / "saves" / f"{n}.srm" for n in ("05-Twoson", "12-Zombies", "26-Dalaam", "36-CavePast")
]
MAP_PATH = ROOT / "build" / "earthbound.map"


def _core_present() -> bool:
    if CORE.exists():
        return True
    return CORE.with_suffix(".so").exists()


needs_assets = pytest.mark.skipif(
    not MANIFEST.exists() or shutil.which("cmake") is None or shutil.which("ebtools") is None,
    reason="needs extracted assets (ebtools extract), cmake and ebtools on PATH",
)
needs_emu = pytest.mark.skipif(
    not (
        _core_present()
        and MAP_PATH.exists()
        and all(p.exists() for p in [SAVE, *SCENE_SAVES])
        and (ROOT / "earthbound.sfc").exists()
    ),
    reason="needs the emulator core (playtest/emu/build.sh), build/earthbound.map (make), the donor ROM and saves",
)


def _build() -> Path:
    if not (BUILD_DIR / "CMakeCache.txt").exists():
        subprocess.run(  # noqa: S603
            ["cmake", "-S", str(C_DIR), "-B", str(BUILD_DIR), "-DCMAKE_BUILD_TYPE=Release"],  # noqa: S607
            check=True,
            capture_output=True,
        )
    jobs = str(os.cpu_count() or 4)
    r = subprocess.run(  # noqa: S603
        ["cmake", "--build", str(BUILD_DIR), "--target", "battle_math_test", "-j", jobs],  # noqa: S607
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0, r.stdout[-4000:] + r.stderr[-4000:]
    exe = BUILD_DIR / "battle_math_test"
    return exe if exe.exists() else exe.with_suffix(".exe")


@needs_assets
def test_port_matches_rom_cases():
    exe = _build()
    r = subprocess.run([str(exe), str(CASES)], capture_output=True, text=True, timeout=120)  # noqa: S603
    assert r.returncode == 0, r.stdout[-6000:]
    assert "cases match the ROM" in r.stdout


@needs_emu
def test_cases_file_is_current(tmp_path):
    out = tmp_path / "cases.txt"
    env = dict(os.environ)
    if CORE.exists():
        env.setdefault("EB_EMU_CORE", str(CORE))
    r = subprocess.run(  # noqa: S603
        [sys.executable, str(C_DIR / "battle_oracle.py"), "--out", str(out), "--save", str(SAVE)],
        capture_output=True,
        text=True,
        timeout=1200,
        env=env,
        cwd=ROOT,
    )
    assert r.returncode == 0, r.stdout[-4000:] + r.stderr[-4000:]
    assert out.read_text() == CASES.read_text(), "the ROM disagrees with tests/c/battle_cases.txt; re-record it"
