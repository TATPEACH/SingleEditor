[CmdletBinding()]
param(
    [ValidateSet("smoke", "routine", "release")]
    [string]$Profile = "routine",
    [string]$ExePath = "",
    [string]$BaselineExePath = "",
    [string]$AudioPath = "",
    [string]$CorpusId = "",
    [string]$SharedRoot = "G:\Codex\SingleEditor",
    [string]$ReceiptPath = "",
    [string]$SettingsFixturePath = "",
    [string]$StartupResourceStage = "",
    [ValidateSet("auto", "handshake", "external-window")]
    [string]$MeasurementMode = "auto",
    [ValidateSet("baseline", "candidate")]
    [string]$StartWith = "baseline",
    [ValidateSet("m2-frozen", "probe")]
    [string]$BaselineContract = "m2-frozen",
    [ValidateSet("default", "modern", "classic")]
    [string]$CandidateRenderer = "default",
    [ValidateSet("default", "modern", "classic")]
    [string]$BaselineRenderer = "default",
    [string]$LaunchProfile = "gui-direct-file",
    [ValidateSet("preserved-source-identity")]
    [string]$AppCacheState = "preserved-source-identity",
    [ValidateSet("system-warm-hash-validated")]
    [string]$SystemCacheState = "system-warm-hash-validated",
    [string]$CachePreparation = "validate full corpus hash; preserve source identity; terminate benchmark-owned process; wait 200 ms",
    [ValidateRange(1000, 60000)]
    [int]$TimeoutMs = 15000,
    [ValidateRange(1, 100)]
    [int]$PollMilliseconds = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptVersion = "1.0.8"
$startupResourceContext = $null
$startupResourceValidation = $null
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$manifestRoot = Join-Path $repoRoot "Document\New_JuceEditor\milestone3\performance\core-open-v1"
$mappingEnvironmentVariable = "SINGLE_EDITOR_COREOPEN_MAPPING"
$sharedMagic = [uint32]0x31504f43
$sharedVersion = [uint32]1
$sharedByteSize = 80
$milestoneOffsets = [ordered]@{
    window = 40
    file_name = 48
    first_waveform = 56
    playable = 64
    full_waveform = 72
}

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) { throw $Message }
}

function Get-Sha256Hash {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            return ([System.BitConverter]::ToString($sha256.ComputeHash($stream))).Replace("-", "")
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Resolve-ExistingFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($Path)) -Message "$Label path is empty."
    Assert-Condition -Condition (Test-Path -LiteralPath $Path -PathType Leaf) -Message "$Label does not exist: $Path"
    return (Resolve-Path -LiteralPath $Path).Path
}

function Read-JsonManifest {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedSchema
    )
    $resolved = Resolve-ExistingFile -Path $Path -Label "Manifest"
    try {
        $value = Get-Content -Raw -LiteralPath $resolved | ConvertFrom-Json
    } catch {
        throw "Manifest JSON is invalid: $resolved $($_.Exception.Message)"
    }
    Assert-Condition -Condition ($value.schema -eq $ExpectedSchema) -Message "Manifest schema mismatch in $resolved. Expected '$ExpectedSchema', got '$($value.schema)'."
    Assert-Condition -Condition ([int]$value.version -eq 1) -Message "Manifest version mismatch in $resolved. Expected 1, got '$($value.version)'."
    return [PSCustomObject]@{
        path = $resolved
        value = $value
        sha256 = Get-Sha256Hash -Path $resolved
    }
}

function Get-FileIdentity {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Role
    )
    $resolved = Resolve-ExistingFile -Path $Path -Label $Role
    $item = Get-Item -LiteralPath $resolved
    return [PSCustomObject][ordered]@{
        role = $Role
        path = $resolved
        bytes = [int64]$item.Length
        sha256 = Get-Sha256Hash -Path $resolved
        modified_utc = $item.LastWriteTimeUtc.ToString("o")
    }
}

function Get-ManifestIdentity {
    param([Parameter(Mandatory = $true)][PSCustomObject]$Manifest)
    return [PSCustomObject][ordered]@{
        path = $Manifest.path
        schema = $Manifest.value.schema
        version = $Manifest.value.version
        sha256 = $Manifest.sha256
    }
}

function Get-Median {
    param([double[]]$Values)
    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-NearestRankPercentile {
    param(
        [double[]]$Values,
        [ValidateRange(0.01, 1.0)][double]$Quantile
    )
    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $rank = [int][Math]::Ceiling($Quantile * $sorted.Count)
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, $rank - 1))
    return [double]$sorted[$index]
}

function Get-MetricStats {
    param([double[]]$Values)
    if ($null -eq $Values -or $Values.Count -eq 0) {
        return [PSCustomObject][ordered]@{
            count = 0
            median_ms = $null
            mad_ms = $null
            min_ms = $null
            max_ms = $null
            range_ms = $null
            p90_nearest_rank_ms = $null
            p90_reportable_ms = $null
            p90_status = "no-valid-samples"
        }
    }
    $median = Get-Median -Values $Values
    $deviations = @($Values | ForEach-Object { [Math]::Abs([double]$_ - [double]$median) })
    $minimum = [double]($Values | Measure-Object -Minimum).Minimum
    $maximum = [double]($Values | Measure-Object -Maximum).Maximum
    $p90 = Get-NearestRankPercentile -Values $Values -Quantile 0.90
    $p90Eligible = $Values.Count -ge 10
    return [PSCustomObject][ordered]@{
        count = $Values.Count
        median_ms = [Math]::Round($median, 3)
        mad_ms = [Math]::Round((Get-Median -Values $deviations), 3)
        min_ms = [Math]::Round($minimum, 3)
        max_ms = [Math]::Round($maximum, 3)
        range_ms = [Math]::Round($maximum - $minimum, 3)
        p90_nearest_rank_ms = [Math]::Round($p90, 3)
        p90_reportable_ms = if ($p90Eligible) { [Math]::Round($p90, 3) } else { $null }
        p90_status = if ($p90Eligible) { "reportable-nearest-rank-v1" } else { "provisional-only-sample-count-below-10" }
    }
}

