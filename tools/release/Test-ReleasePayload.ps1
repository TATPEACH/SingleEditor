[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$StageRoot,
    [string]$PolicyPath = ''
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if ([string]::IsNullOrWhiteSpace($PolicyPath)) { $PolicyPath=Join-Path $PSScriptRoot 'payload-policy.json' }
Import-Module (Join-Path $PSScriptRoot 'ReleasePayload.psm1') -Force
Test-ReleasePayload $StageRoot $PolicyPath | ConvertTo-Json -Depth 5 -Compress
