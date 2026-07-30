# Default BAV entry point: C# full-research port (bav-csharp).
# Usage: .\bav.ps1 compress <in> [-o out] [-m auto|...]
#        .\bav.ps1 decompress <in> [-o out]
#        .\bav.ps1 version
$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$Exe = Join-Path $Root "ports\csharp\bin\Release\net8.0\bav-csharp.exe"
if (-not (Test-Path $Exe)) {
    Write-Host "Building default C# port..."
    Push-Location (Join-Path $Root "ports\csharp")
    try {
        dotnet build -c Release --nologo -v q
        if ($LASTEXITCODE -ne 0) { throw "dotnet build failed" }
    } finally {
        Pop-Location
    }
}
if (-not (Test-Path $Exe)) {
    throw "bav-csharp.exe not found at $Exe — run: dotnet build -c Release ports/csharp"
}
& $Exe @args
exit $LASTEXITCODE
