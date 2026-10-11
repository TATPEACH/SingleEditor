[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('window-shell','ambisonics')][string]$Owner='window-shell'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$build=(Resolve-Path -LiteralPath $BuildDirectory).Path
$output=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new pack verification output.' }
$intent=Get-Content -LiteralPath (Join-Path $build 'resource-intent/Release.json') -Raw | ConvertFrom-Json
$tray=@($intent.resources | Where-Object { $_.storage -eq 'owner-lazy-pack' -and $_.owner -eq "singleeditor.$Owner" })
if ($tray.Count -ne 1) { throw 'Expected one declared tray owner pack.' }
$tray=$tray[0]
$repo=Split-Path $intent.source_root
$tool=Join-Path $intent.source_root 'cmake/BuildWindowShellTrayPack.mjs'
$cache=Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
$match=[regex]::Match($cache,'(?m)^SINGLE_EDITOR_NODE_EXECUTABLE:FILEPATH=(.+)\r?$')
if (-not $match.Success) { throw 'Configured pack builder runtime is missing.' }
$node=$match.Groups[1].Value.Trim()
Import-Module (Join-Path $repo 'tools/release/ReleasePayload.psm1') -Force
[void][IO.Directory]::CreateDirectory($output)
$repeat=Join-Path $output 'repeat.sepack'
& $node $tool (Join-Path $build "owner-packs/$Owner/content/manifest.json") $tray.source $repeat $tray.entry_name
if ($LASTEXITCODE -ne 0) { throw 'Pack rebuild failed.' }
if ((Get-PayloadHash $repeat) -ne $tray.pack_sha256 -or (Get-PayloadHash $tray.pack_source) -ne $tray.pack_sha256) {
    throw 'Pack rebuild is not byte-identical.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::OpenRead($repeat)
$rows=@()
try {
    if (($zip.Entries.FullName -join ',') -cne "manifest.json,$($tray.entry_name)") { throw 'Unexpected ZIP entry set/order.' }
    foreach ($entry in $zip.Entries) {
        if ($entry.Length -gt 524288 -or $entry.CompressedLength -ne $entry.Length -or
            $entry.LastWriteTime.Year -ne 1980 -or $entry.LastWriteTime.Month -ne 1 -or $entry.LastWriteTime.Day -ne 1) {
            throw 'Unexpected ZIP size, compression or timestamp.'
        }
        $stream=$entry.Open(); $memory=[IO.MemoryStream]::new(); $sha=[Security.Cryptography.SHA256]::Create()
        try {
            $stream.CopyTo($memory)
            $bytes=$memory.ToArray()
            $digest=[BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','')
            $expected=if ($entry.FullName -eq 'manifest.json') { $tray.manifest_sha256 } else { Get-PayloadHash $tray.source }
            if ($digest -ne $expected) { throw 'Independent ZIP entry hash mismatch.' }
            $rows+= [ordered]@{name=$entry.FullName;bytes=$entry.Length;compressed_bytes=$entry.CompressedLength;sha256=$digest}
        } finally { $sha.Dispose(); $memory.Dispose(); $stream.Dispose() }
    }
} finally { $zip.Dispose() }
$policyPath=Join-Path $repo 'tools/release/payload-policy.json'
$policy=Read-PayloadPolicy $policyPath
if (@($policy.files | Where-Object destination -eq $tray.pack_path).Count -ne 1) {
    throw 'Shipping policy must contain the declared owner pack exactly once.'
}
if ($Owner -eq 'window-shell' -and (@($policy.files | Where-Object destination -eq 'icons/icon.png').Count -ne 0 -or
    @($policy.source_exclusions | Where-Object { $_.source -eq 'icons' -and $_.path -eq 'icon.png' }).Count -ne 1)) {
    throw 'Shipping policy must replace the old loose tray duplicate with the declared owner pack.'
}
if ($Owner -eq 'ambisonics') {
    $manifest=Get-Content -LiteralPath (Join-Path $build 'owner-packs/ambisonics/content/manifest.json') -Raw | ConvertFrom-Json
    if (@($policy.files | Where-Object destination -eq 'Ambisonics/SADIE_006_DFC_ACN_SN3D_1OA_3OA_48k.sehrir').Count -ne 0 -or
        @($policy.files | Where-Object { $_.destination -eq 'Ambisonics/NOTICE.md' -and $_.group -eq 'ambisonic-notice' }).Count -ne 1 -or
        $manifest.notice_sha256 -ne (Get-PayloadHash (Join-Path $intent.source_root 'Resources/Ambisonics/NOTICE.md'))) {
        throw 'Ambisonics must replace its loose data and retain the matching independent notice.'
    }
}
$receipt=[ordered]@{status='pass';owner=$tray.owner;pack_sha256=(Get-PayloadHash $repeat);entries=$rows
    deterministic_repeat=$true;reader='System.IO.Compression.ZipArchive';shipping_policy_sha256=(Get-PayloadHash $policyPath)
    node=[ordered]@{path=$node;sha256=(Get-PayloadHash $node);version=(& $node --version)}
    builder=[ordered]@{path=$tool;sha256=(Get-PayloadHash $tool)}
    limit='Pack bytes, standard ZIP decoding and shipping policy; runtime lifecycle is verified separately.'}
Write-PayloadJson (Join-Path $output 'receipt.json') $receipt
Write-Output "pack-verification=pass entries=$($rows.Count) sha256=$($receipt.pack_sha256)"
