"""Overworld flyover text parser (5 opcodes).

Ported from flyover.d.
"""

from pathlib import Path

from ebtools.config import CommonData, DumpDoc


def parse_flyover(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Parse flyover text data.

    Outputs:
    - .flyover (main text)
    - .symbols.asm (label declarations)
    """
    filename = f"{base_name}.flyover"
    symbol_filename = f"{base_name}.symbols.asm"

    pos = 0
    current_offset = offset
    tmpbuff = ""
    raw: list[int] = []
    raw2: list[int] = []

    def next_byte() -> int:
        nonlocal pos, current_offset
        val = source[pos]
        pos += 1
        current_offset += 1
        return val

    with (
        (dir / filename).open("w") as out_file,
        (dir / symbol_filename).open("w") as symbol_file,
    ):

        def print_label() -> None:
            symbol = doc.flyoverLabels.get(current_offset, f"FLYOVER_{current_offset:06X}")
            symbol_file.write(f".GLOBAL {symbol}: far\n")
            out_file.write(f"{symbol}: ;${current_offset:06X}\n")

        def flush_buff() -> None:
            nonlocal tmpbuff, raw, raw2
            if not tmpbuff:
                return
            if doc.dontUseTextTable:
                if doc.multibyteFlyovers:
                    words = ", ".join(f"${w:04X}" for w in raw2)
                    out_file.write(f'\t.WORD {words} ;"{tmpbuff}"\n')
                else:
                    bytes_str = ", ".join(f"${b:02X}" for b in raw)
                    out_file.write(f'\t.BYTE {bytes_str} ;"{tmpbuff}"\n')
            else:
                out_file.write(f'\tEBTEXT "{tmpbuff}"\n')
            tmpbuff = ""
            raw = []
            raw2 = []

        base = Path(base_name).name
        out_file.write(f'.INCLUDE "{base}.symbols.asm"\n\n')

        # Print initial label
        print_label()

        while pos < len(source):
            first = next_byte()

            # Handle multibyte flyovers (JP)
            if doc.multibyteFlyovers and first >= 0x80:
                first = (first << 8) | next_byte()

            # Check text table
            if first in doc.flyoverTextTable:
                tmpbuff += doc.flyoverTextTable[first]
                if doc.multibyteFlyovers:
                    raw2.append((first >> 8) | ((first & 0xFF) << 8))
                else:
                    raw.append(first & 0xFF)
                continue

            flush_buff()

            if first == 0x00:
                out_file.write("\tEBFLYOVER_END\n")
                if pos < len(source):
                    out_file.write("\n")
                    print_label()
            elif first == 0x01:
                arg = next_byte()
                out_file.write(f"\tEBFLYOVER_01 ${arg:02X}\n")
            elif first == 0x02:
                arg = next_byte()
                out_file.write(f"\tEBFLYOVER_02 ${arg:02X}\n")
            elif first == 0x08:
                arg = next_byte()
                out_file.write(f"\tEBFLYOVER_08 ${arg:02X}\n")
            elif first == 0x09:
                out_file.write("\tEBFLYOVER_09\n")
            else:
                if doc.multibyteFlyovers and first >= 0x80:
                    out_file.write(f"\t.WORD ${(first >> 8) | ((first & 0xFF) << 8):04X} ;???\n")
                else:
                    out_file.write(f"\t.BYTE ${first:02X} ;???\n")

    return [filename, symbol_filename]
