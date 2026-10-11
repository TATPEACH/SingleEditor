# Internal receipt verifiers shared by staging and fixed-portable consumers.
# Core and repeat assertions are moved byte-for-byte from ReleaseReadiness.
function Test-ReadinessCoreReceipt($Receipt, [string]$GuiPath, [string]$GuiHash, [string]$ManifestRoot) {
    Assert-Readiness ($Receipt.verdict.status -eq 'pass' -and $Receipt.execution.completed -and
        $Receipt.execution.settings.restored -and $Receipt.run_contract.profile -eq 'release' -and
        $Receipt.run_contract.gate_eligible -and $Receipt.run_contract.measurement_mode -eq 'handshake') 'A completed eligible Release handshake receipt is required.'
    $candidate=@($Receipt.artifacts | Where-Object role -eq 'candidate')
    $baseline=@($Receipt.artifacts | Where-Object role -eq 'baseline')
    Assert-Readiness ($candidate.Count -eq 1 -and $candidate[0].sha256 -eq $GuiHash -and
        [IO.Path]::GetFullPath($candidate[0].path) -ieq $GuiPath) 'Core receipt must cover the exact selected payload GUI location and bytes; equal EXE hashes alone are insufficient.'
    Assert-Readiness ($baseline.Count -eq 1 -and $baseline[0].sha256 -ne $GuiHash -and
        $null -ne $Receipt.comparison -and $candidate[0].renderer -eq $baseline[0].renderer) 'A same-renderer, distinct-artifact Core comparison is required.'
    $manifestFiles=@{contract='contract.json';corpus='corpus.json';thresholds='thresholds.json';m2_baseline='baselines/M2_FINAL_BASELINE.json'}
    foreach ($name in $manifestFiles.Keys) {
        Assert-Readiness ($Receipt.manifests.$name.sha256 -eq (Get-PayloadHash (Join-Path $ManifestRoot $manifestFiles[$name]))) "Core $name manifest differs from the current contract."
    }
    $profile=(Get-Content -LiteralPath (Join-Path $ManifestRoot 'contract.json') -Raw | ConvertFrom-Json).profiles.release
    Assert-Readiness ($Receipt.samples.Count -eq 2 * ($profile.warmup_runs + $profile.steady_runs_per_artifact)) 'Core receipt sample inventory is incomplete.'
    foreach ($role in @('candidate','baseline')) {
        $steady=@($Receipt.samples | Where-Object { $_.artifact_role -eq $role -and $_.phase -eq 'steady' })
        Assert-Readiness ($steady.Count -eq $profile.steady_runs_per_artifact) "Core steady sample count is incomplete: $role"
    }
    foreach ($sample in $Receipt.samples) {
        Assert-Readiness ($sample.valid -and $sample.mapping_present -and -not $sample.cleanup.forced -and
            $sample.cleanup.exit_code -eq 0 -and $sample.invalid_reasons.Count -eq 0) 'Core receipt contains invalid or abnormally closed samples.'
        Assert-Readiness ($null -ne $sample.metrics_ms.first_waveform -and $null -ne $sample.metrics_ms.playable -and
            $sample.metrics_ms.core_ready -eq [Math]::Max([double]$sample.metrics_ms.first_waveform,[double]$sample.metrics_ms.playable)) 'CoreReady must be the actual waveform/playable maximum.'
        if ($sample.PSObject.Properties['startup_resources']) {
            Assert-Readiness ($null -eq $sample.startup_resources) 'Resource-observed timings cannot establish a performance gate.'
        }
    }
    'Exact selected staging location; current contract; complete normal-close Release comparison. Not fixed-path or cold-cache acceptance.'
}

