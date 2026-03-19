"""Distortion config parser: 17-byte records -> assembly.

Ported from app.d:parseDistortion.
"""

from pathlib import Path

from ebtools.byte_reader import ByteReader
from ebtools.config import CommonData, DumpDoc

DISTORTION_STYLES = [
    "NONE",
    "HORIZONTAL_SMOOTH",
    "HORIZONTAL_INTERLACED",
    "VERTICAL_SMOOTH",
    "UNKNOWN",
]


def parse_distortion(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Parse distortion effect configuration (17-byte records)."""
    filename = f"{base_name}.{extension}"

    with (dir / filename).open("w") as out:
        for i in range(0, len(source), 17):
            entry = source[i : i + 17]
            if len(entry) < 17:
                break

            r = ByteReader(entry)

            out.write(f"  .BYTE ${r.read_byte():02X} ;Unknown\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Unknown\n")
            out.write(f"  .BYTE DISTORTION_STYLE::{DISTORTION_STYLES[r.read_byte()]}\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Ripple frequency\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Ripple amplitude\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Unknown\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Unknown\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Unknown\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Ripple frequency acceleration\n")
            out.write(f"  .WORD ${r.read_le16():04X} ;Ripple amplitude acceleration\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Speed\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Unknown\n")
            out.write(f"  .BYTE ${r.read_byte():02X} ;Unknown\n")
            out.write("\n")

    return [filename]
