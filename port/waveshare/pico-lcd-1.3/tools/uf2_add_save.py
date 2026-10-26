r"""Bundle an 8 KB ``.srm`` save into a firmware UF2, so one drag-and-drop flashes both.

The RP2040 save region (``drivers/platform/rp2040/save.c``) is the last 2 flash
sectors and holds the raw battery-SRAM image, the same bytes as the desktop
port's ``earthbound.srm`` and the playtest saves in ``playtest/saves/``.

The RP2040 bootrom erases a flash sector only for the first block it receives in
each group of 16 consecutive block *numbers* (``block_no * 256 / 4096``), whatever
the blocks' addresses. Appending the save blocks after the firmware would put
them in a group the firmware's last sector already used, so the save sectors are
programmed without being erased and the save comes out corrupt. The save
therefore goes first, as blocks 0-31 (one group per sector), and the firmware is
renumbered by +32, a multiple of 16, which keeps its own groups sector-aligned.

Usage::

    python port/waveshare/pico-lcd-1.3/tools/uf2_add_save.py build-default/earthbound_pico.uf2 \
        playtest/saves/01-Start.srm earthbound_pico_save.uf2
"""

import argparse
import struct
from pathlib import Path

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_BLOCK_SIZE = 512
UF2_PAYLOAD_SIZE = 256

XIP_BASE = 0x10000000
FLASH_SECTOR_SIZE = 4096
SAVE_SECTOR_COUNT = 2
SAVE_SIZE = SAVE_SECTOR_COUNT * FLASH_SECTOR_SIZE
BLOCKS_PER_SECTOR = FLASH_SECTOR_SIZE // UF2_PAYLOAD_SIZE


def make_save_blocks(save: bytes, save_addr: int, flags: int, family: int) -> list[bytearray]:
    """Split the save image into UF2 blocks targeting ``save_addr`` (numbered later)."""
    blocks = []
    for off in range(0, len(save), UF2_PAYLOAD_SIZE):
        block = bytearray(UF2_BLOCK_SIZE)
        struct.pack_into(
            "<8I", block, 0, UF2_MAGIC_START0, UF2_MAGIC_START1, flags, save_addr + off, UF2_PAYLOAD_SIZE, 0, 0, family
        )
        block[32 : 32 + UF2_PAYLOAD_SIZE] = save[off : off + UF2_PAYLOAD_SIZE]
        struct.pack_into("<I", block, UF2_BLOCK_SIZE - 4, UF2_MAGIC_END)
        blocks.append(block)
    return blocks


def add_save(firmware: bytes, save: bytes, flash_size: int) -> bytes:
    """Return a UF2 with ``save`` written to the save region ahead of ``firmware``."""
    if len(save) != SAVE_SIZE:
        raise ValueError(f"save is {len(save)} bytes, expected {SAVE_SIZE}")
    if len(firmware) % UF2_BLOCK_SIZE:
        raise ValueError("firmware UF2 is not a whole number of 512-byte blocks")

    fw_blocks = [bytearray(firmware[i : i + UF2_BLOCK_SIZE]) for i in range(0, len(firmware), UF2_BLOCK_SIZE)]
    for n, block in enumerate(fw_blocks):
        addr = struct.unpack_from("<I", block, 12)[0]
        if (addr - XIP_BASE) // FLASH_SECTOR_SIZE != n // BLOCKS_PER_SECTOR:
            raise ValueError(f"firmware block {n} at {addr:#x} breaks the bootrom's sector grouping")
    flags, family = struct.unpack_from("<I", fw_blocks[0], 8)[0], struct.unpack_from("<I", fw_blocks[0], 28)[0]

    save_addr = XIP_BASE + flash_size - SAVE_SIZE
    blocks = make_save_blocks(save, save_addr, flags, family) + fw_blocks
    for n, block in enumerate(blocks):
        struct.pack_into("<II", block, 20, n, len(blocks))
    return b"".join(blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("firmware", type=Path, help="firmware UF2 (e.g. build-default/earthbound_pico.uf2)")
    parser.add_argument("save", type=Path, help="8 KB .srm save image")
    parser.add_argument("output", type=Path, help="UF2 to write")
    parser.add_argument("--flash-size", type=int, default=16 * 1024 * 1024, help="flash size in bytes (default 16 MB)")
    args = parser.parse_args()

    out = add_save(args.firmware.read_bytes(), args.save.read_bytes(), args.flash_size)
    args.output.write_bytes(out)
    save_addr = XIP_BASE + args.flash_size - SAVE_SIZE
    print(f"{args.output}: {len(out) // UF2_BLOCK_SIZE} blocks, save at {save_addr:#x}")


if __name__ == "__main__":
    main()
