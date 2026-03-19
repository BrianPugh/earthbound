"""Movement script parser: ~70 opcodes + callroutine arg table.

Ported from app.d:parseMovement.
"""

import sys
from pathlib import Path

from ebtools.config import CommonData, DumpDoc

# Callroutine address -> argument count
# fmt: off
CALLROUTINE_ARGS: dict[int, int] = {
    0xC0AA07: 6, 0xC0AA23: 6, 0xC0A9B3: 6, 0xC0A9CF: 6, 0xC0A9EB: 6,
    0xC0A912: 5,
    0xC05E76: 4, 0xC0A87A: 4, 0xC0A88D: 4, 0xC0A8A0: 4, 0xC0A8B3: 4,
    0xC0A964: 4, 0xC0A98B: 4, 0xC0AAB5: 4, 0xC0A977: 4, 0xC0A99F: 4,
    0xC0AA3F: 3, 0xC0AAD5: 3,
    0xC09E71: 2, 0xC09FAE: 2, 0xC09FBB: 2, 0xC0A643: 2, 0xC0A685: 2,
    0xC0A6A2: 2, 0xC0A6AD: 2, 0xC0A841: 2, 0xC0A84C: 2, 0xC0A857: 2,
    0xC0A86F: 2, 0xC0A92D: 2, 0xC0A938: 2, 0xC0A94E: 2, 0xC0A959: 2,
    0xC0AA6E: 2,
    0xC0A651: 1, 0xC0A679: 1, 0xC0A697: 1, 0xC0A864: 1, 0xC0A907: 1,
    0xC0A943: 1,
}

# Variable arg count: 0xC09F82 reads first byte as N, then N*2 more bytes
VARIABLE_ARG_ROUTINE = 0xC09F82

# Routines with 0 args
ZERO_ARG_ROUTINES = frozenset([
    0xC020F1, 0xC0F3B2, 0xC0F3E8, 0xC46E46, 0xC2DB3F, 0xC0ED5C, 0xC09451,
    0xC0A4A8, 0xC0A4BF, 0xC0D77F, 0xC2654C, 0xC03F1E, 0xC0A82F, 0xC0D7C7,
    0xC0C48F, 0xC0C7DB, 0xC0A65F, 0xC0A8FF, 0xC0A8F7, 0xC0A8C6, 0xC474A8,
    0xC47A9E, 0xC0A6B8, 0xC0C6B6, 0xC0C83B, 0xC468A9, 0xC46C45, 0xC468B5,
    0xC0C4AF, 0xC0A673, 0xC0C682, 0xC46B0A, 0xC0CC11, 0xC47044, 0xC4978E,
    0xC46E74, 0xC20000, 0xC30100, 0xC49EC4, 0xC46B2D, 0xC0A4B2, 0xC0CCCC,
    0xC0D0D9, 0xC2EACF, 0xC1FFD3, 0xC2EA15, 0xC46ADB, 0xC46B65, 0xC0C62B,
    0xC0A8DC, 0xC46B51, 0xC4248A, 0xC423DC, 0xC4730E, 0xC0A8E7, 0xC09FA8,
    0xC0A6D1, 0xC0D0E6, 0xC0C353, 0xC0A6DA, 0xC0CD50, 0xC0A6E3, 0xEF027D,
    0xC03DAA, 0xC4ECE7, 0xC425F3, 0xC47333, 0xC47499, 0xC04EF0, 0xEF0C87,
    0xEF0C97, 0xC4258C, 0xEFE556, 0xC0C4F7, 0xC47B77, 0xC46D4B, 0xC4800B,
    0xC0AAAC, 0xEF0D46, 0xC4733C, 0xC4734C, 0xC4981F, 0xC0A838, 0xC468DC,
    0xEF0D73, 0xC46A6E, 0xC47369, 0xC4E2D7, 0xC4DDD0,
])
# fmt: on


