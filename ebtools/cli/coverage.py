"""Static dispatch coverage analysis for the C port."""

import re
from collections import defaultdict
from pathlib import Path

import yaml
from cyclopts import App

coverage_app = App(name="coverage", help="Analyze C port dispatch coverage against assembly source.")


@coverage_app.default
def coverage(
    project_root: Path = Path(),
    *,
    verbose: bool = False,
):
    """Check that all assembly dispatch targets have C port handlers.

    Scans event scripts, text scripts, and event macros to find all
    dispatch targets, then verifies each has a handler in the C port.
    Exits with code 1 if any gaps are found.
    """
    asm_root = project_root / "asm"
    port_root = project_root / "src"
    include_root = project_root / "include"

    if not asm_root.exists():
        raise SystemExit(f"Assembly source not found: {asm_root}")
    if not port_root.exists():
        raise SystemExit(f"C port source not found: {port_root}")

    total_missing = 0
    total_missing += check_callroutines(include_root, asm_root, port_root, verbose)
    total_missing += check_tick_callbacks(asm_root, port_root, verbose)
    total_missing += check_entity_callbacks(asm_root, port_root, verbose)
    total_missing += check_text_control_codes(include_root, asm_root, port_root, verbose)
    total_missing += check_script_bank_coverage(project_root, asm_root, verbose)

    if total_missing:
        print(f"\n{'=' * 60}")
        print(f"TOTAL MISSING: {total_missing} dispatch target(s)")
        raise SystemExit(1)
    else:
        print(f"\n{'=' * 60}")
        print("All dispatch targets covered.")


_RE_CALLROUTINE = re.compile(r"EVENT_CALLROUTINE\s+([A-Z_][A-Z0-9_]*)")
_RE_ROM_ADDR_DEFINE = re.compile(r"#define\s+ROM_ADDR_([A-Z_][A-Z0-9_]*)\s+(0x[0-9A-Fa-f]+)")
_RE_ROM_ADDR_CASE = re.compile(r"case\s+ROM_ADDR_([A-Z_][A-Z0-9_]*)\s*:")

_RE_TICK_CALLBACK = re.compile(r"EVENT_SET_TICK_CALLBACK\s+([A-Z_][A-Z0-9_]*)")
_RE_TICK_ADDR_DEFINE = re.compile(r"#define\s+TICK_ADDR_([A-Z_][A-Z0-9_]*)\s+(0x[0-9A-Fa-f]+)")
_RE_TICK_ADDR_CASE = re.compile(r"case\s+TICK_ADDR_([A-Z_][A-Z0-9_]*)\s*:")


def _extract_callroutine_labels_from_asm(include_root, asm_root):
    """Extract all unique callroutine target labels from assembly.

    Returns dict mapping label_name -> list of source locations.
    """
    labels: dict[str, list[str]] = defaultdict(list)

    # 1. Parse wrapper macros from include/eventmacros.asm
    eventmacros_path = include_root / "eventmacros.asm"
    if eventmacros_path.exists():
        for lineno, line in enumerate(eventmacros_path.read_text().splitlines(), 1):
            # Strip comments (text after ;)
            comment_idx = line.find(";")
            if comment_idx >= 0:
                line = line[:comment_idx]
            for m in _RE_CALLROUTINE.finditer(line):
                labels[m.group(1)].append(f"{eventmacros_path}:{lineno}")

    # 2. Parse direct usage from asm/data/events/**/*.asm
    events_dir = asm_root / "data" / "events"
    if events_dir.exists():
        for asm_file in sorted(events_dir.rglob("*.asm")):
            for lineno, line in enumerate(asm_file.read_text().splitlines(), 1):
                comment_idx = line.find(";")
                if comment_idx >= 0:
                    line = line[:comment_idx]
                for m in _RE_CALLROUTINE.finditer(line):
                    labels[m.group(1)].append(f"{asm_file}:{lineno}")

    return labels