function Get-EnvironmentSnapshot {
    param([ValidateRange(1, 10)][int]$CpuSamples = 3)
    $cpuValues = New-Object System.Collections.Generic.List[double]
    for ($i = 0; $i -lt $CpuSamples; $i++) {
        try {
            $processors = @(Get-CimInstance Win32_Processor -ErrorAction Stop)
            if ($processors.Count -gt 0) {
                $average = ($processors | Measure-Object -Property LoadPercentage -Average).Average
                if ($null -ne $average) { $cpuValues.Add([double]$average) }
            }
        } catch {}
        if ($i + 1 -lt $CpuSamples) { Start-Sleep -Milliseconds 200 }
    }
    $memoryAvailablePercent = $null
    try {
        $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
        if ([double]$os.TotalVisibleMemorySize -gt 0) {
            $memoryAvailablePercent = 100.0 * [double]$os.FreePhysicalMemory / [double]$os.TotalVisibleMemorySize
        }
    } catch {}
    return [PSCustomObject][ordered]@{
        captured_utc = [DateTime]::UtcNow.ToString("o")
        cpu_samples_percent = $cpuValues.ToArray()
        cpu_median_percent = if ($cpuValues.Count -gt 0) { [Math]::Round((Get-Median -Values ($cpuValues.ToArray())), 3) } else { $null }
        cpu_max_percent = if ($cpuValues.Count -gt 0) { [Math]::Round([double]($cpuValues | Measure-Object -Maximum).Maximum, 3) } else { $null }
        memory_available_percent = if ($null -ne $memoryAvailablePercent) { [Math]::Round($memoryAvailablePercent, 3) } else { $null }
    }
}

function Get-MachineProfile {
    $os = $null
    $cpu = $null
    try { $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop } catch {}
    try { $cpu = @(Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1)[0] } catch {}
    $powerScheme = $null
    try { $powerScheme = ((& powercfg /getactivescheme 2>$null) -join " ").Trim() } catch {}
    return [PSCustomObject][ordered]@{
        os_caption = if ($null -ne $os) { [string]$os.Caption } else { $null }
        os_version = if ($null -ne $os) { [string]$os.Version } else { [Environment]::OSVersion.VersionString }
        os_build = if ($null -ne $os) { [string]$os.BuildNumber } else { $null }
        cpu_name = if ($null -ne $cpu) { ([string]$cpu.Name).Trim() } else { $null }
        logical_processors = [Environment]::ProcessorCount
        total_visible_memory_kib = if ($null -ne $os) { [int64]$os.TotalVisibleMemorySize } else { $null }
        active_power_scheme = $powerScheme
        powershell_version = $PSVersionTable.PSVersion.ToString()
        stopwatch_frequency = [int64][System.Diagnostics.Stopwatch]::Frequency
    }
}

function Get-GitState {
    $head = $null
    $status = @()
    try {
        $headLine = @(& git -C $repoRoot rev-parse HEAD 2>$null | Select-Object -First 1)
        if ($headLine.Count -gt 0) { $head = ([string]$headLine[0]).Trim() }
        $status = @(& git -C $repoRoot status --short 2>$null)
    } catch {}
    return [PSCustomObject][ordered]@{
        head = $head
        dirty = $status.Count -gt 0
        status = $status
    }
}

function Get-ProcessPathSafe {
    param([System.Diagnostics.Process]$Process)
    try { return $Process.Path } catch { return $null }
}

function Get-SingleEditorProcessDescriptions {
    foreach ($running in @(Get-Process -Name "Single Editor" -ErrorAction SilentlyContinue)) {
        try {
            # Do not keep process handles alive across shutdown checks. A process
            # whose exit cannot be verified is conservatively still running.
            $exited = $false
            try { $exited = $running.HasExited } catch {}
            if (-not $exited) {
                $path = Get-ProcessPathSafe -Process $running
                "pid=$($running.Id) path=$path"
            }
        } finally {
            $running.Dispose()
        }
    }
}

function Assert-NoSingleEditorProcess {
    $descriptions = @(Get-SingleEditorProcessDescriptions)
    if ($descriptions.Count -eq 0) { return }
    throw "A Single Editor process is already running. Close it before benchmarking; this script will not terminate a user-owned process. $($descriptions -join '; ')"
}

