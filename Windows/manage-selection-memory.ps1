#requires -Version 7.0
# 选词记忆（CN-06）管理：查看、导出、清除、关闭与开启。只处理当前用户的数据。
# 同时覆盖选词记录（selection-memory.tsv）和个人词频（selection-memory-words.tsv，CN-06c）。
[CmdletBinding(DefaultParameterSetName = 'Show')]
param(
    [Parameter(ParameterSetName = 'Show')][switch]$Show,
    [Parameter(ParameterSetName = 'Show')][ValidateRange(1, 100000)][int]$Top = 50,
    [Parameter(ParameterSetName = 'Export', Mandatory)][string]$Export,
    [Parameter(ParameterSetName = 'Clear', Mandatory)][switch]$Clear,
    [Parameter(ParameterSetName = 'Disable', Mandatory)][switch]$Disable,
    [Parameter(ParameterSetName = 'Enable', Mandatory)][switch]$Enable,
    # 仅供测试覆盖默认位置。个人词频文件固定在选词记录旁边。
    [string]$Path = (Join-Path $env:LOCALAPPDATA 'Meowpuut/selection-memory.tsv'),
    [string]$RegistryPath = 'HKCU:\Software\Meowpuut\Input',
    # 仅供测试固定“现在”（自 1970 年起的整小时数）。
    [long]$Hour = 0
)
$ErrorActionPreference = 'Stop'
$WordsPath = Join-Path (Split-Path -LiteralPath $Path) ([IO.Path]::GetFileNameWithoutExtension($Path) + '-words' + [IO.Path]::GetExtension($Path))
if ($Hour -le 0) { $Hour = [long][Math]::Floor([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() / 3600) }

function Read-Lines([string]$File) {
    if (!(Test-Path -LiteralPath $File)) { return }
    $stream = [IO.File]::Open($File, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite,Delete')
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false))
        while ($null -ne ($line = $reader.ReadLine())) { $line }
    } finally { $stream.Dispose() }
}

function Read-Memory {
    # 与输入服务相同的规则：逐行累计，次数封顶为 8，坏行跳过。
    $entries = [ordered]@{}
    $order = 0
    foreach ($line in Read-Lines $Path) {
        $fields = $line.Split("`t")
        if ($fields.Count -ne 4 -or $fields[0] -notmatch '^[0-9]{1,2}$' -or [int]$fields[0] -lt 1 -or
            $fields[1] -notin 's', 't' -or $fields[2] -notmatch "^[a-z']{1,128}$" -or !$fields[3]) { continue }
        $key = $fields[1] + "`t" + $fields[2] + "`t" + $fields[3]
        $count = [int]$fields[0]
        if ($entries.Contains($key)) { $count += $entries[$key].Count }
        $entries[$key] = [pscustomobject]@{
            Script = if ($fields[1] -eq 's') { '简体' } else { '繁体' }
            Reading = $fields[2]; Text = $fields[3]; Count = [Math]::Min(8, $count); Order = ++$order
        }
    }
    return $entries
}

