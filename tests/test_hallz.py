"""Tests for HALLZ2 compression/decompression."""

import random
from pathlib import Path

import pytest

from ebtools.hallz import compress, decompress, get_compressed_data

# --- Roundtrip tests (compress then decompress) ---


def test_empty():
    compressed = compress(b"")
    assert compressed == b"\xff"
    assert decompress(compressed) == b""


def test_single_byte():
    data = b"\x42"
    assert decompress(compress(data)) == data


def test_few_bytes():
    data = b"\x01\x02\x03"
    assert decompress(compress(data)) == data


def test_byte_fill_short():
    data = bytes([0xAA] * 10)
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) < len(data)


def test_byte_fill_long():
    data = bytes([0x55] * 500)
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) < len(data)


def test_byte_fill_max_extended():
    data = bytes([0x00] * 1024)
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) <= 4  # header(2) + fill_byte(1) + terminator(1)


def test_byte_fill_over_max():
    data = bytes([0x77] * 2000)
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) < 20


def test_word_fill():
    data = bytes([0xAB, 0xCD] * 50)
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) < len(data)


def test_word_fill_long():
    data = bytes([0x12, 0x34] * 500)
    compressed = compress(data)
    assert decompress(compressed) == data


def test_inc_fill():
    data = bytes(range(100))
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) < len(data)


def test_inc_fill_wrapping():
    data = bytes((i & 0xFF) for i in range(250, 250 + 20))
    compressed = compress(data)
    assert decompress(compressed) == data


def test_inc_fill_long():
    data = bytes((i & 0xFF) for i in range(300))
    compressed = compress(data)
    assert decompress(compressed) == data


def test_repeating_pattern():
    """Repeating pattern should be caught by forward copy."""
    data = bytes([1, 2, 3, 4, 5]) * 40
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) < len(data)


def test_forward_copy():
    """Data with a repeated block should use forward copy."""
    block = bytes(range(50))
    data = block + bytes([0xFF] * 10) + block
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) < len(data)


def test_all_zeros():
    data = bytes(4096)
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) < 20


def test_random_data():
    random.seed(42)
    data = bytes(random.randint(0, 255) for _ in range(1000))  # noqa: S311
    compressed = compress(data)
    assert decompress(compressed) == data


def test_random_data_large():
    random.seed(123)
    data = bytes(random.randint(0, 255) for _ in range(8000))  # noqa: S311
    compressed = compress(data)
    assert decompress(compressed) == data


def test_mixed_patterns():
    """Mix of fill types and literals."""
    data = (
        bytes([0xAA] * 30)  # byte fill
        + bytes(range(20))  # inc fill
        + bytes([0x12, 0x34] * 15)  # word fill
        + bytes([0x99])  # literal
        + bytes([0xBB] * 50)  # byte fill
    )
    compressed = compress(data)
    assert decompress(compressed) == data
    assert len(compressed) < len(data)


def test_short_runs_stay_literal():
    """Runs too short for compression should be emitted as literals."""
    data = bytes([1, 2, 1, 2, 3, 4, 3, 4, 5, 6, 5, 6])
    compressed = compress(data)
    assert decompress(compressed) == data


def test_literal_over_1024():
    """Literal runs > 1024 must be split into multiple commands."""
    random.seed(99)
    # Create data that can't be compressed (high entropy)
    data = bytes(random.randint(0, 255) for _ in range(2000))  # noqa: S311
    compressed = compress(data)
    assert decompress(compressed) == data


# --- get_compressed_data tests ---


def test_get_compressed_data_simple():
    compressed = compress(b"hello world")
    extracted = get_compressed_data(compressed)
    assert extracted == compressed


def test_get_compressed_data_with_trailing():
    compressed = compress(b"test")
    padded = compressed + b"\x00\x00\x00"
    extracted = get_compressed_data(padded)
    assert extracted == compressed


# --- Tests against original .lzhal assets ---


def _collect_lzhal_files():
    base = Path("asm/bin")
    if not base.exists():
        return []
    return sorted(base.rglob("*.lzhal"))


_LZHAL_FILES = _collect_lzhal_files()


@pytest.mark.skipif(not _LZHAL_FILES, reason="No extracted .lzhal assets found")
@pytest.mark.parametrize(
    "lzhal_path",
    _LZHAL_FILES,
    ids=[str(p.relative_to("asm/bin")) for p in _LZHAL_FILES],
)
def test_original_roundtrip(lzhal_path):
    """Decompress original, recompress, verify decompression matches."""
    original_compressed = lzhal_path.read_bytes()
    decompressed = decompress(original_compressed)
    recompressed = compress(decompressed)
    re_decompressed = decompress(recompressed)
    assert re_decompressed == decompressed, f"Roundtrip mismatch for {lzhal_path}"


@pytest.mark.skipif(not _LZHAL_FILES, reason="No extracted .lzhal assets found")
def test_compression_ratio():
    """Recompressed data should be close to original size (within 10%)."""
    total_original = 0
    total_recompressed = 0
    worse_files = []

    for f in _LZHAL_FILES:
        original = f.read_bytes()
        decompressed = decompress(original)
        recompressed = compress(decompressed)

        total_original += len(original)
        total_recompressed += len(recompressed)

        if len(recompressed) > len(original) * 1.1:
            worse_files.append((str(f.relative_to("asm/bin")), len(original), len(recompressed)))

    ratio = total_recompressed / total_original if total_original else 1.0
    print(f"\nCompression ratio: {ratio:.4f} (recompressed/original)")
    print(f"Total original: {total_original} bytes")
    print(f"Total recompressed: {total_recompressed} bytes")
    if worse_files:
        print(f"\nFiles >10% worse ({len(worse_files)}):")
        for name, orig, recomp in worse_files[:10]:
            print(f"  {name}: {orig} -> {recomp} ({recomp / orig:.2f}x)")

    assert ratio < 1.15, f"Overall compression ratio {ratio:.4f} is too far from original"


@pytest.mark.skipif(not _LZHAL_FILES, reason="No extracted .lzhal assets found")
def test_byte_exact_matches():
    """Count how many files produce byte-exact compressed output."""
    exact = 0
    total = 0
    for f in _LZHAL_FILES:
        original = f.read_bytes()
        decompressed = decompress(original)
        recompressed = compress(decompressed)
        total += 1
        if recompressed == original:
            exact += 1

    pct = exact / total * 100 if total else 0
    print(f"\nByte-exact matches: {exact}/{total} ({pct:.1f}%)")
