"""List the playtest-related processes and flag the ones that look stuck (read-only: never kills).

Usage (from the repo root)::

    uv run --no-sync python playtest/stale_jobs.py              # flag idle jobs with no new output for 15 min
    uv run --no-sync python playtest/stale_jobs.py --minutes 5 --cpu 2 --all

It lists earthbound binaries, ffmpeg, and python (or ``uv run``) processes running pt.py, regress.py,
run.py, emu.py or any script under playtest/ or a Claude scratchpad dir, with PID, elapsed time, %CPU
and the newest mtime among their likely outputs: files they have open for writing (a redirected
log included), their working directory (a pt.py session dir, a regress run dir), a pt.py session
named in argv, and the dirs of scratchpad files in argv. A process whose %CPU is under ``--cpu``
(default 1) and whose outputs are all older than ``--minutes`` (default 15) is flagged STALE; the
exit status is 1 when anything is flagged. An idle ``--session`` game is a pt.py session nobody
is driving: ``pt.py stop NAME`` ends it.
"""

import argparse
import contextlib
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SESSIONS = HERE / "out" / "sessions"
SCRATCH = re.compile(r"(/private)?/tmp/claude-[^/\s]+/[^\s]*")
PY_TOOLS = re.compile(r"(?:^|/)(pt|regress|run|emu|framehash|stale_jobs)\.py$")
WALK_LIMIT = 4000  # files looked at per directory


def ps():
    """[{pid, ppid, secs, cpu, cmd, argv}] for every process."""
    out = subprocess.run(  # noqa: S603
        ["/bin/ps", "-axww", "-o", "pid=,ppid=,etime=,pcpu=,command="], capture_output=True, text=True, check=True
    ).stdout
    procs = []
    for ln in out.splitlines():
        parts = ln.split(None, 4)
        if len(parts) < 5:
            continue
        pid, ppid, etime, cpu, cmd = parts
        procs.append({"pid": int(pid), "ppid": int(ppid), "secs": etime_secs(etime), "cpu": float(cpu), "cmd": cmd})
    return procs


def etime_secs(t):
    """Elapsed time as ps prints it, ``[[dd-]hh:]mm:ss`` in seconds."""
    days, _, t = t.rpartition("-")
    nums = [int(x) for x in t.split(":")]
    while len(nums) < 3:
        nums.insert(0, 0)
    return (int(days) if days else 0) * 86400 + nums[0] * 3600 + nums[1] * 60 + nums[2]


def kind(cmd):
    """What a command line is ("game", "ffmpeg", "python ...") if it is one of ours, else None."""
    argv = cmd.split()
    exe = Path(argv[0]).name
    if exe == "earthbound":
        return "game-session" if "--session" in argv else "game"
    if exe == "ffmpeg":
        return "ffmpeg" if ("ebsrc" in cmd or SCRATCH.search(cmd) or "video.fifo" in cmd) else None
    if exe.startswith("python") or exe == "uv":
        for a in argv[1:]:
            if a.endswith(".py") and (PY_TOOLS.search(a) or "playtest/" in a or SCRATCH.match(a)):
                if "stale_jobs.py" in a:
                    return None
                return ("uv " if exe == "uv" else "") + Path(a).name
    return None


def lsof(pids):
    """{pid: {"cwd": path, "writing": [paths]}} from one lsof call (empty if lsof is missing)."""
    res = {p: {"cwd": None, "writing": []} for p in pids}
    if not pids:
        return res
    try:
        exe = "/usr/sbin/lsof" if Path("/usr/sbin/lsof").exists() else "lsof"
        out = subprocess.run(  # noqa: S603
            [exe, "-n", "-P", "-Fpfatn", "-p", ",".join(map(str, pids))],
            capture_output=True,
            text=True,
            timeout=30,
        ).stdout
    except (OSError, subprocess.TimeoutExpired):
        return res
    pid = fd = acc = typ = None
    for ln in out.splitlines():
        tag, val = ln[:1], ln[1:]
        if tag == "p":
            pid = int(val)
        elif tag == "f":
            fd, acc, typ = val, None, None
        elif tag == "a":
            acc = val
        elif tag == "t":
            typ = val
        elif tag == "n" and pid in res:
            if fd == "cwd":
                res[pid]["cwd"] = val
            elif typ == "REG" and acc in ("w", "u") and not val.startswith("/dev/"):
                res[pid]["writing"].append(val)
    return res