function Wait-ForNoSingleEditorProcess {
    param([ValidateRange(100, 10000)][int]$TimeoutMilliseconds = 5000)
    $deadline = [System.Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
        if (@(Get-SingleEditorProcessDescriptions).Count -eq 0) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return @(Get-SingleEditorProcessDescriptions).Count -eq 0
}

function Stop-BenchmarkProcess {
    param(
        [System.Diagnostics.Process]$Process,
        [System.Collections.IDictionary]$Diagnostics = @{}
    )
    # Cleanup observations are separate from the startup timing/validity contract.
    # A forced cleanup alone cannot tell us whether WM_CLOSE was even requested.
    # The historical 1200 ms wait is an observation point, not a product shutdown
    # budget. A Windows audio initializer may still be returning after the window
    # closes. Record that delay, then allow bounded natural cleanup rather than
    # converting it into a forced exit. Startup milestones are already captured.
    $Diagnostics.Clear()
    $Diagnostics.close_requested = $null
    $Diagnostics.window_before_close = $null
    $Diagnostics.window_after_wait = $null
    $Diagnostics.close_initial_wait_limit_ms = 1200
    $Diagnostics.close_initial_wait_elapsed_ms = $null
    $Diagnostics.close_initial_wait_exceeded = $false
    $Diagnostics.window_after_initial_wait = $null
    $Diagnostics.close_wait_limit_ms = 5000
    $Diagnostics.close_wait_elapsed_ms = $null
    $Diagnostics.exited_before_kill = $false
    $Diagnostics.forced = $false
    $Diagnostics.exit_code = $null
    if ($null -eq $Process) { return "not-started" }
    try {
        $Process.Refresh()
        if ($Process.HasExited) {
            $Diagnostics.exited_before_kill = $true
            $Diagnostics.exit_code = $Process.ExitCode
            return "exited"
        }
        $Diagnostics.window_before_close = $Process.MainWindowHandle.ToInt64()
        $Diagnostics.close_requested = $Process.CloseMainWindow()
        $closeWait = [System.Diagnostics.Stopwatch]::StartNew()
        $closed = $Process.WaitForExit($Diagnostics.close_initial_wait_limit_ms)
        $Diagnostics.close_initial_wait_elapsed_ms = [Math]::Round($closeWait.Elapsed.TotalMilliseconds, 3)
        $Process.Refresh()
        if (-not $closed -and -not $Process.HasExited) {
            $Diagnostics.close_initial_wait_exceeded = $true
            $Diagnostics.window_after_initial_wait = $Process.MainWindowHandle.ToInt64()
            $remaining = [Math]::Max(0, $Diagnostics.close_wait_limit_ms - [int][Math]::Ceiling($closeWait.Elapsed.TotalMilliseconds))
            $closed = $Process.WaitForExit($remaining)
        }
        $Diagnostics.close_wait_elapsed_ms = [Math]::Round($closeWait.Elapsed.TotalMilliseconds, 3)
        $Process.Refresh()
        if ($closed -or $Process.HasExited) {
            $Diagnostics.exited_before_kill = $true
            $Diagnostics.exit_code = $Process.ExitCode
            return "closed-main-window"
        }
        $Diagnostics.window_after_wait = $Process.MainWindowHandle.ToInt64()
        $Process.Kill()
        $Diagnostics.forced = $true
        if ($Process.WaitForExit(3000)) { $Diagnostics.exit_code = $Process.ExitCode }
        return "killed-benchmark-owned-process"
    } catch {
        return "cleanup-error: $($_.Exception.Message)"
    }
}

function New-ProcessStartInfo {
    param(
        [Parameter(Mandatory = $true)][PSCustomObject]$Artifact,
        [Parameter(Mandatory = $true)][string]$ResolvedAudioPath
    )
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Artifact.path
    $info.Arguments = '"' + $ResolvedAudioPath + '"'
    if ($Artifact.renderer -ne "default") {
        $info.Arguments += " --renderer=$($Artifact.renderer)"
    }
    $info.WorkingDirectory = Split-Path -Parent $Artifact.path
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $false
    return $info
}

function Invoke-CoreOpenRun {
    param(
        [Parameter(Mandatory = $true)][PSCustomObject]$Artifact,
        [Parameter(Mandatory = $true)][string]$ResolvedAudioPath,
        [Parameter(Mandatory = $true)][ValidateSet("handshake", "external-window")][string]$Mode,
        [Parameter(Mandatory = $true)][string]$Phase,
        [Parameter(Mandatory = $true)][int]$SequenceIndex,
        [Parameter(Mandatory = $true)][string]$SequenceToken
    )
    Assert-NoSingleEditorProcess
    Start-Sleep -Milliseconds 200
    $process = $null
    $mapping = $null
    $accessor = $null
    $mappingName = $null
    $launchQpc = [int64]0
    $frequency = [int64][System.Diagnostics.Stopwatch]::Frequency
    $pidFromChannel = [int64]0
    $launchedPid = [int64]0
    $externalWindowQpc = [int64]0
    $milestoneTicks = [ordered]@{}
    foreach ($name in $milestoneOffsets.Keys) { $milestoneTicks[$name] = [int64]0 }
    $invalidReasons = New-Object System.Collections.Generic.List[string]
    $termination = "not-started"
    $cleanupDiagnostics = [ordered]@{}
    $exitCode = $null
    $startupResources = $null
    try {
        $startInfo = New-ProcessStartInfo -Artifact $Artifact -ResolvedAudioPath $ResolvedAudioPath
        if ($Mode -eq "handshake") {
            $mappingName = "Local\SingleEditorCoreOpen_$([Guid]::NewGuid().ToString('N'))"
            $mapping = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($mappingName, $sharedByteSize, [System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::ReadWrite)
            $accessor = $mapping.CreateViewAccessor(0, $sharedByteSize, [System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::ReadWrite)
            $accessor.Write([int64]0, $sharedMagic)
            $accessor.Write([int64]4, $sharedVersion)
            $accessor.Write([int64]8, [uint32]$sharedByteSize)
            $accessor.Write([int64]12, [uint32]0)
            $accessor.Write([int64]16, $frequency)
            $accessor.Write([int64]32, [int64]0)
            foreach ($offset in $milestoneOffsets.Values) { $accessor.Write([int64]$offset, [int64]0) }
            $startInfo.EnvironmentVariables[$mappingEnvironmentVariable] = $mappingName
        } else {
            [void]$startInfo.EnvironmentVariables.Remove($mappingEnvironmentVariable)
        }
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $startInfo
        $launchQpc = [System.Diagnostics.Stopwatch]::GetTimestamp()
        if ($Mode -eq "handshake") {
            $accessor.Write([int64]24, $launchQpc)
            $accessor.Flush()
        }
        Assert-Condition -Condition $process.Start() -Message "Process.Start returned false for $($Artifact.path)."
        $launchedPid = [int64]$process.Id
        $deadline = [System.Diagnostics.Stopwatch]::StartNew()
        while ($deadline.ElapsedMilliseconds -lt $TimeoutMs) {
            $process.Refresh()
            if ($externalWindowQpc -eq 0 -and $process.MainWindowHandle -ne [IntPtr]::Zero) {
                $externalWindowQpc = [System.Diagnostics.Stopwatch]::GetTimestamp()
            }
            if ($Mode -eq "handshake") {
                $pidFromChannel = $accessor.ReadInt64(32)
                foreach ($entry in $milestoneOffsets.GetEnumerator()) {
                    if ([int64]$milestoneTicks[$entry.Key] -eq 0) {
                        $milestoneTicks[$entry.Key] = $accessor.ReadInt64([int64]$entry.Value)
                    }
                }
                if ($null -ne $startupResourceContext -and $null -eq $startupResources -and
                    $pidFromChannel -eq $launchedPid -and $milestoneTicks.first_waveform -ge $launchQpc -and
                    $milestoneTicks.playable -ge $launchQpc) {
                    $coreQpc = [Math]::Max([long]$milestoneTicks.first_waveform, [long]$milestoneTicks.playable)
                    $startupResources = Get-StartupResourceSnapshot -Context $startupResourceContext -Process $process -CoreReadyQpc $coreQpc
                    if ($startupResources.status -ne 'pass') { $invalidReasons.Add('startup-resource-observation-failed') }
                }
                $allPresent = $true
                foreach ($name in $milestoneOffsets.Keys) {
                    if ([int64]$milestoneTicks[$name] -le 0) { $allPresent = $false; break }
                }
                if ($allPresent) { break }
            } elseif ($externalWindowQpc -gt 0) {
                break
            }
            if ($process.HasExited) { break }
            Start-Sleep -Milliseconds $PollMilliseconds
        }
        $process.Refresh()
        if ($process.HasExited) { $exitCode = $process.ExitCode }
        if ($Mode -eq "handshake") {
            foreach ($name in $milestoneOffsets.Keys) {
                if ([int64]$milestoneTicks[$name] -le 0) {
                    $invalidReasons.Add("missing-milestone:$name")
                } elseif ([int64]$milestoneTicks[$name] -lt $launchQpc) {
                    $invalidReasons.Add("milestone-before-launch:$name")
                }
            }
            if ($pidFromChannel -ne [int64]$process.Id) {
                $invalidReasons.Add("channel-pid-mismatch:expected=$($process.Id),actual=$pidFromChannel")
            }
            if ([int64]$milestoneTicks.window -gt 0) {
                foreach ($name in @("file_name", "first_waveform", "playable")) {
                    if ([int64]$milestoneTicks[$name] -gt 0 -and [int64]$milestoneTicks[$name] -lt [int64]$milestoneTicks.window) {
                        $invalidReasons.Add("milestone-before-window:$name")
                    }
                }
            }
            if ([int64]$milestoneTicks.file_name -gt 0 -and [int64]$milestoneTicks.first_waveform -gt 0 -and [int64]$milestoneTicks.file_name -gt [int64]$milestoneTicks.first_waveform) {
                $invalidReasons.Add("file-name-after-first-waveform")
            }
            if ([int64]$milestoneTicks.first_waveform -gt 0 -and [int64]$milestoneTicks.full_waveform -gt 0 -and [int64]$milestoneTicks.full_waveform -lt [int64]$milestoneTicks.first_waveform) {
                $invalidReasons.Add("full-waveform-before-first-waveform")
            }
        } elseif ($externalWindowQpc -le 0) {
            $invalidReasons.Add("missing-external-window")
        }
        if ($process.HasExited -and $invalidReasons.Count -gt 0) {
            $invalidReasons.Add("process-exited:$exitCode")
        }
        if ($null -ne $startupResourceContext -and $null -eq $startupResources) {
            $invalidReasons.Add('startup-resource-observation-missing')
        }
    } catch {
        $invalidReasons.Add("exception:$($_.Exception.Message)")
    } finally {
        $termination = Stop-BenchmarkProcess -Process $process -Diagnostics $cleanupDiagnostics
        if ($null -ne $accessor) { $accessor.Dispose() }
        if ($null -ne $mapping) { $mapping.Dispose() }
        if ($null -ne $process) { $process.Dispose() }
        if (-not (Wait-ForNoSingleEditorProcess -TimeoutMilliseconds 10000)) {
            $invalidReasons.Add("benchmark-process-still-present-after-cleanup")
        }
    }
    $metrics = [ordered]@{}
    if ($Mode -eq "handshake") {
        foreach ($name in $milestoneOffsets.Keys) {
            $tick = [int64]$milestoneTicks[$name]
            $metrics[$name] = if ($tick -gt 0 -and $launchQpc -gt 0) { [Math]::Round(1000.0 * ($tick - $launchQpc) / $frequency, 3) } else { $null }
        }
        $metrics["core_ready"] = if ($null -ne $metrics.first_waveform -and $null -ne $metrics.playable) { [Math]::Max([double]$metrics.first_waveform, [double]$metrics.playable) } else { $null }
    } else {
        $metrics["window_external"] = if ($externalWindowQpc -gt 0 -and $launchQpc -gt 0) { [Math]::Round(1000.0 * ($externalWindowQpc - $launchQpc) / $frequency, 3) } else { $null }
    }
    $externalWindowMs = if ($externalWindowQpc -gt 0 -and $launchQpc -gt 0) { [Math]::Round(1000.0 * ($externalWindowQpc - $launchQpc) / $frequency, 3) } else { $null }
    # Preserve the existing clock observations for external trace correlation.
    # Decimal strings avoid loss of integer precision in JSON consumers. These
    # are the original ticks, not reconstructed from rounded metrics_ms.
    $rawMilestones = [ordered]@{}
    if ($Mode -eq 'handshake') {
        foreach ($name in $milestoneOffsets.Keys) {
            $rawMilestones[$name] = if ($milestoneTicks[$name] -gt 0) { ([int64]$milestoneTicks[$name]).ToString([Globalization.CultureInfo]::InvariantCulture) } else { $null }
        }
    }
    $rawCoreReady = if ($Mode -eq 'handshake' -and $milestoneTicks.first_waveform -gt 0 -and $milestoneTicks.playable -gt 0) {
        ([Math]::Max([int64]$milestoneTicks.first_waveform, [int64]$milestoneTicks.playable)).ToString([Globalization.CultureInfo]::InvariantCulture)
    } else { $null }
    return [PSCustomObject][ordered]@{
        sequence_index = $SequenceIndex
        sequence_token = $SequenceToken
        artifact_role = $Artifact.role
        phase = $Phase
        measurement_mode = $Mode
        mapping_present = $Mode -eq "handshake"
        valid = $invalidReasons.Count -eq 0
        invalid_reasons = $invalidReasons.ToArray()
        process_id = if ($pidFromChannel -gt 0) { $pidFromChannel } elseif ($launchedPid -gt 0) { $launchedPid } else { $null }
        process_exit_code = $exitCode
        termination = $termination
        cleanup = [PSCustomObject]$cleanupDiagnostics
        external_window_ms = $externalWindowMs
        metrics_ms = [PSCustomObject]$metrics
        clock = [PSCustomObject][ordered]@{
            source = 'windows-qpc'
            integer_encoding = 'decimal-string'
            frequency = $frequency.ToString([Globalization.CultureInfo]::InvariantCulture)
            launch = if ($launchQpc -gt 0) { $launchQpc.ToString([Globalization.CultureInfo]::InvariantCulture) } else { $null }
            external_window = if ($externalWindowQpc -gt 0) { $externalWindowQpc.ToString([Globalization.CultureInfo]::InvariantCulture) } else { $null }
            milestones = [PSCustomObject]$rawMilestones
            core_ready = $rawCoreReady
        }
        startup_resources = $startupResources
    }
}

function Get-ArtifactSummary {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Samples,
        [Parameter(Mandatory = $true)][string]$ArtifactRole,
        [Parameter(Mandatory = $true)][string[]]$MetricNames,
        [Parameter(Mandatory = $true)][int]$ExpectedSteadySamples
    )
    $steady = @($Samples | Where-Object { $_.artifact_role -eq $ArtifactRole -and $_.phase -eq "steady" })
    $valid = @($steady | Where-Object { $_.valid })
    $metricStats = [ordered]@{}
    foreach ($metricName in $MetricNames) {
        $values = @($valid | ForEach-Object {
            $value = $_.metrics_ms.$metricName
            if ($null -ne $value) { [double]$value }
        })
        $metricStats[$metricName] = Get-MetricStats -Values $values
    }
    return [PSCustomObject][ordered]@{
        artifact_role = $ArtifactRole
        expected_steady_samples = $ExpectedSteadySamples
        attempted_steady_samples = $steady.Count
        valid_steady_samples = $valid.Count
        invalid_steady_samples = $steady.Count - $valid.Count
        metrics = [PSCustomObject]$metricStats
    }
}

function New-CoreOpenSettingsLease {
    param([Parameter(Mandatory = $true)][string]$Path,
          [Parameter(Mandatory = $true)][string]$BackupDirectory,
          [string]$FixturePath = "")
    Assert-NoSingleEditorProcess
    $settingsFile = [IO.Path]::GetFullPath($Path)
    $originalExists = [IO.File]::Exists($settingsFile)
    $fixtureExists = $originalExists
    $fixtureSource = $settingsFile
    if (-not [string]::IsNullOrWhiteSpace($FixturePath)) {
        $fixtureSource = Resolve-ExistingFile -Path $FixturePath -Label "Settings fixture"
        $fixtureExists = $true
    }
    [void][IO.Directory]::CreateDirectory($BackupDirectory)
    $originalBackup = Join-Path $BackupDirectory 'original.settings'
    $fixtureBackup = Join-Path $BackupDirectory 'fixture.settings'
    if ($originalExists) { [IO.File]::WriteAllBytes($originalBackup, [IO.File]::ReadAllBytes($settingsFile)) }
    if ($fixtureExists) { [IO.File]::WriteAllBytes($fixtureBackup, [IO.File]::ReadAllBytes($fixtureSource)) }
    return [pscustomobject][ordered]@{
        path = $settingsFile
        original_exists = $originalExists
        original_backup_path = $originalBackup
        original_sha256 = if ($originalExists) { Get-Sha256Hash $originalBackup } else { $null }
        fixture_exists = $fixtureExists
        fixture_snapshot_path = $fixtureBackup
        fixture_sha256 = if ($fixtureExists) { Get-Sha256Hash $fixtureBackup } else { $null }
        reset_count = 0
        restored = $false
        restore_error = $null
    }
}

function Reset-CoreOpenSettings {
    param([Parameter(Mandatory = $true)]$Lease)
    Assert-NoSingleEditorProcess
    if ($Lease.fixture_exists) {
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Lease.path))
        [IO.File]::WriteAllBytes($Lease.path, [IO.File]::ReadAllBytes($Lease.fixture_snapshot_path))
        Assert-Condition ((Get-Sha256Hash $Lease.path) -eq $Lease.fixture_sha256) "Settings fixture did not reset byte-for-byte."
    } elseif ([IO.File]::Exists($Lease.path)) {
        Remove-Item -LiteralPath $Lease.path
    }
    $Lease.reset_count++
}

