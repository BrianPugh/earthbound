"""The ``analyze-dump`` subcommand group: inspect debug state dump files."""

import sys
from pathlib import Path
from typing import Annotated

from cyclopts import App, Parameter

dump_app = App(name="analyze-dump", help="Analyze debug state dump files (.bin)")


@dump_app.command
def info(
    dump_file: Path,
    *,
    struct_info: Annotated[Path | None, Parameter(help="Path to struct_info.json")] = None,
) -> None:
    """Show header and per-section field values from a state dump file."""
    from ebtools.state_dump import load_struct_info, parse_dump, print_info

    dump = parse_dump(dump_file)
    si = load_struct_info(struct_info)
    print_info(dump, si)


@dump_app.command
def diff(
    file_a: Path,
    file_b: Path,
    *,
    struct_info: Annotated[Path | None, Parameter(help="Path to struct_info.json")] = None,
    max_diffs: int = 32,
) -> None:
    """Show field-level differences between two state dump files."""
    from ebtools.state_dump import load_struct_info, parse_dump, print_diff

    dump_a = parse_dump(file_a)
    dump_b = parse_dump(file_b)
    si = load_struct_info(struct_info)
    print_diff(dump_a, dump_b, si, max_diffs=max_diffs)


@dump_app.command
def hexdump(
    dump_file: Path,
    section: Annotated[str, Parameter(help="Section name (e.g. RNG, CORE, PPU)")],
    *,
    struct_info: Annotated[Path | None, Parameter(help="Path to struct_info.json")] = None,
) -> None:
    """Show raw hex dump of a single section from a state dump file."""
    from ebtools.state_dump import parse_dump, print_hexdump

    dump = parse_dump(dump_file)
    for sec in dump.sections:
        if sec.name.upper() == section.upper():
            print_hexdump(sec)
            return

    available = ", ".join(s.name for s in dump.sections)
    print(f"Section '{section}' not found. Available: {available}", file=sys.stderr)
    sys.exit(1)
