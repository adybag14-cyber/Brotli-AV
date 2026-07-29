"""Brotli-AV research compressor (BAV format)."""

from .codec import compress, decompress, FORMAT_VERSION, MAGIC

__all__ = ["compress", "decompress", "FORMAT_VERSION", "MAGIC"]
__version__ = "0.1.0"
