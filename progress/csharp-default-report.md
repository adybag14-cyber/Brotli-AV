# C# is the default BAV port (0.3.0)

**Date:** 2026-07-30

## Default entry

| Entry | Target |
|-------|--------|
| `.\bav.ps1` / `.\bav.cmd` | `ports/csharp` → `bav-csharp.exe` |
| Direct | `ports/csharp/bin/Release/net8.0/bav-csharp.exe` |
| Version string | `bav-csharp 0.3.0 (default port, full research)` |

Default method: **`auto`** (all backends + full research).

## Full research coverage

| Family | Coverage |
|--------|----------|
| Backends | STORE, Deflate (`ZLibStream` max), LZMA/XZ, Zstd-22, Brotli (Optimal + SmallestSize) |
| Transpose | widths 2,3,4,5,6,8,12,16 |
| Xform | transpose + NONE/SUB1/SUB4/XOR1 |
| Prefilters | MTF, RLE0, MTF+RLE0, **SUB@1,2,3,4,5,6,8,12,16**, XOR1, XOR4 |
| BWT | raw / MTF / MTF+RLE0 / MTF+SUB1 / MTF+SUB1+RLE0 |
| Multi-block | 1K–16K block sizes, best simple backend each |
| Parts | 2–4 parts; per-part transpose/xform + full prefilter grid |
| Token | printable-gate + dict/id dual backend |

Extended SUB distances mirror Python’s LZMA delta-distance search as **prefilter → best backend** (XZ.NET does not expose FILTER_DELTA chains). Same approach as the Rust port. Filter IDs shared with `src/bav/codec.py` for cross-decode.

## Corpus (auto, 2026-07-30)

| file | raw | csharp | freeze | vs freeze |
|------|-----|--------|--------|-----------|
| 01_plain_text.txt | 81601 | 9943 | 13752 | **beat** |
| 02_html_js.html | 35093 | 1178 | 1166 | +12 |
| 03_binary_records.bin | 30512 | 614 | 726 | **beat** |
| 04_mixed_archive.bin | 12075 | 2270 | 2409 | **beat** |
| 05_source_code.py | 58780 | 883 | 890 | **beat** |
| **main-5 total** | **218061** | **14888** | **18943** | **beat total** |

All listed files + edges: **lossless RT OK**. Wall encode ~8.4 s for main set on this machine.

HTML is slightly above freeze (Brotli/backend quality delta vs Python `brotli` q=11 multi-mode); total and 4/5 files beat freeze.

## Repo updates

- `README.md` — C# default
- `ports/README.md` — C# first
- `ports/build_all.ps1` — C# built first
- `benchmarks/run_lang_bench.py` — C# discovered as `default`
- `src/bav/codec.py` — matching extended prefilter IDs (decode parity)
