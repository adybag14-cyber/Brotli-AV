# Brotli-AV (BAV)

**Mission:** Push the research compressor past its own frozen baseline — **BAV beats BAV** — on every fixed-corpus file and on the total, with lossless round-trip and scientific-frontier size/entropy reporting. Secondary regressions: pure Zstd-22 (every file + total) and Brotli q=11 (total).

Remote: [https://github.com/adybag14-cyber/Brotli-AV.git](https://github.com/adybag14-cyber/Brotli-AV.git)  
Local research root: `E:\brotli-research`

> "ADVANCE COMPUTING FOR EARTH" — ratio first, measured, reproducible.

## Default implementation: **C#**

| | |
|--|--|
| **Default CLI** | `.\bav.ps1` / `.\bav.cmd` → `ports/csharp` (`bav-csharp`) |
| **Build** | `dotnet build -c Release ports/csharp` |
| **Version** | `bav-csharp 0.3.0 (default port, full research)` |
| **Method default** | `auto` — all backends + full research paths |

```powershell
cd E:\brotli-research
dotnet build -c Release ports\csharp

# compress / decompress (auto = full research)
.\bav.ps1 compress corpus\01_plain_text.txt -o out.bav
.\bav.ps1 decompress out.bav -o restored.txt
.\bav.ps1 version

# or call the binary directly
.\ports\csharp\bin\Release\net8.0\bav-csharp.exe compress corpus\02_html_js.html -m auto
```

### Full research coverage (C# auto mode)

Same candidate families as `src/bav/codec.py` gen2+:

1. **Backends:** STORE · Deflate (zlib max) · LZMA/XZ · Zstd-22 · Brotli (optimal + smallest)  
2. **Transpose** widths 2–16 → re-pick backend  
3. **Xform** transpose + SUB/XOR → backend  
4. **Prefilters:** MTF · RLE0 · MTF+RLE0 · **SUB at distances 1,2,3,4,5,6,8,12,16** · XOR1/XOR4 → backend  
5. **BWT** (+ MTF / RLE0 / SUB1) → backend  
6. **Multi-block** adaptive backends  
7. **Parts** (2–4) with per-part transpose/xform/prefilter search  
8. **Token** dictionary + varint ids (text-like inputs)  

Wire format: BAV1 magic `BAV1`, v2 header (v1 decode still supported). Not Zstd/Brotli wire-compatible.

## Primary gate (this generation)

| Item | Definition |
|------|------------|
| Freeze | `benchmarks/prior_bav_baseline.json` (prior BAV1 auto sizes) |
| Win | New auto size **&lt; frozen** for **each** of `corpus/01_`…`05_` **and** total |
| Evidence | Self-beat harness + frontier entropy report |

## Layout

```
E:\brotli-research\
  bav.ps1 / bav.cmd     # default entry → C#
  ports/csharp/         # DEFAULT full-research compressor
  src/bav/              # Python reference (parity + experiments)
  corpus/               # fixed verification files
  benchmarks/           # config, prior freeze, run_bench, run_frontier
  tests/                # lossless, self-beat, zstd/brotli regressions, CLI
  ports/                # other language ports (Rust, ASM, …)
  research/ progress/ third_party/
```

## Python reference (optional)

```powershell
python -m pip install -r requirements.txt
python -m pip install -e .
$env:PYTHONPATH = "E:\brotli-research\src"
python -m bav compress corpus\01_plain_text.txt -m auto
```

Python remains the research **reference** and unit-test host; **C# is the default shipped compressor**.

## Benchmarks & frontier

```powershell
# Self-beat vs frozen prior-BAV (Python harness; sizes comparable to C# auto)
python benchmarks\run_bench.py -o progress\benchmark-beat-bav-report.json

# Entropy / residual-gap frontier report
python benchmarks\run_frontier.py -o progress\frontier-report.json

# Multi-language speed (C# listed as default port)
python benchmarks\run_lang_bench.py --runs 2 -m auto

python -m unittest discover -s tests -v
```

## Other ports

See `ports/README.md`. Notable: Rust (parity), pure NASM lzma/zstd/brotli (`ports/asm`), Zig, C/C++.

## Tests

- `test_lossless.py` — corpus + edges round-trip (Python)  
- `test_beat_prior_bav.py` — **primary** self-beat vs freeze  
- `test_beat_zstd22.py` / `test_beat_brotli.py` — secondary regressions  
- `test_cli.py` — `python -m bav`  
- C#: build + corpus compress/decompress (see `progress/csharp-default-report.md`)  

Entropy columns in the frontier report are Shannon reference bounds — **not** Kolmogorov optimality claims.
