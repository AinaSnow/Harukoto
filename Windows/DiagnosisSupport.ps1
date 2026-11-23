function Compare-HarukotoModule {
    param($Registered, $Loaded)
    if ($Registered.Sha256 -and $Loaded.Sha256) {
        if ($Registered.Sha256 -eq $Loaded.Sha256) { return '一致' }
        return '不同，需重启应用'
    }
    return '无法确认'
}

function Get-HarukotoBasePublication {
    param([string]$Root)
    try {
        $path = Join-Path $Root 'STATE.json'
        if (!(Test-Path -LiteralPath $path)) { return [pscustomobject]@{Status='未发布用户基础词库'; Generation=$null} }
        if ((Get-Item -LiteralPath $path).Length -gt 65536) { throw '状态文件超限' }
        $state = Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
        if ($state.Count -ne 3 -or !$state.ContainsKey('previous') -or $state.format -isnot [long] -and $state.format -isnot [int] -or
            $state.format -ne 1 -or $state.current -isnot [string] -or $state.current -cnotmatch '^[0-9a-f]{64}$' -or
            ($null -ne $state.previous -and ($state.previous -isnot [string] -or $state.previous -cnotmatch '^[0-9a-f]{64}$'))) { throw '版本指针损坏' }
        return [pscustomobject]@{Status='已发布指针（未重新校验词库内容）'; Generation=$state.current}
    } catch { return [pscustomobject]@{Status=('无法读取：' + $_.Exception.Message); Generation=$null} }
}