function Read-Words {
    # 个人词频，规则同输入服务（SelectionMemory.h 的 WordModel）：每次确认加 1、
    # 封顶 20；同读音的其他词减半；每 14 天减半；低于 0.05 即遗忘。
    $groups = @{}
    $order = 0
    $age = {
        param($entry, [long]$at)
        if ($at -gt $entry.Hour) { $entry.Weight *= [Math]::Pow(2, - ($at - $entry.Hour) / 336.0); $entry.Hour = $at }
    }
    foreach ($line in Read-Lines $WordsPath) {
        $fields = $line.Split("`t")
        if ($fields.Count -ne 5 -or $fields[1] -notin 's', 't' -or $fields[2] -notmatch '^[a-z]{1,128}$' -or !$fields[3] -or
            $fields[3] -match '[\x00-\x1f\x7f]' -or $fields[4] -notmatch '^[0-9]{1,9}$' -or [long]$fields[4] -lt 1) { continue }
        $at = [long]$fields[4]
        if ($fields[0] -eq '+') { $amount = 1.0; $selection = $true }
        elseif ($fields[0] -match '^=([0-9]{1,9})$' -and [long]$Matches[1] -ge 1 -and [long]$Matches[1] -le 2000) { $amount = [long]$Matches[1] / 100.0; $selection = $false }
        else { continue }
        $group = $fields[1] + "`t" + $fields[2]
        if (!$groups.ContainsKey($group)) { $groups[$group] = [ordered]@{} }
        $texts = $groups[$group]
        if ($selection) {
            foreach ($other in @($texts.Keys)) {
                if ($other -ceq $fields[3]) { continue }
                & $age $texts[$other] $at
                $texts[$other].Weight /= 2
            }
        }
        if (!$texts.Contains($fields[3])) {
            if ($texts.Count -ge 8) {
                $weakest = $texts.Keys | Sort-Object { $e = $texts[$_]; $e.Weight * [Math]::Pow(2, - [Math]::Max(0, $at - $e.Hour) / 336.0) } | Select-Object -First 1
                $texts.Remove($weakest)
            }
            $texts[$fields[3]] = [pscustomobject]@{
                Script = if ($fields[1] -eq 's') { '简体' } else { '繁体' }
                Reading = $fields[2]; Text = $fields[3]; Weight = 0.0; Hour = $at; Order = 0
            }
        }
        $entry = $texts[$fields[3]]
        & $age $entry $at
        $entry.Weight = [Math]::Min(20.0, $entry.Weight + $amount)
        $entry.Order = ++$order
        foreach ($other in @($texts.Keys)) { if ($texts[$other].Weight -lt 0.05) { $texts.Remove($other) } }
        if ($texts.Count -eq 0) { $groups.Remove($group) }
    }
    $result = foreach ($texts in $groups.Values) {
        foreach ($entry in $texts.Values) {
            $weight = $entry.Weight * [Math]::Pow(2, - [Math]::Max(0, $Hour - $entry.Hour) / 336.0)
            if ($weight -lt 0.05) { continue }
            [pscustomobject]@{
                Script = $entry.Script; Reading = $entry.Reading; Text = $entry.Text
                Weight = [Math]::Round($weight, 2); Bonus = [Math]::Round([Math]::Min(3.0, [Math]::Log(1 + $weight)), 2)
                LastUsed = [DateTimeOffset]::FromUnixTimeSeconds($entry.Hour * 3600).ToLocalTime().ToString('yyyy-MM-dd HH:00')
                Order = $entry.Order
            }
        }
    }
    return @($result)
}

function Get-Enabled {
    $value = Get-ItemProperty -LiteralPath $RegistryPath -Name SelectionMemory -ErrorAction SilentlyContinue
    return !$value -or $value.SelectionMemory -ne 0
}

switch ($PSCmdlet.ParameterSetName) {
    'Disable' {
        New-Item -Path $RegistryPath -Force | Out-Null
        Set-ItemProperty -LiteralPath $RegistryPath -Name SelectionMemory -Type DWord -Value 0
        Write-Output '选词记忆已关闭：不再记录，也不再调整候选顺序（含个人词频）。已有记录保留，可用 -Clear 清除。下次按键时生效。'
    }
    'Enable' {
        New-Item -Path $RegistryPath -Force | Out-Null
        Set-ItemProperty -LiteralPath $RegistryPath -Name SelectionMemory -Type DWord -Value 1
        Write-Output '选词记忆已开启。下次按键时生效。'
    }
    'Clear' {
        foreach ($file in $Path, $WordsPath) {
            if (Test-Path -LiteralPath $file) {
                # 清空而不是删除：正在输入的应用下次查看时发现文件变短，会一并清空内存里的记录。
                $stream = [IO.File]::Open($file, [IO.FileMode]::Truncate, [IO.FileAccess]::Write, [IO.FileShare]'ReadWrite,Delete')
                $stream.Dispose()
            }
        }
        Write-Output '选词记忆和个人词频已清除。各应用会在几秒内的下一次按键时生效。'
    }
    'Export' {
        $rows = @((Read-Memory).Values | Sort-Object Order | Select-Object Script, Reading, Text, Count, @{ n = 'Kind'; e = { '选词' } })
        $rows += @(Read-Words | Sort-Object Order | Select-Object Script, Reading, Text, @{ n = 'Count'; e = { $_.Weight } }, @{ n = 'Kind'; e = { '个人词频' } })
        $rows | Export-Csv -LiteralPath $Export -Encoding utf8BOM
        Write-Output ('已导出 ' + $rows.Count + ' 条到 ' + $Export)
    }
    default {
        $entries = @((Read-Memory).Values)
        $words = Read-Words
        Write-Output ('状态：' + $(if (Get-Enabled) { '开启' } else { '关闭' }))
        Write-Output ('文件：' + $Path)
        Write-Output ('记录：' + $entries.Count + ' 条')
        $entries | Sort-Object Order -Descending | Select-Object -First $Top |
            ForEach-Object { '{0}  {1} → {2}  （{3} 次）' -f $_.Script, $_.Reading, $_.Text, $_.Count }
        Write-Output ('个人词频：' + $WordsPath)
        Write-Output ('词数：' + $words.Count + ' 个')
        $words | Sort-Object Order -Descending | Select-Object -First $Top |
            ForEach-Object { '{0}  {1} → {2}  （权重 {3}，加分 {4}，最近 {5}）' -f $_.Script, $_.Reading, $_.Text, $_.Weight, $_.Bonus, $_.LastUsed }
    }
}
