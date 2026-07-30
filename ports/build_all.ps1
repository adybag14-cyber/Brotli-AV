# Build all BAV language ports on Windows.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = "E:\brotli-research" }
$Ports = Join-Path $Root "ports"
$Zig = $env:BAV_ZIG
if (-not $Zig) {
    $Zig = "C:\Users\adyba\zig-from-fork\zig-x86_64-windows-0.17.0-dev.1476+91a29d707\zig.exe"
}
$Gcc = "C:\Users\adyba\gcc\bin\gcc.exe"
$Gxx = "C:\Users\adyba\gcc\bin\g++.exe"
$Nasm = "C:\Users\adyba\AppData\Local\Programs\NASM\nasm-3.01\nasm.exe"
if (-not (Test-Path $Nasm)) { $Nasm = "nasm" }
if (-not (Test-Path $Gcc)) { $Gcc = "gcc" }
if (-not (Test-Path $Gxx)) { $Gxx = "g++" }

Write-Host "=== C# (DEFAULT full-research port) ==="
Push-Location (Join-Path $Ports "csharp")
dotnet build -c Release 2>&1
if ($LASTEXITCODE -ne 0) { throw "csharp build failed" }
& (Join-Path $Ports "csharp\bin\Release\net8.0\bav-csharp.exe") version
Pop-Location

Write-Host "=== Rust ==="
Push-Location (Join-Path $Ports "rust")
cargo build --release 2>&1
if ($LASTEXITCODE -ne 0) { throw "rust build failed" }
Pop-Location

Write-Host "=== C (plain + NASM) ==="
Push-Location (Join-Path $Ports "c")
& $Gcc -O3 -Wall -o bav-c.exe bav.c main.c -lz
if ($LASTEXITCODE -ne 0) { throw "c build failed" }
& $Nasm -f win64 -o bav_kernels.obj (Join-Path $Ports "nasm\bav_kernels.asm")
if ($LASTEXITCODE -ne 0) { throw "nasm assemble failed" }
& $Gcc -O3 -Wall -DBAV_USE_NASM -o bav-c-nasm.exe bav.c main.c bav_kernels.obj -lz
if ($LASTEXITCODE -ne 0) { throw "c+nasm build failed" }
Pop-Location

Write-Host "=== C++ (plain + NASM) ==="
Push-Location (Join-Path $Ports "cpp")
& $Gxx -O3 -std=c++17 -Wall -o bav-cpp.exe bav.cpp main.cpp -lz
if ($LASTEXITCODE -ne 0) { throw "cpp build failed" }
Copy-Item (Join-Path $Ports "c\bav_kernels.obj") . -Force
& $Gxx -O3 -std=c++17 -Wall -DBAV_USE_NASM -o bav-cpp-nasm.exe bav.cpp main.cpp bav_kernels.obj -lz
if ($LASTEXITCODE -ne 0) { throw "cpp+nasm build failed" }
Pop-Location

Write-Host "=== Zig (canonical adybag14-cyber/zig) ==="
if (-not (Test-Path $Zig)) { throw "BAV_ZIG not found: $Zig" }
Push-Location (Join-Path $Ports "zig")
& $Zig version
& $Zig build -Doptimize=ReleaseFast 2>&1
if ($LASTEXITCODE -ne 0) { throw "zig build failed" }
Pop-Location

Write-Host "=== Ruby ==="
$ruby = Get-Command ruby -ErrorAction SilentlyContinue
if ($ruby) {
    ruby (Join-Path $Ports "ruby\bav.rb") version
} else {
    Write-Host "ruby not on PATH — install later"
}

Write-Host "All builds done."
