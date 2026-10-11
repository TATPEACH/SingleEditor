[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CandidateRoot,
    [Parameter(Mandatory=$true)][string]$VgmRoot,
    [Parameter(Mandatory=$true)][string]$IconsRoot,
    [Parameter(Mandatory=$true)][string]$BuildCachePath,
    [Parameter(Mandatory=$true)][string]$CoreReceiptPath,
    [Parameter(Mandatory=$true)][string]$CrtRoot,
    [Parameter(Mandatory=$true)][string]$GuiIdentityBundlePath,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$releaseRoot=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../release')).Path
Import-Module (Join-Path $releaseRoot 'ReleasePayload.psm1') -Force
$testRoot=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $testRoot) { throw 'Use a new release-payload test directory.' }
[void][IO.Directory]::CreateDirectory($testRoot)
$policyPath=Join-Path $releaseRoot 'payload-policy.json'
$newScript=Join-Path $releaseRoot 'New-ReleasePayload.ps1'
$baseArgs=@{CandidateRoot=$CandidateRoot;VgmRoot=$VgmRoot;IconsRoot=$IconsRoot
    CrtRoot=$CrtRoot;GuiIdentityBundlePath=$GuiIdentityBundlePath
    BuildCachePath=$BuildCachePath;CoreReceiptPath=$CoreReceiptPath;PolicyPath=$policyPath}