def _extract_callroutine_handlers_from_c(port_root):
    """Extract callroutine labels that have both a ROM_ADDR_ define and a case handler.

    Returns
    -------
        names: set of C define label names (intersection of defined & handled)
        addresses: set of int ROM addresses that are handled
    """
    # Parse #define ROM_ADDR_<LABEL> <addr> from event_script_data.h
    header_path = port_root / "data" / "event_script_data.h"
    defined: dict[str, int] = {}  # C name -> address
    if header_path.exists():
        for line in header_path.read_text().splitlines():
            m = _RE_ROM_ADDR_DEFINE.search(line)
            if m:
                defined[m.group(1)] = int(m.group(2), 16)

    # Parse case ROM_ADDR_<LABEL>: from callroutine.c
    callroutine_path = port_root / "entity" / "callroutine.c"
    handled: set[str] = set()
    if callroutine_path.exists():
        for line in callroutine_path.read_text().splitlines():
            m = _RE_ROM_ADDR_CASE.search(line)
            if m:
                handled.add(m.group(1))

    # Build set of handled ROM addresses
    handled_names = set(defined.keys()) & handled
    handled_addresses = {defined[name] for name in handled_names if name in defined}

    return handled_names, handled_addresses


def check_callroutines(include_root, asm_root, port_root, verbose):
    """Check callroutine coverage: ASM labels vs C port handlers."""
    project_root = include_root.parent

    asm_labels = _extract_callroutine_labels_from_asm(include_root, asm_root)
    c_handler_names, c_handler_addrs = _extract_callroutine_handlers_from_c(port_root)

    # Build linker map for address-based matching
    linker_map = _parse_linker_map(project_root)

    missing = []
    jp_only = []
    matched = []

    for asm_label in sorted(asm_labels.keys()):
        # 1. Direct name match
        if asm_label in c_handler_names:
            matched.append(asm_label)
            continue

        # 2. Address-based match via linker map
        addr = linker_map.get(asm_label)
        if addr is not None:
            # Skip RAM addresses (7Exxxx range) — not valid ROM callroutines
            if (addr & 0xFF0000) == 0x7E0000:
                matched.append(asm_label)
                continue
            if addr in c_handler_addrs:
                matched.append(asm_label)
                continue
            missing.append(asm_label)
        else:
            # Label not in US linker map — likely JP-only
            jp_only.append(asm_label)

    total = len(asm_labels)
    handled = len(matched)

    print(f"\n--- Callroutine coverage: {handled}/{total} ---")

    if verbose:
        for label in missing:
            locations = asm_labels[label]
            loc_str = ", ".join(locations[:3])
            if len(locations) > 3:
                loc_str += f" (+{len(locations) - 3} more)"
            print(f"  MISSING: {label}  (used at: {loc_str})")
        for label in jp_only:
            locations = asm_labels[label]
            loc_str = ", ".join(locations[:3])
            if len(locations) > 3:
                loc_str += f" (+{len(locations) - 3} more)"
            print(f"  JP-only (not in US build): {label}  (used at: {loc_str})")

    if missing:
        print(f"  {len(missing)} callroutine(s) missing C port handler.")
    else:
        print("  All callroutines handled.")
    if jp_only:
        print(f"  {len(jp_only)} callroutine(s) JP-only (not in US build).")

    return len(missing)


def _parse_linker_map(project_root):
    """Parse build/earthbound.map to build a label -> address mapping.

    The map file has lines like:
        LABEL_NAME  C0XXXX RLF    LABEL2  C1XXXX RLA
    Each symbol is: NAME  HEXADDR  TYPE, separated by whitespace.

    Returns dict mapping label_name -> int address, or empty dict if map unavailable.
    """
    map_path = project_root / "build" / "earthbound.map"
    if not map_path.exists():
        return {}

    symbols: dict[str, int] = {}
    # Match sequences of: IDENTIFIER HEXADDR TYPE
    sym_re = re.compile(r"([A-Z_][A-Z0-9_]*)\s+([0-9A-Fa-f]{6})\s+[A-Z]{2,4}")
    for line in map_path.read_text().splitlines():
        for m in sym_re.finditer(line):
            label = m.group(1)
            addr = int(m.group(2), 16)
            symbols[label] = addr
    return symbols


