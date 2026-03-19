"""HALLZ2 compression/decompression CLI commands."""

import sys
from pathlib import Path
from typing import Annotated

from cyclopts import App, Parameter

hallz_app = App(name="hallz", help="HALLZ2 compression/decompression utilities.")


@hallz_app.command(name="compress")
def hallz_compress_cmd(
    input_path: Annotated[Path, Parameter(help="Input file to compress")],
    *,
    output: Annotated[
        Path | None, Parameter(alias="-o", help="Output path (default: input with .lzhal suffix)")
    ] = None,
) -> None:
    """Compress a file using HALLZ2.

    If no output path is given, appends .lzhal to the input filename.
    """
    from ebtools.hallz import compress

    if not input_path.exists():
        print(f"Error: {input_path} not found", file=sys.stderr)
        sys.exit(1)

    data = input_path.read_bytes()
    compressed = compress(data)

    out = output if output is not None else input_path.with_suffix(input_path.suffix + ".lzhal")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(compressed)
    ratio = len(compressed) / len(data) * 100 if data else 0
    print(f"Compressed {input_path} ({len(data)} -> {len(compressed)} bytes, {ratio:.1f}%) -> {out}")


@hallz_app.command(name="decompress")
def hallz_decompress_cmd(
    input_path: Annotated[Path, Parameter(help="Input .lzhal file to decompress")],
    *,
    output: Annotated[Path | None, Parameter(alias="-o", help="Output path (default: strip .lzhal suffix)")] = None,
) -> None:
    """Decompress a HALLZ2-compressed file.

    If no output path is given, strips the .lzhal suffix (or appends .bin).
    """
    from ebtools.hallz import decompress

    if not input_path.exists():
        print(f"Error: {input_path} not found", file=sys.stderr)
        sys.exit(1)

    data = input_path.read_bytes()
    decompressed = decompress(data)

    if output is not None:
        out = output
    elif input_path.suffix == ".lzhal":
        out = input_path.with_suffix("")
    else:
        out = input_path.with_suffix(input_path.suffix + ".bin")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(decompressed)
    print(f"Decompressed {input_path} ({len(data)} -> {len(decompressed)} bytes) -> {out}")
