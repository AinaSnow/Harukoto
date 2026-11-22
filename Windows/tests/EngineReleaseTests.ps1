param([Parameter(Mandatory)][string]$Reader)
# WIN-03 step 1 writer tests. Uses an HKCU test key and a temporary folder
# only; HKLM and Program Files are never written. The C++ reader parses what
# the PowerShell writer produced.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../InstallSupport.ps1')
$script:count = 0
function Assert-True($condition, $message) { if (!$condition) { throw $message } }
function Pass($name) { $script:count++; Write-Host "PASS: $name" }

$header = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../EngineProtocol.h') -Raw
Assert-True ($header -match 'constexpr uint16_t ProtocolVersion = ([0-9]+);') 'ProtocolVersion not found'
Assert-True ([int]$Matches[1] -eq $script:HarukotoEngineProtocol) 'Installer protocol differs from EngineProtocol.h'
Assert-True ($script:HarukotoEngineReleaseKey -ceq 'HKLM:\SOFTWARE\Harukoto\EngineReleases') 'Unexpected production key'
# The production descriptor cannot be applied without elevation; the C++
# test checks this exact string against the reader's protection rule.
$readerTest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'EngineReleaseTests.cpp') -Raw
Assert-True ($readerTest.Contains('L"' + $script:HarukotoEngineReleaseSddl + '"')) 'Production SDDL not covered by the reader test'
Pass 'installer protocol and key match the reader'

$install = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../install-ime.ps1') -Raw
$registration = $install.IndexOf('Invoke-MeowpuutRegistration -Dll')
$publish = $install.IndexOf('Publish-HarukotoEngineRelease -VersionDirectory $install')
Assert-True ($registration -gt 0 -and $publish -gt $registration) 'Engine approval must follow successful registration'
$uninstall = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../uninstall-ime.ps1') -Raw
Assert-True ($uninstall.IndexOf('Unpublish-HarukotoEngineRelease') -gt $uninstall.IndexOf("'/u'")) 'Uninstall must remove approvals after unregistration'
Pass 'install / uninstall call order'

