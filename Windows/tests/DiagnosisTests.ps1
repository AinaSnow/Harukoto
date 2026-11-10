#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../DiagnosisSupport.ps1')
function Assert-Equal($Actual, $Expected) { if ($Actual -ne $Expected) { throw "Expected $Expected, got $Actual" } }
Assert-Equal (Compare-HarukotoModule @{Sha256='abc'} @{Sha256='abc'}) '一致'
Assert-Equal (Compare-HarukotoModule @{Sha256='abc'} @{Sha256='def'}) '不同，需重启应用'
Assert-Equal (Compare-HarukotoModule @{Sha256='abc'} @{Sha256=$null}) '无法确认'
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$root = Join-Path $tempBase ('harukoto-diagnosis-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    Assert-Equal (Get-HarukotoBasePublication $root).Status '未发布用户基础词库'
    $basePointer = Join-Path $root 'STATE.json'
    @{format=1; current=('c' * 64); previous=$null} | ConvertTo-Json | Set-Content -LiteralPath $basePointer
    Assert-Equal (Get-HarukotoBasePublication $root).Generation ('c' * 64)
    foreach ($bad in @(@{format=$true; current=('c' * 64); previous=$null}, @{format=1; current='../bad'; previous=$null}, @{format=1; current=('c' * 64); previous='bad'})) {
        $bad | ConvertTo-Json | Set-Content -LiteralPath $basePointer
        if ((Get-HarukotoBasePublication $root).Status -notlike '无法读取*') { throw 'Invalid base pointer accepted' }
    }
    Assert-Equal (Get-HarukotoDictionarySummary $root).Status '未配置主题包'
    [IO.File]::WriteAllText((Join-Path $root 'CURRENT'), '../invalid')
    if ((Get-HarukotoDictionarySummary $root).Status -notlike '读取失败*') { throw 'Invalid pointer accepted' }
    $generation = 'a' * 32
    [IO.File]::WriteAllText((Join-Path $root 'CURRENT'), $generation)
    $snapshot = Join-Path $root $generation
    New-Item -ItemType Directory -Path $snapshot | Out-Null
    $entry = @('nihao','こんにちは',5000,'こんにちは','pinyin','你好')
    $state = @{format=1; packages=@{}}
    foreach ($id in 'test-a','test-b') {
        $state.packages[$id] = @{ enabled=$true; current=@{manifest=@{id=$id; name=$id; version='1.0.0'}; entries=@(,$entry)}; previous=$null }
    }
    $state | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $snapshot 'state.json') -Encoding utf8
    ($entry -join '|') | Set-Content -LiteralPath (Join-Path $snapshot 'enabled.tsv') -Encoding utf8
    $result = Get-HarukotoDictionarySummary $root
    Assert-Equal $result.RuntimeRows 1
    Assert-Equal $result.Packages.Count 2
    if ($result.Status -notlike '元数据可读*') { throw $result.Status }
    [IO.File]::WriteAllText((Join-Path $snapshot 'enabled.tsv'), '')
    if ((Get-HarukotoDictionarySummary $root).Status -notlike '启用行数不一致*') { throw 'Mismatch not detected' }
    [IO.File]::WriteAllText((Join-Path $snapshot 'state.json'), '{}')
    if ((Get-HarukotoDictionarySummary $root).Status -notlike '读取失败*') { throw 'Corruption not detected' }
    Write-Output 'PASS: DLL comparison, unknown identity, pointer, deduplication and damaged snapshots'
} finally {
    $resolved = (Resolve-Path -LiteralPath $root).Path
    if ([IO.Path]::GetDirectoryName($resolved).TrimEnd('\') -ne $tempBase.TrimEnd('\') -or (Split-Path $resolved -Leaf) -notlike 'harukoto-diagnosis-test-*') { throw 'Unexpected cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
