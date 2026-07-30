# C++ backup BAV entry (full research). Main default is .\bav.ps1 (C#).
$ErrorActionPreference = "Stop"
& "$PSScriptRoot\bav.ps1" -impl cpp @args
exit $LASTEXITCODE
