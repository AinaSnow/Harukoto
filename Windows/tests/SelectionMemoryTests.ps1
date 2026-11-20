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

    # CN-07 batch 5: forget rows written by the input service ("-" rows) remove
    # what came before them; a later selection is counted again from one.
    $forgetRows = "3`ts`thao`t号`n1`ts`tshi`t是`n-`ts`thao`t号`n-`tt`tshi`t是`n-`tx`tshi`t是`n1`ts`thao`t号`n-`ts`tni`t你`n"
    $forgetWords = "+`ts`tgongsi`t公司`t$hour`n+`ts`tgongsi`t攻丝`t$hour`n-`ts`tgongsi`t公司`t$hour`n-`ts`tshi`t是`t$hour`n"
    $forgetPath = Join-Path $folder 'forget.tsv'
    [IO.File]::WriteAllText($forgetPath, $forgetRows, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $folder 'forget-words.tsv'), $forgetWords, [Text.UTF8Encoding]::new($false))
    $output = (& $tool -Path $forgetPath -RegistryPath $registry -Hour $hour) -join "`n"
    Assert ($output -match '记录：2 条' -and $output -match '简体  hao → 号  （1 次）' -and $output -match '简体  shi → 是  （1 次）') 'forget rows remove earlier selections, other scripts stay'
    Assert ($output -match '词数：1 个' -and $output -match '简体  gongsi → 攻丝' -and $output -notmatch 'gongsi → 公司') 'forget rows remove learned words'
    Assert ((& $tool -Path $forgetPath -RegistryPath $registry -Hour $hour -Export $csv) -match '已导出 3 条') 'export after forgetting'

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

    # DICT-02b: hand-added words in a table beside the journals.
    $user = Join-Path $folder 'selection-memory-user.tsv'
    $rowsOf = { @([IO.File]::ReadAllLines($user, [Text.UTF8Encoding]::new($false)) | Where-Object { !$_.StartsWith('#') }) }
    Assert ((& $tool @common -AddWord 'zhang san' -Text '张珊') -match "已添加个人词：zhang'san → 张珊") 'add splits and joins syllables'
    Assert ((& $tool @common -AddWord 'zhāngsān' -Text '张珊' -Traditional '張珊') -match "已更新个人词：zhang1'san1 → 张珊（繁体 張珊）") 'tone marks; the same word is updated'
    & $tool @common -AddWord 'xian' -Text '先先' | Out-Null
    & $tool @common -AddWord "xi'an" -Text '西安市' | Out-Null
    & $tool @common -AddWord 'fangan' -Text '方案甲' | Out-Null
    $rows = & $rowsOf
    Assert ($rows.Count -eq 4 -and $rows[0] -ceq "zhang1'san1`t张珊`t張珊" -and $rows[1] -ceq "xian`t先先" -and $rows[2] -ceq "xi'an`t西安市" -and
        $rows[3] -ceq "fang'an`t方案甲") 'table rows: fewest syllables, longest first'
    foreach ($bad in @(@('qxz', '坏'), @('zhang', "a`tb"), @('zhang', "a`u{7f}"), @('zhang1 2', '坏'), @('zhāng3', '坏'), @('zhang', ('长' * 65)),
                       @(('zhang ' * 17), '坏'))) {
        $rejected = $false
        try { & $tool @common -AddWord $bad[0] -Text $bad[1] | Out-Null } catch { $rejected = $true }
        Assert $rejected "rejects $($bad[0]) / $($bad[1])"
    }
    Assert ((& $rowsOf).Count -eq 4) 'rejected words change nothing'
    # Revised in place: a new text drops the old traditional form.
    & $tool @common -ReviseWord 'zhangsan' -Text '张珊' -NewText '章珊' | Out-Null
    $rows = & $rowsOf
    Assert ($rows[0] -ceq "zhang1'san1`t章珊" -and $rows.Count -eq 4) 'revise keeps the position'
    $rejected = $false
    try { & $tool @common -ReviseWord 'xian' -Text '先先' -NewReading "xi'an" -NewText '西安市' | Out-Null } catch { $rejected = $true }
    Assert $rejected 'revising onto another word is refused'
    $rejected = $false
    try { & $tool @common -RemoveWord 'nihao' -Text '你好' | Out-Null } catch { $rejected = $true }
    Assert $rejected 'removing a missing word is refused'
    # Removed, then rolled back, and forward again.
    & $tool @common -RemoveWord 'xian' -Text '先先' | Out-Null
    Assert ((& $rowsOf).Count -eq 3) 'remove'
    & $tool @common -UndoWords | Out-Null
    Assert ((& $rowsOf) -contains "xian`t先先" -and (& $rowsOf).Count -eq 4) 'undo restores the previous version'
    & $tool @common -UndoWords | Out-Null
    Assert ((& $rowsOf).Count -eq 3) 'undo again swaps back'
    & $tool @common -UndoWords | Out-Null
    # Export, clear, import; a broken import changes nothing.
    $exportFile = Join-Path $folder 'words.tsv'
    Assert ((& $tool @common -ExportWords $exportFile) -match '已导出 4 个个人词') 'export words'
    $before = [IO.File]::ReadAllText($user)
    Assert ([IO.File]::ReadAllText($exportFile) -ceq $before) 'export is the table itself'
    & $tool @common -ClearWords | Out-Null
    Assert ((& $rowsOf).Count -eq 0) 'clear words'
    Assert ((& $tool @common -ImportWords $exportFile) -match '新增 4 个') 'import'
    Assert ([IO.File]::ReadAllText($user) -ceq $before) 'import restores the same table'
    $broken = Join-Path $folder 'broken.tsv'
    [IO.File]::WriteAllText($broken, "nihao`t你好`nbad reading`t坏`n", [Text.UTF8Encoding]::new($false))
    $rejected = $false
    try { & $tool @common -ImportWords $broken | Out-Null } catch { $rejected = $true }
    Assert ($rejected -and [IO.File]::ReadAllText($user) -ceq $before) 'a broken import changes nothing'
    [IO.File]::WriteAllText($broken, "`u{feff}ni hao`t你好`nzhang'san`t章珊`t章珊`n", [Text.UTF8Encoding]::new($false))
    $rejected = $false
    try { & $tool @common -ImportWords $broken | Out-Null } catch { $rejected = $true }
    Assert $rejected 'stored readings have no spaces'
    [IO.File]::WriteAllText($broken, "`u{feff}nihao`t你好`nzhang'san`t章珊`t章珊`n", [Text.UTF8Encoding]::new($false))
    Assert ((& $tool @common -ImportWords $broken) -match '新增 1 个，更新 1 个') 'import merges'
    $rows = & $rowsOf
    Assert ($rows.Count -eq 5 -and $rows[0] -ceq "zhang'san`t章珊`t章珊" -and $rows[4] -ceq "ni'hao`t你好") 'merged rows'
    Assert ((& $tool @common -ImportWords $broken -Replace) -match '现在共 2 个') 'import can replace'
    & $tool @common -UndoWords | Out-Null
    Assert ((& $rowsOf).Count -eq 5) 'undo after import'
    # Replaced while an input service briefly has the table open: retried.
    $ready = Join-Path $folder 'held'
    $holder = Start-ThreadJob -ArgumentList $user, $ready {
        param($file, $marker)
        $held = [IO.File]::Open($file, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite,Delete')
        [IO.File]::WriteAllText($marker, '')
        Start-Sleep -Milliseconds 700
        $held.Dispose()
    }
    while (!(Test-Path -LiteralPath $ready)) { Start-Sleep -Milliseconds 20 }
    & $tool @common -AddWord 'qing ci' -Text '晴词' | Out-Null
    $holder | Wait-Job | Remove-Job
    Assert ((& $rowsOf).Count -eq 6) 'replace while open'
    # Shown, listed; neither the switch nor clearing the memory touches them.
    $output = (& $tool @common) -join "`n"
    Assert ($output -match '个人词数：6 个' -and $output -match "qing'ci → 晴词") 'show lists user words'
    Assert (((& $tool @common -ListWords -Top 1) -join "`n") -match "词数：6 个`n zhang'san → 章珊（繁体 章珊）".Replace("`n ", "`n")) 'list words'
    & $tool @common -Disable | Out-Null
    & $tool @common -Clear | Out-Null
    & $tool @common -Enable | Out-Null
    Assert ((& $rowsOf).Count -eq 6) 'clear and switch leave user words'
    Write-Output 'PASS: selection memory, personal words and user words show, export, import, revise, undo, switch and clear'
} finally {
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $registry -Recurse -Force -ErrorAction SilentlyContinue
}
