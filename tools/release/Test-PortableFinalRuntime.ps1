[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$StageRoot,[string]$EvidenceDirectory='',
    [Parameter(Mandatory=$true)][string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'PortablePayloadContext.ps1')
$output=Assert-PortableEvidenceOutput $OutputDirectory $StageRoot
if ($EvidenceDirectory -and (Test-PortableEvidencePathOverlap $output $EvidenceDirectory)) {
    throw 'Review output must be separate from selected runtime evidence.'
}
if (Test-Path -LiteralPath $output) { throw 'Select a fresh review output directory.' }
Import-Module (Join-Path $PSScriptRoot 'PortableFinalRuntime.psm1')
$result=Test-PortableFinalRuntimeEvidence -StageRoot $StageRoot -EvidenceDirectory $EvidenceDirectory
if ($result.status -notin @('failed','pending') -or $result.release_eligible -ne $false) {
    throw 'A partial runtime adapter cannot authorize release.'
}
if (Test-Path -LiteralPath $output) { throw 'Output appeared during review; existing evidence is preserved.' }
[void][IO.Directory]::CreateDirectory($output)
$path=Join-Path $output 'receipt.json'
$stream=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
try { $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($result|ConvertTo-Json -Depth 40)+"`n");$stream.Write($bytes,0,$bytes.Length) }
finally { $stream.Dispose() }
Write-Output ('PORTABLE_FINAL_RUNTIME='+$result.status.ToUpperInvariant()+'; RECEIPT='+$path)
if ($result.status -eq 'failed') { exit 1 }
exit 3
