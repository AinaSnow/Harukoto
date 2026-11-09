#requires -Version 7.0
param([string]$CMake = 'cmake')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$output = Join-Path $root '.build/windows/ime'
$native = Join-Path $root '.build/windows/msvc'
& python (Join-Path $PSScriptRoot 'compile-lexicon.py') $output
if ($LASTEXITCODE -ne 0) { throw 'Lexicon build failed.' }
& $CMake -S $PSScriptRoot -B $native -A x64 "-DMEOWPUUT_LEXICON_DIR=$output" "-DHARUKOTO_PWSH=$(Join-Path $PSHOME 'pwsh.exe')"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $CMake --build $native --config Release
if ($LASTEXITCODE -ne 0) { throw 'Windows build failed.' }
$ctest = Join-Path (Split-Path (Get-Command $CMake).Source) 'ctest.exe'
& $ctest --test-dir $native -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Windows tests failed.' }
& (Join-Path $native 'Release/meowpuut_mixed_candidate_test.exe') (Join-Path $output 'native-pinyin.tsv') (Join-Path $output 'japanese.tsv')
if ($LASTEXITCODE -ne 0) { throw 'Japanese bridge tests failed.' }
Copy-Item -LiteralPath (Join-Path $native 'Release/MeowpuutTip.dll') -Destination $output -Force
& (Join-Path $PSScriptRoot 'build-preview.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Renderer build failed.' }
Write-Output 'Build verified. Run Windows/install-ime.ps1 to install.'
