"""Decoder for EarthBound text bytecode into structured Python dicts.

Reads raw text bytecode and produces a list of dicts, each with an "op" key
(the yaml_name from the opcode table) and argument keys matching the opcode's
ArgSpec names. Literal text bytes produce {"op": "text", "value": "decoded string"}.
"""

from ebtools.byte_reader import ByteReader
from ebtools.text_dsl.opcodes import OPCODE_BY_BYTES, ArgType

# Compressed text prefix bytes.
_COMPRESSED_TEXT_PREFIXES = (0x15, 0x16, 0x17)

# Two-byte opcode prefix range.
_TWO_BYTE_PREFIX_LO = 0x18
_TWO_BYTE_PREFIX_HI = 0x1F


def _read_arg(reader: ByteReader, arg_type: ArgType) -> object:
    """Read a single argument value from the byte stream according to its type."""
    if arg_type in (
        ArgType.U8,
        ArgType.ITEM,
        ArgType.WINDOW,
        ArgType.PARTY,
        ArgType.MUSIC,
        ArgType.SFX,
        ArgType.STATUS_GROUP,
    ):
        return reader.read_byte()
    elif arg_type in (
        ArgType.U16,
        ArgType.FLAG,
        ArgType.SPRITE,
        ArgType.MOVEMENT,
        ArgType.ENEMY_GROUP,
    ):
        return reader.read_le16()
    elif arg_type == ArgType.U24:
        return reader.read_le24()
    elif arg_type in (ArgType.U32, ArgType.LABEL):
        return reader.read_le32()
    elif arg_type == ArgType.JUMP_TABLE:
        count = reader.read_byte()
        return [reader.read_le32() for _ in range(count)]
    elif arg_type == ArgType.STRING:
        return _read_string_arg(reader)
    else:
        raise ValueError(f"Unknown ArgType: {arg_type}")


def _read_string_arg(reader: ByteReader) -> dict:
    """Read a STRING argument: bytes until terminator 0x00, 0x01, or 0x02."""
    text_bytes: list[int] = []
    while reader:
        b = reader.read_byte()
        if b == 0x00:
            return {"text": text_bytes, "terminator": "end"}
        elif b == 0x01:
            label = reader.read_le32()
            return {"text": text_bytes, "terminator": "select_script", "label": label}
        elif b == 0x02:
            return {"text": text_bytes, "terminator": "store"}
        else:
            text_bytes.append(b)
    # Ran out of data without a terminator.
    return {"text": text_bytes, "terminator": "end"}


def decode_text_block(
    data: bytes,
    text_table: dict[int, str],
    *,
    compressed_text: dict[int, str] | None = None,
    stop_at_end_block: bool = False,
) -> list[dict]:
    """Decode raw text bytecode into a list of structured dicts.

    Args:
        data: Raw bytecode bytes.
        text_table: Mapping of byte value -> character string for literal text.
        compressed_text: Optional mapping of index -> expanded string for
            compressed text opcodes (0x15-0x17).
        stop_at_end_block: If True, stop decoding after the first end_block
            opcode (0x02). Useful for decoding a single message from a block
            that contains many.

    Returns
    -------
        List of dicts, each with an "op" key and argument keys.
    """
    reader = ByteReader(data)
    result: list[dict] = []
    text_buf: list[str] = []

    def flush_text():
        if text_buf:
            result.append({"op": "text", "value": "".join(text_buf)})
            text_buf.clear()

    while reader:
        b = reader.peek_byte()

        # Literal text character.
        if b in text_table:
            reader.read_byte()
            text_buf.append(text_table[b])
            continue

        # Compressed text expansion.
        if b in _COMPRESSED_TEXT_PREFIXES:
            reader.read_byte()
            next_byte = reader.read_byte()
            index = (b - 0x15) * 256 + next_byte
            if compressed_text and index in compressed_text:
                text_buf.append(compressed_text[index])
            # If not in compressed_text dict, silently skip (no output).
            continue

        # Control code — flush any accumulated text first.
        flush_text()

        # Two-byte opcode prefix.
        if _TWO_BYTE_PREFIX_LO <= b <= _TWO_BYTE_PREFIX_HI:
            reader.read_byte()
            if not reader:
                result.append({"op": "unknown", "bytes": [b]})
                continue
            sub = reader.read_byte()
            key = (b, sub)
        else:
            reader.read_byte()
            key = (b,)

        opcode = OPCODE_BY_BYTES.get(key)
        if opcode is None:
            result.append({"op": "unknown", "bytes": list(key)})
            continue

        entry: dict = {"op": opcode.yaml_name}
        for arg_spec in opcode.args:
            entry[arg_spec.name] = _read_arg(reader, arg_spec.type)
        result.append(entry)

        if stop_at_end_block and opcode.yaml_name == "end_block":
            break

    # Flush any trailing text.
    flush_text()

    return result
