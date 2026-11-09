param(
    [Parameter(Mandatory)][string]$PowerShellArchive,
    [string]$PowerShellSHA256 = 'ce7484804c0f0ad4dda10c958e71acdaf7eebfb22505c1b480f89e0e9f1da94d'
)
$ErrorActionPreference = 'Stop'
$version = '0.4.4-alpha.4'
$root = Split-Path $PSScriptRoot
$dist = Join-Path $root '.build/windows/dist'
& (Join-Path $PSScriptRoot 'build-preview.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Candidate renderer build failed.' }
if ((Get-FileHash -LiteralPath $PowerShellArchive).Hash -ne $PowerShellSHA256) { throw 'PowerShell archive SHA-256 mismatch.' }
& (Join-Path $PSScriptRoot 'package-ime.ps1') -MSVC -Version $version
$stage = Join-Path $dist ('setup-' + [guid]::NewGuid().ToString('N'))
try {
    Expand-Archive -LiteralPath (Join-Path $dist "Harukoto-$version-windows-x64.zip") -DestinationPath $stage
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'uninstall-ime.ps1') -Destination (Join-Path $stage 'Uninstall.ps1')
    Expand-Archive -LiteralPath $PowerShellArchive -DestinationPath (Join-Path $stage 'PowerShell')
    foreach ($notice in 'LICENSE.txt','ThirdPartyNotices.txt') {
        if (!(Test-Path -LiteralPath (Join-Path $stage "PowerShell/$notice"))) { throw "Missing PowerShell notice: $notice" }
    }
    $runtime = Join-Path $stage 'PowerShell/pwsh.exe'
    if ((Get-AuthenticodeSignature -LiteralPath $runtime).Status -ne 'Valid') { throw 'PowerShell signature verification failed.' }
    & $runtime -NoProfile -File (Join-Path $stage 'Install.ps1') -Prebuilt -VerifyOnly
    if ($LASTEXITCODE -ne 0) { throw 'Embedded runtime preflight failed.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $payload = Join-Path $dist ('payload-' + [guid]::NewGuid().ToString('N') + '.zip')
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $payload, [IO.Compression.CompressionLevel]::Optimal, $false)
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    $output = Join-Path $dist "Harukoto-$version-Setup.exe"
    & $compiler /nologo /target:winexe /platform:x64 /out:$output /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /reference:System.IO.Compression.dll /reference:System.IO.Compression.FileSystem.dll "/resource:$payload,payload.zip" (Join-Path $PSScriptRoot 'Setup.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Setup build failed.' }
    $hash = (Get-FileHash -LiteralPath $output).Hash.ToLowerInvariant()
    "$hash  $(Split-Path $output -Leaf)" | Set-Content -LiteralPath (Join-Path $dist 'SETUP-SHA256SUMS.txt') -Encoding ascii
    Write-Output $output
} finally {
    if ($payload -and (Test-Path -LiteralPath $payload)) { Remove-Item -LiteralPath $payload }
    if (Test-Path -LiteralPath $stage) {
        $resolved = (Resolve-Path -LiteralPath $stage).Path
        if ([IO.Path]::GetDirectoryName($resolved) -eq [IO.Path]::GetFullPath($dist)) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    }
}
