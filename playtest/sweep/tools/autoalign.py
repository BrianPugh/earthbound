"""autoalign.py DIR [MAX]: run rec.py check, and for a door-load divergence add the usual
skip K / lag emu(K+20) pair, for an idle-only 1-frame divergence an `ignore: idle K` line;
repeat. Stops at anything else (prints the report head)."""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path("/Users/brianpugh/projects/ebsrc")
PY = str(ROOT / ".venv/bin/python")
REC = [PY, str(ROOT / "playtest/sweep/rec.py")]


def check(d):
    return subprocess.run(REC + [d, "check"], capture_output=True, text=True, cwd=ROOT).stdout


def emu_frame(d, n):
    out = subprocess.run(REC + [d, "emu-frame", str(n)], capture_output=True, text=True, cwd=ROOT).stdout
    return int(out.split()[-1])


def add(d, lines):
    p = Path(d) / "head.play"
    s = p.read_text()
    i = s.index("\n---")
    p.write_text(s[:i] + "".join("\n# " + ln for ln in lines) + s[i:])


def in_transition(d, k):
    """The port is in a screen transition at frame k (a door / TELEPORT load)."""
    n = 0
    for ln in open(Path(d) / "ad/scenario.port.log", errors="replace"):
        if ln.startswith("STATE"):
            n += 1
            if n >= k - 2 and "SCREEN_TRANSITION" in ln:
                return True
            if n > k + 1:
                return False
    return False


def main():
    d = sys.argv[1]
    mx = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    last = None
    for _ in range(mx):
        rep = check(d)
        if "timed out" in rep:
            print("TIMED OUT", rep.splitlines()[:3])
            return 3
        head = rep.splitlines()[0] if rep else ""
        m = re.search(r"first divergence at logic frame (\d+)", head)
        if not m:
            print(head)
            return 0
        k = int(m.group(1))
        lo = int(__import__("os").environ.get("AA_MIN", "0"))
        if k < lo:
            print(f"REFUSE {k} < {lo}", head)
            return 4
        lines = [ln for ln in rep.splitlines() if ln.strip().startswith(f"frame {k:>5}") or re.match(rf"\s*frame\s+{k} ", ln)]
        fields = {re.sub(r"^\s*frame\s+\d+ \(input [^)]*\) ", "", ln).split(":")[0] for ln in lines}
        if k == last:
            print("REPEAT", head)
            print("\n".join(rep.splitlines()[:12]))
            return 1
        last = k
        if fields <= {"idle"}:
            add(d, [f"ignore: idle {k}"])
            print(f"idle {k}")
        elif "ent26.pos" in fields or "ent28.pos" in fields or "bgm" in fields or in_transition(d, k):
            add(d, [f"skip: {k}", f"lag: {emu_frame(d, k + 20)}"])
            print(f"door {k}")
        else:
            print("OTHER", head)
            print("\n".join(rep.splitlines()[:14]))
            return 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
