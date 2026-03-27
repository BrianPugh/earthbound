"""Tests for the text DSL opcode registry, decoder, and compiler."""

import pytest

from ebtools.text_dsl.compiler import compile_text_block
from ebtools.text_dsl.decoder import decode_text_block
from ebtools.text_dsl.opcodes import (
    OPCODE_BY_BYTES,
    OPCODE_BY_NAME,
    OPCODES,
    ArgSpec,
    ArgType,
    OpcodeSpec,
)
from ebtools.text_dsl.yaml_io import (
    deserialize_dialogue_file,
    serialize_dialogue_file,
)


class TestOpcodeTable:
    def test_no_duplicate_names(self):
        """Every opcode must have a unique yaml_name."""
        names = [op.yaml_name for op in OPCODES]
        assert len(names) == len(set(names)), f"Duplicate names: {[n for n in names if names.count(n) > 1]}"

    def test_no_duplicate_byte_sequences(self):
        """Every opcode must have a unique byte sequence."""
        byte_seqs = [op.bytes for op in OPCODES]
        assert len(byte_seqs) == len(set(byte_seqs)), "Duplicate byte sequences found"

    def test_lookup_by_name(self):
        """OPCODE_BY_NAME should return the correct OpcodeSpec."""
        op = OPCODE_BY_NAME["line_break"]
        assert op.bytes == (0x00,)
        assert op.args == ()

        op = OPCODE_BY_NAME["set_event_flag"]
        assert op.bytes == (0x04,)
        assert len(op.args) == 1
        assert op.args[0].name == "flag"
        assert op.args[0].type == ArgType.FLAG

        op = OPCODE_BY_NAME["open_window"]
        assert op.bytes == (0x18, 0x01)
        assert len(op.args) == 1
        assert op.args[0].type == ArgType.WINDOW

        op = OPCODE_BY_NAME["jump_if_flag_set"]
        assert op.bytes == (0x06,)
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
            assert op.bytes in OPCODE_BY_BYTES, f"{op.bytes} missing from OPCODE_BY_BYTES"
            assert OPCODE_BY_NAME[op.yaml_name] is op
            assert OPCODE_BY_BYTES[op.bytes] is op

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


