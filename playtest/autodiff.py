"""Find where the port and the original game diverge, game-logic frame by frame.

Boots the same save on the port (``$EB_BIN``, a native 256x224 build) and on the
original ROM (emu.py), runs a scenario's setup on each, seeds the RNG on both,
then plays the scenario body one game-logic frame at a time: the port runs one
frame per input, the emu runs ``lstep`` (wall frames until the next game frame
completes, counted by a patched WAIT_UNTIL_NEXT_FRAME, so the original's lag
frames don't shift the inputs; a frame that completes inside a lag frame can't be
sampled and is listed as merged). After
every logic frame both sides print STATE/PARTY/NEAR/FOLLOW/ENEMIES, ``digest``
(event flags, items, music, every entity slot, open windows) and the RNG; the
report lists, per field, the first logic frame where the 2 sides differ.

Scenario file (the playtest command language)::

    # setup: run on both sides after the boot, not compared
    teleport 7557 7248 4
    until idle 600
    ---
    # body: fixed-length inputs only (N BUTTONS / wait N / press BUTTONS [COUNT])
    press A
    wait 30

A setup line prefixed ``port:`` or ``emu:`` runs on that side only.

Usage::

    uv run --no-sync python playtest/autodiff.py SAVE.srm SCENARIO.play [...] [--bin BIN]
"""

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from pt import BOOT, EMU_BOOT  # noqa: E402

DEFAULT_BIN = ROOT / "port" / "unix" / "build-native" / "earthbound"
SNAPSHOT = ["state", "digest", "rng"]
STATE_KEYS = (
    "idle",
    "input",
    "pos",
    "dir",
    "music",
    "money",
    "battle",
    "ow_enemies",
    "blink",
    "surface",
    "walk",
    "party_status",
)
ENT = re.compile(r"(\d+):(-?\d+)@(-?\d+,-?\d+)/(-?\d+)/(-?\d+)")  # digest ents: slot:script@x,y/dir/sprite
WIN = re.compile(r"\d+:\d+,\d+,\d+x\d+")  # digest wins: id:x,y,WxH
# The emu's FOLLOW has no possession ghost
GHOST = re.compile(r" ghost:\([^)]*\)\w*")


def expand_body(lines):
    """Expand fixed-length input commands into one BUTTONS string per logic frame."""
    pads = []
    for raw in lines:
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        cmd, args = parts[0], parts[1:]
        if cmd.isdigit():
            pads += [args[0] if args else "-"] * int(cmd)
        elif cmd == "wait":
            pads += ["-"] * int(args[0])
        elif cmd == "press":
            for _ in range(int(args[1]) if len(args) > 1 else 1):
                pads += [args[0]] * 2 + ["-"] * 4
        else:
            sys.exit(f"body line not fixed-length: {raw!r} (only N BUTTONS / wait N / press)")
    return pads


def side_lines(lines, side):
    """Setup lines for one side (``port:``/``emu:`` prefixes select a side)."""
    out = []
    for raw in lines:
        line = raw.strip()
        for tag in ("port:", "emu:"):
            if line.startswith(tag):
                line = line[len(tag) :].strip() if tag == side + ":" else ""
        if line:
            out.append(line)
    return out


def build_script(side, setup, pads, seed):
    """The whole input script for one side."""
    boot = (BOOT if side == "port" else EMU_BOOT).split(";")
    step = "1 {}" if side == "port" else "lstep {}"
    out = [b.strip() for b in boot] + side_lines(setup, side) + [f"rng {seed[0]} {seed[1]}"]
    if side == "emu":
        out.append("lstep reset")
    out += SNAPSHOT
    for pad in pads:
        out.append(step.format(pad))
        out += SNAPSHOT
    if side == "emu":
        out.append("lag")
    out.append("end")
    return "\n".join(out) + "\n"


def parse_snapshots(text):
    """Split a run's output into {logic frame: field dict} (from the seeding RNG line on).

    Snapshots are numbered in order, or by the preceding ``LSTEP N`` line (emu);
    an index seen twice (an lstep that had nothing left to run) keeps the first.
    """
    snaps, cur, seeded, idx = {}, None, False, 0
    for line in text.splitlines():
        if line.startswith("RNG ") and not seeded:
            seeded = True
            continue
        if not seeded:
            continue
        if line.startswith("LSTEP "):
            idx = int(line.split()[1])
            continue
        if line.startswith("STATE "):
            cur = {}
            m = re.search(r" idle=(\S+)", line)
            for k, v in re.findall(r" (\w+)=(\([^)]*\)|\S+)", line):
                if k in STATE_KEYS:
                    cur[k] = v
            if m:
                cur["idle"] = "yes" if m.group(1) == "yes" else "no"
            cur["raw_state"] = line
        elif cur is None:
            continue
        elif line.startswith(("PARTY", "NEAR", "ENEMIES")):
            key, _, rest = line.partition(" ")
            cur[key.lower()] = rest
        elif line.startswith("FOLLOW"):
            cur["follow"] = GHOST.sub("", line[7:]).strip()
        elif line.startswith("DIGEST "):
            for k, v in re.findall(r" (\w+)=(\S*)", line):
                if k == "ents":
                    for slot, script, pos, d, spr in ENT.findall(v):
                        cur[f"ent{slot}.script"] = script
                        cur[f"ent{slot}.pos"] = pos
                        cur[f"ent{slot}.dir"] = d
                        cur[f"ent{slot}.sprite"] = spr
                elif k == "wins":
                    cur["wins"] = " ".join(sorted(WIN.findall(v), key=lambda w: int(w.split(":")[0])))
                else:
                    cur[k] = v
        elif line.startswith("RNG "):
            cur["rng"] = line[4:].strip()
            snaps.setdefault(idx, cur)
            idx += 1
            cur = None
    return snaps


