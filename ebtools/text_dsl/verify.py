"""Round-trip verification for dialogue YAML ↔ bytecode.

Compiles each dialogue YAML file to bytecode, decodes the bytecode back to
opcode dicts, and structurally compares the result with the original.  Any
difference indicates a compiler or decoder bug.

The core challenge is that YAML uses symbolic label names (strings) but
compiled bytecode uses integer addresses.  After decoding, integer addresses
are mapped back to label names using the inverse of the label offset table.
"""

from pathlib import Path

from ebtools.text_dsl.compiler import compile_text_block
from ebtools.text_dsl.decoder import decode_text_block
from ebtools.text_dsl.opcodes import OPCODE_BY_NAME, ArgType
from ebtools.text_dsl.yaml_io import deserialize_dialogue_file


def _build_offset_to_label(label_offsets: dict[str, int]) -> dict[int, str]:
    """Invert label_offsets: {name: offset} → {offset: name}."""
    result: dict[int, str] = {}
    for name, offset in label_offsets.items():
        result[offset] = name
    return result


def _resolve_addresses_to_labels(
    ops: list[dict],
    offset_to_label: dict[int, str],
) -> list[dict]:
    """Replace integer addresses in decoded ops with label names where possible."""
    resolved: list[dict] = []
    for entry in ops:
        op_name = entry.get("op", "")
        if op_name in ("text", "unknown"):
            resolved.append(dict(entry))
            continue

        spec = OPCODE_BY_NAME.get(op_name)
        if spec is None:
            resolved.append(dict(entry))
            continue

        new_entry = dict(entry)
        for arg_spec in spec.args:
            value = new_entry.get(arg_spec.name)
            if value is None:
                continue

            if arg_spec.type == ArgType.LABEL:
                if isinstance(value, int) and value in offset_to_label:
                    new_entry[arg_spec.name] = offset_to_label[value]

            elif arg_spec.type == ArgType.JUMP_TABLE:
                if isinstance(value, list):
                    new_entry[arg_spec.name] = [
                        offset_to_label[v] if isinstance(v, int) and v in offset_to_label else v for v in value
                    ]

            elif (
                arg_spec.type == ArgType.STRING
                and isinstance(value, dict)
                and value.get("terminator") == "select_script"
            ):
                label_val = value.get("label")
                if isinstance(label_val, int) and label_val in offset_to_label:
                    new_entry[arg_spec.name] = {
                        **value,
                        "label": offset_to_label[label_val],
                    }

        resolved.append(new_entry)
    return resolved


def _normalize_symbolic_to_int(
    ops: list[dict],
    reverse_names: dict[ArgType, dict[str, int]] | None,
) -> list[dict]:
    """Normalize symbolic names in original YAML ops to integers for comparison.

    After decode, the decoder produces integer values for FLAG, ITEM, etc.
    The original YAML has symbolic names.  Normalize YAML to integers so
    comparison is apples-to-apples.
    """
    if reverse_names is None:
        return ops

    _named_types = {
        ArgType.FLAG,
        ArgType.ITEM,
        ArgType.WINDOW,
        ArgType.PARTY,
        ArgType.MUSIC,
        ArgType.SFX,
        ArgType.SPRITE,
        ArgType.MOVEMENT,
        ArgType.STATUS_GROUP,
        ArgType.ENEMY_GROUP,
    }

    result: list[dict] = []
    for entry in ops:
        op_name = entry.get("op", "")
        if op_name in ("text", "unknown"):
            result.append(dict(entry))
            continue

        spec = OPCODE_BY_NAME.get(op_name)
        if spec is None:
            result.append(dict(entry))
            continue

        new_entry = dict(entry)
        for arg_spec in spec.args:
            if arg_spec.type not in _named_types:
                continue
            value = new_entry.get(arg_spec.name)
            if isinstance(value, str):
                lookup = reverse_names.get(arg_spec.type)
                if lookup and value in lookup:
                    new_entry[arg_spec.name] = lookup[value]
        result.append(new_entry)
    return result


