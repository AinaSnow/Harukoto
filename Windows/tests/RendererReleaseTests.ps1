param([Parameter(Mandatory)][string]$Reader)
# RH-2 writer tests. Uses an HKCU test key and a temporary folder only; HKLM
# and Program Files are never written. The C++ reader parses what the
# PowerShell writer produced, and all three payload-version constants are locked.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../InstallSupport.ps1')
$script:count = 0
function Assert-True($condition, $message) { if (!$condition) { throw $message } }
function Pass($name) { $script:count++; Write-Host "PASS: $name" }

# 3-place payload version constant lock: CandidatePayload.cs, RendererRelease.h, InstallSupport.ps1.
$cs = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../CandidatePayload.cs') -Raw
Assert-True ($cs -match 'internal const int MinVersion = ([0-9]+);') 'CandidatePayload.MinVersion not found'
$csMin = [int]$Matches[1]
Assert-True ($cs -match 'internal const int MaxVersion = ([0-9]+);') 'CandidatePayload.MaxVersion not found'
$csMax = [int]$Matches[1]
$cpp = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../RendererRelease.h') -Raw
Assert-True ($cpp -match 'constexpr uint16_t PayloadVersion = ([0-9]+);') 'RendererRelease.h PayloadVersion not found'
$cppVer = [int]$Matches[1]
Assert-True ($csMin -eq $script:HarukotoRendererPayloadMin -and $csMax -eq $script:HarukotoRendererPayloadMax) "Installer payload range ($($script:HarukotoRendererPayloadMin)-$($script:HarukotoRendererPayloadMax)) differs from CandidatePayload.cs ($csMin-$csMax)"
Assert-True ($cppVer -ge $csMin -and $cppVer -le $csMax) "DLL PayloadVersion ($cppVer) is outside renderer range ($csMin-$csMax)"
Assert-True ($script:HarukotoRendererReleaseKey -ceq 'HKLM:\SOFTWARE\Harukoto\RendererReleases') 'Unexpected renderer production key'
$engineTest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'EngineReleaseTests.cpp') -Raw
Assert-True ($engineTest.Contains('L"' + $script:HarukotoRendererReleaseSddl + '"')) 'Production SDDL not covered by the reader test'
Pass 'C#, C++ and installer payload constants and production key match'

$install = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../install-ime.ps1') -Raw
$registration = $install.IndexOf('Invoke-MeowpuutRegistration -Dll')
$publish = $install.IndexOf('Publish-HarukotoRendererRelease -VersionDirectory $install')
Assert-True ($registration -gt 0 -and $publish -gt $registration) 'Renderer approval must follow successful registration'
$uninstall = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../uninstall-ime.ps1') -Raw
Assert-True ($uninstall.IndexOf('Unpublish-HarukotoRendererRelease') -gt $uninstall.IndexOf("'/u'")) 'Uninstall must remove renderer approvals after unregistration'
Pass 'install / uninstall call order'

