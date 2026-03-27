"""Tests for the migrate-text command."""

from ebtools.cli.migrate_text import migrate_json_text_field

# Minimal text table: map EB character codes to ASCII-ish characters.
# 0x50=' ', 0x60='0', 0x71='A', 0x91='a', etc.
_TEXT_TABLE = {
    0x50: " ",
    0x71: "A",
    0x72: "B",
    0x73: "C",
    0x74: "D",
    0x75: "E",
    0x76: "F",
    0x91: "a",
    0x92: "b",
    0x93: "c",
    0x94: "d",
    0x95: "e",
    0x96: "f",
}


def test_simple_help_text_migration():
    """Simple text (only text + end_block) should return inline type."""
    # Build bytecode: "ABCdef" followed by end_block (0x02).
    # A=0x71, B=0x72, C=0x73, d=0x94, e=0x95, f=0x96, end_block=0x02
    bytecode = bytes([0x71, 0x72, 0x73, 0x94, 0x95, 0x96, 0x02])

    # Place the block at SNES address 0xC10000, and point to start of block.
    text_blocks = {0xC10000: bytecode}

    result = migrate_json_text_field(0xC10000, text_blocks, _TEXT_TABLE)

    assert result["type"] == "inline"
    assert result["text"] == "ABCdef"


def test_simple_text_with_line_break():
    """Text with line_break (0x00) is still simple and should be inlined."""
    # "AB" + line_break + "cd" + end_block
    bytecode = bytes([0x71, 0x72, 0x00, 0x93, 0x94, 0x02])
    text_blocks = {0xC10000: bytecode}

    result = migrate_json_text_field(0xC10000, text_blocks, _TEXT_TABLE)

    assert result["type"] == "inline"
    assert "{line_break}" in result["text"]
    assert result["text"] == "AB{line_break}cd"


def test_simple_text_with_halt_with_prompt():
    """Text with halt_with_prompt (0x03) is still simple."""
    # "AB" + halt_with_prompt + "cd" + end_block
    bytecode = bytes([0x71, 0x72, 0x03, 0x93, 0x94, 0x02])
    text_blocks = {0xC10000: bytecode}

    result = migrate_json_text_field(0xC10000, text_blocks, _TEXT_TABLE)

    assert result["type"] == "inline"
    assert result["text"] == "AB{halt_with_prompt}cd"


def test_complex_text_returns_ref():
    """Text with branching opcodes (e.g. set_event_flag) should return ref type."""
    # "AB" + set_event_flag(flag=0x0042) + "cd" + end_block
    # set_event_flag = 0x04, flag is U16 LE = 0x42, 0x00
    bytecode = bytes([0x71, 0x72, 0x04, 0x42, 0x00, 0x93, 0x94, 0x02])
    text_blocks = {0xC10000: bytecode}

    result = migrate_json_text_field(0xC10000, text_blocks, _TEXT_TABLE)

    assert result["type"] == "ref"
    assert isinstance(result["decoded"], list)
    # Should contain text op, set_event_flag op, text op, end_block op
    ops = result["decoded"]
    op_names = [op["op"] for op in ops]
    assert "text" in op_names
    assert "set_event_flag" in op_names


def test_address_not_found_returns_unknown():
    """Address not in any text block should return unknown type."""
    text_blocks = {0xC10000: b"\x02"}

    result = migrate_json_text_field(0xC20000, text_blocks, _TEXT_TABLE)

    assert result["type"] == "unknown"
    assert "0x" in result["address"]


def test_address_offset_within_block():
    """Address in the middle of a text block should decode from that offset."""
    # Block starts at 0xC10000, text at offset 4 is "AB" + end_block
    # First 4 bytes are other data (0xFF padding).
    bytecode = bytes([0xFF, 0xFF, 0xFF, 0xFF, 0x71, 0x72, 0x02])
    text_blocks = {0xC10000: bytecode}

    result = migrate_json_text_field(0xC10004, text_blocks, _TEXT_TABLE)

    assert result["type"] == "inline"
    assert result["text"] == "AB"
