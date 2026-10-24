"""Frame-by-frame comparison of 2 builds over the regress replays (``regress.py framehash``).

Usage (from the repo root)::

    uv run --no-sync python playtest/regress.py framehash OLD_BIN NEW_BIN
    uv run --no-sync python playtest/regress.py framehash OLD_BIN NEW_BIN -k bus --png 5
    uv run --no-sync python playtest/regress.py framehash OLD_BIN NEW_BIN --no-cases --session bk1 --script sun=SAVE.srm:input.play

Each case runs on both binaries at the same time with ``--headless --record-video FIFO``; the
2 raw rgb24 streams are read in lockstep, every frame is hashed, and a frame whose bytes differ
is counted pixel by pixel: how many pixels differ, and how many of those lie in the SNES band
(the 256x224 rectangle ``[PAD_LEFT, PAD_LEFT+256) x [PAD_TOP, PAD_TOP+224)``, clipped to the
viewport) vs the gutters and pad rows outside it. The viewport size comes from the game's
``RECORD video WxH`` line, so 320x240, 256x224 and 240x240 builds all work (both binaries must
match).

Extra cases: ``--script NAME=SAVE:SCRIPT`` (a save plus an input script, played as is: add the
boot lines yourself, as a pt.py history has them) and ``--session NAME`` (a pt.py session's
start.srm and history.play). ``--no-cases`` skips the regress cases.

It never hangs: the FIFO is opened non-blocking with a placeholder writer (so neither side waits
in ``open``), reads go through ``select`` against a per-case wall-clock deadline (``--timeout``,
default 3x the case's), a case that runs out of time has both games killed and is reported as
TIMEOUT, every game still alive at exit is killed, and the temp dirs (FIFOs, saves) are removed.

Results go to ``playtest/out/regress/framehash/A-vs-B/``: ``report.txt``, per case a CSV of every
differing frame (``CASE.csv``) and both runs' frame hashes (``CASE.A.hashes``), the games' logs,
and with ``--png N`` a PNG for each of the first N differing frames per case (A | B | changed
pixels in red on B, SNES band outlined in green).
"""

import atexit
import contextlib
import csv
import hashlib
import os
import re
import select
import shutil
import signal
import subprocess
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

SNES_W, SNES_H = 256, 224
RECORD_LINE = re.compile(rb"RECORD video (\d+)x(\d+)")
CHUNK = 1 << 20

_live = set()  # every game Popen still running, killed at exit
_live_lock = threading.Lock()
_abort = threading.Event()  # set on Ctrl-C: every read loop gives up


class CaseTimeoutError(Exception):
    pass


def _kill_all():
    with _live_lock:
        procs = list(_live)
    for p in procs:
        if p.poll() is None:
            p.kill()
    for p in procs:
        with contextlib.suppress(subprocess.TimeoutExpired):
            p.wait(timeout=5)


atexit.register(_kill_all)


def band_rect(w, h):
    """The SNES-visible rectangle (x0, y0, x1, y1) in viewport coords, clipped to the viewport.

    PAD_LEFT/PAD_TOP are C integer divisions (src/core/types.h), which truncate toward 0: a
    240-wide viewport has PAD_LEFT -8, so its band spans the full width.
    """
    pad_l = int((w - SNES_W) / 2)
    pad_t = int((h - SNES_H) / 2)
    return max(pad_l, 0), max(pad_t, 0), min(pad_l + SNES_W, w), min(pad_t + SNES_H, h)


def band_mask(w, h):
    x0, y0, x1, y1 = band_rect(w, h)
    m = np.zeros((h, w), bool)
    m[y0:y1, x0:x1] = True
    return m


