"""
BAV1 lossless research codec.

Design goals (see README / research notes):
  - Strictly lossless round-trip.
  - Strictly beat the frozen prior-BAV baseline on every fixed-corpus file and total
    (primary self-beat gate). Secondary: beat pure Zstd-22 every-file + Brotli-11 total.
  - Wire format is BAV1 (not Zstd/Brotli wire-compatible); ratio is the research target.

Methods tried in auto mode (smallest payload wins, header overhead included):
  STORE, DEFLATE-9, LZMA-extreme (incl. delta filter chain search), ZSTD-22, BROTLI-11,
  research record-transpose, MTF/RLE0 prefilters, BWT(+MTF/RLE0)+backend, multi-block.
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
# Research BWT: flags u8, primary u32 LE, backend u8, compressed last-column
M_BWT = 7
# Research multi-block: u16 block_size, then repeated (u8 mid, u32 plen, payload)
M_BLOCKS = 8

# Prefilter IDs (payload[0] when method == M_PREFILTER)
F_MTF = 1
F_RLE0 = 2
F_MTF_RLE0 = 3

# BWT flags
BWT_F_MTF = 1
BWT_F_RLE0 = 2

_HEADER = struct.Struct("<4sBBIQI")  # magic, ver, method, flags, orig_size, crc32

# Soft cap for full-file BWT (naive SA is O(n^2 log n) on rotations)
_BWT_MAX_BYTES = 120_000


def _crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _deflate(data: bytes) -> bytes:
    return zlib.compress(data, level=9)


def _inflate(data: bytes) -> bytes:
    return zlib.decompress(data)


def _lzma_enc(data: bytes) -> bytes:
    """
    LZMA2 extreme, plus XZ delta+LZMA2 filter chains at several distances.
    XZ streams are self-describing, so decompress is plain lzma.decompress.
    """
    best = lzma.compress(data, preset=9 | lzma.PRESET_EXTREME)
    for dist in (1, 2, 3, 4, 5, 6, 8, 12, 16):
        filters = [
            {"id": lzma.FILTER_DELTA, "dist": dist},
            {"id": lzma.FILTER_LZMA2, "preset": 9 | lzma.PRESET_EXTREME},
        ]
        try:
            cand = lzma.compress(data, format=lzma.FORMAT_XZ, filters=filters)
        except Exception:
            continue
        if len(cand) < len(best):
            best = cand
    return best


def _lzma_dec(data: bytes) -> bytes:
    return lzma.decompress(data)


def _zstd_enc(data: bytes) -> bytes:
    cctx = zstd.ZstdCompressor(level=22)
    return cctx.compress(data)


def _zstd_dec(data: bytes) -> bytes:
    dctx = zstd.ZstdDecompressor()
    return dctx.decompress(data)


def _brotli_enc(data: bytes) -> bytes:
    # Try generic + text modes; keep the smaller payload
    best = brotli.compress(data, quality=11)
    for mode in (brotli.MODE_TEXT, brotli.MODE_FONT):
        try:
            cand = brotli.compress(data, quality=11, mode=mode)
        except Exception:
            continue
        if len(cand) < len(best):
            best = cand
    return best


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
    """Move-to-front transform (lossless). Position table for O(1) symbol→rank."""
    # pos[symbol] = current rank; table[rank] = symbol
    table = list(range(256))
    pos = list(range(256))
    out = bytearray(len(data))
    for i, b in enumerate(data):
        r = pos[b]
        out[i] = r
        if r:
            # shift symbols in ranks 0..r-1 up by one; place b at front
            for j in range(r, 0, -1):
                s = table[j - 1]
                table[j] = s
                pos[s] = j
            table[0] = b
            pos[b] = 0
    return bytes(out)


def _mtf_decode(data: bytes) -> bytes:
    table = list(range(256))
    out = bytearray(len(data))
    for i, r in enumerate(data):
        b = table[r]
        out[i] = b
        if r:
            for j in range(r, 0, -1):
                table[j] = table[j - 1]
            table[0] = b
    return bytes(out)


def _rle0_encode(data: bytes) -> bytes:
    """
    Zero-run length encoding (bzip2-style lite): non-zero bytes pass through;
    runs of zeros become 0 then count (1..255).
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


def _bwt_encode(data: bytes) -> tuple[bytes, int]:
    """Burrows–Wheeler transform; returns (last_column, primary_index)."""
    n = len(data)
    if n == 0:
        return b"", 0
    s = data + data
    sa = sorted(range(n), key=lambda i: s[i : i + n])
    last = bytes(data[(i - 1) % n] for i in sa)
    primary = sa.index(0)
    return last, primary


