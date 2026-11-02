"""Read the lost-frame check's output (src/core/lost_frame_check.h) from a game log.

A build with ``-DEB_LOST_FRAME_CHECK=ON`` (default in port/unix Debug builds) prints a
``LOST-FRAME`` line to stderr for every frame where an inline window tick or bare wait was
never presented, and ``lost-frame check: N events`` at exit. regress.py and autodiff.py fail
a run that has any; other builds print neither, and the harness says the check was off.
"""

import re
from functools import cache
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOST = re.compile(r"^LOST-FRAME .*$", re.M)
SUMMARY = re.compile(r"^lost-frame check: (\d+) events", re.M)


@cache
def mode_names():
    """GameMode value -> name, from the sequential enum in src/core/mode_stack.h."""
    text = (ROOT / "src/core/mode_stack.h").read_text()
    body = re.search(r"typedef enum \{(.*?)\} GameMode;", text, re.S)
    names, n = {}, 0
    for m in re.finditer(r"^\s*GAME_MODE_(\w+)\s*(?:=\s*(\d+))?\s*,", body.group(1) if body else "", re.M):
        n = int(m.group(2)) if m.group(2) else n
        names[n] = m.group(1)
        n += 1
    return names


def _name_modes(line):
    def one(m):
        mode = int(m.group(1))
        return f"{mode_names().get(mode, mode)}.{m.group(2)}"

    return re.sub(r"modes=(\S+)", lambda m: "modes=" + re.sub(r"(\d+)\.(\d+)", one, m.group(1)), line)


def scan(text):
    """(LOST-FRAME lines with mode names filled in, whether the check was compiled in)."""
    return [_name_modes(ln) for ln in LOST.findall(text)], bool(SUMMARY.search(text)) or bool(LOST.search(text))