function Restore-CoreOpenSettings {
    param([Parameter(Mandatory = $true)]$Lease)
    try {
        Assert-NoSingleEditorProcess
        if ($Lease.original_exists) {
            [IO.File]::WriteAllBytes($Lease.path, [IO.File]::ReadAllBytes($Lease.original_backup_path))
            Assert-Condition ((Get-Sha256Hash $Lease.path) -eq $Lease.original_sha256) "Original settings restoration did not match its backup."
        } elseif ([IO.File]::Exists($Lease.path)) {
            Remove-Item -LiteralPath $Lease.path
        }
        $Lease.restored = $true
        $Lease.restore_error = $null
    } catch {
        $Lease.restore_error = $_.Exception.Message
        throw
    }
}

function Get-CoreOpenPreflightReasons($Environment, $Gate, [bool]$GateEligible) {
    if (-not $GateEligible) { return }
    foreach ($name in @('cpu_median_percent', 'cpu_max_percent', 'memory_available_percent')) {
        if ($null -eq $Environment.$name) { "pre-run-observation-unavailable:$name" }
    }
    if ($null -ne $Environment.cpu_median_percent -and $Environment.cpu_median_percent -gt $Gate.max_pre_run_cpu_median_percent) {
        "pre-run-cpu-median-high:$($Environment.cpu_median_percent)"
    }
    if ($null -ne $Environment.cpu_max_percent -and $Environment.cpu_max_percent -gt $Gate.max_single_pre_run_cpu_percent) {
        "pre-run-cpu-peak-high:$($Environment.cpu_max_percent)"
    }
    if ($null -ne $Environment.memory_available_percent -and $Environment.memory_available_percent -lt $Gate.min_available_memory_percent) {
        "pre-run-memory-low:$($Environment.memory_available_percent)"
    }
}

