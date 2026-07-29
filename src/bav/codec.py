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
FORMAT_VERSION = 2
FORMAT_VERSION_LEGACY = 1

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
M_TOKEN = 9
M_XFORM = 10
M_PARTS = 11

# Prefilter IDs (payload[0] when method == M_PREFILTER)
F_MTF = 1
F_RLE0 = 2
F_MTF_RLE0 = 3
F_SUB1 = 4
F_SUB4 = 5
F_XOR1 = 6

# BWT flags
BWT_F_MTF = 1
BWT_F_RLE0 = 2
BWT_F_SUB1 = 4

# XFORM after transpose
X_NONE = 0
X_SUB1 = 1
X_SUB4 = 2
X_XOR1 = 3

_HEADER_V1 = struct.Struct("<4sBBIQI")
_HEADER_V2 = struct.Struct("<4sBBBII")

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



def _sub_delta(data: bytes, dist: int = 1) -> bytes:
    if dist < 1 or not data:
        return data
    out = bytearray(data)
    for i in range(len(data) - 1, dist - 1, -1):
        out[i] = (data[i] - data[i - dist]) & 0xFF
    return bytes(out)


def _sub_delta_inv(data: bytes, dist: int = 1) -> bytes:
    if dist < 1 or not data:
        return data
    out = bytearray(data)
    for i in range(dist, len(data)):
        out[i] = (out[i] + out[i - dist]) & 0xFF
    return bytes(out)


def _xor_delta(data: bytes, dist: int = 1) -> bytes:
    if dist < 1 or not data:
        return data
    out = bytearray(data)
    for i in range(len(data) - 1, dist - 1, -1):
        out[i] = data[i] ^ data[i - dist]
    return bytes(out)


def _xor_delta_inv(data: bytes, dist: int = 1) -> bytes:
    if dist < 1 or not data:
        return data
    out = bytearray(data)
    for i in range(dist, len(data)):
        out[i] = out[i] ^ out[i - dist]
    return bytes(out)


def _is_word_byte(b: int) -> bool:
    return (48 <= b <= 57) or (65 <= b <= 90) or (97 <= b <= 122) or b == 95


def _token_split(data: bytes) -> list[bytes]:
    tokens: list[bytes] = []
    i, n = 0, len(data)
    while i < n:
        if _is_word_byte(data[i]):
            j = i + 1
            while j < n and _is_word_byte(data[j]):
                j += 1
            tokens.append(data[i:j]); i = j
        else:
            tokens.append(data[i:i+1]); i += 1
    return tokens


def _varint_encode(values: list[int]) -> bytes:
    out = bytearray()
    for v in values:
        while v >= 0x80:
            out.append((v & 0x7F) | 0x80); v >>= 7
        out.append(v)
    return bytes(out)


def _varint_decode(data: bytes) -> list[int]:
    out: list[int] = []; i, n = 0, len(data)
    while i < n:
        shift = val = 0
        while True:
            if i >= n: raise ValueError("truncated varint")
            b = data[i]; i += 1
            val |= (b & 0x7F) << shift
            if b < 0x80: break
            shift += 7
        out.append(val)
    return out


def _token_encode_payload(data: bytes) -> bytes:
    from collections import Counter
    import struct as _st
    tokens = _token_split(data)
    counts = Counter(tokens)
    first = {}
    for i, t in enumerate(tokens):
        if t not in first: first[t] = i
    uniq = sorted(counts.keys(), key=lambda t: (-counts[t], first[t]))
    id_map = {t: i for i, t in enumerate(uniq)}
    ids = [id_map[t] for t in tokens]
    db = bytearray()
    for t in uniq:
        db += _st.pack("<H", len(t)) + t
    d_mid, dcomp = _best_backend(bytes(db))
    i_mid, icomp = _best_backend(_varint_encode(ids))
    return bytes([d_mid & 0xFF, i_mid & 0xFF]) + _st.pack("<II", len(dcomp), len(icomp)) + dcomp + icomp


def _token_decode_payload(payload: bytes) -> bytes:
    import struct as _st
    if len(payload) < 10: raise ValueError("token payload too short")
    d_mid, i_mid = payload[0], payload[1]
    dlen, ilen = _st.unpack_from("<II", payload, 2)
    pos = 10
    dict_blob = _BACKENDS[d_mid][1](payload[pos:pos+dlen])
    id_blob = _BACKENDS[i_mid][1](payload[pos+dlen:pos+dlen+ilen])
    uniq = []; p = 0
    while p < len(dict_blob):
        (ln,) = _st.unpack_from("<H", dict_blob, p); p += 2
        uniq.append(dict_blob[p:p+ln]); p += ln
    out = bytearray()
    for tid in _varint_decode(id_blob):
        out += uniq[tid]
    return bytes(out)


