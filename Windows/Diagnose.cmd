@echo off
if exist "%~dp0PowerShell\pwsh.exe" (
  "%~dp0PowerShell\pwsh.exe" -NoProfile -File "%~dp0diagnose-ime.ps1"
) else (
  pwsh.exe -NoProfile -File "%~dp0diagnose-ime.ps1"
)
pause
