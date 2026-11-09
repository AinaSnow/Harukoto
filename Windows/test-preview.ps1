$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build-preview.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Renderer build failed.' }
$output = Join-Path $PSScriptRoot '../.build/windows'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe'
$wpf = Join-Path (Split-Path $compiler) 'WPF'
$references = 'WindowsBase','PresentationCore','PresentationFramework' |
    ForEach-Object { '/reference:' + (Join-Path $wpf "$_.dll") }
$test = Join-Path $output 'CandidateLayoutTests.exe'
& $compiler /nologo /target:exe /out:$test $references /reference:System.Xaml.dll (Join-Path $PSScriptRoot 'tests/CandidateLayoutTests.cs')
if ($LASTEXITCODE -ne 0) { throw 'Layout test build failed.' }
& $test (Join-Path $output 'MeowpuutGlassPreview.exe') (Join-Path $output 'candidate-layout.png')
if ($LASTEXITCODE -ne 0) { throw 'Layout tests failed.' }
$settingsTest = Join-Path $output 'LayoutOptionsTests.exe'
& $compiler /nologo /target:exe /out:$settingsTest (Join-Path $PSScriptRoot 'LayoutOptions.cs') (Join-Path $PSScriptRoot 'AppearanceCli.cs') (Join-Path $PSScriptRoot 'tests/LayoutOptionsTests.cs')
if ($LASTEXITCODE -ne 0) { throw 'Layout settings test build failed.' }
& $settingsTest
if ($LASTEXITCODE -ne 0) { throw 'Layout settings tests failed.' }
