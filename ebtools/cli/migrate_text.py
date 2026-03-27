"""Export ROM text to editable dialogue YAML and migrate JSON text pointers."""

import json
import sys
from pathlib import Path
from typing import TYPE_CHECKING, Annotated

from cyclopts import Parameter

from ebtools.text_dsl.decoder import decode_text_block

if TYPE_CHECKING:
    from ebtools.config import CommonData, DumpDoc
from ebtools.text_dsl.inline import is_simple_text
from ebtools.text_dsl.opcodes import OPCODE_BY_NAME, ArgType
from ebtools.text_dsl.yaml_io import serialize_dialogue_file


def _resolve_label_addresses(ops: list[dict], addr_to_label: dict[int, str]) -> None:
    """Replace raw SNES address integers with symbolic label names in-place.

    Walks all opcode dicts and replaces integer values in LABEL-type args
    with their symbolic names from addr_to_label.  Also handles JUMP_TABLE
    args (lists of addresses).
    """
    for op in ops:
        op_name = op.get("op")
        if op_name in ("text", "unknown") or op_name is None:
            continue

        spec = OPCODE_BY_NAME.get(op_name)
        if spec is None:
            continue

        for arg_spec in spec.args:
            if arg_spec.type == ArgType.LABEL:
                val = op.get(arg_spec.name)
                if isinstance(val, int) and val in addr_to_label:
                    op[arg_spec.name] = addr_to_label[val]
            elif arg_spec.type == ArgType.JUMP_TABLE:
                targets = op.get(arg_spec.name)
                if isinstance(targets, list):
                    op[arg_spec.name] = [addr_to_label.get(t, t) if isinstance(t, int) else t for t in targets]


def _collect_unresolved_label_addresses(all_messages: dict[str, dict[str, list[dict]]]) -> set[int]:
    """Collect all LABEL-type integer values that haven't been resolved to names yet.

    Args:
        all_messages: Mapping of block_name -> {label_name: [ops...]}.

    Returns
    -------
        Set of SNES addresses still stored as raw integers.
    """
    unresolved: set[int] = set()
    for _block_name, messages in all_messages.items():
        for _label, ops in messages.items():
            for op in ops:
                op_name = op.get("op")
                if op_name in ("text", "unknown") or op_name is None:
                    continue
                spec = OPCODE_BY_NAME.get(op_name)
                if spec is None:
                    continue
                for arg_spec in spec.args:
                    if arg_spec.type == ArgType.LABEL:
                        val = op.get(arg_spec.name)
                        if isinstance(val, int):
                            unresolved.add(val)
                    elif arg_spec.type == ArgType.JUMP_TABLE:
                        targets = op.get(arg_spec.name)
                        if isinstance(targets, list):
                            for t in targets:
                                if isinstance(t, int):
                                    unresolved.add(t)
    return unresolved


def _find_block_for_address(
    snes_addr: int,
    text_blocks: dict[int, bytes],
    block_base_to_name: dict[int, str],
) -> tuple[str, int, bytes] | None:
    """Find the text block containing a SNES address.

    Returns (block_name, block_base, block_data) or None.
    """
    for block_base, block_data in text_blocks.items():
        if block_base <= snes_addr < block_base + len(block_data):
            name = block_base_to_name.get(block_base)
            if name is not None:
                return name, block_base, block_data
    return None


def _resolve_symbolic_names(ops: list[dict], common_data: CommonData) -> None:
    """Replace numeric IDs with symbolic names from CommonData in-place.

    Walks all opcode dicts and replaces integer values in typed args
    (FLAG, ITEM, WINDOW, etc.) with their human-readable names from
    the common data definitions.
    """
    # Map ArgType -> (list of names, is_1_indexed)
    argtype_to_names: dict[ArgType, tuple[list[str], bool]] = {
        ArgType.FLAG: (common_data.eventFlags, False),
        ArgType.ITEM: (common_data.items, False),
        ArgType.WINDOW: (common_data.windows, False),
        ArgType.PARTY: (common_data.partyMembers, False),
        ArgType.MUSIC: (common_data.musicTracks, False),
        ArgType.SFX: (common_data.sfx, False),
        ArgType.SPRITE: (common_data.sprites, False),
        ArgType.MOVEMENT: (common_data.movements, False),
        ArgType.STATUS_GROUP: (common_data.statusGroups, True),  # 1-indexed in assembly
        ArgType.ENEMY_GROUP: (common_data.enemyGroups, False),
    }

    for op in ops:
        op_name = op.get("op")
        if op_name in ("text", "unknown") or op_name is None:
            continue

        spec = OPCODE_BY_NAME.get(op_name)
        if spec is None:
            continue

        for arg_spec in spec.args:
            names_info = argtype_to_names.get(arg_spec.type)
            if names_info is None:
                continue

            names_list, is_1_indexed = names_info
            val = op.get(arg_spec.name)
            if not isinstance(val, int):
                continue

            index = (val - 1) if is_1_indexed else val
            if index < 0 or index >= len(names_list):
                continue

            name = names_list[index]
            if name:  # Only replace if name is non-empty
                op[arg_spec.name] = name


