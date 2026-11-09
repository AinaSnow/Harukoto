@echo off
setlocal
set "MEOWPUUT_PS7="
for /f "delims=" %%P in ('where pwsh.exe 2^>nul') do if not defined MEOWPUUT_PS7 set "MEOWPUUT_PS7=%%P"
if not defined MEOWPUUT_PS7 if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" set "MEOWPUUT_PS7=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not defined MEOWPUUT_PS7 if exist "%LOCALAPPDATA%\Microsoft\PowerShell\7\pwsh.exe" set "MEOWPUUT_PS7=%LOCALAPPDATA%\Microsoft\PowerShell\7\pwsh.exe"
if not defined MEOWPUUT_PS7 (
    echo PowerShell 7 is required. Install it or add pwsh.exe to PATH.
    pause
    exit /b 1
)
"%MEOWPUUT_PS7%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" -Prebuilt
if errorlevel 1 (
    echo.
    echo Meowpuut installation failed. See the message above.
    pause
    exit /b 1
)
echo.
echo Meowpuut Pinyin is available under Traditional Chinese. Press Win+Space to switch.
pause
