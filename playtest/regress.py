"""Replay-based regression check for the C port.

Usage (from the repo root)::

    uv run --no-sync python playtest/regress.py compare BIN_A BIN_B      # run both, diff (no goldens)
    uv run --no-sync python playtest/regress.py record NAME [--bin BIN]  # write goldens for BIN
    uv run --no-sync python playtest/regress.py check NAME [--bin BIN]   # run BIN, diff against goldens NAME
    uv run --no-sync python playtest/regress.py diff RUN_DIR_A RUN_DIR_B    # re-diff two finished runs
    uv run --no-sync python playtest/regress.py list
    uv run --no-sync python playtest/regress.py framehash BIN_A BIN_B [--png N] [--script NAME=SAVE:SCRIPT] [--session NAME]

Common options: ``-k PATTERN`` (run cases whose name contains PATTERN; repeatable), ``-j N``
(parallel games, default the CPU count), ``--timeout S`` (wall-clock limit per case).

Each case is ``playtest/regress/cases/NAME.play``: header lines ``# save: NN-Name`` (a
``playtest/saves/`` file; ``none`` for an empty SRAM), ``# about: ...``, optionally ``# boot: no``
(start on the title screen instead of file 1 -> Continue), ``# timeout: S`` and ``# args: ...``
(extra binary arguments; a case with ``--selftest-savestate`` has no script). The body is the
input-script language of playtest/README.md plus ``check LABEL``: a checkpoint that records the
STATE line (without ``frame=``), PARTY, ENEMIES, the window text (``text``), every TEXT line
printed since the previous checkpoint, the ``until`` results (met/TIMEOUT) and ERR lines in
between, and a screenshot. Checkpoints line up by label, so a fix that shifts timing doesn't
misalign later ones: put an event ``until`` (``battle``, ``in:TEXT_PROMPT``, ``idle``,
``said:WORD``) before each ``check``. The frame number is reported as a timing note only.

A run that ends early (crash, signal, wall-clock timeout, game exit) is reported, and so is a
checkpoint that never ran. Results go to ``playtest/out/regress/`` (gitignored): ``runs/TAG/CASE/``
holds each game's log and shots, ``golden/NAME/`` the recorded goldens, and ``diff/A-vs-B/``
the report and a PNG per differing checkpoint (A | B | changed pixels in red on B).

``framehash`` compares every frame of 2 builds over the cases instead of the checkpoints (see
playtest/framehash.py): per case the frame counts, each differing frame, and how many of its
pixels differ inside the SNES band vs the gutters. Results go to ``framehash/A-vs-B/``.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CASES = HERE / "regress" / "cases"
SAVES = HERE / "saves"
OUT = HERE / "out" / "regress"
DEFAULT_BIN = os.environ.get("EB_BIN", str(ROOT / "port/unix/build/earthbound"))

# Title screen -> file select -> file 1 -> Continue, then wait for control (as pt.py)
BOOT = ["300 -", "1 START", "200 -", "1 A", "120 -", "1 A", "120 -", "1 A", "until idle 900"]
PROBLEM = re.compile(r"error|halt|assert|abort|unknown script|sanitizer|segmentation", re.IGNORECASE)
CHECK_NOTE = "> CHECK "
END_SYNC = "rg-end"


def load_case(path):
    """Parse a case file into a dict: name, save, about, boot, timeout, args, body lines."""
    case = {"name": path.stem, "save": None, "about": "", "boot": True, "timeout": 60.0, "args": [], "body": []}
    for ln in path.read_text().splitlines():
        m = re.match(r"#\s*(save|about|boot|timeout|args):\s*(.*)$", ln)
        if m and not case["body"]:
            key, val = m.group(1), m.group(2).strip()
            if key == "boot":
                case["boot"] = val.lower() not in ("no", "false", "0")
            elif key == "timeout":
                case["timeout"] = float(val)
            elif key == "args":
                case["args"] = val.split()
            else:
                case[key] = val
            continue
        # One command per line or ';'-separated, as in pt.py do; '#' starts a comment
        for part in ln.split("#", 1)[0].split(";"):
            if part.strip():
                case["body"].append(part.strip())
    return case


def all_cases(patterns=None):
    cases = [load_case(p) for p in sorted(CASES.glob("*.play"))]
    if patterns:
        cases = [c for c in cases if any(p in c["name"] for p in patterns)]
    return cases


def script_for(case):
    """The input script the game runs: boot + body, each ``check`` expanded, then an end marker."""
    lines = list(BOOT) if case["boot"] else []
    for ln in case["body"]:
        if ln.startswith("check "):
            label = ln.split(None, 1)[1].strip()
            lines += [f"note CHECK {label}", "text", f"state {label}", f"shot ck-{label}"]
        else:
            lines.append(ln)
    lines += ["wait 1", f"sync {END_SYNC}"]
    return "\n".join(lines) + "\n"


def run_case(case, binary, workdir, timeout=None):
    """Run one case in ``workdir``; return the parsed result dict (also written to result.json)."""
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)
    selftest = "--selftest-savestate" in case["args"]
    cmd = [str(Path(binary).resolve()), "--headless", *case["args"]]
    if case["save"] and case["save"] != "none":
        src = SAVES / (case["save"] if case["save"].endswith(".srm") else case["save"] + ".srm")
        if not src.exists():
            return {"case": case["name"], "status": f"missing save {src.name}", "checks": {}, "order": []}
        shutil.copy(src, workdir / "save.srm")
        cmd += ["--save", "save.srm"]
    elif not selftest:
        (workdir / "save.srm").write_bytes(bytes(8192))  # empty SRAM: a fresh file select
        cmd += ["--save", "save.srm"]
    if not selftest:
        (workdir / "input.play").write_text(script_for(case))
        cmd += ["--input", "input.play"]
    t0 = time.time()
    status = "done"
    try:
        p = subprocess.run(  # noqa: S603
            cmd, cwd=workdir, capture_output=True, text=True, errors="replace", timeout=timeout or case["timeout"]
        )
        out, err, rc = p.stdout, p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        err = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
        rc, status = None, f"HANG: wall-clock timeout after {timeout or case['timeout']:.0f}s"
    secs = time.time() - t0
    (workdir / "log.txt").write_text(out + ("\n--- stderr\n" + err if err else ""))
    res = parse_output(case, out, workdir)
    if status == "done":
        if rc is not None and rc < 0:
            status = f"CRASH: signal {-rc}"
        elif selftest:
            status = "done" if rc == 0 else f"FAIL rc={rc}"
            res["checks"]["selftest"] = {"lines": [ln for ln in err.splitlines() if "self-test" in ln], "frame": 0}
            res["order"].append("selftest")
        elif f"SYNC {END_SYNC} " not in out:
            status = f"CRASH: game exited before the end of the script (rc={rc})"
    res["status"] = status
    res["rc"] = rc
    res["secs"] = round(secs, 1)
    res["problems"] = [ln for ln in (out + err).splitlines() if PROBLEM.search(ln) and not ln.startswith("TEXT")][:20]
    (workdir / "result.json").write_text(json.dumps(res, indent=1))
    return res


def parse_output(case, out, workdir):
    """Split the game's stdout into checkpoints: {label: {state, party, enemies, win, text, until, err, shot}}."""
    checks, order = {}, []
    pending_text, pending_until, pending_err = [], [], []
    cur = None
    for ln in out.splitlines():
        if ln.startswith(CHECK_NOTE):
            cur = {
                "frame": None,
                "text": pending_text,
                "until": pending_until,
                "err": pending_err,
                "win": [],
                "state": "",
                "party": "",
                "enemies": "",
                "shot": None,
            }
            label = ln[len(CHECK_NOTE) :].strip()
            checks[label] = cur
            order.append(label)
            pending_text, pending_until, pending_err = [], [], []
            continue
        if ln.startswith("TEXT "):
            pending_text.append(ln[5:])
        elif ln.startswith("UNTIL "):
            m = re.match(r"UNTIL (\S+)", ln)
            pending_until.append(m.group(1) if m else ln)
        elif ln.startswith("ERR"):
            pending_err.append(re.sub(r"line \d+: ", "", ln))
        elif cur is not None and cur["shot"] is None:
            # Lines that belong to the checkpoint just opened (text, state, shot)
            if ln.startswith(("WIN ", "MENU ")):
                cur["win"].append(ln)
            elif ln.startswith("STATE "):
                m = re.search(r"frame=(\d+)", ln)
                cur["frame"] = int(m.group(1)) if m else None
                cur["state"] = re.sub(r" frame=\d+", "", ln)
            elif ln.startswith("PARTY"):
                cur["party"] = ln
            elif ln.startswith("ENEMIES"):
                cur["enemies"] = ln
            elif ln.startswith("SHOT "):
                cur["shot"] = str((workdir / ln.split()[1]).resolve())
    return {"case": case["name"], "checks": checks, "order": order, "tail_text": pending_text, "tail_err": pending_err}


