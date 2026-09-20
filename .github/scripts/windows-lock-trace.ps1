# Diagnostic only: never changes the build command, retries it, or copies its DLLs.
# Raw machine-wide PML/CSV stay in RUNNER_TEMP; only relevant file metadata is published.
[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('Start', 'Stop')][string]$Phase)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if (-not $IsWindows) { throw 'This diagnostic requires native Windows.' }
$trace = Join-Path $env:RUNNER_TEMP "turbodb-locktrace-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT"
$evidence = Join-Path $env:GITHUB_WORKSPACE 'evidence/windows-lock-trace'
$pm = Join-Path $trace 'Procmon64.exe'
$pml = Join-Path $trace 'capture.pml'
$csv = Join-Path $trace 'capture.csv'
$probe = Join-Path $trace 'locked-probe.exe'
$stateFile = Join-Path $evidence 'trace.json'
New-Item -ItemType Directory -Force -Path $evidence | Out-Null

function Invoke-TraceCommand([string]$Arguments, [int]$Seconds) {
    $process = Start-Process -FilePath $pm -ArgumentList $Arguments -PassThru
    if (-not $process.WaitForExit($Seconds * 1000)) {
        $process.Kill()
        throw "Process Monitor command timed out: $Arguments"
    }
    if ($process.ExitCode -ne 0) { throw "Process Monitor exit $($process.ExitCode): $Arguments" }
}

