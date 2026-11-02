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

A setup line prefixed ``port:`` or ``emu:`` runs on that side only. A
``# save: NN-Name`` header line (as in the regress cases) names the save in
``playtest/saves/``, and ``# ignore: REGEX [A[-B]]`` lines skip fields for that scenario
(e.g. an NPC that wandered differently before the seed), on (port) logic frames A to B
only when a range is given; a SAVE.srm given first on the command line is the default
for scenarios without one. A ``# lag: K`` line marks emu logic frame K as one the
original spends on CPU time alone (a loop's extra WAIT_UNTIL_NEXT_FRAME after a lag
frame, which the port can't have): the emu gets the port's frame-K pad twice, frame
K isn't compared, and later emu frames are compared with port frame - 1. A ``# skip: K``
line drops (port) frame K from the comparison: the emu sampled it part-way into a long
load's lag frame. A ``# reseed: K [A B]`` line seeds the RNG (default: the --rng seed) on both
sides after (port) frame K's snapshot: for a lag-shifted stretch where the original's extra
frame also called RAND, so the 2 RNGs are one call apart from then on (giygas-pray.play). An
``# at: K debug on|off`` line runs that command on both sides after (port) frame K (a debug win
in the body, whose debug mode must be off again before the text that follows: paula-farewell.play). ``# save: blank`` boots an empty SRAM, and ``# boot: menu`` stops the
boot at the file select menu's first input wait instead of loading slot 1, so the
scenario can pick a slot, start a new game or name the party. ``# overclock: max`` runs
the emu with snes9x's CPU overclock, for places whose logic overruns every frame
(Moonside): there no frame boundary falls on a wall-frame boundary, so every sample
would be ``LSTEP mid``; with the faster CPU each frame fits and parks, like the port's. An
``# overclock-off: K`` line turns it off after (port) frame K (emu.py ``overclock disabled``),
for a scenario whose later part breaks under it (the credits hang overclocked: ending.play), and
``# overclock-on: K [MODE]`` turns it on (default ``max``) after (port) frame K, for a
scenario whose setup and start must run at the normal speed (NPC phases before the seed)
but whose later part lags (a town walk: playtest/sweep/).
``# boot: power`` plays the body from power-on (no boot commands, no seed: RESET seeds the
RNG with the default --rng). An ``# extra: K [K ...]`` line marks port frames the original
spends inside a load's lag (the mirror of ``# lag:``): they have no emu counterpart, the emu
doesn't get their pads, and later emu frames are compared with port frame + 1 for each.
``# compare: bright`` also compares the brightness each logic frame ended with (``frac``).

Lost frames: when the port binary is built with ``-DEB_LOST_FRAME_CHECK=ON``
(src/core/lost_frame_check.h), each ``LOST-FRAME`` line it prints (a frame whose inline
window tick or bare wait was never presented) is listed under the scenario and fails it,
even when every field matches. ``--port-only`` skips the emu and only runs that check
(fast: the whole suite in about a minute); a binary without the check fails it.

