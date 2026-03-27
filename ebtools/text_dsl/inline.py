"""Inline text encoder for EarthBound text bytecode.

Encodes inline text strings with {control_code} interpolation into bytecode.
Used for item help text, enemy text, etc.
"""

import re

from ebtools.text_dsl.compiler import compile_text_block
from ebtools.text_dsl.opcodes import OPCODE_BY_NAME

# Opcodes considered "simple" — safe for inline text (no branching, menus, flags, etc.)
_SIMPLE_OPCODES = frozenset(
    {
        "text",
        "end_block",
        "line_break",
        "start_new_line",
        "halt_with_prompt",
        "halt_without_prompt",
        "pause",
        "print_char_name",
        "print_char",
        "print_stat",
        "print_item_name",
        "print_number",
        "print_money_amount",
        "print_action_user_name",
        "print_action_target_name",
        "print_action_amount",
        "print_psi_name",
        "print_teleport_destination_name",
        "text_colour_effects",
        "use_normal_font",
        "use_mr_saturn_font",
    }
)

_INTERPOLATION_RE = re.compile(r"\{([^}]+)\}")


def is_simple_text(ops: list[dict]) -> bool:
    """Check if decoded opcode list uses only simple (inlineable) opcodes."""
    return all(op["op"] in _SIMPLE_OPCODES for op in ops)


def parse_inline_to_ops(text: str) -> list[dict]:
    """Parse inline text to list of opcode dicts (without trailing end_block).

    Text between {control_code} interpolations becomes {"op": "text", "value": "..."}.
    Each {opcode arg1 arg2} interpolation is parsed: first part is opcode name,
    remaining parts are integer arguments (supports 0x hex).
    """
    ops: list[dict] = []
    last_end = 0

    for match in _INTERPOLATION_RE.finditer(text):
        # Add any literal text before this interpolation
        if match.start() > last_end:
            ops.append({"op": "text", "value": text[last_end : match.start()]})

        # Parse the interpolation
        parts = match.group(1).split()
        op_name = parts[0]
        spec = OPCODE_BY_NAME.get(op_name)
        if spec is None:
            raise ValueError(f"Unknown opcode in interpolation: {op_name!r}")

        entry: dict = {"op": op_name}
        arg_values = [int(p, 0) for p in parts[1:]]

        if len(arg_values) != len(spec.args):
            raise ValueError(f"Opcode {op_name!r} expects {len(spec.args)} args, got {len(arg_values)}")

        for arg_spec, value in zip(spec.args, arg_values):
            entry[arg_spec.name] = value

        ops.append(entry)
        last_end = match.end()

    # Add any trailing literal text
    if last_end < len(text):
        ops.append({"op": "text", "value": text[last_end:]})

    return ops


def encode_inline_text(text: str, reverse_text_table: dict[str, int]) -> bytes:
    """Encode inline text with {control_code} interpolation to bytecode (with trailing end_block)."""
    ops = parse_inline_to_ops(text)
    ops.append({"op": "end_block"})
    return compile_text_block(ops, reverse_text_table)