class TestDecoder:
    def test_simple_text_and_end(self):
        # 'H'=0x78, 'i'=0x99, end_block=0x02
        bytecode = bytes([0x78, 0x99, 0x02])
        text_table = {0x78: "H", 0x99: "i"}
        result = decode_text_block(bytecode, text_table)
        assert result == [{"op": "text", "value": "Hi"}, {"op": "end_block"}]

    def test_control_code_with_args(self):
        # pause(30), end_block
        bytecode = bytes([0x10, 0x1E, 0x02])
        result = decode_text_block(bytecode, {})
        assert result == [{"op": "pause", "frames": 30}, {"op": "end_block"}]

    def test_event_flag(self):
        # set_event_flag(5), end_block
        bytecode = bytes([0x04, 0x05, 0x00, 0x02])
        result = decode_text_block(bytecode, {})
        assert result == [{"op": "set_event_flag", "flag": 5}, {"op": "end_block"}]

    def test_two_byte_opcode(self):
        # close_window (0x18, 0x00), end_block
        bytecode = bytes([0x18, 0x00, 0x02])
        result = decode_text_block(bytecode, {})
        assert result == [{"op": "close_window"}, {"op": "end_block"}]

    def test_compressed_text_expanded(self):
        # CC 0x15 index 0x3D -> "I " from compressed dict
        bytecode = bytes([0x15, 0x3D, 0x02])
        compressed = {0x003D: "I "}
        result = decode_text_block(bytecode, {}, compressed_text=compressed)
        assert result == [{"op": "text", "value": "I "}, {"op": "end_block"}]

    def test_compressed_text_bank_1(self):
        # CC 0x16 index 0x05 -> key = 256 + 5 = 261
        bytecode = bytes([0x16, 0x05, 0x02])
        compressed = {261: "the "}
        result = decode_text_block(bytecode, {}, compressed_text=compressed)
        assert result == [{"op": "text", "value": "the "}, {"op": "end_block"}]

    def test_compressed_text_bank_2(self):
        # CC 0x17 index 0x0A -> key = 512 + 10 = 522
        bytecode = bytes([0x17, 0x0A, 0x02])
        compressed = {522: "and "}
        result = decode_text_block(bytecode, {}, compressed_text=compressed)
        assert result == [{"op": "text", "value": "and "}, {"op": "end_block"}]

    def test_text_flushed_before_control(self):
        # Text, then control code — text should be flushed into its own dict
        bytecode = bytes([0x78, 0x00, 0x02])
        text_table = {0x78: "H"}
        result = decode_text_block(bytecode, text_table)
        assert result == [
            {"op": "text", "value": "H"},
            {"op": "line_break"},
            {"op": "end_block"},
        ]

    def test_unknown_opcode(self):
        # Byte 0xFF is not in text_table or OPCODE_BY_BYTES
        bytecode = bytes([0xFF, 0x02])
        result = decode_text_block(bytecode, {})
        assert result[0] == {"op": "unknown", "bytes": [0xFF]}
        assert result[1] == {"op": "end_block"}

    def test_unknown_two_byte_opcode(self):
        # 0x18, 0xFF is not a known two-byte opcode
        bytecode = bytes([0x18, 0xFF, 0x02])
        result = decode_text_block(bytecode, {})
        assert result[0] == {"op": "unknown", "bytes": [0x18, 0xFF]}
        assert result[1] == {"op": "end_block"}

    def test_jump_multi(self):
        # jump_multi with 2 targets
        bytecode = bytes(
            [
                0x09,  # jump_multi
                0x02,  # count = 2
                0x10,
                0x00,
                0x00,
                0xC0,  # target 1 = 0xC0000010
                0x20,
                0x00,
                0x00,
                0xC0,  # target 2 = 0xC0000020
                0x02,  # end_block
            ]
        )
        result = decode_text_block(bytecode, {})
        assert result == [
            {"op": "jump_multi", "targets": [0xC0000010, 0xC0000020]},
            {"op": "end_block"},
        ]

    def test_string_arg_end_terminator(self):
        # load_string_to_memory (0x19, 0x02) with end terminator (0x00)
        bytecode = bytes([0x19, 0x02, 0x41, 0x42, 0x00, 0x02])
        result = decode_text_block(bytecode, {})
        assert result[0] == {
            "op": "load_string_to_memory",
            "payload": {"text": [0x41, 0x42], "terminator": "end"},
        }

    def test_string_arg_select_script_terminator(self):
        # load_string_to_memory with select_script terminator (0x01) + label
        bytecode = bytes(
            [
                0x19,
                0x02,
                0x41,  # one text byte
                0x01,  # select_script terminator
                0x78,
                0x56,
                0x34,
                0x12,  # label = 0x12345678
                0x02,  # end_block
            ]
        )
        result = decode_text_block(bytecode, {})
        assert result[0] == {
            "op": "load_string_to_memory",
            "payload": {"text": [0x41], "terminator": "select_script", "label": 0x12345678},
        }

    def test_string_arg_store_terminator(self):
        # load_string_to_memory with store terminator (0x02)
        bytecode = bytes([0x19, 0x02, 0x41, 0x02, 0x02])
        result = decode_text_block(bytecode, {})
        # First 0x02 is the store terminator for the STRING arg,
        # second 0x02 is end_block.
        assert result[0]["op"] == "load_string_to_memory"
        assert result[0]["payload"]["terminator"] == "store"
        assert result[1] == {"op": "end_block"}

    def test_mixed_text_and_compressed(self):
        # Text char, compressed text, more text — all merged into one text entry
        bytecode = bytes([0x78, 0x15, 0x3D, 0x99, 0x02])
        text_table = {0x78: "H", 0x99: "i"}
        compressed = {0x003D: "ello "}
        result = decode_text_block(bytecode, text_table, compressed_text=compressed)
        assert result == [
            {"op": "text", "value": "Hello i"},
            {"op": "end_block"},
        ]

    def test_empty_input(self):
        result = decode_text_block(b"", {})
        assert result == []

    def test_label_arg(self):
        # call_text (0x08) with a LABEL argument
        bytecode = bytes([0x08, 0x00, 0x10, 0x00, 0xC0, 0x02])
        result = decode_text_block(bytecode, {})
        assert result == [
            {"op": "call_text", "dest": 0xC0001000},
            {"op": "end_block"},
        ]

    def test_u24_arg(self):
        # give_experience (0x1E, 0x09) with U8 character + U24 amount
        bytecode = bytes([0x1E, 0x09, 0x01, 0x00, 0x01, 0x00, 0x02])
        result = decode_text_block(bytecode, {})
        assert result == [
            {"op": "give_experience", "character": 1, "amount": 256},
            {"op": "end_block"},
        ]