def check_tick_callbacks(asm_root, port_root, verbose):
    """Check tick callback coverage: ASM labels vs C port handlers."""
    project_root = asm_root.parent

    # 1. Extract all EVENT_SET_TICK_CALLBACK labels from event scripts
    asm_labels: dict[str, list[str]] = defaultdict(list)
    events_dir = asm_root / "data" / "events"
    if events_dir.exists():
        for asm_file in sorted(events_dir.rglob("*.asm")):
            for lineno, line in enumerate(asm_file.read_text().splitlines(), 1):
                comment_idx = line.find(";")
                if comment_idx >= 0:
                    line = line[:comment_idx]
                for m in _RE_TICK_CALLBACK.finditer(line):
                    asm_labels[m.group(1)].append(f"{asm_file}:{lineno}")

    # 2. Extract TICK_ADDR defines and case handlers from callroutine.c
    callroutine_path = port_root / "entity" / "callroutine.c"
    tick_defines: dict[str, int] = {}  # C name -> hex address
    tick_handled: set[str] = set()  # C names with case handlers

    if callroutine_path.exists():
        text = callroutine_path.read_text()
        for line in text.splitlines():
            m = _RE_TICK_ADDR_DEFINE.search(line)
            if m:
                tick_defines[m.group(1)] = int(m.group(2), 16)
            m = _RE_TICK_ADDR_CASE.search(line)
            if m:
                tick_handled.add(m.group(1))

    # Build reverse map: address -> set of C define names that are handled
    addr_to_handled: dict[int, set[str]] = defaultdict(set)
    for name, addr in tick_defines.items():
        if name in tick_handled:
            addr_to_handled[addr].add(name)

    # 3. Match ASM labels to C handlers via linker map (address-based)
    linker_map = _parse_linker_map(project_root)

    missing = []
    matched = []

    for asm_label in sorted(asm_labels.keys()):
        # Try address-based matching via linker map
        addr = linker_map.get(asm_label)
        if addr is not None and addr in addr_to_handled:
            matched.append(asm_label)
            continue

        # Fallback: substring matching against C define names
        # e.g. ASM "ACTIONSCRIPT_SIMPLE_SCREEN_POSITION_CALLBACK" matches
        #      C   "SIMPLE_SCREEN_POS_CALLBACK"
        found = False
        for c_name in tick_handled:
            # Check if either is a substring of the other (after removing common prefixes)
            asm_clean = asm_label.replace("ACTIONSCRIPT_", "")
            if c_name in asm_clean or asm_clean in c_name:
                found = True
                break
            # Also try direct substring
            if c_name in asm_label or asm_label in c_name:
                found = True
                break
        if found:
            matched.append(asm_label)
        else:
            missing.append(asm_label)

    total = len(asm_labels)
    handled = len(matched)

    print(f"\n--- Tick callback coverage: {handled}/{total} ---")

    if verbose:
        for label in missing:
            locations = asm_labels[label]
            loc_str = ", ".join(locations[:3])
            if len(locations) > 3:
                loc_str += f" (+{len(locations) - 3} more)"
            print(f"  MISSING: {label}  (used at: {loc_str})")

    if missing:
        print(f"  {len(missing)} tick callback(s) missing C port handler.")
    else:
        print("  All tick callbacks handled.")

    return len(missing)


def _switch_has_default(source_text, case_prefix):
    """Check if the switch block handling cases with the given prefix has a default: branch.

    Searches for the first ``case <prefix>`` occurrence, walks backward to
    find the enclosing ``switch`` statement, then scans the entire switch
    block for a ``default:`` label.
    """
    lines = source_text.splitlines()
    case_re = re.compile(rf"case\s+{case_prefix}")
    first_case_line = None
    for i, line in enumerate(lines):
        if case_re.search(line):
            first_case_line = i
            break
    if first_case_line is None:
        return False

    # Walk backward from first case to find the ``switch (...)  {`` line
    switch_line = first_case_line
    for i in range(first_case_line - 1, max(first_case_line - 20, -1), -1):
        if "switch" in lines[i]:
            switch_line = i
            break

    # Scan forward from the switch line, tracking brace depth
    brace_depth = 0
    in_body = False
    for i in range(switch_line, len(lines)):
        line = lines[i]
        for ch in line:
            if ch == "{":
                brace_depth += 1
                in_body = True
            elif ch == "}":
                brace_depth -= 1
        if in_body and re.search(r"\bdefault\s*:", line):
            return True
        if in_body and brace_depth <= 0:
            break
    return False


