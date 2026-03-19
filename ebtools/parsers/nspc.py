"""NSPC music pack assembly.

Ported from app.d:parseNSPC.
"""

import struct
from pathlib import Path

from ebtools.config import CommonData, DumpDoc


def parse_nspc(
    dir: Path,
    base_name: str,
    extension: str,
    source: bytes,
    offset: int,
    doc: DumpDoc,
    common_data: CommonData,
    full_rom: bytes,
) -> list[str]:
    """Parse NSPC music data and create per-song .nspc files.

    The source data is a table of 3-byte entries (bgmPacks): for each song,
    three pack indices. The pack pointer table and song pointer table are
    at ROM offsets specified in doc.music.
    """
    files: list[str] = []

    pack_ptr_table_offset = doc.music.packPointerTable
    num_packs = doc.music.numPacks
    song_ptr_table_offset = doc.music.songPointerTable

    # Read pack pointer table: each entry is 3 bytes (1 byte bank + 2 byte LE addr)
    pack_pointers: list[int] = []
    for i in range(num_packs):
        base = pack_ptr_table_offset + i * 3
        bank = full_rom[base]
        addr = full_rom[base + 1] | (full_rom[base + 2] << 8)
        pack_pointers.append(addr + (bank << 16))

    # BGM packs: source is array of 3-byte entries
    num_songs = len(source) // 3

    # Read song pointer table: ushort per song
    song_pointers: list[int] = []
    for i in range(num_songs):
        base = song_ptr_table_offset + i * 2
        song_pointers.append(full_rom[base] | (full_rom[base + 1] << 8))

    for idx in range(num_songs):
        song_packs = [source[idx * 3 + j] for j in range(3)]
        filename = f"{idx + 1:03d}.nspc"
        files.append(filename)

        with (dir / filename).open("wb") as f:

            def write_pack(pack: int) -> None:
                rom_offset = pack_pointers[pack] - 0xC00000
                while True:
                    size = full_rom[rom_offset] | (full_rom[rom_offset + 1] << 8)
                    if size == 0:
                        break
                    # Write the pack block: size(2) + spc_offset(2) + data(size)
                    f.write(full_rom[rom_offset : rom_offset + size + 4])
                    rom_offset += size + 4

            # Write NSPC file header (32 bytes)
            # variant(4) + songBase(2) + instrumentBase(2) + sampleBase(2) + reserved(22)
            header = struct.pack(
                "<IHH",
                0,  # variant
                song_pointers[idx],  # songBase
                0x6E00,  # instrumentBase
            )
            header += struct.pack("<H", 0x6C00)  # sampleBase
            header += b"\x00" * 22  # reserved
            f.write(header)

            # If third pack is 0xFF, write pack 1 first (always-loaded pack)
            if song_packs[2] == 0xFF:
                write_pack(1)

            # Write each non-0xFF pack
            for pack in song_packs:
                if pack == 0xFF:
                    continue
                write_pack(pack)

            # Write terminator (ushort 0)
            f.write(b"\x00\x00")

    return files