$user = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$testSddl = "D:PAI(A;CI;KA;;;$user)(A;CI;KA;;;SY)(A;CI;KR;;;BU)"
$parentName = 'HarukotoTest-' + [guid]::NewGuid().ToString('N')
$parent = "HKCU:\Software\$parentName"
$key = "$parent\RendererReleases"
$subkey = "Software\$parentName\RendererReleases"
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('harukoto-renderer-release-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temporary | Out-Null

function New-Version($id, $content) {
    $folder = Join-Path $temporary $id
    New-Item -ItemType Directory $folder | Out-Null
    Set-Content -LiteralPath (Join-Path $folder $script:HarukotoRendererName) -Value "renderer-$content" -Encoding ascii
    return $folder
}
function Read-Releases($version = $cppVer) {
    $previous = [Console]::OutputEncoding
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
    try { $output = & $Reader --dump $subkey $version } finally { [Console]::OutputEncoding = $previous }
    Assert-True ($LASTEXITCODE -eq 0) "Reader failed: $LASTEXITCODE"
    $result = [pscustomobject]@{ Status = ''; Rejected = 0; Entries = @{}; Selected = '' }
    foreach ($line in $output) {
        $field = $line -split "`t"
        switch ($field[0]) {
            'status' { $result.Status = $field[1]; $result.Rejected = [int]$field[2] }
            'entry' { $result.Entries[$field[1]] = [pscustomobject]@{ Sequence = [long]$field[2]; Min = [int]$field[3]; Max = [int]$field[4]; Renderer = $field[5]; Sha256 = $field[6] } }
            'selected' { $result.Selected = $field[1] }
        }
    }
    return $result
}
function Publish($folder, $limit = 4) {
    Publish-HarukotoRendererRelease -VersionDirectory $folder -Id (Split-Path $folder -Leaf) -KeyPath $key -Sddl $testSddl -Limit $limit
}

try {
    Assert-True ((Read-Releases).Status -eq 'missing') 'Absent key must read as missing'
    $a = New-Version 'AAAA0001-1111' 'a'
    $published = Publish $a
    $state = Read-Releases
    $entry = $state.Entries['AAAA0001-1111']
    Assert-True ($state.Status -eq 'ready' -and $state.Entries.Count -eq 1 -and $state.Selected -eq 'AAAA0001-1111') 'First publication not readable'
    Assert-True ($entry.Sequence -eq 1 -and $entry.Min -eq $csMin -and $entry.Max -eq $csMax -and $published.Sequence -eq 1) 'First sequence and range'
    Assert-True ($entry.Renderer -ceq (Join-Path $a $script:HarukotoRendererName)) 'Approved path'
    Assert-True ($entry.Sha256 -ceq (Get-FileHash -LiteralPath $entry.Renderer).Hash.ToLowerInvariant()) 'Renderer digest'
    Assert-True ((Get-Item -LiteralPath $key).GetValueKind('AAAA0001-1111') -eq 'MultiString') 'Value kind'
    Pass 'first install publishes one atomic REG_MULTI_SZ entry read by the C++ reader'

    $b = New-Version 'BBBB0002-2222' 'b'
    Publish $b | Out-Null
    $state = Read-Releases
    Assert-True ($state.Entries.Count -eq 2 -and $state.Entries['BBBB0002-2222'].Sequence -eq 2 -and $state.Selected -eq 'BBBB0002-2222') 'Upgrade coexistence'
    Pass 'upgrade keeps previous version and selects newer one'

    Publish $a | Out-Null
    $state = Read-Releases
    Assert-True ($state.Entries['AAAA0001-1111'].Sequence -eq 3 -and $state.Selected -eq 'AAAA0001-1111' -and $state.Entries.Count -eq 2) 'Rollback reinstall'
    Pass 'reinstalling older version becomes the selected release'

    $before = (Get-Item -LiteralPath $key).GetValue('AAAA0001-1111') -join '|'
    $broken = Join-Path $temporary 'CCCC0003-3333'
    New-Item -ItemType Directory $broken | Out-Null
    $failure = ''
    try { Publish $broken | Out-Null } catch { $failure = $_.Exception.Message }
    Assert-True ($failure.Contains('Missing renderer program')) "Missing renderer must fail: $failure"
    $state = Read-Releases
    Assert-True ($state.Entries.Count -eq 2 -and ((Get-Item -LiteralPath $key).GetValue('AAAA0001-1111') -join '|') -eq $before) 'Failed publication changed metadata'
    Pass 'failed publication leaves existing approvals unchanged'

    New-ItemProperty -LiteralPath $key -Name 'CORRUPT' -PropertyType String -Value 'garbage' | Out-Null
    Remove-Item -LiteralPath (Join-Path $b $script:HarukotoRendererName)
    $state = Read-Releases
    Assert-True ($state.Rejected -eq 1 -and $state.Entries.Count -eq 2) 'Reader rejects corrupt entry only'
    $d = New-Version 'DDDD0004-4444' 'd'
    Publish $d | Out-Null
    $names = @((Get-Item -LiteralPath $key).GetValueNames() | Sort-Object)
    Assert-True (($names -join ',') -eq 'AAAA0001-1111,DDDD0004-4444') "Prune result: $($names -join ',')"
    Assert-True ((Read-Releases).Selected -eq 'DDDD0004-4444') 'After prune'
    Pass 'corrupt entries and removed versions are pruned on next install'

    $e = New-Version 'EEEE0005-5555' 'e'
    $f = New-Version 'FFFF0006-6666' 'f'
    Publish $e | Out-Null
    Publish $f 2 | Out-Null
    $names = @((Get-Item -LiteralPath $key).GetValueNames() | Sort-Object)
    Assert-True (($names -join ',') -eq 'EEEE0005-5555,FFFF0006-6666') "Limit result: $($names -join ',')"
    Pass 'entry count stays within limit, lowest sequences pruned first'

    Unpublish-HarukotoRendererRelease -KeyPath $key
    Assert-True (!(Test-Path -LiteralPath $key) -and !(Test-Path -LiteralPath $parent)) 'Uninstall must remove key and empty parent'
    Assert-True ((Read-Releases).Status -eq 'missing') 'Removed key must read as missing'
    Unpublish-HarukotoRendererRelease -KeyPath $key
    Pass 'uninstall removes approvals and is idempotent'
} finally {
    if (Test-Path -LiteralPath $parent) { Remove-Item -LiteralPath $parent -Recurse }
    $resolved = (Resolve-Path -LiteralPath $temporary).Path
    if ([IO.Path]::GetDirectoryName($resolved) -eq [IO.Path]::GetTempPath().TrimEnd([IO.Path]::DirectorySeparatorChar)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
Write-Host "PASS: $count renderer release writer checks. Only an HKCU test key was written."