def check_entity_callbacks(asm_root, port_root, verbose):
    """Check draw/pos/move callback ID coverage."""
    total_missing = 0
    callbacks_c = port_root / "entity" / "callbacks.c"
    header = port_root / "data" / "event_script_data.h"

    if not header.exists():
        print("\n--- Entity callback coverage: header not found ---")
        return 0
    if not callbacks_c.exists():
        print("\n--- Entity callback coverage: callbacks.c not found ---")
        return 0

    header_text = header.read_text()
    callbacks_text = callbacks_c.read_text()

    for prefix, label in [("CB_DRAW_", "Draw"), ("CB_POS_", "Position"), ("CB_MOVE_", "Move")]:
        re_define = re.compile(rf"#define\s+({prefix}[A-Z_][A-Z0-9_]*)\s+(\d+)")
        re_case = re.compile(rf"case\s+({prefix}[A-Z_][A-Z0-9_]*)\s*:")

        defined = {m.group(1) for m in re_define.finditer(header_text)}
        handled = {m.group(1) for m in re_case.finditer(callbacks_text)}

        # Check if the relevant switch block has a default: handler
        has_default = _switch_has_default(callbacks_text, prefix)

        missing = sorted(defined - handled)
        default_handled = []
        truly_missing = []
        if has_default:
            for name in missing:
                default_handled.append(name)
            # These are handled by default:, not truly missing
        else:
            truly_missing = missing

        n_defined = len(defined)
        n_handled = n_defined - len(truly_missing)

        print(f"\n--- {label} callback coverage: {n_handled}/{n_defined} ---")

        if verbose:
            for name in truly_missing:
                print(f"  MISSING: {name}")

        if default_handled:
            for name in default_handled:
                print(f"  NOTE: {name} handled by default: branch")

        if truly_missing:
            print(f"  {len(truly_missing)} {label.lower()} callback(s) missing C port handler.")
        else:
            print(f"  All {label.lower()} callbacks handled.")

        total_missing += len(truly_missing)

    return total_missing


def _parse_textmacros(include_root):
    """Parse include/textmacros.asm to build macro name -> byte tuple mapping.

    Returns dict like {"EBTEXT_HALT_WITH_PROMPT": (0x00,), "EBTEXT_GENERATE_ACTIVE_SPRITE": (0x1F, 0x15), ...}
    """
    path = include_root / "textmacros.asm"
    if not path.exists():
        return {}

    macros: dict[str, tuple[int, ...]] = {}
    current_macro = None
    byte_re = re.compile(r"\.BYTE\s+(.+)")

    for line in path.read_text().splitlines():
        stripped = line.strip()
        # Strip comments
        comment_idx = stripped.find(";")
        if comment_idx >= 0:
            stripped = stripped[:comment_idx].strip()

        if stripped.upper().startswith(".MACRO "):
            parts = stripped.split()
            if len(parts) >= 2:
                current_macro = parts[1]
        elif stripped.upper() == ".ENDMACRO":
            current_macro = None
        elif current_macro and current_macro not in macros:
            # Look for the first .BYTE directive in this macro
            m = byte_re.match(stripped)
            if m:
                # Parse the byte values: "$1F, $15" or "$00" or "$1F, .PARAMCOUNT"
                byte_args = m.group(1).split(",")
                byte_vals = []
                for arg in byte_args:
                    arg = arg.strip()
                    if arg.startswith("$"):
                        try:
                            byte_vals.append(int(arg[1:], 16))
                        except ValueError:
                            break
                    else:
                        # Not a hex literal (e.g. .PARAMCOUNT, arg) — stop here
                        break
                if byte_vals:
                    macros[current_macro] = tuple(byte_vals)

    return macros


def _find_used_text_macros(asm_root):
    """Scan .ebtxt.uncompressed files to find which EBTEXT_* macros are used.

    Returns set of macro names (e.g. {"EBTEXT_HALT_WITH_PROMPT", ...}).
    """
    used = set()
    text_dir = asm_root / "bin" / "US" / "text_data"
    if not text_dir.exists():
        return used

    macro_re = re.compile(r"(EBTEXT_[A-Z_][A-Z0-9_]*)")
    for f in text_dir.rglob("*.ebtxt.uncompressed"):
        for line in f.read_text().splitlines():
            comment_idx = line.find(";")
            if comment_idx >= 0:
                line = line[:comment_idx]
            for m in macro_re.finditer(line):
                used.add(m.group(1))

    return used


