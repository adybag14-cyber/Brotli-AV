# Zig toolchain for Zig BAV port

Use the **adybag14-cyber/zig** release toolchain only (not stock ziglang.org as the sole build compiler).

## Recommended binary (Windows x86_64)

- Release: https://github.com/adybag14-cyber/zig/releases/tag/latest-master
- Asset: `zig-x86_64-windows-latest-master.zip`
- Local install used for this branch:
  `C:\Users\adyba\zig-from-fork\zig-x86_64-windows-0.17.0-dev.1476+91a29d707\zig.exe`
- Version: `0.17.0-dev.1476+91a29d707`
- Upstream commit: `91a29d7074a61ba192fcefb351a10a26d60b85c8`

## Build Zig BAV

```powershell
$env:BAV_ZIG = 'C:\Users\adyba\zig-from-fork\zig-x86_64-windows-0.17.0-dev.1476+91a29d707\zig.exe'
& $env:BAV_ZIG build -C ports/zig -Doptimize=ReleaseFast
```

Or set `BAV_ZIG` / pass the full path in the multi-lang harness (`benchmarks/run_lang_bench.py`).

## Bootstrap host fix

Host zig2 failed CI with undeclared `sinl`/`cosl` while compiling generated `compiler_rt.c`.
Fix lands in https://github.com/adybag14-cyber/zig `scripts/build-zig-bootstrap-group.sh`:
`CMAKE_C_FLAGS=-include math.h` for the host zig cmake configure.