class GameRun:
    """One game recording to a FIFO; ``next_frame`` returns raw frames (None at the end)."""

    def __init__(self, binary, workdir, save, script, args=()):
        self.binary, self.workdir = str(Path(binary).resolve()), Path(workdir)
        self.workdir.mkdir(parents=True)
        self.fifo = self.workdir / "video.fifo"
        os.mkfifo(self.fifo)
        if save:
            shutil.copy(save, self.workdir / "save.srm")
        else:
            (self.workdir / "save.srm").write_bytes(bytes(8192))  # empty SRAM: a fresh file select
        (self.workdir / "input.play").write_text(script)
        self.args = list(args)
        self.proc = None
        self.rfd = self.wfd = None
        self.buf = bytearray()
        self.eof = False
        self.size = None
        self.frames = 0
        self.hashes = []
        self.log = self.workdir / "log.txt"

    def start(self):
        # Reader first, non-blocking (never waits for a writer); then a placeholder writer, so the
        # game's open() for writing returns at once and our reads see no EOF before the game opens
        # the FIFO. The placeholder is closed once the game has exited, which lets EOF through.
        self.rfd = os.open(self.fifo, os.O_RDONLY | os.O_NONBLOCK)
        self.wfd = os.open(self.fifo, os.O_WRONLY | os.O_NONBLOCK)
        cmd = [self.binary, "--headless", *self.args, "--save", "save.srm", "--input", "input.play"]
        cmd += ["--record-video", "video.fifo"]
        with self.log.open("wb") as logf:
            self.proc = subprocess.Popen(  # noqa: S603
                cmd, cwd=self.workdir, stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT
            )
        with _live_lock:
            _live.add(self.proc)

    def _release_writer_if_exited(self):
        if self.wfd is not None and self.proc.poll() is not None:
            os.close(self.wfd)
            self.wfd = None

    def _fill(self, deadline):
        """Read what is available (waiting at most until ``deadline``); False at EOF."""
        self._release_writer_if_exited()
        left = deadline - time.monotonic()
        if left <= 0 or _abort.is_set():
            raise CaseTimeoutError
        ready, _, _ = select.select([self.rfd], [], [], min(0.25, left))
        if not ready:
            return True
        try:
            chunk = os.read(self.rfd, CHUNK)
        except BlockingIOError:
            return True
        if not chunk:
            self.eof = True
            return False
        self.buf += chunk
        return True

    def wait_size(self, deadline):
        """The viewport (w, h) from the game's RECORD line, or None if it exited without one."""
        while self.size is None:
            m = RECORD_LINE.search(self.log.read_bytes())
            if m:
                self.size = (int(m.group(1)), int(m.group(2)))
                break
            if self.proc.poll() is not None:
                # It may have printed the line just before exiting
                m = RECORD_LINE.search(self.log.read_bytes())
                self.size = (int(m.group(1)), int(m.group(2))) if m else None
                return self.size
            if time.monotonic() > deadline or _abort.is_set():
                raise CaseTimeoutError
            time.sleep(0.02)
        return self.size

    def next_frame(self, deadline):
        fs = self.size[0] * self.size[1] * 3
        while len(self.buf) < fs:
            if self.eof or not self._fill(deadline):
                return None
        frame = bytes(self.buf[:fs])
        del self.buf[:fs]
        self.frames += 1
        self.hashes.append(hashlib.md5(frame, usedforsecurity=False).hexdigest()[:16])
        return frame

    def finish(self, grace=10.0):
        """Wait briefly for the game to exit (kill it otherwise); return its status string."""
        status = None
        try:
            self.proc.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            status = "TIMEOUT"
        return status or self.exit_status()

    def exit_status(self):
        rc = self.proc.returncode
        if rc is None:
            return "running"
        if rc < 0:
            return f"CRASH signal {-rc}"
        return "done" if rc == 0 else f"rc={rc}"

    def close(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.kill()
            with contextlib.suppress(subprocess.TimeoutExpired):
                self.proc.wait(timeout=5)
            with _live_lock:
                _live.discard(self.proc)
        for fd in (self.rfd, self.wfd):
            if fd is not None:
                os.close(fd)
        self.rfd = self.wfd = None


def diff_png(a, b, changed, rect, out_png, title):
    """Write A | B | changed pixels in red on B, with the SNES band outlined on the diff panel."""
    h, w = changed.shape
    ia, ib = Image.fromarray(a), Image.fromarray(b)
    sheet = Image.new("RGB", (w * 3 + 8, h + 14), "white")
    sheet.paste(ia, (0, 14))
    sheet.paste(ib, (w + 4, 14))
    red = Image.blend(ib, Image.new("RGB", ib.size, "black"), 0.6)
    red.paste(Image.new("RGB", ib.size, (255, 0, 0)), mask=Image.fromarray((changed * 255).astype(np.uint8)))
    sheet.paste(red, (2 * w + 8, 14))
    d = ImageDraw.Draw(sheet)
    d.text((2, 1), "A", fill="black")
    d.text((w + 6, 1), "B", fill="black")
    d.text((2 * w + 10, 1), title, fill="black")
    x0, y0, x1, y1 = rect
    d.rectangle([2 * w + 8 + x0, 14 + y0, 2 * w + 8 + x1 - 1, 14 + y1 - 1], outline=(0, 255, 0))
    out_png.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_png)


