"""HALLZ2 decompression and stream scanning."""

# Precomputed bit-reverse table
_BIT_REVERSE = bytes(sum(((b >> i) & 1) << (7 - i) for i in range(8)) for b in range(256))


def _reverse_bits(b: int) -> int:
    """Reverse the bits of a byte."""
    return _BIT_REVERSE[b]


def get_compressed_data(data: bytes | bytearray | memoryview) -> bytes:
    """Walk the HALLZ2 command stream to find the end without decompressing.

    Returns the slice of data consumed by one compressed block.
    Ported from common.d:getCompressedData.
    """
    pos = 0
    while True:
        command_byte = data[pos]
        pos += 1
        command_id = command_byte >> 5

        if command_id == 7:
            # Extended format
            command_id = (command_byte & 0x1C) >> 2
            if command_id != 7:
                # Has a length byte
                command_length = ((command_byte & 3) << 8) + data[pos] + 1
                pos += 1
            else:
                # Terminator (double-extended = command ID 7)
                break
        else:
            command_length = (command_byte & 0x1F) + 1

        # Commands 4-6 read a 2-byte buffer position
        if 4 <= command_id < 7:
            pos += 2

        if command_id == 0:
            # Uncompressed literal
            pos += command_length
        elif command_id == 1:
            # Fill byte
            pos += 1
        elif command_id == 2:
            # Fill word
            pos += 2
        elif command_id == 3:
            # Incrementing fill
            pos += 1
        elif command_id in (4, 5, 6):
            # Copy from buffer (back-reference already consumed)
            pass

    return bytes(data[:pos])


def decompress(data: bytes | bytearray | memoryview) -> bytes:
    """Full HALLZ2 decompression.

    Ported from src/core/decomp.c.

    Command byte format:
      Bits 7-5 = command ID (0-6)
      Bits 4-0 = length - 1 (range 1-32)

    Extended format (when bits 7-5 == 0b111):
      Byte 0: 0xE0 | (cmd_id << 2) | len_hi
      Byte 1: len_lo
      Length = (len_hi << 8) | len_lo + 1 (range 1-1024)
      When cmd_id == 7 (byte == 0xFF): terminator

    Commands:
      0 (0x00): Literal copy - N bytes verbatim
      1 (0x20): Byte fill - fill N bytes with 1 byte
      2 (0x40): Word fill - fill N words with 1 word (2 bytes per word)
      3 (0x60): Incrementing fill - fill N bytes starting from value, +1 each
      4 (0x80): Forward copy from output buffer position (big-endian offset)
      5 (0xA0): Bit-reversed copy from output buffer position
      6 (0xC0): Backward copy from output buffer position (reads backward)
    """
    src = data
    si = 0
    out = bytearray()

    while si < len(src):
        cmd = src[si]

        # Terminator
        if cmd == 0xFF:
            break

        # Determine operation and length
        if (cmd & 0xE0) == 0xE0:
            # Extended format
            op = (cmd << 3) & 0xE0
            len_hi = cmd & 0x03
            si += 1
            if si >= len(src):
                break
            length = (len_hi << 8) | src[si]
            length += 1
            si += 1
        else:
            op = cmd & 0xE0
            length = (cmd & 0x1F) + 1
            si += 1

        if op == 0x00:
            # Copy N bytes verbatim
            for _ in range(length):
                if si >= len(src):
                    break
                out.append(src[si])
                si += 1

        elif op == 0x20:
            # Fill N bytes with single byte
            if si >= len(src):
                break
            fill = src[si]
            si += 1
            out.extend(bytes([fill]) * length)

        elif op == 0x40:
            # Fill N words with single word
            if si + 1 >= len(src):
                break
            lo = src[si]
            hi = src[si + 1]
            si += 2
            for _ in range(length):
                out.append(lo)
                out.append(hi)

        elif op == 0x60:
            # Fill N bytes with incrementing byte
            if si >= len(src):
                break
            val = src[si]
            si += 1
            for _ in range(length):
                out.append(val & 0xFF)
                val += 1

        elif op == 0x80:
            # Copy N bytes from previously decompressed data (forward)
            if si + 1 >= len(src):
                break
            offset = (src[si] << 8) | src[si + 1]
            si += 2
            ref = offset
            for _ in range(length):
                if ref < len(out):
                    out.append(out[ref])
                else:
                    out.append(0)
                ref += 1

        elif op == 0xA0:
            # Copy N bytes with bit-reversal
            if si + 1 >= len(src):
                break
            offset = (src[si] << 8) | src[si + 1]
            si += 2
            ref = offset
            for _ in range(length):
                b = out[ref] if ref < len(out) else 0
                out.append(_reverse_bits(b))
                ref += 1

        elif op == 0xC0:
            # Copy N bytes backwards
            if si + 1 >= len(src):
                break
            offset = (src[si] << 8) | src[si + 1]
            si += 2
            ref = offset
            for _ in range(length):
                b = out[ref] if ref < len(out) else 0
                out.append(b)
                if ref > 0:
                    ref -= 1

        else:
            # Unknown op: treat as forward copy (matches assembly fallthrough)
            if si + 1 >= len(src):
                break
            offset = (src[si] << 8) | src[si + 1]
            si += 2
            ref = offset
            for _ in range(length):
                if ref < len(out):
                    out.append(out[ref])
                else:
                    out.append(0)
                ref += 1

    return bytes(out)
