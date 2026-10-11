[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$StageRoot,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$CompilerPath='',
    [string]$PolicyPath=''
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'ReleasePayload.psm1') -Force
if ([string]::IsNullOrWhiteSpace($PolicyPath)) { $PolicyPath=Join-Path $PSScriptRoot 'payload-policy.json' }
$verified=Test-ReleasePayload $StageRoot $PolicyPath
$stage=$verified.stage
$output=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Installer output already exists. Select a new explicit directory.' }
$prefix=$stage.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if ($output.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Installer output must be outside the sealed stage.' }
if ([string]::IsNullOrWhiteSpace($CompilerPath)) {
    $installed=Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($installed) { $CompilerPath=$installed.Source }
    else {
        foreach ($candidate in @('C:/Program Files (x86)/Inno Setup 6/ISCC.exe','C:/Program Files/Inno Setup 6/ISCC.exe')) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $CompilerPath=$candidate; break }
        }
    }
}
if ([string]::IsNullOrWhiteSpace($CompilerPath) -or -not (Test-Path -LiteralPath $CompilerPath -PathType Leaf)) {
    throw 'ISCC.exe is unavailable. Pass an installed compiler with -CompilerPath; no build or payload fallback is used.'
}
$compiler=(Resolve-Path -LiteralPath $CompilerPath).Path
$iss=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../installer/SingleEditor.iss')).Path
$payload=Join-Path $stage 'payload'
$receipt=[ordered]@{schema='singleeditor.installer-build';version=1;status='incomplete';product_version=$verified.product_version
    stage=$stage;payload_bom_sha256=$verified.bom_sha256;policy_sha256=$verified.policy_sha256
    compiler=$compiler;compiler_sha256=(Get-PayloadHash $compiler);installer_source_sha256=(Get-PayloadHash $iss)
    output_sha256=$null;source_unchanged=$false;error=$null
    acceptance='Installer compilation only; installation, associations, runtime and release acceptance remain separate.'}
[void][IO.Directory]::CreateDirectory($output)
try {
    & $compiler "/DVerifiedPayloadRoot=$payload" "/DReleaseVersion=$($verified.product_version)" "/DInstallerOutputRoot=$output" $iss *> (Join-Path $output 'compiler.log')
    if ($LASTEXITCODE -ne 0) { throw "Installer compiler failed with exit $LASTEXITCODE." }
    $after=Test-ReleasePayload $stage $PolicyPath
    if ($after.bom_sha256 -ne $verified.bom_sha256) { throw 'Installer source payload changed during compilation.' }
    $receipt.source_unchanged=$true
    $receipt.output_sha256=Get-PayloadHash (Join-Path $output 'SingleEditor_Installer.exe')
    $receipt.status='compiled'
} catch { $receipt.error=$_.Exception.Message; $receipt.status='failed'; throw }
finally { Write-PayloadJson (Join-Path $output 'receipt.json') $receipt }
$receipt | ConvertTo-Json -Depth 5 -Compress
