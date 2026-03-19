"""Compressed text parser: null-delimited strings decoded via textTable.

Ported from app.d:parseCompressedText.
"""

from pathlib import Path

from ebtools.config import CommonData, DumpDoc


def parse_compressed_text(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Parse compressed text (null-delimited strings)."""
    filename = f"{base_name}.{extension}"

    with (dir / filename).open("w") as out_file:
        for c in source:
            if c == 0x00:
                out_file.write("\n")
                continue
            out_file.write(doc.textTable.get(c, "ERROR!!!"))
        out_file.write("\n")

    return [filename]
