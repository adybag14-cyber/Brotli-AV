# Brotli-AV (BAV)

**Mission:** Push the research compressor past its own frozen baseline — **BAV beats BAV** — on every fixed-corpus file and on the total, with lossless round-trip and scientific-frontier size/entropy reporting. Secondary regressions: pure Zstd-22 (every file + total) and Brotli q=11 (total).

Remote: [https://github.com/adybag14-cyber/Brotli-AV.git](https://github.com/adybag14-cyber/Brotli-AV.git)  
Local research root: `E:\brotli-research`

> “ADVANCE COMPUTING FOR EARTH” — ratio first, measured, reproducible.

## Primary gate (this generation)

| Item | Definition |
|------|------------|
| Freeze | `benchmarks/prior_bav_baseline.json` (prior BAV1 auto sizes) |
| Win | New auto size **&lt; frozen** for **each** of `corpus/01_…`…`05_…` **and** total |
| Evidence | Self-beat harness + frontier entropy report |

## Layout

```
E:\brotli-research\
  src/bav/              # shipped compressor
  corpus/               # fixed verification files
  benchmarks/           # config, prior freeze, run_bench, run_frontier
  tests/                # lossless, self-beat, zstd/brotli regressions, CLI
  research/ progress/ third_party/
```

## Setup

```powershell
cd E:\brotli-research
python -m pip install -r requirements.txt
python -m pip install -e .
python tools\generate_corpus.py   # if corpus missing
```

## CLI

```powershell
python -m bav compress corpus\01_plain_text.txt -o out.bav
python -m bav decompress out.bav -o restored.txt
python -m bav version
```

## Benchmarks & frontier

```powershell
# Self-beat vs frozen prior-BAV
python benchmarks\run_bench.py -o progress\benchmark-beat-bav-report.json

# Entropy / residual-gap frontier report (order-0 & order-1 estimates)
python benchmarks\run_frontier.py -o progress\frontier-report.json

python -m unittest discover -s tests -v
```

## Research approach (current)

BAV1 auto selects the smallest frame among:

1. Store / Deflate-9 / **LZMA2 extreme + XZ delta chains** / Zstd-22 / Brotli-11  
2. Record **transpose** then re-select backend  
3. **MTF / RLE0** prefilters then re-select backend  
4. **BWT** (+ optional MTF/RLE0) then re-select backend  
5. **Multi-block** adaptive backends  

Format: BAV1 magic `BAV1` (not Zstd/Brotli wire-compatible). No shared cross-file dictionary.

Entropy columns in the frontier report are Shannon reference bounds — **not** Kolmogorov optimality claims.

## Tests

- `test_lossless.py` — corpus + edges round-trip  
- `test_beat_prior_bav.py` — **primary** self-beat vs freeze  
- `test_beat_zstd22.py` / `test_beat_brotli.py` — secondary regressions  
- `test_cli.py` — real `python -m bav`  
