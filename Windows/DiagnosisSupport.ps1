function Compare-HarukotoModule {
    param($Registered, $Loaded)
    if ($Registered.Sha256 -and $Loaded.Sha256) {
        if ($Registered.Sha256 -eq $Loaded.Sha256) { return '一致' }
        return '不同，需重启应用'
    }
    return '无法确认'
}

function Get-HarukotoDictionarySummary {
    param([string]$Root)
    $result = [ordered]@{ Root = $Root; Status = '未配置主题包'; Generation = $null; Packages = @(); RuntimeRows = $null; RuntimeSha256 = $null }
    try {
        $pointer = Join-Path $Root 'CURRENT'
        if (!(Test-Path -LiteralPath $pointer)) { return [pscustomobject]$result }
        if ((Get-Item -LiteralPath $pointer).Length -ne 32) { throw '激活指针格式损坏' }
        $bytes = [IO.File]::ReadAllBytes($pointer)
        $generation = [Text.Encoding]::ASCII.GetString($bytes)
        if ($bytes.Length -ne 32 -or $generation -cnotmatch '^[0-9a-f]{32}$') { throw '激活指针格式损坏' }
        $result.Generation = $generation
        $folder = Join-Path $Root $generation
        $stateFile = Get-Item -LiteralPath (Join-Path $folder 'state.json') -ErrorAction Stop
        $runtime = Get-Item -LiteralPath (Join-Path $folder 'enabled.tsv') -ErrorAction Stop
        if ($stateFile.Length -gt 256MB -or $runtime.Length -gt 256MB) { throw '词库文件超出大小限制' }
        $state = Get-Content -LiteralPath $stateFile.FullName -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable -ErrorAction Stop
        if ($state.format -ne 1 -or $state.packages -isnot [System.Collections.IDictionary] -or $state.packages.Count -gt 128) { throw '词库状态格式损坏' }
        $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        $result.Packages = @(foreach ($id in $state.packages.Keys) {
            $item = $state.packages[$id]
            $manifest = $item.current.manifest
            if ($item.enabled -isnot [bool] -or $manifest.id -ne $id -or !$manifest.version -or $item.current.entries -isnot [array]) { throw '主题包元数据损坏' }
            if ($item.enabled) {
                foreach ($entry in $item.current.entries) { [void]$expected.Add(($entry -join '|')) }
            }
            [pscustomobject]@{ Id = $id; Name = $manifest.name; Version = $manifest.version; Enabled = $item.enabled; Entries = $item.current.entries.Count }
        })
        $count = 0
        foreach ($line in [IO.File]::ReadLines($runtime.FullName)) { $count++ }
        $result.RuntimeRows = $count
        $result.RuntimeSha256 = (Get-FileHash -LiteralPath $runtime.FullName -Algorithm SHA256).Hash
        $result.Status = if ($count -eq $expected.Count) { '元数据可读，启用行数一致；未执行完整词条校验' } else { '启用行数不一致，请用词库管理器检查' }
    } catch { $result.Status = '读取失败：' + $_.Exception.Message }
    return [pscustomobject]$result
}
