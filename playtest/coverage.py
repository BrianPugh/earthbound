#!/usr/bin/env python3
"""Source coverage of the C port over the autodiff suite and the regress cases.

    uv run --no-sync python playtest/coverage.py build            # instrumented 256x224 build
    uv run --no-sync python playtest/coverage.py run [-j N]       # autodiff suite + regress cases
    uv run --no-sync python playtest/coverage.py report [--md F]  # per-function summary

``build`` configures ``port/unix/build-cov`` with Homebrew clang (``-fprofile-instr-generate
-fcoverage-mapping``, ``EB_WERROR=OFF``: newer clang warns where Apple clang doesn't). ``run`` plays
every ``playtest/autodiff/*.play`` (port and emu side, as usual) and records the regress cases with
that binary, each port process writing ``playtest/out/coverage/prof/*.profraw``; ``--only
autodiff|regress`` picks one, ``--keep`` adds to the profiles of an earlier run. ``report`` merges
them and prints executed/total functions and lines per ``src/`` subsystem, the never-executed game
functions (debug, platform, selftest and tool code left out) and the barely-executed ones, largest
first. ``--md FILE`` writes the same as Markdown. Needs ``brew install llvm``.
"""

# ruff: noqa: S603, S607  (local tools with fixed arguments)
import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BUILD = ROOT / "port" / "unix" / "build-cov"
BIN = BUILD / "earthbound"
OUT = Path(os.environ.get("EB_COV_OUT", HERE / "out" / "coverage"))
PROF = OUT / "prof"
LLVM = Path(os.environ.get("LLVM_BIN", "/opt/homebrew/opt/llvm/bin"))

# Code no player reaches: debug tools, hosts, self-tests, tools, platform glue.
EXCLUDE_PATH = re.compile(r"(^|/)(debug|tools|verify|platform|vendor|selftest)[/_]|state_dump|selftest|_debug|debug_")
EXCLUDE_NAME = re.compile(r"debug|selftest|dump|trace|profile|verify|^test_|_test$")


def build(_args):
    cflags = "-fprofile-instr-generate -fcoverage-mapping"
    subprocess.run(
        [
            "cmake",
            "-S",
            str(ROOT / "port" / "unix"),
            "-B",
            str(BUILD),
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_C_COMPILER={LLVM / 'clang'}",
            f"-DCMAKE_C_FLAGS={cflags}",
            "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate",
            "-DEB_VIEWPORT_WIDTH=256",
            "-DEB_VIEWPORT_HEIGHT=224",
            "-DEB_WERROR=OFF",
        ],
        check=True,
    )
    subprocess.run(["cmake", "--build", str(BUILD), "-j", str(os.cpu_count() or 4)], check=True)


def run(args):
    if not args.keep and PROF.exists():
        shutil.rmtree(PROF)
    PROF.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, LLVM_PROFILE_FILE=str(PROF / "%p.profraw"))
    py = [sys.executable]
    if args.only in (None, "autodiff"):
        plays = sorted(str(p) for p in (HERE / "autodiff").glob("*.play"))
        cmd = py + [str(HERE / "autodiff.py"), "--bin", str(BIN), "-j", str(args.j), "--out", str(OUT / "autodiff")]
        subprocess.run(cmd + plays, env=env, cwd=ROOT)
    if args.only in (None, "regress"):
        cmd = py + [str(HERE / "regress.py"), "record", "coverage", "--bin", str(BIN), "-j", str(args.j)]
        subprocess.run(cmd, env=env, cwd=ROOT)


