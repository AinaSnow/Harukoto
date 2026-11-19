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
    $latest = @(& $tool @common -Top 1) -match ' → '
    Assert ($latest.Count -eq 1 -and $latest[0] -match 'nihao → 泥号') 'most recent first'

    $csv = Join-Path $folder 'export.csv'
    Assert ((& $tool @common -Export $csv) -match '已导出 3 条') 'export count'
    $exported = Import-Csv -LiteralPath $csv
    Assert ($exported.Count -eq 3 -and $exported[0].Reading -eq 'hao' -and $exported[0].Text -eq '号' -and $exported[0].Count -eq '2' -and $exported[0].Kind -eq '选词') 'export content'

    # CN-06c personal words live beside the journal and follow the same rules
    # as the input service: +1 per selection, the other words of the reading
    # halve, weights halve every 336 hours, broken rows are skipped.
    $words = Join-Path $folder 'selection-memory-words.tsv'
    $hour = 480000
    $wordRows = "+`ts`tshiyan`t试验`t$hour`n+`ts`tshiyan`t试验`t$hour`n+`ts`tshiyan`t实验`t$hour`n" +
        "=200`tt`tgongsi`t公司`t$($hour - 336)`nbroken`n+`ts`tni'hao`t你好`t$hour`n+`ts`tni`t你`t0`n=9999`ts`tni`t你`t$hour`n"
    [IO.File]::WriteAllText($words, $wordRows, [Text.UTF8Encoding]::new($false))
    $output = (& $tool @common -Hour $hour) -join "`n"
    Assert ($output -match '词数：3 个') 'word rows are replayed and broken rows skipped'
    Assert ($output -match '简体  shiyan → 实验  （权重 1，加分 0.69' -and $output -match '简体  shiyan → 试验  （权重 1，加分 0.69') 'a selection halves the other words of its reading'
    Assert ($output -match '繁体  gongsi → 公司  （权重 1，') 'compacted weights decay by half-life'
    Assert (((& $tool @common -Hour ($hour + 336 * 4)) -join "`n") -match '词数：3 个') 'faded words remain until below the floor'
    Assert (((& $tool @common -Hour ($hour + 336 * 5)) -join "`n") -match '词数：0 个') 'faded words are forgotten'
    Assert ((& $tool @common -Export $csv -Hour $hour) -match '已导出 6 条') 'export includes words'
    $exported = Import-Csv -LiteralPath $csv
    $word = $exported | Where-Object { $_.Kind -eq '个人词频' -and $_.Text -eq '实验' }
    Assert ($word -and $word.Reading -eq 'shiyan' -and $word.Count -eq '1' -and $word.Script -eq '简体') 'exported word content'

    & $tool @common -Disable | Out-Null
    Assert ((Get-ItemProperty -LiteralPath $registry).SelectionMemory -eq 0) 'disable writes the switch'
    Assert (((& $tool @common) -join "`n") -match '状态：关闭') 'disabled state is shown'
    Assert ([IO.File]::ReadAllText($path) -eq $rows -and [IO.File]::ReadAllText($words) -eq $wordRows) 'disabling keeps the records'
    & $tool @common -Enable | Out-Null
    Assert ((Get-ItemProperty -LiteralPath $registry).SelectionMemory -eq 1) 'enable writes the switch'

    # Clearing must work while an input process has the journal open.
    $held = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite,Delete')
    try { & $tool @common -Clear | Out-Null } finally { $held.Dispose() }
    Assert ((Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).Length -eq 0) 'clear truncates in place'
    Assert ((Get-Item -LiteralPath $words).Length -eq 0) 'clear truncates the word file too'
    $cleared = (& $tool @common) -join "`n"
    Assert ($cleared -match '记录：0 条' -and $cleared -match '词数：0 个') 'cleared memory is empty'
    Remove-Item -LiteralPath $path, $words
    & $tool @common -Clear | Out-Null
    Write-Output 'PASS: selection memory and personal words show, export, switch and clear'
} finally {
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $registry -Recurse -Force -ErrorAction SilentlyContinue
}