def _ops_to_inline(ops: list[dict]) -> str:
    """Concatenate simple opcode dicts into an inline text string.

    Text ops become literal text; other simple ops become {opcode ...} interpolations.
    """
    parts: list[str] = []
    for op in ops:
        name = op["op"]
        if name == "text":
            parts.append(op["value"])
        elif name == "end_block":
            continue
        else:
            # Build interpolation: {opcode arg1 arg2 ...}
            args = [str(v) for k, v in op.items() if k != "op"]
            if args:
                parts.append("{" + name + " " + " ".join(args) + "}")
            else:
                parts.append("{" + name + "}")
    return "".join(parts)


def migrate_json_text_field(
    snes_address: int,
    text_blocks: dict[int, bytes],
    text_table: dict[int, str],
    *,
    compressed_text: dict[int, str] | None = None,
) -> dict:
    """Migrate a single text pointer to inline text or a dialogue ref.

    Args:
        snes_address: SNES address (e.g. 0xC539C4) of the text entry.
        text_blocks: Mapping of SNES start address -> raw bytecode bytes
            for each text block file.
        text_table: Byte-to-character mapping for literal text decoding.
        compressed_text: Optional mapping of index -> expanded string for
            compressed text opcodes (0x15-0x17).

    Returns
    -------
        One of:
        - {"type": "inline", "text": "concatenated text"}
        - {"type": "ref", "decoded": [...list of opcode dicts...]}
        - {"type": "unknown", "address": "0x..."}
    """
    # Find the text block containing this address.
    containing_block_start = None
    containing_block_data = None

    for block_start, block_data in text_blocks.items():
        block_end = block_start + len(block_data)
        if block_start <= snes_address < block_end:
            containing_block_start = block_start
            containing_block_data = block_data
            break

    if containing_block_data is None or containing_block_start is None:
        return {"type": "unknown", "address": f"0x{snes_address:06X}"}

    # Extract the slice starting at the target address.
    offset_in_block = snes_address - containing_block_start
    data_slice = containing_block_data[offset_in_block:]

    decoded = decode_text_block(data_slice, text_table, compressed_text=compressed_text, stop_at_end_block=True)

    if is_simple_text(decoded):
        return {"type": "inline", "text": _ops_to_inline(decoded)}
    else:
        return {"type": "ref", "decoded": decoded}


def _load_text_blocks(bin_dir: Path, dump_entries: list) -> dict[int, bytes]:
    """Load raw binary text block files into a {snes_address: bytes} mapping.

    Reads bin files whose dump entries have extension 'bin' and share a name
    with an ebtxt entry.
    """
    text_blocks: dict[int, bytes] = {}

    # Collect the names of ebtxt entries to know which bin files are text blocks.
    ebtxt_names: set[str] = set()
    for entry in dump_entries:
        if entry.extension == "ebtxt":
            ebtxt_names.add(entry.name)

    # Load the corresponding bin files.
    for entry in dump_entries:
        if entry.extension == "bin" and entry.name in ebtxt_names:
            bin_path = bin_dir / entry.subdir / f"{entry.name}.bin"
            if bin_path.exists():
                snes_addr = entry.offset + 0xC00000
                text_blocks[snes_addr] = bin_path.read_bytes()

    return text_blocks


def _build_compressed_text_map(compressed_strings: list[str]) -> dict[int, str]:
    """Build index -> string mapping for compressed text expansion."""
    return {i: s for i, s in enumerate(compressed_strings) if s}


def _build_addr_to_label_map(
    dump_entries: list,
    rename_labels: dict[str, dict[int, str]],
) -> dict[int, str]:
    """Build SNES address -> label name mapping using dump entries."""
    addr_to_label: dict[int, str] = {}

    # Build name -> snes_base_address from ebtxt dump entries.
    name_to_addr: dict[str, int] = {}
    for entry in dump_entries:
        if entry.extension == "ebtxt":
            name_to_addr[entry.name] = entry.offset + 0xC00000

    for block_name, labels in rename_labels.items():
        base_addr = name_to_addr.get(block_name)
        if base_addr is None:
            continue
        for offset, label_name in labels.items():
            addr_to_label[base_addr + offset] = label_name

    return addr_to_label


