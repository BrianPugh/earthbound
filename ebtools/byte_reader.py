"""ByteReader: wraps a bytes buffer with position tracking and LE read methods."""

import struct


class ByteReader:
    """Read little-endian values from a byte buffer with automatic position tracking."""

    __slots__ = ("data", "pos")

    def __init__(self, data: bytes | bytearray | memoryview, pos: int = 0):
        self.data = data
        self.pos = pos

    def __len__(self) -> int:
        return len(self.data) - self.pos

    def __bool__(self) -> bool:
        return self.pos < len(self.data)

    def read_byte(self) -> int:
        val = self.data[self.pos]
        self.pos += 1
        return val

    def read_le16(self) -> int:
        (val,) = struct.unpack_from("<H", self.data, self.pos)
        self.pos += 2
        return val

    def read_le24(self) -> int:
        b0, b1, b2 = self.data[self.pos], self.data[self.pos + 1], self.data[self.pos + 2]
        self.pos += 3
        return b0 | (b1 << 8) | (b2 << 16)

    def read_le32(self) -> int:
        (val,) = struct.unpack_from("<I", self.data, self.pos)
        self.pos += 4
        return val

    def read_bytes(self, n: int) -> bytes:
        result = bytes(self.data[self.pos : self.pos + n])
        self.pos += n
        return result

    def peek_byte(self) -> int:
        return self.data[self.pos]