def parse_movement(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
) -> list[str]:
    """Parse movement script bytecode."""
    filename = f"{base_name}.{extension}"
    pos = 0

    def next_byte() -> int:
        nonlocal pos, offset
        val = source[pos]
        pos += 1
        offset += 1
        return val

    def next_le16() -> int:
        return next_byte() | (next_byte() << 8)

    def next_le24() -> int:
        return next_byte() | (next_byte() << 8) | (next_byte() << 16)

    with (dir / filename).open("w") as out:
        while pos < len(source):
            cc = next_byte()

            if cc == 0x00:
                out.write("\tEBMOVE_END\n")
            elif cc == 0x01:
                arg = next_byte()
                out.write(f"\tEBMOVE_LOOP ${arg:02X}\n")
            elif cc == 0x02:
                out.write("\tEBMOVE_LOOP_END\n")
            elif cc == 0x03:
                arg = next_le24()
                out.write(f"\tEBMOVE_LONGJUMP ${arg:06X}\n")
            elif cc == 0x04:
                arg = next_le24()
                out.write(f"\tEBMOVE_LONGCALL ${arg:06X}\n")
            elif cc == 0x05:
                out.write("\tEBMOVE_LONG_RETURN\n")
            elif cc == 0x06:
                arg = next_byte()
                out.write(f"\tEBMOVE_PAUSE {arg}\n")
            elif cc == 0x07:
                arg = next_le16()
                out.write(f"\tEBMOVE_SHORTJUMP_UNKNOWN ${arg:04X}\n")
            elif cc == 0x08:
                arg = next_le24()
                out.write(f"\tEBMOVE_UNKNOWN_08 ${arg:06X}\n")
            elif cc == 0x09:
                out.write("\tEBMOVE_HALT\n")
            elif cc == 0x0A:
                arg = next_le16()
                out.write(f"\tEBMOVE_SHORTCALL_CONDITIONAL ${arg:04X}\n")
            elif cc == 0x0B:
                arg = next_le16()
                out.write(f"\tEBMOVE_SHORTCALL_CONDITIONAL_NOT ${arg:04X}\n")
            elif cc == 0x0C:
                out.write("\tEBMOVE_END_UNKNOWN\n")
            elif cc == 0x0D:
                arg1 = next_le16()
                arg2 = next_byte()
                arg3 = next_le16()
                out.write(f"\tEBMOVE_BINOP_WRAM ${arg1:04X}, ${arg2:02X}, ${arg3:04X}\n")
            elif cc == 0x0E:
                arg1 = next_byte()
                arg2 = next_le16()
                out.write(f"\tEBMOVE_WRITE_WORD_TO_9AF9_ENTRY ${arg1:02X}, ${arg2:04X}\n")
            elif cc == 0x0F:
                out.write("\tEBMOVE_UNKNOWN_08_3B_94_C0\n")
            elif cc == 0x10:
                num = next_byte()
                dests = []
                for _ in range(num):
                    dests.append(f"${next_le16():04X}")
                out.write(f"\tEBMOVE_SWITCH_JUMP_TEMPVAR {', '.join(dests)}\n")
            elif cc == 0x11:
                num = next_byte()
                dests = []
                for _ in range(num):
                    dests.append(f"${next_le16():04X}")
                out.write(f"\tEBMOVE_SWITCH_CALL_TEMPVAR {', '.join(dests)}\n")
            elif cc == 0x12:
                arg1 = next_le16()
                arg2 = next_byte()
                out.write(f"\tEBMOVE_WRITE_BYTE_WRAM ${arg1:04X}, ${arg2:02X}\n")
            elif cc == 0x13:
                out.write("\tEBMOVE_END_UNKNOWN2\n")
            elif cc == 0x14:
                arg1 = next_byte()
                arg2 = next_byte()
                arg3 = next_le16()
                out.write(f"\tEBMOVE_BINOP_9AF9 ${arg1:02X}, ${arg2:02X}, ${arg3:04X}\n")
            elif cc == 0x15:
                arg1 = next_le16()
                arg2 = next_le16()
                out.write(f"\tEBMOVE_WRITE_WORD_WRAM ${arg1:04X}, ${arg2:04X}\n")
            elif cc == 0x16:
                arg1 = next_le16()
                out.write(f"\tEBMOVE_UNKNOWN_16 ${arg1:04X}\n")
            elif cc == 0x17:
                arg1 = next_le16()
                out.write(f"\tEBMOVE_UNKNOWN_17 ${arg1:04X}\n")
            elif cc == 0x18:
                arg1 = next_le16()
                arg2 = next_byte()
                arg3 = next_byte()
                out.write(f"\tEBMOVE_BINOP_WRAM ${arg1:04X}, ${arg2:02X}, ${arg3:02X}\n")
            elif cc == 0x19:
                arg = next_le16()
                out.write(f"\tEBMOVE_SHORTJUMP ${arg:04X}\n")
            elif cc == 0x1A:
                arg = next_le16()
                out.write(f"\tEBMOVE_SHORTCALL ${arg:04X}\n")
            elif cc == 0x1B:
                out.write("\tEBMOVE_SHORT_RETURN\n")
            elif cc == 0x1C:
                arg = next_le24()
                out.write(f"\tEBMOVE_WRITE_PTR_UNKNOWN ${arg:06X}\n")
            elif cc == 0x1D:
                arg = next_le16()
                out.write(f"\tEBMOVE_WRITE_WORD_TEMPVAR ${arg:04X}\n")
            elif cc == 0x1E:
                arg = next_le16()
                out.write(f"\tEBMOVE_WRITE_WRAM_TEMPVAR ${arg:04X}\n")
            elif cc == 0x1F:
                arg = next_byte()
                out.write(f"\tEBMOVE_WRITE_TEMPVAR_9AF9 ${arg:02X}\n")
            elif cc == 0x20:
                arg = next_byte()
                out.write(f"\tEBMOVE_WRITE_9AF9_TEMPVAR ${arg:02X}\n")
            elif cc == 0x21:
                arg = next_byte()
                out.write(f"\tEBMOVE_WRITE_9AF9_WAIT_TIMER ${arg:02X}\n")
            elif cc == 0x22:
                arg = next_le16()
                out.write(f"\tEBMOVE_UNKNOWN_WRITE_11E2 ${arg:04X}\n")
            elif cc == 0x23:
                arg = next_le16()
                out.write(f"\tEBMOVE_UNKNOWN_WRITE_11A6 ${arg:04X}\n")
            elif cc == 0x24:
                out.write("\tEBMOVE_LOOP_TEMPVAR\n")
            elif cc == 0x25:
                arg = next_le16()
                out.write(f"\tEBMOVE_UNKNOWN_WRITE_121E ${arg:04X}\n")
            elif cc == 0x26:
                arg = next_byte()
                out.write(f"\tEBMOVE_WRITE_9AF9_10F2 ${arg:02X}\n")
            elif cc == 0x27:
                arg1 = next_byte()
                arg2 = next_le16()
                out.write(f"\tEBMOVE_BINOP_TEMPVAR ${arg1:02X}, ${arg2:04X}\n")
            elif cc == 0x28:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_X ${arg:04X}\n")
            elif cc == 0x29:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_Y ${arg:04X}\n")
            elif cc == 0x2A:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_Z ${arg:04X}\n")
            elif cc == 0x2B:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_X_RELATIVE ${arg:04X}\n")
            elif cc == 0x2C:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_Y_RELATIVE ${arg:04X}\n")
            elif cc == 0x2D:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_Z_RELATIVE ${arg:04X}\n")
            elif cc == 0x2E:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_X_VELOCITY_RELATIVE ${arg:04X}\n")
            elif cc == 0x2F:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_Y_VELOCITY_RELATIVE ${arg:04X}\n")
            elif cc == 0x30:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_Z_VELOCITY_RELATIVE ${arg:04X}\n")
            elif 0x31 <= cc <= 0x38:
                arg1 = next_byte()
                arg2 = next_le16()
                out.write(f"\tEBMOVE_UNKNOWN_{cc:02X} ${arg1:02X}, ${arg2:04X}\n")
            elif cc == 0x39:
                out.write("\tEBMOVE_SET_VELOCITIES_ZERO\n")
            elif cc == 0x3A:
                arg = next_byte()
                out.write(f"\tEBMOVE_UNKNOWN_3A ${arg:02X}\n")
            elif cc == 0x3B:
                arg = next_byte()
                out.write(f"\tEBMOVE_SET_10F2 ${arg:02X}\n")
            elif cc == 0x3C:
                out.write("\tEBMOVE_INC_10F2\n")
            elif cc == 0x3D:
                out.write("\tEBMOVE_DEC_10F2\n")
            elif cc == 0x3E:
                arg = next_byte()
                out.write(f"\tEBMOVE_INC_10F2_BY ${arg:02X}\n")
            elif cc == 0x3F:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_X_VELOCITY ${arg:04X}\n")
            elif cc == 0x40:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_Y_VELOCITY ${arg:04X}\n")
            elif cc == 0x41:
                arg = next_le16()
                out.write(f"\tEBMOVE_SET_Z_VELOCITY ${arg:04X}\n")
            elif cc == 0x42:
                # CALLROUTINE: variable arg count based on routine address
                routine = next_le24()
                args: list[int] = []

                if routine == VARIABLE_ARG_ROUTINE:
                    first_arg = next_byte()
                    args.append(first_arg)
                    arg_count = first_arg * 2
                elif routine in CALLROUTINE_ARGS:
                    arg_count = CALLROUTINE_ARGS[routine]
                elif routine in ZERO_ARG_ROUTINES:
                    arg_count = 0
                else:
                    print(f"UNKNOWN ROUTINE {routine:06X}, ASSUMING 0 ARGS", file=sys.stderr)
                    arg_count = 0

                for _ in range(arg_count):
                    args.append(next_byte())

                if args:
                    args_str = ", ".join(f"${a:02X}" for a in args)
                    out.write(f"\tEBMOVE_CALLROUTINE ${routine:06X}, {args_str}\n")
                else:
                    out.write(f"\tEBMOVE_CALLROUTINE ${routine:06X}\n")
            elif cc == 0x43:
                arg = next_byte()
                out.write(f"\tEBMOVE_UNKNOWN_43 ${arg:02X}\n")
            elif cc == 0x44:
                out.write("\tEBMOVE_WRITE_TEMPVAR_WAITTIMER\n")
            else:
                out.write(f"UNHANDLED: {cc:02X}\n")

    return [filename]