def compare_case(case, bin_a, bin_b, outdir, timeout, max_png=0):
    """Run one case on both binaries and compare every frame; return a result dict."""
    name = case["name"]
    res = {"case": name, "status": "done", "frames": [0, 0], "status_ab": ["", ""], "diffs": [], "size": None}
    t0 = time.monotonic()
    deadline = t0 + timeout
    tmp = Path(tempfile.mkdtemp(prefix=f"eb-framehash-{name}-"))
    runs = []
    try:
        for b, tag in ((bin_a, "A"), (bin_b, "B")):
            runs.append(GameRun(b, tmp / tag, case["save"], case["script"], case.get("args", ())))
        for r in runs:
            r.start()
        sizes = [r.wait_size(deadline) for r in runs]
        res["size"] = sizes[0] or sizes[1]
        if None in sizes or sizes[0] != sizes[1]:
            res["status"] = "NO VIDEO" if None in sizes else f"viewport mismatch A={sizes[0]} B={sizes[1]}"
        else:
            w, h = sizes[0]
            mask = band_mask(w, h)
            rect = band_rect(w, h)
            i = 0
            while True:
                fa = runs[0].next_frame(deadline)
                fb = runs[1].next_frame(deadline)
                if fa is None and fb is None:
                    break
                if fa is not None and fb is not None and fa != fb:
                    a = np.frombuffer(fa, np.uint8).reshape(h, w, 3)
                    b = np.frombuffer(fb, np.uint8).reshape(h, w, 3)
                    changed = (a != b).any(-1)
                    n, nb = int(changed.sum()), int((changed & mask).sum())
                    res["diffs"].append((i, n, nb))
                    if len(res["diffs"]) <= max_png:
                        title = f"frame {i}: {n} px ({nb} band, {n - nb} outside)"
                        diff_png(a, b, changed, rect, outdir / "png" / f"{name}--{i:06d}.png", title)
                i += 1
            for k, r in enumerate(runs):
                res["status_ab"][k] = r.finish(grace=max(1.0, deadline - time.monotonic()))
    except CaseTimeoutError:
        res["status"] = "INTERRUPTED" if _abort.is_set() else f"TIMEOUT after {timeout:.0f}s"
    finally:
        for k, r in enumerate(runs):
            if r.proc is not None and not res["status_ab"][k]:
                res["status_ab"][k] = r.exit_status() if r.proc.poll() is not None else "killed"
            r.close()
            res["frames"][k] = r.frames
            tag = "AB"[k]
            if r.log.exists():
                shutil.copy(r.log, outdir / f"{name}.{tag}.log")
            (outdir / f"{name}.{tag}.hashes").write_text("".join(f"{x}\n" for x in r.hashes))
        shutil.rmtree(tmp, ignore_errors=True)
    sides = f"A {res['status_ab'][0] or '-'}, B {res['status_ab'][1] or '-'}"
    if res["status"] == "done" and any(s not in ("done", "") for s in res["status_ab"]):
        res["status"] = sides
    elif res["status"] != "done":
        res["status"] += f" ({sides})"
    for tag in "AB":
        log = outdir / f"{name}.{tag}.log"
        if (
            res["status"] == "done"
            and case.get("end_marker")
            and log.exists()
            and case["end_marker"] not in log.read_text(errors="replace")
        ):
            res.setdefault("notes", []).append(f"{tag} ended before the end of the script")
    if res["diffs"]:
        with (outdir / f"{name}.csv").open("w", newline="") as f:
            wr = csv.writer(f)
            wr.writerow(["frame", "pixels", "band", "outside"])
            wr.writerows([(i, n, nb, n - nb) for i, n, nb in res["diffs"]])
    res["secs"] = round(time.monotonic() - t0, 1)
    return res


def case_lines(res, list_max):
    """Report lines for one result."""
    fa, fb = res["frames"]
    nd = len(res["diffs"])
    status = "" if res["status"] == "done" else f" [{res['status']}]"
    notes = "".join(f" [{n}]" for n in res.get("notes", []))
    if nd == 0 and fa == fb and not status:
        return [f"CASE {res['case']}: same, frames A={fa} B={fb}{notes}"]
    band = sum(d[2] for d in res["diffs"])
    out = [f"CASE {res['case']}: DIFF frames A={fa} B={fb} differing={nd}{status}{notes}"]
    if fa != fb:
        out.append(f"  frame count differs: {abs(fa - fb)} extra frames in {'A' if fa > fb else 'B'}")
    if nd:
        inband = sum(1 for d in res["diffs"] if d[2])
        out.append(f"  {inband} of {nd} differing frames touch the SNES band ({band} band px in all)")
        for i, n, nb in res["diffs"][:list_max]:
            out.append(f"  frame {i}: {n} px (band {nb}, outside {n - nb})")
        if nd > list_max:
            out.append(f"  ... {nd - list_max} more in {res['case']}.csv")
    return out