def verify_dialogue_round_trip(
    dialogue_dir: Path,
    text_table: dict[int, str],
    reverse_text_table: dict[str, int],
    *,
    label_offsets: dict[str, int] | None = None,
    reverse_names: dict[ArgType, dict[str, int]] | None = None,
    compressed_text: dict[int, str] | None = None,
) -> list[str]:
    """Verify round-trip integrity of all dialogue YAML files.

    For each file: deserialize → compile → decode → compare.

    Parameters
    ----------
    dialogue_dir
        Directory containing ``*.yml`` / ``*.yaml`` files.
    text_table
        Byte value → character mapping (for decoder).
    reverse_text_table
        Character → byte value mapping (for compiler).
    label_offsets
        Label name → address mapping.  If None, synthetic offsets are generated.
    reverse_names
        ArgType → {symbolic_name: int} mapping for resolving named args.
    compressed_text
        Compressed text index → expanded string mapping (for decoder).

    Returns
    -------
    list[str]
        List of error descriptions.  Empty means all files verified.
    """
    yaml_files = sorted(dialogue_dir.glob("*.yaml")) + sorted(dialogue_dir.glob("*.yml"))
    if not yaml_files:
        return [f"No dialogue YAML files found in {dialogue_dir}"]

    all_messages: list[tuple[Path, dict[str, list[dict]]]] = []
    for yaml_file in yaml_files:
        messages = deserialize_dialogue_file(yaml_file.read_text())
        all_messages.append((yaml_file, messages))

    # Build flat label offsets for dialogue labels, seeded with any
    # externally-provided offsets (e.g., original ROM label addresses
    # for cross-block references).
    seed_offsets = dict(label_offsets) if label_offsets else {}
    flat_label_offsets: dict[str, int] = {}
    pos = 0x100000
    for _, messages in all_messages:
        dummy_offsets: dict[str, int] = {**seed_offsets, **flat_label_offsets, **dict.fromkeys(messages, 0)}
        for label, ops in messages.items():
            flat_label_offsets[label] = pos
            compiled = compile_text_block(
                ops,
                reverse_text_table,
                label_offsets=dummy_offsets,
                reverse_names=reverse_names,
            )
            pos += len(compiled)
            dummy_offsets[label] = flat_label_offsets[label]

    # Merge: dialogue labels + seed labels (for cross-block refs).
    label_offsets = {**seed_offsets, **flat_label_offsets}

    offset_to_label = _build_offset_to_label(label_offsets)
    errors: list[str] = []

    for yaml_file, messages in all_messages:
        fname = yaml_file.name
        for label, original_ops in messages.items():
            # Compile to bytecode.
            try:
                compiled = compile_text_block(
                    original_ops,
                    reverse_text_table,
                    label_offsets=label_offsets,
                    reverse_names=reverse_names,
                )
            except Exception as exc:
                errors.append(f"{fname}: {label}: compile error: {exc}")
                continue

            # Decode back to ops.
            try:
                decoded_ops = decode_text_block(
                    compiled,
                    text_table,
                    compressed_text=compressed_text,
                )
            except Exception as exc:
                errors.append(f"{fname}: {label}: decode error: {exc}")
                continue

            # Resolve integer addresses back to label names.
            decoded_ops = _resolve_addresses_to_labels(decoded_ops, offset_to_label)

            # Normalize original ops: symbolic names → integers (decoder returns ints).
            normalized_original = _normalize_symbolic_to_int(original_ops, reverse_names)

            # Compare.
            if normalized_original != decoded_ops:
                # Find first mismatch for a useful error message.
                for i, (orig, dec) in enumerate(zip(normalized_original, decoded_ops)):
                    if orig != dec:
                        errors.append(f"{fname}: {label}: op {i} mismatch:\n  original:  {orig}\n  roundtrip: {dec}")
                        break
                else:
                    # Length mismatch.
                    errors.append(
                        f"{fname}: {label}: length mismatch: "
                        f"original={len(normalized_original)}, roundtrip={len(decoded_ops)}"
                    )

    return errors
