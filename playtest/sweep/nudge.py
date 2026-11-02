"""Move pad changes off the frame after an emu lag-merged frame, then rerun autodiff.

A frame that completes inside one of the original's lag frames can't be sampled (merged),
and the frame after it reads the pad of the one before. A padlog body whose direction
change (or button press) lands there reaches the original a frame late, and the walk drifts
a pixel apart. This rewrites such a pad change one frame later (the same input, one frame
later: harmless for the route) for every merged frame before the first divergence, reruns
the scenario, and repeats until nothing is left to move. It edits the scenario in place.

Usage::

    uv run --no-sync python playtest/sweep/nudge.py SCENARIO.play [--iters N] [--out DIR]
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))
from autodiff import expand_body  # noqa: E402


def runs(pads):
    out = []
    for p in pads:
        if out and out[-1][1] == p:
            out[-1][0] += 1
        else:
            out.append([1, p])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario")
    ap.add_argument("--iters", type=int, default=8)
    ap.add_argument("--out", default=str(ROOT / "playtest/out/autodiff-nudge"))
    ap.add_argument("--bin", help="port binary (autodiff.py --bin)")
    a = ap.parse_args()
    scen = Path(a.scenario)
    for _ in range(a.iters):
        lines = scen.read_text().splitlines()
        cut = lines.index("---")
        head, body = lines[: cut + 1], lines[cut + 1 :]
        pads = expand_body([c for line in body for c in line.split(";")])
        cmd = [sys.executable, str(HERE.parent / "autodiff.py"), str(scen), "--out", a.out]
        if a.bin:
            cmd += ["--bin", a.bin]
        rep = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT).stdout  # noqa: S603
        print(rep.splitlines()[0] if rep else "no output", flush=True)
        fd = re.search(r"first divergence at logic frame (\d+)", rep)
        if not fd:
            break
        first = int(fd.group(1))
        m = re.search(r"merged by lag \(not compared\): \[([^\]]*)\]", rep)
        merged = [int(x) for x in m.group(1).split(",")] if m and m.group(1).strip() else []
        moved = 0
        for k in merged:  # frame k+1 plays pads[k] and the original reads pads[k-1] there
            if k > first:
                break
            if 1 <= k < len(pads) and pads[k] != pads[k - 1]:
                pads[k] = pads[k - 1]
                moved += 1
        print(f"  first divergence {first}, moved {moved} pad changes", flush=True)
        if not moved:
            print("\n".join(rep.splitlines()[1:14]))
            break
        items = [f"{n} {p}" for n, p in runs(pads)]
        scen.write_text("\n".join(head + ["; ".join(items[i : i + 10]) for i in range(0, len(items), 10)]) + "\n")


if __name__ == "__main__":
    main()
