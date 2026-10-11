param(
    [Parameter(Mandatory = $true)]
    [string] $CliPath,

    [Parameter(Mandatory = $true)]
    [string] $InputWav
)

$ErrorActionPreference = 'Stop'
$cli = (Resolve-Path -LiteralPath $CliPath).Path
$fixture = (Resolve-Path -LiteralPath $InputWav).Path
$runRoot = Join-Path $env:TEMP ('singleeditor-m3-18-e2e-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null

function Invoke-SingleEditorCli {
    param(
        [string] $Step,
        [string[]] $Arguments
    )

    $lines = @(& $cli @Arguments)
    $exitCode = $LASTEXITCODE
    $terminal = $lines |
        ForEach-Object { $_ | ConvertFrom-Json } |
        Where-Object { $_.terminal } |
        Select-Object -Last 1
    if ($null -eq $terminal) {
        throw "$Step returned no terminal JSONL event"
    }
    [pscustomobject]@{
        step = $Step
        exit = $exitCode
        terminal_type = $terminal.type
        ok = $terminal.ok
        error_code = $terminal.error.code
        status = $terminal.payload.status
        processed = $terminal.payload.processed
        succeeded = $terminal.payload.succeeded
        failed = $terminal.payload.failed
        resume_skipped = $terminal.payload.resume_skipped
    }
}

try {
    $eqInput = Join-Path $runRoot 'eq-input.wav'
    $limiterInput = Join-Path $runRoot 'limiter-input.wav'
    Copy-Item -LiteralPath $fixture -Destination $eqInput
    Copy-Item -LiteralPath $fixture -Destination $limiterInput

    $types = @('high-pass', 'low-shelf', 'bell', 'bell', 'bell', 'bell',
               'high-shelf', 'low-pass')
    $frequencies = @(30.0, 100.0, 250.0, 500.0, 1000.0, 2500.0,
                     8000.0, 20000.0)
    $bands = @()
    for ($index = 0; $index -lt 8; ++$index) {
        $bands += [ordered]@{
            index = $index
            enabled = $index -eq 0 -or $index -eq 4
            type = $types[$index]
            frequency_hz = $frequencies[$index]
            gain_db = if ($index -eq 4) { 3.0 } else { 0.0 }
            q = if ($index -eq 4) { 0.9 } else { 0.7071067811865476 }
            slope = if ($index -eq 0) { 'db24' } else { 'db12' }
        }
    }
    $eqParameters = [ordered]@{ preset_id = 'cli-e2e'; bands = $bands }
    $limiterParameters = [ordered]@{
        enabled = $true
        threshold_db = -12.0
        ceiling_dbtp = -1.0
        lookahead_ms = 5.0
        release_mode = 'automatic'
        release_ms = 120.0
        oversampling = 'sample-rate-aware-fixed'
        channels_linked = $true
    }
    $utf8 = [Text.UTF8Encoding]::new($false)
    $eqParametersPath = Join-Path $runRoot 'eq.json'
    $limiterParametersPath = Join-Path $runRoot 'limiter.json'
    [IO.File]::WriteAllText(
        $eqParametersPath,
        ($eqParameters | ConvertTo-Json -Depth 8),
        $utf8)
    [IO.File]::WriteAllText(
        $limiterParametersPath,
        ($limiterParameters | ConvertTo-Json -Depth 4),
        $utf8)

    $eqPlan = Join-Path $runRoot 'eq.plan.json'
    $limiterPlan = Join-Path $runRoot 'limiter.plan.json'
    $eqOutput = Join-Path $runRoot 'eq-output.wav'
    $limiterOutput = Join-Path $runRoot 'limiter-output.wav'
    $batchPlan = Join-Path $runRoot 'batch.plan.json'
    $evidence = Join-Path $runRoot 'batch.evidence.json'

    $results = @()
    $results += Invoke-SingleEditorCli 'eq-analyze' @(
        'analyze', 'eq', '--input', $eqInput, '--plan', $eqPlan,
        '--parameters', $eqParametersPath, '--destination-mode', 'create_new',
        '--destination', $eqOutput, '--jsonl')
    $results += Invoke-SingleEditorCli 'limiter-analyze' @(
        'analyze', 'limiter', '--input', $limiterInput, '--plan', $limiterPlan,
        '--parameters', $limiterParametersPath, '--destination-mode', 'create_new',
        '--destination', $limiterOutput, '--jsonl')
    $results += Invoke-SingleEditorCli 'batch-plan' @(
        'batch-plan', '--output', $batchPlan, '--item', 'eq-item', $eqPlan,
        '--item', 'limiter-item', $limiterPlan, '--jsonl')
    $results += Invoke-SingleEditorCli 'batch-apply' @(
        'batch-apply', '--batch-plan', $batchPlan, '--evidence', $evidence,
        '--jsonl')
    $results += Invoke-SingleEditorCli 'batch-resume' @(
        'batch-apply', '--batch-plan', $batchPlan, '--evidence', $evidence,
        '--resume', '--jsonl')

    if (@($results | Where-Object { $_.exit -ne 0 }).Count -ne 0) {
        throw 'One or more CLI E2E steps failed'
    }
    $evidenceObject = Get-Content -Raw -LiteralPath $evidence | ConvertFrom-Json
    if ($evidenceObject.status -ne 'completed' -or
        @($evidenceObject.items | Where-Object { $_.status -ne 'succeeded' }).Count -ne 0) {
        throw 'Batch evidence did not durably complete every item'
    }
    [pscustomobject]@{
        steps = $results
        outputs = @(
            [pscustomobject]@{
                file = 'eq-output.wav'
                exists = Test-Path -LiteralPath $eqOutput
                bytes = (Get-Item -LiteralPath $eqOutput).Length
            },
            [pscustomobject]@{
                file = 'limiter-output.wav'
                exists = Test-Path -LiteralPath $limiterOutput
                bytes = (Get-Item -LiteralPath $limiterOutput).Length
            })
        evidence_status = $evidenceObject.status
        evidence_items = @($evidenceObject.items | ForEach-Object { $_.status })
    } | ConvertTo-Json -Depth 6 -Compress
}
finally {
    $resolvedRunRoot = [IO.Path]::GetFullPath($runRoot)
    $resolvedTemp = [IO.Path]::GetFullPath($env:TEMP)
    $safeTemporaryRoot = $resolvedRunRoot.StartsWith(
        $resolvedTemp,
        [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedRunRoot).StartsWith('singleeditor-m3-18-e2e-')
    if ($safeTemporaryRoot -and (Test-Path -LiteralPath $resolvedRunRoot)) {
        Remove-Item -LiteralPath $resolvedRunRoot -Recurse -Force
    }
}
