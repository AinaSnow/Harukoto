#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../DiagnosisSupport.ps1')
function Assert-Equal($Actual, $Expected) { if ($Actual -ne $Expected) { throw "Expected $Expected, got $Actual" } }
Assert-Equal (Compare-HarukotoModule @{Sha256='abc'} @{Sha256='abc'}) '一致'
Assert-Equal (Compare-HarukotoModule @{Sha256='abc'} @{Sha256='def'}) '不同，需重启应用'
Assert-Equal (Compare-HarukotoModule @{Sha256='abc'} @{Sha256=$null}) '无法确认'
# WIN-08 runtime fields: optional (older services), validated when present.
$record = [ordered]@{format=1; thread_id=7; slot_id=0; observed_tick_ms=1; service_version='x'; base_version=''
    base_generation=''; theme_generation=''; pending_base_generation=''; phase='idle'; last_apply_failed=$false}
$old = Convert-HarukotoRuntimeRecord ($record | ConvertTo-Json -Compress) $null
Assert-Equal $old.InputMode '未报告（旧版）'
if ($null -ne $old.GameInput) { throw 'Game fields invented for an old record' }
$record += [ordered]@{input_mode='host_closed'; open_close_changes=3; host_closes=2; candidate_ui='host'; ui_elements=4
    ui_elements_host_drawn=4; ui_element_failures=0; compositions=4; composition_failures=0; compositions_terminated=1}
$game = Convert-HarukotoRuntimeRecord ($record | ConvertTo-Json -Compress) $null
Assert-Equal $game.InputMode '英文（应用关闭了输入法）'
Assert-Equal $game.CandidateUi '应用自绘（UI-less）'
Assert-Equal $game.GameInput.HostCloses 2
Assert-Equal $game.GameInput.CompositionsTerminated 1
foreach ($bad in @(@{candidate_ui='other'}, @{input_mode='klingon'}, @{host_closes=-1}, @{compositions='many'})) {
    $broken = [ordered]@{} + $record
    foreach ($key in $bad.Keys) { $broken[$key] = $bad[$key] }
    $rejected = $false
    try { Convert-HarukotoRuntimeRecord ($broken | ConvertTo-Json -Compress) $null | Out-Null } catch { $rejected = $true }
    if (!$rejected) { throw "Invalid game field accepted: $($bad.Keys)" }
}
# WIN-03 step 6a engine fields: optional, validated when present.
Assert-Equal $game.Engine '未报告（旧版）'
$record += [ordered]@{engine_path='engine'; engine_served=5; engine_fallbacks=1; engine_reason='none'}
Assert-Equal (Convert-HarukotoRuntimeRecord ($record | ConvertTo-Json -Compress) $null).Engine '独立引擎；引擎提供 5 次，回退 1 次，原因 none'
$record += [ordered]@{engine_memory_behind=2}
Assert-Equal (Convert-HarukotoRuntimeRecord ($record | ConvertTo-Json -Compress) $null).Engine '独立引擎；引擎提供 5 次，回退 1 次，原因 none，学习记录未追上 2 次'
# WIN-03 step 7a/7b/7d cutover, slot lifecycle, thin-client & compatibility fields: optional, validated when present.
$record7a = [ordered]@{} + $record + [ordered]@{engine_sequence=2; engine_cutovers=1; engine_cutover_failures=1; engine_yields=3; engine_reclaimed=2; engine_local_lexicon=$false; engine_incompatible=$true}
Assert-Equal (Convert-HarukotoRuntimeRecord ($record7a | ConvertTo-Json -Compress) $null).Engine '独立引擎；引擎提供 5 次，回退 1 次，原因 none，学习记录未追上 2 次，版本序号 2，已换代 1 次，换代失败 1 次，主动礼让 3 次，空闲回收 2 次，瘦客户端，协议不兼容，请重开应用'
foreach ($bad in @(@{engine_path='remote'}, @{engine_served=-1}, @{engine_reason='Bad Reason'}, @{engine_memory_behind='x'}, @{engine_sequence=-1}, @{engine_cutovers='one'}, @{engine_cutover_failures=-2}, @{engine_yields=-1}, @{engine_reclaimed='two'}, @{engine_local_lexicon='no'}, @{engine_incompatible='yes'})) {
    $broken = [ordered]@{} + $record
    foreach ($key in $bad.Keys) { $broken[$key] = $bad[$key] }
    $rejected = $false
    try { Convert-HarukotoRuntimeRecord ($broken | ConvertTo-Json -Compress) $null | Out-Null } catch { $rejected = $true }
    if (!$rejected) { throw "Invalid engine field accepted: $($bad.Keys)" }
}
# WIN-03 step 6c: the resident engine record from harukoto_engine_diagnose --resident --json.
Assert-Equal (Convert-HarukotoResidentRecord '{"format":1,"scope":"resident_engine","trusted_discovery":false,"present":false}').Present $false
$resident = Convert-HarukotoResidentRecord '{"format":1,"scope":"resident_engine","trusted_discovery":false,"present":true,"owner_pid":42,"alive":true,"start_reason":"demand","state":"ready","uptime_ms":5,"clients":2,"host_pid":43,"host_restarts":0,"last_exit":"none","last_exit_pid":0,"last_exit_age_ms":null,"recent_on_demand_starts":1,"breaker_tripped":false}'
Assert-Equal $resident.Status '就绪，所有者 42（运行中），接入方 2，宿主重启 0 次，近期按需启动 1 次'
$superseded = Convert-HarukotoResidentRecord '{"format":1,"scope":"resident_engine","trusted_discovery":false,"present":true,"owner_pid":42,"alive":false,"start_reason":"demand","state":"exited","uptime_ms":0,"clients":0,"host_pid":0,"host_restarts":0,"last_exit":"superseded","last_exit_pid":42,"last_exit_age_ms":100,"recent_on_demand_starts":0,"breaker_tripped":false}'
Assert-Equal $superseded.State 'exited'
foreach ($bad in @('{"format":2,"scope":"resident_engine","present":false}', '{"format":1,"scope":"resident_engine","present":true,"state":"flying","alive":true,"breaker_tripped":false}')) {
    $rejected = $false
    try { Convert-HarukotoResidentRecord $bad | Out-Null } catch { $rejected = $true }
    if (!$rejected) { throw "Invalid resident record accepted: $bad" }
}
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
