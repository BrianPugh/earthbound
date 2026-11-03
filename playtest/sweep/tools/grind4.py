"""One grinding phase per call, recorded as one padlog chunk (the driver checks/replays between).

grind4.py SESSION DIR PHASE [LEAD]
PHASE walk : walk the queue (terrace loop / back home) until a battle starts or the loop is done
PHASE fight: fight the battle in progress (Auto Fight; full-inventory prompts answered)
PHASE heal : walk home -> tent -> cook -> sleep -> out -> home
State (queue of waypoints, history since HOME) lives in DIR/grind_state.json.
Prints RESULT lines the driver parses.
"""

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import grind2 as g  # noqa: E402

HOME = (447, 3102)
LOOP = [(216, 3102), (216, 2904), (259, 2790), (282, 2766), (381, 2766), (404, 2790), (504, 2794)]


class Battle(Exception):
    pass


def load(d):
    p = d / "grind_state.json"
    if not p.exists():
        return [], []
    q, h = json.loads(p.read_text())
    return [x if x == "HEAL" else tuple(x) for x in q], [tuple(x) for x in h]


def save(d, q, h):
    (d / "grind_state.json").write_text(json.dumps([q, h]))


def main():
    s, d, phase = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
    lead = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    q, h = load(d)
    g.do(s, f"padlog {g.chunk(d)}" + (f"; wait {lead}" if lead else ""))
    try:
        if phase == "fight":
            o = g.do(s, "until input 1500 advance3", 1500).replace("UNTIL TIMEOUT", "UNTIL waited")
            en = re.findall(r"ENEMIES (.*)", o)
            if "BATTLE_MENU" in g.modes(o) and __import__("os").environ.get("RUN"):
                for _ in range(6):  # Run Away until it works (Auto Fight keeps losing this one)
                    o += g.do(s, "wait 10; 6 RIGHT; 8 -; 6 RIGHT; 8 -; 6 DOWN; 8 -; 6 A; 8 -; until input 3000 advance3")
                    if "BATTLE_MENU" not in g.modes(o):
                        break
                o += g.finish(s, auto="BATTLE_MENU" in g.modes(o))
            else:
                o += g.finish(s, auto="BATTLE_MENU" in g.modes(o))
            p = g.party(o + g.do(s, "state"))
            print(f"RESULT fight {en[-1][:40] if en else '?'} L{p[0]} HP {p[1]}/{p[2]}", flush=True)
        elif phase == "heal":
            g.heal(s)
            print("RESULT healed", flush=True)
        else:
            orig = g.fight

            def stop(_s):
                raise Battle()

            g.fight = stop
            pos = g.go.do(s, "state")[0]
            if 200 <= pos[0] <= 262 and 2995 <= pos[1] <= 3075 and q and tuple(q[0]) != (216, 2904):
                # the slope east of the left column (battles push Jeff there): the way down
                # is round its west edge (BFS route)
                # BFS route off the slope (from 206,3064: RIGHT, DOWN, DOWN+RIGHT, then east)
                g.do(s, "60 LEFT; 60 DOWN; 16 RIGHT; 16 DOWN; 16 DOWN+RIGHT; 16 RIGHT")
                pos = g.go.do(s, "state")[0]
            pts = LOOP + [HOME]
            if min(abs(pos[0] - x) + abs(pos[1] - y) for x, y in pts) > 60:
                # off the terrace loop (a late-pads run left the cook route part-way): back
                # along the cook route first
                i = min(range(len(g.FROM_COOK)), key=lambda k: abs(pos[0] - g.FROM_COOK[k][0]) + abs(pos[1] - g.FROM_COOK[k][1]))
                q = list(g.FROM_COOK[i:]) + q
            if not q:
                q = LOOP + list(reversed(LOOP[:-1])) + [HOME]
            try:
                while q and q[0] != "HEAL":
                    g.walk(s, [q[0]])
                    wp = q.pop(0)
                    if wp == HOME:
                        h = []
                    elif len(h) >= 2 and h[-2] == wp:
                        h.pop()
                    else:
                        h.append(wp)
                print("RESULT walked", flush=True)
            except Battle:
                print("RESULT battle", flush=True)
            finally:
                g.fight = orig
            save(d, q, h)
    except g.Retry as e:
        print(f"RESULT retry {e}", flush=True)
    finally:
        g.do(s, "padlog off")


if __name__ == "__main__":
    main()