def run_all(cases, binaries, outdirs, jobs, timeout=None):
    """Run every case on every binary in parallel; return {tag: {case: result}}."""
    work = [(c, b, d) for b, d in zip(binaries, outdirs) for c in cases]
    results = {str(d): {} for d in outdirs}
    t0 = time.time()

    def one(item):
        c, b, d = item
        r = run_case(c, b, d / c["name"], timeout)
        flag = "" if r["status"] == "done" else f"  <-- {r['status']}"
        print(f"  {d.name:>12} {c['name']:<28} {r['secs']:6.1f}s {len(r['order'])} checks{flag}", flush=True)
        return d, c["name"], r

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for d, name, r in ex.map(one, work):
            results[str(d)][name] = r
    print(f"ran {len(work)} games in {time.time() - t0:.0f}s")
    return results


# ---- diffing ----


def shot_diff(pa, pb, out_png):
    """Compare two screenshots; return (changed pixels, bbox) and write a side-by-side PNG if they differ."""
    if not pa or not pb or not Path(pa).exists() or not Path(pb).exists():
        return None, None
    a, b = Image.open(pa).convert("RGB"), Image.open(pb).convert("RGB")
    if a.size != b.size:
        return -1, None
    diff = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v else 0)
    bbox = diff.getbbox()
    if not bbox:
        return 0, None
    n = diff.histogram()[255]
    w, h = a.size
    sheet = Image.new("RGB", (w * 3 + 8, h + 14), "white")
    sheet.paste(a, (0, 14))
    sheet.paste(b, (w + 4, 14))
    red = Image.blend(b, Image.new("RGB", b.size, "black"), 0.6)
    red.paste(Image.new("RGB", b.size, (255, 0, 0)), mask=diff)
    sheet.paste(red, (2 * w + 8, 14))
    d = ImageDraw.Draw(sheet)
    d.text((2, 1), "A", fill="black")
    d.text((w + 6, 1), "B", fill="black")
    d.text((2 * w + 10, 1), f"{n} px changed, bbox {bbox}", fill="black")
    d.rectangle([2 * w + 8 + bbox[0], 14 + bbox[1], 2 * w + 8 + bbox[2] - 1, 14 + bbox[3] - 1], outline=(0, 255, 0))
    out_png.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_png)
    return n, bbox


