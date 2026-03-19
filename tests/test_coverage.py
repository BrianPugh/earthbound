"""Smoke tests for ebtools coverage command."""

from pathlib import Path

from ebtools.cli.coverage import (
    _extract_callroutine_handlers_from_c,
    _extract_callroutine_labels_from_asm,
    _switch_has_default,
)


def test_extract_callroutine_labels_finds_known_labels():
    root = Path(__file__).parent.parent
    include_root = root / "include"
    asm_root = root / "src"
    if not include_root.exists():
        return  # skip if not in repo root
    labels = _extract_callroutine_labels_from_asm(include_root, asm_root)
    assert "SPAWN_ENTITY" in labels
    assert "UPDATE_BATTLE_SCREEN_EFFECTS" in labels
    assert len(labels) > 100


def test_extract_callroutine_handlers_finds_known_handlers():
    root = Path(__file__).parent.parent
    port_root = root / "port" / "src"
    if not port_root.exists():
        return
    handled_names, handled_addrs = _extract_callroutine_handlers_from_c(port_root)
    assert "SPAWN_ENTITY" in handled_names
    assert len(handled_names) > 100
    assert len(handled_addrs) > 100


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