function Invoke-QualifiedCoreOpenSequence {
    param(
        [AllowEmptyCollection()][string[]]$PreflightReasons,
        [System.Collections.IDictionary]$SequenceArguments,
        [string]$SettingsPath, [string]$BackupDirectory, [string]$FixturePath = ''
    )
    if ($PreflightReasons.Count -gt 0) {
        # Do not acquire/reset settings or start a process after a known
        # ineligible preflight. Zero attempted samples remain distinguishable
        # from both an interrupted run and a completed measurement.
        return [pscustomobject]@{
            samples = @(); completed = $false; attempted_sequence_index = 0
            error = 'preflight-ineligible'; settings = $null
        }
    }
    $lease = New-CoreOpenSettingsLease -Path $SettingsPath -BackupDirectory $BackupDirectory -FixturePath $FixturePath
    Invoke-CoreOpenSequence @SequenceArguments -SettingsLease $lease
}

function Invoke-CoreOpenSequence {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Artifacts,
        [Parameter(Mandatory = $true)][string]$ResolvedAudioPath,
        [Parameter(Mandatory = $true)][string]$Mode,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$WarmupOrder,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$SteadyOrder,
        $SettingsLease = $null
    )
    $samples = New-Object System.Collections.Generic.List[object]
    $sequenceIndex = 0
    $runError = $null
    try {
        foreach ($phase in @("warmup", "steady")) {
            $order = if ($phase -eq "warmup") { $WarmupOrder } else { $SteadyOrder }
            foreach ($role in $order) {
                $sequenceIndex++
                $token = if ($role -eq "baseline") { "A" } else { "B" }
                Write-Host "[$sequenceIndex] $phase $token $role ($Mode)"
                if ($null -ne $SettingsLease) { Reset-CoreOpenSettings -Lease $SettingsLease }
                $sample = Invoke-CoreOpenRun -Artifact $Artifacts[$role] -ResolvedAudioPath $ResolvedAudioPath -Mode $Mode -Phase $phase -SequenceIndex $sequenceIndex -SequenceToken $token
                if ($null -ne $SettingsLease) { $sample | Add-Member -NotePropertyName settings_fixture_sha256 -NotePropertyValue $SettingsLease.fixture_sha256 }
                $samples.Add($sample)
                Write-Host "    valid=$($sample.valid) metrics=$($sample.metrics_ms | ConvertTo-Json -Compress) termination=$($sample.termination) invalid=$($sample.invalid_reasons | ConvertTo-Json -Compress)"
            }
        }
    } catch {
        $runError = $_.Exception.Message
        Write-Warning "Measurement interrupted at sample ${sequenceIndex}: $runError"
    } finally {
        if ($null -ne $SettingsLease) {
            try { Restore-CoreOpenSettings -Lease $SettingsLease }
            catch {
                $restoreError = "settings-restore-failed:$($_.Exception.Message)"
                $runError = if ($null -eq $runError) { $restoreError } else { "$runError; $restoreError" }
                Write-Warning $restoreError
            }
        }
    }
    return [PSCustomObject][ordered]@{
        samples = $samples.ToArray()
        completed = $null -eq $runError
        attempted_sequence_index = $sequenceIndex
        error = $runError
        settings = $SettingsLease
    }
}

