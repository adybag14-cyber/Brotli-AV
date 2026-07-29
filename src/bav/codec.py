"""
BAV1 lossless research codec.

Design goals (see README / research notes):
  - Strictly lossless round-trip.
  - Beat pure Zstd level 22 on every fixed-corpus file and on the total
    compressed byte count (primary gate). Secondary: still beat Brotli q=11 total.
  - Wire format is BAV1 (not Zstd/Brotli wire-compatible); ratio is the target.

Methods tried in auto mode (smallest payload wins, header overhead included):
  STORE, DEFLATE-9, LZMA-extreme, ZSTD-22, BROTLI-11, research record-transpose,
  and research prefilters (MTF, RLE0) re-selected among backends.
"""

from __future__ import annotations

import lzma
import struct
import zlib
from typing import Callable

try:
    import brotli
except ImportError as e:  # pragma: no cover
    raise ImportError("brotli is required: pip install -r requirements.txt") from e

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
# Research: width u8, backend u8, then compressed transposed bytes
M_TRANSPOSE = 4
M_BROTLI = 5
# Research prefilter: filter_id u8, backend u8, then compressed filtered bytes
M_PREFILTER = 6

# Prefilter IDs (payload[0] when method == M_PREFILTER)
F_MTF = 1
F_RLE0 = 2
F_MTF_RLE0 = 3

_HEADER = struct.Struct("<4sBBIQI")  # magic, ver, method, flags, orig_size, crc32


def _crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _deflate(data: bytes) -> bytes:
    return zlib.compress(data, level=9)


def _inflate(data: bytes) -> bytes:
    return zlib.decompress(data)


def _lzma_enc(data: bytes) -> bytes:
    return lzma.compress(data, preset=9 | lzma.PRESET_EXTREME)


def _lzma_dec(data: bytes) -> bytes:
    return lzma.decompress(data)


def _zstd_enc(data: bytes) -> bytes:
    cctx = zstd.ZstdCompressor(level=22)
    return cctx.compress(data)


def _zstd_dec(data: bytes) -> bytes:
    dctx = zstd.ZstdDecompressor()
    return dctx.decompress(data)


def _brotli_enc(data: bytes) -> bytes:
    return brotli.compress(data, quality=11)


def _brotli_dec(data: bytes) -> bytes:
    return brotli.decompress(data)


def _store_enc(data: bytes) -> bytes:
    return data


def _store_dec(data: bytes) -> bytes:
    return data


_BACKENDS: dict[int, tuple[Callable[[bytes], bytes], Callable[[bytes], bytes]]] = {
    M_STORE: (_store_enc, _store_dec),
    M_DEFLATE: (_deflate, _inflate),
    M_LZMA: (_lzma_enc, _lzma_dec),
    M_ZSTD: (_zstd_enc, _zstd_dec),
    M_BROTLI: (_brotli_enc, _brotli_dec),
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


def _mtf_encode(data: bytes) -> bytes:
    """Move-to-front transform (lossless)."""
    table = list(range(256))
    out = bytearray(len(data))
    for i, b in enumerate(data):
        idx = table.index(b)
        out[i] = idx
        # move to front
        del table[idx]
        table.insert(0, b)
    return bytes(out)


def _mtf_decode(data: bytes) -> bytes:
    table = list(range(256))
    out = bytearray(len(data))
    for i, idx in enumerate(data):
        b = table[idx]
        out[i] = b
        del table[idx]
        table.insert(0, b)
    return bytes(out)


def _rle0_encode(data: bytes) -> bytes:
    """
    Zero-run length encoding (bzip2-style lite): non-zero bytes pass through;
    runs of zeros become a count in a simple escape form.
    Format: for each run of zeros of length n>=1, emit 0 then min(n,255) as u8,
    repeating for long runs. Non-zero byte b emits b as-is.
    """
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        if data[i] == 0:
            j = i
            while j < n and data[j] == 0 and (j - i) < 255:
                j += 1
            out.append(0)
            out.append(j - i)
            i = j
        else:
            out.append(data[i])
            i += 1
    return bytes(out)


def _rle0_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        if b == 0:
            if i + 1 >= n:
                raise ValueError("truncated RLE0")
            count = data[i + 1]
            out.extend(b"\x00" * count)
            i += 2
        else:
            out.append(b)
            i += 1
    return bytes(out)


def _apply_filter(data: bytes, fid: int) -> bytes:
    if fid == F_MTF:
        return _mtf_encode(data)
    if fid == F_RLE0:
        return _rle0_encode(data)
    if fid == F_MTF_RLE0:
        return _rle0_encode(_mtf_encode(data))
    raise ValueError(f"unknown filter {fid}")


def _undo_filter(data: bytes, fid: int) -> bytes:
    if fid == F_MTF:
        return _mtf_decode(data)
    if fid == F_RLE0:
        return _rle0_decode(data)
    if fid == F_MTF_RLE0:
        return _mtf_decode(_rle0_decode(data))
    raise ValueError(f"unknown filter {fid}")


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
    candidates.sort(key=lambda x: (len(x[1]), x[0]))
    return candidates[0]


def _research_transpose_candidates(data: bytes) -> list[tuple[int, bytes]]:
    """Research path: fixed-width transpose + best backend for a few widths."""
    results: list[tuple[int, bytes]] = []
    for width in (2, 3, 4, 6, 8, 12, 16):
        if len(data) < width * 4:
            continue
        transformed = _transpose(data, width)
        mid, payload = _best_backend(transformed)
        wrapped = bytes([width & 0xFF, mid & 0xFF]) + payload
        results.append((M_TRANSPOSE, wrapped))
    return results


def _research_prefilter_candidates(data: bytes) -> list[tuple[int, bytes]]:
    """Research path: MTF / RLE0 / both then best backend."""
    results: list[tuple[int, bytes]] = []
    if len(data) < 32:
        return results
    for fid in (F_MTF, F_RLE0, F_MTF_RLE0):
        try:
            filtered = _apply_filter(data, fid)
        except Exception:
            continue
        # Skip if filter expanded too much (unlikely to help after entropy coding)
        if len(filtered) > len(data) * 2 + 64:
            continue
        mid, payload = _best_backend(filtered)
        wrapped = bytes([fid & 0xFF, mid & 0xFF]) + payload
        results.append((M_PREFILTER, wrapped))
    return results


def compress(data: bytes, *, method: str = "auto") -> bytes:
    """
    Compress *data* into a BAV1 frame.

    method:
      "auto"     — try all backends + research paths; pick smallest frame
      "store"    — no compression
      "deflate"  — zlib level 9
      "lzma"     — LZMA2 extreme
      "zstd"     — zstd level 22
      "brotli"   — brotli quality 11
      "research" — same as auto (all research candidates)
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
        "brotli": M_BROTLI,
    }

    candidates: list[tuple[int, bytes]] = []
    if method in forced:
        mid = forced[method]
        enc, _ = _BACKENDS[mid]
        candidates.append((mid, enc(data)))
    else:
        candidates.extend(_try_backends(data))
        candidates.extend(_research_transpose_candidates(data))
        candidates.extend(_research_prefilter_candidates(data))

    if not candidates:
        candidates.append((M_STORE, data))

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
    elif mid == M_PREFILTER:
        if len(payload) < 2:
            raise ValueError("prefilter payload too short")
        fid = payload[0]
        backend = payload[1]
        if backend not in _BACKENDS:
            raise ValueError(f"unknown prefilter backend {backend}")
        _, dec = _BACKENDS[backend]
        filtered = dec(payload[2:])
        data = _undo_filter(filtered, fid)
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