def run(cases, bin_a, bin_b, outdir, jobs=None, timeout=None, max_png=0, list_max=40, clean=True):
    """Compare every case; print and write the report; return the number of differing cases."""
    if clean:  # a full run: drop files of cases that no longer exist
        shutil.rmtree(outdir, ignore_errors=True)
    outdir.mkdir(parents=True, exist_ok=True)
    for c in cases:  # this run's cases start afresh
        for f in [*outdir.glob(f"{c['name']}.*"), *outdir.glob(f"png/{c['name']}--*.png")]:
            f.unlink()
    jobs = jobs or max(1, (os.cpu_count() or 2) // 2)  # each case runs 2 games
    t0 = time.time()

    def one(case):
        r = compare_case(case, bin_a, bin_b, outdir, timeout or case["timeout"], max_png)
        flag = "" if r["status"] == "done" else f"  <-- {r['status']}"
        fa, fb = r["frames"]
        print(f"  {case['name']:<28} {r['secs']:6.1f}s frames {fa}/{fb} differing {len(r['diffs'])}{flag}", flush=True)
        return r

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    # SIGTERM/SIGHUP (and SIGINT, even when inherited as ignored) unwind like Ctrl-C, so the games
    # get killed and the temp dirs removed
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    results = []
    ex = ThreadPoolExecutor(max_workers=jobs)
    try:
        results = list(ex.map(one, cases))
    except KeyboardInterrupt:
        _abort.set()
    finally:
        ex.shutdown(wait=True, cancel_futures=True)
        _kill_all()
    if _abort.is_set():
        raise SystemExit("framehash: interrupted; games killed, temp dirs removed")
    size = next((r["size"] for r in results if r["size"]), None)
    lines = [f"A = {bin_a}", f"B = {bin_b}"]
    if size:
        x0, y0, x1, y1 = band_rect(*size)
        lines.append(f"viewport {size[0]}x{size[1]}, SNES band x[{x0},{x1}) y[{y0},{y1})")
    lines.append("")
    ndiff = 0
    for r in sorted(results, key=lambda r: r["case"]):
        cl = case_lines(r, list_max)
        ndiff += len(cl) > 1 or "DIFF" in cl[0]
        lines += cl
    total = sum(len(r["diffs"]) for r in results)
    lines += ["", f"{ndiff} of {len(results)} cases differ, {total} differing frames in all ({time.time() - t0:.0f}s)"]
    lines.append(f"details in {outdir}")
    text = "\n".join(lines)
    (outdir / "report.txt").write_text(text + "\n")
    print(text)
    return ndiff


def regress_cases(regress, patterns):
    """Regress cases as framehash cases (save path, full script; the savestate self-test is skipped)."""
    out = []
    for c in regress.all_cases(patterns):
        if "--selftest-savestate" in c["args"]:
            continue
        save = None
        if c["save"] and c["save"] != "none":
            save = regress.SAVES / (c["save"] if c["save"].endswith(".srm") else c["save"] + ".srm")
            if not save.exists():
                print(f"skipping {c['name']}: missing save {save}")
                continue
        out.append(
            {
                "name": c["name"],
                "save": save,
                "script": regress.script_for(c),
                "args": c["args"],
                "timeout": 3 * c["timeout"],
                "end_marker": f"SYNC {regress.END_SYNC} ",
            }
        )
    return out


def script_case(spec, timeout=600.0):
    """``NAME=SAVE:SCRIPT`` (or ``SAVE:SCRIPT``, named after the script's dir) as a case."""
    name, _, rest = spec.rpartition("=")
    save, sep, script = rest.partition(":")
    if not sep:
        raise SystemExit(f"--script wants NAME=SAVE:SCRIPT, got {spec}")
    script = Path(script)
    name = name or (script.parent.name if script.stem == "input" else script.stem)
    return {"name": "x-" + name, "save": Path(save), "script": script.read_text(), "timeout": timeout}


def session_case(sessions, name, timeout=600.0):
    """A pt.py session's start.srm + history.play (comments dropped) as a case."""
    d = sessions / name
    hist, save = d / "history.play", d / "start.srm"
    if not hist.exists() or not save.exists():
        raise SystemExit(f"session {name} has no history.play/start.srm in {d}")
    cmds = [ln for ln in hist.read_text().splitlines() if ln.strip() and not ln.startswith("#")]
    return {"name": "s-" + name, "save": save, "script": "\n".join(cmds) + "\n", "timeout": timeout}
