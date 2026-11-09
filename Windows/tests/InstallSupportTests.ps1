$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../InstallSupport.ps1')
$script:count = 0
function Assert-True($condition, $message) {
    if (!$condition) { throw $message }
}
function Test-Registration($name, $previous, $behavior, $cleanupFails, $expectedCalls, $expectedError) {
    $state = @{ Registered = $previous; Calls = [Collections.Generic.List[string]]::new(); Messages = [Collections.Generic.List[string]]::new() }
    $register = { param($path)
        $state.Calls.Add("register:$path")
        if ($path -eq 'new.dll') {
            switch ($behavior) {
                'success' { $state.Registered = $path; return 0 }
                'mismatch' { $state.Registered = 'wrong.dll'; return 0 }
                'throw' { $state.Registered = $null; throw 'Simulated process failure' }
                default { $state.Registered = $null; return 5 }
            }
        }
        if ($behavior -eq 'recovery-failure') { return 5 }
        $state.Registered = $path
        return 0
    }
    $unregister = { param($path)
        $state.Calls.Add("unregister:$path")
        if ($cleanupFails) { return 5 }
        $state.Registered = $null
        return 0
    }
    $failure = ''
    try {
        Invoke-MeowpuutRegistration -Dll 'new.dll' -PreviousDll $previous -RegisterServer $register -UnregisterServer $unregister -GetRegisteredDll { $state.Registered } -Log { param($text) $state.Messages.Add($text) }
    } catch { $failure = $_.Exception.Message }
    Assert-True (($state.Calls -join '|') -eq $expectedCalls) "$name call order: $($state.Calls -join '|')"
    if ($expectedError) { Assert-True ($failure.Contains($expectedError)) "$name failure not reported: $failure" }
    else { Assert-True (!$failure) "$name unexpectedly failed: $failure" }
    if ($expectedError -eq 'Recovery completed' -and $previous) {
        Assert-True ($state.Registered -eq $previous) "$name did not restore previous version"
    }
    Assert-True ($state.Messages.Count -ge 2) "$name missing progress log"
    $script:count++
    Write-Host "PASS: $name"
}
Test-Registration 'first installation' '' 'success' $false 'register:new.dll' ''
Test-Registration 'upgrade success' 'old.dll' 'success' $false 'register:new.dll' ''
Test-Registration 'same version repair' 'new.dll' 'success' $false 'register:new.dll' ''
Test-Registration 'upgrade rollback' 'old.dll' 'failure' $false 'register:new.dll|register:old.dll' 'Recovery completed'
Test-Registration 'wrong registry path rollback' 'old.dll' 'mismatch' $false 'register:new.dll|register:old.dll' 'Recovery completed'
Test-Registration 'process exception rollback' 'old.dll' 'throw' $false 'register:new.dll|register:old.dll' 'Recovery completed'
Test-Registration 'rollback failure reported' 'old.dll' 'recovery-failure' $false 'register:new.dll|register:old.dll' 'Recovery also failed'
Test-Registration 'first installation cleanup' '' 'failure' $false 'register:new.dll|unregister:new.dll' 'Recovery completed'
Test-Registration 'cleanup failure reported' '' 'failure' $true 'register:new.dll|unregister:new.dll' 'Recovery also failed'

$temporary = Join-Path ([IO.Path]::GetTempPath()) ('meowpuut-install-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$lock = $null
try {
    $source = Join-Path $temporary 'source.dll'
    $destination = Join-Path $temporary 'version'
    Set-Content -LiteralPath $source -Value 'version-one'
    Copy-MeowpuutPayload -Files @($source) -Destination $destination
    $target = Join-Path $destination 'source.dll'
    Assert-True ((Get-FileHash $target).Hash -eq (Get-FileHash $source).Hash) 'Initial payload copy failed'
    $count++
    Write-Host 'PASS: payload copy SHA-256'
    $lock = [IO.File]::Open($target, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    Copy-MeowpuutPayload -Files @($source) -Destination $destination
    $count++
    Write-Host 'PASS: matching locked file reused without overwrite'
    Set-Content -LiteralPath $source -Value 'different-content'
    $failure = ''
    try { Copy-MeowpuutPayload -Files @($source) -Destination $destination } catch { $failure = $_.Exception.Message }
    Assert-True ($failure.Contains('was not overwritten')) 'Changed existing version must be rejected'
    Assert-True ((Get-Content -LiteralPath $target -Raw).Trim() -eq 'version-one') 'Existing version was modified'
    $count++
    Write-Host 'PASS: mismatched locked file preserved'
    $lock.Dispose(); $lock = $null
    $lock = [IO.File]::Open($target, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $failure = ''
    try { Copy-MeowpuutPayload -Files @($source) -Destination $destination } catch { $failure = $_.Exception.Message }
    Assert-True ([bool]$failure) 'Exclusively locked file must stop installation'
    $count++
    Write-Host 'PASS: exclusive lock stops payload installation'
    $lock.Dispose(); $lock = $null

    # Execute the real entry script against an incomplete package, without reaching UAC.
    $package = Join-Path $temporary 'package'
    New-Item -ItemType Directory -Path $package | Out-Null
    foreach ($name in 'install-ime.ps1','PowerShellHost.ps1','InstallSupport.ps1','InputProfile.ps1') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../$name") -Destination $package
    }
    $processInfo = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSHOME 'pwsh.exe'))
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    foreach ($argument in '-NoProfile','-File',(Join-Path $package 'install-ime.ps1'),'-Prebuilt') { $processInfo.ArgumentList.Add($argument) }
    $processInfo.Environment['LOCALAPPDATA'] = $temporary
    $process = [Diagnostics.Process]::Start($processInfo)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout.GetAwaiter().GetResult() | Out-Null
    $stderr.GetAwaiter().GetResult() | Out-Null
    Assert-True ($process.ExitCode -ne 0) 'Incomplete package should fail'
    $process.Dispose()
    $logs = @(Get-ChildItem -LiteralPath (Join-Path $temporary 'Meowpuut/Logs') -Filter '*.log')
    Assert-True ($logs.Count -eq 1) 'Expected one failure transcript'
    $log = Get-Content -LiteralPath $logs[0].FullName -Raw
    Assert-True ($log.Contains('Installation failed:') -and $log.Contains('Build the Windows IME')) 'Failure transcript did not capture the cause'
    $count++
    Write-Host 'PASS: real installer preflight failure produces diagnostic transcript'
} finally {
    if ($lock) { $lock.Dispose() }
    $resolved = (Resolve-Path -LiteralPath $temporary).Path
    if ([IO.Path]::GetDirectoryName($resolved) -eq [IO.Path]::GetTempPath().TrimEnd([IO.Path]::DirectorySeparatorChar)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
Write-Host "PASS: $count installer checks. Registration calls were simulated; no system registration changed."
