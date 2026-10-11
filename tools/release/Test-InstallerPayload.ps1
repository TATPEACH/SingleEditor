[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$StageRoot,
    [Parameter(Mandatory=$true)][string]$InstallerDirectory,
    [Parameter(Mandatory=$true)][string]$ExtractorPath,
    [Parameter(Mandatory=$true)][string]$GuiBundlePath,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$ExtractorSha256='1C8453C198DD3FE7947F3705E59D2294DDCE3DFA9F96A737E94449B119F44181'
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'ReleasePayload.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'InstallerContract.psm1') -Force
$stage=(Resolve-Path -LiteralPath $StageRoot).Path
$installerRoot=(Resolve-Path -LiteralPath $InstallerDirectory).Path
$extractor=(Resolve-Path -LiteralPath $ExtractorPath).Path
$installer=Join-Path $installerRoot 'SingleEditor_Installer.exe'
$output=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Select a new extraction/evidence directory; previous results are preserved.' }
foreach ($source in @($stage,$installerRoot,[IO.Path]::GetDirectoryName($extractor))) {
    $prefix=$source.TrimEnd('\','/')+'\'
    if (($output+'\').StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or
        $prefix.StartsWith($output+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Extraction output must be separate from source artifacts and the tool.' }
}
if ((Get-PayloadHash $extractor) -ne $ExtractorSha256) { throw 'Extractor does not match the selected tool identity.' }
$policyPath=Join-Path $stage 'policy.json'
$verified=Test-ReleasePayload $stage $policyPath
$build=Get-Content -LiteralPath (Join-Path $installerRoot 'receipt.json') -Raw | ConvertFrom-Json
$installerHash=Get-PayloadHash $installer
if ($build.schema -ne 'singleeditor.installer-build' -or $build.status -ne 'compiled' -or -not $build.source_unchanged -or
    $build.payload_bom_sha256 -ne $verified.bom_sha256 -or $build.policy_sha256 -ne $verified.policy_sha256 -or
    $build.output_sha256 -ne $installerHash) { throw 'Installer build receipt does not bind this exact stage and executable.' }
$receipt=[ordered]@{schema='singleeditor.installer-content';version=1;status='incomplete';installer_sha256=$installerHash
    extractor_path=$extractor;extractor_sha256=$ExtractorSha256;payload_bom_sha256=$verified.bom_sha256
    payload_equal=$false;compiled_contract=$null;cli_smoke=$null;extraction_artifacts=@();error=$null
    scope='Actual installer container extraction and compiled metadata only. No setup execution, registry changes or installed/uninstall acceptance.'}
[void][IO.Directory]::CreateDirectory($output)
try {
    $extracted=Join-Path $output 'extracted'
    & $extractor -x -m -b -o -n "-d$extracted" $installer *> (Join-Path $output 'extractor.log')
    if ($LASTEXITCODE -ne 0) { throw "Installer extraction failed: $LASTEXITCODE" }
    $app=Join-Path $extracted '{app}'
    $actual=Get-PayloadInventory $app (Read-PayloadPolicy $policyPath) (Get-PayloadHash $policyPath)
    $expected=Get-Content -LiteralPath (Join-Path $stage 'bom.json') -Raw | ConvertFrom-Json
    if (($actual | ConvertTo-Json -Depth 14 -Compress) -cne ($expected | ConvertTo-Json -Depth 14 -Compress)) {
        throw 'Actual installer payload differs from the sealed BOM.'
    }
    $receipt.payload_equal=$true
    $receipt.payload_entries=$actual.entries.Count
    $receipt.payload_bytes=($actual.entries | Measure-Object bytes -Sum).Sum
    # These are unpacker outputs, not extra user payload files. The language stub
    # and reconstructed script are explicitly distinguished from embedded images.
    $expectedExtra=@{'install_script.iss'='extractor-reconstructed-script';'embedded/english.isl'='extractor-language-stub'
        'embedded/WizardImage0.png'='installer-embedded-image';'embedded/WizardSmallImage0.png'='installer-embedded-image'}
    $extra=@(Get-PayloadNames $extracted | Where-Object { -not $_.StartsWith('{app}/',[StringComparison]::Ordinal) })
    if ($extra.Count -ne $expectedExtra.Count) { throw 'Unclassified installer extraction artifact.' }
    $receipt.extraction_artifacts=@(foreach ($name in $extra) {
        if (-not $expectedExtra.ContainsKey($name)) { throw "Unclassified installer extraction artifact: $name" }
        $path=Resolve-PayloadChild $extracted $name
        [pscustomobject]@{path=$name;kind=$expectedExtra[$name];bytes=(Get-Item -LiteralPath $path).Length;sha256=(Get-PayloadHash $path)}
    })
    $receipt.compiled_contract=Test-InstallerAssociationContract (Join-Path $extracted 'install_script.iss')
    if ($receipt.compiled_contract.setup.AppVersion -ne $verified.product_version) {
        throw 'Compiled installer product version differs from the actual GUI/CLI payload.'
    }
    $events=& (Join-Path $app 'SingleEditorCLI.exe') diagnostics validate --bundle ([IO.Path]::GetFullPath($GuiBundlePath)) --jsonl
    $cliExit=$LASTEXITCODE
    [IO.File]::WriteAllLines((Join-Path $output 'cli.jsonl'),[string[]]$events,[Text.UTF8Encoding]::new($false))
    if ($cliExit -ne 0) { throw 'Extracted CLI did not validate the actual GUI bundle.' }
    $terminal=$events[-1] | ConvertFrom-Json
    if (-not $terminal.ok -or $terminal.payload.write_performed) { throw 'Unexpected extracted CLI validation result.' }
    $receipt.cli_smoke=@{exit_code=$cliExit;verified_entries=$terminal.payload.verified_entries;write_performed=$false}
    if ((Get-PayloadHash $installer) -ne $installerHash -or (Get-PayloadHash $extractor) -ne $ExtractorSha256) { throw 'Installer or extractor changed during verification.' }
    [void](Test-ReleasePayload $stage $policyPath)
    $receipt.status='pass'
} catch { $receipt.status='failed'; $receipt.error=$_.Exception.Message; throw }
finally { Write-PayloadJson (Join-Path $output 'receipt.json') $receipt }
[pscustomobject]@{status=$receipt.status;entries=$receipt.payload_entries;bytes=$receipt.payload_bytes
    installer_sha256=$installerHash;bom_sha256=$verified.bom_sha256;compiled_association_entries=$receipt.compiled_contract.association_entries
    acceptance=$receipt.scope} | ConvertTo-Json -Compress
