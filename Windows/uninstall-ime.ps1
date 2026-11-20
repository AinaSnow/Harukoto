param([switch]$Unregister, [switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PowerShellHost.ps1')
. (Join-Path $PSScriptRoot 'InputProfile.ps1')
. (Join-Path $PSScriptRoot 'InstallSupport.ps1')
$hostPath = Get-MeowpuutPowerShell
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Run this script with PowerShell 7.' }
if ($VerifyOnly) { Write-Output 'Uninstaller is available.'; return }
$logRoot = Join-Path $env:LOCALAPPDATA 'Meowpuut/Logs'
New-Item -ItemType Directory -Force $logRoot | Out-Null
Start-Transcript -LiteralPath (Join-Path $logRoot ('uninstall-' + [guid]::NewGuid().ToString('N') + '.log')) | Out-Null
try {
    if ($Unregister) {
        if (![Environment]::Is64BitProcess) { throw 'Use x64 PowerShell 7.' }
        $root = [IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'Meowpuut/IME'))
        $lock = $null
        try {
            if (Test-Path -LiteralPath $root) {
                $lock = [IO.File]::Open((Join-Path $root '.install.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            }
            $key = 'HKLM:\Software\Classes\CLSID\{961C0ADA-4DC6-488F-9563-4FC144EFAE27}\InprocServer32'
            if (Test-Path -LiteralPath $key) {
                $dll = [IO.Path]::GetFullPath((Get-Item -LiteralPath $key).GetValue(''))
                $folder = Split-Path $dll
                if ([IO.Path]::GetDirectoryName($folder) -ne $root -or (Split-Path $dll -Leaf) -ne 'MeowpuutTip.dll') {
                    throw 'Registered DLL is outside the managed installation directory; no files were removed.'
                }
                foreach ($path in $root,$folder,$dll) {
                    if ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing a redirected installation path.' }
                }
                $process = Start-Process -FilePath "$env:WINDIR/System32/regsvr32.exe" -ArgumentList @('/s','/u',('"' + $dll + '"')) -WindowStyle Hidden -Wait -PassThru
                if ($process.ExitCode -ne 0 -or (Test-Path -LiteralPath $key)) {
                    throw "Unregistration could not be verified (exit $($process.ExitCode)). Files were preserved."
                }
                Write-Host 'Machine registration removed. Version files retained for apps that still hold the DLL.'
            }
            # Version files stay on disk, but no engine remains approved.
            try {
                Unpublish-HarukotoEngineRelease
                Write-Host 'Engine release metadata removed.'
            } catch { Write-Warning "Engine release metadata could not be removed: $_" }
        } finally { if ($lock) { $lock.Dispose() } }
        return
    }
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Unregister'
    $process = Start-Process -FilePath $hostPath -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "Machine unregistration failed: $($process.ExitCode). See the elevated account uninstall log." }
    $list = Get-WinUserLanguageList
    if (Update-HarukotoLanguageList -List $list -Remove) { Set-WinUserLanguageList $list -Force }
    $root = [IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'Meowpuut/IME')) + [IO.Path]::DirectorySeparatorChar
    Get-Process MeowpuutGlassPreview -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process
    foreach ($shortcut in @(
        (Join-Path ([Environment]::GetFolderPath('Startup')) 'Meowpuut Candidate Renderer.lnk'),
        (Join-Path ([Environment]::GetFolderPath('Programs')) 'Meowpuut/Meowpuut Settings.lnk'),
        (Join-Path ([Environment]::GetFolderPath('Programs')) 'Meowpuut/晴词候选窗设置.lnk'),
        (Join-Path ([Environment]::GetFolderPath('Programs')) 'Meowpuut/晴词主题词库.lnk'),
        (Join-Path ([Environment]::GetFolderPath('Programs')) '晴词 Harukoto/晴词候选窗设置.lnk'),
        (Join-Path ([Environment]::GetFolderPath('Programs')) '晴词 Harukoto/晴词主题词库.lnk')
    )) {
        if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut }
    }
    foreach ($name in 'Meowpuut','晴词 Harukoto') {
        $folder = Join-Path ([Environment]::GetFolderPath('Programs')) $name
        if ((Test-Path -LiteralPath $folder) -and !(Get-ChildItem -LiteralPath $folder -Force)) {
            Remove-Item -LiteralPath $folder
        }
    }
    Write-Host 'Uninstalled for this user. Personal data and version files are retained. Reopen applications.'
} catch { Write-Host "Uninstallation failed: $_"; throw }
finally { Stop-Transcript | Out-Null }
