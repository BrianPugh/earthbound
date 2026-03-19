"""ROM detection: header check, checksum validation, title matching."""

import struct
from dataclasses import dataclass


@dataclass
class DetectResult:
    header: bool
    matched: bool


def detect(data: bytes, identifier: str) -> DetectResult:
    """Check SNES header at two possible offsets (headerless and headered).

    Returns whether the ROM matched and whether it has a 512-byte SMC header.
    """
    # Try headerless (base=0xFFB0) then headered (base=0x101B0)
    for headered, base in [(False, 0xFFB0), (True, 0x101B0)]:
        if base + 48 > len(data):
            continue
        checksum_complement = struct.unpack_from("<H", data, base + 44)[0]
        checksum = struct.unpack_from("<H", data, base + 46)[0]
        if (checksum ^ checksum_complement) == 0xFFFF:
            title = data[base + 16 : base + 37].decode("ascii", errors="replace")
            if title == identifier:
                return DetectResult(header=headered, matched=True)
    return DetectResult(header=False, matched=False)
