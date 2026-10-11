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
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) "singleeditor-m3-19-routing-e2e"
$multichannel = Join-Path $testRoot "m3-19-wave-7.1.wav"
$stereo = Join-Path $testRoot "m3-19-stereo.wav"

New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
try {
    & $ffmpeg -hide_banner -loglevel error -y -f lavfi `
        -i "anullsrc=r=48000:cl=7.1" -t 0.1 -c:a pcm_s24le $multichannel
    Assert-True ($LASTEXITCODE -eq 0) "Could not create the 7.1 WAVE fixture"

    & $ffmpeg -hide_banner -loglevel error -y -f lavfi `
        -i "anullsrc=r=48000:cl=stereo" -t 0.1 -c:a pcm_s24le $stereo
    Assert-True ($LASTEXITCODE -eq 0) "Could not create the stereo WAVE fixture"

    $multi = Invoke-RoutingJson -Arguments @(
        "routing", "--input", $multichannel, "--output-channels", "8",
        "--output-map", "Lsr=5", "--output-map", "Rsr=6",
        "--output-map", "Lss=7", "--output-map", "Rss=8", "--json"
    )
    $multiResult = $multi.results[0]
    Assert-True ([bool]$multi.ok) "Multichannel routing inspection did not succeed"
    Assert-True ($multiResult.source_layout.interpretation_origin -eq "wave_channel_mask") `
        "7.1 WAVE source interpretation did not retain mask provenance"
    Assert-True ($multiResult.source_layout.order -eq "L R C LFE Lsr Rsr Lss Rss") `
        "7.1 WAVE source order is not deterministic"
    Assert-True ([bool]$multiResult.physical_output_matrix.valid) `
        "Physical output matrix is invalid"
    Assert-True ([int]$multiResult.physical_output_matrix.active_output_channels -eq 8) `
        "Physical output matrix did not adapt to eight outputs"

    $rearLeft = $multiResult.physical_output_matrix.routes |
        Where-Object { $_.role -eq "Lsr" } | Select-Object -First 1
    Assert-True ($null -ne $rearLeft -and [int]$rearLeft.physical_output_channel -eq 5) `
        "Rear-left canonical role did not map to physical output 5"

    $rearLeftPing = $multiResult.silent_route_probe.role_pings |
        Where-Object { $_.source_role -eq "Lsr" } | Select-Object -First 1
    Assert-True (-not [bool]$multiResult.silent_route_probe.audio_device_required) `
        "Silent route probe unexpectedly requires an audio device"
    Assert-True (-not [bool]$multiResult.silent_route_probe.audible_output_emitted) `
        "Silent route probe claims audible output"
    Assert-True ($null -ne $rearLeftPing -and [bool]$rearLeftPing.routed `
                 -and [int]$rearLeftPing.outputs[0].physical_output_channel -eq 5) `
        "Rear-left silent role Ping did not prove physical output 5"

    $stereoResult = (Invoke-RoutingJson -Arguments @(
        "routing", "--input", $stereo, "--output-channels", "2", "--json"
    )).results[0]
    Assert-True (-not [bool]$stereoResult.source_layout.ambisonic) `
        "Ordinary stereo was interpreted as Ambisonics"
    Assert-True ($stereoResult.source_layout.monitor_decode_runtime_state -eq "not_required") `
        "Ordinary stereo initialized an Ambisonic decode path"
    Assert-True ([int]$stereoResult.physical_output_matrix.active_output_channels -eq 2) `
        "Stereo physical matrix did not adapt to two outputs"

    [pscustomobject]@{
        status = "pass"
        cli = $resolvedCli
        source_order = $multiResult.source_layout.order
        physical_outputs = [int]$multiResult.physical_output_matrix.active_output_channels
        rear_left_output = [int]$rearLeft.physical_output_channel
        silent_role_pings = $multiResult.silent_route_probe.role_pings.Count
        stereo_spatial_runtime = $stereoResult.source_layout.monitor_decode_runtime_state
    } | ConvertTo-Json -Compress
}
finally {
    foreach ($fixture in @($multichannel, $stereo)) {
        if (Test-Path -LiteralPath $fixture) {
            Remove-Item -LiteralPath $fixture -Force
        }
    }
    if ((Test-Path -LiteralPath $testRoot) `
        -and @(Get-ChildItem -LiteralPath $testRoot -Force).Count -eq 0) {
        Remove-Item -LiteralPath $testRoot -Force
    }
}