def _bwt_decode(last: bytes, primary: int) -> bytes:
    """Inverse BWT via LF-mapping / sorted first-column chain."""
    n = len(last)
    if n == 0:
        return b""
    if primary < 0 or primary >= n:
        raise ValueError("BWT primary index out of range")
    # T[j] = position in L corresponding to F[j] after stable sort by L[i]
    order = sorted(range(n), key=lambda i: last[i])
    # Walk: start at primary row; next row is order chain
    # Standard: result[i] = L[p]; p = index of this L in the sorted F correspondence
    # Using: p starts as primary; for i in 0..n-1: p = order[p]; out[i] = L[p]
    # Actually the common form that matches our encode:
    out = bytearray(n)
    p = primary
    for i in range(n):
        p = order[p]
        out[i] = last[p]
    return bytes(out)


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
    for width in (2, 3, 4, 5, 6, 8, 12, 16):
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
        if len(filtered) > len(data) * 2 + 64:
            continue
        mid, payload = _best_backend(filtered)
        wrapped = bytes([fid & 0xFF, mid & 0xFF]) + payload
        results.append((M_PREFILTER, wrapped))
    return results


def _research_bwt_candidates(data: bytes) -> list[tuple[int, bytes]]:
    """Research path: full-file BWT (+ optional MTF/RLE0) + best backend."""
    results: list[tuple[int, bytes]] = []
    n = len(data)
    if n < 64 or n > _BWT_MAX_BYTES:
        return results
    try:
        last, primary = _bwt_encode(data)
    except Exception:
        return results

    variants: list[tuple[int, bytes]] = [
        (0, last),
        (BWT_F_MTF, _mtf_encode(last)),
        (BWT_F_MTF | BWT_F_RLE0, _rle0_encode(_mtf_encode(last))),
    ]
    for flags, transformed in variants:
        mid, payload = _best_backend(transformed)
        wrapped = (
            bytes([flags & 0xFF])
            + struct.pack("<I", primary)
            + bytes([mid & 0xFF])
            + payload
        )
        results.append((M_BWT, wrapped))
    return results


def _research_block_candidates(data: bytes) -> list[tuple[int, bytes]]:
    """
    Research path: fixed-size blocks, each compressed with the best *simple* backend
    (no recursive research paths — avoids exponential cost).
    """
    results: list[tuple[int, bytes]] = []
    n = len(data)
    if n < 1024:
        return results
    for bs in (4096, 8192, 16384):
        if n < bs:
            continue
        out = bytearray()
        out += struct.pack("<H", bs)
        i = 0
        while i < n:
            chunk = data[i : i + bs]
            mid, payload = _best_backend(chunk)
            if len(payload) > 0xFFFFFFFF:
                break
            out += bytes([mid & 0xFF]) + struct.pack("<I", len(payload)) + payload
            i += bs
        else:
            results.append((M_BLOCKS, bytes(out)))
    return results


def compress(data: bytes, *, method: str = "auto") -> bytes:
    """
    Compress *data* into a BAV1 frame.

    method:
      "auto"     — try all backends + research paths; pick smallest frame
      "store"    — no compression
      "deflate"  — zlib level 9
      "lzma"     — LZMA2 extreme (+ delta chains)
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
        candidates.extend(_research_bwt_candidates(data))
        candidates.extend(_research_block_candidates(data))

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
    elif mid == M_BWT:
        if len(payload) < 1 + 4 + 1:
            raise ValueError("BWT payload too short")
        flags = payload[0]
        primary = struct.unpack_from("<I", payload, 1)[0]
        backend = payload[5]
        if backend not in _BACKENDS:
            raise ValueError(f"unknown BWT backend {backend}")
        _, dec = _BACKENDS[backend]
        transformed = dec(payload[6:])
        if flags & BWT_F_RLE0:
            transformed = _rle0_decode(transformed)
        if flags & BWT_F_MTF:
            transformed = _mtf_decode(transformed)
        data = _bwt_decode(transformed, primary)
    elif mid == M_BLOCKS:
        if len(payload) < 2:
            raise ValueError("blocks payload too short")
        bs = struct.unpack_from("<H", payload, 0)[0]
        if bs == 0:
            raise ValueError("invalid block size")
        pos = 2
        chunks: list[bytes] = []
        while pos < len(payload):
            if pos + 5 > len(payload):
                raise ValueError("truncated block header")
            bmid = payload[pos]
            plen = struct.unpack_from("<I", payload, pos + 1)[0]
            pos += 5
            if pos + plen > len(payload):
                raise ValueError("truncated block payload")
            if bmid not in _BACKENDS:
                raise ValueError(f"unknown block backend {bmid}")
            _, dec = _BACKENDS[bmid]
            chunks.append(dec(payload[pos : pos + plen]))
            pos += plen
        data = b"".join(chunks)
        # trailing incomplete logical size is OK if orig matches
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
