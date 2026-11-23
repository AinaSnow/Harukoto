@echo off
rem Without arguments: open the dictionary manager window.
rem With arguments: command-line management, e.g. Dictionaries.cmd list (see docs/DICTIONARY_PACKAGES.md).
if "%~1"=="" (
    start "" "%~dp0payload\MeowpuutGlassPreview.exe" --dictionaries
    exit /b 0
)
start "" /b /wait "%~dp0payload\MeowpuutGlassPreview.exe" --dictionaries %*
exit /b %ERRORLEVEL%
