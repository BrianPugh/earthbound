"""Record a sweep segment in chunks that stay aligned with the original's lag.

The original reads a late pad on the frame after a lag-merged frame (autodiff.py
``# late-pads``), so a port recording whose later moves were planned on the pads the port
read can go off route once autodiff replays the port on the pads the original read. This
keeps the recording session on those pads: record a chunk, `check` it against the emu, and
`replay` the session on the late pads before planning the next chunk.

Layout: DIR/head.play is the scenario header (comments, setup, ``# late-pads`` and the
``---`` line); DIR/NNN.pad are padlog chunks in order (port pads as recorded).

Usage::

    rec.py DIR build OUT.play        # head + every chunk's body -> OUT.play
    rec.py DIR check OUT.play        # build, run autodiff (prints the report head)
    rec.py DIR replay SESSION SAVE   # restart SESSION on SAVE, replay the setup and the late
                                     # pads of the last check, open padlog DIR/<next>.pad
    rec.py DIR verify SESSION        # does the late-pads port run (last check) end where
                                     # the recording session is?
    rec.py DIR truncate N            # keep the first N frames of the chunks (cut the rest)
    rec.py DIR drop                  # delete the last chunk
    rec.py DIR import SCEN.play      # start DIR from a scenario (a segment in progress):
                                     # its header -> head.play, its body -> 000.pad
    rec.py DIR emu-frame N [N ...]   # the emu logic frame that port frame N compares with
                                     # under head.play's lags (`# lag:` takes emu frames)
"""

import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))
import autodiff  # noqa: E402
from padlog_body import BITS, pad_name, runs  # noqa: E402

PT = [sys.executable, str(ROOT / "playtest/pt.py")]
# The recording session must run the binary autodiff runs (a 256x224 build)
ENV = dict(os.environ, EB_BIN=os.environ.get("EB_BIN", str(autodiff.DEFAULT_BIN.resolve())))
SEED = "rng 4660 22136; fc 0"


def chunks(d):
    return sorted(d.glob("[0-9][0-9][0-9].pad"))


def body_pads(d):
    pads = []
    for c in chunks(d):
        pads += [pad_name(int(t, 16)) for t in c.read_text().split()]
    return pads


def build(d, out):
    items = [f"{n} {p}" for n, p in runs(body_pads(d))]
    lines = ["; ".join(items[i : i + 10]) for i in range(0, len(items), 10)]
    Path(out).write_text((d / "head.play").read_text().rstrip("\n") + "\n" + "\n".join(lines) + "\n")


def outdir(d):
    return d / "ad"


def check(d, out):
    build(d, out)
    rep = subprocess.run(  # noqa: S603
        # A long segment's emu side can pass autodiff's 300 s default on a loaded machine; the
        # timed-out run then reports a bogus divergence (and a shorter LAG line)
        [
            sys.executable,
            str(HERE.parent / "autodiff.py"),
            str(out),
            "--out",
            str(outdir(d)),
            "--bin",
            ENV["EB_BIN"],
            "--timeout",
            "3600",
        ],
        capture_output=True,
        text=True,
        cwd=ROOT,
    ).stdout
    keep = [ln for ln in rep.splitlines() if "merged by lag" not in ln]
    print("\n".join(keep[:16]))


def effective_pads(d, scen):
    """The pads autodiff's late-pads rerun gave the port in the last check."""
    text = Path(scen).read_text()
    lags = {int(k) for k in autodiff.LAG_HEADER.findall(text)}
    extras = {int(k) for ks in autodiff.EXTRA_HEADER.findall(text) for k in ks.split()}
    pads = body_pads(d)
    log = outdir(d) / f"{Path(scen).stem}.emu.log"
    text = log.read_text(errors="replace")
    steps = sum(1 for ln in text.split("\n> SEED\n", 1)[-1].splitlines() if ln.startswith("LSTEP "))
    if steps < len(autodiff.emu_pads(pads, lags, extras)):
        sys.exit(f"{log} is older than the chunks: run check first")
    return autodiff.late_pads(pads, lags, extras, text)


