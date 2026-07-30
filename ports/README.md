# Multi-language BAV1 compressor ports

**Default production port: C#** (`ports/csharp`) — full research coverage, invoked via repo-root `bav.ps1` / `bav.cmd`.

Python (`src/bav`) remains the ratio/experiment reference and unit-test host.

| Port | Path | Build | Role |
|------|------|-------|------|
| **C# (default)** | `ports/csharp` | `dotnet build -c Release` | Full research + multi-backend; `bav-csharp` |
| Python | `src/bav` | `pip install -e .` | Reference / tests |
| Rust | `ports/rust` | `cargo build --release` | Full parity |
| Pure ASM | `ports/asm` | `powershell -File build.ps1` | NASM transforms + pure NASM lzma/zstd/brotli |
| Zig | `ports/zig` | `$BAV_ZIG build -Doptimize=ReleaseFast` | See `ZIG_TOOLCHAIN.md` |
| C / C++ | `ports/c`, `ports/cpp` | gcc/g++ + optional NASM | zlib + research transforms |
| Ruby | `ports/ruby` | `ruby bav.rb` | stdlib zlib subset |

## C# full research (default)

```powershell
dotnet build -c Release ports\csharp
..\bav.ps1 compress corpus\01_plain_text.txt -m auto
# or:
.\ports\csharp\bin\Release\net8.0\bav-csharp.exe version
```

Auto mode families: store, deflate, lzma/xz, zstd-22, brotli, transpose, xform, prefilters (MTF/RLE0/SUB@1–16/XOR), BWT, multi-block, parts, token.

Packages: `ZstdSharp.Port`, `XZ.NET`; Brotli via `System.IO.Compression.BrotliStream`.

## Semantics

BAV1/BAV2 header: magic `BAV1`, method, flags, orig size, CRC32.  
Backends: STORE, DEFLATE, LZMA, ZSTD, BROTLI.  
Research: transpose, xform, prefilter, BWT, blocks, parts, token (language-dependent completeness).

## Speed harness

```powershell
$env:PYTHONPATH = "E:\brotli-research\src"
python benchmarks/run_lang_bench.py --runs 2 -m auto
```

Reports: `progress/lang-speed-report.{json,txt}`.

## One-shot Windows build

```powershell
powershell -File ports/build_all.ps1
```

Build order starts with **C#** (default), then Rust and the rest.
