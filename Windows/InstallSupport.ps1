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
