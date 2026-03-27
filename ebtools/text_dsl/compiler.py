"""Compiler for EarthBound text bytecode from structured Python dicts.

The inverse of decoder.py: takes a list of opcode dicts and produces raw bytecode.
"""

import struct
from typing import TYPE_CHECKING

from ebtools.text_dsl.opcodes import OPCODE_BY_NAME, ArgType

if TYPE_CHECKING:
    from ebtools.config import CommonData

# ArgTypes that encode as U8 and support symbolic name resolution.
_NAMED_U8_TYPES = frozenset(
    {
        ArgType.ITEM,
        ArgType.WINDOW,
        ArgType.PARTY,
        ArgType.MUSIC,
        ArgType.SFX,
        ArgType.STATUS_GROUP,
    }
)

# ArgTypes that encode as U16 and support symbolic name resolution.
_NAMED_U16_TYPES = frozenset(
    {
        ArgType.FLAG,
        ArgType.SPRITE,
        ArgType.MOVEMENT,
        ArgType.ENEMY_GROUP,
    }
)


def _resolve_named_value(
    arg_type: ArgType,
    value: object,
    reverse_names: dict[ArgType, dict[str, int]] | None,
    *,
    opcode_name: str = "",
    arg_name: str = "",
) -> int:
    """Resolve a possibly-symbolic value to an integer.

    If *value* is already an int, return it unchanged.  If it's a string,
    look it up in the reverse_names table for the given ArgType.
    """
    context = ""
    if opcode_name:
        context = f" (opcode {opcode_name!r}, arg {arg_name!r})"

    if isinstance(value, int):
        return value
    if isinstance(value, str):
        if reverse_names is None:
            raise ValueError(f"String name {value!r} for {arg_type.name} but no reverse_names provided{context}")
        lookup = reverse_names.get(arg_type)
        if lookup is None:
            raise ValueError(f"No reverse name table for {arg_type.name} (trying to resolve {value!r}){context}")
        if value not in lookup:
            raise KeyError(f"Name {value!r} not found in reverse name table for {arg_type.name}{context}")
        return lookup[value]
    raise TypeError(f"Expected int or str for {arg_type.name}, got {type(value).__name__}{context}")


def _write_arg(
    buf: bytearray,
    arg_type: ArgType,
    value: object,
    label_offsets: dict[str, int] | None,
    reverse_names: dict[ArgType, dict[str, int]] | None = None,
    *,
    opcode_name: str = "",
    arg_name: str = "",
) -> None:
    """Write a single argument value to the byte buffer according to its type."""
    if arg_type in (ArgType.U8,) or arg_type in _NAMED_U8_TYPES:
        if arg_type in _NAMED_U8_TYPES:
            value = _resolve_named_value(arg_type, value, reverse_names, opcode_name=opcode_name, arg_name=arg_name)
        buf.append(value & 0xFF)
    elif arg_type in (ArgType.U16,) or arg_type in _NAMED_U16_TYPES:
        if arg_type in _NAMED_U16_TYPES:
            value = _resolve_named_value(arg_type, value, reverse_names, opcode_name=opcode_name, arg_name=arg_name)
        buf.extend(struct.pack("<H", value))
    elif arg_type == ArgType.U24:
        buf.append(value & 0xFF)
        buf.append((value >> 8) & 0xFF)
        buf.append((value >> 16) & 0xFF)
    elif arg_type == ArgType.U32:
        buf.extend(struct.pack("<I", value))
    elif arg_type == ArgType.LABEL:
        resolved = _resolve_label(value, label_offsets)
        buf.extend(struct.pack("<I", resolved))
    elif arg_type == ArgType.JUMP_TABLE:
        targets = value
        buf.append(len(targets) & 0xFF)
        for target in targets:
            resolved = _resolve_label(target, label_offsets)
            buf.extend(struct.pack("<I", resolved))
    elif arg_type == ArgType.STRING:
        _write_string_arg(buf, value, label_offsets)
    else:
        raise ValueError(f"Unknown ArgType: {arg_type}")


