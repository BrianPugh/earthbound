"""playtest/lost_frames.py: the harness side of the lost-frame check (src/core/lost_frame_check.h)."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "playtest"))
import lost_frames  # noqa: E402


def test_mode_names_cover_the_enum():
    names = lost_frames.mode_names()
    header = (ROOT / "src/core/mode_stack.h").read_text()
    body = re.search(r"typedef enum \{(.*?)\} GameMode;", header, re.S).group(1)
    assert names[0] == "NONE"
    assert names[max(names)] == "COUNT"
    assert len(names) == len(re.findall(r"^\s*GAME_MODE_\w+", body, re.M))


def test_scan():
    names = lost_frames.mode_names()
    log = (
        "STATE x\n"
        "LOST-FRAME frame=9 lost=1 owed=1 claimed=0 work=1 modes=1.0>2.3 events=[T D1@window.c:10 W]\n"
        "lost-frame check: 1 events\n"
    )
    lost, checked = lost_frames.scan(log)
    assert checked
    assert len(lost) == 1
    assert f"modes={names[1]}.0>{names[2]}.3 " in lost[0]
    assert lost_frames.scan("lost-frame check: 0 events\n") == ([], True)
    assert lost_frames.scan("STATE x\n") == ([], False)