def load_functions():
    raws = sorted(PROF.glob("*.profraw"))
    if not raws:
        sys.exit(f"no profiles in {PROF}; run `coverage.py run` first")
    data = OUT / "merged.profdata"
    subprocess.run(
        [str(LLVM / "llvm-profdata"), "merge", "-sparse", "-o", str(data)] + list(map(str, raws)), check=True
    )
    rep = subprocess.run(
        [str(LLVM / "llvm-cov"), "report", "-show-functions", str(BIN), f"-instr-profile={data}", str(ROOT / "src")],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    src = str(ROOT / "src") + "/"
    funcs, rel = [], None
    row = re.compile(r"^(\S+)\s+(\d+)\s+(\d+)\s+\S+\s+(\d+)\s+(\d+)\s")
    for line in rep.splitlines():
        if line.startswith("File '"):
            rel = line[6:].rstrip("':")
            rel = rel[len(src) :] if rel.startswith(src) else None
            continue
        m = row.match(line)
        if not m or rel is None or m.group(1) in ("Name", "TOTAL"):
            continue
        regions, rmiss, lines, lmiss = map(int, m.group(2, 3, 4, 5))
        name = m.group(1).split(":")[-1]
        if rel.endswith(".h"):  # static inline helpers: one copy per includer, not game logic
            continue
        funcs.append({"file": rel, "name": name, "run": rmiss < regions, "lines": lines, "zero_lines": lmiss})
    return funcs


def excluded(f):
    return bool(EXCLUDE_PATH.search(f["file"]) or EXCLUDE_NAME.search(f["name"]))


def subsystem(rel):
    parts = rel.split("/")
    if len(parts) == 1:
        return "(top)"
    base = re.sub(
        r"_(actions|calc|targeting|psi|ui|cc|menus|movement|palette|screen|sprite|action|collision|"
        r"interaction|spawn|teleport)$",
        "",
        Path(parts[-1]).stem,
    )
    return f"{parts[0]}/{base}"


def report(args):
    funcs = load_functions()
    game = [f for f in funcs if not excluded(f)]
    out = []

    def tot(fs):
        n = len(fs)
        e = sum(1 for f in fs if f["run"])
        lines = sum(f["lines"] for f in fs)
        hit = sum(f["lines"] - f["zero_lines"] for f in fs)
        return n, e, lines, hit

    n, e, lines, hit = tot(funcs)
    gn, ge, gl, gh = tot(game)
    out.append("# C port coverage: autodiff suite + regress cases\n")
    out.append(
        f"All src/: {e}/{n} functions executed ({100 * e / n:.1f}%), {hit}/{lines} lines ({100 * hit / lines:.1f}%)."
    )
    out.append(
        f"Game code (debug/platform/selftest/tools excluded): {ge}/{gn} functions ({100 * ge / gn:.1f}%), "
        f"{gh}/{gl} lines ({100 * gh / gl:.1f}%).\n"
    )
    groups = defaultdict(list)
    for f in game:
        groups[subsystem(f["file"])].append(f)
    out.append("## Per subsystem\n")
    out.append("| subsystem | functions | lines | never-run lines |")
    out.append("|---|---|---|---|")
    rows = []
    for g, fs in groups.items():
        n, e, lines, hit = tot(fs)
        rows.append((lines - hit, g, n, e, lines, hit))
    for miss, g, n, e, lines, hit in sorted(rows, reverse=True):
        out.append(f"| {g} | {e}/{n} | {hit}/{lines} ({100 * hit / max(lines, 1):.0f}%) | {miss} |")
    out.append("\n## Never-executed game functions (largest first, per subsystem)\n")
    for _, g, *_ in sorted(rows, reverse=True):
        never = sorted((f for f in groups[g] if not f["run"]), key=lambda f: -f["lines"])
        if not never:
            continue
        out.append(f"### {g} ({len(never)})\n")
        out.append(", ".join(f"`{f['name']}` {f['file']} ({f['lines']})" for f in never))
        out.append("")
    out.append("## Barely-executed game functions (run, but most lines never)\n")
    barely = [f for f in game if f["run"] and f["lines"] >= 40 and f["zero_lines"] > 0.6 * f["lines"]]
    for f in sorted(barely, key=lambda f: -f["zero_lines"])[: args.top]:
        out.append(f"- `{f['name']}` {f['file']}: {f['zero_lines']}/{f['lines']} lines never run")
    text = "\n".join(out) + "\n"
    if args.md:
        Path(args.md).parent.mkdir(parents=True, exist_ok=True)
        Path(args.md).write_text(text)
        print(f"wrote {args.md}")
    else:
        print(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("build")
    s = sub.add_parser("run")
    s.add_argument("-j", type=int, default=4)
    s.add_argument("--only", choices=["autodiff", "regress"])
    s.add_argument("--keep", action="store_true", help="add to the existing profiles")
    s = sub.add_parser("report")
    s.add_argument("--md", help="write Markdown here")
    s.add_argument("--top", type=int, default=80, help="barely-executed functions listed")
    args = ap.parse_args()
    {"build": build, "run": run, "report": report}[args.cmd](args)


if __name__ == "__main__":
    main()