class TestCompiler:
    def test_simple_text_and_end(self):
        """Compile text 'Hi' + end_block produces correct bytes."""
        ops = [{"op": "text", "value": "Hi"}, {"op": "end_block"}]
        reverse_table = {"H": 0x78, "i": 0x99}
        result = compile_text_block(ops, reverse_table)
        assert result == bytes([0x78, 0x99, 0x02])

    def test_control_code_with_args(self):
        """pause(30) + end_block produces correct bytes."""
        ops = [{"op": "pause", "frames": 30}, {"op": "end_block"}]
        result = compile_text_block(ops, {})
        assert result == bytes([0x10, 0x1E, 0x02])

    def test_event_flag(self):
        """set_event_flag(5) + end_block produces correct bytes."""
        ops = [{"op": "set_event_flag", "flag": 5}, {"op": "end_block"}]
        result = compile_text_block(ops, {})
        assert result == bytes([0x04, 0x05, 0x00, 0x02])

    def test_two_byte_opcode(self):
        """close_window + end_block produces correct bytes."""
        ops = [{"op": "close_window"}, {"op": "end_block"}]
        result = compile_text_block(ops, {})
        assert result == bytes([0x18, 0x00, 0x02])

    def test_round_trip(self):
        """Decode then compile produces identical bytecode."""
        text_table = {0x78: "H", 0x99: "i"}
        reverse_table = {"H": 0x78, "i": 0x99}

        test_cases = [
            bytes([0x78, 0x99, 0x02]),
            bytes([0x10, 0x1E, 0x02]),
            bytes([0x04, 0x05, 0x00, 0x02]),
            bytes([0x18, 0x00, 0x02]),
            bytes(
                [
                    0x09,
                    0x02,
                    0x10,
                    0x00,
                    0x00,
                    0xC0,
                    0x20,
                    0x00,
                    0x00,
                    0xC0,
                    0x02,
                ]
            ),
            bytes([0x08, 0x00, 0x10, 0x00, 0xC0, 0x02]),
            bytes([0x1E, 0x09, 0x01, 0x00, 0x01, 0x00, 0x02]),
            bytes([0x19, 0x02, 0x41, 0x42, 0x00, 0x02]),
            bytes([0x19, 0x02, 0x41, 0x02, 0x02]),
            bytes([0x78, 0x00, 0x99, 0x02]),
        ]

        for bytecode in test_cases:
            decoded = decode_text_block(bytecode, text_table)
            recompiled = compile_text_block(decoded, reverse_table)
            assert recompiled == bytecode, (
                f"Round-trip failed for {bytecode.hex()}: decoded={decoded}, recompiled={recompiled.hex()}"
            )

    def test_label_resolution(self):
        """Compile with string label resolved via label_offsets."""
        ops = [
            {"op": "call_text", "dest": "my_label"},
            {"op": "end_block"},
        ]
        label_offsets = {"my_label": 0xC0001000}
        result = compile_text_block(ops, {}, label_offsets=label_offsets)
        assert result == bytes([0x08, 0x00, 0x10, 0x00, 0xC0, 0x02])

    def test_unknown_passthrough(self):
        """Unknown opcodes pass through raw bytes."""
        ops = [{"op": "unknown", "bytes": [0xFF]}, {"op": "end_block"}]
        result = compile_text_block(ops, {})
        assert result == bytes([0xFF, 0x02])

    def test_jump_table_with_labels(self):
        """JUMP_TABLE args resolve string labels."""
        ops = [
            {"op": "jump_multi", "targets": ["label_a", "label_b"]},
            {"op": "end_block"},
        ]
        label_offsets = {"label_a": 0xC0000010, "label_b": 0xC0000020}
        result = compile_text_block(ops, {}, label_offsets=label_offsets)
        assert result == bytes(
            [
                0x09,
                0x02,
                0x10,
                0x00,
                0x00,
                0xC0,
                0x20,
                0x00,
                0x00,
                0xC0,
                0x02,
            ]
        )

    def test_string_arg_select_script(self):
        """STRING arg with select_script terminator and label."""
        ops = [
            {
                "op": "load_string_to_memory",
                "payload": {"text": [0x41], "terminator": "select_script", "label": 0x12345678},
            },
            {"op": "end_block"},
        ]
        result = compile_text_block(ops, {})
        assert result == bytes(
            [
                0x19,
                0x02,
                0x41,
                0x01,
                0x78,
                0x56,
                0x34,
                0x12,
                0x02,
            ]
        )