def _apply_filter(data: bytes, fid: int) -> bytes:
    if fid == F_MTF:
        return _mtf_encode(data)
    if fid == F_RLE0:
        return _rle0_encode(data)
    if fid == F_MTF_RLE0:
        return _rle0_encode(_mtf_encode(data))
    if fid == F_SUB1:
        return _sub_delta(data, 1)
    if fid == F_SUB4:
        return _sub_delta(data, 4)
    if fid == F_XOR1:
        return _xor_delta(data, 1)
    raise ValueError(f"unknown filter {fid}")


def _undo_filter(data: bytes, fid: int) -> bytes:
    if fid == F_MTF:
        return _mtf_decode(data)
    if fid == F_RLE0:
        return _rle0_decode(data)
    if fid == F_MTF_RLE0:
        return _mtf_decode(_rle0_decode(data))
    if fid == F_SUB1:
        return _sub_delta_inv(data, 1)
    if fid == F_SUB4:
        return _sub_delta_inv(data, 4)
    if fid == F_XOR1:
        return _xor_delta_inv(data, 1)
    raise ValueError(f"unknown filter {fid}")


def _apply_xform(data: bytes, xid: int) -> bytes:
    if xid == X_NONE: return data
    if xid == X_SUB1: return _sub_delta(data, 1)
    if xid == X_SUB4: return _sub_delta(data, 4)
    if xid == X_XOR1: return _xor_delta(data, 1)
    raise ValueError(f"unknown xform {xid}")


