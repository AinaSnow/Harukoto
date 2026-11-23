#requires -Version 7.0
# UPD-01: update check logic with fake responses and a fake clock; no network, HKCU test key only.
$ErrorActionPreference = 'Stop'
$windows = Split-Path $PSScriptRoot
$folder = Join-Path ([IO.Path]::GetTempPath()) ('harukoto-update-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder | Out-Null
try {
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe'
    $exe = Join-Path $folder 'UpdateCheckTests.exe'
    & $compiler /nologo /target:exe /out:$exe (Join-Path $windows 'UpdateCheck.cs') (Join-Path $PSScriptRoot 'UpdateCheckTests.cs')
    if ($LASTEXITCODE -ne 0) { throw 'UpdateCheck test build failed.' }
    & $exe $windows
    if ($LASTEXITCODE -ne 0) { throw 'UpdateCheck tests failed.' }
} finally {
    if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
}