def export_dialogue_yaml(
    *,
    assets_dir: Path,
    bin_dir: Path,
    doc: DumpDoc,
    common_data: CommonData,
) -> None:
    """Export ROM text to editable dialogue YAML and migrate JSON text pointers.

    Decodes raw text block binaries into human-readable dialogue YAML files
    with symbolic labels, event flag names, item names, etc.  Also converts
    text pointer fields in items.json, enemies.json, battle_actions.json,
    psi_abilities.json, and npc_config.json to inline text or dialogue
    references.

    Args:
        assets_dir: Human-friendly assets directory (e.g. ``src/assets/``).
        bin_dir: Extracted binary assets directory (e.g. ``asm/bin/``).
        doc: Loaded DumpDoc configuration.
        common_data: Loaded CommonData definitions.
    """
    text_table = doc.textTable
    compressed_text = _build_compressed_text_map(doc.compressedTextStrings)

    # Load all raw text block binaries.
    text_blocks = _load_text_blocks(bin_dir, doc.dumpEntries)
    if not text_blocks:
        print("ERROR: No text block binaries found.  Run 'ebtools extract' first.", file=sys.stderr)
        sys.exit(1)

    print(f"Loaded {len(text_blocks)} text blocks from {bin_dir}")

    # Build rename labels lookup and address-to-label map.
    rename_labels = doc.renameLabels
    addr_to_label = _build_addr_to_label_map(doc.dumpEntries, rename_labels)

    # ── 1. Generate dialogue YAML files ─────────────────────────────────
    dialogue_dir = assets_dir / "dialogue"
    dialogue_dir.mkdir(parents=True, exist_ok=True)

    # Build a global SNES address → label name map across ALL text blocks
    # so cross-block call_text references can also be resolved.
    global_addr_to_label: dict[int, str] = {}
    for entry in doc.dumpEntries:
        if entry.extension != "ebtxt":
            continue
        block_base = entry.offset + 0xC00000
        block_labels = rename_labels.get(entry.name, {})
        for label_offset, label_name in block_labels.items():
            global_addr_to_label[block_base + label_offset] = label_name

    ebtxt_entries = [e for e in doc.dumpEntries if e.extension == "ebtxt"]

    # Map block base addresses to entry names for reverse lookup.
    block_base_to_name: dict[int, str] = {}
    for entry in ebtxt_entries:
        block_base_to_name[entry.offset + 0xC00000] = entry.name

    # ── 1a. First pass: decode all blocks ────────────────────────────
    # all_messages: block_name -> {label_name: [ops...]}
    all_messages: dict[str, dict[str, list[dict]]] = {}

    for entry in ebtxt_entries:
        snes_addr = entry.offset + 0xC00000
        if snes_addr not in text_blocks:
            continue

        block_data = text_blocks[snes_addr]
        labels = rename_labels.get(entry.name, {})

        # Build messages dict: {label: [ops...]}
        messages: dict[str, list[dict]] = {}
        sorted_offsets = sorted(labels.keys())

        for i, label_offset in enumerate(sorted_offsets):
            label_name = labels[label_offset]
            start = label_offset
            end = sorted_offsets[i + 1] if i + 1 < len(sorted_offsets) else len(block_data)

            if start >= len(block_data):
                continue

            data_slice = block_data[start:end]
            decoded = decode_text_block(data_slice, text_table, compressed_text=compressed_text)

            if decoded:
                _resolve_label_addresses(decoded, global_addr_to_label)
                messages[label_name] = decoded

        if messages:
            all_messages[entry.name] = messages

    # ── 1b. Auto-generate labels for unresolved LABEL addresses ──────
    # Iterate until no new unresolved addresses are found (new auto-labels
    # may themselves reference further unresolved addresses).
    max_iterations = 10
    for _iteration in range(max_iterations):
        unresolved = _collect_unresolved_label_addresses(all_messages)
        if not unresolved:
            break

        new_labels_added = 0
        for addr in sorted(unresolved):
            if addr in global_addr_to_label:
                # Already resolved (may have been added by a previous addr in this loop).
                continue

            result = _find_block_for_address(addr, text_blocks, block_base_to_name)
            if result is None:
                # Address doesn't fall in any known text block — skip.
                continue

            block_name, block_base, block_data = result
            offset_in_block = addr - block_base

            # Generate a unique auto-label name.
            auto_label = f"_AUTO_{block_name}_{offset_in_block:04X}"
            global_addr_to_label[addr] = auto_label

            # Decode from this offset and add as a new message entry.
            data_slice = block_data[offset_in_block:]
            decoded = decode_text_block(data_slice, text_table, compressed_text=compressed_text)
            if decoded:
                _resolve_label_addresses(decoded, global_addr_to_label)
                if block_name not in all_messages:
                    all_messages[block_name] = {}
                all_messages[block_name][auto_label] = decoded
                new_labels_added += 1

        if new_labels_added == 0:
            # No new decodable labels found — re-resolve remaining refs and stop.
            for messages in all_messages.values():
                for ops in messages.values():
                    _resolve_label_addresses(ops, global_addr_to_label)
            break
    else:
        # Final re-resolve after last iteration.
        for messages in all_messages.values():
            for ops in messages.values():
                _resolve_label_addresses(ops, global_addr_to_label)

    # Re-resolve all ops one final pass to catch any stragglers.
    for messages in all_messages.values():
        for ops in messages.values():
            _resolve_label_addresses(ops, global_addr_to_label)

    # Replace numeric IDs with symbolic names from CommonData.
    for messages in all_messages.values():
        for ops in messages.values():
            _resolve_symbolic_names(ops, common_data)

    # ── 1c. Write dialogue YAML files ────────────────────────────────
    dialogue_count = 0
    for block_name, messages in all_messages.items():
        if messages:
            yaml_content = serialize_dialogue_file(messages)
            out_path = dialogue_dir / f"{block_name}.yml"
            out_path.write_text(yaml_content)
            dialogue_count += 1

    print(f"Generated {dialogue_count} dialogue YAML files in {dialogue_dir}")

    # ── 2. Migrate items.json ───────────────────────────────────────────
    items_json_path = assets_dir / "items" / "items.json"
    if items_json_path.exists():
        _migrate_items(items_json_path, text_blocks, text_table, compressed_text)
    else:
        print(f"  Skipping items (not found: {items_json_path})", file=sys.stderr)

    # ── 3. Migrate enemies.json ─────────────────────────────────────────
    enemies_json_path = assets_dir / "enemies" / "enemies.json"
    if enemies_json_path.exists():
        _migrate_enemies(enemies_json_path, text_blocks, text_table, compressed_text)
    else:
        print(f"  Skipping enemies (not found: {enemies_json_path})", file=sys.stderr)

    # ── 4. Migrate psi_abilities.json ───────────────────────────────────
    psi_json_path = assets_dir / "battle" / "psi_abilities.json"
    if psi_json_path.exists():
        _migrate_psi(psi_json_path, text_blocks, text_table, compressed_text)
    else:
        print(f"  Skipping PSI abilities (not found: {psi_json_path})", file=sys.stderr)

    # ── 5. Migrate battle_actions.json ──────────────────────────────────
    ba_json_path = assets_dir / "battle" / "battle_actions.json"
    if ba_json_path.exists():
        _migrate_battle_actions(ba_json_path, text_blocks, text_table, compressed_text)
    else:
        print(f"  Skipping battle actions (not found: {ba_json_path})", file=sys.stderr)

    # ── 6. Migrate npc_config.json ──────────────────────────────────────
    npc_json_path = assets_dir / "data" / "npc_config.json"
    if npc_json_path.exists():
        _migrate_npcs(npc_json_path, addr_to_label)
    else:
        print(f"  Skipping NPCs (not found: {npc_json_path})", file=sys.stderr)

    print("\nMigration complete.")


