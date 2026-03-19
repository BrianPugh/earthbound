"""HALLZ2 compression/decompression package."""

from ebtools.hallz.compress import compress
from ebtools.hallz.decompress import decompress, get_compressed_data

__all__ = ["compress", "decompress", "get_compressed_data"]