class TestYamlIO:
    def test_serialize_simple(self):
        """Text + end_block serializes to YAML with 'text:' and bare 'end_block'."""
        messages = {
            "label_01": [
                {"op": "text", "value": "Hello!"},
                {"op": "end_block"},
            ]
        }
        yaml_str = serialize_dialogue_file(messages)
        assert "text: Hello!" in yaml_str or "text: 'Hello!'" in yaml_str
        assert "- end_block" in yaml_str

    def test_serialize_opcode_with_args(self):
        """Single-arg opcode serializes as {op: value}."""
        messages = {
            "label_01": [
                {"op": "pause", "frames": 30},
                {"op": "end_block"},
            ]
        }
        yaml_str = serialize_dialogue_file(messages)
        assert "pause: 30" in yaml_str

    def test_deserialize(self):
        """YAML string deserializes back to opcode dicts."""
        yaml_str = "label_01:\n- text: Hello!\n- pause: 30\n- end_block\n"
        result = deserialize_dialogue_file(yaml_str)
        assert result == {
            "label_01": [
                {"op": "text", "value": "Hello!"},
                {"op": "pause", "frames": 30},
                {"op": "end_block"},
            ]
        }

    def test_round_trip(self):
        """Serialize -> deserialize produces identical data."""
        messages = {
            "label_01": [
                {"op": "text", "value": "Hello!"},
                {"op": "pause", "frames": 30},
                {"op": "line_break"},
                {"op": "text", "value": "World"},
                {"op": "end_block"},
            ]
        }
        yaml_str = serialize_dialogue_file(messages)
        result = deserialize_dialogue_file(yaml_str)
        assert result == messages

    def test_multi_arg_opcode(self):
        """Multi-arg opcodes serialize as {op: {arg1: val1, arg2: val2}}."""
        messages = {
            "label_01": [
                {"op": "jump_if_flag_set", "flag": 5, "dest": 12345},
                {"op": "end_block"},
            ]
        }
        yaml_str = serialize_dialogue_file(messages)
        result = deserialize_dialogue_file(yaml_str)
        assert result == messages

    def test_round_trip_multi_labels(self):
        """Round-trip with multiple labels preserves all data."""
        messages = {
            "greeting": [
                {"op": "text", "value": "Hi there!"},
                {"op": "halt_with_prompt"},
                {"op": "end_block"},
            ],
            "farewell": [
                {"op": "text", "value": "Bye!"},
                {"op": "end_block"},
            ],
        }
        yaml_str = serialize_dialogue_file(messages)
        result = deserialize_dialogue_file(yaml_str)
        assert result == messages

    def test_deserialize_empty(self):
        """Empty YAML string deserializes to empty dict."""
        assert deserialize_dialogue_file("") == {}
        assert deserialize_dialogue_file("{}") == {}