$contractManifest = Read-JsonManifest -Path (Join-Path $manifestRoot "contract.json") -ExpectedSchema "singleeditor.core-open.contract"
$corpusManifest = Read-JsonManifest -Path (Join-Path $manifestRoot "corpus.json") -ExpectedSchema "singleeditor.core-open.corpus"
$thresholdManifest = Read-JsonManifest -Path (Join-Path $manifestRoot "thresholds.json") -ExpectedSchema "singleeditor.core-open.thresholds"
$baselineManifest = Read-JsonManifest -Path (Join-Path $manifestRoot "baselines\M2_FINAL_BASELINE.json") -ExpectedSchema "singleeditor.core-open.historical-baseline"
$contract = $contractManifest.value
$corpus = $corpusManifest.value
$thresholds = $thresholdManifest.value
$historicalBaseline = $baselineManifest.value

Assert-Condition -Condition ($contract.clock -eq "windows-qpc") -Message "Only windows-qpc is supported."
Assert-Condition -Condition ([int]$contract.channel.byte_size -eq $sharedByteSize) -Message "Shared-memory byte size mismatch."
Assert-Condition -Condition ($contract.channel.environment_variable -eq $mappingEnvironmentVariable) -Message "Shared-memory environment variable mismatch."
Assert-Condition -Condition ($null -ne $contract.profiles.$Profile) -Message "Profile '$Profile' is absent from contract.json."

$profileContract = $contract.profiles.$Profile
$warmupRuns = [int]$profileContract.warmup_runs
$steadyRunsPerArtifact = [int]$profileContract.steady_runs_per_artifact
$gateEligible = [bool]$profileContract.gate_eligible

if ([string]::IsNullOrWhiteSpace($ExePath)) { $ExePath = [string]$contract.release_artifact.fixed_portable_path }
$candidate = Get-FileIdentity -Path $ExePath -Role "candidate"
$candidate | Add-Member -NotePropertyName renderer -NotePropertyValue $CandidateRenderer
$baseline = $null
if (-not [string]::IsNullOrWhiteSpace($BaselineExePath)) {
    $baseline = Get-FileIdentity -Path $BaselineExePath -Role "baseline"
    $baseline | Add-Member -NotePropertyName renderer -NotePropertyValue $BaselineRenderer
    if ($BaselineContract -eq "m2-frozen") {
        Assert-Condition -Condition ($baseline.sha256 -eq [string]$historicalBaseline.artifact.sha256) -Message "Baseline hash does not match M2_FINAL_BASELINE.json. Expected $($historicalBaseline.artifact.sha256), got $($baseline.sha256)."
        Assert-Condition -Condition ($baseline.bytes -eq [int64]$historicalBaseline.artifact.bytes) -Message "Baseline size does not match M2_FINAL_BASELINE.json."
    }
    Assert-Condition -Condition ($baseline.sha256 -ne $candidate.sha256) -Message "Baseline and candidate artifacts are identical; a comparison would be meaningless."
}

if ($MeasurementMode -eq "auto") {
    $MeasurementMode = if ($null -eq $baseline -or $BaselineContract -eq "probe") { "handshake" } else { "external-window" }
}
if ($null -ne $baseline -and $BaselineContract -eq "m2-frozen" -and $MeasurementMode -eq "handshake") {
    Write-Warning "The frozen M2 portable predates CoreOpenProbe. Handshake comparison is expected to fail; use auto/external-window for the M2 bridge."
}

if (-not [string]::IsNullOrWhiteSpace($StartupResourceStage)) {
    Assert-Condition -Condition ($Profile -eq 'smoke' -and -not $gateEligible -and $MeasurementMode -eq 'handshake' -and $null -eq $baseline) -Message 'Startup resource observation requires smoke/handshake without a baseline; it is never performance-gate evidence.'
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($ReceiptPath)) -Message 'Resource observation requires a new explicit receipt path.'
    if (-not [IO.Path]::IsPathRooted($ReceiptPath)) { $ReceiptPath = Join-Path $repoRoot $ReceiptPath }
    $ReceiptPath = [IO.Path]::GetFullPath($ReceiptPath)
    Assert-Condition -Condition (-not (Test-Path -LiteralPath $ReceiptPath)) -Message 'Resource observation requires a new explicit receipt path.'
    Import-Module (Join-Path $PSScriptRoot 'release/StartupResources.psm1')
    $startupResourceContext = New-StartupResourceContext -StageRoot $StartupResourceStage -GuiPath $candidate.path
    Assert-Condition -Condition (-not $ReceiptPath.StartsWith($startupResourceContext.stage + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) -Message 'Resource receipt must be outside the sealed stage.'
}

if ([string]::IsNullOrWhiteSpace($CorpusId)) {
    $CorpusId = if ($Profile -eq "smoke") { "ordinary-medium-wav-smoke-v1" } else { [string]$corpus.primary_id }
}
$asset = @($corpus.assets | Where-Object { $_.id -eq $CorpusId } | Select-Object -First 1)
Assert-Condition -Condition ($asset.Count -eq 1) -Message "Corpus asset '$CorpusId' is missing or duplicated."
if ([string]::IsNullOrWhiteSpace($AudioPath)) {
    $relative = ([string]$asset[0].relative_path_from_shared_root).Replace("/", [IO.Path]::DirectorySeparatorChar)
    $AudioPath = Join-Path $SharedRoot $relative
}
$resolvedAudio = Resolve-ExistingFile -Path $AudioPath -Label "Audio corpus"
$audioIdentity = Get-FileIdentity -Path $resolvedAudio -Role "corpus"
Assert-Condition -Condition ($audioIdentity.bytes -eq [int64]$asset[0].bytes) -Message "Corpus byte size mismatch."
Assert-Condition -Condition ($audioIdentity.sha256 -eq [string]$asset[0].sha256) -Message "Corpus SHA-256 mismatch."

$artifacts = [ordered]@{ candidate = $candidate }
if ($null -ne $baseline) { $artifacts["baseline"] = $baseline }
$warmupOrder = New-Object System.Collections.Generic.List[string]
if ($warmupRuns -gt 0) {
    if ($null -ne $baseline) {
        $first = $StartWith
        $second = if ($StartWith -eq "baseline") { "candidate" } else { "baseline" }
        for ($i = 0; $i -lt $warmupRuns; $i++) { $warmupOrder.Add($first); $warmupOrder.Add($second) }
    } else {
        for ($i = 0; $i -lt $warmupRuns; $i++) { $warmupOrder.Add("candidate") }
    }
}
$steadyOrder = New-Object System.Collections.Generic.List[string]
if ($null -ne $baseline) {
    $first = $StartWith
    $second = if ($StartWith -eq "baseline") { "candidate" } else { "baseline" }
    for ($i = 0; $i -lt $steadyRunsPerArtifact; $i++) { $steadyOrder.Add($first); $steadyOrder.Add($second) }
} else {
    for ($i = 0; $i -lt $steadyRunsPerArtifact; $i++) { $steadyOrder.Add("candidate") }
}

