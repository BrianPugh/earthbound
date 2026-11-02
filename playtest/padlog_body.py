"""Turn a port ``padlog`` file into a fixed-length autodiff scenario body.

Plan a scenario on the port with ``until`` commands (``until input 3000 advance`` and
so on), put ``rng A B; fc 0; padlog FILE`` where the autodiff seed goes, then run
this on FILE: it prints the pads as run-length ``N BUTTONS`` lines that replay the
port run frame for frame (autodiff.py feeds the same pads to the original).

Usage::

    uv run --no-sync python playtest/padlog_body.py FILE [--width 10]
"""

import argparse
from pathlib import Path

BITS = [
    ("B", 15),
    ("Y", 14),
    ("SELECT", 13),
    ("START", 12),
    ("UP", 11),
    ("DOWN", 10),
    ("LEFT", 9),
    ("RIGHT", 8),
    ("A", 7),
    ("X", 6),
    ("L", 5),
    ("R", 4),
]


def pad_name(v):
    names = [n for n, b in BITS if v >> b & 1]
    return "+".join(names) if names else "-"


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
    ap.add_argument("file", help="padlog output (one hex pad word per frame)")
    ap.add_argument("--width", type=int, default=10, help="runs per output line")
    a = ap.parse_args()
    pads = [pad_name(int(t, 16)) for t in Path(a.file).read_text().split()]
    items = [f"{n} {p}" for n, p in runs(pads)]
    for i in range(0, len(items), a.width):
        print("; ".join(items[i : i + a.width]))


if __name__ == "__main__":
    main()
