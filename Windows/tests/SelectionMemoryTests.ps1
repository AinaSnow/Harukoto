#requires -Version 7.0
# manage-selection-memory.ps1 against a temporary file and registry key only.
$ErrorActionPreference = 'Stop'
$tool = Join-Path (Split-Path $PSScriptRoot) 'manage-selection-memory.ps1'
$folder = Join-Path ([IO.Path]::GetTempPath()) ('harukoto-memory-' + [guid]::NewGuid().ToString('N'))
$registry = 'HKCU:\Software\HarukotoSelectionMemoryToolTest-' + $PID
New-Item -ItemType Directory -Path $folder | Out-Null
$path = Join-Path $folder 'selection-memory.tsv'
$common = @{ Path = $path; RegistryPath = $registry }
function Assert($condition, $message) { if (!$condition) { throw $message } }
try {
    $output = & $tool @common
    Assert (($output -join "`n") -match '状态：开启' -and ($output -join "`n") -match '记录：0 条') 'missing file is an empty, enabled memory'

    $rows = "1`ts`thao`t号`n1`ts`thao`t号`nbroken`n0`ts`thao`t好`n1`tx`thao`t好`n9`tt`tshi`t是`n1`ts`tnihao`t泥号`n"
    [IO.File]::WriteAllText($path, $rows, [Text.UTF8Encoding]::new($false))
    $output = (& $tool @common) -join "`n"
    Assert ($output -match '记录：3 条') 'malformed rows are skipped and repeats are merged'
    Assert ($output -match '简体  hao → 号  （2 次）' -and $output -match '繁体  shi → 是  （8 次）') 'counts are summed and capped'
    Assert (((& $tool @common -Top 1) | Select-Object -Last 1) -match 'nihao → 泥号') 'most recent first'

    $csv = Join-Path $folder 'export.csv'
    Assert ((& $tool @common -Export $csv) -match '已导出 3 条') 'export count'
    $exported = Import-Csv -LiteralPath $csv
    Assert ($exported.Count -eq 3 -and $exported[0].Reading -eq 'hao' -and $exported[0].Text -eq '号' -and $exported[0].Count -eq '2') 'export content'

    & $tool @common -Disable | Out-Null
    Assert ((Get-ItemProperty -LiteralPath $registry).SelectionMemory -eq 0) 'disable writes the switch'
    Assert (((& $tool @common) -join "`n") -match '状态：关闭') 'disabled state is shown'
    Assert ([IO.File]::ReadAllText($path) -eq $rows) 'disabling keeps the records'
    & $tool @common -Enable | Out-Null
    Assert ((Get-ItemProperty -LiteralPath $registry).SelectionMemory -eq 1) 'enable writes the switch'

    # Clearing must work while an input process has the journal open.
    $held = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite,Delete')
    try { & $tool @common -Clear | Out-Null } finally { $held.Dispose() }
    Assert ((Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).Length -eq 0) 'clear truncates in place'
    Assert (((& $tool @common) -join "`n") -match '记录：0 条') 'cleared memory is empty'
    Remove-Item -LiteralPath $path
    & $tool @common -Clear | Out-Null
    Write-Output 'PASS: selection memory show, export, switch and clear'
} finally {
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $registry -Recurse -Force -ErrorAction SilentlyContinue
}
