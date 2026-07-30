# Progress log

## Recursive self-improvement loop (≥2 generations)

### Generation 1
| | Total |
|--|------:|
| Freeze | **21364** |
| Win | **18943** (−2421) |

Ideas: LZMA delta chains, BWT(+MTF/RLE0), multi-block.  
Artifacts: `benchmarks/generations/gen1_freeze.json`, `gen1_win.json`.

### Generation 2 (active freeze = gen1 win)
| | Total |
|--|------:|
| Freeze | **18943** |
| Win | **14888** (−4055) |

| File | Freeze | New | Δ |
|------|-------:|----:|--:|
| 01_plain_text.txt | 13752 | 9943 | 3809 |
| 02_html_js.html | 1166 | 1159 | 7 |
| 03_binary_records.bin | 726 | 630 | 96 |
| 04_mixed_archive.bin | 2409 | 2273 | 136 |
| 05_source_code.py | 890 | 883 | 7 |

Ideas: **token dictionary**, transpose+sub/xor (M_XFORM), BWT+sub, expanded blocks/parts, **BAV v2 compact header** (u32 size).  
Artifacts: `gen2_freeze.json`, `gen2_win.json`, `generation_chain.json`.

Chain: **21364 → 18943 → 14895**. Active test bar remains gen2 freeze (18943).

### Default port: C# 0.3.0

- Entry: repo-root `bav.ps1` / `bav.cmd` → `ports/csharp` (`bav-csharp`)
- Full research auto (backends + transpose/xform/prefilters@SUB1–16/BWT/blocks/parts/token)
- Main-5 total **14888** (beats freeze 18943); see `progress/csharp-default-report.md`
- Python remains reference + unit tests; filter IDs aligned for cross-decode

## Evidence paths

| Artifact | Location |
|----------|----------|
| Repo / chain | `{SCRATCH}/repo-loop-goal.txt`, `generation-chain-report.json` |
| Lossless | `{SCRATCH}/lossless-tests.log` |
| Self-beat | `{SCRATCH}/beat-prior-bav-tests.log` |
| Regressions / CLI | `{SCRATCH}/regression-tests.log`, `cli-run1/2.log` |
| Frontier | `{SCRATCH}/frontier-report.json` |
