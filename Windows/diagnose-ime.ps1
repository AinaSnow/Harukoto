#requires -Version 7.0
param([switch]$Json)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'DiagnosisSupport.ps1')
$identities = @{}
function Get-DllIdentity([string]$Path) {
    if ($identities.ContainsKey($Path)) { return $identities[$Path] }
    $result = [ordered]@{ Path = $Path; Sha256 = $null; FileVersion = $null; Error = $null }
    try {
        $file = Get-Item -LiteralPath $Path
        $result.Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        $result.FileVersion = $file.VersionInfo.FileVersion
    } catch { $result.Error = $_.Exception.Message }
    $identities[$Path] = [pscustomobject]$result
    return $identities[$Path]
}

$registry = @()
foreach ($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryHive]::LocalMachine)) {
    $base = $null; $key = $null
    try {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
        $key = $base.OpenSubKey('Software\Classes\CLSID\{961C0ADA-4DC6-488F-9563-4FC144EFAE27}\InprocServer32')
        $path = if ($key) { [string]$key.GetValue('') } else { $null }
        $registry += [pscustomobject]@{ Hive = "$hive"; Status = $(if ($path) { '已注册' } else { '未注册' }); Dll = $(if ($path) { Get-DllIdentity $path } else { $null }) }
    } catch { $registry += [pscustomobject]@{ Hive = "$hive"; Status = '无法读取'; Dll = $null } }
    finally { if ($key) { $key.Dispose() }; if ($base) { $base.Dispose() } }
}
$registered = @($registry | Where-Object Dll | Select-Object -First 1)
$reference = if ($registered.Count -and !($registry | Where-Object Status -eq '无法读取')) { $registered[0].Dll } else { [pscustomobject]@{ Sha256 = $null } }
$matches = @(); $unknown = 0; $checked = 0
$basePublication = Get-HarukotoBasePublication (Join-Path $env:LOCALAPPDATA 'Meowpuut/BaseDictionaries')
foreach ($process in Get-Process) {
    try {
        $modules = @($process.Modules)
        if ($modules.Count -eq 0) { $unknown++; continue }
        $checked++
        foreach ($module in $modules) {
            if ($module.ModuleName -ine 'MeowpuutTip.dll') { continue }
            $loaded = Get-DllIdentity $module.FileName
            $matches += [pscustomobject]@{ Name = $process.ProcessName; Id = $process.Id; Dll = $loaded; Status = (Compare-HarukotoModule $reference $loaded)
                Runtime = Get-HarukotoRuntimeStatus $process $basePublication.Generation }
        }
    } catch { $unknown++ }
}
# WIN-08: whether the TIP is in the user's input list, the default input
# method override, and the game-related settings. Read-only.
$inputSettings = [ordered]@{ InLanguageList = '无法确认'; DefaultOverride = '无法确认'; InlineComposition = '默认（0 自动）'; ShiftToggle = '默认（1 左右均可）' }
try {
    $tips = @(Get-WinUserLanguageList -ErrorAction Stop | ForEach-Object { $_.InputMethodTips })
    $inputSettings.InLanguageList = if ($tips | Where-Object { $_ -like '*961C0ADA-4DC6-488F-9563-4FC144EFAE27*' }) { '是' } else { '否（请在语言设置中添加晴词）' }
} catch {}
try {
    $override = Get-WinDefaultInputMethodOverride -ErrorAction Stop
    $inputSettings.DefaultOverride = if (!$override) { '未设置' }
        elseif ("$($override.InputMethodTip)" -like '*961C0ADA-4DC6-488F-9563-4FC144EFAE27*') { '晴词' }
        else { "其他：$($override.Description)" }
} catch {}
try {
    $inputKey = Get-ItemProperty -LiteralPath 'HKCU:\Software\Meowpuut\Input' -ErrorAction Stop
    if ($null -ne $inputKey.InlineComposition) { $inputSettings.InlineComposition = "$($inputKey.InlineComposition)" }
    if ($null -ne $inputKey.ShiftToggle) { $inputSettings.ShiftToggle = "$($inputKey.ShiftToggle)" }
} catch {}
$version = $null
try { $version = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Meowpuut').DisplayVersion } catch {}
$report = [pscustomobject]@{
    Format = 2; Timestamp = [DateTimeOffset]::Now.ToString('o'); PowerShell = $PSVersionTable.PSVersion.ToString()
    PackageVersion = $version; Registration64 = $registry; Processes = $matches
    CheckedProcesses = $checked; UnknownProcesses = $unknown
    BasePublication = $basePublication
    InputSettings = [pscustomobject]$inputSettings
    Dictionaries = Get-HarukotoDictionarySummary (Join-Path $env:LOCALAPPDATA 'Meowpuut/Dictionaries')
    Notes = @('安装器版本不等于进程版本；DLL 以路径和 SHA256 对比，缺少文件版本时不猜测。',
        '比较模块路径当前文件的哈希；不是进程内存校验。采用版本目录的安装可据此排查残留旧模块。',
        '只检查 64 位注册。无权读取、进程退出或空模块表均记为未知，不能据此声称所有应用已更新。',
        '发布指针与实例实际采用版本分开显示；状态为最近实际按键时观察，不是持续心跳，空闲应用可能保留较旧的阶段描述。',
        '实例报告不含输入文字；无报告、超出每进程 64 个实例或访问受限时不能据此确认已更新。安装目录来源未报告完整内容哈希。',
        '游戏（WIN-08）：InputMode 为“应用关闭了输入法”时单按 Shift 即恢复中文；CandidateUi 为“应用自绘”表示游戏自己画候选，晴词候选窗按设计不显示。',
        '发现不同版本时请保存工作，从托盘彻底退出相关应用后重开；本工具不结束进程、不提权、不修改配置。')
}
if ($Json) { $report | ConvertTo-Json -Depth 10; return }
Write-Output '晴词 · 安装与旧进程诊断'
Write-Output ('安装器记录版本：' + $(if ($version) { $version } else { '未知／非安装器安装' }))
foreach ($registration in $registry) {
    Write-Output ("注册 [$($registration.Hive)]：$($registration.Status)")
    if ($registration.Dll) {
        Write-Output ("  $($registration.Dll.Path)")
        Write-Output ("  SHA256: $($registration.Dll.Sha256)")
    }
}
if ($matches.Count) { $matches | Select-Object Name,Id,Status,@{n='DLL';e={$_.Dll.Path}} | Format-Table -Wrap -AutoSize }
else { Write-Output '未在可检查进程中找到晴词 DLL。' }
Write-Output "已检查 $checked 个进程，$unknown 个无法确认。"
Write-Output ('基础词库发布：' + $basePublication.Status + ' ' + $basePublication.Generation)
foreach ($match in $matches) {
    Write-Output ("运行时 [$($match.Name) / $($match.Id)]：" + $match.Runtime.Status)
    $match.Runtime.Instances | Format-List ThreadId,SlotId,ServiceVersion,BaseSource,BaseVersion,BaseGeneration,ThemeGeneration,PendingBaseGeneration,Phase,LastApplyFailed,Publication,ObservationAgeSeconds,InputMode,CandidateUi,
        @{n='OpenClose';e={if ($_.GameInput) {"应用改动 $($_.GameInput.OpenCloseChanges) 次，其中关闭 $($_.GameInput.HostCloses) 次"}}},
        @{n='UiElements';e={if ($_.GameInput) {"$($_.GameInput.UiElements) 次，应用自绘 $($_.GameInput.UiElementsHostDrawn) 次，失败 $($_.GameInput.UiElementFailures) 次"}}},
        @{n='Compositions';e={if ($_.GameInput) {"$($_.GameInput.Compositions) 次，失败 $($_.GameInput.CompositionFailures) 次，被应用结束 $($_.GameInput.CompositionsTerminated) 次"}}}
}
Write-Output '输入设置（WIN-08）：'
Write-Output ('  语言列表含晴词：' + $report.InputSettings.InLanguageList)
Write-Output ('  默认输入法覆盖：' + $report.InputSettings.DefaultOverride)
Write-Output ('  InlineComposition：' + $report.InputSettings.InlineComposition + '；ShiftToggle：' + $report.InputSettings.ShiftToggle)
Write-Output ('主题词库：' + $report.Dictionaries.Status)
if ($report.Dictionaries.Packages.Count) { $report.Dictionaries.Packages | Format-Table Name,Version,Enabled,Entries -AutoSize }
$report.Notes | ForEach-Object { Write-Output "- $_" }