def migrate_text(
    assets_dir: Annotated[Path, Parameter(help="Human-friendly assets directory (e.g. src/assets/)")] = Path(
        "src/assets"
    ),
    bin_dir: Annotated[Path, Parameter(help="Extracted binary assets (e.g. asm/bin/)")] = Path("asm/bin"),
    *,
    yaml_config: Annotated[Path, Parameter(alias="-y", help="Dump doc YAML")] = Path("earthbound.yml"),
    commondata: Annotated[Path, Parameter(alias="-c", help="Common data definitions")] = Path("commondefs.yml"),
) -> None:
    """Export ROM text to editable dialogue YAML and migrate JSON text pointers.

    CLI wrapper around :func:`export_dialogue_yaml`.  Loads DumpDoc and
    CommonData from the given YAML config files, then delegates to the
    core export function.
    """
    from ebtools.config import load_common_data, load_dump_doc

    doc = load_dump_doc(yaml_config)
    common_data = load_common_data(commondata)

    export_dialogue_yaml(
        assets_dir=assets_dir,
        bin_dir=bin_dir,
        doc=doc,
        common_data=common_data,
    )


# ── JSON migration helpers ──────────────────────────────────────────────


def _migrate_pointer_field(
    entry: dict,
    pointer_field: str,
    text_field: str,
    ref_field: str,
    text_blocks: dict[int, bytes],
    text_table: dict[int, str],
    compressed_text: dict[int, str] | None,
) -> bool:
    """Migrate a text pointer field to inline text or ref.

    Returns True if the entry was modified.
    """
    pointer_str = entry.get(pointer_field)
    if not pointer_str:
        return False

    try:
        addr = int(pointer_str, 16)
    except (ValueError, TypeError):
        return False

    if addr == 0:
        return False

    result = migrate_json_text_field(addr, text_blocks, text_table, compressed_text=compressed_text)

    if result["type"] == "inline":
        entry[text_field] = result["text"]
        entry.pop(pointer_field, None)
        return True
    elif result["type"] == "ref":
        entry[ref_field] = f"0x{addr:06X}"
        return True

    return False


