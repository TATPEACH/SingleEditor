[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CandidateRoot,
    [Parameter(Mandatory=$true)][string]$VgmRoot,
    [Parameter(Mandatory=$true)][string]$IconsRoot,
    [Parameter(Mandatory=$true)][string]$BuildCachePath,
    [Parameter(Mandatory=$true)][string]$CoreReceiptPath,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$CrtRoot = '',
    [string]$GuiIdentityBundlePath = '',
    [string]$PolicyPath = ''
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if ([string]::IsNullOrWhiteSpace($PolicyPath)) { $PolicyPath=Join-Path $PSScriptRoot 'payload-policy.json' }
Import-Module (Join-Path $PSScriptRoot 'ReleasePayload.psm1') -Force
$policy = Read-PayloadPolicy $PolicyPath
$policyHash = Get-PayloadHash $PolicyPath
$stage = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $stage) { throw 'Staging destination already exists. Select a new explicit directory; no payload is overwritten.' }
$roots=@{candidate=(Resolve-Path -LiteralPath $CandidateRoot).Path
    vgmstream=(Resolve-Path -LiteralPath $VgmRoot).Path;icons=(Resolve-Path -LiteralPath $IconsRoot).Path}
if ($policy.version -ge 2) {
    if (-not $CrtRoot -or -not $GuiIdentityBundlePath) { throw 'Policy v2 requires explicit CrtRoot and GuiIdentityBundlePath.' }
    $roots.crt=(Resolve-Path -LiteralPath $CrtRoot).Path
    $roots.notices=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'notices')).Path
}
$stagePrefix=$stage.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
foreach ($key in $roots.Keys) {
    $rootPrefix=$roots[$key].TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if ($stagePrefix.StartsWith($rootPrefix,[StringComparison]::OrdinalIgnoreCase) -or
        $rootPrefix.StartsWith($stagePrefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Staging and source roots must be separate.' }
    $expected=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($spec in $policy.files | Where-Object source -eq $key) { [void]$expected.Add($spec.path) }
    # The official redist directory has additional, unused libraries. Only the
    # four explicitly selected imports are copied; unrelated redist files stay put.
    if ($key -eq 'crt') {
        foreach ($path in $expected) {
            if (-not (Test-Path -LiteralPath (Resolve-PayloadChild $roots[$key] $path) -PathType Leaf)) {
                throw "Required CRT input is missing: $path"
            }
        }
        continue
    }
    $actual=@(Get-PayloadNames $roots[$key])
    if ($policy.version -ge 3) {
        $excluded=@($policy.source_exclusions | Where-Object source -eq $key | ForEach-Object path)
        $actual=@($actual | Where-Object { $_ -cnotin $excluded })
    }
    if ($actual.Count -ne $expected.Count) { throw "Source inventory differs from the explicit policy: $key" }
    foreach ($path in $actual) { if (-not $expected.Contains($path)) { throw "Unclassified source input: $key/$path" } }
}
$proof = Read-PayloadBuildProof $BuildCachePath $CoreReceiptPath (Join-Path $roots.candidate 'Single Editor.exe')
$totals=@{}; foreach ($group in $policy.groups) { $totals[$group.id]=[long]0 }
$inputs=@(
    foreach ($spec in $policy.files) {
        if (-not $roots.ContainsKey($spec.source)) { throw 'Unknown source binding in payload policy.' }
        $path = Resolve-PayloadChild $roots[$spec.source] $spec.path
        $file = Get-Item -LiteralPath $path
        $totals[$spec.group] += $file.Length
        [pscustomobject]@{source=$path;destination=$spec.destination;bytes=$file.Length;sha256=(Get-PayloadHash $path)}
    }
)
foreach ($group in $policy.groups) {
    if ($totals[$group.id] -gt $group.max_bytes) { throw "Source byte budget exceeded: $($group.id). No stage was created." }
}
$evidence=@(
    [pscustomobject]@{path='evidence/CMakeCache.txt';source=(Resolve-Path -LiteralPath $BuildCachePath).Path;sha256=(Get-PayloadHash $BuildCachePath)},
    [pscustomobject]@{path='evidence/core-open.json';source=(Resolve-Path -LiteralPath $CoreReceiptPath).Path;sha256=(Get-PayloadHash $CoreReceiptPath)}
)
if ($policy.version -ge 2) {
    $evidence+= [pscustomobject]@{path='evidence/gui-identity.zip';source=(Resolve-Path -LiteralPath $GuiIdentityBundlePath).Path;sha256=(Get-PayloadHash $GuiIdentityBundlePath)}
    $cacheText=Get-Content -LiteralPath $BuildCachePath -Raw
    $linkerMatch=[regex]::Match($cacheText,'(?m)^CMAKE_LINKER:FILEPATH=(.+)\r?$')
    if (-not $linkerMatch.Success) { throw 'Selected cache does not identify a PE inspection tool.' }
    $linkerPath=$linkerMatch.Groups[1].Value.Trim()
    if (-not (Test-Path -LiteralPath $linkerPath -PathType Leaf)) { throw 'Selected PE inspection tool is unavailable.' }
}
# From here onward any failure leaves an incomplete or unverifiable directory for
# inspection. Never delete or reuse another stage, candidate or distribution.
[void][IO.Directory]::CreateDirectory((Join-Path $stage 'payload'))
foreach ($sourceItem in $inputs) {
    $target=Resolve-PayloadChild (Join-Path $stage 'payload') $sourceItem.destination
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
    [IO.File]::Copy($sourceItem.source,$target,$false)
    if ((Get-PayloadHash $target) -ne $sourceItem.sha256 -or (Get-Item -LiteralPath $target).Length -ne $sourceItem.bytes) { throw 'Source changed during payload copy.' }
}
foreach ($sourceItem in $inputs) {
    if ((Get-PayloadHash $sourceItem.source) -ne $sourceItem.sha256) { throw 'Source changed before the stage was sealed.' }
}
$bom=Get-PayloadInventory (Join-Path $stage 'payload') $policy $policyHash
foreach ($item in $evidence) {
    $target=Resolve-PayloadChild $stage $item.path
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
    [IO.File]::Copy($item.source,$target,$false)
    if ((Get-PayloadHash $target) -ne $item.sha256) { throw 'Build evidence changed during staging.' }
}
if ($policy.version -ge 2) {
    New-PayloadRuntimeProof $stage $policy (Join-Path $stage 'evidence/gui-identity.zip') $linkerPath
    foreach ($name in @('runtime-proof.json','cli-identity.jsonl','cli-identity-run.json')) {
        $path=Join-Path $stage "evidence/$name"
        $evidence+= [pscustomobject]@{path="evidence/$name";source='observed during this stage';sha256=(Get-PayloadHash $path)}
    }
}
[IO.File]::Copy((Resolve-Path -LiteralPath $PolicyPath).Path,(Join-Path $stage 'policy.json'),$false)
Write-PayloadJson (Join-Path $stage 'bom.json') $bom
Write-PayloadJson (Join-Path $stage 'provenance.json') ([ordered]@{
    schema='singleeditor.release-payload-provenance';version=1;proof=$proof;source_roots=$roots
    evidence=$evidence;source_inputs=$inputs
    disposition='staging only; no fixed portable promotion, installer execution or publication'
})
Write-PayloadJson (Join-Path $stage 'seal.json') ([ordered]@{
    schema='singleeditor.release-payload-seal';version=1
    bom_sha256=(Get-PayloadHash (Join-Path $stage 'bom.json'))
    provenance_sha256=(Get-PayloadHash (Join-Path $stage 'provenance.json'))
})
Test-ReleasePayload $stage $PolicyPath | ConvertTo-Json -Depth 5 -Compress
