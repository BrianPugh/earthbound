"""Run one scripted playtest.

Usage: uv run --no-sync python playtest/run.py NAME SAVE.srm SCRIPT [--cols N]

Boots SAVE (file 1 -> Continue), plays SCRIPT, then writes
playtest/out/NAME/{log.txt,sheet.png}. The script may use 'shot'/'state' lines.
"""

import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).parent
BIN = str(Path(__file__).resolve().parent.parent / "port/unix/build/earthbound")

BOOT = """\
300 -
1 START
200 -
1 A
120 -
1 A
120 -
1 A
240 -
state booted
shot booted
"""


def run(name, save, script, cols=6, boot=True):
    out = HERE / "out" / name
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    shutil.copy(save, out / "save.srm")
    (out / "input.txt").write_text((BOOT if boot else "") + script + "\n2 -\n")
    p = subprocess.run(  # noqa: S603
        [BIN, "--headless", "--save", "save.srm", "--input", "input.txt"],
        cwd=out,
        capture_output=True,
        text=True,
        timeout=600,
    )
    lines = [ln for ln in p.stdout.splitlines() if ln.startswith(("STATE", "SHOT"))]
    errs = [
        ln
        for ln in (p.stdout + p.stderr).splitlines()
        if any(k in ln.lower() for k in ("error", "warn", "halt", "unknown", "abort", "assert"))
    ]
    log = "\n".join(lines + [f"--- rc={p.returncode}"] + errs[:40])
    (out / "log.txt").write_text(log)

    shots = [ln.split() for ln in lines if ln.startswith("SHOT")]
    tiles = []
    for _, idx, label, frame in shots:
        bmp = out / "debug" / f"screenshot_{idx}.bmp"
        if bmp.exists() and bmp.stat().st_size > 0:
            tiles.append((Image.open(bmp).convert("RGB"), f"{idx} {label} {frame}"))
    if tiles:
        w, h = tiles[0][0].size
        rows = (len(tiles) + cols - 1) // cols
        sheet = Image.new("RGB", (w * cols, (h + 12) * rows), "white")
        d = ImageDraw.Draw(sheet)
        for i, (im, lab) in enumerate(tiles):
            x, y = (i % cols) * w, (i // cols) * (h + 12)
            sheet.paste(im, (x, y + 12))
            d.text((x + 2, y), lab, fill="black")
        sheet.save(out / "sheet.png")
    shutil.rmtree(out / "debug", ignore_errors=True)
    return log


if __name__ == "__main__":
    name, save, script = sys.argv[1:4]
    cols = int(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[4] == "--cols" else 6
    print(run(name, save, Path(script).read_text(), cols))