def line_diff(a, b):
    """Short -/+ listing of lines that differ between two lists (order kept, common lines dropped)."""
    out = []
    import difflib

    for ln in difflib.unified_diff(a, b, lineterm="", n=0):
        if ln.startswith(("---", "+++", "@@")):
            continue
        out.append(ln)
    return out


def diff_case(name, ra, rb, diffdir):
    """Diff one case's two results; return a list of report lines (empty when identical)."""
    rep = []
    if ra["status"] != rb["status"]:
        rep.append(f"status: A={ra['status']} | B={rb['status']}")
    elif ra["status"] != "done":
        rep.append(f"status (both): {ra['status']}")
    for label in dict.fromkeys(ra["order"] + rb["order"]):
        ca, cb = ra["checks"].get(label), rb["checks"].get(label)
        if ca is None or cb is None:
            rep.append(f"[{label}] checkpoint missing in {'A' if ca is None else 'B'}")
            continue
        sub = []
        for key in ("until", "err", "state", "party", "enemies", "lines"):
            va, vb = ca.get(key), cb.get(key)
            if va != vb:
                if isinstance(va, list):
                    sub.append(f"{key}: " + " ; ".join(line_diff(va, vb) or [f"A={va} B={vb}"]))
                else:
                    sub.append(f"{key}:\n      A {va}\n      B {vb}")
        for key in ("text", "win"):
            if ca.get(key, []) != cb.get(key, []):
                sub.append(f"{key}:\n      " + "\n      ".join(line_diff(ca.get(key, []), cb.get(key, []))))
        n, bbox = shot_diff(ca.get("shot"), cb.get("shot"), diffdir / f"{name}--{label}.png")
        if n:
            sub.append(f"shot: {n} px changed, bbox {bbox} -> {diffdir / f'{name}--{label}.png'}")
        fa, fb = ca.get("frame"), cb.get("frame")
        if sub and fa is not None and fb is not None and fa != fb:
            sub.append(f"timing: frame A={fa} B={fb} ({fb - fa:+d})")
        elif fa is not None and fb is not None and fa != fb:
            sub.append(f"timing only: frame A={fa} B={fb} ({fb - fa:+d})")
        rep += [f"[{label}] {s}" for s in sub]
    if ra.get("problems") != rb.get("problems"):
        rep.append("problems: " + " ; ".join(line_diff(ra.get("problems", []), rb.get("problems", []))))
    return rep


