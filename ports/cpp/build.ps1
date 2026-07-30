# Build bav-cpp full research backup port (zlib + liblzma + zstd + brotli)
$ErrorActionPreference = "Stop"
$Cpp = $PSScriptRoot
$Root = Split-Path -Parent (Split-Path -Parent $Cpp)
$Gcc = if (Test-Path "C:\Users\adyba\gcc\bin\gcc.exe") { "C:\Users\adyba\gcc\bin\gcc.exe" } else { "gcc" }
$Gxx = if (Test-Path "C:\Users\adyba\gcc\bin\g++.exe") { "C:\Users\adyba\gcc\bin\g++.exe" } else { "g++" }
$Cmake = if (Test-Path "C:\Program Files\CMake\bin\cmake.exe") {
    "C:\Program Files\CMake\bin\cmake.exe"
} else { "cmake" }

$env:PATH = (Split-Path $Gcc) + ";" + $env:PATH

$Tp = Join-Path $Cpp "third_party"
$AsmTp = Join-Path $Root "ports\asm\third_party"
$ZstdInc = Join-Path $AsmTp "zstd\zstd-v1.5.6-win64\include"
$ZstdDllA = Join-Path $AsmTp "zstd\zstd-v1.5.6-win64\dll\libzstd.dll.a"
$ZstdDll = Join-Path $AsmTp "zstd\zstd-v1.5.6-win64\dll\libzstd.dll"
$BrotliRoot = Join-Path $AsmTp "brotli-src\brotli-1.1.0\c"
$XzSrc = Join-Path $Tp "xz-5.6.2"
$XzBuild = Join-Path $Tp "xz-build"
$LzmaA = Join-Path $XzBuild "liblzma.a"
$LzmaInc = Join-Path $XzSrc "src\liblzma\api"

# --- ensure xz source + liblzma ---
if (-not (Test-Path $LzmaA)) {
    Write-Host "=== building liblzma ==="
    if (-not (Test-Path $XzSrc)) {
        $tgz = Join-Path $Tp "xz-src.tar.gz"
        if (-not (Test-Path $tgz)) {
            New-Item -ItemType Directory -Force -Path $Tp | Out-Null
            Invoke-WebRequest -Uri "https://github.com/tukaani-project/xz/archive/refs/tags/v5.6.2.tar.gz" `
                -OutFile $tgz -UseBasicParsing
        }
        Push-Location $Tp
        tar -xzf xz-src.tar.gz
        Pop-Location
    }
    New-Item -ItemType Directory -Force -Path $XzBuild | Out-Null
    & $Cmake -S $XzSrc -B $XzBuild -G "MinGW Makefiles" `
        "-DCMAKE_C_COMPILER=$($Gcc -replace '\\','/')" `
        "-DCMAKE_BUILD_TYPE=Release" `
        "-DBUILD_SHARED_LIBS=OFF" `
        "-DBUILD_TESTING=OFF"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure liblzma failed" }
    & $Cmake --build $XzBuild --target liblzma -j 8
    if ($LASTEXITCODE -ne 0) { throw "liblzma build failed" }
}

if (-not (Test-Path $ZstdDllA)) { throw "missing zstd import lib: $ZstdDllA" }
if (-not (Test-Path $BrotliRoot)) { throw "missing brotli sources: $BrotliRoot" }
if (-not (Test-Path $LzmaA)) { throw "missing liblzma.a" }

Write-Host "=== brotli sources ==="
$brotliC = @(
    Get-ChildItem (Join-Path $BrotliRoot "common") -Filter *.c
    Get-ChildItem (Join-Path $BrotliRoot "enc") -Filter *.c
    Get-ChildItem (Join-Path $BrotliRoot "dec") -Filter *.c
) | ForEach-Object { $_.FullName }
Write-Host "brotli files:" $brotliC.Count

Write-Host "=== g++ link bav-cpp ==="
$out = Join-Path $Cpp "bav-cpp.exe"
$incs = @(
    "-I$Cpp",
    "-I$LzmaInc",
    "-I$ZstdInc",
    "-I$(Join-Path $BrotliRoot 'include')"
)
$defs = @("-DBAV_HAVE_ZSTD", "-DBAV_HAVE_BROTLI", "-DBAV_HAVE_LZMA")
$cfiles = @(
    (Join-Path $Cpp "bav.cpp"),
    (Join-Path $Cpp "main.cpp")
) + $brotliC

& $Gxx -O3 -std=c++17 -Wall -pthread @defs @incs @cfiles `
    $LzmaA $ZstdDllA `
    -o $out -lz -lpthread
if ($LASTEXITCODE -ne 0) { throw "g++ failed" }

if (Test-Path $ZstdDll) {
    Copy-Item $ZstdDll $Cpp -Force
}
Write-Host "Built $out"
& $out version
