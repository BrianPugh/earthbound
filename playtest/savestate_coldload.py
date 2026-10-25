"""Cold-process savestate check: save in one game process, load in a fresh one.

For each scene below, a pt.py session boots a save, plays to the scene and takes a
``snap``. Then:

1. ``earthbound --selftest-coldload COPY`` loads a copy of the snap in a fresh
   process (nothing but game_init has run), re-saves it and requires the two slot
   images to be byte-identical.
2. The saving session restores its own snap and plays a few steps; a second, fresh
   session restores the same snap and plays the same steps. Their STATE/PARTY/text
   output must match: the cold process continues exactly like the warm one.

Usage::

    EB_BIN=/abs/path/earthbound uv run --no-sync python playtest/savestate_coldload.py [-k SCENE]

Exit status is 0 when every scene passes.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PT = [sys.executable, str(ROOT / "playtest" / "pt.py")]
SESSIONS = ROOT / "playtest" / "out" / "sessions"
SAVES = ROOT / "playtest" / "saves"

# name, save, steps to reach the scene, steps to play after the restore
SCENES = [
    ("overworld", "24-Museum", "40 DOWN", "60 LEFT; 30 DOWN"),
    ("pause-menu", "24-Museum", "press A; until input 60; wait 10", "press DOWN; wait 10; press B; wait 30; 30 UP"),
    (
        "dialogue",
        "02-Sharks",
        "press A; until input 300; press A; wait 20",
        "until input 600 advance; wait 10; press B; wait 30",
    ),
    (
        "battle",
        "17-GoldMine",
        "teleport 3424 9312; until battle 600 LEFT; wait 200",
        "until in:BATTLE_MENU 1500 advance; wait 10; press RIGHT 2; press A; wait 600",
    ),
    ("town-twoson", "05-Twoson", "120 RIGHT; 60 DOWN", "90 LEFT; 40 UP"),
    (
        "town-map",
        "05-Twoson",
        "debug on; 10 B+R; until input 300; wait 10; press DOWN 5; press A; wait 10; press A; wait 10;"
        " press UP 1; wait 10; press A; wait 100",
        "wait 30; press DOWN; wait 60",
    ),
    (
        "credits",
        "36-CavePast",
        "debug on; 10 B+R; until input 300; wait 10; press DOWN 5; press A; wait 10; press A; wait 10;"
        " press A; wait 10; press UP 2; wait 10; press A; wait 1500",
        "wait 600",
    ),
]

# Lines that legitimately differ between two processes playing the same frames.
IGNORE = re.compile(r"^(SHOT|UNTIL)\b|^> ")


def pt(*args, check=True):
    """Run one pt.py command and return its stdout."""
    r = subprocess.run(PT + list(args), cwd=ROOT, capture_output=True, text=True)  # noqa: S603
    if check and r.returncode != 0:
        raise RuntimeError(f"pt.py {' '.join(args)} failed:\n{r.stdout}\n{r.stderr}")
    return r.stdout


def comparable(out):
    """Keep the lines that describe game state, minus per-process noise."""
    return [ln for ln in out.splitlines() if ln.strip() and not IGNORE.match(ln)]


def run_scene(name, save, reach, after, binary):
    warm, cold = f"ssc-{name}-a", f"ssc-{name}-b"
    problems = []
    try:
        pt("start", warm, "--save", str(SAVES / f"{save}.srm"))
        pt("do", warm, f"{reach}; snap ssc", "--no-shot", "--quiet")
        snaps = SESSIONS / warm / "snaps"

        with tempfile.TemporaryDirectory() as tmp:
            for f in snaps.glob("ssc.*"):
                shutil.copy(f, tmp)
            r = subprocess.run(  # noqa: S603
                [binary, "--selftest-coldload", str(Path(tmp) / "ssc")], capture_output=True, text=True
            )
            if r.returncode != 0 or "cold-load self-test: PASS" not in r.stderr:
                problems.append("cold-load re-save differs: " + r.stderr.strip().splitlines()[-1:][0])

        warm_out = pt("do", warm, f"restore ssc; wait 2; {after}", "--no-shot")
        pt("start", cold, "--save", str(SAVES / f"{save}.srm"))
        (SESSIONS / cold / "snaps").mkdir(exist_ok=True)
        for f in snaps.glob("ssc.*"):
            shutil.copy(f, SESSIONS / cold / "snaps" / f.name)
        cold_out = pt("do", cold, f"restore ssc; wait 2; {after}", "--no-shot")
        a, b = comparable(warm_out), comparable(cold_out)
        if not any(ln.startswith("STATE") for ln in b):
            problems.append("cold session printed no STATE line")
        elif a != b:
            diff = next((f"warm: {x}\n    cold: {y}" for x, y in zip(a, b) if x != y), "line counts differ")
            problems.append("cold-process continuation differs:\n    " + diff)
    finally:
        pt("stop", warm, check=False)
        pt("stop", cold, check=False)
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", help="only scenes whose name contains this")
    args = ap.parse_args()
    binary = os.environ.get("EB_BIN", str(ROOT / "port/unix/build/earthbound"))
    failed = 0
    for name, save, reach, after in SCENES:
        if args.k and args.k not in name:
            continue
        problems = run_scene(name, save, reach, after, binary)
        print(f"{name}: {'PASS' if not problems else 'FAIL'}")
        for p in problems:
            print(f"  {p}")
        failed += bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
