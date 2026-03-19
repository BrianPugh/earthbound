"""Credits/staff text parser (5 opcodes).

Ported from app.d:parseStaffText.
"""

from pathlib import Path

from ebtools.config import CommonData, DumpDoc


def parse_staff_text(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Parse staff/credits text."""
    filename = f"{base_name}.{extension}"

    pos = 0

    def next_byte() -> int:
        nonlocal pos, offset
        val = source[pos]
        pos += 1
        offset += 1
        return val

    def read_text_string() -> str:
        tmpbuff = ""
        while True:
            arg = next_byte()
            if arg == 0:
                break
            if doc.dontUseTextTable:
                tmpbuff += f"\\x{arg:02X}"
            else:
                if arg in doc.staffTextTable:
                    tmpbuff += doc.staffTextTable[arg]
                else:
                    tmpbuff += f"[{arg:02X}]"
        return tmpbuff

    with (dir / filename).open("w") as out:
        while pos < len(source):
            first = next_byte()

            if first == 0x01:
                text = read_text_string()
                out.write(f'\tEBSTAFF_SMALLTEXT "{text}"\n')
            elif first == 0x02:
                text = read_text_string()
                out.write(f'\tEBSTAFF_BIGTEXT "{text}"\n')
            elif first == 0x03:
                arg = next_byte()
                out.write(f"\tEBSTAFF_VERTICALSPACE ${arg:02X}\n")
            elif first == 0x04:
                out.write("\tEBSTAFF_PRINTPLAYER\n")
            elif first == 0xFF:
                out.write("\tEBSTAFF_ENDCREDITS\n")
            else:
                out.write(f"UNHANDLED: {first:02X}\n")

    return [filename]
