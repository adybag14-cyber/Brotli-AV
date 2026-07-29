"""
BAV1 lossless research codec.

Design goals (see README / research notes):
  - Strictly lossless round-trip.
  - Beat stock Google Brotli (quality 11) on the fixed project corpus by total
    compressed bytes via adaptive multi-backend selection and light transforms.
  - Wire format is BAV1 (not RFC 7932); ratio win is the research target.

Methods tried in auto mode (smallest payload wins, header overhead included):
  STORE, DEFLATE-9, LZMA-extreme, ZSTD-22, and a research path that applies
  optional structured-record transpose then re-selects among the backends.
"""

from __future__ import annotations

import lzma
import struct
import zlib
from typing import Callable

try:
    import zstandard as zstd
except ImportError as e:  # pragma: no cover
    raise ImportError("zstandard is required: pip install -r requirements.txt") from e

MAGIC = b"BAV1"
FORMAT_VERSION = 1

# Method IDs stored in the stream
M_STORE = 0
M_DEFLATE = 1
M_LZMA = 2
M_ZSTD = 3
# Research: 1-byte record width N in payload[0], then backend method in payload[1],
# then compressed transposed bytes. Only used when transpose helps.
M_TRANSPOSE = 4

_HEADER = struct.Struct("<4sBBIQI")  # magic, ver, method, flags, orig_size, crc32
# flags bit0: reserved


def _crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _deflate(data: bytes) -> bytes:
    return zlib.compress(data, level=9)


def _inflate(data: bytes) -> bytes:
    return zlib.decompress(data)


def _lzma_enc(data: bytes) -> bytes:
    # Extreme preset: strong ratio on binary/source-heavy inputs
    return lzma.compress(data, preset=9 | lzma.PRESET_EXTREME)


def _lzma_dec(data: bytes) -> bytes:
    return lzma.decompress(data)


def _zstd_enc(data: bytes) -> bytes:
    cctx = zstd.ZstdCompressor(level=22)
    return cctx.compress(data)


def _zstd_dec(data: bytes) -> bytes:
    dctx = zstd.ZstdDecompressor()
    return dctx.decompress(data)


def _store_enc(data: bytes) -> bytes:
    return data


def _store_dec(data: bytes) -> bytes:
    return data


_BACKENDS: dict[int, tuple[Callable[[bytes], bytes], Callable[[bytes], bytes]]] = {
    M_STORE: (_store_enc, _store_dec),
    M_DEFLATE: (_deflate, _inflate),
    M_LZMA: (_lzma_enc, _lzma_dec),
    M_ZSTD: (_zstd_enc, _zstd_dec),
}


def _transpose(data: bytes, width: int) -> bytes:
    """Column-major reorder of fixed-width records (helps LZ on structured binary)."""
    if width <= 1 or len(data) < width * 2:
        return data
    n = len(data) - (len(data) % width)
    if n == 0:
        return data
    body, tail = data[:n], data[n:]
    rows = n // width
    out = bytearray(n)
    for col in range(width):
        base = col * rows
        for row in range(rows):
            out[base + row] = body[row * width + col]
    return bytes(out) + tail


def _untranspose(data: bytes, width: int) -> bytes:
    if width <= 1 or len(data) < width * 2:
        return data
    n = len(data) - (len(data) % width)
    if n == 0:
        return data
    body, tail = data[:n], data[n:]
    rows = n // width
    out = bytearray(n)
    for col in range(width):
        base = col * rows
        for row in range(rows):
            out[row * width + col] = body[base + row]
    return bytes(out) + tail


def _try_backends(data: bytes) -> list[tuple[int, bytes]]:
    """Return list of (method_id, payload) candidates."""
    out: list[tuple[int, bytes]] = []
    for mid, (enc, _) in _BACKENDS.items():
        try:
            payload = enc(data)
        except Exception:
            continue
        out.append((mid, payload))
    return out


def _best_backend(data: bytes) -> tuple[int, bytes]:
    candidates = _try_backends(data)
    if not candidates:
        return M_STORE, data
    # Prefer smallest payload; tie-break lower method id for stability
    candidates.sort(key=lambda x: (len(x[1]), x[0]))
    return candidates[0]