def replay(d, session, save, scen):
    pads = effective_pads(d, scen) if chunks(d) else []
    head = (d / "head.play").read_text().splitlines()
    setup = [c.strip() for raw in head[: head.index("---")] for c in raw.split("#", 1)[0].split(";")]
    setup = autodiff.side_lines(setup, "port")
    body = "; ".join(f"{n} {p}" for n, p in runs(pads))
    nxt = d / f"{len(chunks(d)):03d}.pad"
    subprocess.run(PT + ["stop", session], capture_output=True, cwd=ROOT, env=ENV)  # noqa: S603
    subprocess.run(  # noqa: S603
        PT + ["start", session, "--save", save], capture_output=True, cwd=ROOT, check=True, env=ENV
    )
    cmds = "; ".join(setup + [SEED] + ([body] if body else []) + [f"padlog {nxt}"])
    out = subprocess.run(  # noqa: S603
        PT + ["do", session, cmds, "--timeout", "1800"], capture_output=True, text=True, cwd=ROOT, env=ENV
    ).stdout
    print("\n".join(ln for ln in out.splitlines() if ln.startswith(("STATE", "PARTY", "NEAR", "SHOT"))))
    print(f"recording into {nxt}")


def verify(d, scen, session):
    """Check the late-pads port run of the last check ends where the recording session is.

    The session played the late pads of the chunks before the last replay and the last
    chunk as recorded; the check's port run plays the late pads of everything. If the last
    chunk's late pads changed the route, the 2 end apart: truncate the chunk before the
    change and record the rest again.
    """
    log = (ROOT / "playtest/out/sessions" / session / "log.txt").read_text(errors="replace")
    sess = [ln for ln in log.splitlines() if ln.startswith("STATE ")][-1]
    b = autodiff.parse_snapshots((outdir(d) / f"{Path(scen).stem}.port.log").read_text(errors="replace"))
    late = b[max(b)]["raw_state"]

    def key(line):
        m = re.search(r"modes=\[[^\]]*\] .*? pos=\(\d+,\d+\)", line)
        money = re.search(r" money=\d+", line)
        return (m.group(0) if m else line) + (money.group(0) if money else "")

    print(f"session: {key(sess)}\nlate:    {key(late)}\n" + ("SAME" if key(sess) == key(late) else "DIFFERENT"))


def truncate(d, n):
    left = n
    for c in chunks(d):
        words = c.read_text().split()
        if left <= 0:
            c.unlink()
        elif len(words) > left:
            c.write_text("\n".join(words[:left]) + "\n")
            left = 0
        else:
            left -= len(words)


def import_scenario(d, scen):
    lines = Path(scen).read_text().splitlines()
    cut = lines.index("---")
    d.mkdir(parents=True, exist_ok=True)
    (d / "head.play").write_text("\n".join(lines[: cut + 1]) + "\n")
    names = autodiff.expand_body([c for line in lines[cut + 1 :] for c in line.split(";")])
    bits = dict(BITS)
    words = []
    for name in names:
        v = 0
        for b in ("" if name == "-" else name).split("+"):
            if b:
                v |= 1 << bits[b]
        words.append(f"{v:04x}")
    (d / "000.pad").write_text("\n".join(words) + "\n")


def main():
    d, cmd, args = Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
    scen = d / "scenario.play"
    if cmd == "build":
        build(d, args[0])
    elif cmd == "check":
        check(d, args[0] if args else scen)
    elif cmd == "replay":
        replay(d, args[0], args[1], args[2] if len(args) > 2 else scen)
    elif cmd == "verify":
        verify(d, scen, args[0])
    elif cmd == "truncate":
        truncate(d, int(args[0]))
    elif cmd == "import":
        import_scenario(d, args[0])
    elif cmd == "emu-frame":
        text = (d / "head.play").read_text()
        lags = {int(k) for k in autodiff.LAG_HEADER.findall(text)}
        extras = {int(k) for ks in autodiff.EXTRA_HEADER.findall(text) for k in ks.split()}
        for n in map(int, args):
            j = n
            while autodiff.emu_to_port(j, lags, extras) != n:
                j += 1
            print(f"port {n} -> emu {j}")
    elif cmd == "drop":
        chunks(d)[-1].unlink()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