class TestEndToEnd:
    def test_item_inline_text_round_trip(self):
        """Item with inline help_text packs to binary with string table offset."""
        from ebtools.text_dsl.decoder import decode_text_block
        from ebtools.text_dsl.string_table import StringTableBuilder

        text_table = {0x78: "H", 0x99: "i", 0x50: " "}
        reverse_table = {v: k for k, v in text_table.items()}
        builder = StringTableBuilder(reverse_table)

        offset = builder.add("Hi")
        assert offset == 0
        assert offset < 0xC00000  # new-format range

        data = builder.build()
        decoded = decode_text_block(data, text_table)
        assert decoded == [
            {"op": "text", "value": "Hi"},
            {"op": "end_block"},
        ]

    def test_dialogue_yaml_full_round_trip(self):
        """YAML dialogue -> compile -> decode -> re-serialize produces same data."""
        text_table = {0x78: "H", 0x99: "i", 0x50: " "}
        reverse_table = {v: k for k, v in text_table.items()}

        messages = {
            "MSG_GREETING": [
                {"op": "text", "value": "Hi"},
                {"op": "pause", "frames": 30},
                {"op": "text", "value": " Hi"},
                {"op": "end_block"},
            ]
        }

        yaml_str = serialize_dialogue_file(messages)
        loaded = deserialize_dialogue_file(yaml_str)
        assert loaded == messages

        for _label, ops in loaded.items():
            compiled = compile_text_block(ops, reverse_table)
            decoded = decode_text_block(compiled, text_table)
            assert decoded == ops

    def test_inline_text_with_control_codes(self):
        """Inline text with {control_code} round-trips through string table."""
        from ebtools.text_dsl.decoder import decode_text_block
        from ebtools.text_dsl.string_table import StringTableBuilder

        text_table = {0x50: " ", 0x78: "H", 0x99: "i"}
        reverse_table = {v: k for k, v in text_table.items()}
        builder = StringTableBuilder(reverse_table)

        offset = builder.add("Hi{pause 30} Hi")
        data = builder.build()
        decoded = decode_text_block(data[offset:], text_table)

        # Should have: text "Hi", pause 30, text " Hi", end_block
        assert decoded[0] == {"op": "text", "value": "Hi"}
        assert decoded[1] == {"op": "pause", "frames": 30}
        assert decoded[2] == {"op": "text", "value": " Hi"}
        assert decoded[3] == {"op": "end_block"}


class TestInlineText:
    def test_plain_string(self):
        """Plain 'Hi' encodes to text bytes + end_block."""
        from ebtools.text_dsl.inline import encode_inline_text

        reverse_table = {"H": 0x78, "i": 0x99}
        result = encode_inline_text("Hi", reverse_table)
        assert result == bytes([0x78, 0x99, 0x02])

    def test_control_code_interpolation(self):
        """Text with {pause 30} interpolation encodes correctly."""
        from ebtools.text_dsl.inline import encode_inline_text

        reverse_table = {"a": 0x91, "t": 0xA0, "e": 0x95, " ": 0x50}
        result = encode_inline_text("ate {pause 30}", reverse_table)
        assert result == bytes([0x91, 0xA0, 0x95, 0x50, 0x10, 0x1E, 0x02])

    def test_print_char_name(self):
        """{print_char_name 0} encodes correctly."""
        from ebtools.text_dsl.inline import encode_inline_text

        result = encode_inline_text("{print_char_name 0}", {})
        assert result == bytes([0x1C, 0x02, 0x00, 0x02])

    def test_is_simple_text_true(self):
        """Ops with only text + end_block are simple."""
        from ebtools.text_dsl.inline import is_simple_text

        ops = [{"op": "text", "value": "Hello"}, {"op": "end_block"}]
        assert is_simple_text(ops) is True

    def test_is_simple_text_false(self):
        """Ops with jump_if_flag_set are not simple."""
        from ebtools.text_dsl.inline import is_simple_text

        ops = [
            {"op": "text", "value": "Hello"},
            {"op": "jump_if_flag_set", "flag": 5, "dest": 12345},
            {"op": "end_block"},
        ]
        assert is_simple_text(ops) is False

    def test_hex_args(self):
        """{pause 0x1E} works the same as {pause 30}."""
        from ebtools.text_dsl.inline import encode_inline_text

        result = encode_inline_text("{pause 0x1E}", {})
        assert result == bytes([0x10, 0x1E, 0x02])