def _research_transpose_candidates(data: bytes) -> list[tuple[int, bytes]]:
    """Research path: fixed-width transpose + best backend for a few widths."""
    results: list[tuple[int, bytes]] = []
    # Common record sizes in binary / archive-like data
    for width in (2, 4, 8, 12, 16):
        if len(data) < width * 4:
            continue
        transformed = _transpose(data, width)
        mid, payload = _best_backend(transformed)
        # payload layout: width u8, backend u8, compressed...
        wrapped = bytes([width & 0xFF, mid & 0xFF]) + payload
        results.append((M_TRANSPOSE, wrapped))
    return results


def compress(data: bytes, *, method: str = "auto") -> bytes:
    """
    Compress *data* into a BAV1 frame.

    method:
      "auto"     — try all backends + research transpose; pick smallest frame
      "store"    — no compression
      "deflate"  — zlib level 9
      "lzma"     — LZMA2 extreme
      "zstd"     — zstd level 22
      "research" — transpose candidates only + auto backends (same as auto)
    """
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError("data must be bytes-like")
    data = bytes(data)
    crc = _crc32(data)
    orig = len(data)

    method = (method or "auto").lower()
    forced = {
        "store": M_STORE,
        "deflate": M_DEFLATE,
        "lzma": M_LZMA,
        "zstd": M_ZSTD,
    }

    candidates: list[tuple[int, bytes]] = []
    if method in forced:
        mid = forced[method]
        enc, _ = _BACKENDS[mid]
        candidates.append((mid, enc(data)))
    else:
        # auto / research
        candidates.extend(_try_backends(data))
        candidates.extend(_research_transpose_candidates(data))

    if not candidates:
        candidates.append((M_STORE, data))

    # Choose smallest full frame (header is fixed size so payload size ranks)
    candidates.sort(key=lambda x: (len(x[1]), x[0]))
    mid, payload = candidates[0]

    header = _HEADER.pack(MAGIC, FORMAT_VERSION, mid, 0, orig, crc)
    return header + payload


def decompress(frame: bytes) -> bytes:
    """Decompress a BAV1 frame; verify CRC32 and original size."""
    if not isinstance(frame, (bytes, bytearray, memoryview)):
        raise TypeError("frame must be bytes-like")
    frame = bytes(frame)
    if len(frame) < _HEADER.size:
        raise ValueError("BAV frame too short")

    magic, ver, mid, _flags, orig, crc = _HEADER.unpack_from(frame, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic: {magic!r}")
    if ver != FORMAT_VERSION:
        raise ValueError(f"unsupported BAV version: {ver}")

    payload = frame[_HEADER.size :]

    if mid == M_TRANSPOSE:
        if len(payload) < 2:
            raise ValueError("transpose payload too short")
        width = payload[0]
        backend = payload[1]
        if backend not in _BACKENDS:
            raise ValueError(f"unknown transpose backend {backend}")
        _, dec = _BACKENDS[backend]
        transformed = dec(payload[2:])
        data = _untranspose(transformed, width)
    elif mid in _BACKENDS:
        _, dec = _BACKENDS[mid]
        data = dec(payload)
    else:
        raise ValueError(f"unknown method id {mid}")

    if len(data) != orig:
        raise ValueError(f"size mismatch: got {len(data)}, expected {orig}")
    if _crc32(data) != crc:
        raise ValueError("CRC32 mismatch (corrupt or non-lossless frame)")
    return data


def compress_file(in_path: str, out_path: str, *, method: str = "auto") -> int:
    with open(in_path, "rb") as f:
        data = f.read()
    frame = compress(data, method=method)
    with open(out_path, "wb") as f:
        f.write(frame)
    return len(frame)


def decompress_file(in_path: str, out_path: str) -> int:
    with open(in_path, "rb") as f:
        frame = f.read()
    data = decompress(frame)
    with open(out_path, "wb") as f:
        f.write(data)
    return len(data)