def _migrate_items(
    json_path: Path,
    text_blocks: dict[int, bytes],
    text_table: dict[int, str],
    compressed_text: dict[int, str] | None,
) -> None:
    """Migrate help_text_pointer fields in items.json to inline text."""
    data = json.loads(json_path.read_text())
    modified = 0

    for item in data.get("items", []):
        if _migrate_pointer_field(
            item,
            "help_text_pointer",
            "help_text",
            "help_text_ref",
            text_blocks,
            text_table,
            compressed_text,
        ):
            modified += 1

    json_path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"  Migrated {modified} item text fields in {json_path}")


def _migrate_enemies(
    json_path: Path,
    text_blocks: dict[int, bytes],
    text_table: dict[int, str],
    compressed_text: dict[int, str] | None,
) -> None:
    """Migrate text_pointer_1/2 fields in enemies.json to inline text."""
    data = json.loads(json_path.read_text())
    modified = 0

    for enemy in data.get("enemies", []):
        if _migrate_pointer_field(
            enemy,
            "text_pointer_1",
            "text_1",
            "text_1_ref",
            text_blocks,
            text_table,
            compressed_text,
        ):
            modified += 1
        if _migrate_pointer_field(
            enemy,
            "text_pointer_2",
            "text_2",
            "text_2_ref",
            text_blocks,
            text_table,
            compressed_text,
        ):
            modified += 1

    json_path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"  Migrated {modified} enemy text fields in {json_path}")


def _migrate_psi(
    json_path: Path,
    text_blocks: dict[int, bytes],
    text_table: dict[int, str],
    compressed_text: dict[int, str] | None,
) -> None:
    """Migrate text_pointer fields in psi_abilities.json to inline text."""
    data = json.loads(json_path.read_text())
    modified = 0

    for ability in data.get("abilities", []):
        if _migrate_pointer_field(
            ability,
            "text_pointer",
            "description",
            "description_ref",
            text_blocks,
            text_table,
            compressed_text,
        ):
            modified += 1

    json_path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"  Migrated {modified} PSI ability text fields in {json_path}")


def _migrate_battle_actions(
    json_path: Path,
    text_blocks: dict[int, bytes],
    text_table: dict[int, str],
    compressed_text: dict[int, str] | None,
) -> None:
    """Migrate description_pointer fields in battle_actions.json to inline text."""
    data = json.loads(json_path.read_text())
    modified = 0

    for action in data.get("actions", []):
        if _migrate_pointer_field(
            action,
            "description_pointer",
            "description",
            "description_ref",
            text_blocks,
            text_table,
            compressed_text,
        ):
            modified += 1

    json_path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"  Migrated {modified} battle action text fields in {json_path}")


def _migrate_npcs(
    json_path: Path,
    addr_to_label: dict[int, str],
) -> None:
    """Migrate NPC text_pointer fields to dialogue_ref.

    NPC text is always complex (event script dialogue), so we replace
    text_pointer with a dialogue_ref label name.
    """
    data = json.loads(json_path.read_text())
    modified = 0

    for npc in data.get("npcs", []):
        pointer_str = npc.get("text_pointer")
        if not pointer_str:
            continue

        try:
            addr = int(pointer_str, 16)
        except (ValueError, TypeError):
            continue

        if addr == 0:
            continue

        label = addr_to_label.get(addr)
        if label:
            npc["dialogue_ref"] = label
            modified += 1

    json_path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"  Migrated {modified} NPC dialogue refs in {json_path}")
