#requires -Version 7.0
param(
    [ValidateRange(14,32)][int]$FontSize,
    [ValidateRange(480,1000)][int]$Width,
    [switch]$Reset
)
$ErrorActionPreference = 'Stop'
if (!('HarukotoAppearanceBridge' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'LayoutOptions.cs'),(Join-Path $PSScriptRoot 'AppearanceCli.cs')
}
$snapshot = [HarukotoAppearanceBridge]::Update('Software\Meowpuut\CandidateWindow', $FontSize, $Width, $Reset.IsPresent)
Write-Output ('布局配置：' + $snapshot)
Write-Output '下次输入按键时生效。Reset 只恢复默认布局，保留简繁和主题词库。'
