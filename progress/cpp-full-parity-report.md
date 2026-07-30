# Full C++ BAV port (backup) — 0.3.0

**Date:** 2026-07-30

## Role

| | |
|--|--|
| **Main** | C# (`bav.ps1` → `bav-csharp`) |
| **Backup** | C++ (`bav.ps1 -impl cpp` / `bav-cpp.ps1` → `bav-cpp`) |

## Coverage (parity with C# / Python gen2+)

- Backends: STORE, Deflate-9 (zlib), **LZMA/XZ + delta distances 1–16**, Zstd-22, Brotli q11 (generic/text/font)
- Transpose, xform, prefilters (MTF/RLE0/SUB@1–16/XOR), BWT, blocks, parts, token
- BAV v2 header encode, v1+v2 decode
- Parallel research families via `std::thread`

## Build

```powershell
powershell -File ports\cpp\build.ps1
```

Links: `-lz`, static `liblzma.a` (xz 5.6.2), `libzstd.dll.a`, brotli C sources.

## Corpus (auto)

| file | raw | cpp | RT |
|------|-----|-----|-----|
| 01_plain_text.txt | 81601 | 9943 | OK |
| 02_html_js.html | 35093 | 1159 | OK |
| 03_binary_records.bin | 30512 | 614 | OK |
| 04_mixed_archive.bin | 12075 | 2270 | OK |
| 05_source_code.py | 58780 | 883 | OK |
| edges | | | OK |
| **main-5 total** | | **14869** | |

Beats freeze total 18943. HTML 1159 matches Python gen2 win (C# was 1178).

Encode wall ~147 s for full set (heavy LZMA extreme + delta grid + BWT); decode &lt; 0.1 s/file.
