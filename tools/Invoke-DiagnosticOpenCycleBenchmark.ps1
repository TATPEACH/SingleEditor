[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [Parameter(Mandatory = $true)][string]$AudioPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateSet('smoke', 'release')][string]$Profile = 'release'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$cycleRepo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# Reuse the existing settings lease, cleanup, statistics and environment rules
# without executing or changing the original COP1 benchmark entry point.
$cycleTokens = $null; $cycleParseErrors = $null
$cycleLegacyPath = Join-Path $PSScriptRoot 'Invoke-CoreOpenBenchmark.ps1'
$cycleAst = [Management.Automation.Language.Parser]::ParseFile($cycleLegacyPath, [ref]$cycleTokens, [ref]$cycleParseErrors)
if ($cycleParseErrors.Count) { throw 'Original benchmark helpers could not be parsed.' }
$cycleHelpers = @('Assert-Condition', 'Get-Sha256Hash', 'Resolve-ExistingFile', 'Get-Median',
    'Get-NearestRankPercentile', 'Get-MetricStats', 'Get-EnvironmentSnapshot', 'Get-ProcessPathSafe',
    'Get-SingleEditorProcessDescriptions', 'Assert-NoSingleEditorProcess', 'Wait-ForNoSingleEditorProcess',
    'Stop-BenchmarkProcess', 'New-ProcessStartInfo', 'New-CoreOpenSettingsLease', 'Reset-CoreOpenSettings', 'Restore-CoreOpenSettings')
