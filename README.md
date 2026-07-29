# Brotli-AV (BAV)

**Mission:** Advance compression research to **beat Google Brotli** on compression ratio for a fixed, multi-type benchmark corpus — lossless end-to-end.

Remote: [https://github.com/adybag14-cyber/Brotli-AV.git](https://github.com/adybag14-cyber/Brotli-AV.git)  
Local research root: `E:\brotli-research`

> “ADVANCE COMPUTING FOR EARTH” — ratio first, measured, reproducible.

## What this repo is

| Piece | Role |
|-------|------|
| **BAV1 codec** (`src/bav`) | Research compress/decompress path (not RFC 7932 wire-compatible) |
| **Fixed corpus** (`corpus/`) | Plain text, HTML/JS, structured binary, mixed archive-style, source |
| **Harness** (`benchmarks/`) | Side-by-side vs **stock Google Brotli** quality **11** (`brotli==1.2.0`) |
| **Tests** (`tests/`) | Lossless round-trips + total-byte win gate |
| **Research / progress** | Notes and logs under `research/`, `progress/` |

Gating win condition: **sum of compressed sizes on the fixed corpus is strictly smaller than Google Brotli q=11**, with full lossless round-trip on every file.

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
- `pip install -r requirements.txt` (pins `brotli==1.2.0`, `zstandard`)

```powershell
cd E:\brotli-research
python -m pip install -r requirements.txt
python -m pip install -e .
python tools\generate_corpus.py
```

## Build / install

No C toolchain required for the default path. The research codec is pure Python + stdlib `lzma`/`zlib` + `zstandard`, compared against the official `brotli` package (Google libbrotli bindings).

Optional editable install:

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

After `pip install -e .`, the `bav` console script is also available.

## Benchmarks vs Google Brotli

Pinned baseline (see `benchmarks/config.json` and `third_party/BASELINE.md`):

| Setting | Value |
|---------|--------|
| Engine | Google Brotli via PyPI `brotli==1.2.0` |
| Quality | **11** (maximum) |
| Research method | `auto` (multi-backend + transpose research path) |
| Corpus | `corpus/01_…` … `05_…` (not edge_* files for the total-byte gate) |

```powershell
python benchmarks\run_bench.py -o progress\benchmark-report.json
python -m unittest discover -s tests -v
```

The harness records per-file uncompressed size, Brotli size, BAV size, ratios, and totals. Exit code **1** if BAV does not beat Brotli on total compressed bytes.

## Research approach (short)

BAV1 tries multiple strong backends per file and keeps the **smallest frame** (header overhead included):

1. Store / Deflate-9 / **LZMA2 extreme** / **Zstd-22**
2. **Research transpose path**: column-major reorder of fixed-width records (2/4/8/12/16), then re-select backend — helps structured binary

No shared cross-file dictionary cheat: each file is compressed independently. Format is BAV1 (magic `BAV1`), not Brotli-compatible streams.

## Tests

```powershell
python -m unittest discover -s tests -v
```

- `test_lossless.py` — round-trip on full corpus + edges via shipped API  
- `test_cli.py` — real `python -m bav` compress/decompress  
- `test_beat_brotli.py` — total compressed bytes &lt; Brotli q=11  

## License

Research workspace. Baseline Brotli is Google’s project under its own license; use their package as a comparison dependency only.
