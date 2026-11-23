#requires -Version 7.0
param([Parameter(Mandatory)][string]$Writer)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../DiagnosisSupport.ps1')
Add-Type -Path (Join-Path $PSScriptRoot '../RuntimeStatusReader.cs')
$start = [Diagnostics.ProcessStartInfo]::new($Writer)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$process = [Diagnostics.Process]::Start($start)
function Check($Condition, $Message) { if (!$Condition) { throw $Message } }
function Read-Status { Get-HarukotoRuntimeStatus $process ('a' * 64) }
function Send-Command($Text) {
    $process.StandardInput.WriteLine($Text); $process.StandardInput.Flush()
    Check ($process.StandardOutput.ReadLine() -eq 'done') 'Writer did not acknowledge command'
}
try {
    Check ($process.StandardOutput.ReadLine() -eq 'ready') 'Writer startup failed'
    $status = Read-Status
    Check ($status.Instances.Count -eq 2) 'Missing live slots'
    $first = @($status.Instances | Where-Object BaseGeneration -eq ('a' * 64))[0]
    Check ($first.Publication -eq '已采用发布版本') 'Published generation comparison failed'
    Check ($first.Renderer -eq '桌面渲染器；载荷 v1，版本序号 5，换代 2 次，失败 0 次') "Renderer field mismatch: $($first.Renderer)"
    Check (@($status.Instances | Where-Object Publication -eq '尚未采用当前发布版本').Count -eq 1) 'Pending generation reported as active'
    Send-Command 'close'
    Check ((Read-Status).Instances.Count -eq 1) 'Closed instance still reported'
    Send-Command 'reuse'
    $status = Read-Status
    Check ($status.Instances.Count -eq 2) 'Slot reuse failed'
    Check (@($status.Instances | Where-Object Phase -eq '已准备，等待组字边界').Count -eq 1) 'Phase lost'
    Send-Command 'capacity'
    Check ((Read-Status).Instances.Count -eq 2) 'Capacity handling changed existing reports'
    # Read concurrently with native writes; accepted records must be whole.
    $process.StandardInput.WriteLine('stress'); $process.StandardInput.Flush()
    for ($i = 0; $i -lt 100; $i++) {
        $rows = [HarukotoRuntimeStatusReader]::Read($process.Id, $process.StartTime.ToUniversalTime().ToFileTimeUtc())
        foreach ($row in $rows) {
            $record = $row | ConvertFrom-Json
            if ($record.slot_id -eq $first.SlotId -and $record.phase -in 'idle','failed') {
                $expected = if ($record.phase -eq 'idle') {'a' * 64} else {'b' * 64}
                Check ($record.base_generation -ceq $expected) 'Torn runtime record'
            }
        }
    }
    Check ($process.StandardOutput.ReadLine() -eq 'done') 'Stress writer failed'
    $sample = ([HarukotoRuntimeStatusReader]::Read($process.Id, $process.StartTime.ToUniversalTime().ToFileTimeUtc()))[0] | ConvertFrom-Json -AsHashtable
    foreach ($field in 'base_generation','phase','observed_tick_ms') {
        $bad = $sample.Clone(); $bad[$field] = 'invalid'
        $rejected = $false
        try { Convert-HarukotoRuntimeRecord ($bad | ConvertTo-Json -Compress) ('a' * 64) | Out-Null } catch { $rejected = $true }
        Check $rejected "Malformed $field accepted"
    }
    $process.StandardInput.WriteLine('exit'); $process.StandardInput.Flush()
    Check ($process.WaitForExit(10000)) 'Writer shutdown timeout'
    Check ($process.ExitCode -eq 0) 'Writer failed'
    Check ((Read-Status).Instances.Count -eq 0) 'Exited process confirmed as live'
    Write-Output 'PASS: native/PS runtime reports, multiple instances, generation comparison, close/reuse, concurrent reads and invalid records'
} finally {
    if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
}
