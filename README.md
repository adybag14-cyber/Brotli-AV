# Brotli-AV (BAV)

**Mission:** Push the research compressor past its own frozen baseline — **BAV beats BAV** — on every fixed-corpus file and on the total, with lossless round-trip and scientific-frontier size/entropy reporting. Secondary regressions: pure Zstd-22 (every file + total) and Brotli q=11 (total).

Remote: [https://github.com/adybag14-cyber/Brotli-AV.git](https://github.com/adybag14-cyber/Brotli-AV.git)  
Local research root: `E:\brotli-research`

> "ADVANCE COMPUTING FOR EARTH" — ratio first, measured, reproducible.

## Ports: C# main · C++ backup

| Role | Impl | CLI | Version |
|------|------|-----|---------|
| **Main** | C# | `.\bav.ps1` → `ports/csharp` | `bav-csharp 0.3.0` |
| **Backup** | C++ | `.\bav.ps1 -impl cpp` or `.\bav-cpp.ps1` | `bav-cpp 0.3.0` |
| Reference | Python | `python -m bav` | research / unit tests |

```powershell
cd E:\brotli-research

# Main (C#)
dotnet build -c Release ports\csharp
.\bav.ps1 compress corpus\01_plain_text.txt -o out.bav
.\bav.ps1 decompress out.bav -o restored.txt
.\bav.ps1 version

# Backup (C++ full research — zlib + liblzma + zstd + brotli)
powershell -File ports\cpp\build.ps1
.\bav.ps1 -impl cpp compress corpus\02_html_js.html -m auto
.\bav-cpp.ps1 version
```

Both main and backup implement **full research auto**:

1. **Backends:** STORE · Deflate-9 · LZMA/XZ (+ delta distance search on C++/Python) · Zstd-22 · Brotli-11  
2. **Transpose** widths 2–16  
3. **Xform** transpose + SUB/XOR  
4. **Prefilters:** MTF · RLE0 · MTF+RLE0 · SUB@1,2,3,4,5,6,8,12,16 · XOR1/XOR4  
5. **BWT** (+ MTF / RLE0 / SUB1)  
6. **Multi-block** · **Parts** · **Token**  

Wire: BAV1 magic `BAV1`, v2 header (v1 decode supported). Not Zstd/Brotli wire-compatible.

## Primary gate

| Item | Definition |
|------|------------|
| Freeze | `benchmarks/prior_bav_baseline.json` |
| Win | Auto size **&lt; frozen** for each of `corpus/01_`…`05_` **and** total |
| Evidence | Self-beat harness + frontier entropy report |

## Layout

```
E:\brotli-research\
  bav.ps1 / bav.cmd      # main → C#
  bav-cpp.ps1            # backup → C++
  ports/csharp/          # MAIN full-research compressor
  ports/cpp/             # BACKUP full-research compressor
  src/bav/               # Python reference + tests
  corpus/ benchmarks/ tests/ ports/ research/ progress/
```

## Python reference

```powershell
python -m pip install -r requirements.txt
python -m pip install -e .
$env:PYTHONPATH = "E:\brotli-research\src"
python -m bav compress corpus\01_plain_text.txt -m auto
```

## Benchmarks

```powershell
python benchmarks\run_bench.py -o progress\benchmark-beat-bav-report.json
python benchmarks\run_frontier.py -o progress\frontier-report.json
python benchmarks\run_lang_bench.py --runs 2 -m auto
python -m unittest discover -s tests -v
```

## Other ports

See `ports/README.md` (Rust, pure NASM, Zig, C, Ruby).
