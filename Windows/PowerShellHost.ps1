# Resolve once, then use the same host for elevated child processes.
function Get-MeowpuutPowerShell {
    if ($PSVersionTable.PSVersion.Major -ge 7) {
        return (Join-Path $PSHOME 'pwsh.exe')
    }
    $command = Get-Command pwsh.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in @(
        (Join-Path $env:ProgramFiles 'PowerShell/7/pwsh.exe'),
        (Join-Path $env:LOCALAPPDATA 'Microsoft/PowerShell/7/pwsh.exe')
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    throw 'PowerShell 7 (pwsh.exe) is required. Install it or add it to PATH, then retry.'
}
