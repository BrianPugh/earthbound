"""YAML serialization/deserialization for EarthBound dialogue files.

Converts between opcode dicts (as produced by the decoder) and human-readable
YAML format.

Text convention: an ``@`` character (EB character code 0x70) at the start of a
text string means "clear the text window before printing."  It acts as a
paragraph separator -- without it, new text appends to whatever is already
displayed in the window.
"""

import yaml

from ebtools.text_dsl.opcodes import OPCODE_BY_NAME


def _op_to_yaml(op_dict: dict) -> str | dict:
    """Convert a single opcode dict to its YAML-friendly representation.

    Returns either a bare string (no-arg opcode) or a single-key dict.
    """
    op_name = op_dict["op"]

    if op_name == "text":
        return {"text": op_dict["value"]}

    # Collect all keys that aren't "op"
    arg_keys = [k for k in op_dict if k != "op"]

    if not arg_keys:
        # No-arg opcode -> bare string
        return op_name

    if len(arg_keys) == 1:
        # Single-arg opcode -> {op_name: value}
        return {op_name: op_dict[arg_keys[0]]}

    # Multi-arg opcode -> {op_name: {arg1: val1, arg2: val2, ...}}
    args = {k: op_dict[k] for k in arg_keys}
    return {op_name: args}


def _yaml_to_op(item: str | dict) -> dict:
    """Convert a YAML item back to an opcode dict.

    Accepts bare strings (no-arg opcodes) or single-key dicts.
    """
    if isinstance(item, str):
        # Bare string -> no-arg opcode
        return {"op": item}

    if not isinstance(item, dict) or len(item) != 1:
        raise ValueError(f"Expected a string or single-key dict, got: {item!r}")

    key = next(iter(item))
    value = item[key]

    # Text entry
    if key == "text":
        return {"op": "text", "value": value}

    # Pass through unknown opcodes
    if key == "unknown":
        return {"op": "unknown", "bytes": value}

    # Look up opcode spec
    if key not in OPCODE_BY_NAME:
        raise ValueError(f"Unknown opcode: {key!r}")

    spec = OPCODE_BY_NAME[key]
    args = spec.args

    if len(args) == 0:
        # No-arg opcode that was serialized as a dict (shouldn't happen, but handle it)
        return {"op": key}

    if len(args) == 1:
        # Single-arg: value is the arg directly
        return {"op": key, args[0].name: value}

    # Multi-arg: value is a dict of arg names
    if not isinstance(value, dict):
        raise TypeError(f"Opcode {key!r} expects {len(args)} args as a dict, got: {value!r}")
    result = {"op": key}
    for arg_spec in args:
        if arg_spec.name in value:
            result[arg_spec.name] = value[arg_spec.name]
    return result


def serialize_dialogue_file(messages: dict[str, list[dict]]) -> str:
    """Serialize a dialogue file (label -> list of opcode dicts) to YAML.

    Args:
        messages: Mapping of label names to lists of opcode dicts.

    Returns
    -------
        YAML string representation.
    """
    yaml_data: dict[str, list] = {}
    for label, ops in messages.items():
        yaml_data[label] = [_op_to_yaml(op) for op in ops]

    class _IndentedDumper(yaml.Dumper):
        def increase_indent(self, flow=False, indentless=False):
            return super().increase_indent(flow, False)

    return yaml.dump(
        yaml_data,
        Dumper=_IndentedDumper,
        default_flow_style=False,
        sort_keys=False,
        allow_unicode=True,
    )


def deserialize_dialogue_file(yaml_str: str) -> dict[str, list[dict]]:
    """Deserialize a YAML string into a dialogue file structure.

    Args:
        yaml_str: YAML string (as produced by serialize_dialogue_file).

    Returns
    -------
        Mapping of label names to lists of opcode dicts.
    """
    yaml_data = yaml.safe_load(yaml_str)
    if yaml_data is None:
        return {}

    result: dict[str, list[dict]] = {}
    for label, items in yaml_data.items():
        result[label] = [_yaml_to_op(item) for item in items]

    return result
