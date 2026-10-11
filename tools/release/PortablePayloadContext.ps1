# Internal fixed-destination context. Dot-sourced by the two release-only consumers.
# No caller-selected portable root, filesystem mutation or product startup.
function Get-FixedPortablePath {
    [IO.Path]::GetFullPath('G:\Codex\SingleEditor\portable_dist\SingleEditor').TrimEnd('\','/')
}
function Assert-PortableEvidencePath([string]$Path) {
    $full=[IO.Path]::GetFullPath($Path).TrimEnd('\','/')
    if ($full -notmatch '^[A-Za-z]:\\' -or $full.IndexOf(':',2) -ge 0 -or $full -match '[. ](\\|$)') {
        throw 'Evidence requires an ordinary local drive path.'
    }
    $cursor=$full
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Release evidence paths must not traverse reparse points.'
        }
        $cursor=[IO.Path]::GetDirectoryName($cursor)
    }
    $full
}
function Test-PortableEvidencePathOverlap([string]$Left,[string]$Right) {
    $a=[IO.Path]::GetFullPath($Left).TrimEnd('\','/')+'\'
    $b=[IO.Path]::GetFullPath($Right).TrimEnd('\','/')+'\'
    $a.StartsWith($b,[StringComparison]::OrdinalIgnoreCase) -or
        $b.StartsWith($a,[StringComparison]::OrdinalIgnoreCase)
}
function Assert-PortableEvidenceOutput([string]$Path,[string]$StageRoot) {
    $output=Assert-PortableEvidencePath $Path
    if ((Test-PortableEvidencePathOverlap $output (Get-FixedPortablePath)) -or
        (Test-PortableEvidencePathOverlap $output $StageRoot)) {
        throw 'Evidence output must be outside the sealed stage and fixed portable, including their ancestors.'
    }
    $output
}
function New-FixedPortablePayloadContext([string]$StageRoot) {
    $stage=Assert-PortableEvidencePath $StageRoot
    $payload=Assert-PortableEvidencePath (Get-FixedPortablePath)
    if (Test-PortableEvidencePathOverlap $stage $payload) { throw 'The immutable stage must be separate from the fixed portable.' }
    foreach ($item in Get-ChildItem -LiteralPath $payload -Force -Recurse) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The fixed portable must not contain reparse entries.' }
    }
    $policyPath=Join-Path $PSScriptRoot 'payload-policy.json'
    $before=Test-ReleasePayload $stage $policyPath
    $policy=Read-PayloadPolicy $policyPath
    $bom=Get-Content -LiteralPath (Join-Path $stage 'bom.json') -Raw | ConvertFrom-Json
    $actual=Get-PayloadInventory $payload $policy $before.policy_sha256
    if (($actual | ConvertTo-Json -Depth 14 -Compress) -cne ($bom | ConvertTo-Json -Depth 14 -Compress)) {
        throw 'The actual fixed portable differs from the selected sealed BOM.'
    }
    $byPath=@{}
    foreach ($entry in $bom.entries) { $byPath[(Join-Path $payload $entry.path)]=$entry }
    [pscustomobject]@{mode='fixed-portable';stage=$stage;payload=$payload;payload_root=$payload
        gui_path=(Join-Path $payload 'Single Editor.exe');cli_path=(Join-Path $payload 'SingleEditorCLI.exe')
        policy_path=$policyPath;policy=$policy;bom=$bom;before=$before;by_path=$byPath}
}
function Assert-FixedPortableContextIdentity($Context) {
    $fixed=Get-FixedPortablePath
    if ($Context.mode -cne 'fixed-portable' -or
        [IO.Path]::GetFullPath($Context.payload).TrimEnd('\','/') -ine $fixed -or
        [IO.Path]::GetFullPath($Context.payload_root).TrimEnd('\','/') -ine $fixed -or
        [IO.Path]::GetFullPath($Context.gui_path) -ine (Join-Path $fixed 'Single Editor.exe') -or
        [IO.Path]::GetFullPath($Context.cli_path) -ine (Join-Path $fixed 'SingleEditorCLI.exe')) {
        throw 'Unsupported fixed-portable context identity.'
    }
}
