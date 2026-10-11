[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$SharedRoot = 'G:\Codex\SingleEditor',
    [string]$SettingsFixturePath = '',
    [ValidateSet('default', 'modern', 'classic')][string]$Renderer = 'modern'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Output already exists: $output" }
if (@(Get-Process -Name 'Single Editor', 'SingleEditor' -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'Close existing editor windows before collecting a startup trace.'
}
[void][IO.Directory]::CreateDirectory($output)
$utf8 = New-Object Text.UTF8Encoding($false)
$instance = 'SingleEditorStartup_' + [guid]::NewGuid().ToString('N')
$wpr = Join-Path $env:SystemRoot 'System32/wpr.exe'
$hostExe = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$runner = Join-Path $repo 'tools/Invoke-CoreOpenBenchmark.ps1'
$template = Join-Path $PSScriptRoot 'SingleEditorStartup.wprp'
$profile = Join-Path $output 'startup.wprp'
$etl = Join-Path $output 'startup.etl'
$coreReceipt = Join-Path $output 'core-smoke.json'
$commands = New-Object Collections.Generic.List[object]
$pending = @(
    'ETL event loss, provider/filter coverage and exact event/PID/QPC correlation require analysis.',
    'Filtered events cannot prove absence of System/other-process reads, mapped-page touches, disk I/O or antivirus activity.',
    'This smoke capture does not establish performance acceptance or the complete startup trace release gate.'
)
$status = 'failed'
$errorText = $null
$started = $false
$stopped = $false
$stopAttempted = $false
$core = $null
$candidateBefore = $null

function File-Fact([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { return $null }
    return [ordered]@{path=$Path;bytes=(Get-Item -LiteralPath $Path).Length;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
}
function Invoke-OwnedWpr([string]$Step, [string[]]$Arguments) {
    # Instance name MUST remain last for every command. Never cancel a default
    # or foreign recording, even when our own start or stop fails.
    $argsWithInstance = @($Arguments) + @('-instancename', $instance)
    $log = Join-Path $output ($Step + '.log')
    $begin = [Diagnostics.Stopwatch]::GetTimestamp().ToString([Globalization.CultureInfo]::InvariantCulture)
    # Windows PowerShell otherwise turns native stderr (even an empty line)
    # into a terminating NativeCommandError before its exit code is recorded.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $wpr @argsWithInstance > $log 2>&1
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    $end = [Diagnostics.Stopwatch]::GetTimestamp().ToString([Globalization.CultureInfo]::InvariantCulture)
    $commands.Add([ordered]@{step=$Step;arguments=$argsWithInstance;exit_code=$code;begin_qpc=$begin;end_qpc=$end;log=$log})
    return $code
}

try {
    if (-not [IO.File]::Exists($wpr)) { throw "Windows Performance Recorder is unavailable: $wpr" }
    $candidateBefore = File-Fact $exe
    $leaf = [IO.Path]::GetFileName($exe)
    # Semicolons separate process names in WPR; this profile observes one name.
    if ($leaf.Contains(';')) { throw 'The executable name cannot contain a WPR process-filter separator.' }
    $xml = [IO.File]::ReadAllText($template).Replace('__EDITOR_EXE__', [Security.SecurityElement]::Escape($leaf))
    [IO.File]::WriteAllText($profile, $xml, $utf8)
    $profileCode = Invoke-OwnedWpr 'profile' @('-profiles', $profile)
    if ($profileCode -ne 0) { throw "WPR profile validation failed ($profileCode); see profile.log." }
    $startCode = Invoke-OwnedWpr 'start' @('-start', ($profile + '!SingleEditorStartup'))
    if ($startCode -ne 0) { throw "WPR could not start ($startCode); see start.log. The editor was not launched." }
    $started = $true

    # Reuse the established corpus, settings lease, real milestone channel and
    # normal-close cleanup. Smoke is explicitly ineligible for performance.
    $runnerArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $runner,
        '-Profile', 'smoke', '-ExePath', $exe, '-SharedRoot', $SharedRoot,
        '-MeasurementMode', 'handshake', '-CandidateRenderer', $Renderer,
        '-ReceiptPath', $coreReceipt)
    if ($SettingsFixturePath) { $runnerArgs += @('-SettingsFixturePath', $SettingsFixturePath) }
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $hostExe @runnerArgs > (Join-Path $output 'core-smoke.log') 2>&1
        $runnerCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if (-not [IO.File]::Exists($coreReceipt)) { throw "Core smoke produced no receipt (exit $runnerCode)." }
    $core = Get-Content -LiteralPath $coreReceipt -Raw | ConvertFrom-Json
    if ($runnerCode -ne 0 -or $core.run_contract.gate_eligible -or $core.verdict.status -ne 'pass' -or
        -not $core.execution.completed -or @($core.samples).Count -ne 1 -or -not $core.samples[0].valid -or
        -not $core.execution.settings.restored -or $core.samples[0].termination -ne 'closed-main-window' -or
        $core.samples[0].clock.source -ne 'windows-qpc' -or -not $core.samples[0].clock.core_ready) {
        throw "Core smoke did not supply one valid, non-performance QPC sample (exit $runnerCode)."
    }
    if (@($core.artifacts).Count -ne 1 -or $core.artifacts[0].sha256 -ne $candidateBefore.sha256 -or
        (File-Fact $exe).sha256 -ne $candidateBefore.sha256) { throw 'Candidate identity changed across trace capture.' }
    $stopAttempted = $true
    $stopCode = Invoke-OwnedWpr 'stop' @('-stop', $etl, '-skipPdbGen')
    if ($stopCode -ne 0 -or -not [IO.File]::Exists($etl)) { throw "WPR trace stop failed ($stopCode); see stop.log." }
    $stopped = $true
    $status = 'captured_pending_analysis'
} catch {
    $errorText = $_.Exception.Message
} finally {
    if ($started -and -not $stopped) {
        # Preserve an incomplete capture when the editor run failed. If saving
        # cannot finish, cancel only the uniquely named recording we started.
        if (-not $stopAttempted) {
            try {
                $code = Invoke-OwnedWpr 'stop-incomplete' @('-stop', $etl, '-skipPdbGen')
                $stopped = $code -eq 0
            } catch { $errorText += " Stop cleanup: $($_.Exception.Message)" }
        }
        if (-not $stopped) {
            try {
                $code = Invoke-OwnedWpr 'cancel-owned' @('-cancel')
                if ($code -ne 0) { $errorText += " Owned trace cleanup failed ($code); instance=$instance." }
            } catch { $errorText += " Owned trace cleanup: $($_.Exception.Message); instance=$instance." }
        }
    }
    $receipt = [ordered]@{
        schema='singleeditor.startup-trace';version=1;created_utc=[DateTime]::UtcNow.ToString('o')
        status=$status;release_eligible=$false;performance_eligible=$false;error=$errorText
        instance=$instance;collection_mode='bounded-memory';buffer_bytes=16777216
        started=$started;stopped=$stopped;process_exe_filter=[IO.Path]::GetFileName($exe)
        clock=[ordered]@{source='windows-qpc';integer_encoding='decimal-string';frequency=[Diagnostics.Stopwatch]::Frequency.ToString([Globalization.CultureInfo]::InvariantCulture)}
        candidate=$candidateBefore;candidate_after=(File-Fact $exe);runner=(File-Fact $runner);collector=(File-Fact $PSCommandPath)
        wpr=(File-Fact $wpr);profile_template=(File-Fact $template);profile=(File-Fact $profile)
        core_receipt=(File-Fact $coreReceipt);trace=(File-Fact $etl)
        commands=@($commands.ToArray());pending=$pending
    }
    [IO.File]::WriteAllText((Join-Path $output 'receipt.json'), ($receipt | ConvertTo-Json -Depth 12) + [Environment]::NewLine, $utf8)
}
Write-Host "STARTUP_TRACE=$status"
Write-Host "RECEIPT=$(Join-Path $output 'receipt.json')"
if ($status -eq 'failed') { Write-Host $errorText; exit 1 }
exit 3