def report(results_a, results_b, diffdir, label_a, label_b, clean=True):
    if clean:
        shutil.rmtree(diffdir, ignore_errors=True)
    diffdir.mkdir(parents=True, exist_ok=True)
    lines = [f"A = {label_a}", f"B = {label_b}", ""]
    ndiff = 0
    for name in sorted(set(results_a) | set(results_b)):
        ra, rb = results_a.get(name), results_b.get(name)
        if ra is None or rb is None:
            lines.append(f"CASE {name}: only in {'B' if ra is None else 'A'}")
            ndiff += 1
            continue
        rep = diff_case(name, ra, rb, diffdir)
        real = [r for r in rep if "timing only" not in r]
        if real:
            ndiff += 1
            lines.append(f"CASE {name}: DIFF")
            lines += [f"  {r}" for r in rep]
        else:
            note = f" ({len(rep)} timing-only)" if rep else ""
            lines.append(f"CASE {name}: same ({len(ra['order'])} checkpoints){note}")
    lines.append("")
    lines.append(f"{ndiff} of {len(set(results_a) | set(results_b))} cases differ; diff PNGs in {diffdir}")
    text = "\n".join(lines)
    (diffdir / "report.txt").write_text(text + "\n")
    print(text)
    return ndiff


def load_results(d, cases=None):
    """{case: result} from the result.json files under run dir ``d``."""
    out = {}
    for f in sorted(Path(d).glob("*/result.json")):
        if cases is None or f.parent.name in cases:
            out[f.parent.name] = json.loads(f.read_text())
    return out