function Test-ReadinessResourceReceipt($Resource,[string]$GuiPath,[string]$GuiHash,
    [string]$StageRoot,$Verified,[string]$FixedPayloadRoot='') {
    $binding=$Resource.startup_resource_validation
    Assert-Readiness ($Resource.verdict.status -eq 'pass' -and $Resource.execution.completed -and
        $Resource.execution.settings.restored -and $Resource.run_contract.profile -eq 'smoke' -and
        -not $Resource.run_contract.gate_eligible -and $binding.status -eq 'pass' -and $binding.payload_unchanged -and
        $binding.bom_sha256 -eq $Verified.bom_sha256 -and $binding.policy_sha256 -eq $Verified.policy_sha256 -and
        [IO.Path]::GetFullPath($binding.stage) -ieq $StageRoot) 'Resource receipt does not bind the selected unchanged sealed stage.'
    $candidate=@($Resource.artifacts | Where-Object role -eq 'candidate')
    Assert-Readiness ($candidate.Count -eq 1 -and $candidate[0].sha256 -eq $GuiHash -and
        [IO.Path]::GetFullPath($candidate[0].path) -ieq $GuiPath -and $Resource.samples.Count -eq 1) 'Resource sample must identify the selected GUI.'
    $sample=$Resource.samples[0];$snapshot=$sample.startup_resources
    Assert-Readiness ($sample.valid -and $snapshot.status -eq 'pass' -and $snapshot.issues.Count -eq 0 -and
        $snapshot.modules.Count -gt 0 -and $snapshot.process_id -eq $sample.process_id -and
        $snapshot.capture_begin_qpc -ge $snapshot.core_ready_qpc -and
        $snapshot.capture_end_qpc -ge $snapshot.capture_begin_qpc -and
        -not $sample.cleanup.forced -and $sample.cleanup.exit_code -eq 0) 'Resource capture is incomplete or not a normal post-Core sample.'
    if ($FixedPayloadRoot) {
        Assert-Readiness ($binding.PSObject.Properties['mode'] -and $binding.mode -ceq 'fixed-portable' -and
            $binding.PSObject.Properties['payload_root'] -and
            [IO.Path]::GetFullPath($binding.payload_root).TrimEnd('\','/') -ieq $FixedPayloadRoot -and
            $binding.PSObject.Properties['gui_path'] -and
            [IO.Path]::GetFullPath($binding.gui_path) -ieq $GuiPath) 'Resource receipt is not an actual fixed-portable observation.'
    } elseif ($binding.PSObject.Properties['mode']) {
        Assert-Readiness ($binding.mode -ceq 'staging') 'A fixed-portable resource receipt cannot replace a staging observation.'
    }
}

function Test-ReadinessRepeatOpenReceipt($Receipt, [string]$ReceiptPath, [string]$GuiPath, [string]$GuiHash, [string]$ManifestRoot) {
    Assert-Readiness ($Receipt.profile -eq 'release' -and $Receipt.status -eq 'pass' -and
        $null -eq $Receipt.error -and $Receipt.reasons.Count -eq 0 -and $Receipt.pre_eligible -and
        $Receipt.settings.restored) 'A completed eligible diagnostic repeat-open Release receipt is required.'
    Assert-Readiness ($Receipt.executable_sha256 -eq $GuiHash -and
        [IO.Path]::GetFullPath($Receipt.executable) -ieq $GuiPath) 'Repeat-open receipt must cover the exact selected payload GUI location and bytes.'
    Assert-Readiness ($Receipt.harness_sha256 -eq (Get-PayloadHash (Join-Path $PSScriptRoot '../Invoke-DiagnosticOpenCycleBenchmark.ps1')) -and
        $Receipt.helper_sha256 -eq (Get-PayloadHash (Join-Path $PSScriptRoot '../Invoke-CoreOpenBenchmark.ps1')) -and
        $Receipt.thresholds_sha256 -eq (Get-PayloadHash (Join-Path $ManifestRoot 'thresholds.json'))) 'Repeat-open harness, helper or thresholds differ from the current contract.'
    $root=Split-Path -Parent ([IO.Path]::GetFullPath($ReceiptPath))
    foreach ($name in @('cycle-a.wav','cycle-b.wav')) {
        Assert-Readiness ((Get-PayloadHash (Join-Path $root $name)) -eq $Receipt.audio_sha256) 'Retained repeat-open audio differs from the measured input.'
    }
    $export=@($Receipt.sessions | Where-Object export -eq $true)
    $control=@($Receipt.sessions | Where-Object export -eq $false)
    Assert-Readiness ($Receipt.sessions.Count -eq 2 -and $export.Count -eq 1 -and $control.Count -eq 1 -and
        $export[0].process_id -gt 0 -and $control[0].process_id -gt 0 -and
        $export[0].process_id -ne $control[0].process_id) 'Repeat-open requires one export session and a distinct control session.'
    foreach ($session in $Receipt.sessions) {
        Assert-Readiness ($session.cop1_preserved -and $session.cop1.Count -eq 7 -and
            $session.cop1[1] -eq $session.process_id -and $session.termination -eq 'closed-main-window' -and
            $session.cleanup.close_requested -and $session.cleanup.exited_before_kill -and
            -not $session.cleanup.forced -and $session.cleanup.exit_code -eq 0) 'Repeat-open sessions must preserve launch marks and close normally.'
    }
    $bundle=Join-Path $root 'reviewed-snapshot.zip'
    Assert-Readiness ($export[0].export_completed -and
        (Get-PayloadHash $bundle) -eq $export[0].export_sha256) 'Actual reviewed diagnostic export is missing or changed.'
    Test-PayloadApplicationIdentity (Read-PayloadGuiIdentity $bundle) $GuiPath
    $counts=@{'startup-supplemental'=2;warmup=4;'A-before'=12;'B-after'=12;'A-control'=12}
    Assert-Readiness ($Receipt.samples.Count -eq 42) 'Repeat-open sample inventory is incomplete.'
    foreach ($phase in $counts.Keys) {
        $samples=@($Receipt.samples | Where-Object phase -eq $phase)
        Assert-Readiness ($samples.Count -eq $counts[$phase]) "Repeat-open sample count differs: $phase"
        if ($phase -in @('A-before','B-after','A-control')) {
            $expectedPid=if ($phase -eq 'A-control') {$control[0].process_id} else {$export[0].process_id}
            Assert-Readiness (@($samples | Where-Object process_id -ne $expectedPid).Count -eq 0) 'Repeat-open before/after phases must belong to the same export process, with a separate control.'
        }
    }
    $generations=[Collections.Generic.HashSet[string]]::new()
    foreach ($sample in $Receipt.samples) {
        Assert-Readiness ($sample.process_id -in @($export[0].process_id,$control[0].process_id) -and
            $sample.generation -gt 0 -and $generations.Add("$($sample.process_id):$($sample.generation)")) 'Repeat-open samples must identify distinct generations of the selected sessions.'
        Assert-Readiness ($null -ne $sample.first_waveform_ms -and $null -ne $sample.playable_ms -and
            $sample.first_waveform_ms -ge 0 -and $sample.playable_ms -ge 0 -and
            $sample.core_ready_ms -eq [Math]::Max([double]$sample.first_waveform_ms,[double]$sample.playable_ms)) 'Repeat-open CoreReady must be the actual waveform/playable maximum.'
    }
    'Exact staged GUI; real reviewed export; 36 steady opens in complete normal-close sessions. Document-generation timing, separate from launch CoreReady and full RC.'
}
