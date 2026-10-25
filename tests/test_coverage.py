"""Smoke tests for ebtools coverage command."""

from pathlib import Path

import pytest

from ebtools.cli.coverage import (
    _extract_callroutine_handlers_from_c,
    _extract_callroutine_labels_from_asm,
    _parse_display_text_cases,
    _switch_has_default,
    coverage,
)


def test_extract_callroutine_labels_finds_known_labels():
    root = Path(__file__).parent.parent
    include_root = root / "include"
    asm_root = root / "asm"
    if not include_root.exists():
        pytest.skip("not run from a repo checkout")
    labels = _extract_callroutine_labels_from_asm(include_root, asm_root)
    assert "SPAWN_ENTITY" in labels
    assert "UPDATE_BATTLE_SCREEN_EFFECTS" in labels
    assert len(labels) > 100


def test_extract_callroutine_handlers_finds_known_handlers():
    root = Path(__file__).parent.parent
    src_root = root / "src"
    if not (src_root / "entity").exists():
        pytest.skip("not run from a repo checkout")
    handled_names, handled_addrs = _extract_callroutine_handlers_from_c(src_root)
    assert "SPAWN_ENTITY" in handled_names
    assert len(handled_names) > 100
    assert len(handled_addrs) > 100


def test_parse_display_text_cases_finds_dispatchers():
    root = Path(__file__).parent.parent
    if not (root / "src" / "game" / "display_text.c").exists():
        pytest.skip("not run from a repo checkout")
    main_cases, sub_cases = _parse_display_text_cases(root / "src")
    # Main loop handles the simple CCs and every tree prefix.
    assert {0x00, 0x01, 0x18, 0x19, 0x1F} <= main_cases
    # One sub-dispatcher per tree prefix 0x18..0x1F.
    assert set(sub_cases) == set(range(0x18, 0x20))
    assert all(sub_cases.values())


def test_coverage_all_dispatch_targets_handled():
    """Every asm dispatch target (callroutines, tick/entity callbacks, text CCs) has a C handler.

    Callroutines whose asm and C names differ are matched by address, which needs the linker map.
    """
    root = Path(__file__).parent.parent
    if not (root / "build" / "earthbound.map").exists():
        pytest.skip("needs build/earthbound.map (run `make`)")
    coverage(root)  # raises SystemExit(1) on any gap


def test_switch_has_default_detects_default():
    source = """
void entity_position_callback(int entity_offset) {
    switch (entities[entity_offset].pos_callback) {
    case CB_POS_SCREEN_BG1_Z:
        entity_screen_coords_bg1_with_z(entity_offset);
        break;
    default:
        entity_screen_coords_bg1(entity_offset);
        break;
    }
}
"""
    assert _switch_has_default(source, "CB_POS_") is True


def test_switch_has_default_no_default():
    source = """
void entity_position_callback(int entity_offset) {
    switch (entities[entity_offset].pos_callback) {
    case CB_POS_SCREEN_BG1_Z:
        entity_screen_coords_bg1_with_z(entity_offset);
        break;
    }
}
"""
    assert _switch_has_default(source, "CB_POS_") is False