function Convert-HarukotoRuntimeRecord {
    param([string]$Json, [string]$PublishedGeneration, [long]$NowTick = [Environment]::TickCount64)
    $value = $Json | ConvertFrom-Json -AsHashtable -ErrorAction Stop
    foreach ($key in 'format','observed_tick_ms','thread_id','slot_id') {
        if ($value[$key] -isnot [long] -and $value[$key] -isnot [int]) { throw '无效运行时数值字段' }
    }
    if ($value.format -ne 1 -or
        $value.observed_tick_ms -lt 0 -or $value.observed_tick_ms -gt $NowTick -or
        $value.service_version -isnot [string] -or $value.service_version.Length -gt 128 -or
        $value.base_version -isnot [string] -or $value.base_version.Length -gt 128 -or
        $value.last_apply_failed -isnot [bool] -or $value.thread_id -le 0 -or
        $value.slot_id -lt 0 -or $value.slot_id -ge 64) { throw '无效运行时记录' }
    foreach ($key in 'base_generation','pending_base_generation','theme_generation') {
        $length = if ($key -eq 'theme_generation') {32} else {64}
        if ($value[$key] -isnot [string] -or ($value[$key] -ne '' -and $value[$key] -cnotmatch "^[0-9a-f]{$length}$")) { throw '无效运行时版本标识' }
    }
    $phases = @{idle='空闲／上次检查完成'; loading='后台检查或加载中'; waiting_boundary='已准备，等待组字边界'; failed='更新失败，沿用旧版'; unavailable='重载状态不可用'}
    if (!$phases.ContainsKey($value.phase)) { throw '未知运行时状态' }
    # WIN-08 fields, absent in older services: input mode, open/close from
    # the host, who draws the candidates, inline compositions. No text.
    $modes = @{chinese='中文'; english='英文'; host_closed='英文（应用关闭了输入法）'}
    $candidateViews = @{none='尚未出现候选'; own='晴词候选窗'; host='应用自绘（UI-less）'; own_fallback='晴词候选窗（应用不支持 UI 元素）'}
    $game = $null
    if ($value.ContainsKey('input_mode')) {
        if (!$modes.ContainsKey($value.input_mode) -or !$candidateViews.ContainsKey($value.candidate_ui)) { throw '未知游戏输入状态' }
        $counts = [ordered]@{}
        foreach ($key in 'open_close_changes','host_closes','ui_elements','ui_elements_host_drawn','ui_element_failures','compositions','composition_failures','compositions_terminated') {
            if (($value[$key] -isnot [long] -and $value[$key] -isnot [int]) -or $value[$key] -lt 0) { throw '无效游戏输入计数' }
            $counts[$key] = $value[$key]
        }
        $game = [pscustomobject]@{ InputMode=$modes[$value.input_mode]; CandidateUi=$candidateViews[$value.candidate_ui]
            OpenCloseChanges=$counts.open_close_changes; HostCloses=$counts.host_closes
            UiElements=$counts.ui_elements; UiElementsHostDrawn=$counts.ui_elements_host_drawn; UiElementFailures=$counts.ui_element_failures
            Compositions=$counts.compositions; CompositionFailures=$counts.composition_failures; CompositionsTerminated=$counts.compositions_terminated }
    }
    # WIN-03 step 6a, absent in older services: who listed the last
    # candidates (standalone engine or in-process) and why.
    $engine = $null
    if ($value.ContainsKey('engine_path')) {
        $paths = @{off='关闭（进程内）'; in_process='进程内（引擎不适用）'; engine='独立引擎'; fallback='已回退进程内'}
        if (!$paths.ContainsKey($value.engine_path) -or $value.engine_reason -isnot [string] -or $value.engine_reason -cnotmatch '^[a-z_]{1,32}$') { throw '无效引擎路径字段' }
        foreach ($key in 'engine_served','engine_fallbacks') {
            if (($value[$key] -isnot [long] -and $value[$key] -isnot [int]) -or $value[$key] -lt 0) { throw '无效引擎计数' }
        }
        $engine = "$($paths[$value.engine_path])；引擎提供 $($value.engine_served) 次，回退 $($value.engine_fallbacks) 次，原因 $($value.engine_reason)"
        # Step 6b, optional: replies where the engine had not caught up with the learning.
        if ($value.ContainsKey('engine_memory_behind')) {
            if (($value.engine_memory_behind -isnot [long] -and $value.engine_memory_behind -isnot [int]) -or $value.engine_memory_behind -lt 0) { throw '无效引擎计数' }
            $engine += "，学习记录未追上 $($value.engine_memory_behind) 次"
        }
        # Step 7a/7b, optional: active release sequence, cutover/yield/reclaimed counts and protocol compatibility.
        foreach ($key in 'engine_sequence','engine_cutovers','engine_cutover_failures','engine_yields','engine_reclaimed') {
            if ($value.ContainsKey($key)) {
                if (($value[$key] -isnot [long] -and $value[$key] -isnot [int]) -or $value[$key] -lt 0) { throw '无效引擎计数' }
            }
        }
        if ($value.ContainsKey('engine_incompatible') -and $value.engine_incompatible -isnot [bool]) { throw '无效引擎路径字段' }
        if ($value.ContainsKey('engine_local_lexicon') -and $value.engine_local_lexicon -isnot [bool]) { throw '无效引擎路径字段' }
        if ($value.ContainsKey('engine_sequence') -and $value.engine_sequence -gt 0) { $engine += "，版本序号 $($value.engine_sequence)" }
        if ($value.ContainsKey('engine_cutovers') -and $value.engine_cutovers -gt 0) { $engine += "，已换代 $($value.engine_cutovers) 次" }
        if ($value.ContainsKey('engine_cutover_failures') -and $value.engine_cutover_failures -gt 0) { $engine += "，换代失败 $($value.engine_cutover_failures) 次" }
        if ($value.ContainsKey('engine_yields') -and $value.engine_yields -gt 0) { $engine += "，主动礼让 $($value.engine_yields) 次" }
        if ($value.ContainsKey('engine_reclaimed') -and $value.engine_reclaimed -gt 0) { $engine += "，空闲回收 $($value.engine_reclaimed) 次" }
        if ($value.ContainsKey('engine_local_lexicon') -and !$value.engine_local_lexicon) { $engine += '，瘦客户端' }
        if ($value.ContainsKey('engine_incompatible') -and $value.engine_incompatible) { $engine += '，协议不兼容，请重开应用' }
    }
    # RH-4, absent in older services: which candidate renderer is active, its
    # approved release sequence, payload version, handoff and failure counts.
    $renderer = $null
    if ($value.ContainsKey('renderer_source')) {
        $sources = @{own='自带目录'; desktop='桌面渲染器'; approved='已批准新版（每进程）'}
        if (!$sources.ContainsKey($value.renderer_source)) { throw '无效渲染器来源字段' }
        foreach ($key in 'renderer_sequence','renderer_handoffs','renderer_failures') {
            if (($value[$key] -isnot [long] -and $value[$key] -isnot [int]) -or $value[$key] -lt 0) { throw '无效渲染器计数' }
        }
        if (($value.renderer_payload -isnot [long] -and $value.renderer_payload -isnot [int]) -or $value.renderer_payload -lt 1) { throw '无效渲染器载荷版本' }
        $renderer = "$($sources[$value.renderer_source])；载荷 v$($value.renderer_payload)"
        if ($value.renderer_sequence -gt 0) { $renderer += "，版本序号 $($value.renderer_sequence)" }
        $renderer += "，换代 $($value.renderer_handoffs) 次，失败 $($value.renderer_failures) 次"
    }
    $relation = if (!$PublishedGeneration) { '无可比较的发布指针' }
        elseif ($value.base_generation -ceq $PublishedGeneration) { '已采用发布版本' }
        else { '尚未采用当前发布版本' }
    [pscustomobject]@{
        ThreadId=$value.thread_id; SlotId=$value.slot_id
        ServiceVersion=$value.service_version; BaseVersion=$value.base_version
        BaseGeneration=$value.base_generation; ThemeGeneration=$value.theme_generation
        PendingBaseGeneration=$value.pending_base_generation
        BaseSource=$(if ($value.base_generation) {'用户快照'} else {'安装目录（未报告内容哈希）'})
        Phase=$phases[$value.phase]; LastApplyFailed=$value.last_apply_failed; Publication=$relation
        ObservationAgeSeconds=[math]::Round(($NowTick - $value.observed_tick_ms) / 1000, 1)
        InputMode=$(if ($game) {$game.InputMode} else {'未报告（旧版）'})
        CandidateUi=$(if ($game) {$game.CandidateUi} else {'未报告（旧版）'})
        GameInput=$game
        Engine=$(if ($engine) {$engine} else {'未报告（旧版）'})
        Renderer=$(if ($renderer) {$renderer} else {'未报告（旧版）'})
    }
}

