# Keep registration recovery separate from OS calls so failures can be tested safely.
function Invoke-MeowpuutRegistration {
    param(
        [Parameter(Mandatory)][string]$Dll,
        [string]$PreviousDll,
        [Parameter(Mandatory)][scriptblock]$RegisterServer,
        [Parameter(Mandatory)][scriptblock]$UnregisterServer,
        [Parameter(Mandatory)][scriptblock]$GetRegisteredDll,
        [scriptblock]$Log = { param($message) Write-Host $message }
    )
    try {
        & $Log "Registering: $Dll"
        $code = & $RegisterServer $Dll
        if ($code -ne 0) { throw "regsvr32 returned $code." }
        if ((& $GetRegisteredDll) -ne $Dll) { throw 'Registered DLL path does not match the new version.' }
        & $Log 'Machine registration succeeded.'
    } catch {
        $failure = $_.Exception.Message
        & $Log "Registration failed: $failure"
        try {
            if ($PreviousDll) {
                & $Log "Restoring previous registration: $PreviousDll"
                $code = & $RegisterServer $PreviousDll
                if ($code -ne 0 -or (& $GetRegisteredDll) -ne $PreviousDll) {
                    throw "Previous registration could not be verified (exit $code)."
                }
                & $Log 'Previous machine registration restored.'
            } else {
                & $Log 'Removing incomplete first-install registration.'
                $code = & $UnregisterServer $Dll
                if ($code -ne 0 -or (& $GetRegisteredDll)) {
                    throw "First-install cleanup could not be verified (exit $code)."
                }
                & $Log 'Incomplete machine registration removed.'
            }
        } catch {
            throw "Registration failed: $failure Recovery also failed: $($_.Exception.Message) Keep both version directories and inspect the installation log."
        }
        throw "Registration failed: $failure Recovery completed; installation was not continued."
    }
}

function Copy-MeowpuutPayload {
    param([Parameter(Mandatory)][string[]]$Files, [Parameter(Mandatory)][string]$Destination)
    New-Item -ItemType Directory -Force $Destination | Out-Null
    foreach ($source in $Files) {
        $target = Join-Path $Destination (Split-Path $source -Leaf)
        $expected = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        if (Test-Path -LiteralPath $target) {
            if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $expected) {
                throw "Existing version file differs: $target. It was not overwritten; keep it for diagnosis."
            }
        } else {
            Copy-Item -LiteralPath $source -Destination $target -ErrorAction Stop
        }
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $expected) {
            throw "Copied file failed SHA-256 verification: $target"
        }
    }
}

# WIN-03 step 1: approved engine deployments, see docs/ENGINE_RELEASE.md.
# Only the elevated installer writes this key (64-bit view; the -Register phase
# requires x64 PowerShell). One REG_MULTI_SZ value per version directory, so
# each version is published by a single atomic registry write.
$script:HarukotoEngineReleaseKey = 'HKLM:\SOFTWARE\Harukoto\EngineReleases'
$script:HarukotoEngineReleaseFormat = 'format=harukoto-engine-release/1'
# Must equal ProtocolVersion in Windows/EngineProtocol.h (checked by tests).
$script:HarukotoEngineProtocol = 2
# Owner Administrators; SYSTEM/Administrators full; users, app containers and
# the LPAC registryRead capability read only. Not inherited from the parent.
$script:HarukotoEngineReleaseSddl = 'O:BAD:PAI(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;KR;;;BU)(A;CI;KR;;;AC)(A;CI;KR;;;S-1-15-3-1024-1065365936-1281604716-3511738428-1654721687-432734479-3232135806-4053264122-3456934681)'
$script:HarukotoEngineHostName = 'harukoto_engine_host.exe'
$script:HarukotoEngineOwnerName = 'harukoto_engine_owner.exe'

function Test-HarukotoEngineReleaseId([string]$Id) {
    return $Id -cmatch '^[0-9A-Za-z][0-9A-Za-z._-]{0,239}$'
}

function Get-HarukotoEngineReleaseField {
    param([string[]]$Lines, [Parameter(Mandatory)][string]$Name)
    foreach ($line in @($Lines)) {
        if ($line -and $line.StartsWith("$Name=", [StringComparison]::Ordinal)) { return $line.Substring($Name.Length + 1) }
    }
    return $null
}

