# Brotli-AV (BAV)

**Mission:** Advance compression research to **beat pure Zstd level 22** on compressed size for every file in a fixed multi-type corpus (and on the total) — lossless end-to-end. Historical bar (Google Brotli q=11 total) remains a secondary regression.

Remote: [https://github.com/adybag14-cyber/Brotli-AV.git](https://github.com/adybag14-cyber/Brotli-AV.git)  
Local research root: `E:\brotli-research`

> “ADVANCE COMPUTING FOR EARTH” — ratio first, measured, reproducible.

## What this repo is

| Piece | Role |
|-------|------|
| **BAV1 codec** (`src/bav`) | Research compress/decompress path (not Zstd/Brotli wire-compatible) |
| **Fixed corpus** (`corpus/`) | Plain text, HTML/JS, structured binary, mixed archive-style, source |
| **Harness** (`benchmarks/`) | Side-by-side vs **pure Zstd-22** (`zstandard==0.25.0`) |
| **Tests** (`tests/`) | Lossless round-trips + per-file & total Zstd-22 win gate |
| **Research / progress** | Notes and logs under `research/`, `progress/` |

**Gating win condition:** for every fixed corpus file and for the sum of all of them, BAV compressed size is **strictly smaller than pure Zstd level 22**. Round-trip must be lossless. Speed is not a gate.

## Layout

```
E:\brotli-research\
  README.md
  requirements.txt / pyproject.toml
  src/bav/           # shipped compressor package
  corpus/            # fixed verification files (generated)
  benchmarks/        # config.json + run_bench.py
  tests/             # unit/integration tests (real APIs)
  tools/             # generate_corpus.py
  research/          # design notes
  progress/          # measured progress log
  third_party/       # baseline pin notes
```

## Requirements

- Python **3.10+** (developed on 3.14)
- `pip install -r requirements.txt` (pins `zstandard==0.25.0`, `brotli==1.2.0`)

```powershell
cd E:\brotli-research
python -m pip install -r requirements.txt
python -m pip install -e .
python tools\generate_corpus.py
```

## Build / install

No C toolchain required for the default path. Research codec uses Python + stdlib `lzma`/`zlib` + `zstandard` + `brotli`, compared against **pure** `ZstdCompressor(level=22)` frames.

```powershell
python -m pip install -e .
```

Or set `PYTHONPATH=E:\brotli-research\src`.

## CLI (primary entry)

```powershell
# Compress
python -m bav compress corpus\01_plain_text.txt -o out.bav

# Decompress
python -m bav decompress out.bav -o restored.txt

# Version
python -m bav version
```

## Benchmarks vs Zstd-22

Pinned primary baseline (see `benchmarks/config.json` and `third_party/BASELINE.md`):

| Setting | Value |
|---------|--------|
| Engine | Zstd via PyPI `zstandard==0.25.0` |
| Level | **22** (maximum) |
| Comparison | **Pure zstd frames** (not BAV-wrapped) |
| Research method | `auto` (multi-backend + research transforms) |
| Corpus | `corpus/01_…` … `05_…` |

```powershell
python benchmarks\run_bench.py -o progress\benchmark-zstd22-report.json
python -m unittest discover -s tests -v
```

Exit code **1** if BAV does not strictly beat pure Zstd-22 on **every file** and the **total**.

Optional secondary Brotli sizes: `python benchmarks\run_bench.py --with-brotli -o report.json`

## Research approach (short)

BAV1 tries multiple strong backends and research pre/transforms per file; keeps the **smallest BAV1 frame** (18-byte header counted):

1. Store / Deflate-9 / **LZMA2 extreme** / **Zstd-22** / **Brotli-11**
2. **Record transpose** (widths 2–16) then re-select backend
3. **MTF / RLE0 / MTF+RLE0** prefilters then re-select backend

No shared cross-file dictionary. Format is BAV1 (magic `BAV1`).

## Tests

```powershell
python -m unittest discover -s tests -v
```

- `test_lossless.py` — round-trip on full corpus + edges via shipped API  
- `test_cli.py` — real `python -m bav` compress/decompress  
- `test_beat_zstd22.py` — **every file + total** &lt; pure Zstd-22  
- `test_beat_brotli.py` — secondary: total &lt; Brotli q=11  

## License

Research workspace. Zstd and Brotli are third-party projects under their own licenses; used here as comparison dependencies and backends.