def build_tag(binary):
    """A short name for a binary: its build dir (port/unix/build-X/earthbound -> build-X)."""
    p = Path(binary).resolve()
    return p.parent.name if p.name == "earthbound" else p.stem


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-k", action="append", help="only cases whose name contains this (repeatable)")
    common.add_argument("-j", type=int, default=os.cpu_count() or 4, help="parallel games")
    common.add_argument("--timeout", type=float, help="wall-clock seconds per case (default: the case's, 60)")
    s = sub.add_parser("compare", parents=[common], help="run every case on two binaries and diff them")
    s.add_argument("bin_a")
    s.add_argument("bin_b")
    s = sub.add_parser("record", parents=[common], help="run every case and store the results as goldens")
    s.add_argument("name")
    s.add_argument("--bin", default=DEFAULT_BIN)
    s = sub.add_parser("check", parents=[common], help="run every case and diff against stored goldens")
    s.add_argument("name")
    s.add_argument("--bin", default=DEFAULT_BIN)
    s = sub.add_parser("diff", help="diff two finished runs (runs/TAG or golden/NAME dirs) without rerunning")
    s.add_argument("dir_a")
    s.add_argument("dir_b")
    sub.add_parser("list", help="list the cases")
    s = sub.add_parser("framehash", help="compare every frame of two binaries over the cases (playtest/framehash.py)")
    s.add_argument("bin_a")
    s.add_argument("bin_b")
    s.add_argument("-k", action="append", help="only cases whose name contains this (repeatable)")
    s.add_argument("-j", type=int, help="parallel cases, 2 games each (default: half the CPU count)")
    s.add_argument("--timeout", type=float, help="wall-clock seconds per case (default: 3x the case's; extras 600)")
    s.add_argument("--png", type=int, default=0, metavar="N", help="write diff PNGs of the first N differing frames")
    s.add_argument("--list", type=int, default=40, metavar="N", help="differing frames listed per case in the report")
    s.add_argument("--script", action="append", default=[], metavar="NAME=SAVE:SCRIPT", help="extra case (repeatable)")
    s.add_argument(
        "--session", action="append", default=[], metavar="NAME", help="extra case: a pt.py session's history"
    )
    s.add_argument("--no-cases", action="store_true", help="only the --script/--session extras")
    a = ap.parse_args()

    if a.cmd == "diff":
        da, db = Path(a.dir_a), Path(a.dir_b)
        ra, rb = load_results(da), load_results(db)
        return 1 if report(ra, rb, OUT / "diff" / f"{da.name}-vs-{db.name}", str(da), str(db)) else 0
    if a.cmd == "list":
        for c in all_cases():
            print(f"{c['name']:<28} {c['save'] or '-':<16} {c['about']}")
        return 0
    if a.cmd == "framehash":
        import framehash

        for b in (a.bin_a, a.bin_b):
            if not Path(b).exists():
                sys.exit(f"binary not found: {b}")
        cases = [] if a.no_cases else framehash.regress_cases(sys.modules[__name__], a.k)
        cases += [framehash.script_case(spec) for spec in a.script]
        cases += [framehash.session_case(HERE / "out" / "sessions", n) for n in a.session]
        if not cases:
            sys.exit("no cases")
        ta, tb = build_tag(a.bin_a), build_tag(a.bin_b)
        if ta == tb:
            ta, tb = ta + "-A", tb + "-B"
        outdir = OUT / "framehash" / f"{ta}-vs-{tb}"
        return (
            1
            if framehash.run(
                cases, a.bin_a, a.bin_b, outdir, a.j, a.timeout, a.png, a.list, clean=not (a.k or a.no_cases)
            )
            else 0
        )
    cases = all_cases(a.k)
    if not cases:
        sys.exit("no cases match")
    for b in [a.bin_a, a.bin_b] if a.cmd == "compare" else [a.bin]:
        if not Path(b).exists():
            sys.exit(f"binary not found: {b}")

    if a.cmd == "compare":
        ta, tb = build_tag(a.bin_a), build_tag(a.bin_b)
        if ta == tb:
            ta, tb = ta + "-A", tb + "-B"
        da, db = OUT / "runs" / ta, OUT / "runs" / tb
        if not a.k:  # a full run: drop results of cases that no longer exist
            shutil.rmtree(da, ignore_errors=True)
            shutil.rmtree(db, ignore_errors=True)
        res = run_all(cases, [a.bin_a, a.bin_b], [da, db], a.j, a.timeout)
        n = report(res[str(da)], res[str(db)], OUT / "diff" / f"{ta}-vs-{tb}", a.bin_a, a.bin_b, clean=not a.k)
        return 1 if n else 0
    if a.cmd == "record":
        gd = OUT / "golden" / a.name
        if not a.k:
            shutil.rmtree(gd, ignore_errors=True)
        res = run_all(cases, [a.bin], [gd], a.j, a.timeout)[str(gd)]
        (gd / "binary").write_text(str(Path(a.bin).resolve()) + "\n")
        bad = [n for n, r in res.items() if r["status"] != "done"]
        print(f"goldens in {gd}" + (f"; NOT done: {', '.join(bad)}" if bad else ""))
        return 1 if bad else 0
    gd = OUT / "golden" / a.name
    if not gd.exists():
        sys.exit(f"no goldens named {a.name} (record them first)")
    golden = load_results(gd, {c["name"] for c in cases})
    rd = OUT / "runs" / f"check-{a.name}"
    if not a.k:
        shutil.rmtree(rd, ignore_errors=True)
    res = run_all(cases, [a.bin], [rd], a.j, a.timeout)[str(rd)]
    n = report(golden, res, OUT / "diff" / f"golden-{a.name}-vs-check", f"golden {a.name}", a.bin, clean=not a.k)
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