function Set-HarukotoEngineReleaseSecurity {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Sddl)
    if ($Path -notmatch '^(HKLM|HKCU):\\(.+)$') { throw "Unsupported registry path: $Path" }
    $hive = if ($Matches[1] -eq 'HKLM') { [Microsoft.Win32.RegistryHive]::LocalMachine } else { [Microsoft.Win32.RegistryHive]::CurrentUser }
    $sections = [Security.AccessControl.AccessControlSections]::Access
    $rights = [Security.AccessControl.RegistryRights]'ReadKey, ChangePermissions'
    if ($Sddl.StartsWith('O:', [StringComparison]::Ordinal)) {
        $sections = $sections -bor [Security.AccessControl.AccessControlSections]::Owner
        $rights = $rights -bor [Security.AccessControl.RegistryRights]::TakeOwnership
    }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $item = $base.OpenSubKey($Matches[2], [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, $rights)
        if (!$item) { throw "Registry key not found: $Path" }
        try {
            $security = $item.GetAccessControl($sections)
            $security.SetSecurityDescriptorSddlForm($Sddl, $sections)
            $item.SetAccessControl($security)
        } finally { $item.Dispose() }
    } finally { $base.Dispose() }
}

function New-HarukotoEngineRelease {
    param(
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][long]$Sequence,
        [Parameter(Mandatory)][int]$Protocol,
        [Parameter(Mandatory)][string]$EngineHost,
        [Parameter(Mandatory)][string]$HostSha256,
        [Parameter(Mandatory)][string]$Owner,
        [Parameter(Mandatory)][string]$OwnerSha256
    )
    return [string[]]@(
        $script:HarukotoEngineReleaseFormat, "id=$Id", "sequence=$Sequence", "protocol=$Protocol",
        "host=$EngineHost", "host-sha256=$HostSha256", "owner=$Owner", "owner-sha256=$OwnerSha256"
    )
}