def _parse_display_text_cases(port_root):
    """Parse display_text.c to find all handled CC byte values per dispatch context.

    Returns
    -------
        main_cases: set of int — case values in the main dispatch loop
        sub_cases: dict[int, set[int]] — prefix byte -> set of sub-case values
    """
    path = port_root / "game" / "display_text.c"
    if not path.exists():
        return set(), {}

    text = path.read_text()
    lines = text.splitlines()

    dispatch_re = re.compile(r"static void cc_([0-9a-f]{2})_dispatch\b")
    case_re = re.compile(r"case\s+0x([0-9a-fA-F]+)\s*:")
    # Also match "if (byte == 0xNN)" or "byte == 0xNN" style checks
    byte_eq_re = re.compile(r"byte\s*==\s*0x([0-9a-fA-F]+)")

    def _extract_function_cases(start_line):
        """Extract all case values from a function starting at start_line."""
        brace_depth = 0
        in_body = False
        cases: set[int] = set()
        for j in range(start_line, len(lines)):
            line = lines[j]
            for ch in line:
                if ch == "{":
                    brace_depth += 1
                    in_body = True
                elif ch == "}":
                    brace_depth -= 1
            if in_body:
                for cm in case_re.finditer(line):
                    cases.add(int(cm.group(1), 16))
            if in_body and brace_depth == 0:
                return cases, j
        return cases, len(lines)

    # Extract sub-dispatch function cases and their line ranges
    sub_cases: dict[int, set[int]] = {}
    sub_ranges: list[tuple[int, int]] = []  # (start, end) line indices

    for i, line in enumerate(lines):
        dm = dispatch_re.search(line)
        if dm:
            prefix = int(dm.group(1), 16)
            cases, end_line = _extract_function_cases(i)
            sub_cases[prefix] = cases
            sub_ranges.append((i, end_line))

    # Find the main dispatch: look for "cc_18_dispatch" call (not the function def)
    # and walk backwards to find "switch (byte)" or similar
    main_cases: set[int] = set()
    main_dispatch_re = re.compile(r"cc_18_dispatch\s*\(\s*&")
    for i, line in enumerate(lines):
        if main_dispatch_re.search(line):
            # Check this isn't inside a sub-dispatch function
            in_sub = any(s <= i <= e for s, e in sub_ranges)
            if in_sub:
                continue
            # Walk backwards to find the enclosing switch
            switch_line = None
            for j in range(i, max(i - 200, -1), -1):
                if "switch" in lines[j] and "byte" in lines[j]:
                    switch_line = j
                    break
            if switch_line is not None:
                # Also scan the area before the switch for "if (byte == 0xNN)" checks
                # (e.g. END_BLOCK is handled before the switch via an if statement)
                for j in range(max(switch_line - 50, 0), switch_line):
                    for bm in byte_eq_re.finditer(lines[j]):
                        main_cases.add(int(bm.group(1), 16))

                # Extract cases from this switch block, excluding sub-dispatch ranges
                brace_depth = 0
                in_body = False
                for j in range(switch_line, len(lines)):
                    line_j = lines[j]
                    in_sub = any(s <= j <= e for s, e in sub_ranges)
                    for ch in line_j:
                        if ch == "{":
                            brace_depth += 1
                            in_body = True
                        elif ch == "}":
                            brace_depth -= 1
                    if in_body and not in_sub:
                        for cm in case_re.finditer(line_j):
                            main_cases.add(int(cm.group(1), 16))
                    if in_body and brace_depth == 0:
                        break
            break

    # Also scan cc_skip_args for additional main CC coverage
    skip_re = re.compile(r"static void cc_skip_args\b")
    for i, line in enumerate(lines):
        if skip_re.search(line):
            cases, _ = _extract_function_cases(i)
            main_cases |= cases
            break

    return main_cases, sub_cases


def check_text_control_codes(include_root, asm_root, port_root, verbose):
    """Check text control code coverage: ASM macros vs C port handlers."""
    # 1. Parse textmacros.asm for macro -> byte values
    macros = _parse_textmacros(include_root)

    # 2. Find which macros are actually used in text data
    used_macros = _find_used_text_macros(asm_root)

    # 3. Parse display_text.c for handled case values
    main_cases, sub_cases = _parse_display_text_cases(port_root)

    # 4. Match: for each used macro, check if its byte values are handled
    missing = []
    handled_count = 0

    for macro_name in sorted(used_macros):
        if macro_name not in macros:
            # Macro not found in textmacros.asm — might be defined elsewhere, skip
            continue

        byte_vals = macros[macro_name]
        cc_byte = byte_vals[0]

        if len(byte_vals) == 1:
            # Simple CC (0x00-0x14): check main dispatch
            if cc_byte in main_cases:
                handled_count += 1
            else:
                missing.append((macro_name, byte_vals))
        elif len(byte_vals) >= 2:
            # Prefix CC (0x15-0x1F + sub-opcode)
            sub_byte = byte_vals[1]
            prefix_handled = cc_byte in main_cases
            sub_handled = cc_byte in sub_cases and sub_byte in sub_cases[cc_byte]

            if prefix_handled and sub_handled:
                handled_count += 1
            else:
                missing.append((macro_name, byte_vals))

    total = handled_count + len(missing)
    print(f"\n--- Text control code coverage: {handled_count}/{total} ---")

    if verbose:
        for macro_name, byte_vals in missing:
            hex_str = ", ".join(f"0x{b:02X}" for b in byte_vals)
            print(f"  MISSING: {macro_name}  (bytes: {hex_str})")

    if missing:
        print(f"  {len(missing)} text control code(s) missing C port handler.")
    else:
        print("  All text control codes handled.")

    return len(missing)


