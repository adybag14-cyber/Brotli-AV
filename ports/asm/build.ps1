# Build bav-asm: NASM kernels + pure NASM lzma/zstd/brotli + zlib deflate only
$ErrorActionPreference = "Stop"
$Asm = $PSScriptRoot
$Gcc = if (Test-Path "C:\Users\adyba\gcc\bin\gcc.exe") { "C:\Users\adyba\gcc\bin\gcc.exe" } else { "gcc" }
$Nasm = if (Test-Path "C:\Users\adyba\AppData\Local\Programs\NASM\nasm-3.01\nasm.exe") {
    "C:\Users\adyba\AppData\Local\Programs\NASM\nasm-3.01\nasm.exe"
} else { "nasm" }

Write-Host "=== NASM kernels + pure codecs ==="
$asmFiles = @(
    @{ src = "kernels.asm"; obj = "kernels.obj" },
    @{ src = "pure\pure_lzma.asm"; obj = "pure_lzma.obj" },
    @{ src = "pure\pure_zstd.asm"; obj = "pure_zstd.obj" },
    @{ src = "pure\pure_brotli.asm"; obj = "pure_brotli.obj" }
)
$objs = @()
foreach ($a in $asmFiles) {
    $src = Join-Path $Asm $a.src
    $obj = Join-Path $Asm $a.obj
    Write-Host "  nasm $($a.src)"
    & $Nasm -f win64 -o $obj $src
    if ($LASTEXITCODE -ne 0) { throw "nasm failed: $($a.src)" }
    $objs += $obj
}

Write-Host "=== gcc compile/link (no libzstd/libbrotli) ==="
$out = Join-Path $Asm "bav-asm.exe"
$cfiles = @(
    (Join-Path $Asm "backends.c"),
    (Join-Path $Asm "codec.c"),
    (Join-Path $Asm "main.c"),
    (Join-Path $Asm "pure\pure_glue.c")
)
& $Gcc -O3 -std=c11 -Wall -I$Asm @(
    $cfiles + $objs + @("-o", $out, "-lz")
)
if ($LASTEXITCODE -ne 0) { throw "gcc failed" }

Write-Host "Built $out"
& $out version

Write-Host "=== pure codec smoke test ==="
$testOut = Join-Path $Asm "test_pure.exe"
& $Gcc -O2 -std=c11 -Wall -I$Asm `
    (Join-Path $Asm "pure\test_pure.c") `
    (Join-Path $Asm "pure\pure_glue.c") `
    $objs `
    -o $testOut
if ($LASTEXITCODE -ne 0) { throw "test_pure build failed" }
& $testOut
if ($LASTEXITCODE -ne 0) { throw "test_pure failed" }
