@echo off
rem Harukoto candidate diagnostics (DICT-02a), read-only. No arguments: interactive mode.
"%~dp0harukoto_candidate_explain.exe" %*
if "%~1"=="" pause