def _parse_script_bank_ranges(project_root):
    """Parse earthbound.yml to find all extracted event script bank regions.

    Returns list of (bank_number, within_bank_start, within_bank_end) tuples.
    """
    yml_path = project_root / "earthbound.yml"
    if not yml_path.exists():
        return []

    with yml_path.open() as f:
        doc = yaml.safe_load(f)

    ranges = []
    for entry in doc.get("dumpEntries", []):
        name = entry.get("name", "")
        subdir = entry.get("subdir", "")
        if "script" not in name:
            continue
        if "events" not in subdir and "intro" not in subdir:
            continue

        offset = entry["offset"]
        size = entry["size"]
        # HiROM: offset 0x03xxxx → bank 0xC3, within-bank = offset & 0xFFFF
        bank = (offset >> 16) + 0xC0
        within_start = offset & 0xFFFF
        within_end = within_start + size
        ranges.append((bank, within_start, within_end))

    return ranges


_RE_SHORTCALL = re.compile(r"EVENT_SHORTCALL(?:_CONDITIONAL(?:_NOT)?)?\s+\.LOWORD\(([A-Z_][A-Z0-9_]*)\)")
_RE_SHORTJUMP = re.compile(r"EVENT_SHORTJUMP\s+\.LOWORD\(([A-Z_][A-Z0-9_]*)\)")


def check_script_bank_coverage(project_root, asm_root, verbose):
    """Check that all SHORTCALL/SHORTJUMP global targets fall within extracted script banks."""
    bank_ranges = _parse_script_bank_ranges(project_root)
    if not bank_ranges:
        print("\n--- Script bank coverage: earthbound.yml not found ---")
        return 0

    # Find all global SHORTCALL/SHORTJUMP targets in event scripts
    targets: dict[str, list[str]] = defaultdict(list)
    events_dir = asm_root / "data" / "events"
    if events_dir.exists():
        for asm_file in sorted(events_dir.rglob("*.asm")):
            for lineno, line in enumerate(asm_file.read_text().splitlines(), 1):
                comment_idx = line.find(";")
                if comment_idx >= 0:
                    line = line[:comment_idx]
                for pattern in (_RE_SHORTCALL, _RE_SHORTJUMP):
                    for m in pattern.finditer(line):
                        targets[m.group(1)].append(f"{asm_file.relative_to(events_dir)}:{lineno}")

    if not targets:
        print("\n--- Script bank coverage: no SHORTCALL/SHORTJUMP targets found ---")
        return 0

    linker_map = _parse_linker_map(project_root)

    missing = {}
    covered = 0
    unresolved = []

    for label in sorted(targets):
        addr = linker_map.get(label)
        if addr is None:
            unresolved.append(label)
            continue

        bank = (addr >> 16) & 0xFF
        within_bank = addr & 0xFFFF

        in_range = any(b == bank and start <= within_bank < end for b, start, end in bank_ranges)

        if in_range:
            covered += 1
        else:
            bank_ranges_for_bank = [(s, e) for b, s, e in bank_ranges if b == bank]
            missing[label] = (addr, bank, within_bank, bank_ranges_for_bank)

    total = covered + len(missing)
    print(f"\n--- Script bank coverage: {covered}/{total} ---")

    if verbose or missing:
        for label, (addr, bank, within_bank, ranges_for_bank) in sorted(missing.items()):
            locs = ", ".join(targets[label][:3])
            range_str = ", ".join(f"[0x{s:04X}-0x{e:04X})" for s, e in ranges_for_bank)
            print(
                f"  MISSING: {label} (0x{addr:06X}, bank ${bank:02X}:${within_bank:04X}) "
                f"not in ranges {range_str or 'NONE'}  (used at: {locs})"
            )

    if unresolved and verbose:
        print(f"  ({len(unresolved)} labels not in linker map — JP-only or local)")

    if missing:
        print(f"  {len(missing)} SHORTCALL/SHORTJUMP target(s) outside extracted script banks.")
    else:
        print("  All SHORTCALL/SHORTJUMP targets within extracted script banks.")

    return len(missing)