$preEnvironment = Get-EnvironmentSnapshot
$environmentReasons = New-Object System.Collections.Generic.List[string]
foreach ($reason in @(Get-CoreOpenPreflightReasons $preEnvironment $thresholds.environment_gate $gateEligible)) { $environmentReasons.Add($reason) }
$preflightSkipped = $environmentReasons.Count -gt 0
$machineProfile = Get-MachineProfile
$gitState = Get-GitState
$settingsPath = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'SingleEditor/SingleEditor.settings'
$settingsBackupDirectory = Join-Path $repoRoot ('out/core-open-benchmark-settings/' + [guid]::NewGuid().ToString('N'))
$sequenceArguments = @{Artifacts=$artifacts;ResolvedAudioPath=$resolvedAudio;Mode=$MeasurementMode;WarmupOrder=$warmupOrder.ToArray();SteadyOrder=$steadyOrder.ToArray()}
$execution = Invoke-QualifiedCoreOpenSequence -PreflightReasons $environmentReasons.ToArray() -SequenceArguments $sequenceArguments -SettingsPath $settingsPath -BackupDirectory $settingsBackupDirectory -FixturePath $SettingsFixturePath
$sampleArray = $execution.samples
if ($null -ne $startupResourceContext) {
    try { $startupResourceValidation = Complete-StartupResourceContext $startupResourceContext }
    catch { $startupResourceValidation = [pscustomobject]@{status='failed';error=$_.Exception.Message;payload_unchanged=$false} }
}
$postEnvironment = Get-EnvironmentSnapshot
$metricNames = if ($MeasurementMode -eq "handshake") { @("window", "file_name", "first_waveform", "playable", "core_ready", "full_waveform") } else { @("window_external") }
$primaryMetric = if ($MeasurementMode -eq "handshake") { "core_ready" } else { "window_external" }
$summaries = New-Object System.Collections.Generic.List[object]
if ($null -ne $baseline) { $summaries.Add((Get-ArtifactSummary -Samples $sampleArray -ArtifactRole "baseline" -MetricNames $metricNames -ExpectedSteadySamples $steadyRunsPerArtifact)) }
$summaries.Add((Get-ArtifactSummary -Samples $sampleArray -ArtifactRole "candidate" -MetricNames $metricNames -ExpectedSteadySamples $steadyRunsPerArtifact))
$summaryArray = $summaries.ToArray()

foreach ($summary in $summaryArray) {
    $primaryStats = $summary.metrics.$primaryMetric
    if ($gateEligible -and $null -ne $primaryStats.median_ms -and [double]$primaryStats.median_ms -gt 0) {
        $madRatio = [double]$primaryStats.mad_ms / [double]$primaryStats.median_ms
        if ($madRatio -gt [double]$thresholds.environment_gate.max_core_ready_mad_ratio) {
            $environmentReasons.Add("$($summary.artifact_role)-$primaryMetric-mad-ratio-high:$([Math]::Round($madRatio, 4))")
        }
    }
}

$controlDrift = $null
if ($null -ne $baseline) {
    $baselineSteady = @($sampleArray | Where-Object { $_.phase -eq "steady" -and $_.artifact_role -eq "baseline" -and $_.valid })
    if ($baselineSteady.Count -ge 4) {
        $split = [int][Math]::Floor($baselineSteady.Count / 2)
        $firstHalf = @($baselineSteady[0..($split - 1)] | ForEach-Object { [double]$_.metrics_ms.$primaryMetric })
        $secondHalf = @($baselineSteady[$split..($baselineSteady.Count - 1)] | ForEach-Object { [double]$_.metrics_ms.$primaryMetric })
        $firstMedian = Get-Median -Values $firstHalf
        $secondMedian = Get-Median -Values $secondHalf
        $delta = [double]$secondMedian - [double]$firstMedian
        $percent = if ([double]$firstMedian -ne 0) { 100.0 * $delta / [double]$firstMedian } else { $null }
        $controlDrift = [PSCustomObject][ordered]@{
            metric = $primaryMetric
            first_half_median_ms = [Math]::Round($firstMedian, 3)
            second_half_median_ms = [Math]::Round($secondMedian, 3)
            delta_ms = [Math]::Round($delta, 3)
            delta_percent = if ($null -ne $percent) { [Math]::Round($percent, 3) } else { $null }
        }
        if ($gateEligible -and [Math]::Abs($delta) -gt [double]$thresholds.environment_gate.minimum_practical_drift_ms -and $null -ne $percent -and [Math]::Abs($percent) -gt [double]$thresholds.environment_gate.max_control_half_drift_percent) {
            $environmentReasons.Add("baseline-control-half-drift")
        }
    }
}

$comparison = $null
$regressionReasons = New-Object System.Collections.Generic.List[string]
$warningReasons = New-Object System.Collections.Generic.List[string]
if ($null -ne $baseline) {
    $baselineSummary = @($summaryArray | Where-Object { $_.artifact_role -eq "baseline" })[0]
    $candidateSummary = @($summaryArray | Where-Object { $_.artifact_role -eq "candidate" })[0]
    $metricComparisons = [ordered]@{}
    foreach ($metric in $metricNames) {
        $baselineMedian = $baselineSummary.metrics.$metric.median_ms
        $candidateMedian = $candidateSummary.metrics.$metric.median_ms
        if ($null -eq $baselineMedian -or $null -eq $candidateMedian) { $metricComparisons[$metric] = $null; continue }
        $delta = [double]$candidateMedian - [double]$baselineMedian
        $percent = if ([double]$baselineMedian -ne 0) { 100.0 * $delta / [double]$baselineMedian } else { $null }
        $metricComparisons[$metric] = [PSCustomObject][ordered]@{
            baseline_median_ms = $baselineMedian
            candidate_median_ms = $candidateMedian
            delta_ms = [Math]::Round($delta, 3)
            delta_percent = if ($null -ne $percent) { [Math]::Round($percent, 3) } else { $null }
        }
        if ($gateEligible -and $null -ne $percent -and $percent -ge [double]$thresholds.relative_gate.regression_percent -and $delta -ge [double]$thresholds.relative_gate.minimum_practical_delta_ms) {
            $regressionReasons.Add("$metric-relative-and-practical-gate")
        } elseif ($gateEligible -and $null -ne $percent -and $percent -ge [double]$thresholds.relative_gate.warn_percent) {
            $warningReasons.Add("$metric-warning-delta")
        }
    }
    $comparison = [PSCustomObject][ordered]@{
        labels = if ($BaselineContract -eq "m2-frozen") { "A=baseline M2 portable, B=candidate" } else { "A=probe baseline, B=candidate" }
        order = @($sampleArray | ForEach-Object { "$($_.phase):$($_.sequence_token)" })
        metrics = [PSCustomObject]$metricComparisons
        control_drift = $controlDrift
    }
}

