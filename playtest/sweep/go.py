"""Walk a pt.py session through waypoints with 8-way steps, Auto Fighting any battle.

For recording sweep segments: start a session, `rng 4660 22136; fc 0; padlog FILE`, then
walk with this (it only sends `until battle N DIR` steps, so padlog sees every frame).

Usage::

    EB_BIN=/abs/build-native/earthbound uv run --no-sync python playtest/sweep/go.py SESSION X,Y [X,Y ...]
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PT = [sys.executable, str(ROOT / "playtest/pt.py")]
FIGHT = "until in:BATTLE_MENU 1500 advance; wait 10; press RIGHT 2; press A; until idle 8000 advance"


def do(session, cmd):
    out = subprocess.run(PT + ["do", session, cmd, "--no-shot"], capture_output=True, text=True, cwd=ROOT).stdout  # noqa: S603
    pos = re.findall(r"pos=\((\d+),(\d+)\)", out)
    bat = re.findall(r"battle=(\d+)", out)
    modes = re.findall(r"modes=\[([^\]]*)\]", out)
    return (
        (int(pos[-1][0]), int(pos[-1][1])) if pos else None,
        (bool(bat) and bat[-1] != "0") or "idle=battle" in out,
        modes[-1] if modes else "",
        out,
    )


def fight(session):
    print("  battle", flush=True)
    out = do(session, FIGHT)[3]
    for line in out.splitlines():
        if line.startswith(("PARTY", "UNTIL")):
            print("   ", line[:200])
    if "TIMEOUT" in out:
        sys.exit("battle timeout")


def heading(dx, dy):
    h = "RIGHT" if dx > 0 else "LEFT"
    v = "DOWN" if dy > 0 else "UP"
    if abs(dx) > 2 * abs(dy):
        return h
    if abs(dy) > 2 * abs(dx):
        return v
    return v + "+" + h


def sidesteps(d):
    if "+" in d:
        return d.split("+")[::-1]
    if d in ("LEFT", "RIGHT"):
        return [f"UP+{d}", f"DOWN+{d}"]
    return [f"{d}+LEFT", f"{d}+RIGHT"]


def main():
    session, points = sys.argv[1], sys.argv[2:]
    pos = do(session, "state")[0]
    for wp in points:
        tx, ty = map(int, wp.split(","))
        stuck = 0
        best, since_best = 1 << 30, 0
        while abs(tx - pos[0]) > 3 or abs(ty - pos[1]) > 3:
            dist = abs(tx - pos[0]) + abs(ty - pos[1])
            if dist < best:
                best, since_best = dist, 0
            else:
                since_best += 1
                if since_best >= 6:  # oscillating round an obstacle: give up on this waypoint
                    sys.exit(f"NO PROGRESS at {pos} heading {wp}")
            d = heading(tx - pos[0], ty - pos[1])
            n = max(2, min(24, int(max(abs(tx - pos[0]), abs(ty - pos[1])) / 1.5)))
            npos, bat, modes, _ = do(session, f"until battle {n} {d}")
            if bat or "BATTLE" in modes:
                fight(session)
                pos = do(session, "state")[0]
                continue
            if "DOOR" in modes or "TELEPORT" in modes:
                do(session, "until idle 600")
            moved = abs(npos[0] - pos[0]) + abs(npos[1] - pos[1])
            pos = npos
            if moved >= 2:
                stuck = 0
                continue
            stuck += 1
            for alt in sidesteps(d):
                npos, bat, _, _ = do(session, f"until battle 12 {alt}")
                if bat:
                    fight(session)
                    npos = do(session, "state")[0]
                    break
                if abs(npos[0] - pos[0]) + abs(npos[1] - pos[1]) >= 2:
                    break
            pos = npos
            if stuck >= 4:
                sys.exit(f"STUCK at {pos} heading {wp}")
        print(f"reached {wp} at {pos}", flush=True)
    print("DONE", pos)


if __name__ == "__main__":
    main()