class TestModelUpdates:
    """Test that pydantic model updates for inline text fields work correctly."""

    def test_item_legacy_pointer(self):
        """Item with only help_text_pointer (legacy format) validates."""
        from ebtools.parsers.item import Item, ItemParams

        item = Item(
            id=1,
            symbol="CRACKED_BAT",
            name="Cracked bat",
            type=0,
            cost=18,
            flags=0,
            effect=0,
            params=ItemParams(strength=4, epi=0, ep=0, special=0),
            help_text_pointer="0xC539C4",
        )
        assert item.help_text_pointer == "0xC539C4"
        assert item.help_text is None
        assert item.help_text_ref is None

    def test_item_inline_text(self):
        """Item with help_text (no pointer) validates."""
        from ebtools.parsers.item import Item, ItemParams

        item = Item(
            id=1,
            symbol="CRACKED_BAT",
            name="Cracked bat",
            type=0,
            cost=18,
            flags=0,
            effect=0,
            params=ItemParams(strength=4, epi=0, ep=0, special=0),
            help_text="A bat that is cracked.",
        )
        assert item.help_text == "A bat that is cracked."
        assert item.help_text_pointer is None

    def test_item_both(self):
        """Item with both help_text and help_text_pointer validates (help_text takes priority at pack time)."""
        from ebtools.parsers.item import Item, ItemParams

        item = Item(
            id=1,
            symbol="CRACKED_BAT",
            name="Cracked bat",
            type=0,
            cost=18,
            flags=0,
            effect=0,
            params=ItemParams(strength=4, epi=0, ep=0, special=0),
            help_text="A bat that is cracked.",
            help_text_pointer="0xC539C4",
        )
        assert item.help_text == "A bat that is cracked."
        assert item.help_text_pointer == "0xC539C4"

    def test_enemy_legacy_pointers(self):
        """Enemy with only text pointers (legacy format) validates."""
        from ebtools.parsers.enemy import Enemy, EnemyAction

        enemy = Enemy(
            id=0,
            name="Coil Snake",
            the_flag=0,
            gender="NEUTRAL",
            type="NORMAL",
            battle_sprite=0,
            overworld_sprite=0,
            run_flag=0,
            hp=18,
            pp=0,
            experience=1,
            money=4,
            movement=0,
            text_pointer_1="0xC5F64A",
            text_pointer_2="0xC5F64A",
            palette=0,
            level=1,
            music="BATTLE_AGAINST_A_WEAK_OPPONENT",
            offense=3,
            defense=2,
            speed=1,
            guts=0,
            luck=2,
            weakness_fire=0,
            weakness_ice=0,
            weakness_flash=0,
            weakness_paralysis=0,
            weakness_hypnosis=0,
            miss_rate=0,
            action_order=0,
            actions=[EnemyAction(action=1, argument=0) for _ in range(5)],
            iq=1,
            boss_flag=0,
            item_drop_rate=0,
            item_dropped=0,
            initial_status=0,
            death_style=0,
            row=0,
            max_allies_called=0,
            mirror_success_rate=0,
        )
        assert enemy.text_pointer_1 == "0xC5F64A"
        assert enemy.text_1 is None
        assert enemy.text_1_ref is None

    def test_enemy_inline_text(self):
        """Enemy with inline text fields (no pointers) validates."""
        from ebtools.parsers.enemy import Enemy, EnemyAction

        enemy = Enemy(
            id=0,
            name="Coil Snake",
            the_flag=0,
            gender="NEUTRAL",
            type="NORMAL",
            battle_sprite=0,
            overworld_sprite=0,
            run_flag=0,
            hp=18,
            pp=0,
            experience=1,
            money=4,
            movement=0,
            text_1="Coil Snake attacks!",
            text_2="Coil Snake coiled!",
            palette=0,
            level=1,
            music="BATTLE_AGAINST_A_WEAK_OPPONENT",
            offense=3,
            defense=2,
            speed=1,
            guts=0,
            luck=2,
            weakness_fire=0,
            weakness_ice=0,
            weakness_flash=0,
            weakness_paralysis=0,
            weakness_hypnosis=0,
            miss_rate=0,
            action_order=0,
            actions=[EnemyAction(action=1, argument=0) for _ in range(5)],
            iq=1,
            boss_flag=0,
            item_drop_rate=0,
            item_dropped=0,
            initial_status=0,
            death_style=0,
            row=0,
            max_allies_called=0,
            mirror_success_rate=0,
        )
        assert enemy.text_1 == "Coil Snake attacks!"
        assert enemy.text_pointer_1 is None

    def test_psi_ability_inline(self):
        """PsiAbility with inline description validates."""
        from ebtools.parsers.psi_ability import PsiAbility

        ability = PsiAbility(
            id=0,
            name_index=0,
            level=1,
            category=0,
            usability=0,
            battle_action=0,
            ness_learn_level=0,
            paula_learn_level=0,
            poo_learn_level=0,
            menu_group=0,
            menu_position=0,
            description="Deals fire damage to one enemy.",
        )
        assert ability.description == "Deals fire damage to one enemy."
        assert ability.text_pointer is None

    def test_battle_action_inline(self):
        """BattleAction with inline description validates."""
        from ebtools.parsers.battle_action import BattleAction

        action = BattleAction(
            id=0,
            direction=0,
            target=0,
            type=0,
            pp_cost=0,
            description="Bash attack",
            function_pointer="0xC20000",
        )
        assert action.description == "Bash attack"
        assert action.description_pointer is None

    def test_npc_dialogue_ref(self):
        """NpcEntry with dialogue_ref validates."""
        from ebtools.parsers.npc import NpcEntry

        npc = NpcEntry(
            id=0,
            type="PERSON",
            sprite_id=1,
            direction=0,
            movement_type=0,
            appearance_style=0,
            event_flag=0,
            show_condition=0,
            text_pointer="0xC50000",
            dialogue_ref="npc_greeting",
        )
        assert npc.dialogue_ref == "npc_greeting"
        assert npc.text_pointer == "0xC50000"


