#requires -Version 7.0
# 选词记忆（CN-06）管理：查看、导出、清除、关闭与开启。只处理当前用户的数据。
[CmdletBinding(DefaultParameterSetName = 'Show')]
param(
    [Parameter(ParameterSetName = 'Show')][switch]$Show,
    [Parameter(ParameterSetName = 'Show')][ValidateRange(1, 100000)][int]$Top = 50,
    [Parameter(ParameterSetName = 'Export', Mandatory)][string]$Export,
    [Parameter(ParameterSetName = 'Clear', Mandatory)][switch]$Clear,
    [Parameter(ParameterSetName = 'Disable', Mandatory)][switch]$Disable,
    [Parameter(ParameterSetName = 'Enable', Mandatory)][switch]$Enable,
    # 仅供测试覆盖默认位置。
    [string]$Path = (Join-Path $env:LOCALAPPDATA 'Meowpuut/selection-memory.tsv'),
    [string]$RegistryPath = 'HKCU:\Software\Meowpuut\Input'
)
$ErrorActionPreference = 'Stop'

function Read-Memory {
    # 与输入服务相同的规则：逐行累计，次数封顶为 8，坏行跳过。
    $entries = [ordered]@{}
    if (!(Test-Path -LiteralPath $Path)) { return $entries }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite,Delete')
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false))
        $order = 0
        while ($null -ne ($line = $reader.ReadLine())) {
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
    } finally { $stream.Dispose() }
    return $entries
}

function Get-Enabled {
    $value = Get-ItemProperty -LiteralPath $RegistryPath -Name SelectionMemory -ErrorAction SilentlyContinue
    return !$value -or $value.SelectionMemory -ne 0
}

switch ($PSCmdlet.ParameterSetName) {
    'Disable' {
        New-Item -Path $RegistryPath -Force | Out-Null
        Set-ItemProperty -LiteralPath $RegistryPath -Name SelectionMemory -Type DWord -Value 0
        Write-Output '选词记忆已关闭：不再记录，也不再调整候选顺序。已有记录保留，可用 -Clear 清除。下次按键时生效。'
    }
    'Enable' {
        New-Item -Path $RegistryPath -Force | Out-Null
        Set-ItemProperty -LiteralPath $RegistryPath -Name SelectionMemory -Type DWord -Value 1
        Write-Output '选词记忆已开启。下次按键时生效。'
    }
    'Clear' {
        if (Test-Path -LiteralPath $Path) {
            # 清空而不是删除：正在输入的应用下次查看时发现文件变短，会一并清空内存里的记录。
            $stream = [IO.File]::Open($Path, [IO.FileMode]::Truncate, [IO.FileAccess]::Write, [IO.FileShare]'ReadWrite,Delete')
            $stream.Dispose()
        }
        Write-Output '选词记忆已清除。各应用会在几秒内的下一次按键时生效。'
    }
    'Export' {
        $rows = (Read-Memory).Values | Sort-Object Order | Select-Object Script, Reading, Text, Count
        $rows | Export-Csv -LiteralPath $Export -Encoding utf8BOM
        Write-Output ('已导出 ' + @($rows).Count + ' 条到 ' + $Export)
    }
    default {
        $entries = @((Read-Memory).Values)
        Write-Output ('状态：' + $(if (Get-Enabled) { '开启' } else { '关闭' }))
        Write-Output ('文件：' + $Path)
        Write-Output ('记录：' + $entries.Count + ' 条')
        $entries | Sort-Object Order -Descending | Select-Object -First $Top |
            ForEach-Object { '{0}  {1} → {2}  （{3} 次）' -f $_.Script, $_.Reading, $_.Text, $_.Count }
    }
}
