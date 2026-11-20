param([switch]$Register, [switch]$Signed, [switch]$Prebuilt, [switch]$VerifyOnly)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PowerShellHost.ps1')
. (Join-Path $PSScriptRoot 'InstallSupport.ps1')
. (Join-Path $PSScriptRoot 'InputProfile.ps1')
$powerShellHost = Get-MeowpuutPowerShell
if ($PSVersionTable.PSVersion.Major -lt 7) {
    $forwarded = @()
    foreach ($name in 'Register','Signed','Prebuilt','VerifyOnly') {
        if ($PSBoundParameters[$name]) { $forwarded += "-$name" }
    }
    & $powerShellHost -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath @forwarded
    exit $LASTEXITCODE
}
$transcriptStarted = $false
$installLock = $null
try {
if (!$VerifyOnly) {
    $logRoot = Join-Path $env:LOCALAPPDATA 'Meowpuut/Logs'
    New-Item -ItemType Directory -Force $logRoot | Out-Null
    $phase = if ($Register) { 'register' } else { 'install' }
    $logPath = Join-Path $logRoot ('install-{0}-{1}-{2}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $phase, [guid]::NewGuid().ToString('N'))
    Start-Transcript -LiteralPath $logPath | Out-Null
    $transcriptStarted = $true
    Write-Host "Installation log: $logPath"
    Write-Host "PowerShell: $($PSVersionTable.PSVersion); phase: $phase"
}
$build = Join-Path (Split-Path $PSScriptRoot) '.build/windows/ime'
$sourceDll = Join-Path $build 'MeowpuutTip.dll'
$sourceLexicon = Join-Path $build 'native-pinyin.tsv'
$sourceJapanese = Join-Path $build 'japanese.tsv'
$sourceEvidence = Join-Path $build 'language-evidence.sqlite'
$renderer = Join-Path (Split-Path $build) 'MeowpuutGlassPreview.exe'
if ($Prebuilt) {
    $build = Join-Path $PSScriptRoot 'payload'
    $sourceDll = Join-Path $build 'MeowpuutTip.dll'
    $sourceLexicon = Join-Path $build 'native-pinyin.tsv'
    $sourceJapanese = Join-Path $build 'japanese.tsv'
    $sourceEvidence = Join-Path $build 'language-evidence.sqlite'
    $renderer = Join-Path $build 'MeowpuutGlassPreview.exe'
}


if (!(Test-Path -LiteralPath $sourceDll) -or !(Test-Path -LiteralPath $sourceLexicon) -or !(Test-Path -LiteralPath $sourceJapanese) -or !(Test-Path -LiteralPath $sourceEvidence)) {
    throw 'Build the Windows IME before installing it.'
}
if ($Signed) {
    foreach ($binary in $sourceDll, $renderer) {
        if (!(Test-Path -LiteralPath $binary) -or
            (Get-AuthenticodeSignature -LiteralPath $binary).Status -ne 'Valid') {
            throw "A valid Authenticode signature is required for $binary."
        }
    }
} elseif (!$Prebuilt -and !$Register -and !$VerifyOnly) {
    & (Join-Path $PSScriptRoot 'build-preview.ps1') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Glass renderer build failed.' }
}
if (!(Test-Path -LiteralPath $renderer -PathType Leaf)) {
    throw 'The glass renderer is missing from the installation files.'
}
$managerFiles = foreach ($name in 'dictionary-manager.pyw','japanese_packages.py','japanese_dictionary.py','ff14-manifest.json','ff14-words.txt','ff14-reviewed-source.json') {
    $path = Join-Path (Split-Path $renderer) $name
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing dictionary manager file: $path" }
    $path
}
if ($Prebuilt -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'Explain.cmd')) -and
    !(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'harukoto_candidate_explain.exe') -PathType Leaf)) {
    throw 'Explain.cmd is present but harukoto_candidate_explain.exe is missing.'
}
if ($VerifyOnly) {
    Write-Output 'All Meowpuut installation files are present.'
    return
}
if ($Register) {
    if (![Environment]::Is64BitProcess) { throw 'Use x64 PowerShell 7 to register the x64 input service.' }
    $installRoot = Join-Path $env:ProgramFiles 'Meowpuut/IME'
    New-Item -ItemType Directory -Force $installRoot | Out-Null
    # Serialize elevated registration and hold the lock until this process finishes.
    try {
        $installLock = [IO.File]::Open((Join-Path $installRoot '.install.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    } catch { throw "Another installation may be running, or the installation directory is inaccessible. $_" }
    $serverKey = 'HKLM:\Software\Classes\CLSID\{961C0ADA-4DC6-488F-9563-4FC144EFAE27}\InprocServer32'
    $getRegisteredDll = {
        if (Test-Path -LiteralPath $serverKey) { (Get-Item -LiteralPath $serverKey).GetValue('') }
    }
    $previousDll = & $getRegisteredDll
    if ($previousDll -and !(Test-Path -LiteralPath $previousDll -PathType Leaf)) {
        throw "Previous registered DLL is missing: $previousDll. Repair it before upgrading so recovery remains possible."
    }
    Write-Host "Previous registered DLL: $previousDll"
    $packageFiles = @($sourceDll, $sourceLexicon, $sourceJapanese, $sourceEvidence, $renderer) + @($managerFiles)
    $conjugationData = Join-Path $build 'japanese-conjugations.bin'
    if (Test-Path -LiteralPath $conjugationData -PathType Leaf) { $packageFiles += $conjugationData }
    $version = ($packageFiles | ForEach-Object { (Get-FileHash -LiteralPath $_).Hash.Substring(0, 12) }) -join '-'
    $install = Join-Path $env:ProgramFiles "Meowpuut/IME/$version"
    $dll = Join-Path $install 'MeowpuutTip.dll'
    Copy-MeowpuutPayload -Files $packageFiles -Destination $install
    Write-Host "Verified payload: $install"
    if ($Prebuilt) {
        foreach ($notice in 'LICENSE','THIRD_PARTY.md','Licenses') {
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot $notice) -Destination $install -Recurse -Force
        }
    }
    $registerServer = { param($path)
        (Start-Process -FilePath "$env:WINDIR/System32/regsvr32.exe" -ArgumentList @('/s', ('"' + $path + '"')) -PassThru -Wait -WindowStyle Hidden).ExitCode
    }
    $unregisterServer = { param($path)
        (Start-Process -FilePath "$env:WINDIR/System32/regsvr32.exe" -ArgumentList @('/s','/u', ('"' + $path + '"')) -PassThru -Wait -WindowStyle Hidden).ExitCode
    }
    Invoke-MeowpuutRegistration -Dll $dll -PreviousDll $previousDll -RegisterServer $registerServer -UnregisterServer $unregisterServer -GetRegisteredDll $getRegisteredDll
    return
}

$arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Register'
if ($Prebuilt) { $arguments += ' -Prebuilt' }
if ($Signed) { $arguments += ' -Signed' }
$setup = Start-Process -FilePath $powerShellHost -ArgumentList $arguments -Verb RunAs -PassThru -Wait -WindowStyle Hidden
if ($setup.ExitCode -ne 0) {
    throw "TSF registration failed with exit code $($setup.ExitCode). See the register log in Meowpuut/Logs under the elevated account LOCALAPPDATA."
}
Write-Host 'Machine registration completed. Configuring the current user profile.'

# Remove the old per-user server override only after machine registration succeeds.
$legacy = 'HKCU:\Software\Classes\CLSID\{961C0ADA-4DC6-488F-9563-4FC144EFAE27}\InprocServer32'
if (Test-Path -LiteralPath $legacy) {
    Remove-Item -LiteralPath $legacy
}
$tip = $script:HarukotoTip
$list = Get-WinUserLanguageList
if (Update-HarukotoLanguageList -List $list) {
    Set-WinUserLanguageList $list -Force
}


# Publish the enabled profile to the active sign-in session.
Write-Host 'User language list configured. Publishing the profile to the active session.'
if (-not ('Meowpuut.InputProfile' -as [type])) {
    Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
namespace Meowpuut
{
    public static class InputProfile
    {
        [DllImport("input.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool InstallLayoutOrTip(string profile, uint flags);

        [DllImport("msctf.dll", ExactSpelling = true)]
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        public static extern int TF_InvalidAssemblyListCacheIfExist();
    }
}
"@
}
if ([Meowpuut.InputProfile]::TF_InvalidAssemblyListCacheIfExist() -lt 0) {
    Write-Warning 'Windows could not refresh the TSF cache. Sign out and back in if apps still load the previous version.'
}
if (-not [Meowpuut.InputProfile]::InstallLayoutOrTip("0x$tip", 0)) {
    throw 'Windows could not apply the Meowpuut profile to the current session.'
}

# Run the glass renderer outside packaged text editors, and start it again at sign-in.
Write-Host 'Session profile published. Configuring settings and candidate renderer.'
$serverKey = 'HKLM:\Software\Classes\CLSID\{961C0ADA-4DC6-488F-9563-4FC144EFAE27}\InprocServer32'
$registeredDll = (Get-Item -LiteralPath $serverKey).GetValue('')
$glass = Join-Path (Split-Path $registeredDll) 'MeowpuutGlassPreview.exe'
$programs = Join-Path ([Environment]::GetFolderPath('Programs')) '晴词 Harukoto'
New-Item -ItemType Directory -Force $programs | Out-Null
$settingsShortcut = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $programs '晴词候选窗设置.lnk'))
$settingsShortcut.TargetPath = $glass
$settingsShortcut.Arguments = '--settings'
$settingsShortcut.WorkingDirectory = Split-Path $glass
$settingsShortcut.Save()
$dictionaryShortcut = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $programs '晴词主题词库.lnk'))
$dictionaryShortcut.TargetPath = $glass
$dictionaryShortcut.Arguments = '--dictionaries'
$dictionaryShortcut.WorkingDirectory = Split-Path $glass
$dictionaryShortcut.Save()
# Publish both new shortcuts before removing the known legacy entries.
# Preserve anything else the user placed in the old folder.
$legacyPrograms = Join-Path ([Environment]::GetFolderPath('Programs')) 'Meowpuut'
foreach ($name in 'Meowpuut Settings.lnk','晴词候选窗设置.lnk','晴词主题词库.lnk') {
    $legacyShortcut = Join-Path $legacyPrograms $name
    if (Test-Path -LiteralPath $legacyShortcut) { Remove-Item -LiteralPath $legacyShortcut }
}
if ((Test-Path -LiteralPath $legacyPrograms) -and !(Get-ChildItem -LiteralPath $legacyPrograms -Force)) {
    Remove-Item -LiteralPath $legacyPrograms
}
Write-Host 'Settings shortcut saved. Checking the candidate renderer.'
if (-not (Get-Process MeowpuutGlassPreview -ErrorAction SilentlyContinue | Where-Object Path -eq $glass)) {
    try {
        $rendererProcess = Start-Process -FilePath $glass -ArgumentList '--ime','desktop' -WindowStyle Hidden -PassThru
        if ($rendererProcess.WaitForExit(500)) {
            throw "The renderer exited during startup with code $($rendererProcess.ExitCode)."
        }
    } catch {
        Write-Warning "The glass renderer could not start. The installed IME will use its native candidate window. $_"
        return
    }
}
$startup = Join-Path ([Environment]::GetFolderPath('Startup')) 'Meowpuut Candidate Renderer.lnk'
Write-Host 'Candidate renderer available. Updating the startup shortcut.'
$previous = (New-Object -ComObject WScript.Shell).CreateShortcut($startup)
if ($previous.TargetPath -ne $glass) {
    $oldPath = $previous.TargetPath
    if ($oldPath) {
        Get-Process MeowpuutGlassPreview -ErrorAction SilentlyContinue |
            Where-Object Path -eq $oldPath |
            Stop-Process
    }
    $previous.TargetPath = $glass
    $previous.Arguments = '--ime desktop'
    $previous.WorkingDirectory = Split-Path $glass
    $previous.Save()
}
Write-Host 'Installation completed. Reopen applications to load the new input service.'
} catch {
    Write-Host "Installation failed: $($_.Exception.Message)"
    throw
} finally {
    if ($installLock) { $installLock.Dispose() }
    if ($transcriptStarted) { Stop-Transcript | Out-Null }
}
