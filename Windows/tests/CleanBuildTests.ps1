$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
$build = (Resolve-Path -LiteralPath (Join-Path $repo '.build')).Path
$case = Join-Path $build ('cleanup-test-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path (Join-Path $case 'Windows'),(Join-Path $case '.build/windows/dist'),(Join-Path $case '.build/windows/native') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'Windows/clean-build.ps1') -Destination (Join-Path $case 'Windows/clean-build.ps1')
    & git -C $case init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'test git init failed' }
    $script = Join-Path $case 'Windows/clean-build.ps1'
    $dist = Join-Path $case '.build/windows/dist'
    foreach ($version in @(1,2,3)) {
        Set-Content -LiteralPath (Join-Path $dist "Harukoto-0.4.4-alpha.$version-Setup.exe") -Value 'fixture'
    }
    $old = Join-Path $dist 'Harukoto-0.4.4-alpha.1-Setup.exe'
    $unknown = Join-Path $dist 'user-notes.txt'
    Set-Content -LiteralPath $unknown -Value 'keep'
    & $script | Out-Null
    if (!(Test-Path -LiteralPath $old)) { throw 'preview deleted a file' }
    $tracked = Join-Path $case '.build/windows/native/tracked.txt'
    Set-Content -LiteralPath $tracked -Value 'tracked'
    & git -C $case add -- .build/windows/native/tracked.txt
    if ($LASTEXITCODE -ne 0) { throw 'test git add failed' }
    $blocked = $false
    try { & $script -Apply | Out-Null } catch { $blocked = $true }
    if (!$blocked -or !(Test-Path -LiteralPath $old) -or !(Test-Path -LiteralPath $tracked)) { throw 'tracked file protection failed' }
    & git -C $case rm --cached --quiet -- .build/windows/native/tracked.txt
    if ($LASTEXITCODE -ne 0) { throw 'test git untrack failed' }
    & $script -Apply | Out-Null
    if (Test-Path -LiteralPath $old) { throw 'old installer retained' }
    foreach ($name in @('Harukoto-0.4.4-alpha.2-Setup.exe','Harukoto-0.4.4-alpha.3-Setup.exe','user-notes.txt')) {
        if (!(Test-Path -LiteralPath (Join-Path $dist $name))) { throw "retained file removed: $name" }
    }
    & $script -Apply | Out-Null
    Write-Output 'PASS: preview, latest-two retention, unknown file preservation, tracked-file fail-before-delete, repeated cleanup'
} finally {
    if (Test-Path -LiteralPath $case) {
        $resolved = (Resolve-Path -LiteralPath $case).Path
        if (!$resolved.StartsWith($build + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
            (Get-Item -LiteralPath $resolved).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'unsafe test cleanup path' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
