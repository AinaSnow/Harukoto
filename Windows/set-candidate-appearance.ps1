param(
    [ValidateRange(14,32)][int]$FontSize,
    [ValidateRange(480,1000)][int]$Width,
    [switch]$Reset
)
$ErrorActionPreference = 'Stop'
$key = 'HKCU:/Software/Meowpuut/CandidateWindow'
New-Item -Path $key -Force | Out-Null
if ($Reset) { $FontSize = 18; $Width = 672 }
if ($FontSize) { New-ItemProperty -LiteralPath $key -Name FontSize -Value $FontSize -PropertyType DWord -Force | Out-Null }
if ($Width) { New-ItemProperty -LiteralPath $key -Name Width -Value $Width -PropertyType DWord -Force | Out-Null }
Get-ItemProperty -LiteralPath $key | Select-Object FontSize,Width
Write-Output 'Settings take effect on the next composition update. Defaults: FontSize 18, Width 672.'
