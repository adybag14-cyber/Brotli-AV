# Progress log

## 2026-07-29 — BAV beats frozen prior-BAV (frontier generation)

- Froze prior auto sizes in `benchmarks/prior_bav_baseline.json` (total **21364**)
- Codec: LZMA delta chains, BWT(+MTF/RLE0), multi-block; improved MTF
- Primary gate: every file + total &lt; freeze; frontier report with H0/H1 gaps
- Secondary: Zstd-22 every-file + Brotli total remain green

### Measured (new vs freeze)

| File | Frozen | New | Delta |
|------|-------:|----:|------:|
| 01_plain_text.txt | 13798 | 13752 | 46 |
| 02_html_js.html | 1262 | 1166 | 96 |
| 03_binary_records.bin | 1764 | 726 | 1038 |
| 04_mixed_archive.bin | 3386 | 2409 | 977 |
| 05_source_code.py | 1154 | 890 | 264 |
| **TOTAL** | **21364** | **18943** | **2421** |

CLI twice: size 13752 identical. Frontier: `progress/frontier-report.json`.

### Evidence paths

| Artifact | Location |
|----------|----------|
| Repo / freeze docs | `{SCRATCH}/repo-frontier-goal.txt` |
| Lossless tests | `{SCRATCH}/lossless-tests.log` |
| Self-beat tests | `{SCRATCH}/beat-prior-bav-tests.log` |
| Self-beat harness | `{SCRATCH}/benchmark-beat-bav-report.json` |
| Frontier report | `{SCRATCH}/frontier-report.json` |
| CLI / regressions | `{SCRATCH}/cli-run*.log`, `{SCRATCH}/regression-tests.log` |
