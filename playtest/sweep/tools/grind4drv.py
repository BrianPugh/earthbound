"""Driver: grind4 phases with check / verify / replay after every phase.

grind4drv.py SESSION DIR SAVE TARGET_LEVEL [MAX_PHASES]
"""

import json
import random
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path("/Users/brianpugh/projects/ebsrc")
S = Path(__file__).resolve().parent
PY = str(ROOT / ".venv/bin/python")
PT = [PY, str(ROOT / "playtest/pt.py")]
REC = [PY, str(ROOT / "playtest/sweep/rec.py")]
HOME = (447, 3102)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, **kw).stdout


def state(s):
    o = run(PT + ["do", s, "state", "--no-shot"])
    st = next((ln for ln in o.splitlines() if ln.startswith("STATE")), "")
    pa = re.search(r"PARTY 3:Jeff L(\d+) HP (\d+)/(\d+)", o)
    pos = re.search(r"pos=\((\d+),(\d+)\)", st)
    md = re.search(r"modes=\[([^\]]*)\]", st)
    return (md.group(1) if md else "", (int(pos.group(1)), int(pos.group(2))) if pos else None,
            tuple(map(int, pa.groups())) if pa else None, o)


def chunks(d):
    return sorted(d.glob("[0-9][0-9][0-9].pad"))


def sync(s, d, save):
    """autoalign + verify; replay onto the late pads if needed. Returns verify word."""
    tot = sum(len(c.read_text().split()) for c in chunks(d))
    env = dict(__import__("os").environ, AA_MIN=str(max(0, tot - 8000)))
    print("  align:", run([PY, str(S / "autoalign.py"), str(d), "12"], env=env).strip().splitlines()[-1][:120], flush=True)
    v = run(REC + [str(d), "verify", s]).strip().splitlines()[-1]
    if v != "SAME":
        run(PT + ["stop", s])
        run(REC + [str(d), "replay", s, save])
        run(PT + ["do", s, "padlog off", "--no-shot"])
    return v


def sane(md, party, phase):
    if party is None or "GAME_OVER" in md or party[1] == 0:
        return False
    if phase == "fight":
        return "BATTLE" not in md
    return md == "OVERWORLD" or "BATTLE_ENTRY" in md or "INSTANT_WIN" in md


def main():
    s, d, save, target = sys.argv[1], Path(sys.argv[2]), sys.argv[3], int(sys.argv[4])
    maxp = int(sys.argv[5]) if len(sys.argv) > 5 else 500
    tries = 0
    for _ in range(maxp):
        if (S / "STOP").exists():
            print("STOPPED", flush=True)
            return
        md, pos, party, o0 = state(s)
        if party is None:
            run(PT + ["stop", s])
            run(REC + [str(d), "replay", s, save])
            run(PT + ["do", s, "padlog off", "--no-shot"])
            md, pos, party, o0 = state(s)
        lv, hp, mx = party
        q, h = json.loads((d / "grind_state.json").read_text()) if (d / "grind_state.json").exists() else ([], [])
        if lv >= target and "BATTLE" not in md:
            print("TARGET", party, flush=True)
            return
        if "BATTLE" in md or "INSTANT_WIN" in md or "idle=battle" in o0:
            phase = "fight"
        elif hp < 0.8 * mx and pos and abs(pos[0] - HOME[0]) <= 4 and abs(pos[1] - HOME[1]) <= 4:
            phase = "heal"
        else:
            if hp < 0.8 * mx:
                loop = [(216, 3102), (216, 2904), (259, 2790), (282, 2766), (381, 2766), (404, 2790), (504, 2794)]
                i = min(range(len(loop)), key=lambda k: abs(pos[0] - loop[k][0]) + abs(pos[1] - loop[k][1]))
                q = [list(loop[k]) for k in range(i, -1, -1)] + [list(HOME)]
                if tries % 2 == 1:
                    q = [list(HOME)]  # the plateau east of the left column: straight home
                (d / "grind_state.json").write_text(json.dumps([q, []]))
            phase = "walk"
        before = len(chunks(d))
        gs = (d / "grind_state.json").read_text() if (d / "grind_state.json").exists() else None
        lead = 0 if tries == 0 else random.randint(3, 30)
        env = dict(__import__("os").environ)
        if phase == "fight" and tries >= 3:
            env["RUN"] = "1"
        out = run([PY, str(S / "grind4.py"), s, str(d), phase, str(lead)], env=env)
        res = [ln for ln in out.splitlines() if ln.startswith("RESULT")]
        print(f"{phase} L{lv} HP {hp}/{mx} lead {lead}: {res[-1] if res else out[-200:]}", flush=True)
        if phase == "heal" and res and "retry" in res[-1]:
            pass
        v = sync(s, d, save)
        md2, pos2, party2, _ = state(s)
        healed = phase == "heal" and party2 and party2[1] == party2[2] and md2 == "OVERWORLD"
        if not healed and ((res and "retry" in res[-1]) or not sane(md2, party2, phase)):
            print(f"  {v}; off route ({md2} {party2}): cut the phase, retry", flush=True)
            for c in chunks(d)[before:]:
                c.unlink()
            if gs is not None:
                (d / "grind_state.json").write_text(gs)
            run([PY, str(S / "prunehead.py"), str(d)])
            run(PT + ["stop", s])
            run(REC + [str(d), "check"])
            run(REC + [str(d), "replay", s, save])
            run(PT + ["do", s, "padlog off", "--no-shot"])
            print("  after cut:", run(REC + [str(d), "verify", s]).strip().splitlines()[-1], flush=True)
            tries += 1
            if tries > 12:
                sys.exit("too many retries")
            continue
        tries = 0
        print(f"  {v} -> {md2} {pos2} {party2}", flush=True)


if __name__ == "__main__":
    main()
