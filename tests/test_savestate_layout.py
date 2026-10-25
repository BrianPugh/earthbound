"""The committed savestate layout fingerprint must match the serialized types."""

import importlib.util
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "src" / "tools" / "savestate_layout.py"


def _load():
    spec = importlib.util.spec_from_file_location("savestate_layout", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_layout_header_is_current():
    layout = _load()
    value, texts = layout.fingerprint()
    have = layout.HEADER.read_text(encoding="utf-8")
    assert have == layout.render(value, len(texts)), (
        "src/core/state_layout.h is stale: run `uv run --no-sync python src/tools/savestate_layout.py`"
    )


def test_layout_covers_mode_state_and_sections():
    layout = _load()
    index = layout.build_index()
    texts = set(layout.closure(index))
    for name in ("ModeStack", "ModeState", "GameMode", "PPUState", "BattleState", "OvalWindowSaveState"):
        assert index.types[name] & texts, name


def test_comment_edits_do_not_change_the_hash():
    layout = _load()
    code, _ = layout._strip_comments("typedef struct { int a; /* x */ int b; // y\n} T;")
    assert layout._normalize(code) == "typedef struct { int a; int b; } T;"