foreach ($helper in $cycleHelpers) {
    $definition = $cycleAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $helper }, $false)
    if ($null -eq $definition) { throw "Missing original helper: $helper" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms, System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
public sealed class SingleEditorOpenCycleChannel : IDisposable {
    public readonly string CycleName = "Local\\SingleEditorOpenCycle-" + Guid.NewGuid().ToString("N");
    public readonly string LegacyName = "Local\\SingleEditorCoreOpen-" + Guid.NewGuid().ToString("N");
    readonly MemoryMappedFile cycles, legacy;
    readonly MemoryMappedViewAccessor c, l;
    public SingleEditorOpenCycleChannel() {
        cycles = MemoryMappedFile.CreateNew(CycleName, 2096); c = cycles.CreateViewAccessor();
        legacy = MemoryMappedFile.CreateNew(LegacyName, 80); l = legacy.CreateViewAccessor();
        c.Write(0, 0x3159434fu); c.Write(4, 1u); c.Write(8, 2096u); c.Write(12, 64u); c.Write(16, Stopwatch.Frequency);
        l.Write(0, 0x31504f43u); l.Write(4, 1u); l.Write(8, 80u); l.Write(16, Stopwatch.Frequency);
    }
    public void BeforeLaunch() { l.Write(24, Stopwatch.GetTimestamp()); }
    public long ProcessId { get { return c.ReadInt64(24); } }
    public long Count { get { return c.ReadInt64(32); } }
    public long Overflow { get { return c.ReadInt64(40); } }
    public long[] ReadCycle(int index) {
        if (index < 0 || index >= 64) throw new ArgumentOutOfRangeException();
        int o = 48 + index * 32;
        return new long[] { c.ReadInt64(o), c.ReadInt64(o+8), c.ReadInt64(o+16), c.ReadInt64(o+24) };
    }
    public long[] Legacy() {
        return new long[] { l.ReadInt64(24), l.ReadInt64(32), l.ReadInt64(40), l.ReadInt64(48), l.ReadInt64(56), l.ReadInt64(64), l.ReadInt64(72) };
    }
    public void Dispose() { c.Dispose(); l.Dispose(); cycles.Dispose(); legacy.Dispose(); }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")]
    static extern IntPtr SendText(IntPtr hwnd, uint message, IntPtr w, string text);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")]
    static extern IntPtr ReadText(IntPtr hwnd, uint message, IntPtr capacity, System.Text.StringBuilder text);
    public static bool SetFileName(IntPtr edit, string text) {
        SendText(edit,0x000c,IntPtr.Zero,text);
        var actual=new System.Text.StringBuilder(text.Length+2);
        ReadText(edit,0x000d,(IntPtr)actual.Capacity,actual);
        return actual.ToString()==text;
    }
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int left, top, right, bottom; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    public static void Capture(IntPtr hwnd, string path) {
        RECT r; if (!GetWindowRect(hwnd, out r)) return;
        using (var bitmap = new System.Drawing.Bitmap(Math.Max(1,r.right-r.left),Math.Max(1,r.bottom-r.top)))
        using (var g = System.Drawing.Graphics.FromImage(bitmap)) {
            IntPtr dc=g.GetHdc(); bool ok;
            try { ok=PrintWindow(hwnd,dc,2); } finally { g.ReleaseHdc(dc); }
            if(ok) bitmap.Save(path,System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
'@
function Find-CycleElement($Root, [string]$Name, [string]$AutomationId = '') {
    $property = if ($AutomationId) { [Windows.Automation.AutomationElement]::AutomationIdProperty } else { [Windows.Automation.AutomationElement]::NameProperty }
    $value = if ($AutomationId) { $AutomationId } else { $Name }
    return $Root.FindFirst([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.PropertyCondition]::new($property, $value))
}
function Find-CycleOwned([int]$OwnerPid, [string]$Name, [string]$AutomationId = '') {
    $property = if ($AutomationId) { [Windows.Automation.AutomationElement]::AutomationIdProperty } else { [Windows.Automation.AutomationElement]::NameProperty }
    $value = if ($AutomationId) { $AutomationId } else { $Name }
    $condition = [Windows.Automation.AndCondition]::new(
        [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ProcessIdProperty, $OwnerPid),
        [Windows.Automation.PropertyCondition]::new($property, $value))
    return [Windows.Automation.AutomationElement]::RootElement.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
}
function Wait-CycleCondition([scriptblock]$Condition, [string]$Description, [int]$Milliseconds = 15000) {
    $wait = [Diagnostics.Stopwatch]::StartNew()
    while ($wait.ElapsedMilliseconds -lt $Milliseconds) {
        $answer = & $Condition
        if ($answer) { return $answer }
        Start-Sleep -Milliseconds 25
    }
    throw "Timed out: $Description"
}
function Invoke-CycleElement($Element) {
    Assert-Condition ($null -ne $Element) 'Missing UIA action.'
    $pattern = $null
    Assert-Condition ($Element.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) 'UIA action has no InvokePattern.'
    ([Windows.Automation.InvokePattern]$pattern).Invoke()
}
function Set-CycleText($Element, [string]$Value, [IntPtr]$Window) {
    $pattern = $null
    if ($Element.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$pattern)) {
        ([Windows.Automation.ValuePattern]$pattern).SetValue($Value); return
    }
    $Element.SetFocus()
    [Windows.Forms.SendKeys]::SendWait('^a')
    foreach ($character in $Value.ToCharArray()) {
        [void][SingleEditorOpenCycleChannel]::PostMessage($Window, 0x0102, [IntPtr][int]$character, [IntPtr]1)
    }
}
function Export-CycleSnapshot($Process, [string]$Destination) {
    $Process.Refresh(); $windowHandle = $Process.MainWindowHandle
    $root = [Windows.Automation.AutomationElement]::FromHandle($windowHandle)
    $focusAccepted = [SingleEditorOpenCycleChannel]::SetForegroundWindow($windowHandle)
    Start-Sleep -Milliseconds 100
    [uint32]$foregroundPid = 0
    [void][SingleEditorOpenCycleChannel]::GetWindowThreadProcessId([SingleEditorOpenCycleChannel]::GetForegroundWindow(), [ref]$foregroundPid)
    $script:cycleFocus.Add([pscustomobject]@{ method='SetForegroundWindow'; owner_pid=$Process.Id; window=$windowHandle.ToInt64(); accepted=$focusAccepted; foreground_pid=$foregroundPid })
    if ($foregroundPid -ne $Process.Id) {
        # UIA focus is a bounded second request to this owned window. It is not
        # proof of foreground: keep the actual process check before any input.
        $root.SetFocus()
        [void][SingleEditorOpenCycleChannel]::SetForegroundWindow($windowHandle)
        Start-Sleep -Milliseconds 100
        [void][SingleEditorOpenCycleChannel]::GetWindowThreadProcessId([SingleEditorOpenCycleChannel]::GetForegroundWindow(), [ref]$foregroundPid)
        $script:cycleFocus.Add([pscustomobject]@{ method='UIA.SetFocus'; owner_pid=$Process.Id; window=$windowHandle.ToInt64(); foreground_pid=$foregroundPid })
    }
    Assert-Condition ($foregroundPid -eq $Process.Id) 'Owned editor did not acquire foreground focus.'
    # Invoke the visible product route. Synthetic Ctrl+Shift+P did not produce
    # a palette in smoke-1/3; shortcut acceptance remains a separate UI gate.
    Invoke-CycleElement (Find-CycleElement $root 'More file, edit and view commands')
    Invoke-CycleElement (Wait-CycleCondition { Find-CycleOwned $Process.Id 'Command Palette...' } 'command palette menu item')
    $search = Wait-CycleCondition { Find-CycleElement $root 'Search commands' } 'command palette search'
    Set-CycleText $search 'Local Diagnostics' $windowHandle
    Start-Sleep -Milliseconds 250
    Invoke-CycleElement (Wait-CycleCondition { Find-CycleElement $root 'Run selected command' } 'command palette action')
    $dialog = Wait-CycleCondition { Find-CycleOwned $Process.Id 'Local Diagnostics' } 'diagnostic window'
    $save = Wait-CycleCondition {
        $control = Find-CycleElement $dialog 'Save reviewed bundle...'
        if ($control -and $control.Current.IsEnabled) { $control }
    } 'ready diagnostic review' 30000
    Invoke-CycleElement $save
    $fileDialog = Wait-CycleCondition {
        $condition = [Windows.Automation.AndCondition]::new(
            [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ProcessIdProperty, $Process.Id),
            [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ClassNameProperty, '#32770'))
        [Windows.Automation.AutomationElement]::RootElement.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
    } 'native Save top-level dialog'
    $script:cycleChooserHandle = [IntPtr]$fileDialog.Current.NativeWindowHandle
    Assert-Condition ($script:cycleChooserHandle -ne [IntPtr]::Zero) 'Native Save dialog has no window handle.'
    $fileName = Wait-CycleCondition {
        $edit = Find-CycleElement $fileDialog '' 'FileNameControlHost'
        if ($edit) {
            $edit = $edit.FindFirst([Windows.Automation.TreeScope]::Descendants,
                [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ClassNameProperty, 'Edit'))
        }
        $edit
    } 'native Save file-name edit'
    # This Windows dialog exposes native Edit/Button handles but omits their
    # UIA Edit/Invoke patterns. Use the observed, owned controls themselves.
    $editHandle=[IntPtr]$fileName.Current.NativeWindowHandle
    [uint32]$editPid=0
    [void][SingleEditorOpenCycleChannel]::GetWindowThreadProcessId($editHandle,[ref]$editPid)
    Assert-Condition ($editPid -eq $Process.Id) 'File-name edit belongs to another process.'
    Assert-Condition ([SingleEditorOpenCycleChannel]::SetFileName($editHandle,$Destination)) 'Native file-name text did not match the destination.'
    $saveHandle=[SingleEditorOpenCycleChannel]::GetDlgItem($script:cycleChooserHandle,1)
    Assert-Condition ($saveHandle -ne [IntPtr]::Zero) 'Native Save button not identified.'
    [void][SingleEditorOpenCycleChannel]::PostMessage($saveHandle,0x00f5,[IntPtr]::Zero,[IntPtr]::Zero)
    [void](Wait-CycleCondition { [IO.File]::Exists($Destination) -and $save.Current.IsEnabled } 'completed reviewed export' 30000)
    $dialogHandle = [IntPtr]$dialog.Current.NativeWindowHandle
    Assert-Condition ($dialogHandle -ne [IntPtr]::Zero) 'Diagnostic window has no native handle.'
    [void][SingleEditorOpenCycleChannel]::PostMessage($dialogHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    [void](Wait-CycleCondition { $null -eq (Find-CycleOwned $Process.Id 'Local Diagnostics') } 'released diagnostic window')
    Start-Sleep -Milliseconds 500
}
function Read-CycleReady($Channel, $Process, [int]$Index, [string]$Phase, [string]$File) {
    $raw = Wait-CycleCondition {
        Assert-Condition (-not $Process.HasExited) 'Measured editor exited during a sample.'
        Assert-Condition ($Channel.Overflow -eq 0) 'Cycle mapping capacity was exceeded.'
        if ($Channel.Count -gt $Index) {
            $value = $Channel.ReadCycle($Index)
            if ($value[2] -gt 0 -and $value[3] -gt 0) { ,$value }
        }
    } 'actual waveform and playable milestones'
    Assert-Condition ($Channel.ProcessId -eq $Process.Id) 'Cycle belongs to another process.'
    Assert-Condition ($Channel.Count -eq $Index + 1) 'Unexpected document generation intervened.'
    Assert-Condition ($raw[0] -gt 0 -and $raw[1] -gt 0 -and $raw[2] -ge $raw[1] -and $raw[3] -ge $raw[1]) 'Invalid cycle chronology.'
    $wave = 1000.0 * ($raw[2] - $raw[1]) / [Diagnostics.Stopwatch]::Frequency
    $play = 1000.0 * ($raw[3] - $raw[1]) / [Diagnostics.Stopwatch]::Frequency
    return [pscustomobject]@{ phase=$Phase; process_id=$Process.Id; index=$Index; generation=$raw[0]; file=[IO.Path]::GetFileName($File)
        first_waveform_ms=$wave; playable_ms=$play; core_ready_ms=[Math]::Max($wave,$play); raw_qpc=$raw }
}
function Open-CycleFile($Channel, $Process, [string]$Phase, [string]$File) {
    $index = [int]$Channel.Count
    $launch = [Diagnostics.Process]::Start((New-ProcessStartInfo $script:cycleArtifact $File))
    try {
        Assert-Condition ($launch.WaitForExit(5000)) 'Single-instance file-open launcher did not return.'
        Assert-Condition ($launch.ExitCode -eq 0) 'Single-instance file-open launcher failed.'
        return Read-CycleReady $Channel $Process $index $Phase $File
    } finally {
        if (-not $launch.HasExited) { $details=@{}; [void](Stop-BenchmarkProcess $launch $details) }
        $launch.Dispose()
    }
}
function Invoke-CycleSession([bool]$WithExport) {
    Reset-CoreOpenSettings $script:cycleSettings
    $channel = [SingleEditorOpenCycleChannel]::new(); $process = $null
    $script:cycleChooserHandle = [IntPtr]::Zero
    $cleanup = [ordered]@{}; $legacyBefore = $null; $legacyAfter = $null
    $session = [ordered]@{ export=$WithExport; process_id=$null; cleanup=$cleanup; cop1_preserved=$false }
    $script:cycleSessions.Add($session)
    try {
        $info = New-ProcessStartInfo $script:cycleArtifact $script:cycleFiles[0]
        $info.EnvironmentVariables['SINGLE_EDITOR_OPENCYCLE_MAPPING'] = $channel.CycleName
        $info.EnvironmentVariables['SINGLE_EDITOR_COREOPEN_MAPPING'] = $channel.LegacyName
        $channel.BeforeLaunch(); $process = [Diagnostics.Process]::Start($info); $session.process_id=$process.Id
        $script:cycleSamples.Add((Read-CycleReady $channel $process 0 'startup-supplemental' $script:cycleFiles[0]))
        [void](Wait-CycleCondition { $values=$channel.Legacy(); $values[6] -gt 0 } 'complete original COP1 startup observation')
        $legacyBefore = $channel.Legacy(); $session.cop1=$legacyBefore
        $nextFile = 0
        for ($i=0; $i -lt 2; $i++) {
            $nextFile = 1-$nextFile
            $script:cycleSamples.Add((Open-CycleFile $channel $process 'warmup' $script:cycleFiles[$nextFile]))
            Start-Sleep -Milliseconds 200
        }
        $phases = if ($WithExport) { @('A-before','B-after') } else { @('A-control') }
        foreach ($phase in $phases) {
            if ($phase -eq 'B-after') {
                $beforeExportCount = $channel.Count
                Export-CycleSnapshot $process $script:cycleBundle
                Assert-Condition ($channel.Count -eq $beforeExportCount) 'Passive export changed document generation.'
                Assert-Condition (-not $process.HasExited) 'Export terminated the measured process.'
                $session.export_completed=$true
                $session.export_sha256=Get-Sha256Hash $script:cycleBundle
            }
            for ($i=0; $i -lt $script:cycleCount; $i++) {
                $nextFile = 1-$nextFile
                $script:cycleSamples.Add((Open-CycleFile $channel $process $phase $script:cycleFiles[$nextFile]))
                Start-Sleep -Milliseconds 200
            }
        }
        $legacyAfter = $channel.Legacy()
        Assert-Condition (($legacyBefore -join ',') -eq ($legacyAfter -join ',')) 'Repeat-open activity changed original COP1 milestones.'
        $session.cop1_preserved=$true
    } catch {
        $session.failure=$_.Exception.Message
        $session.failure_stack=$_.ScriptStackTrace
        # Retain a bounded view of this owned window before cleanup. No other
        # application or desktop content is included in failure evidence.
        if ($process -and -not $process.HasExited) {
            try {
                $process.Refresh()
                $windows = [Windows.Automation.AutomationElement]::RootElement.FindAll([Windows.Automation.TreeScope]::Children,
                    [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id))
                $session.failure_windows=@($windows | ForEach-Object {
                    [pscustomobject]@{name=$_.Current.Name;class=$_.Current.ClassName;handle=$_.Current.NativeWindowHandle}
                })
                $root = [Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
                [SingleEditorOpenCycleChannel]::Capture($process.MainWindowHandle, (Join-Path $cycleOut 'failure-window.png'))
                $elements = $root.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
                $session.failure_native_elements=@($elements | Where-Object { $_.Current.NativeWindowHandle -ne 0 } | Select-Object -First 48 | ForEach-Object {
                    [pscustomobject]@{name=$_.Current.Name;class=$_.Current.ClassName;id=$_.Current.AutomationId;handle=$_.Current.NativeWindowHandle}
                })
                $session.failure_controls=@($elements | Where-Object {
                    $_.Current.AutomationId -notlike 'System.*' -and
                    $_.Current.ControlType -in @([Windows.Automation.ControlType]::Edit, [Windows.Automation.ControlType]::Button, [Windows.Automation.ControlType]::ComboBox)
                } | Select-Object -First 64 | ForEach-Object {
                    [pscustomobject]@{name=$_.Current.Name;id=$_.Current.AutomationId;type=$_.Current.ControlType.ProgrammaticName}
                })
            } catch { $session.failure_controls_error=$_.Exception.Message }
        }
        throw
    } finally {
        if ($script:cycleChooserHandle -ne [IntPtr]::Zero -and $process -and -not $process.HasExited) {
            [uint32]$chooserPid = 0
            [void][SingleEditorOpenCycleChannel]::GetWindowThreadProcessId($script:cycleChooserHandle, [ref]$chooserPid)
            if ($chooserPid -eq $process.Id) {
                [void][SingleEditorOpenCycleChannel]::PostMessage($script:cycleChooserHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
                Start-Sleep -Milliseconds 200
            }
        }
        if ($process) { $session.termination=Stop-BenchmarkProcess $process $cleanup; $process.Dispose() }
        $channel.Dispose()
    }
    Assert-Condition (-not $cleanup.forced -and $cleanup.exit_code -eq 0) 'Measured editor did not close normally.'
    Assert-Condition (Wait-ForNoSingleEditorProcess) 'Owned editor remained after closure.'
}

$script:cycleArtifact = [pscustomobject]@{path=(Resolve-ExistingFile $ExePath 'Executable');renderer='modern'}
$cycleAudio = Resolve-ExistingFile $AudioPath 'Audio'
$cycleOut = [IO.Path]::GetFullPath($OutputDirectory)
Assert-Condition (-not (Test-Path -LiteralPath $cycleOut)) 'Use a new output directory; earlier evidence is immutable.'
[void][IO.Directory]::CreateDirectory($cycleOut)
$script:cycleCount = if ($Profile -eq 'release') { 12 } else { 2 }
$script:cycleFiles = @((Join-Path $cycleOut 'cycle-a.wav'), (Join-Path $cycleOut 'cycle-b.wav'))
$script:cycleBundle = Join-Path $cycleOut 'reviewed-snapshot.zip'
$script:cycleSamples = [Collections.Generic.List[object]]::new()
$script:cycleSessions = [Collections.Generic.List[object]]::new()
$script:cycleFocus = [Collections.Generic.List[object]]::new()
$cycleThresholdPath = Join-Path $cycleRepo 'Document/New_JuceEditor/milestone3/performance/core-open-v1/thresholds.json'
$cycleThresholds = Get-Content -LiteralPath $cycleThresholdPath -Raw | ConvertFrom-Json
$cycleReceipt = [ordered]@{schema='singleeditor.diagnostic-repeat-open';version=1;profile=$Profile;status='incomplete'
    metric='document-generation entry to max(actual first waveform, actual playable); separate from COP1 launch KPI'
    executable=$script:cycleArtifact.path;executable_sha256=(Get-Sha256Hash $script:cycleArtifact.path)
    audio_sha256=(Get-Sha256Hash $cycleAudio);thresholds_sha256=(Get-Sha256Hash $cycleThresholdPath)
    harness_sha256=(Get-Sha256Hash $PSCommandPath)
    helper_sha256=(Get-Sha256Hash $cycleLegacyPath);samples=$script:cycleSamples;sessions=$script:cycleSessions;focus=$script:cycleFocus
    pre=$null;post=$null;summary=$null;reasons=@();error=$null;settings=$null}
$script:cycleSettings = $null
try {
    Assert-NoSingleEditorProcess
    foreach ($file in $script:cycleFiles) { [IO.File]::Copy($cycleAudio,$file); Assert-Condition ((Get-Sha256Hash $file) -eq $cycleReceipt.audio_sha256) 'Cycle WAV copy changed.' }
    $script:cycleSettings = New-CoreOpenSettingsLease (Join-Path $env:APPDATA 'SingleEditor/SingleEditor.settings') (Join-Path $cycleOut 'settings')
    $cycleReceipt.settings=$script:cycleSettings
    $cycleReceipt.pre=Get-EnvironmentSnapshot
    $pre=$cycleReceipt.pre; $gate=$cycleThresholds.environment_gate
    Assert-Condition ($null -ne $pre.cpu_median_percent -and $null -ne $pre.memory_available_percent) 'Preflight observations unavailable.'
    $preEligible=($pre.cpu_median_percent -le $gate.max_pre_run_cpu_median_percent -and $pre.cpu_max_percent -le $gate.max_single_pre_run_cpu_percent -and $pre.memory_available_percent -ge $gate.min_available_memory_percent)
    $cycleReceipt.pre_eligible=$preEligible
    if ($Profile -eq 'release' -and -not $preEligible) {
        $cycleReceipt.status='inconclusive_environment'; throw 'Preflight was not eligible; no measured editor launched.'
    }
    Invoke-CycleSession $true
    Invoke-CycleSession $false
    $cycleReceipt.post=Get-EnvironmentSnapshot
    $stats=[ordered]@{}
    foreach ($phase in @('A-before','B-after','A-control')) {
        $values=@($script:cycleSamples | Where-Object phase -eq $phase | ForEach-Object core_ready_ms)
        Assert-Condition ($values.Count -eq $script:cycleCount) 'Missing requested steady cycles.'
        $stats[$phase]=Get-MetricStats $values
    }
    $cycleReceipt.summary=$stats
    $baseline=Get-Median @($stats['A-before'].median_ms,$stats['A-control'].median_ms)
    $delta=$stats['B-after'].median_ms-$baseline
    $percent=100*$delta/$baseline
    $drift=$stats['A-control'].median_ms-$stats['A-before'].median_ms
    $cycleReceipt.comparison=@{baseline_median_ms=$baseline;delta_ms=$delta;delta_percent=$percent;control_drift_ms=$drift}
    $reasons=[Collections.Generic.List[string]]::new()
    if (-not $preEligible) { $reasons.Add('pre-environment') }
    foreach ($value in $stats.Values) { if ($value.mad_ms/$value.median_ms -gt $gate.max_core_ready_mad_ratio) { $reasons.Add('core-ready-noise') } }
    if ([Math]::Abs($drift) -gt $gate.minimum_practical_drift_ms -and 100*[Math]::Abs($drift)/$stats['A-before'].median_ms -gt $gate.max_control_half_drift_percent) { $reasons.Add('control-drift') }
    $post=$cycleReceipt.post
    if ($null -eq $post.cpu_median_percent -or $null -eq $post.memory_available_percent -or $post.cpu_median_percent -gt $gate.max_pre_run_cpu_median_percent -or $post.cpu_max_percent -gt $gate.max_single_pre_run_cpu_percent -or $post.memory_available_percent -lt $gate.min_available_memory_percent) { $reasons.Add('post-environment') }
    $cycleReceipt.reasons=@($reasons.ToArray())
    # Smoke establishes UI/protocol/cleanup functionality only. Keep every
    # environment observation, but never present smoke as performance acceptance.
    if ($Profile -eq 'smoke') { $cycleReceipt.status='smoke_pass' }
    elseif ($reasons.Count) { $cycleReceipt.status='inconclusive_environment' }
    elseif ($stats['B-after'].max_ms -gt $cycleThresholds.absolute_ceiling_ms.core_ready -or ($delta -ge $cycleThresholds.relative_gate.minimum_practical_delta_ms -and $percent -ge $cycleThresholds.relative_gate.regression_percent)) { $cycleReceipt.status='regression' }
    else { $cycleReceipt.status=if ($Profile -eq 'release') {'pass'} else {'smoke_pass'} }
    $cycleReceipt.warning=($percent -ge $cycleThresholds.relative_gate.warn_percent)
    Assert-Condition ((Get-Sha256Hash $script:cycleArtifact.path) -eq $cycleReceipt.executable_sha256) 'Measured artifact changed.'
} catch {
    $cycleReceipt.error=$_.Exception.Message
    if ($cycleReceipt.status -ne 'inconclusive_environment') { $cycleReceipt.status='failed' }
} finally {
    if ($script:cycleSettings) {
        try { Restore-CoreOpenSettings $script:cycleSettings } catch { $cycleReceipt.error=$_.Exception.Message; $cycleReceipt.status='failed' }
    }
    $cycleReceipt | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $cycleOut 'receipt.json') -Encoding utf8
}
Write-Output "VERDICT=$($cycleReceipt.status)"
if ($cycleReceipt.error) { Write-Output $cycleReceipt.error }
Write-Output "RECEIPT=$(Join-Path $cycleOut 'receipt.json')"
if ($cycleReceipt.status -eq 'pass' -or $cycleReceipt.status -eq 'smoke_pass') { exit 0 }
if ($cycleReceipt.status -eq 'inconclusive_environment') { exit 3 }
exit 1
