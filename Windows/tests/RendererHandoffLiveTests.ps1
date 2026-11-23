#requires -Version 7.0
param([Parameter(Mandatory)][string]$Runner)
# RH-5: Compiles a fresh MeowpuutGlassPreview.exe in an isolated temporary
# folder and executes harukoto_renderer_handoff_live_test against it.
# Never writes HKLM, Program Files, or .build/windows/MeowpuutGlassPreview.exe.
$ErrorActionPreference = 'Stop'
$windows = Split-Path $PSScriptRoot
$folder = Join-Path ([IO.Path]::GetTempPath()) ('harukoto-rh-live-build-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder | Out-Null
try {
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe'
    $wpf = Join-Path (Split-Path $compiler) 'WPF'
    $references = 'WindowsBase', 'PresentationCore', 'PresentationFramework', 'UIAutomationTypes' |
        ForEach-Object { '/reference:' + (Join-Path $wpf "$_.dll") }
    $layout = Join-Path $windows 'GlassPreview.xaml'
    $candidateLayout = Join-Path $windows 'CandidateWindow.xaml'
    $exe = Join-Path $folder 'MeowpuutGlassPreview.exe'
    & $compiler /nologo /target:winexe /out:$exe $references /reference:System.Xaml.dll /reference:System.Drawing.dll `
        "/resource:$layout,GlassPreview.xaml" "/resource:$candidateLayout,CandidateWindow.xaml" `
        (Join-Path $windows 'GlassPreview.cs') (Join-Path $windows 'CandidatePayload.cs') `
        (Join-Path $windows 'GlassBackdrop.cs') (Join-Path $windows 'CandidateLayout.cs') `
        (Join-Path $windows 'LayoutOptions.cs') (Join-Path $windows 'CandidateView.cs') `
        (Join-Path $windows 'CandidateTheme.cs') (Join-Path $windows 'CandidatePreview.cs') `
        (Join-Path $windows 'CandidateSettings.cs') (Join-Path $windows 'DictionaryLauncher.cs') `
        (Join-Path $windows 'DictionaryManager.cs') (Join-Path $windows 'ThemePackages.cs') `
        (Join-Path $windows 'DictionaryCli.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Renderer build for live handoff test failed.' }
    & $Runner $exe
    if ($LASTEXITCODE -ne 0) { throw "Renderer handoff live test failed with exit code $LASTEXITCODE." }
} finally {
    if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
}