# WIN-03 step 6c: harukoto_engine_diagnose --resident --json, as one line of text.
function Convert-HarukotoResidentRecord {
    param([string]$Json)
    $value = $Json | ConvertFrom-Json -AsHashtable -ErrorAction Stop
    if ($value.format -ne 1 -or $value.scope -ne 'resident_engine' -or $value.present -isnot [bool]) { throw '无效常驻引擎记录' }
    if (!$value.present) { return [pscustomobject]@{ Status = '本次登录后尚未启动过常驻引擎'; Present = $false } }
    $states = @{none='无'; starting='启动中'; ready='就绪'; restarting='重启中'; stopping='停止中'; exited='已退出'}
    if (!$states.ContainsKey($value.state) -or $value.alive -isnot [bool] -or $value.breaker_tripped -isnot [bool]) { throw '无效常驻引擎记录' }
    foreach ($key in 'owner_pid','clients','host_restarts','recent_on_demand_starts') {
        if (($value[$key] -isnot [long] -and $value[$key] -isnot [int]) -or $value[$key] -lt 0) { throw '无效常驻引擎计数' }
    }
    $text = "$($states[$value.state])，所有者 $($value.owner_pid)$(if ($value.alive) {'（运行中）'} else {'（已不在运行）'})，接入方 $($value.clients)，宿主重启 $($value.host_restarts) 次，近期按需启动 $($value.recent_on_demand_starts) 次$(if ($value.breaker_tripped) {'，断路器已打开'})"
    [pscustomobject]@{ Status = $text; Present = $true; State = $value.state; Alive = $value.alive; Clients = $value.clients; BreakerTripped = $value.breaker_tripped }
}

function Get-HarukotoRuntimeStatus {
    param([Diagnostics.Process]$Process, [string]$PublishedGeneration)
    try {
        if (!('HarukotoRuntimeStatusReader' -as [type])) {
            Add-Type -Path (Join-Path $PSScriptRoot 'RuntimeStatusReader.cs')
        }
        $rows = @([HarukotoRuntimeStatusReader]::Read($Process.Id, $Process.StartTime.ToUniversalTime().ToFileTimeUtc()))
        $instances = @($rows | ForEach-Object { Convert-HarukotoRuntimeRecord $_ $PublishedGeneration })
        [pscustomobject]@{ Status=$(if ($instances.Count) {'已读取实例报告'} else {'无稳定的活动实例报告，无法确认'}); Instances=$instances }
    } catch { [pscustomobject]@{Status='无可读报告（旧版、访问受限、进程退出或记录损坏），无法确认'; Instances=@()} }
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