Usage::

    uv run --no-sync python playtest/autodiff.py [SAVE.srm] SCENARIO.play [...] [--bin BIN] [-j N]
    uv run --no-sync python playtest/autodiff.py playtest/autodiff/*.play   # the suite
"""

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import lost_frames  # noqa: E402
from pt import BOOT, EMU_BOOT  # noqa: E402

DEFAULT_BIN = ROOT / "port" / "unix" / "build-native" / "earthbound"
# ``# boot: menu``: stop at the file select menu's first input wait instead of loading
# slot 1, so a scenario can pick a slot, start a new game, name the party, ...
# (emu.py's FILE_MENU is an alias for FILE_SELECT_INIT)
MENU_BOOT = "until in:FILE_MENU 3000 START*; until input 600"
SNAPSHOT = ["state", "digest", "rng"]
# ``# compare: bright``: also the brightness each logic frame ended with (FRAC bright=)
SNAPSHOT_BRIGHT = ["state", "digest", "frac", "rng"]
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


def emu_pads(pads, lags, extras=()):
    """The emu's pads: the port's minus each ``# extra: K`` frame's, each ``# lag: K`` one repeated."""
    out = [p for n, p in enumerate(pads, 1) if n not in extras]
    for k in sorted(lags):
        out.insert(k - 1, out[k - 2] if k >= 2 else "-")
    return out


def emu_to_port(j, lags, extras=()):
    """The port frame emu logic frame j is compared with (None for an extra lag frame).

    The k-th emu frame that isn't a ``# lag:`` frame is compared with the k-th port
    frame that isn't an ``# extra:`` frame.
    """
    if j in lags:
        return None
    p = j - sum(1 for k in lags if k < j)
    for e in sorted(extras):
        if e > p:
            break
        p += 1
    return p


def build_script(
    side,
    setup,
    pads,
    seed,
    lags=(),
    boot_kind="load",
    reseeds=None,
    oc_off=(),
    at=None,
    extras=(),
    bright=False,
    oc_on=None,
):
    """The whole input script for one side."""
    snapshot = SNAPSHOT_BRIGHT if bright else SNAPSHOT
    reseeds = reseeds or {}
    if boot_kind == "power":
        # From power-on: no boot commands and no seed (RESET seeds RAND_A/RAND_B with
        # $56781234, the default --rng, and clears FRAME_COUNTER on both sides)
        boot = []
    else:
        boot = (MENU_BOOT if boot_kind == "menu" else BOOT if side == "port" else EMU_BOOT).split(";")
    step = "1 {}" if side == "port" else "lstep {}"
    out = [b.strip() for b in boot] + side_lines(setup, side) + ["note SEED"]
    if boot_kind != "power" or setup:
        out += [f"rng {seed[0]} {seed[1]}", "fc 0"]
    if side == "emu":
        out.append("lstep reset")
        pads = emu_pads(pads, lags, extras)
    out += snapshot
    last = 0
    for i, pad in enumerate(pads, 1):
        out.append(step.format(pad))
        if side == "emu":
            # The original's lag frames advance FRAME_COUNTER (NMI) without a game
            # frame; the port has no lag. Re-sync it to the game-frame count so its
            # phase (HP/PP roller member, blinks) matches the port's.
            p = emu_to_port(i, lags, extras)
            out.append(f"fc {(p if p is not None else last) & 0xFF}")
            last = p if p is not None else last
        else:
            p = i
        out += snapshot
        if p in reseeds:
            out.append("rng {} {}".format(*reseeds[p]))
        if side == "emu" and p in oc_off:
            out.append("overclock disabled")
        if side == "emu" and p in (oc_on or {}):
            out.append(f"overclock {oc_on[p]}")
        out += (at or {}).get(p, [])
    if side == "emu":
        out.append("lag")
    out.append("end")
    return "\n".join(out) + "\n"


def parse_snapshots(text):
    """Split a run's output into {logic frame: field dict} (from the seeding RNG line on).

    Snapshots are numbered in order, or by the preceding ``LSTEP N`` line (emu);
    an index seen twice (an lstep that had nothing left to run) keeps the first,
    and an ``LSTEP N mid`` snapshot (taken part-way into frame N+1) is dropped.
    """
    snaps, cur, seeded, idx, mid = {}, None, False, 0, False
    # From the seed on: the ``note SEED`` marker (a setup can print RNG and LSTEP lines too),
    # else the first RNG line (older logs)
    marked = "\n> SEED\n" in text
    for line in text.splitlines():
        if not seeded and (line == "> SEED" if marked else line.startswith("RNG ")):
            seeded = True
            idx, mid = 0, False
            continue
        if marked and seeded and cur is None and not snaps and line.startswith("RNG "):
            continue  # the seeding rng's own line
        if not seeded:
            continue
        if line.startswith("LSTEP "):
            words = line.split()
            idx, mid = int(words[1]), "mid" in words[2:]
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
            if key == "NEAR":  # the 2 sides list the same entries in different orders
                rest = " | ".join(sorted(rest.split(" | ")))
            cur[key.lower()] = rest
        elif line.startswith("FOLLOW"):
            follow = GHOST.sub("", line[7:]).strip()
            if follow:  # the emu prints no FOLLOW line when no member follows
                cur["follow"] = follow
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
        elif line.startswith("FRAC "):
            m = re.search(r" bright=(\S+)", line)
            if m:
                cur["bright"] = m.group(1)
        elif line.startswith("RNG "):
            cur["rng"] = line[4:].strip()
            if not mid:  # sampled part-way into the next frame (emu lag): not comparable
                snaps.setdefault(idx, cur)
            idx += 1
            mid = False
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


def ignored(k, i, ignore):
    """Whether field k at logic frame i matches an ignore entry (REGEX, or (REGEX, first, last))."""
    for pat in ignore:
        if isinstance(pat, str):
            if re.fullmatch(pat, k):
                return True
        elif pat[1] <= i <= pat[2] and re.fullmatch(pat[0], k):
            return True
    return False


def parse_ignores(text):
    """``# ignore: REGEX [A[-B]]`` header lines as REGEX or (REGEX, A, B)."""
    out = []
    for pat, lo, hi in IGNORE_HEADER.findall(text):
        out.append((pat, int(lo), int(hi or lo)) if lo else pat)
    return out


def compare(port, emu, pads, ignore):
    """Return (frames compared, {field: first differing frame}, report lines)."""
    frames = sorted(set(port) & set(emu))
    n = len(frames)
    first = {}
    count = {}
    for i in frames:
        p, e = port[i], emu[i]
        for k in sorted(set(p) | set(e)):
            if k == "raw_state" or ignored(k, i, ignore):
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


SAVE_HEADER = re.compile(r"^#\s*save:\s*(\S+)", re.M)
IGNORE_HEADER = re.compile(r"^#\s*ignore:\s*(\S+)(?:[ \t]+(\d+)(?:-(\d+))?)?[ \t]*$", re.M)
LAG_HEADER = re.compile(r"^#\s*lag:\s*(\d+)", re.M)
EXTRA_HEADER = re.compile(r"^#\s*extra:((?:[ \t]+\d+)+)", re.M)
OVERCLOCK_HEADER = re.compile(r"^#\s*overclock:\s*(light|compatible|max)\b", re.M)
SKIP_HEADER = re.compile(r"^#\s*skip:\s*(\d+)", re.M)
AT_HEADER = re.compile(r"^#\s*at:\s*(\d+)\s+(debug on|debug off)\s*$", re.M)
OVERCLOCK_OFF_HEADER = re.compile(r"^#\s*overclock-off:\s*(\d+)", re.M)
OVERCLOCK_ON_HEADER = re.compile(r"^#\s*overclock-on:\s*(\d+)(?:[ \t]+(light|compatible|max))?", re.M)
RESEED_HEADER = re.compile(r"^#\s*reseed:\s*(\d+)(?:[ \t]+(\d+)[ \t]+(\d+))?", re.M)
COMPARE_BRIGHT_HEADER = re.compile(r"^#\s*compare:\s*bright\b", re.M)
BOOT_HEADER = re.compile(r"^#\s*boot:\s*(load|menu|power)\b", re.M)
SRAM_SIZE = 8192
LAG_WINDOW = 24  # logic frames after a lag-merged frame where NMI-driven state (fades) can shift


def scenario_save(scen, default):
    """The save a scenario names in its ``# save:`` header, else the default."""
    m = SAVE_HEADER.search(Path(scen).read_text())
    if not m:
        return default
    name = m.group(1)
    if name == "blank":  # an empty SRAM: no save files (new game)
        return "blank"
    path = HERE / "saves" / (name if name.endswith(".srm") else name + ".srm")
    return str(path.resolve())


def run_scenario(scen, save, a, out):
    """Play one scenario on both sides; return (status 0/1, report text)."""
    name = Path(scen).stem
    text = [c for raw in Path(scen).read_text().splitlines() for c in raw.split("#", 1)[0].split(";")]
    if "---" in [t.strip() for t in text]:
        cut = [t.strip() for t in text].index("---")
        setup, body = text[:cut], text[cut + 1 :]
    else:
        setup, body = [], text
    pads = expand_body(body)
    lags = {int(k) for k in LAG_HEADER.findall(Path(scen).read_text())}
    bright = bool(COMPARE_BRIGHT_HEADER.search(Path(scen).read_text()))
    extras = {int(k) for ks in EXTRA_HEADER.findall(Path(scen).read_text()) for k in ks.split()}
    at = {}
    for k, cmd in AT_HEADER.findall(Path(scen).read_text()):
        at.setdefault(int(k), []).append(cmd.strip())
    reseeds = {
        int(k): (int(x or a.rng[0]), int(y or a.rng[1])) for k, x, y in RESEED_HEADER.findall(Path(scen).read_text())
    }
    # Each side plays a private copy: both write SRAM back to --save when the game saves
    # (a phone call to Dad), which would change the save for every later run.
    copies = {side: out / f"{name}.{side}.srm" for side in ("port", "emu")}
    for c in copies.values():
        if save == "blank":
            c.write_bytes(bytes(SRAM_SIZE))
        else:
            shutil.copyfile(save, c)
    m = BOOT_HEADER.search(Path(scen).read_text())
    boot_kind = m.group(1) if m else "load"
    sides = {
        "port": [
            str(Path(a.bin).resolve()),
            "--headless",
            "--save",
            str(copies["port"]),
            "--record-audio",
            "/dev/null",
        ],
        "emu": [sys.executable, str(HERE / "emu.py"), "--save", str(copies["emu"]), "--count-frames"],
    }
    m = OVERCLOCK_HEADER.search(Path(scen).read_text())
    if m:
        sides["emu"] += ["--overclock", m.group(1)]
    if a.port_only:
        del sides["emu"]
    procs = {}
    t0 = time.time()
    report = []
    for side, cmd in sides.items():
        script = out / f"{name}.{side}.play"
        oc_off = {int(k) for k in OVERCLOCK_OFF_HEADER.findall(Path(scen).read_text())}
        oc_on = {int(k): m or "max" for k, m in OVERCLOCK_ON_HEADER.findall(Path(scen).read_text())}
        script.write_text(
            build_script(side, setup, pads, a.rng, lags, boot_kind, reseeds, oc_off, at, extras, bright, oc_on)
        )
        procs[side] = run_side(cmd, script, out / f"{name}.{side}.log")
    for side, p in procs.items():
        try:
            p.wait(timeout=max(1.0, a.timeout - (time.time() - t0)))
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            report.append(f"  {side} timed out")
    lost, checked = lost_frames.scan((out / f"{name}.port.log").read_text(errors="replace"))
    if a.port_only:
        status = f"{len(lost)} lost-frame events" if checked else "lost-frame check off in this build"
        report.insert(0, f"== {name} [{Path(save).stem}]: port only, {len(pads)} logic frames ({status})")
        report += [f"  {ln}" for ln in lost]
        return (1 if lost or not checked else 0), "\n".join(report)
    res = {s: parse_snapshots((out / f"{name}.{s}.log").read_text(errors="replace")) for s in sides}
    if lags or extras:
        res["emu"] = {
            emu_to_port(j, lags, extras): v for j, v in res["emu"].items() if emu_to_port(j, lags, extras) is not None
        }
    lag = re.findall(r"^LAG .*", (out / f"{name}.emu.log").read_text(errors="replace"), re.M)
    ignore = a.ignore + parse_ignores(Path(scen).read_text())
    if boot_kind == "power":
        res["emu"].pop(0, None)  # sampled before RESET ran: uninitialized WRAM
    for k in SKIP_HEADER.findall(Path(scen).read_text()):
        res["emu"].pop(int(k), None)  # sampled part-way into a load's lag frame
    n, first, lines = compare(res["port"], res["emu"], pads, ignore)
    merged = sorted(set(range(len(pads) + 1)) - set(res["emu"]) - extras)
    if first:
        f0 = min(first.values())
        near = [m for m in merged if f0 - LAG_WINDOW <= m <= f0]
        status = f"first divergence at logic frame {f0}"
        if near and {k for k, i in first.items() if i == f0} <= {"idle"}:
            # A fade runs on NMIs, so a lag stretch before it (the original's
            # WAIT_UNTIL_NEXT_FRAME returning at once after a long load) moves its
            # end in logic frames: townmap's fade-in ends 2 frames later in the emu.
            status += f", {f0 - near[-1]} after lag-merged frame {near[-1]}: likely fade timing after lag"
    else:
        status = "identical"
    head = f"== {name} [{Path(save).stem}]: {len(pads)} logic frames, compared {n} ({status}; "
    report.insert(0, head + f"{lag[-1] if lag else 'no LAG line'})")
    if merged:
        report.append(f"  emu frames merged by lag (not compared): {merged}")
    if len(res["port"]) < len(pads) + 1 or (res["emu"] and max(res["emu"]) < len(pads)):
        report.append(f"  warning: snapshots port={len(res['port'])} emu={len(res['emu'])}, expected {len(pads) + 1}")
    report += lines
    if lost:  # a frame the port ran and never presented (src/core/lost_frame_check.h)
        report.append(f"  LOST FRAMES (port): {len(lost)}")
        report += [f"    {ln}" for ln in lost[:5]]
    report.append(f"  scripts/logs: {out}/{name}.{{port,emu}}.{{play,log}}")
    return (1 if first or lost else 0), "\n".join(report)


def main():
    """Command-line entry point."""
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="[SAVE.srm] then scenario .play files (setup --- body)")
    ap.add_argument("--bin", default=os.environ.get("EB_BIN", str(DEFAULT_BIN)), help="port binary (256x224 build)")
    ap.add_argument("--rng", nargs=2, type=int, default=(4660, 22136), metavar=("A", "B"), help="RNG seed")
    ap.add_argument(
        "--ignore", action="append", default=["input"], help="regex of field names to skip (repeatable; default: input)"
    )
    ap.add_argument("--out", default=str(HERE / "out" / "autodiff"), help="output dir (scripts, logs)")
    ap.add_argument("--timeout", type=float, default=300, help="seconds per side")
    ap.add_argument("-j", "--jobs", type=int, default=3, help="scenarios run at once (2 processes each)")
    ap.add_argument(
        "--port-only",
        action="store_true",
        help="run only the port side and check it for lost frames (needs a -DEB_LOST_FRAME_CHECK=ON build)",
    )
    a = ap.parse_args()

    cache = Path(a.bin).resolve().parent / "CMakeCache.txt"
    if cache.exists() and "EB_VIEWPORT_WIDTH:STRING=256" not in cache.read_text():
        print(f"warning: {a.bin} is not a 256x224 build: enemy spawns will differ", file=sys.stderr)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    files = list(a.files)
    default = str(Path(files.pop(0)).resolve()) if files[0].endswith(".srm") else None
    jobs = []
    for scen in files:
        save = scenario_save(scen, default)
        if save is None:
            sys.exit(f"{scen}: no '# save:' header and no SAVE.srm given")
        jobs.append((scen, save))
    worst = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        futs = [ex.submit(run_scenario, scen, save, a, out) for scen, save in jobs]
        for fut in futs:
            status, text = fut.result()
            print(text, flush=True)
            worst = max(worst, status)
    return worst


if __name__ == "__main__":
    sys.exit(main())
