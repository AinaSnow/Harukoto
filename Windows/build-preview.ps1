$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe'
$output = Join-Path $PSScriptRoot '../.build/windows'
New-Item -ItemType Directory -Force $output | Out-Null
$exe = Join-Path $output 'MeowpuutGlassPreview.exe'
$wpf = Join-Path (Split-Path $compiler) 'WPF'
$references = 'WindowsBase', 'PresentationCore', 'PresentationFramework', 'UIAutomationTypes' |
    ForEach-Object { '/reference:' + (Join-Path $wpf "$_.dll") }
$layout = Join-Path $PSScriptRoot 'GlassPreview.xaml'
$candidateLayout = Join-Path $PSScriptRoot 'CandidateWindow.xaml'
& $compiler /nologo /target:winexe /out:$exe $references /reference:System.Xaml.dll /reference:System.Drawing.dll "/resource:$layout,GlassPreview.xaml" "/resource:$candidateLayout,CandidateWindow.xaml" (Join-Path $PSScriptRoot 'GlassPreview.cs') (Join-Path $PSScriptRoot 'GlassBackdrop.cs') (Join-Path $PSScriptRoot 'CandidateLayout.cs') (Join-Path $PSScriptRoot 'CandidateView.cs') (Join-Path $PSScriptRoot 'CandidatePreview.cs') (Join-Path $PSScriptRoot 'CandidateSettings.cs') (Join-Path $PSScriptRoot 'DictionaryLauncher.cs')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($source in (Join-Path $PSScriptRoot 'dictionary-manager.pyw'),(Join-Path $PSScriptRoot '../scripts/japanese_packages.py'),(Join-Path $PSScriptRoot '../scripts/japanese_dictionary.py')) {
    Copy-Item -LiteralPath $source -Destination $output -Force
}
Write-Output (Resolve-Path $exe).Path
$ff14 = Join-Path $PSScriptRoot '../Resources/DictionaryPackages/ff14/full'
Copy-Item -LiteralPath (Join-Path $ff14 'manifest.json') -Destination (Join-Path $output 'ff14-manifest.json') -Force
Copy-Item -LiteralPath (Join-Path $ff14 'ff14-words.txt') -Destination $output -Force
Copy-Item -LiteralPath (Join-Path $ff14 'reviewed-source.json') -Destination (Join-Path $output 'ff14-reviewed-source.json') -Force