$candidateSummaryForCeilings = @($summaryArray | Where-Object { $_.artifact_role -eq "candidate" })[0]
$ceilingMetricMap = if ($MeasurementMode -eq "handshake") {
    [ordered]@{ window = "window"; file_name = "file_name"; first_waveform = "first_waveform"; playable = "playable"; core_ready = "core_ready"; full_waveform = "full_waveform" }
} else {
    [ordered]@{ window_external = "window" }
}
foreach ($entry in $ceilingMetricMap.GetEnumerator()) {
    $median = $candidateSummaryForCeilings.metrics.($entry.Key).median_ms
    $ceiling = $thresholds.absolute_ceiling_ms.($entry.Value)
    if ($gateEligible -and $null -ne $median -and $null -ne $ceiling -and [double]$median -gt [double]$ceiling) {
        $regressionReasons.Add("$($entry.Key)-absolute-ceiling")
    }
}

$invalidSamples = @($sampleArray | Where-Object { -not $_.valid })
$insufficientSummaries = @($summaryArray | Where-Object { $_.valid_steady_samples -ne $_.expected_steady_samples })
$verdict = "pass"
if ($null -ne $startupResourceValidation -and $startupResourceValidation.status -ne 'pass') {
    $verdict = 'failed-resource-validation'
} elseif ($preflightSkipped) {
    $verdict = 'inconclusive_environment'
} elseif (-not $execution.completed -or $invalidSamples.Count -gt 0 -or $insufficientSummaries.Count -gt 0) {
    $verdict = "failed-invalid-samples"
} elseif ($gateEligible -and $environmentReasons.Count -gt 0) {
    $verdict = "inconclusive_environment"
} elseif ($regressionReasons.Count -gt 0) {
    $verdict = "regression"
} elseif ($warningReasons.Count -gt 0) {
    $verdict = "warning"
}

if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $receiptDirectory = Join-Path $manifestRoot "receipts"
    $receiptFileName = "{0}-core-open-v1-{1}-{2}.json" -f [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssZ"), $Profile, $MeasurementMode
    $ReceiptPath = Join-Path $receiptDirectory $receiptFileName
} elseif (-not [IO.Path]::IsPathRooted($ReceiptPath)) {
    $ReceiptPath = Join-Path $repoRoot $ReceiptPath
}
$receiptDirectory = Split-Path -Parent $ReceiptPath
if (-not (Test-Path -LiteralPath $receiptDirectory)) { [void](New-Item -ItemType Directory -Path $receiptDirectory -Force) }

$receipt = [PSCustomObject][ordered]@{
    schema = "singleeditor.core-open.receipt"
    version = 1
    created_utc = [DateTime]::UtcNow.ToString("o")
    runner = [PSCustomObject][ordered]@{ path = $MyInvocation.MyCommand.Path; version = $scriptVersion; powershell = $PSVersionTable.PSVersion.ToString() }
    source = $gitState
    manifests = [PSCustomObject][ordered]@{
        contract = Get-ManifestIdentity -Manifest $contractManifest
        corpus = Get-ManifestIdentity -Manifest $corpusManifest
        thresholds = Get-ManifestIdentity -Manifest $thresholdManifest
        m2_baseline = Get-ManifestIdentity -Manifest $baselineManifest
    }
    run_contract = [PSCustomObject][ordered]@{
        profile = $Profile
        gate_eligible = $gateEligible
        measurement_mode = $MeasurementMode
        comparability = if ($null -ne $startupResourceContext) { 'Resource observation only; observer can perturb timings; no performance acceptance' } elseif ($MeasurementMode -eq "external-window") { "legacy M2 bridge only; mapping absent; no claim of five-milestone comparability" } else { "Core Open KPI v1 shared-memory handshake" }
        warmup_runs_per_artifact = $warmupRuns
        steady_runs_per_artifact = $steadyRunsPerArtifact
        start_with = $StartWith
        baseline_contract = $BaselineContract
        launch_profile = $LaunchProfile
        app_cache_state = $AppCacheState
        system_cache_state = $SystemCacheState
        cache_preparation = $CachePreparation
        timeout_ms = $TimeoutMs
        poll_ms = $PollMilliseconds
        quantile_method = "nearest-rank-v1; reportable p90 requires at least 10 valid samples"
        settings_policy = "identical snapshot before every launch; original bytes restored after owned processes exit"
    }
    machine = $machineProfile
    environment = [PSCustomObject][ordered]@{ pre = $preEnvironment; post = $postEnvironment; control_drift = $controlDrift; inconclusive_reasons = $environmentReasons.ToArray() }
    corpus = $audioIdentity
    startup_resource_validation = $startupResourceValidation
    artifacts = @($artifacts.GetEnumerator() | ForEach-Object { $_.Value })
    samples = $sampleArray
    execution = [PSCustomObject][ordered]@{
        preflight_skipped = $preflightSkipped
        completed = $execution.completed
        attempted_sequence_index = $execution.attempted_sequence_index
        completed_sample_count = $sampleArray.Count
        error = $execution.error
        settings = $execution.settings
    }
    summaries = $summaryArray
    comparison = $comparison
    verdict = [PSCustomObject][ordered]@{
        status = $verdict
        invalid_sample_count = $invalidSamples.Count
        regression_reasons = $regressionReasons.ToArray()
        warning_reasons = $warningReasons.ToArray()
        environment_reasons = $environmentReasons.ToArray()
    }
}

$json = $receipt | ConvertTo-Json -Depth 16
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($ReceiptPath, $json + [Environment]::NewLine, $utf8NoBom)

Write-Host ""
Write-Host "Core Open summary"
foreach ($summary in $summaryArray) {
    foreach ($metric in $metricNames) {
        $stats = $summary.metrics.$metric
        $line = "  {0,-9} {1,-16} n={2} median={3} ms MAD={4} range={5} p90-est={6} p90-status={7}" -f $summary.artifact_role, $metric, $stats.count, $stats.median_ms, $stats.mad_ms, $stats.range_ms, $stats.p90_nearest_rank_ms, $stats.p90_status
        Write-Host $line
    }
}
Write-Host "VERDICT=$verdict"
Write-Host "RECEIPT=$ReceiptPath"

switch ($verdict) {
    "pass" { exit 0 }
    "warning" { exit 2 }
    "inconclusive_environment" { exit 3 }
    "regression" { exit 4 }
    default { exit 5 }
}