$cases=[Collections.Generic.List[string]]::new()
$result=[ordered]@{schema='singleeditor.release-payload-tests';version=1;status='incomplete';cases=$cases;error=$null}
function Expect-PayloadRejection([string]$Name, [scriptblock]$Action, [string]$Message) {
    $rejected=$false
    try { & $Action | Out-Null }
    catch {
        if (-not $_.Exception.Message.Contains($Message)) { throw }
        $rejected=$true
    }
    if (-not $rejected) { throw "Expected rejection: $Name" }
    $cases.Add($Name)
}
function Move-OwnedFixture([string]$From, [string]$To) {
    [void](Get-PayloadRelativePath $testRoot $From)
    [void](Get-PayloadRelativePath $testRoot $To)
    Move-Item -LiteralPath $From -Destination $To -ErrorAction Stop
}
try {
    $first=Join-Path $testRoot 'first'; $second=Join-Path $testRoot 'second'
    & $newScript @baseArgs -OutputDirectory $first | Out-Null
    & $newScript @baseArgs -OutputDirectory $second | Out-Null
    $a=Test-ReleasePayload $first $policyPath; $b=Test-ReleasePayload $second $policyPath
    $policy=Read-PayloadPolicy $policyPath
    if ($a.bom_sha256 -ne $b.bom_sha256 -or $a.entries -ne @($policy.files).Count) { throw 'Repeated payloads were not deterministic and complete.' }
    $cases.Add("actual $($a.entries)-file roundtrip, deterministic BOM and staged CRT loading")
    $result.bom_sha256=$a.bom_sha256
    $runtime=Test-PayloadRuntimeProof $first $policy
    $badIdentity=$runtime.cli | ConvertTo-Json | ConvertFrom-Json
    $badIdentity.configuration='Debug'
    Expect-PayloadRejection 'actual Debug identity refused' { Test-PayloadApplicationIdentity $badIdentity (Join-Path $first 'payload/SingleEditorCLI.exe') } 'Release/x64 with timing disabled'
    $badIdentity.configuration='Release'; $badIdentity.timingInstrumentation=$true
    Expect-PayloadRejection 'actual timing identity refused' { Test-PayloadApplicationIdentity $badIdentity (Join-Path $first 'payload/SingleEditorCLI.exe') } 'Release/x64 with timing disabled'
    $badIdentity.timingInstrumentation=$false; $badIdentity.binarySha256='0'*64
    Expect-PayloadRejection 'unrelated executable identity refused' { Test-PayloadApplicationIdentity $badIdentity (Join-Path $first 'payload/SingleEditorCLI.exe') } 'does not cover the selected executable'
    $badPe=$runtime.pe | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $badPe[0].imports+= 'optional-eager.dll'
    Expect-PayloadRejection 'unclassified dependency refused' { Test-PayloadPeInventory $badPe (Join-Path $first 'payload') $policy } 'Unresolved app-local PE dependency'
    $lowImage=$policy | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    ($lowImage.groups | Where-Object id -eq 'gui').max_image_bytes=1
    Expect-PayloadRejection 'image reservation budget enforced' { Test-PayloadPeInventory $runtime.pe (Join-Path $first 'payload') $lowImage } 'PE image byte budget exceeded'
    $missingCrtRoot=Join-Path $testRoot 'missing-crt-input'
    [void][IO.Directory]::CreateDirectory($missingCrtRoot)
    $arguments=$baseArgs.Clone(); $arguments.CrtRoot=$missingCrtRoot
    Expect-PayloadRejection 'missing CRT source refused before copying' { & $newScript @arguments -OutputDirectory (Join-Path $testRoot 'missing-crt-stage') } 'Required CRT input is missing'
    if (Test-Path -LiteralPath (Join-Path $testRoot 'missing-crt-stage')) { throw 'Missing CRT created a stage.' }
    Expect-PayloadRejection 'existing stage preserved' { & $newScript @baseArgs -OutputDirectory $first } 'already exists'
    if ((Test-ReleasePayload $first $policyPath).bom_sha256 -ne $a.bom_sha256) { throw 'Existing stage changed on refusal.' }

    $receipt=Get-Content -LiteralPath $CoreReceiptPath -Raw | ConvertFrom-Json
    ($receipt.artifacts | Where-Object role -eq 'candidate').sha256='0' * 64
    $wrongReceipt=Join-Path $testRoot 'wrong-gui-receipt.json'
    Write-PayloadJson $wrongReceipt $receipt
    $arguments=$baseArgs.Clone(); $arguments.CoreReceiptPath=$wrongReceipt
    $refused=Join-Path $testRoot 'wrong-gui'
    Expect-PayloadRejection 'unrelated GUI evidence rejected before copying' { & $newScript @arguments -OutputDirectory $refused } 'does not cover the selected GUI'
    if (Test-Path -LiteralPath $refused) { throw 'Invalid evidence created a stage.' }

    $timingCache=Join-Path $testRoot 'timing-on-cache.txt'
    [IO.File]::WriteAllText($timingCache,(Get-Content -LiteralPath $BuildCachePath -Raw).Replace('SINGLE_EDITOR_ENABLE_OPEN_TIMING:BOOL=OFF','SINGLE_EDITOR_ENABLE_OPEN_TIMING:BOOL=ON'))
    $arguments=$baseArgs.Clone(); $arguments.BuildCachePath=$timingCache
    Expect-PayloadRejection 'timing build refused' { & $newScript @arguments -OutputDirectory (Join-Path $testRoot 'timing-on') } 'does not disable open timing'

    $budgetPolicy=Get-Content -LiteralPath $policyPath -Raw | ConvertFrom-Json
    ($budgetPolicy.groups | Where-Object id -eq 'gui').max_bytes=(Get-Item -LiteralPath (Join-Path $CandidateRoot 'Single Editor.exe')).Length - 1
    $lowPolicy=Join-Path $testRoot 'low-budget-policy.json'
    Write-PayloadJson $lowPolicy $budgetPolicy
    $arguments=$baseArgs.Clone(); $arguments.PolicyPath=$lowPolicy
    Expect-PayloadRejection 'byte growth fails before stage creation' { & $newScript @arguments -OutputDirectory (Join-Path $testRoot 'over-budget') } 'Source byte budget exceeded'
    if (Test-Path -LiteralPath (Join-Path $testRoot 'over-budget')) { throw 'Over-budget source created a stage.' }
    Expect-PayloadRejection 'changed selected policy refused' { Test-ReleasePayload $first $lowPolicy } 'Staged policy does not match'

    $extra=Join-Path $second 'payload/unexpected.pdb'
    [IO.File]::WriteAllText($extra,'owned test-only material')
    Expect-PayloadRejection 'extra development file refused without cleanup' { Test-ReleasePayload $second $policyPath } 'file set differs'
    if (-not (Test-Path -LiteralPath $extra)) { throw 'Verifier unexpectedly removed evidence.' }
    Move-OwnedFixture $extra (Join-Path $testRoot 'retained-unexpected.pdb')
    $license=Join-Path $second 'payload/licenses/libsamplerate-0.2.2.txt'
    $heldLicense=Join-Path $testRoot 'retained-license.txt'
    Move-OwnedFixture $license $heldLicense
    Expect-PayloadRejection 'missing license refused' { Test-ReleasePayload $second $policyPath } 'file set differs'
    Move-OwnedFixture $heldLicense $license
    [void](Test-ReleasePayload $second $policyPath)

    $notice=Join-Path $second 'payload/Ambisonics/NOTICE.md'
    $stream=[IO.File]::Open($notice,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try { $firstByte=$stream.ReadByte(); $stream.Position=0; $stream.WriteByte($firstByte -bxor 1) }
    finally { $stream.Dispose() }
    Expect-PayloadRejection 'same-size changed resource refused' { Test-ReleasePayload $second $policyPath } 'Payload bytes or ownership metadata differ'
    if ((Test-ReleasePayload $first $policyPath).bom_sha256 -ne $a.bom_sha256) { throw 'The control stage was changed by fixture tests.' }

    Expect-PayloadRejection 'missing installer compiler does not choose another build' {
        & (Join-Path $releaseRoot 'Build-Installer.ps1') -StageRoot $first -OutputDirectory (Join-Path $testRoot 'installer-no-compiler') -CompilerPath (Join-Path $testRoot 'absent-ISCC.exe')
    } 'ISCC.exe is unavailable'
    if (Test-Path -LiteralPath (Join-Path $testRoot 'installer-no-compiler')) { throw 'Missing compiler created installer output.' }
    $result.status='pass'
} catch { $result.status='failed'; $result.error=$_.Exception.Message; throw }
finally { Write-PayloadJson (Join-Path $testRoot 'receipt.json') $result }
"Release payload checks passed: $($cases.Count) cases; $($result.bom_sha256)"