class TestDialogueCompilation:
    def test_compile_dialogue_file(self, tmp_path):
        """Create YAML in tmp_path, deserialize, compile each message, decode back, verify identity."""
        from ebtools.text_dsl.decoder import decode_text_block

        text_table = {0x78: "H", 0x99: "i", 0x72: "B", 0xA9: "y", 0x95: "e", 0x50: " "}
        reverse_table = {char: code for code, char in text_table.items()}

        # Build a YAML dialogue file with two labels
        yaml_content = "greeting:\n- text: Hi\n- end_block\nfarewell:\n- text: Bye\n- end_block\n"
        yaml_file = tmp_path / "test_dialogue.yaml"
        yaml_file.write_text(yaml_content)

        # Deserialize
        messages = deserialize_dialogue_file(yaml_file.read_text())
        assert "greeting" in messages
        assert "farewell" in messages

        # Compile each message individually
        for label, ops in messages.items():
            compiled = compile_text_block(ops, reverse_table)
            # Decode back
            decoded = decode_text_block(compiled, text_table)
            assert decoded == ops, f"Round-trip failed for label {label!r}"

    def test_compile_dialogue_with_labels(self, tmp_path):
        """Dialogue with cross-label references compiles with resolved offsets."""
        reverse_table = {"H": 0x78, "i": 0x99}

        yaml_content = "start:\n- text: Hi\n- call_text: target\n- end_block\ntarget:\n- text: Hi\n- end_block\n"
        yaml_file = tmp_path / "label_test.yaml"
        yaml_file.write_text(yaml_content)

        messages = deserialize_dialogue_file(yaml_file.read_text())

        # First pass: calculate offsets (use dummy offsets for size measurement)
        dummy_offsets = dict.fromkeys(messages, 0)
        label_offsets: dict[str, int] = {}
        offset = 0
        for label, ops in messages.items():
            label_offsets[label] = offset
            compiled = compile_text_block(ops, reverse_table, label_offsets=dummy_offsets)
            offset += len(compiled)

        # "start" compiles to: H(1) i(1) call_text_opcode(1) label(4) end_block(1) = 8 bytes
        assert label_offsets["start"] == 0
        assert label_offsets["target"] == 8

        # Second pass: compile with labels
        buf = bytearray()
        for _label, ops in messages.items():
            compiled = compile_text_block(ops, reverse_table, label_offsets=label_offsets)
            buf.extend(compiled)

        # Verify the call_text in "start" references offset 8
        # call_text opcode is 0x08, then 4 bytes LE for the label
        import struct

        # Find call_text byte at position 2 (after 'H' and 'i')
        assert buf[2] == 0x08
        label_val = struct.unpack_from("<I", buf, 3)[0]
        assert label_val == 8


