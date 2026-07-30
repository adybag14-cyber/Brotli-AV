# Multi-language BAV1 compressor ports

| Role | Port | Path | Build |
|------|------|------|-------|
| **Main** | **C#** | `ports/csharp` | `dotnet build -c Release` → `bav-csharp` |
| **Backup** | **C++** | `ports/cpp` | `powershell -File ports/cpp/build.ps1` → `bav-cpp` |
| Reference | Python | `src/bav` | `pip install -e .` |
| Other | Rust, pure ASM, Zig, C, Ruby | under `ports/` | see below |

Repo-root entry:

```powershell
.\bav.ps1 compress corpus\01_plain_text.txt -m auto   # C# main
.\bav.ps1 -impl cpp compress corpus\01_plain_text.txt # C++ backup
.\bav-cpp.ps1 version                                 # shortcut to C++
```

Env: `BAV_IMPL=csharp|cpp`.

## C# main (default)

Full research auto: store, deflate, lzma/xz, zstd-22, brotli, transpose, xform,  
prefilters (MTF/RLE0/SUB@1–16/XOR), BWT, multi-block, parts, token.

```powershell
dotnet build -c Release ports\csharp
..\bav.ps1 version
# bav-csharp 0.3.0 (default port, full research)
```

Packages: `ZstdSharp.Port`, `XZ.NET`; Brotli via BCL.

## C++ backup (full research)

Same candidate families as C#/Python, including **XZ delta+LZMA2 distance search**  
via liblzma. Parallel research families (`std::thread`).

```powershell
powershell -File ports\cpp\build.ps1
.\ports\cpp\bav-cpp.exe version
# bav-cpp 0.3.0 (backup port, full research)
```

Deps (fetched/built by `build.ps1`):

- zlib (`-lz`)
- liblzma (xz 5.6.2 source → static `liblzma.a`)
- zstd (import lib + DLL from `ports/asm/third_party/zstd`)
- brotli 1.1.0 sources from `ports/asm/third_party/brotli-src`

## Other ports

| Port | Path | Build |
|------|------|-------|
| Python | `src/bav` | `pip install -e .` |
| Rust | `ports/rust` | `cargo build --release` |
| Pure ASM | `ports/asm` | `powershell -File build.ps1` |
| Zig | `ports/zig` | `$BAV_ZIG build -Doptimize=ReleaseFast` |
| C | `ports/c` | gcc + zlib (subset) |
| Ruby | `ports/ruby` | `ruby bav.rb` |

## Semantics

BAV1/v2 header: magic `BAV1`, method, flags, orig size, CRC32.  
Backends: STORE, DEFLATE, LZMA, ZSTD, BROTLI.  
Research: transpose, xform, prefilter, BWT, blocks, parts, token.

## Speed harness

```powershell
python benchmarks/run_lang_bench.py --runs 2 -m auto
```

## One-shot Windows build

```powershell
powershell -File ports/build_all.ps1
```

Order: **C# main → C++ backup → Rust → …**
