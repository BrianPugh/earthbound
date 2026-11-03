# ruff: noqa  (scratch recording helpers, kept as used; see README.md)
"""Best-first route search over a pt.py session: bfs.py SESSION GX GY [STEP] [MAXN]
From the current state (snapped as bfs0), tries 8 directions x STEP frames from each node,
dedupes on a 12 px grid, expands nodes nearest the goal first. Prints the step list."""

import heapq
import re
import subprocess
import sys

PT = ["/Users/brianpugh/projects/ebsrc/.venv/bin/python", "/Users/brianpugh/projects/ebsrc/playtest/pt.py"]
DIRS = ["UP", "DOWN", "LEFT", "RIGHT", "UP+LEFT", "UP+RIGHT", "DOWN+LEFT", "DOWN+RIGHT"]


def do(s, cmd):
    out = subprocess.run(PT + ["do", s, cmd, "--no-shot", "--quiet"], capture_output=True, text=True).stdout
    p = re.findall(r"pos=\((\d+),(\d+)\)", out)
    return (int(p[-1][0]), int(p[-1][1])) if p else None, out


def main():
    s, gx, gy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    step = int(sys.argv[4]) if len(sys.argv) > 4 else 20
    maxn = int(sys.argv[5]) if len(sys.argv) > 5 else 600
    pos, _ = do(s, "flag 11 1; state")
    seen = {(pos[0] // 12, pos[1] // 12)}
    nodes = {0: (pos, [])}
    heap = [(abs(gx - pos[0]) + abs(gy - pos[1]), 0)]
    nid = 1
    while heap and nid < maxn:
        _, i = heapq.heappop(heap)
        p0, path = nodes[i]
        for d in DIRS:
            np, out = do(s, f"restore bfs{i}; {step} {d}; state")
            if not np or "DOOR" in out or "TELEPORT" in out:
                continue
            k = (np[0] // 12, np[1] // 12)
            if k in seen:
                continue
            seen.add(k)
            do(s, f"snap bfs{nid}")
            nodes[nid] = (np, path + [f"{step} {d}"])
            dist = abs(gx - np[0]) + abs(gy - np[1])
            if dist < 16:
                print("FOUND", np, "; ".join(nodes[nid][1]), flush=True)
                return
            heapq.heappush(heap, (dist, nid))
            nid += 1
        if nid % 50 < 8:
            best = min(heap)[0] if heap else -1
            print(f"n={nid} best={best}", flush=True)
    b = min(heap)
    print("BEST", nodes[b[1]][0], "; ".join(nodes[b[1]][1]))


if __name__ == "__main__":
    main()
