# Progress log

## 2026-07-29 — Workspace bootstrap

- Initialized `E:\brotli-research` git repo → origin `https://github.com/adybag14-cyber/Brotli-AV.git`
- Scaffolded layout: `src/bav`, `corpus`, `benchmarks`, `tests`, `research`, `progress`, `third_party`, `tools`
- Implemented BAV1 research codec + CLI (`python -m bav`)
- Pinned Google Brotli baseline `brotli==1.2.0` @ quality 11
- Generated fixed multi-type corpus; harness + lossless / beat-brotli / CLI tests
- Evidence captures: see implementer scratch + this tree’s `progress/benchmark-report.*` when generated

### Measured totals (fixed corpus, Brotli q=11 vs BAV auto)

| Engine | Total compressed bytes |
|--------|------------------------:|
| Google Brotli q=11 | 34097 |
| Brotli-AV (BAV1 auto) | **21490** |
| Delta (BR − BAV) | 12607 |

All 8 unit/integration tests OK; CLI compress twice → identical size 13904.

### Evidence paths (verification run)

| Artifact | Location |
|----------|----------|
| Repo setup | implementer scratch `repo-setup.txt` |
| Lossless tests | `lossless-tests.log` |
| Benchmark report | `benchmark-report.json` (+ `.txt`); also `progress/benchmark-report.json` |
| CLI run 1/2 | `cli-run1.log`, `cli-run2.log` |