if ($Phase -eq 'Start') {
    if (Get-Process -Name Procmon, Procmon64, Procmon64a -ErrorAction SilentlyContinue) {
        throw 'Refusing to interfere with an existing Process Monitor session.'
    }
    New-Item -ItemType Directory -Force -Path $trace | Out-Null
    $archive = Join-Path $trace 'ProcessMonitor.zip'
    Invoke-WebRequest 'https://download.sysinternals.com/files/ProcessMonitor.zip' -OutFile $archive -TimeoutSec 60
    Expand-Archive -LiteralPath $archive -DestinationPath $trace
    $signature = Get-AuthenticodeSignature -FilePath $pm
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(?:,|$)') {
        throw 'Process Monitor must have a valid Microsoft Authenticode signature.'
    }
    $vcpkg = Join-Path $env:DRIVER_SDK_VCPKG_ROOT 'vcpkg.exe'
    & $vcpkg version *> (Join-Path $evidence 'vcpkg-version.log')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify vcpkg executable.' }
    $started = [DateTime]::UtcNow
    $state = [ordered]@{
        head = $env:GITHUB_SHA; run_id = $env:GITHUB_RUN_ID; attempt = $env:GITHUB_RUN_ATTEMPT
        started_utc = $started.ToString('o'); max_capture_seconds = 600
        procmon_version = (Get-Item -LiteralPath $pm).VersionInfo.FileVersion
        procmon_sha256 = (Get-FileHash -LiteralPath $pm -Algorithm SHA256).Hash
        procmon_signer = $signature.SignerCertificate.Subject
        archive_sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        vcpkg_sha256 = (Get-FileHash -LiteralPath $vcpkg -Algorithm SHA256).Hash
        vcpkg_revision = $env:DRIVER_SDK_VCPKG_REVISION
        probe_path = $probe; status = 'starting'
    }
    $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding utf8
    $capture = Start-Process -FilePath $pm -ArgumentList "/AcceptEula /Quiet /Minimized /NoFilter /BackingFile `"$pml`" /Runtime 600" -PassThru
    $state.capture_pid = $capture.Id
    $state.status = 'capturing'
    $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding utf8
    'started=true' >> $env:GITHUB_OUTPUT
    Invoke-TraceCommand '/AcceptEula /Quiet /WaitForIdle' 30

    # Controlled negative/positive pair checks the observer, not the Salts build.
    # The copied system executable is inspected as PE data, never executed.
    Copy-Item -LiteralPath (Join-Path $env:WINDIR 'System32/where.exe') -Destination $probe
    $empty = Join-Path $trace 'empty-bin'
    New-Item -ItemType Directory -Path $empty | Out-Null
    $hold = [IO.File]::Open($probe, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        & $vcpkg z-applocal "--target-binary=$probe" "--installed-bin-dir=$empty" --debug *> (Join-Path $evidence 'probe-locked.log')
        $state.probe_locked_exit = $LASTEXITCODE
    } finally { $hold.Dispose() }
    & $vcpkg z-applocal "--target-binary=$probe" "--installed-bin-dir=$empty" --debug *> (Join-Path $evidence 'probe-unlocked.log')
    $state.probe_unlocked_exit = $LASTEXITCODE
    $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding utf8
    if ($state.probe_locked_exit -eq 0 -or $state.probe_unlocked_exit -ne 0) {
        throw 'The isolated locked/unlocked probe did not produce failure/success as expected.'
    }
    Write-Host 'Trace active; controlled lock fails and unlocked control succeeds. Normal build follows unchanged.'
    exit 0
}

$state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json -AsHashtable
try {
    Invoke-TraceCommand '/AcceptEula /Quiet /Terminate' 30
    $capture = Get-Process -Id $state.capture_pid -ErrorAction SilentlyContinue
    if ($capture -and -not $capture.WaitForExit(30000)) { throw 'Capture did not finish flushing.' }
    $state.stopped_utc = [DateTime]::UtcNow.ToString('o')
    $elapsed = ([DateTime]::Parse($state.stopped_utc) - [DateTime]::Parse($state.started_utc)).TotalSeconds
    $state.within_capture_time_bound = $elapsed -lt $state.max_capture_seconds
    Invoke-TraceCommand "/AcceptEula /Quiet /Minimized /OpenLog `"$pml`" /SaveAs `"$csv`"" 180
    if (-not (Test-Path -LiteralPath $csv)) { throw 'Process Monitor did not export a CSV.' }

    $roots = @($env:GITHUB_WORKSPACE, $env:DRIVER_SDK_VCPKG_ROOT, $trace) | ForEach-Object { $_.TrimEnd('\') + '\' }
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $processIds = [Collections.Generic.HashSet[string]]::new()
    $columns = @('Time of Day', 'Process Name', 'PID', 'Operation', 'Path', 'Result', 'Detail')
    $state.probe_sharing_events = 0
    $state.build_access_failures = 0
    $state.csv_sha256 = (Get-FileHash -LiteralPath $csv -Algorithm SHA256).Hash
    Import-Csv -LiteralPath $csv | Where-Object {
        $event = $_
        $inScope = $false
        foreach ($root in $roots) {
            if ($event.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { $inScope = $true; break }
        }
        $inScope -and $event.Result -match '^(SHARING VIOLATION|LOCK VIOLATION|ACCESS DENIED)$'
    } | ForEach-Object {
        [void]$paths.Add($_.Path)
        [void]$processIds.Add($_.PID)
        if ($_.Path -eq $probe -and $_.Result -eq 'SHARING VIOLATION' -and $_.'Process Name' -eq 'vcpkg.exe') {
            $state.probe_sharing_events++
        } elseif (-not $_.Path.StartsWith($trace, [StringComparison]::OrdinalIgnoreCase)) {
            $state.build_access_failures++
        }
        $_ | Select-Object -Property $columns
    } | Export-Csv -LiteralPath (Join-Path $evidence 'access-failures.csv') -NoTypeInformation -Encoding utf8

    # Include preceding opens and subsequent closes from all processes on failed paths.
    # Never export process-start environments, registry values, or the raw system trace.
    Import-Csv -LiteralPath $csv | Where-Object {
        ($paths.Contains($_.Path) -and $_.Operation -match '^(CreateFile|CreateFileMapping|ReadFile|WriteFile|CloseFile|QueryOpen|FlushBuffersFile|Load Image|Set.*File|Query.*File)$') -or
        ($_.Operation -eq 'Process Exit' -and $processIds.Contains($_.PID))
    } | Select-Object -Property $columns | Export-Csv -LiteralPath (Join-Path $evidence 'failed-path-history.csv') -NoTypeInformation -Encoding utf8
    $state.status = 'collected'
    if ($state.probe_sharing_events -lt 1) { throw 'Trace is invalid: the controlled vcpkg sharing violation was not observed.' }
    Write-Host "Observed probe sharing violations: $($state.probe_sharing_events); build access failures: $($state.build_access_failures)."
    Write-Host 'These are diagnostic events, not proof of a build fix or identification of the locking process.'
} catch {
    $state.status = 'diagnostic-failed'
    $state.error = $_.Exception.Message
    throw
} finally {
    $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $stateFile -Encoding utf8
}
