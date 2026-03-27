"""Tests for the text DSL opcode registry."""

import pytest

from ebtools.text_dsl.opcodes import (
    OPCODE_BY_BYTES,
    OPCODE_BY_NAME,
    OPCODES,
    ArgSpec,
    ArgType,
    OpcodeSpec,
)


class TestOpcodeTable:
    def test_no_duplicate_names(self):
        """Every opcode must have a unique yaml_name."""
        names = [op.yaml_name for op in OPCODES]
        assert len(names) == len(set(names)), f"Duplicate names: {[n for n in names if names.count(n) > 1]}"

    def test_no_duplicate_byte_sequences(self):
        """Every opcode must have a unique byte sequence."""
        byte_seqs = [tuple(op.bytes) for op in OPCODES]
        assert len(byte_seqs) == len(set(byte_seqs)), "Duplicate byte sequences found"

    def test_lookup_by_name(self):
        """OPCODE_BY_NAME should return the correct OpcodeSpec."""
        op = OPCODE_BY_NAME["line_break"]
        assert op.bytes == [0x00]
        assert op.args == []

        op = OPCODE_BY_NAME["set_event_flag"]
        assert op.bytes == [0x04]
        assert len(op.args) == 1
        assert op.args[0].name == "flag"
        assert op.args[0].type == ArgType.FLAG

        op = OPCODE_BY_NAME["open_window"]
        assert op.bytes == [0x18, 0x01]
        assert len(op.args) == 1
        assert op.args[0].type == ArgType.WINDOW

        op = OPCODE_BY_NAME["jump_if_flag_set"]
        assert op.bytes == [0x06]
        assert len(op.args) == 2
        assert op.args[0].type == ArgType.FLAG
        assert op.args[1].type == ArgType.LABEL

    def test_lookup_by_bytes(self):
        """OPCODE_BY_BYTES should return the correct OpcodeSpec."""
        op = OPCODE_BY_BYTES[(0x00,)]
        assert op.yaml_name == "line_break"

        op = OPCODE_BY_BYTES[(0x18, 0x01)]
        assert op.yaml_name == "open_window"

        op = OPCODE_BY_BYTES[(0x1F, 0x23)]
        assert op.yaml_name == "trigger_battle"
        assert op.args[0].type == ArgType.ENEMY_GROUP

        op = OPCODE_BY_BYTES[(0x1F, 0x02)]
        assert op.yaml_name == "play_sound"
        assert op.args[0].type == ArgType.SFX

    def test_all_opcodes_in_lookups(self):
        """Every opcode in OPCODES should be in both lookup dicts."""
        for op in OPCODES:
            assert op.yaml_name in OPCODE_BY_NAME, f"{op.yaml_name} missing from OPCODE_BY_NAME"
            assert tuple(op.bytes) in OPCODE_BY_BYTES, f"{op.bytes} missing from OPCODE_BY_BYTES"
            assert OPCODE_BY_NAME[op.yaml_name] is op
            assert OPCODE_BY_BYTES[tuple(op.bytes)] is op

    def test_lookup_sizes_match(self):
        """Lookup dicts should have the same number of entries as OPCODES."""
        assert len(OPCODE_BY_NAME) == len(OPCODES)
        assert len(OPCODE_BY_BYTES) == len(OPCODES)

    def test_all_bytes_non_empty(self):
        """Every opcode must have at least one byte."""
        for op in OPCODES:
            assert len(op.bytes) >= 1, f"{op.yaml_name} has no bytes"

    def test_two_byte_opcodes_start_with_prefix(self):
        """Two-byte opcodes should start with 0x18-0x1F."""
        for op in OPCODES:
            if len(op.bytes) == 2:
                assert 0x18 <= op.bytes[0] <= 0x1F, (
                    f"{op.yaml_name} has 2 bytes but first byte {op.bytes[0]:#x} is not in 0x18-0x1F"
                )

    def test_jump_table_arg_type(self):
        """Opcodes with JUMP_TABLE args should be jump_multi and jump_multi2."""
        jump_table_ops = [op for op in OPCODES if any(a.type == ArgType.JUMP_TABLE for a in op.args)]
        names = {op.yaml_name for op in jump_table_ops}
        assert names == {"jump_multi", "jump_multi2"}

    def test_string_arg_type(self):
        """load_string_to_memory should have a STRING argument."""
        op = OPCODE_BY_NAME["load_string_to_memory"]
        assert any(a.type == ArgType.STRING for a in op.args)

    def test_symbolic_arg_types_present(self):
        """Check that symbolic arg types are used for the appropriate opcodes."""
        # MUSIC
        op = OPCODE_BY_NAME["play_music"]
        assert any(a.type == ArgType.MUSIC for a in op.args)
        # SFX
        op = OPCODE_BY_NAME["play_sound"]
        assert any(a.type == ArgType.SFX for a in op.args)
        # SPRITE
        op = OPCODE_BY_NAME["generate_active_sprite"]
        assert any(a.type == ArgType.SPRITE for a in op.args)
        # MOVEMENT
        op = OPCODE_BY_NAME["generate_active_sprite"]
        assert any(a.type == ArgType.MOVEMENT for a in op.args)
        # ENEMY_GROUP
        op = OPCODE_BY_NAME["trigger_battle"]
        assert any(a.type == ArgType.ENEMY_GROUP for a in op.args)
        # PARTY
        op = OPCODE_BY_NAME["add_party_member"]
        assert any(a.type == ArgType.PARTY for a in op.args)
        # ITEM
        op = OPCODE_BY_NAME["print_item_name"]
        assert any(a.type == ArgType.ITEM for a in op.args)
        # FLAG
        op = OPCODE_BY_NAME["set_event_flag"]
        assert any(a.type == ArgType.FLAG for a in op.args)
        # WINDOW
        op = OPCODE_BY_NAME["open_window"]
        assert any(a.type == ArgType.WINDOW for a in op.args)
        # STATUS_GROUP
        op = OPCODE_BY_NAME["character_has_ailment"]
        assert any(a.type == ArgType.STATUS_GROUP for a in op.args)
