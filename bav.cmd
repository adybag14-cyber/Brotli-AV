@echo off
REM Default BAV entry point -> C# full-research port
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0bav.ps1" %*