def newest(path):
    """Newest mtime of a file, or of anything in a directory (2 levels, bounded); None if missing."""
    p = Path(path)
    try:
        best = p.stat().st_mtime
    except OSError:
        return None
    if p.is_dir():
        seen = 0
        for root, dirs, files in os.walk(p):
            depth = len(Path(root).relative_to(p).parts)
            if depth >= 2:
                dirs[:] = []
            for f in files:
                seen += 1
                with contextlib.suppress(OSError):
                    best = max(best, (Path(root) / f).stat().st_mtime)
            if seen > WALK_LIMIT:
                break
    return best


def outputs(proc, info):
    """Paths whose mtimes show this process's progress."""
    paths = list(info["writing"])
    cwd = info["cwd"]
    if cwd and Path(cwd).resolve() not in (ROOT, Path.home(), Path("/")):
        paths.append(cwd)
    argv = proc["cmd"].split()
    if "pt.py" in proc["cmd"]:
        # pt.py CMD NAME ...: the session dir
        for i, a in enumerate(argv):
            if a.endswith("pt.py") and i + 2 < len(argv) and (SESSIONS / argv[i + 2]).is_dir():
                paths.append(str(SESSIONS / argv[i + 2]))
    if "regress.py" in proc["cmd"]:
        paths.append(str(HERE / "out" / "regress"))
    for a in argv[1:]:
        m = SCRATCH.match(a.split("=", 1)[-1])
        if m:
            p = Path(m.group(0))
            if p.is_file() and os.access(p, os.X_OK):
                continue  # a binary is an input
            paths.append(str(p if p.is_dir() else p.parent))
    cache = str(Path.home() / ".cache")
    return [p for p in dict.fromkeys(paths) if not p.startswith(cache)]


def fmt_secs(s):
    if s is None:
        return "-"
    s = int(s)
    if s >= 86400:
        return f"{s // 86400}d{s % 86400 // 3600:02d}h"
    if s >= 3600:
        return f"{s // 3600}h{s % 3600 // 60:02d}m"
    return f"{s // 60}m{s % 60:02d}s"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minutes", type=float, default=15.0, help="outputs older than this count as stale")
    ap.add_argument("--cpu", type=float, default=1.0, help="%%CPU below this counts as idle")
    ap.add_argument("--all", action="store_true", help="also print each process's output paths")
    a = ap.parse_args()

    me = os.getpid()
    procs = [p for p in ps() if p["pid"] != me and kind(p["cmd"])]
    info = lsof([p["pid"] for p in procs])
    now = time.time()
    rows = []
    for p in procs:
        outs = outputs(p, info[p["pid"]])
        times = [t for t in (newest(o) for o in outs) if t is not None]
        age = now - max(times) if times else None
        idle = p["cpu"] < a.cpu
        quiet = (age if age is not None else p["secs"]) > a.minutes * 60
        rows.append((idle and quiet, p, kind(p["cmd"]), age, outs))
    rows.sort(key=lambda r: (not r[0], -r[1]["secs"]))

    print(f"{'':6} {'PID':>6} {'ELAPSED':>8} {'%CPU':>5} {'OUT AGE':>8}  KIND / COMMAND")
    for stale, p, k, age, outs in rows:
        flag = "STALE" if stale else ""
        age_s = fmt_secs(age) if age is not None else "none"
        cmd = p["cmd"] if len(p["cmd"]) <= 150 else p["cmd"][:147] + "..."
        print(f"{flag:6} {p['pid']:>6} {fmt_secs(p['secs']):>8} {p['cpu']:>5.1f} {age_s:>8}  {k}: {cmd}")
        if a.all:
            for o in outs:
                print(f"{'':38}out {o}")
    nstale = sum(r[0] for r in rows)
    print(
        f"\n{len(rows)} processes, {nstale} STALE (%CPU < {a.cpu:g} and no output change for "
        f"{a.minutes:g} min). Read-only: nothing was killed."
    )
    return 1 if nstale else 0


if __name__ == "__main__":
    sys.exit(main())
