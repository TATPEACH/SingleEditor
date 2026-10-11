[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CliPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-RoutingJson {
    param([string[]]$Arguments)
    $raw = & $CliPath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Routing CLI exited with $LASTEXITCODE`: $raw"
    }
    return $raw | ConvertFrom-Json
}

$resolvedCli = (Resolve-Path -LiteralPath $CliPath).Path
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) "singleeditor-m3-20-stage-graph-e2e"
$foa = Join-Path $testRoot "m3-20-front-foa.wav"
$stereo = Join-Path $testRoot "m3-20-stereo.wav"

New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
try {
    & $ffmpeg -hide_banner -loglevel error -y -f lavfi `
        -i "aevalsrc=0.25|0|0|0.25:s=48000:d=0.1:c=4c" -c:a pcm_f32le $foa
    Assert-True ($LASTEXITCODE -eq 0) "Could not create the front-facing FOA fixture"

    & $ffmpeg -hide_banner -loglevel error -y -f lavfi `
        -i "anullsrc=r=48000:cl=stereo" -t 0.1 -c:a pcm_s24le $stereo
    Assert-True ($LASTEXITCODE -eq 0) "Could not create the ordinary stereo fixture"

    $foaResult = (Invoke-RoutingJson -Arguments @(
        "routing", "--input", $foa, "--source-preset", "foa-acn-sn3d",
        "--output-channels", "12", "--runtime-probe", "--json"
    )).results[0]
    Assert-True ([bool]$foaResult.source_layout.ambisonic) `
        "Explicit FOA was not interpreted as Ambisonics"
    Assert-True ($foaResult.source_layout.monitor_decode_mode -eq "canonical_stage_graph_max_re") `
        "FOA did not use the canonical stage graph"
    Assert-True ([int]$foaResult.source_layout.ambisonic_stage_graph_schema -eq 1) `
        "Stage graph schema is not version 1"
    Assert-True ($foaResult.source_layout.ambisonic_decoder_profile `
                    -eq "max-re-137.9deg-fixed-reference-v1") `
        "The locked max-rE profile identity changed"
    Assert-True ($foaResult.source_layout.ambisonic_stage_order `
                    -eq "validate-convert>rotate>analyze>decode-max-re>canonical-speaker-bus>role-mapping>physical-device>meter") `
        "The canonical stage order changed"
    Assert-True ([bool]$foaResult.runtime_render_probe.ambisonic_field.valid `
                    -and [int]$foaResult.runtime_render_probe.ambisonic_field.order -eq 1) `
        "The explicitly leased runtime analyzer did not publish truthful FOA energy"
    Assert-True (-not [bool]$foaResult.runtime_render_probe.channels[3].active) `
        "FOA decode fed directional energy to LFE"
    foreach ($output in @(0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11)) {
        Assert-True ([bool]$foaResult.runtime_render_probe.channels[$output].active) `
            "Low-order decode did not energize canonical non-LFE output $($output + 1)"
    }

    $yaw0 = (Invoke-RoutingJson -Arguments @(
        "routing", "--input", $foa, "--source-preset", "foa-acn-sn3d",
        "--output-channels", "2", "--ambisonic-yaw", "0", "--json"
    )).results[0]
    $yaw90 = (Invoke-RoutingJson -Arguments @(
        "routing", "--input", $foa, "--source-preset", "foa-acn-sn3d",
        "--output-channels", "2", "--ambisonic-yaw", "90", "--json"
    )).results[0]
    $yaw0LeftX = [double]$yaw0.routes[3].outputs[0].gain
    $yaw90LeftX = [double]$yaw90.routes[3].outputs[0].gain
    Assert-True ([math]::Abs($yaw0LeftX - $yaw90LeftX) -gt 0.01) `
        "Continuous yaw did not deterministically change the decode matrix"

    $stereoResult = (Invoke-RoutingJson -Arguments @(
        "routing", "--input", $stereo, "--output-channels", "2", "--json"
    )).results[0]
    Assert-True (-not [bool]$stereoResult.source_layout.ambisonic `
                    -and $stereoResult.source_layout.monitor_decode_runtime_state -eq "not_required") `
        "Ordinary stereo entered an Ambisonics runtime"
    Assert-True ($null -eq $stereoResult.source_layout.ambisonic_stage_graph_schema `
                    -and $null -eq $stereoResult.source_layout.ambisonic_decoder_profile) `
        "Ordinary stereo exposed an allocated Ambisonics stage"

    [pscustomobject]@{
        status = "pass"
        cli = $resolvedCli
        stage_schema = [int]$foaResult.source_layout.ambisonic_stage_graph_schema
        decoder_profile = $foaResult.source_layout.ambisonic_decoder_profile
        runtime_field_order = [int]$foaResult.runtime_render_probe.ambisonic_field.order
        active_non_lfe_outputs = @($foaResult.runtime_render_probe.channels |
            Where-Object { $_.active }).Count
        yaw_left_x_delta = [math]::Abs($yaw0LeftX - $yaw90LeftX)
        stereo_spatial_runtime = $stereoResult.source_layout.monitor_decode_runtime_state
    } | ConvertTo-Json -Compress
}
finally {
    foreach ($fixture in @($foa, $stereo)) {
        if (Test-Path -LiteralPath $fixture) {
            Remove-Item -LiteralPath $fixture -Force
        }
    }
    if ((Test-Path -LiteralPath $testRoot) `
        -and @(Get-ChildItem -LiteralPath $testRoot -Force).Count -eq 0) {
        Remove-Item -LiteralPath $testRoot -Force
    }
}