$user = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
# Same shape as production, but owned by the current user (no elevation).
$testSddl = "D:PAI(A;CI;KA;;;$user)(A;CI;KA;;;SY)(A;CI;KR;;;BU)"
$parentName = 'HarukotoTest-' + [guid]::NewGuid().ToString('N')
$parent = "HKCU:\Software\$parentName"
$key = "$parent\EngineReleases"
$subkey = "Software\$parentName\EngineReleases"
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('harukoto-release-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temporary | Out-Null
function New-Version($id, $content) {
    $folder = Join-Path $temporary $id
    New-Item -ItemType Directory $folder | Out-Null
    Set-Content -LiteralPath (Join-Path $folder $script:HarukotoEngineHostName) -Value "host-$content" -Encoding ascii
    Set-Content -LiteralPath (Join-Path $folder $script:HarukotoEngineOwnerName) -Value "owner-$content" -Encoding ascii
    # Step 6c: a release needs the manifest that makes the folder a base snapshot.
    Set-Content -LiteralPath (Join-Path $folder 'base-dictionary.json') -Value '{}' -Encoding ascii
    return $folder
}
function Read-Releases {
    $previous = [Console]::OutputEncoding
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
    try { $output = & $Reader --dump $subkey $script:HarukotoEngineProtocol } finally { [Console]::OutputEncoding = $previous }
    Assert-True ($LASTEXITCODE -eq 0) "Reader failed: $LASTEXITCODE"
    $result = [pscustomobject]@{ Status = ''; Rejected = 0; Entries = @{}; Selected = '' }
    foreach ($line in $output) {
        $field = $line -split "`t"
        switch ($field[0]) {
            'status' { $result.Status = $field[1]; $result.Rejected = [int]$field[2] }
            'entry' { $result.Entries[$field[1]] = [pscustomobject]@{ Sequence = [long]$field[2]; Protocol = [int]$field[3]; Host = $field[4]; HostSha256 = $field[5]; Owner = $field[6]; OwnerSha256 = $field[7] } }
            'selected' { $result.Selected = $field[1] }
        }
    }
    return $result
}
function Publish($folder, $limit = 4) {
    Publish-HarukotoEngineRelease -VersionDirectory $folder -Id (Split-Path $folder -Leaf) -KeyPath $key -Sddl $testSddl -Limit $limit
}
try {
    Assert-True ((Read-Releases).Status -eq 'missing') 'Absent key must read as missing'
    $a = New-Version 'AAAA0001-1111' 'a'
    $published = Publish $a
    $state = Read-Releases
    $entry = $state.Entries['AAAA0001-1111']
    Assert-True ($state.Status -eq 'ready' -and $state.Entries.Count -eq 1 -and $state.Selected -eq 'AAAA0001-1111') 'First publication not readable'
    Assert-True ($entry.Sequence -eq 1 -and $entry.Protocol -eq 1 -and $published.Sequence -eq 1) 'First sequence'
    Assert-True ($entry.Host -ceq (Join-Path $a $script:HarukotoEngineHostName) -and $entry.Owner -ceq (Join-Path $a $script:HarukotoEngineOwnerName)) 'Approved paths'
    Assert-True ($entry.HostSha256 -ceq (Get-FileHash -LiteralPath $entry.Host).Hash.ToLowerInvariant()) 'Host digest'
    Assert-True ($entry.OwnerSha256 -ceq (Get-FileHash -LiteralPath $entry.Owner).Hash.ToLowerInvariant()) 'Owner digest'
    Assert-True ((Get-Item -LiteralPath $key).GetValueKind('AAAA0001-1111') -eq 'MultiString') 'Value kind'
    Pass 'first install publishes one atomic REG_MULTI_SZ entry read by the C++ reader'

    foreach ($path in $parent, $key) {
        $acl = Get-Acl -LiteralPath $path
        Assert-True ($acl.AreAccessRulesProtected) "Inherited rules not removed: $path"
        Assert-True ($acl.Sddl.Contains('(A;CI;KR;;;BU)') -and !$acl.Sddl.Contains('(A;CI;KA;;;BU)')) "Unexpected descriptor: $($acl.Sddl)"
    }
    Pass 'explicit non-inherited key descriptor applied to the key and its parent'

    $b = New-Version 'BBBB0002-2222' 'b'
    Publish $b | Out-Null
    $state = Read-Releases
    Assert-True ($state.Entries.Count -eq 2 -and $state.Entries['BBBB0002-2222'].Sequence -eq 2 -and $state.Selected -eq 'BBBB0002-2222') 'Upgrade window'
    Pass 'upgrade keeps the previous version and selects the newer one'

    Publish $a | Out-Null
    $state = Read-Releases
    Assert-True ($state.Entries['AAAA0001-1111'].Sequence -eq 3 -and $state.Selected -eq 'AAAA0001-1111' -and $state.Entries.Count -eq 2) 'Reinstall of older version'
    Pass 'reinstalling an older package becomes the selected release'

    $before = (Get-Item -LiteralPath $key).GetValue('AAAA0001-1111') -join '|'
    $broken = New-Version 'CCCC0003-3333' 'c'
    Remove-Item -LiteralPath (Join-Path $broken $script:HarukotoEngineOwnerName)
    $failure = ''
    try { Publish $broken | Out-Null } catch { $failure = $_.Exception.Message }
    Assert-True ($failure.Contains('Missing engine program')) "Missing owner must fail: $failure"
    $unlisted = New-Version '9999ABCD-0000' 'x'
    Remove-Item -LiteralPath (Join-Path $unlisted 'base-dictionary.json')
    $failure = ''
    try { Publish $unlisted | Out-Null } catch { $failure = $_.Exception.Message }
    Assert-True ($failure.Contains('base-dictionary.json')) "A version without its base manifest must fail: $failure"
    foreach ($case in @(@{ Folder = $a; Id = 'Other' }, @{ Folder = $a; Id = '-bad' })) {
        $failure = ''
        try { Publish-HarukotoEngineRelease -VersionDirectory $case.Folder -Id $case.Id -KeyPath $key -Sddl $testSddl | Out-Null } catch { $failure = $_.Exception.Message }
        Assert-True ([bool]$failure) "Invalid id must fail: $($case.Id)"
    }
    $state = Read-Releases
    Assert-True ($state.Entries.Count -eq 2 -and ((Get-Item -LiteralPath $key).GetValue('AAAA0001-1111') -join '|') -eq $before) 'Failed publication changed metadata'
    Pass 'failed publication leaves existing approvals unchanged'

    New-ItemProperty -LiteralPath $key -Name 'CORRUPT' -PropertyType String -Value 'garbage' | Out-Null
    Remove-Item -LiteralPath (Join-Path $b $script:HarukotoEngineHostName)
    $state = Read-Releases
    Assert-True ($state.Rejected -eq 1 -and $state.Entries.Count -eq 2) 'Reader must reject the corrupt value only'
    $d = New-Version 'DDDD0004-4444' 'd'
    Publish $d | Out-Null
    $names = @((Get-Item -LiteralPath $key).GetValueNames() | Sort-Object)
    Assert-True (($names -join ',') -eq 'AAAA0001-1111,DDDD0004-4444') "Prune result: $($names -join ',')"
    $state = Read-Releases
    Assert-True ($state.Rejected -eq 0 -and $state.Selected -eq 'DDDD0004-4444' -and $state.Entries['DDDD0004-4444'].Sequence -eq 4) 'After prune'
    Pass 'corrupt entries and removed versions are pruned on the next install'

    $e = New-Version 'EEEE0005-5555' 'e'
    $f = New-Version 'FFFF0006-6666' 'f'
    Publish $e | Out-Null
    Publish $f 2 | Out-Null
    $names = @((Get-Item -LiteralPath $key).GetValueNames() | Sort-Object)
    Assert-True (($names -join ',') -eq 'EEEE0005-5555,FFFF0006-6666') "Limit result: $($names -join ',')"
    Assert-True ((Read-Releases).Selected -eq 'FFFF0006-6666') 'Limit selection'
    Pass 'entry count stays within the limit, lowest sequences removed first'

    Unpublish-HarukotoEngineRelease -KeyPath $key
    Assert-True (!(Test-Path -LiteralPath $key) -and !(Test-Path -LiteralPath $parent)) 'Uninstall must remove the key and empty parent'
    Assert-True ((Read-Releases).Status -eq 'missing') 'Removed key must read as missing'
    Unpublish-HarukotoEngineRelease -KeyPath $key
    Pass 'uninstall removes all approvals and is idempotent'

    # WIN-03 step 4: uninstall stops resident owners under the install root.
    # The graceful WM_CLOSE path runs against a real owner in engine_resident;
    # here a windowless stand-in under the root must be terminated, one with
    # the same name outside the root must be left alone.
    $unregister = $uninstall.IndexOf('Unpublish-HarukotoEngineRelease')
    $stop = $uninstall.IndexOf('Stop-HarukotoEngineOwners -Root $root')
    Assert-True ($stop -gt $unregister) 'Uninstall must stop owners after removing approvals'
    Assert-True ($install.IndexOf('Stop-HarukotoEngineOwners') -lt 0) 'Upgrade must not stop running owners (step 7 switches generations)'
    $none = Stop-HarukotoEngineOwners -Root (Join-Path $temporary 'nothing-here')
    Assert-True ($none.Graceful -eq 0 -and $none.Forced -eq 0) 'Nothing to stop'
    $inside = Join-Path $temporary 'root\v1'; $outside = Join-Path $temporary 'elsewhere'
    New-Item -ItemType Directory $inside, $outside | Out-Null
    $ping = Join-Path $env:WINDIR 'System32\PING.EXE'
    $standIns = foreach ($folder in $inside, $outside) {
        $copy = Join-Path $folder $script:HarukotoEngineOwnerName
        Copy-Item -LiteralPath $ping -Destination $copy
        Start-Process -FilePath $copy -ArgumentList '-n','60','127.0.0.1' -WindowStyle Hidden -PassThru
    }
    try {
        $stopped = Stop-HarukotoEngineOwners -Root (Join-Path $temporary 'root') -TimeoutSeconds 1
        Assert-True ($stopped.Graceful -eq 0 -and $stopped.Forced -eq 1) "Stop counts: $($stopped.Graceful)/$($stopped.Forced)"
        Assert-True ($standIns[0].WaitForExit(5000)) 'Owner under the root must be stopped'
        Assert-True (!$standIns[1].HasExited) 'Same name outside the root must keep running'
    } finally { foreach ($p in $standIns) { if (!$p.HasExited) { $p.Kill(); [void]$p.WaitForExit(5000) } } }
    Pass 'uninstall stops owners under its root only, forcing those that do not close'
} finally {
    if (Test-Path -LiteralPath $parent) { Remove-Item -LiteralPath $parent -Recurse }
    $resolved = (Resolve-Path -LiteralPath $temporary).Path
    if ([IO.Path]::GetDirectoryName($resolved) -eq [IO.Path]::GetTempPath().TrimEnd([IO.Path]::DirectorySeparatorChar)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
Write-Host "PASS: $count engine release writer checks. Only an HKCU test key was written."
