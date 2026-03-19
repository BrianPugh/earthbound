"""PSI arrangement frame bundling.

Takes .arr.lzhal files (HALLZ2-compressed tile arrangements), splits them
into 8-frame bundles, compresses each bundle independently, and writes a
.arr.bundled format for streaming decompression in the C port.

Bundled format:
    uint8_t  frames_per_bundle      // always 8
    uint8_t  total_frames
    uint16_t bundle_count           // ceil(total_frames / frames_per_bundle)
    uint16_t offsets[bundle_count+1] // byte offsets from start of data section
    uint8_t  data[]                 // concatenated HALLZ2-compressed bundles
All multi-byte values are little-endian.
"""

import math
import struct
from pathlib import Path

from ebtools.hallz import compress, decompress

FRAME_SIZE = 0x400  # 1024 bytes per arrangement frame
FRAMES_PER_BUNDLE = 8
NUM_ARRANGEMENTS = 34  # 0-33


def pack_bundled_arrangements(input_dir: Path, output_dir: Path) -> int:
    """Convert .arr.lzhal files to .arr.bundled format.

    Parameters
    ----------
    input_dir
        Directory containing N.arr.lzhal files (e.g. asm/bin/psianims/arrangements/).
    output_dir
        Directory to write N.arr.bundled files.

    Returns
    -------
    int
        Number of files processed.
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    count = 0

    for i in range(NUM_ARRANGEMENTS):
        lzhal_path = input_dir / f"{i}.arr.lzhal"
        if not lzhal_path.exists():
            continue

        compressed_data = lzhal_path.read_bytes()
        raw = decompress(compressed_data)

        total_frames = len(raw) // FRAME_SIZE
        if len(raw) % FRAME_SIZE != 0:
            raise ValueError(f"Arrangement {i}: decompressed size {len(raw)} is not a multiple of {FRAME_SIZE}")

        bundle_count = math.ceil(total_frames / FRAMES_PER_BUNDLE)

        # Compress each bundle
        compressed_bundles: list[bytes] = []
        for b in range(bundle_count):
            start_frame = b * FRAMES_PER_BUNDLE
            end_frame = min(start_frame + FRAMES_PER_BUNDLE, total_frames)
            bundle_raw = raw[start_frame * FRAME_SIZE : end_frame * FRAME_SIZE]
            compressed_bundles.append(compress(bundle_raw))

        # Build offset table (offsets from start of data section)
        offsets: list[int] = []
        pos = 0
        for cb in compressed_bundles:
            offsets.append(pos)
            pos += len(cb)
        offsets.append(pos)  # sentinel: total data size

        # Write header + data
        header = struct.pack("<BBH", FRAMES_PER_BUNDLE, total_frames, bundle_count)
        offset_data = struct.pack(f"<{len(offsets)}H", *offsets)
        blob = b"".join(compressed_bundles)

        out_path = output_dir / f"{i}.arr.bundled"
        out_path.write_bytes(header + offset_data + blob)
        count += 1

    return count
