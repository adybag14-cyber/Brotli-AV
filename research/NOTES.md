# Research notes — Brotli-AV

## Objective (current)

Beat **pure Zstd level 22** on **compressed size for every fixed-corpus file and on the total**, with strict lossless round-trip. Speed is not a gate.

Wire-format compatibility with Zstd or RFC 7932 Brotli is intentionally **not** required.

## Approach v0.2

**Adaptive multi-backend selection** plus light research transforms:

- Candidate backends: store, zlib-9, LZMA2 extreme, Zstd-22, **Brotli-11**.
- Research: fixed-width **record transpose** (2/3/4/6/8/12/16) then re-run backends.
- Research: **MTF**, **RLE0**, and **MTF+RLE0** prefilters then re-run backends.
- Emit BAV1 frame for the candidate with the smallest payload (stable tie-break on method id).

### Why wrapping alone is not enough

A pure “try zstd-22 among backends” selector cannot beat **raw** Zstd-22 when zstd is already best: the BAV header adds overhead. Winning requires either a stronger backend (e.g. Brotli-11 on text) or a transform that improves compressibility enough to pay for the header (transpose on structured binary).

### Measured insight (fixed corpus)

- Plain English-like text: Brotli-11 often beats Zstd-22 enough to cover the 18-byte BAV header.
- Structured binary records: column transpose + LZMA collapses far below Zstd-22.
- HTML / mixed / source: LZMA or Brotli typically beats raw Zstd-22 after selection.

## Future directions (not gating)

- Block-level method switching
- Optimal parsing / larger windows on a custom LZ
- PPM / context-mixing residual coding
- Domain filters (EXE, RGB, etc.)
- Broader public suites (Squash, LTCB) after local corpus remains green
