# ruff: noqa  (scratch recording helpers, kept as used; see README.md)
"""Grind real battles in a rec.py recording session, one padlog chunk per walked leg.

grind2.py SESSION DIR TARGET_LEVEL
Walks the LOOP legs back and forth. Before each leg: padlog off, snap, padlog <next chunk>.
A battle against an AVOID enemy while Jeff is below AVOID_BELOW, or a lost battle, restores
the snap and redoes the leg after a lead-in wait (other pads, other spawns / RNG); the
recorded chunk is only ever the attempt that went through. Heals at the cook (HEAL route)
when HP falls below HEAL_AT of max after a leg.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path("/Users/brianpugh/projects/ebsrc")
sys.path.insert(0, str(ROOT / "playtest/sweep"))
import go  # noqa: E402

PT = [str(ROOT / ".venv/bin/python"), str(ROOT / "playtest/pt.py")]
AVOID = ("Gruff Goat",)
AVOID_BELOW = 0
HEAL_AT = 0.8
LEGS = [
    [(447, 3102), (216, 3102), (216, 2904), (259, 2790), (282, 2766), (381, 2766), (404, 2790), (504, 2794)],
    [(404, 2790), (381, 2766), (282, 2766), (259, 2790), (216, 2904), (216, 3102), (447, 3102)],
]
TO_COOK = [
    (470, 3125),
    (601, 3128),
    (624, 3105),
    (786, 3096),
    (791, 3104),
    (792, 3127),
    (782, 3150),
    (768, 3183),
    (675, 3276),
    (672, 3374),
    (606, 3374),
    (540, 3374),
    (536, 3352),
    (404, 3352),
    (390, 3369),
    (346, 3369),
]
FROM_COOK = [
    (390, 3369),
    (404, 3352),
    (536, 3352),
    (540, 3374),
    (606, 3374),
    (672, 3374),
    (675, 3276),
    (768, 3183),
    (792, 3127),
    (791, 3104),
    (786, 3096),
    (624, 3105),
    (601, 3128),
    (470, 3125),
    (447, 3102),
]


class Retry(Exception):
    pass


def do(s, cmd, timeout=900):
    return subprocess.run(
        PT + ["do", s, cmd, "--no-shot", "--timeout", str(timeout)], capture_output=True, text=True, cwd=ROOT
    ).stdout


def party(out):
    m = re.findall(r"PARTY 3:Jeff L(\d+) HP (\d+)/(\d+)", out)
    return tuple(map(int, m[-1])) if m else None


def modes(out):
    m = re.findall(r"modes=\[([^\]]*)\]", out)
    return m[-1] if m else ""


def finish(s, auto=True):
    """Auto Fight to the end of the battle; answer No to a full-inventory throw-away prompt."""
    o = do(s, "wait 10; 6 RIGHT; 8 -; 6 RIGHT; 8 -; 6 A; 8 -", 300) if auto else ""
    for _ in range(20):
        r = do(s, "until out:INSTANT_WIN 600 advance3; until out:BATTLE 600 advance3", 1500)
        o += r.replace("UNTIL TIMEOUT", "UNTIL waited")
        md = modes(r)
        if "BATTLE" not in md and "INSTANT_WIN" not in md:
            return o + do(s, "wait 20")
        t = do(s, "text")
        if "SELECTION_MENU" in modes(t):
            if "behind" in t:
                o += do(s, "wait 6; press A; wait 10")
            else:
                o += do(s, "wait 6; press RIGHT; wait 6; press A; wait 10")
    return o + "TIMEOUT"


def fight(s):
    o = do(s, "until input 1500 advance3", 1500).replace("UNTIL TIMEOUT", "UNTIL waited")
    en = re.findall(r"ENEMIES (.*)", o)
    lv = party(o + do(s, "state"))[0]
    if en and lv < AVOID_BELOW and any(a in en[-1] for a in AVOID):
        raise Retry(f"avoid {en[-1][:50]}")
    if "BATTLE_MENU" in modes(o):
        o += finish(s)
    else:
        o += finish(s, auto=False)
    if "GAME_OVER" in o or "unconscious" in o or "TIMEOUT" in o:
        raise Retry(f"lost to {en[-1][:50] if en else '?'}")
    p = party(o + do(s, "state"))
    print(f"   won vs {en[-1][:50] if en else '?'} -> L{p[0]} HP {p[1]}/{p[2]}", flush=True)


def walk(s, points):
    pos = go.do(s, "state")[0]
    for tx, ty in points:
        best, since = 1 << 30, 0
        stuck = 0
        while abs(tx - pos[0]) > 3 or abs(ty - pos[1]) > 3:
            dist = abs(tx - pos[0]) + abs(ty - pos[1])
            if dist < best:
                best, since = dist, 0
            else:
                since += 1
                if since >= 6:
                    raise Retry(f"NO PROGRESS at {pos} -> {tx},{ty}")
            d = go.heading(tx - pos[0], ty - pos[1])
            n = max(2, min(24, int(max(abs(tx - pos[0]), abs(ty - pos[1])) / 1.5)))
            npos, bat, md, _ = go.do(s, f"until battle {n} {d}")
            if bat or "BATTLE" in md:
                fight(s)
                pos = go.do(s, "state")[0]
                continue
            moved = abs(npos[0] - pos[0]) + abs(npos[1] - pos[1])
            pos = npos
            if moved >= 2:
                stuck = 0
                continue
            stuck += 1
            for alt in go.sidesteps(d):
                npos, bat, _, _ = go.do(s, f"until battle 12 {alt}")
                if bat:
                    fight(s)
                    npos = go.do(s, "state")[0]
                    break
                if abs(npos[0] - pos[0]) + abs(npos[1] - pos[1]) >= 2:
                    break
            pos = npos
            if stuck >= 4:
                raise Retry(f"STUCK at {pos}")


def chunk(d):
    return d / f"{len(sorted(d.glob('[0-9][0-9][0-9].pad'))):03d}.pad"


def leg(s, d, rnd, fn):
    nxt = chunk(d)
    do(s, f"padlog off; snap gr{rnd}")
    for attempt in range(16):
        do(s, f"padlog {nxt}" + (f"; wait {attempt * 11}" if attempt else ""))
        try:
            fn()
            do(s, "padlog off")
            return
        except Retry as e:
            print(f"  retry {attempt}: {e}", flush=True)
            do(s, f"padlog off; restore gr{rnd}")
    nxt.unlink()
    raise SystemExit("gave up")


def heal(s):
    pos = go.do(s, "state")[0]
    i = min(range(len(TO_COOK)), key=lambda k: abs(pos[0] - TO_COOK[k][0]) + abs(pos[1] - TO_COOK[k][1]))
    walk(s, TO_COOK[i:])
    do(s, "until in:DOOR_TRANSITION 60 UP; until out:DOOR_TRANSITION 300; wait 10")
    do(s, "30 DOWN")
    for k in range(120):
        if k and k % 30 == 0:
            do(s, "wait 60")
        m = re.search(r"npc612 \(([-+]\d+),([-+]\d+)\)", do(s, "state"))
        cx, cy = int(m.group(1)), int(m.group(2))
        if abs(cx) <= 18 and abs(cy) <= 8:
            break
        if abs(cy) > 8:
            do(s, "4 UP" if cy < 0 else "4 DOWN")
        else:
            do(s, "4 LEFT" if cx < 0 else "4 RIGHT")
    else:
        raise Retry("cook out of reach")
    do(
        s,
        ("press LEFT" if cx < 0 else "press RIGHT") if abs(cx) >= abs(cy) else ("press UP" if cy < 0 else "press DOWN"),
    )
    o = do(
        s,
        "press A; until input 60; wait 6; press A; until in:SELECTION_MENU 900 advance3; wait 10; press A; "
        "until out:DISPLAY_TEXT 4000 advance3; wait 30",
        1500,
    )
    if "TIMEOUT" in o or "Who are you" in o:
        raise Retry("heal failed")
    walk(s, [(7262, 1728)])
    o = do(s, "until in:DOOR_TRANSITION 60 RIGHT; until out:DOOR_TRANSITION 300; wait 10")
    if go.do(s, "state")[0][0] > 4000:
        raise SystemExit("tent exit failed")
    walk(s, FROM_COOK)


def main():
    s, d, target = sys.argv[1], Path(sys.argv[2]), int(sys.argv[3])
    maxlegs = int(sys.argv[4]) if len(sys.argv) > 4 else 200
    rnd = 100 * int(sys.argv[6]) if len(sys.argv) > 6 else 0
    first = int(sys.argv[7]) if len(sys.argv) > 7 else 0
    if len(sys.argv) > 5 and sys.argv[5] == "start":  # start: walk from the camp to the first leg
        leg(s, d, rnd, lambda: walk(s, FROM_COOK[4:]))
        rnd += 1
    for i in range(first, first + maxlegs):
        lv, hp, mx = party(do(s, "state"))
        print(f"leg {i}: L{lv} HP {hp}/{mx}", flush=True)
        if lv >= target:
            break
        if i % 2 == 0 and hp < HEAL_AT * mx:
            leg(s, d, rnd, lambda: heal(s))
            rnd += 1
        leg(s, d, rnd, lambda i=i: walk(s, LEGS[i % 2]))
        rnd += 1
    do(s, "padlog off")


if __name__ == "__main__":
    main()
