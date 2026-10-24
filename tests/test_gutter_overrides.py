"""Tests for the wide-viewport gutter override packer."""

import json
import struct
from pathlib import Path

import pytest

from ebtools.parsers.gutter_overrides import pack_gutter_overrides

REPO_JSON = Path(__file__).resolve().parent.parent / "src" / "custom_assets" / "maps" / "gutter_overrides.json"


def _write(tmp_path: Path, overrides: list) -> Path:
    p = tmp_path / "gutter_overrides.json"
    p.write_text(json.dumps({"overrides": overrides}))
    return p


def test_pack_layout(tmp_path):
    src = _write(tmp_path, [{"tileset_combo": 6, "block_x": 135, "block_y": 216, "blocks": [[24, 25], [36, 44]]}])
    out = tmp_path / "out.bin"
    assert pack_gutter_overrides(src, out) == 1
    data = out.read_bytes()
    assert struct.unpack_from("<H", data, 0) == (1,)
    assert struct.unpack_from("<HHBBBB", data, 2) == (135, 216, 2, 2, 6, 0)
    assert struct.unpack_from("<4H", data, 10) == (24, 25, 36, 44)
    assert len(data) == 18


@pytest.mark.parametrize(
    "entry",
    [
        {"tileset_combo": 6, "block_x": 255, "block_y": 0, "blocks": [[1, 1]]},  # off the right edge
        {"tileset_combo": 6, "block_x": 0, "block_y": 0, "blocks": [[1], [1, 2]]},  # ragged
        {"tileset_combo": 6, "block_x": 0, "block_y": 0, "blocks": [[1024]]},  # block id too big
        {"tileset_combo": 32, "block_x": 0, "block_y": 0, "blocks": [[1]]},  # combo too big
    ],
)
def test_pack_rejects_bad_entries(tmp_path, entry):
    with pytest.raises(ValueError):
        pack_gutter_overrides(_write(tmp_path, [entry]), tmp_path / "out.bin")


def test_repo_overrides_pack(tmp_path):
    assert pack_gutter_overrides(REPO_JSON, tmp_path / "out.bin") >= 1
