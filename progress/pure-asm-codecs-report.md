# Pure hand-written NASM zstd / brotli / lzma

**Date:** 2026-07-30  
**Binary:** `ports/asm/bav-asm.exe` **0.3.0**

## What shipped

| Codec | File | Magic | Notes |
|-------|------|-------|-------|
| LZMA-family | `ports/asm/pure/pure_lzma.asm` | `pLZ1` | 3-byte hash, min-match 3, chain 48 |
| Zstd-family | `ports/asm/pure/pure_zstd.asm` | `pZS1` | 4-byte hash, min-match 4, chain 64 |
| Brotli-family | `ports/asm/pure/pure_brotli.asm` | `pBR1` | 3-byte hash, min-match 3, chain 56 |

- **No** libzstd, libbrotli, or xz/lzma libraries linked for these backends.
- Encode + decode are pure Win64 NASM (Microsoft x64 ABI).
- C glue only: malloc/hash workspace (`pure_glue.c`), BAV orchestration, zlib still used for **deflate** only.
- Wire format: 4-byte magic + u32le uncompressed size + MSB-first bit-packed LZ77 tokens  
  `(is_match, len-MIN or literal, dist)`.

## Build

```powershell
cd ports/asm
powershell -File build.ps1
```

Builds `bav-asm.exe` and runs `test_pure.exe` smoke round-trips.

## Smoke tests

All pure codecs: empty, 1 byte, short text, repeated phrase, 4 KiB synthetic — **ALL OK**.

## Corpus (BAV auto: store/deflate/lzma/zstd/brotli + research transforms)

| file | raw | bav | ratio | RT |
|------|-----|-----|-------|-----|
| 01_plain_text.txt | 81601 | 16372 | 0.20 | OK |
| 02_html_js.html | 35093 | 1317 | 0.04 | OK |
| 03_binary_records.bin | 30512 | 760 | 0.02 | OK |
| 04_mixed_archive.bin | 12075 | 4371 | 0.36 | OK |
| 05_source_code.py | 58780 | 911 | 0.02 | OK |
| edge_small.txt | 54 | 44 | 0.81 | OK |
| edge_tiny.txt | 1 | 16 | 16.0 | OK |
| **TOTAL** | **218116** | **23791** | **0.109** | **OK** |

Encode wall ~1.0 s total on this set (hash-chain in NASM; not heavily optimized).

## Vs prior ports (same corpus totals, approximate)

| Port | Total compressed | Notes |
|------|------------------|-------|
| Python BAV gen2 freeze | ~14888 | full research + real backends |
| Rust full parity | ~14895 | flate2/xz2/zstd/brotli crates |
| C# full parity | ~14908 | ZstdSharp/XZ.NET/BrotliStream |
| ASM-first (C libs) | ~mid 15k class | zlib + libzstd + brotli C |
| **Pure NASM 0.3.0** | **23791** | pure LZ77 bit-pack; weaker entropy |

Pure NASM is intentionally **library-free**. Ratio is worse than real Brotli/Zstd/LZMA entropy (no FSE/Huffman/range yet), but:

1. Correct self-contained encode/decode in hand-written assembly  
2. Strong LZ77 on repetitive data (html/source/binary records)  
3. Integrated as BAV backends with `M_LZMA` added  

## Layout

```
ports/asm/pure/
  pure_codecs.h
  pure_lzma.asm
  pure_zstd.asm
  pure_brotli.asm
  pure_glue.c
  test_pure.c
ports/asm/
  backends.c    # pure backends + zlib deflate
  build.ps1     # no brotli/zstd C sources
```

## Next upgrades (optional)

1. Real range coder (LZMA) / FSE (Zstd) / context prefix (Brotli) on the same token stream  
2. Longer window + binary-tree match finder  
3. Pure NASM deflate to drop zlib  
4. Size race vs C# once entropy matches real formats  