def _undo_xform(data: bytes, xid: int) -> bytes:
    if xid == X_NONE: return data
    if xid == X_SUB1: return _sub_delta_inv(data, 1)
    if xid == X_SUB4: return _sub_delta_inv(data, 4)
    if xid == X_XOR1: return _xor_delta_inv(data, 1)
    raise ValueError(f"unknown xform {xid}")


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
    for fid in (F_MTF, F_RLE0, F_MTF_RLE0, F_SUB1, F_SUB4, F_XOR1):
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

    mtf = _mtf_encode(last)
    variants: list[tuple[int, bytes]] = [
        (0, last),
        (BWT_F_MTF, mtf),
        (BWT_F_MTF | BWT_F_RLE0, _rle0_encode(mtf)),
        (BWT_F_MTF | BWT_F_SUB1, _sub_delta(mtf, 1)),
        (BWT_F_MTF | BWT_F_SUB1 | BWT_F_RLE0, _rle0_encode(_sub_delta(mtf, 1))),
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
    for bs in (1024, 2048, 3072, 4096, 6144, 8192, 16384):
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



def _research_xform_candidates(data: bytes) -> list[tuple[int, bytes]]:
    results: list[tuple[int, bytes]] = []
    for width in (2, 4, 8, 12, 16):
        if len(data) < width * 4:
            continue
        base = _transpose(data, width)
        for xid in (X_NONE, X_SUB1, X_SUB4, X_XOR1):
            transformed = _apply_xform(base, xid)
            mid, payload = _best_backend(transformed)
            wrapped = bytes([width & 0xFF, xid & 0xFF, mid & 0xFF]) + payload
            results.append((M_XFORM, wrapped))
    return results


def _research_token_candidates(data: bytes) -> list[tuple[int, bytes]]:
    if len(data) < 64:
        return []
    printable = sum(1 for b in data if 32 <= b < 127 or b in (9, 10, 13))
    if printable < len(data) * 0.4:
        return []
    try:
        return [(M_TOKEN, _token_encode_payload(data))]
    except Exception:
        return []


def _research_parts_candidates(data: bytes) -> list[tuple[int, bytes]]:
    results: list[tuple[int, bytes]] = []
    n = len(data)
    if n < 1024:
        return results
    for nparts in (2, 3, 4):
        out = bytearray()
        out += struct.pack("<H", nparts)
        for i in range(nparts):
            a = i * n // nparts
            b = (i + 1) * n // nparts
            chunk = data[a:b]
            mid, payload = _best_backend(chunk)
            for w in (4, 8, 12):
                if len(chunk) < w * 4:
                    continue
                base = _transpose(chunk, w)
                for xid in (X_NONE, X_SUB1, X_SUB4):
                    tr = _apply_xform(base, xid)
                    m2, p2 = _best_backend(tr)
                    xp = bytes([w & 0xFF, xid & 0xFF, m2 & 0xFF]) + p2
                    if len(xp) < len(payload):
                        mid, payload = M_XFORM, xp
            for fid in (F_SUB1, F_SUB4):
                try:
                    f = _apply_filter(chunk, fid)
                except Exception:
                    continue
                m2, p2 = _best_backend(f)
                pref = bytes([fid & 0xFF, m2 & 0xFF]) + p2
                if len(pref) < len(payload):
                    mid, payload = M_PREFILTER, pref
            out += bytes([mid & 0xFF]) + struct.pack("<I", len(payload)) + payload
        results.append((M_PARTS, bytes(out)))
    return results


def _decode_method_payload(mid: int, payload: bytes) -> bytes:
    if mid == M_TRANSPOSE:
        width, backend = payload[0], payload[1]
        return _untranspose(_BACKENDS[backend][1](payload[2:]), width)
    if mid == M_XFORM:
        width, xid, backend = payload[0], payload[1], payload[2]
        return _untranspose(_undo_xform(_BACKENDS[backend][1](payload[3:]), xid), width)
    if mid == M_PREFILTER:
        fid, backend = payload[0], payload[1]
        return _undo_filter(_BACKENDS[backend][1](payload[2:]), fid)
    if mid == M_BWT:
        flags = payload[0]
        primary = struct.unpack_from("<I", payload, 1)[0]
        backend = payload[5]
        transformed = _BACKENDS[backend][1](payload[6:])
        if flags & BWT_F_RLE0:
            transformed = _rle0_decode(transformed)
        if flags & BWT_F_SUB1:
            transformed = _sub_delta_inv(transformed, 1)
        if flags & BWT_F_MTF:
            transformed = _mtf_decode(transformed)
        return _bwt_decode(transformed, primary)
    if mid == M_TOKEN:
        return _token_decode_payload(payload)
    if mid == M_BLOCKS:
        bs = struct.unpack_from("<H", payload, 0)[0]
        pos = 2
        chunks = []
        while pos < len(payload):
            bmid = payload[pos]
            plen = struct.unpack_from("<I", payload, pos + 1)[0]
            pos += 5
            chunks.append(_decode_method_payload(bmid, payload[pos:pos+plen]))
            pos += plen
        return b"".join(chunks)
    if mid == M_PARTS:
        nparts = struct.unpack_from("<H", payload, 0)[0]
        pos = 2
        chunks = []
        for _ in range(nparts):
            pmid = payload[pos]
            plen = struct.unpack_from("<I", payload, pos + 1)[0]
            pos += 5
            chunks.append(_decode_method_payload(pmid, payload[pos:pos+plen]))
            pos += plen
        return b"".join(chunks)
    if mid in _BACKENDS:
        return _BACKENDS[mid][1](payload)
    raise ValueError(f"unknown method id {mid}")


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
        candidates.extend(_research_xform_candidates(data))
        candidates.extend(_research_prefilter_candidates(data))
        candidates.extend(_research_bwt_candidates(data))
        candidates.extend(_research_block_candidates(data))
        candidates.extend(_research_parts_candidates(data))
        candidates.extend(_research_token_candidates(data))

    if not candidates:
        candidates.append((M_STORE, data))

    candidates.sort(key=lambda x: (len(x[1]), x[0]))
    mid, payload = candidates[0]

    if orig > 0xFFFFFFFF:
        header = _HEADER_V1.pack(MAGIC, FORMAT_VERSION_LEGACY, mid, 0, orig, crc)
    else:
        header = _HEADER_V2.pack(MAGIC, FORMAT_VERSION, mid, 0, orig, crc)
    return header + payload


def decompress(frame: bytes) -> bytes:
    """Decompress a BAV frame (v1 or v2); verify CRC32 and original size."""
    if not isinstance(frame, (bytes, bytearray, memoryview)):
        raise TypeError("frame must be bytes-like")
    frame = bytes(frame)
    if len(frame) < 8:
        raise ValueError("BAV frame too short")
    if frame[:4] != MAGIC:
        raise ValueError(f"bad magic: {frame[:4]!r}")
    ver = frame[4]
    if ver == FORMAT_VERSION:
        if len(frame) < _HEADER_V2.size:
            raise ValueError("BAV v2 frame too short")
        _m, _v, mid, _f, orig, crc = _HEADER_V2.unpack_from(frame, 0)
        payload = frame[_HEADER_V2.size:]
    elif ver == FORMAT_VERSION_LEGACY:
        if len(frame) < _HEADER_V1.size:
            raise ValueError("BAV v1 frame too short")
        _m, _v, mid, _f, orig, crc = _HEADER_V1.unpack_from(frame, 0)
        payload = frame[_HEADER_V1.size:]
    else:
        raise ValueError(f"unsupported BAV version: {ver}")
    data = _decode_method_payload(mid, payload)
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
