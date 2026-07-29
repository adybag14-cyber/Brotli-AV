# Research notes — Brotli-AV

## Objective

Beat Google Brotli (q=11) on **total compressed bytes** over a diverse fixed corpus, with strict lossless round-trip. Wire-format compatibility with RFC 7932 is intentionally **not** required (see plan non-goals).

## Approach v0.1

**Adaptive multi-backend selection** with a light structural transform:

- Candidate backends: store, zlib-9, LZMA2 extreme, Zstd level 22.
- Research path: fixed-width **record transpose** (widths 2/4/8/12/16) then re-run backends.
- Emit BAV1 frame for the candidate with the smallest payload (stable tie-break on method id).

### Why this can beat Brotli

Brotli is excellent on HTML/text with its static dictionary and context modeling. LZMA2 extreme and transpose+LZ often win on **structured binary** and some **source** layouts. Selecting per-file best backend yields a lower **sum** without shared dictionaries.

## Future directions (not gating)

- Block-level (not whole-file) method switching
- Optimal parsing / larger windows on a custom LZ
- PPM / context-mixing residual coding
- Domain filters (EXE, RGB, etc.)
- Broader public suites (Squash, LTCB) after local corpus remains green
