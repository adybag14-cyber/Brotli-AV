# Default BAV entry: C# (main). C++ is the full-research backup.
# Usage:
#   .\bav.ps1 compress <in> [-o out] [-m auto|...]
#   .\bav.ps1 decompress <in> [-o out]
#   .\bav.ps1 version
#   .\bav.ps1 -impl csharp|cpp ...
# Env: BAV_IMPL=csharp|cpp  (default csharp)
$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

$impl = $env:BAV_IMPL
if (-not $impl) { $impl = "csharp" }

# optional leading -impl flag
$argList = [System.Collections.Generic.List[string]]::new()
for ($i = 0; $i -lt $args.Count; $i++) {
    if ($args[$i] -eq "-impl" -and ($i + 1) -lt $args.Count) {
        $impl = $args[$i + 1]
        $i++
        continue
    }
    $argList.Add([string]$args[$i])
}

$impl = $impl.ToLowerInvariant()
if ($impl -eq "cpp" -or $impl -eq "c++" -or $impl -eq "backup") {
    $Exe = Join-Path $Root "ports\cpp\bav-cpp.exe"
    if (-not (Test-Path $Exe)) {
        Write-Host "Building C++ backup port..."
        powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root "ports\cpp\build.ps1")
    }
    if (-not (Test-Path $Exe)) { throw "bav-cpp.exe missing — run ports/cpp/build.ps1" }
    # zstd dll next to exe
    $dll = Join-Path $Root "ports\cpp\libzstd.dll"
    if (-not (Test-Path $dll)) {
        $src = Join-Path $Root "ports\asm\third_party\zstd\zstd-v1.5.6-win64\dll\libzstd.dll"
        if (Test-Path $src) { Copy-Item $src (Split-Path $Exe) -Force }
    }
} else {
    $impl = "csharp"
    $Exe = Join-Path $Root "ports\csharp\bin\Release\net8.0\bav-csharp.exe"
    if (-not (Test-Path $Exe)) {
        Write-Host "Building default C# port..."
        Push-Location (Join-Path $Root "ports\csharp")
        try {
            dotnet build -c Release --nologo -v q
            if ($LASTEXITCODE -ne 0) { throw "dotnet build failed" }
        } finally { Pop-Location }
    }
    if (-not (Test-Path $Exe)) {
        throw "bav-csharp.exe not found — run: dotnet build -c Release ports/csharp"
    }
}

& $Exe @argList
exit $LASTEXITCODE