def flag_delta(a, b):
    """Event flag numbers set on one side only, from 2 hex flag strings."""
    out = []
    for i in range(0, min(len(a), len(b)), 2):
        x = int(a[i : i + 2], 16) ^ int(b[i : i + 2], 16)
        for bit in range(8):
            if x >> bit & 1:
                n = (i // 2) * 8 + bit + 1
                out.append(f"{n}:{'port' if int(a[i : i + 2], 16) >> bit & 1 else 'emu'}")
    return " ".join(out)


def show(field, a, b):
    """One field's port/emu values for the report."""
    if field == "flags":
        return f"flags set on one side only: {flag_delta(a or '', b or '')}"
    return f"port={a if a is not None else '(none)'}  emu={b if b is not None else '(none)'}"


def compare(port, emu, pads, ignore):
    """Return (frames compared, {field: first differing frame}, report lines)."""
    frames = sorted(set(port) & set(emu))
    n = len(frames)
    first = {}
    count = {}
    for i in frames:
        p, e = port[i], emu[i]
        for k in sorted(set(p) | set(e)):
            if k == "raw_state" or any(re.fullmatch(pat, k) for pat in ignore):
                continue
            if p.get(k) != e.get(k):
                count[k] = count.get(k, 0) + 1
                first.setdefault(k, i)
    lines = []
    for k in sorted(first, key=lambda k: (first[k], k)):
        i = first[k]
        pad = pads[i - 1] if i else "(start)"
        lines.append(
            f"  frame {i:4d} (input {pad}) {k}: {show(k, port[i].get(k), emu[i].get(k))}  [{count[k]}/{n} frames]"
        )
    return n, first, lines


def run_side(cmd, script_path, log_path):
    """Start one side (stdout to log_path); return the Popen."""
    with Path(log_path).open("w") as log:
        return subprocess.Popen(  # noqa: S603
            cmd + ["--input", str(script_path)], stdout=log, stderr=subprocess.STDOUT, cwd=ROOT, text=True
        )


def main():
    """Command-line entry point."""
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("save", help=".srm save (booted as file 1 -> Continue on both sides)")
    ap.add_argument("scenarios", nargs="+", help="scenario .play files (setup --- body)")
    ap.add_argument("--bin", default=os.environ.get("EB_BIN", str(DEFAULT_BIN)), help="port binary (256x224 build)")
    ap.add_argument("--rng", nargs=2, type=int, default=(4660, 22136), metavar=("A", "B"), help="RNG seed")
    ap.add_argument(
        "--ignore", action="append", default=["input"], help="regex of field names to skip (repeatable; default: input)"
    )
    ap.add_argument("--out", default=str(HERE / "out" / "autodiff"), help="output dir (scripts, logs)")
    ap.add_argument("--timeout", type=float, default=300, help="seconds per side")
    a = ap.parse_args()

    cache = Path(a.bin).resolve().parent / "CMakeCache.txt"
    if cache.exists() and "EB_VIEWPORT_WIDTH:STRING=256" not in cache.read_text():
        print(f"warning: {a.bin} is not a 256x224 build: enemy spawns will differ", file=sys.stderr)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    save = str(Path(a.save).resolve())
    sides = {
        "port": [str(Path(a.bin).resolve()), "--headless", "--save", save, "--record-audio", "/dev/null"],
        "emu": [sys.executable, str(HERE / "emu.py"), "--save", save, "--count-frames"],
    }
    worst = 0
    for scen in a.scenarios:
        name = Path(scen).stem
        text = [c for raw in Path(scen).read_text().splitlines() for c in raw.split("#", 1)[0].split(";")]
        if "---" in [t.strip() for t in text]:
            cut = [t.strip() for t in text].index("---")
            setup, body = text[:cut], text[cut + 1 :]
        else:
            setup, body = [], text
        pads = expand_body(body)
        procs = {}
        t0 = time.time()
        for side, cmd in sides.items():
            script = out / f"{name}.{side}.play"
            script.write_text(build_script(side, setup, pads, a.rng))
            procs[side] = run_side(cmd, script, out / f"{name}.{side}.log")
        for side, p in procs.items():
            try:
                p.wait(timeout=max(1.0, a.timeout - (time.time() - t0)))
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
                print(f"{name}: {side} timed out", file=sys.stderr)
        res = {s: parse_snapshots((out / f"{name}.{s}.log").read_text(errors="replace")) for s in sides}
        lag = re.findall(r"^LAG .*", (out / f"{name}.emu.log").read_text(errors="replace"), re.M)
        n, first, lines = compare(res["port"], res["emu"], pads, a.ignore)
        status = "identical" if not first else f"first divergence at logic frame {min(first.values())}"
        print(f"== {name}: {len(pads)} logic frames, compared {n} ({status}; {lag[-1] if lag else 'no LAG line'})")
        merged = sorted(set(range(len(pads) + 1)) - set(res["emu"]))
        if merged:
            print(f"  emu frames merged by lag (not compared): {merged}")
        if len(res["port"]) < len(pads) + 1 or (res["emu"] and max(res["emu"]) < len(pads)):
            print(f"  warning: snapshots port={len(res['port'])} emu={len(res['emu'])}, expected {len(pads) + 1}")
        for line in lines:
            print(line)
        print(f"  scripts/logs: {out}/{name}.{{port,emu}}.{{play,log}}")
        worst = max(worst, 1 if first else 0)
    return worst


if __name__ == "__main__":
    sys.exit(main())
