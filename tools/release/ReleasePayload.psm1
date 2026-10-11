Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-PayloadHash([string]$Path) {
    # Keep the batch -> Windows PowerShell path independent of caller-specific
    # module auto-loading (PowerShell 7 can pass a different PSModulePath).
    $algorithm=[Security.Cryptography.SHA256]::Create()
    $stream=$null
    try {
        $stream=[IO.File]::OpenRead($Path)
        [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','')
    } finally {
        if ($stream) { $stream.Dispose() }
        $algorithm.Dispose()
    }
}
function Get-PayloadRelativePath([string]$Root, [string]$Path) {
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw "Path is outside payload root: $full" }
    $full.Substring($prefix.Length).Replace('\','/')
}
function Resolve-PayloadChild([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative.Contains('\') -or ($Relative.Split('/') -contains '..')) {
        throw "Expected a relative payload path: $Relative"
    }
    $full = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    [void](Get-PayloadRelativePath $Root $full)
    $full
}
function Get-PayloadNames([string]$Root) {
    $names = [Collections.Generic.List[string]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse) {
        $names.Add((Get-PayloadRelativePath $Root $file.FullName))
    }
    $names.Sort([StringComparer]::Ordinal)
    $names.ToArray()
}
function Write-PayloadJson([string]$Path, $Value) {
    $json = ($Value | ConvertTo-Json -Depth 14 -Compress) + "`n"
    [IO.File]::WriteAllText($Path,$json,[Text.UTF8Encoding]::new($false))
}
function Read-PayloadPolicy([string]$Path) {
    $policy = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($policy.schema -ne 'singleeditor.release-payload-policy' -or $policy.version -notin @(1,2,3,4,5,6)) { throw 'Unsupported payload policy.' }
    $destinations = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $groups = @{}
    foreach ($group in $policy.groups) {
        if ($groups.ContainsKey($group.id) -or $group.max_bytes -le 0) { throw 'Invalid payload budget group.' }
        $groups[$group.id] = $group
    }
    foreach ($file in $policy.files) {
        if (-not $groups.ContainsKey($file.group) -or -not $destinations.Add($file.destination)) { throw 'Duplicate or unclassified payload destination.' }
        if ([IO.Path]::GetExtension($file.destination) -in @('.pdb','.obj','.lib','.ilk','.exp')) { throw 'Development material cannot enter a release payload.' }
        if ($policy.version -ge 6) {
            $kinds=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
            if ($file.physical_kinds.Count -eq 0) { throw 'A payload file requires explicit physical kinds.' }
            foreach ($kind in $file.physical_kinds) {
                if ($kind -cnotin @('executable','shared-library','resource-pack','font','icon','config','schema','license','documentation','helper') -or
                    -not $kinds.Add($kind)) { throw 'Unsupported or duplicate physical kind.' }
            }
        }
    }
    if ($policy.version -ge 3) {
        $excluded = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($item in $policy.source_exclusions) {
            if (-not $item.reason -or $item.path.Contains('\') -or [IO.Path]::IsPathRooted($item.path) -or
                ($item.path.Split('/') -contains '..') -or -not $excluded.Add($item.source+'/'+$item.path) -or
                @($policy.files | Where-Object { $_.source -eq $item.source -and $_.path -eq $item.path }).Count -gt 0) {
                throw 'Invalid retained non-shipping source declaration.'
            }
        }
    }
    $policy
}
function Get-PayloadInventory([string]$Root, $Policy, [string]$PolicyHash) {
    $actual = @(Get-PayloadNames $Root)
    $expected = [Collections.Generic.List[string]]::new()
    $byPath = @{}
    foreach ($file in $Policy.files) { $expected.Add($file.destination); $byPath[$file.destination] = $file }
    $expected.Sort([StringComparer]::Ordinal)
    if (($actual -join "`n") -cne ($expected.ToArray() -join "`n")) { throw 'Payload file set differs from the explicit policy (missing or extra files).' }
    $groups = @{}; $totals = @{}
    foreach ($group in $Policy.groups) { $groups[$group.id]=$group; $totals[$group.id]=[long]0 }
    $entries = @(
        foreach ($name in $actual) {
            $path = Resolve-PayloadChild $Root $name
            $file = Get-Item -LiteralPath $path
            $spec = $byPath[$name]; $group=$groups[$spec.group]
            $totals[$group.id] += $file.Length
            $entry=[pscustomobject][ordered]@{path=$name;bytes=$file.Length;sha256=(Get-PayloadHash $path)
                group=$group.id;owner=$group.owner;kind=$group.kind;declared_loading_policy=$group.declared_loading_policy}
            if ($Policy.version -ge 6) { $entry | Add-Member -NotePropertyName physical_kinds -NotePropertyValue @($spec.physical_kinds) }
            $entry
        }
    )
    $budgetRows = @(
        foreach ($group in $Policy.groups) {
            if ($totals[$group.id] -gt $group.max_bytes) { throw "Payload byte budget exceeded: $($group.id) ($($totals[$group.id]) > $($group.max_bytes))." }
            [pscustomobject][ordered]@{group=$group.id;bytes=$totals[$group.id];max_bytes=$group.max_bytes}
        }
    )
    $gui = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $Root 'Single Editor.exe'))
    $cli = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $Root 'SingleEditorCLI.exe'))
    if ($gui.ProductVersion -notmatch '^\d+\.\d+\.\d+(\.\d+)?$' -or $gui.ProductVersion -ne $cli.ProductVersion) {
        throw 'GUI and CLI must have the same actual numeric product version.'
    }
    [pscustomobject][ordered]@{schema='singleeditor.release-payload-bom';version=$(if ($Policy.version -ge 6) {2} else {1});product_version=$gui.ProductVersion
        policy_sha256=$PolicyHash;entries=$entries;byte_budgets=$budgetRows}
}
function Test-ReleasePayload([string]$StageRoot, [string]$PolicyPath) {
    $stage = (Resolve-Path -LiteralPath $StageRoot).Path
    $policy = Read-PayloadPolicy $PolicyPath
    $policyHash = Get-PayloadHash $PolicyPath
    if ((Get-PayloadHash (Join-Path $stage 'policy.json')) -ne $policyHash) { throw 'Staged policy does not match the selected release policy.' }
    $seal = Get-Content -LiteralPath (Join-Path $stage 'seal.json') -Raw | ConvertFrom-Json
    if ($seal.schema -ne 'singleeditor.release-payload-seal' -or $seal.version -ne 1) { throw 'Unsupported payload seal.' }
    foreach ($name in @('bom.json','provenance.json')) {
        $field = if ($name -eq 'bom.json') {'bom_sha256'} else {'provenance_sha256'}
        if ((Get-PayloadHash (Join-Path $stage $name)) -ne $seal.$field) { throw "Sealed metadata changed: $name" }
    }
    $bom = Get-Content -LiteralPath (Join-Path $stage 'bom.json') -Raw | ConvertFrom-Json
    $actual = Get-PayloadInventory (Join-Path $stage 'payload') $policy $policyHash
    if (($bom | ConvertTo-Json -Depth 14 -Compress) -cne ($actual | ConvertTo-Json -Depth 14 -Compress)) {
        throw 'Payload bytes or ownership metadata differ from the sealed BOM.'
    }
    $provenance = Get-Content -LiteralPath (Join-Path $stage 'provenance.json') -Raw | ConvertFrom-Json
    foreach ($evidence in $provenance.evidence) {
        if ((Get-PayloadHash (Resolve-PayloadChild $stage $evidence.path)) -ne $evidence.sha256) { throw 'Staged build evidence changed.' }
    }
    [void](Read-PayloadBuildProof (Join-Path $stage 'evidence/CMakeCache.txt') (Join-Path $stage 'evidence/core-open.json') (Join-Path $stage 'payload/Single Editor.exe'))
    if ($policy.version -ge 2) {
        [void](Test-PayloadRuntimeProof $stage $policy)
    }
    [pscustomobject]@{status='content-verified';stage=$stage;product_version=$bom.product_version
        entries=$bom.entries.Count;bytes=($bom.entries | Measure-Object bytes -Sum).Sum
        bom_sha256=$seal.bom_sha256;policy_sha256=$policyHash
        runtime_proof= $(if ($policy.version -ge 2) { 'exact GUI/CLI Release identity, static PE imports/image budgets and CLI app-local CRT' } else { 'not established by policy v1' })
        acceptance='Content checks plus the stated runtime proof; resident startup resources, clean-machine, installer behavior and complete release acceptance remain separate.'}
}
function Read-PayloadBuildProof([string]$CachePath, [string]$ReceiptPath, [string]$GuiPath) {
    $cache = Get-Content -LiteralPath $CachePath -Raw
    if ($cache -notmatch '(?m)^SINGLE_EDITOR_ENABLE_OPEN_TIMING:BOOL=OFF\r?$') { throw 'Selected build cache does not disable open timing.' }
    $receipt = Get-Content -LiteralPath $ReceiptPath -Raw | ConvertFrom-Json
    if ($receipt.verdict.status -ne 'pass' -or $receipt.run_contract.profile -ne 'release' -or
        -not $receipt.run_contract.gate_eligible -or -not $receipt.execution.completed -or
        -not $receipt.execution.settings.restored) { throw 'A completed eligible Core Open Release receipt is required.' }
    $candidate = @($receipt.artifacts | Where-Object role -eq 'candidate')
    $summary = @($receipt.summaries | Where-Object artifact_role -eq 'candidate')
    if ($candidate.Count -ne 1 -or $candidate[0].sha256 -ne (Get-PayloadHash $GuiPath) -or
        $summary.Count -ne 1 -or $summary[0].valid_steady_samples -lt 12) { throw 'Core Open evidence does not cover the selected GUI bytes.' }
    [pscustomobject]@{source_head=$receipt.source.head;source_dirty=$receipt.source.dirty
        observed_source_timing='OFF';gui_core_release='pass-for-exact-bytes'
        limit='Recorded source configuration and GUI Core evidence; not overall CLI, resource, installer or release acceptance.'}
}
. (Join-Path $PSScriptRoot 'RuntimeProof.ps1')
Export-ModuleMember -Function Get-PayloadHash,Get-PayloadRelativePath,Resolve-PayloadChild,Get-PayloadNames,Write-PayloadJson,Read-PayloadPolicy,Get-PayloadInventory,Read-PayloadBuildProof,Test-ReleasePayload,New-PayloadRuntimeProof,Test-PayloadRuntimeProof,Read-PayloadGuiIdentity,Test-PayloadApplicationIdentity,Test-PayloadPeInventory