def _resolve_label(value: object, label_offsets: dict[str, int] | None) -> int:
    """Resolve a label value: if it's a string, look it up; otherwise treat as int."""
    if isinstance(value, str):
        if label_offsets is None:
            raise ValueError(f"String label {value!r} but no label_offsets provided")
        if value not in label_offsets:
            raise KeyError(f"Label {value!r} not found in label_offsets")
        return label_offsets[value]
    return value


def _write_string_arg(buf: bytearray, value: dict, label_offsets: dict[str, int] | None) -> None:
    """Write a STRING argument: text bytes + terminator."""
    for b in value["text"]:
        buf.append(b & 0xFF)
    terminator = value["terminator"]
    if terminator == "end":
        buf.append(0x00)
    elif terminator == "select_script":
        buf.append(0x01)
        resolved = _resolve_label(value["label"], label_offsets)
        buf.extend(struct.pack("<I", resolved))
    elif terminator == "store":
        buf.append(0x02)
    else:
        raise ValueError(f"Unknown STRING terminator: {terminator!r}")


def build_reverse_names(common_data: CommonData) -> dict[ArgType, dict[str, int]]:
    """Build a reverse lookup table from CommonData for compiling symbolic names.

    Maps each ArgType to a dict of {name: numeric_value}.  STATUS_GROUP is
    1-indexed in the bytecode, so its values are offset by +1.

    Args:
        common_data: The loaded CommonData instance.

    Returns
    -------
        Mapping of ArgType -> {symbolic_name: integer_value}.
    """
    result: dict[ArgType, dict[str, int]] = {}

    # (ArgType, list of names, is_1_indexed)
    mappings: list[tuple[ArgType, list[str], bool]] = [
        (ArgType.FLAG, common_data.eventFlags, False),
        (ArgType.ITEM, common_data.items, False),
        (ArgType.WINDOW, common_data.windows, False),
        (ArgType.PARTY, common_data.partyMembers, False),
        (ArgType.MUSIC, common_data.musicTracks, False),
        (ArgType.SFX, common_data.sfx, False),
        (ArgType.SPRITE, common_data.sprites, False),
        (ArgType.MOVEMENT, common_data.movements, False),
        (ArgType.STATUS_GROUP, common_data.statusGroups, True),
        (ArgType.ENEMY_GROUP, common_data.enemyGroups, False),
    ]

    for arg_type, names, is_1_indexed in mappings:
        lookup: dict[str, int] = {}
        for i, name in enumerate(names):
            if name:
                value = (i + 1) if is_1_indexed else i
                lookup[name] = value
        result[arg_type] = lookup

    return result


def compile_text_block(
    ops: list[dict],
    reverse_text_table: dict[str, int],
    *,
    label_offsets: dict[str, int] | None = None,
    reverse_names: dict[ArgType, dict[str, int]] | None = None,
) -> bytes:
    """Compile a list of structured opcode dicts into raw text bytecode.

    Args:
        ops: List of dicts, each with an "op" key and argument keys.
        reverse_text_table: Mapping of character string -> byte value for literal text.
        label_offsets: Optional mapping of label name -> address for resolving
            string labels in LABEL and JUMP_TABLE args.
        reverse_names: Optional mapping of ArgType -> {name: value} for resolving
            symbolic names (e.g. "COOKIE" -> 5 for ITEM) back to integers.

    Returns
    -------
        Raw bytecode bytes.
    """
    buf = bytearray()

    for entry in ops:
        op_name = entry["op"]

        if op_name == "text":
            # Encode each character via reverse_text_table.
            for ch in entry["value"]:
                if ch not in reverse_text_table:
                    raise ValueError(f"Character {ch!r} not in reverse_text_table")
                buf.append(reverse_text_table[ch])
            continue

        if op_name == "unknown":
            # Pass through raw bytes.
            buf.extend(entry["bytes"])
            continue

        # Look up the opcode spec.
        spec = OPCODE_BY_NAME.get(op_name)
        if spec is None:
            raise ValueError(f"Unknown opcode name: {op_name!r}")

        # Write opcode bytes.
        buf.extend(spec.bytes)

        # Write arguments.
        for arg_spec in spec.args:
            _write_arg(
                buf,
                arg_spec.type,
                entry[arg_spec.name],
                label_offsets,
                reverse_names,
                opcode_name=op_name,
                arg_name=arg_spec.name,
            )

    return bytes(buf)