class TestStringTableBuilder:
    def test_add_and_get_offset(self):
        """First string should be at offset 0."""
        from ebtools.text_dsl.string_table import StringTableBuilder

        reverse_table = {"H": 0x78, "i": 0x99}
        builder = StringTableBuilder(reverse_table)
        offset = builder.add("Hi")
        assert offset == 0

    def test_deduplication(self):
        """Same string added twice returns same offset."""
        from ebtools.text_dsl.string_table import StringTableBuilder

        reverse_table = {"H": 0x78, "i": 0x99}
        builder = StringTableBuilder(reverse_table)
        offset1 = builder.add("Hi")
        offset2 = builder.add("Hi")
        assert offset1 == offset2
        # Build should contain only one copy
        data = builder.build()
        # "Hi" encodes to [0x78, 0x99, 0x02] (text + end_block)
        assert data == bytes([0x78, 0x99, 0x02])

    def test_multiple_strings(self):
        """Multiple distinct strings get sequential offsets."""
        from ebtools.text_dsl.string_table import StringTableBuilder

        reverse_table = {"H": 0x78, "i": 0x99, "B": 0x72, "y": 0xA9, "e": 0x95}
        builder = StringTableBuilder(reverse_table)

        offset_hi = builder.add("Hi")
        offset_bye = builder.add("Bye")
        assert offset_hi == 0
        # "Hi" = [0x78, 0x99, 0x02] = 3 bytes
        assert offset_bye == 3

    def test_build_output(self):
        """build() returns concatenated encoded strings."""
        from ebtools.text_dsl.string_table import StringTableBuilder

        reverse_table = {"H": 0x78, "i": 0x99, "B": 0x72, "y": 0xA9, "e": 0x95}
        builder = StringTableBuilder(reverse_table)

        builder.add("Hi")
        builder.add("Bye")
        data = builder.build()

        # "Hi" -> [0x78, 0x99, 0x02], "Bye" -> [0x72, 0xA9, 0x95, 0x02]
        assert data == bytes([0x78, 0x99, 0x02, 0x72, 0xA9, 0x95, 0x02])

    def test_empty_build(self):
        """Empty builder produces empty bytes."""
        from ebtools.text_dsl.string_table import StringTableBuilder

        builder = StringTableBuilder({})
        assert builder.build() == b""
