# Progress log

## 2026-07-29 — Beat Zstd-22 on every file + total

- Primary baseline retargeted to **pure Zstd level 22** (`zstandard==0.25.0`)
- Codec: added **Brotli-11** backend + MTF/RLE0 prefilter research path; keep transpose
- Harness `benchmarks/run_bench.py` compares vs pure zstd frames; fails if any file or total does not win
- Gating test: `tests/test_beat_zstd22.py` (per-file + total); Brotli total remains secondary

### Measured totals (fixed corpus)

| Engine | Total compressed bytes |
|--------|------------------------:|
| Pure Zstd-22 | 38032 |
| Brotli-AV (BAV1 auto) | **21364** |
| Delta (Zstd − BAV) | 16668 |

Every corpus file: BAV size &lt; pure Zstd-22 size.

### Evidence paths

| Artifact | Location |
|----------|----------|
| Repo setup | `{SCRATCH}/repo-zstd-goal.txt` |
| Lossless tests | `{SCRATCH}/lossless-tests.log` |
| Beat Zstd-22 tests | `{SCRATCH}/beat-zstd22-tests.log` |
| Benchmark report | `{SCRATCH}/benchmark-zstd22-report.json` (+ `.txt`) |
| CLI run 1/2 | `{SCRATCH}/cli-run1.log`, `{SCRATCH}/cli-run2.log` |

## Earlier — Beat Google Brotli (total)

See git history / prior progress: BAV total beat Brotli q=11 on the same corpus (total-only gate).
