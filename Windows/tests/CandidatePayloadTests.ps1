#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$windows = Split-Path $PSScriptRoot
$folder = Join-Path ([IO.Path]::GetTempPath()) ('harukoto-payload-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder | Out-Null
try {
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe'
    $exe = Join-Path $folder 'CandidatePayloadTests.exe'
    & $compiler /nologo /target:exe /out:$exe (Join-Path $windows 'CandidatePayload.cs') (Join-Path $PSScriptRoot 'CandidatePayloadTests.cs')
    if ($LASTEXITCODE -ne 0) { throw 'CandidatePayload test build failed.' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'CandidatePayload tests failed.' }
} finally {
    if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
}