# Publish the engine programs of an installed version directory. Digests are
# computed from the installed copies, never from the package or a running
# process. Older versions stay listed for the upgrade window; entries whose
# programs are gone are pruned, and at most $Limit entries are kept.
function Publish-HarukotoEngineRelease {
    param(
        [Parameter(Mandatory)][string]$VersionDirectory,
        [Parameter(Mandatory)][string]$Id,
        [string]$KeyPath = $script:HarukotoEngineReleaseKey,
        [string]$Sddl = $script:HarukotoEngineReleaseSddl,
        [int]$Protocol = $script:HarukotoEngineProtocol,
        [ValidateRange(1, 8)][int]$Limit = 4
    )
    if (!(Test-HarukotoEngineReleaseId $Id)) { throw "Invalid engine release id: $Id" }
    if ($Protocol -lt 1 -or $Protocol -gt 65535) { throw "Invalid engine protocol: $Protocol" }
    $folder = [IO.Path]::GetFullPath($VersionDirectory).TrimEnd('\')
    if ((Split-Path $folder -Leaf) -cne $Id) { throw 'The engine release id must name its version directory.' }
    if ($folder -notmatch '^[A-Za-z]:\\[^\\]') { throw "Engine programs must be on a local drive path: $folder" }
    if ((Get-Item -LiteralPath $folder -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing a redirected engine directory.' }
    # WIN-03 step 6c: the engine serves this folder as a base snapshot.
    if (!(Test-Path -LiteralPath (Join-Path $folder 'base-dictionary.json') -PathType Leaf)) { throw 'Missing base-dictionary.json: the engine could not load this version.' }
    $files = foreach ($name in $script:HarukotoEngineHostName, $script:HarukotoEngineOwnerName) {
        $path = Join-Path $folder $name
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing engine program: $path" }
        [pscustomobject]@{ Path = $path; Sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    if (!(Test-Path -LiteralPath $KeyPath)) {
        $parent = Split-Path $KeyPath
        if (!(Test-Path -LiteralPath $parent)) {
            New-Item -Path $parent -Force | Out-Null
            Set-HarukotoEngineReleaseSecurity -Path $parent -Sddl $Sddl
        }
        New-Item -Path $KeyPath | Out-Null
    }
    # Re-apply every time so an earlier, looser descriptor is corrected.
    Set-HarukotoEngineReleaseSecurity -Path (Split-Path $KeyPath) -Sddl $Sddl
    Set-HarukotoEngineReleaseSecurity -Path $KeyPath -Sddl $Sddl
    $key = Get-Item -LiteralPath $KeyPath
    $existing = @(foreach ($name in $key.GetValueNames()) {
        $lines = $null
        if ($key.GetValueKind($name) -eq [Microsoft.Win32.RegistryValueKind]::MultiString) { $lines = [string[]]$key.GetValue($name) }
        $sequence = Get-HarukotoEngineReleaseField $lines 'sequence'
        [pscustomobject]@{
            Name = $name
            Sequence = if ($sequence -cmatch '^[1-9][0-9]{0,15}$') { [long]$sequence } else { 0L }
            Host = Get-HarukotoEngineReleaseField $lines 'host'
        }
    })
    $next = 1L
    foreach ($entry in $existing) { if ($entry.Sequence -ge $next) { $next = $entry.Sequence + 1 } }
    $lines = New-HarukotoEngineRelease -Id $Id -Sequence $next -Protocol $Protocol -EngineHost $files[0].Path -HostSha256 $files[0].Sha256 -Owner $files[1].Path -OwnerSha256 $files[1].Sha256
    if ((($lines -join "`0").Length + 2) * 2 -gt 8192) { throw 'Engine release entry exceeds the 8 KiB reader limit.' }
    New-ItemProperty -LiteralPath $KeyPath -Name $Id -PropertyType MultiString -Value $lines -Force | Out-Null
    $key = Get-Item -LiteralPath $KeyPath
    if ($key.GetValueKind($Id) -ne [Microsoft.Win32.RegistryValueKind]::MultiString -or
        (([string[]]$key.GetValue($Id)) -join "`n") -cne ($lines -join "`n")) {
        throw 'Engine release entry could not be verified after writing.'
    }
    # Prune: malformed entries and versions whose host program is gone, then
    # the lowest sequences beyond the limit. The new entry is always kept.
    $kept = [Collections.Generic.List[object]]::new()
    foreach ($entry in $existing) {
        if ($entry.Name -ceq $Id) { continue }
        if ($entry.Sequence -gt 0 -and $entry.Host -and (Test-Path -LiteralPath $entry.Host -PathType Leaf)) { $kept.Add($entry) }
        else { Remove-ItemProperty -LiteralPath $KeyPath -Name $entry.Name }
    }
    $sorted = @($kept | Sort-Object Sequence -Descending)
    for ($i = $Limit - 1; $i -lt $sorted.Count; $i++) { Remove-ItemProperty -LiteralPath $KeyPath -Name $sorted[$i].Name }
    return [pscustomobject]@{ Id = $Id; Sequence = $next; Host = $files[0]; Owner = $files[1] }
}

# Uninstall: no engine stays approved once the input service is removed.
function Unpublish-HarukotoEngineRelease {
    param([string]$KeyPath = $script:HarukotoEngineReleaseKey)
    if (Test-Path -LiteralPath $KeyPath) { Remove-Item -LiteralPath $KeyPath -Recurse }
    $parent = Split-Path $KeyPath
    if ((Test-Path -LiteralPath $parent)) {
        $item = Get-Item -LiteralPath $parent
        if ($item.SubKeyCount -eq 0 -and $item.ValueCount -eq 0) { Remove-Item -LiteralPath $parent }
    }
}

# RH-2: approved candidate-window renderers, see docs/RENDERER_HANDOFF.md.
# Separate from EngineReleases so older DLLs do not reject extra lines in that
# key. Same HKLM ACL, atomic REG_MULTI_SZ per version, and sequence rollback.
$script:HarukotoRendererReleaseKey = 'HKLM:\SOFTWARE\Harukoto\RendererReleases'
$script:HarukotoRendererReleaseFormat = 'format=harukoto-renderer-release/1'
# Must match CandidatePayload.MinVersion / MaxVersion and cover RendererRelease.h PayloadVersion.
$script:HarukotoRendererPayloadMin = 1
$script:HarukotoRendererPayloadMax = 1
$script:HarukotoRendererReleaseSddl = $script:HarukotoEngineReleaseSddl
$script:HarukotoRendererName = 'MeowpuutGlassPreview.exe'

function New-HarukotoRendererRelease {
    param(
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][long]$Sequence,
        [Parameter(Mandatory)][int]$PayloadMin,
        [Parameter(Mandatory)][int]$PayloadMax,
        [Parameter(Mandatory)][string]$Renderer,
        [Parameter(Mandatory)][string]$RendererSha256
    )
    return [string[]]@(
        $script:HarukotoRendererReleaseFormat,
        "id=$Id",
        "sequence=$Sequence",
        "payload=$PayloadMin-$PayloadMax",
        "renderer=$Renderer",
        "renderer-sha256=$RendererSha256"
    )
}

function Publish-HarukotoRendererRelease {
    param(
        [Parameter(Mandatory)][string]$VersionDirectory,
        [Parameter(Mandatory)][string]$Id,
        [string]$KeyPath = $script:HarukotoRendererReleaseKey,
        [string]$Sddl = $script:HarukotoRendererReleaseSddl,
        [int]$PayloadMin = $script:HarukotoRendererPayloadMin,
        [int]$PayloadMax = $script:HarukotoRendererPayloadMax,
        [ValidateRange(1, 8)][int]$Limit = 4
    )
    if (!(Test-HarukotoEngineReleaseId $Id)) { throw "Invalid renderer release id: $Id" }
    if ($PayloadMin -lt 1 -or $PayloadMax -gt 65535 -or $PayloadMin -gt $PayloadMax) { throw "Invalid renderer payload range: $PayloadMin-$PayloadMax" }
    $folder = [IO.Path]::GetFullPath($VersionDirectory).TrimEnd('\')
    if ((Split-Path $folder -Leaf) -cne $Id) { throw 'The renderer release id must name its version directory.' }
    if ($folder -notmatch '^[A-Za-z]:\\[^\\]') { throw "Renderer must be on a local drive path: $folder" }
    if ((Get-Item -LiteralPath $folder -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing a redirected renderer directory.' }
    $rendererPath = Join-Path $folder $script:HarukotoRendererName
    if (!(Test-Path -LiteralPath $rendererPath -PathType Leaf)) { throw "Missing renderer program: $rendererPath" }
    $file = [pscustomobject]@{ Path = $rendererPath; Sha256 = (Get-FileHash -LiteralPath $rendererPath -Algorithm SHA256).Hash.ToLowerInvariant() }
    if (!(Test-Path -LiteralPath $KeyPath)) {
        $parent = Split-Path $KeyPath
        if (!(Test-Path -LiteralPath $parent)) {
            New-Item -Path $parent -Force | Out-Null
            Set-HarukotoEngineReleaseSecurity -Path $parent -Sddl $Sddl
        }
        New-Item -Path $KeyPath | Out-Null
    }
    Set-HarukotoEngineReleaseSecurity -Path (Split-Path $KeyPath) -Sddl $Sddl
    Set-HarukotoEngineReleaseSecurity -Path $KeyPath -Sddl $Sddl
    $key = Get-Item -LiteralPath $KeyPath
    $existing = @(foreach ($name in $key.GetValueNames()) {
        $lines = $null
        if ($key.GetValueKind($name) -eq [Microsoft.Win32.RegistryValueKind]::MultiString) { $lines = [string[]]$key.GetValue($name) }
        $sequence = Get-HarukotoEngineReleaseField $lines 'sequence'
        [pscustomobject]@{
            Name = $name
            Sequence = if ($sequence -cmatch '^[1-9][0-9]{0,15}$') { [long]$sequence } else { 0L }
            Renderer = Get-HarukotoEngineReleaseField $lines 'renderer'
        }
    })
    $next = 1L
    foreach ($entry in $existing) { if ($entry.Sequence -ge $next) { $next = $entry.Sequence + 1 } }
    $lines = New-HarukotoRendererRelease -Id $Id -Sequence $next -PayloadMin $PayloadMin -PayloadMax $PayloadMax -Renderer $file.Path -RendererSha256 $file.Sha256
    if ((($lines -join "`0").Length + 2) * 2 -gt 8192) { throw 'Renderer release entry exceeds the 8 KiB reader limit.' }
    New-ItemProperty -LiteralPath $KeyPath -Name $Id -PropertyType MultiString -Value $lines -Force | Out-Null
    $key = Get-Item -LiteralPath $KeyPath
    if ($key.GetValueKind($Id) -ne [Microsoft.Win32.RegistryValueKind]::MultiString -or
        (([string[]]$key.GetValue($Id)) -join "`n") -cne ($lines -join "`n")) {
        throw 'Renderer release entry could not be verified after writing.'
    }
    $kept = [Collections.Generic.List[object]]::new()
    foreach ($entry in $existing) {
        if ($entry.Name -ceq $Id) { continue }
        if ($entry.Sequence -gt 0 -and $entry.Renderer -and (Test-Path -LiteralPath $entry.Renderer -PathType Leaf)) { $kept.Add($entry) }
        else { Remove-ItemProperty -LiteralPath $KeyPath -Name $entry.Name }
    }
    $sorted = @($kept | Sort-Object Sequence -Descending)
    for ($i = $Limit - 1; $i -lt $sorted.Count; $i++) { Remove-ItemProperty -LiteralPath $KeyPath -Name $sorted[$i].Name }
    return [pscustomobject]@{ Id = $Id; Sequence = $next; PayloadMin = $PayloadMin; PayloadMax = $PayloadMax; Renderer = $file }
}

function Unpublish-HarukotoRendererRelease {
    param([string]$KeyPath = $script:HarukotoRendererReleaseKey)
    if (Test-Path -LiteralPath $KeyPath) { Remove-Item -LiteralPath $KeyPath -Recurse }
    $parent = Split-Path $KeyPath
    if ((Test-Path -LiteralPath $parent)) {
        $item = Get-Item -LiteralPath $parent
        if ($item.SubKeyCount -eq 0 -and $item.ValueCount -eq 0) { Remove-Item -LiteralPath $parent }
    }
}

# WIN-03 step 4 (docs/ENGINE_LIFECYCLE.md): stop resident engine owners whose
# program lives under $Root before files go away. Owners in this session get
# WM_CLOSE on their hidden window (graceful: the host is stopped first);
# whatever is still running after the timeout, in any session, is terminated
# (the elevated uninstaller may do so). Hosts die with their owner's Job; any
# left under $Root are terminated too. Returns the counts.
$script:HarukotoEngineWindowClass = 'Harukoto.ResidentOwner.v1'
function Stop-HarukotoEngineOwners {
    param([Parameter(Mandatory)][string]$Root, [ValidateRange(1, 60)][int]$TimeoutSeconds = 5)
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $under = {
        param([string]$Name)
        @(Get-Process -Name $Name -ErrorAction SilentlyContinue | Where-Object {
            $path = $null; try { $path = $_.Path } catch { }
            $path -and $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
        })
    }
    $owners = & $under ([IO.Path]::GetFileNameWithoutExtension($script:HarukotoEngineOwnerName))
    $result = [pscustomobject]@{ Graceful = 0; Forced = 0 }
    if ($owners.Count) {
        if (-not ('HarukotoEngineWindows' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class HarukotoEngineWindows {
    delegate bool EnumProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder name, int size);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    // Posts WM_CLOSE to the process's windows of the given class; returns how many.
    public static int Close(uint pid, string cls) {
        int count = 0;
        EnumWindows((window, parameter) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner != pid) return true;
            var name = new StringBuilder(64); GetClassName(window, name, name.Capacity);
            if (name.ToString() == cls && PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero)) count++;
            return true;
        }, IntPtr.Zero);
        return count;
    }
}
'@
        }
        foreach ($owner in $owners) { [void][HarukotoEngineWindows]::Close([uint32]$owner.Id, $script:HarukotoEngineWindowClass) }
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        foreach ($owner in $owners) {
            $left = [int][Math]::Max(0, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
            if ($owner.WaitForExit($left)) { $result.Graceful++ }
            else {
                try { $owner.Kill(); [void]$owner.WaitForExit(5000); $result.Forced++ } catch { Write-Warning "Engine owner $($owner.Id) could not be stopped: $_" }
            }
        }
    }
    foreach ($engineHost in (& $under ([IO.Path]::GetFileNameWithoutExtension($script:HarukotoEngineHostName)))) {
        if (!$engineHost.WaitForExit(2000)) {
            try { $engineHost.Kill(); [void]$engineHost.WaitForExit(5000); $result.Forced++ } catch { Write-Warning "Engine host $($engineHost.Id) could not be stopped: $_" }
        }
    }
    return $result
}
