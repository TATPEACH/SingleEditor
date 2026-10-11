Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$script:PortableFinalRuntimeModulePath=$PSCommandPath
Import-Module (Join-Path $PSScriptRoot 'ReleasePayload.psm1')
. (Join-Path $PSScriptRoot 'PortablePayloadContext.ps1')
. (Join-Path $PSScriptRoot 'RuntimeReceiptChecks.ps1')
. (Join-Path $PSScriptRoot 'RuntimeProof.ps1')

function Assert-Readiness([bool]$Condition,[string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Get-FinalRuntimeFact([string]$Path) {
    $full=Assert-PortableEvidencePath $Path
    $file=Get-Item -LiteralPath $full -Force
    Assert-Readiness (-not $file.PSIsContainer) 'A selected evidence input must be a file.'
    [pscustomobject]@{path=$full;bytes=$file.Length;sha256=(Get-PayloadHash $full)}
}
function Test-FinalRuntimeFact($Fact,[string]$ExpectedPath) {
    Assert-Readiness ($null -ne $Fact) 'Selected evidence fact is missing.'
    $actual=Get-FinalRuntimeFact $ExpectedPath
    Assert-Readiness ([IO.Path]::GetFullPath($Fact.path) -ieq $actual.path -and
        $Fact.bytes -eq $actual.bytes -and $Fact.sha256 -ceq $actual.sha256) 'Selected evidence path, bytes or SHA changed.'
    $actual
}
function Get-FinalRuntimeProducers {
    $result=[ordered]@{}
    foreach ($name in @('PortableFinalRuntime.psm1','PortablePayloadContext.ps1','RuntimeReceiptChecks.ps1',
        'RuntimeProof.ps1','ReleasePayload.psm1','StartupResources.psm1','payload-policy.json',
        '../Invoke-CoreOpenBenchmark.ps1','../Invoke-DiagnosticOpenCycleBenchmark.ps1')) {
        $result[$name]=Get-FinalRuntimeFact (Join-Path $PSScriptRoot $name)
    }
    [pscustomobject]$result
}
function Get-FinalRuntimeBinding($Context) {
    Assert-FixedPortableContextIdentity $Context
    [pscustomobject]@{stage_root=$Context.stage;payload_root=$Context.payload
        bom=(Get-FinalRuntimeFact (Join-Path $Context.stage 'bom.json'))
        seal=(Get-FinalRuntimeFact (Join-Path $Context.stage 'seal.json'))
        provenance=(Get-FinalRuntimeFact (Join-Path $Context.stage 'provenance.json'))
        policy_sha256=$Context.before.policy_sha256
        gui=(Get-FinalRuntimeFact $Context.gui_path);cli=(Get-FinalRuntimeFact $Context.cli_path)}
}
function Assert-FinalRuntimeSame($Expected,$Actual,[string]$Message) {
    # These objects are produced in the same fixed field order on both sides.
    Assert-Readiness (($Expected|ConvertTo-Json -Depth 30 -Compress) -ceq
        ($Actual|ConvertTo-Json -Depth 30 -Compress)) $Message
}
function Read-FinalRuntimeCollection([string]$EvidenceDirectory,$Context) {
    $directory=Assert-PortableEvidenceOutput $EvidenceDirectory $Context.stage
    Assert-Readiness (Test-Path -LiteralPath $directory -PathType Container) 'Selected runtime evidence directory does not exist.'
    $path=Join-Path $directory 'collection.json'
    $fact=Get-FinalRuntimeFact $path
    $record=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    Assert-Readiness ($record.schema -ceq 'singleeditor.fixed-portable-runtime-collection' -and $record.version -eq 1 -and
        $record.scope -ceq 'existing-warm-runtime-subset' -and $record.release_eligible -eq $false -and
        [IO.Path]::GetFullPath($record.stage_root) -ieq $Context.stage -and
        [IO.Path]::GetFullPath($record.payload_root).TrimEnd('\','/') -ieq $Context.payload) 'Unsupported fixed-portable collection identity.'
    $producers=Get-FinalRuntimeProducers
    Assert-FinalRuntimeSame $record.producers $producers 'Runtime producer inputs changed or are incomplete.'
    $binding=Get-FinalRuntimeBinding $Context
    Assert-FinalRuntimeSame $record.before $binding 'Runtime collection did not begin with this exact fixed payload and sealed source.'
    Assert-FinalRuntimeSame $record.after $binding 'Runtime collection did not preserve this exact fixed payload and sealed source.'
    Assert-Readiness ($record.completed -eq $true -and $null -eq $record.error) 'Runtime collection did not complete normally.'
    [void](Test-FinalRuntimeFact $record.inputs.audio $record.inputs.audio.path)
    [void](Test-FinalRuntimeFact $record.inputs.baseline $record.inputs.baseline.path)
    Assert-Readiness ($record.inputs.baseline.sha256 -cne $binding.gui.sha256) 'Collection baseline must be a distinct actual artifact.'
    [pscustomobject]@{directory=$directory;path=$path;fact=$fact;record=$record}
}
function Test-FinalRuntimeResourceModules($Resource,$Context) {
    $snapshot=$Resource.samples[0].startup_resources
    $seen=@{}
    foreach ($row in $snapshot.modules) {
        $path=[IO.Path]::GetFullPath($row.path)
        Assert-Readiness (-not $seen.ContainsKey($path)) 'Resource snapshot repeats a module path.'
        $seen[$path]=$true
        $entry=$Context.by_path[$path]
        if ($null -ne $entry) {
            Assert-Readiness ($entry.group -in @('gui','vc-runtime') -and $row.group -ceq $entry.group -and
                $row.payload_sha256 -ceq $entry.sha256 -and (Get-PayloadHash $path) -ceq $entry.sha256 -and
                $row.resident_pages -gt 0) 'Resource snapshot module does not bind actual required fixed payload bytes.'
        } else {
            Assert-Readiness (-not $path.StartsWith($Context.payload+'\',[StringComparison]::OrdinalIgnoreCase) -and
                $row.group -ceq 'external-to-payload' -and $null -eq $row.payload_sha256) 'Resource snapshot contains an unclassified or mislabelled fixed module.'
            Assert-Readiness (-not $path.StartsWith((Join-Path $Context.stage 'payload')+'\',[StringComparison]::OrdinalIgnoreCase)) 'A staging module cannot establish fixed-portable execution.'
        }
    }
    foreach ($path in $Context.by_path.Keys) {
        if ($Context.by_path[$path].group -in @('gui','vc-runtime')) {
            Assert-Readiness ($seen.ContainsKey($path)) 'Required app-local fixed GUI/CRT module is absent.'
        }
    }
}
function Test-PortableFinalRuntimeEvidence {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$StageRoot,[string]$EvidenceDirectory='')
    $rows=[Collections.Generic.List[object]]::new()
    $capabilityIds=[ordered]@{warm_core='warm-core';post_core_resources='post-core-resident-snapshot'
        diagnostic_repeat='diagnostic-repeat';cli_identity='cli-identity-local-crt'}
    $collection=$null;$identity=$null;$binding=$null;$errorText=$null
    try {
        $context=New-FixedPortablePayloadContext $StageRoot
        $binding=Get-FinalRuntimeBinding $context
        $rows.Add([pscustomobject]@{id='fixed-payload-identity';status='pass';detail='Actual fixed inventory exactly matches the reverified immutable stage.';input=$binding})
        if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
            foreach ($id in @($capabilityIds.Values)+@('gui-identity')) {
                $rows.Add([pscustomobject]@{id=$id;status='pending';detail='No final-path execution evidence selected.'})
            }
        } else {
            $collection=Read-FinalRuntimeCollection $EvidenceDirectory $context
            $record=$collection.record
            $manifestRoot=Join-Path $PSScriptRoot '../../Document/New_JuceEditor/milestone3/performance/core-open-v1'
            $receipts=@{warm_core='core.json';post_core_resources='resource.json';diagnostic_repeat='repeat/receipt.json';cli_identity='cli/cli-identity-run.json'}
            foreach ($name in @('warm_core','post_core_resources','diagnostic_repeat','cli_identity')) {
                $id=$capabilityIds[$name]
                $fact=$record.receipts.$name
                if ($null -eq $fact) {
                    $rows.Add([pscustomobject]@{id=$id;status='pending';detail='No execution evidence selected for this capability.'})
                    if ($name -eq 'diagnostic_repeat') { $rows.Add([pscustomobject]@{id='gui-identity';status='pending';detail='No actual reviewed diagnostic bundle selected.'}) }
                    continue
                }
                $path=Join-Path $collection.directory $receipts[$name]
                try {
                    $verifiedFact=Test-FinalRuntimeFact $fact $path
                    $receipt=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
                    switch ($name) {
                        'warm_core' {
                            [void](Test-ReadinessCoreReceipt $receipt $context.gui_path $binding.gui.sha256 $manifestRoot)
                            $baseline=@($receipt.artifacts|Where-Object role -eq 'baseline')[0]
                            Assert-Readiness ($baseline.sha256 -ceq $record.inputs.baseline.sha256 -and
                                [IO.Path]::GetFullPath($baseline.path) -ieq [IO.Path]::GetFullPath($record.inputs.baseline.path) -and
                                $receipt.corpus.sha256 -ceq $record.inputs.audio.sha256) 'Core run differs from the captured baseline/audio inputs.'
                            Assert-Readiness ($receipt.run_contract.app_cache_state -ceq 'preserved-source-identity' -and
                                $receipt.run_contract.system_cache_state -ceq 'system-warm-hash-validated') 'This adapter only supports the existing declared warm-cache protocol.'
                        }
                        'post_core_resources' {
                            Test-ReadinessResourceReceipt $receipt $context.gui_path $binding.gui.sha256 $context.stage $context.before $context.payload
                            Test-FinalRuntimeResourceModules $receipt $context
                            Assert-Readiness ($receipt.corpus.sha256 -ceq $record.inputs.audio.sha256) 'Resource run uses another audio input.'
                        }
                        'diagnostic_repeat' {
                            [void](Test-ReadinessRepeatOpenReceipt $receipt $path $context.gui_path $binding.gui.sha256 $manifestRoot)
                            $bundle=Join-Path $collection.directory 'repeat/reviewed-snapshot.zip'
                            $identity=Read-PayloadGuiIdentity $bundle
                            Test-PayloadApplicationIdentity $identity $context.gui_path
                            Assert-Readiness ($receipt.audio_sha256 -ceq $record.inputs.audio.sha256) 'Repeat run uses another audio input.'
                            $rows.Add([pscustomobject]@{id='gui-identity';status='pass';detail='Actual fixed-path GUI identity from the verified reviewed export.';input=(Get-FinalRuntimeFact $bundle)})
                        }
                        'cli_identity' {
                            Assert-Readiness ($null -ne $identity) 'CLI identity requires the selected valid fixed GUI identity first.'
                            [void](Test-PayloadCliIdentityEvidence -PayloadRoot $context.payload -EvidenceDirectory (Join-Path $collection.directory 'cli') -Policy $context.policy -GuiIdentity $identity)
                        }
                    }
                    [void](Test-FinalRuntimeFact $verifiedFact $path)
                    $rows.Add([pscustomobject]@{id=$id;status='pass';detail='Revalidated the existing runtime assertions at the actual fixed path.';input=$verifiedFact})
                } catch {
                    $rows.Add([pscustomobject]@{id=$id;status='failed';detail=$_.Exception.Message;input=$fact})
                }
            }
            [void](Test-FinalRuntimeFact $collection.fact $collection.path)
        }
        $after=New-FixedPortablePayloadContext $StageRoot
        Assert-FinalRuntimeSame $binding (Get-FinalRuntimeBinding $after) 'Selected payload or stage changed during read-only evidence verification.'
    } catch {
        $errorText=$_.Exception.Message
        $rows.Add([pscustomobject]@{id='selected-input';status='failed';detail=$errorText})
    }
    foreach ($item in @(
        @('cold-runtime','No validated cold-cache executor/evidence is supplied by this warm-runtime adapter.'),
        @('optional-lifecycle-history','A post-Core module/page snapshot does not prove pre-Core I/O, first touch or optional creation/removal history.'),
        @('full-release-admission','Formal budgets/owner closure, M32 preseal policy, install and final RC remain independently required.'))) {
        $rows.Add([pscustomobject]@{id=$item[0];status='pending';detail=$item[1]})
    }
    [pscustomobject][ordered]@{schema='singleeditor.fixed-portable-runtime-review';version=1
        status=$(if (@($rows|Where-Object status -eq 'failed').Count) {'failed'} else {'pending'})
        scope='existing-warm-runtime-subset';release_eligible=$false;promotion_performed=$false;product_started=$false
        stage_root=[IO.Path]::GetFullPath($StageRoot);payload_root=(Get-FixedPortablePath)
        evidence_directory=$EvidenceDirectory;producer=(Get-FinalRuntimeFact $script:PortableFinalRuntimeModulePath)
        collection=$(if ($collection) {$collection.fact} else {$null});binding=$binding
        results=$rows.ToArray();error=$errorText;checked_utc=[DateTime]::UtcNow.ToString('o')}
}

# Fixed-context engineering actor used by the controlled promotion dispatcher.
# This warm subset neither admits promotion nor grants complete release approval.
function Invoke-FixedPortableRuntimeCollection {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$StageRoot,[Parameter(Mandatory=$true)][string]$EvidenceDirectory,
        [Parameter(Mandatory=$true)][string]$AudioPath,[Parameter(Mandatory=$true)][string]$BaselineExePath)
    $context=New-FixedPortablePayloadContext $StageRoot
    $output=Assert-PortableEvidenceOutput $EvidenceDirectory $context.stage
    foreach ($inputPath in @($AudioPath,$BaselineExePath)) {
        if (Test-PortableEvidencePathOverlap $output $inputPath) { throw 'Evidence output must be separate from runtime inputs.' }
    }
    Assert-Readiness (-not (Test-Path -LiteralPath $output)) 'Runtime evidence requires a fresh directory.'
    $before=Get-FinalRuntimeBinding $context;$producers=Get-FinalRuntimeProducers
    $inputs=[pscustomobject]@{audio=(Get-FinalRuntimeFact $AudioPath);baseline=(Get-FinalRuntimeFact $BaselineExePath)}
    Assert-Readiness ($inputs.baseline.sha256 -cne $before.gui.sha256) 'A distinct baseline artifact is required.'
    [void][IO.Directory]::CreateDirectory($output)
    $receiptFacts=[ordered]@{warm_core=$null;post_core_resources=$null;diagnostic_repeat=$null;cli_identity=$null}
    $failure=$null;$after=$null
    try {
        $shell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
        $core=Join-Path $PSScriptRoot '../Invoke-CoreOpenBenchmark.ps1'
        & $shell -NoProfile -ExecutionPolicy Bypass -File $core -Profile release -ExePath $context.gui_path -BaselineExePath $BaselineExePath -AudioPath $AudioPath -CorpusId ordinary-long-wav-v1 -ReceiptPath (Join-Path $output 'core.json') -MeasurementMode handshake -BaselineContract probe -CandidateRenderer modern -BaselineRenderer modern *> (Join-Path $output 'core.log')
        Assert-Readiness ($LASTEXITCODE -eq 0) 'Fixed-path Release Core did not pass.'
        $receiptFacts.warm_core=Get-FinalRuntimeFact (Join-Path $output 'core.json')
        & $shell -NoProfile -ExecutionPolicy Bypass -File $core -Profile smoke -ExePath $context.gui_path -AudioPath $AudioPath -CorpusId ordinary-long-wav-v1 -ReceiptPath (Join-Path $output 'resource.json') -MeasurementMode handshake -CandidateRenderer modern -StartupResourceStage $context.stage *> (Join-Path $output 'resource.log')
        Assert-Readiness ($LASTEXITCODE -eq 0) 'Fixed-path non-performance resource observation did not pass.'
        $receiptFacts.post_core_resources=Get-FinalRuntimeFact (Join-Path $output 'resource.json')
        & $shell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '../Invoke-DiagnosticOpenCycleBenchmark.ps1') -Profile release -ExePath $context.gui_path -AudioPath $AudioPath -OutputDirectory (Join-Path $output 'repeat') *> (Join-Path $output 'repeat.log')
        Assert-Readiness ($LASTEXITCODE -eq 0) 'Fixed-path diagnostic repeat did not pass.'
        $receiptFacts.diagnostic_repeat=Get-FinalRuntimeFact (Join-Path $output 'repeat/receipt.json')
        $identity=Read-PayloadGuiIdentity (Join-Path $output 'repeat/reviewed-snapshot.zip')
        [void][IO.Directory]::CreateDirectory((Join-Path $output 'cli'))
        [void](Invoke-PayloadCliIdentityEvidence -PayloadRoot $context.payload -EvidenceDirectory (Join-Path $output 'cli') -Policy $context.policy -GuiIdentity $identity)
        $receiptFacts.cli_identity=Get-FinalRuntimeFact (Join-Path $output 'cli/cli-identity-run.json')
    } catch { $failure=$_.Exception.Message }
    try {
        $after=Get-FinalRuntimeBinding (New-FixedPortablePayloadContext $StageRoot)
        Assert-FinalRuntimeSame $before $after 'Payload or stage changed during actual fixed-path runtime collection.'
        Assert-FinalRuntimeSame $producers (Get-FinalRuntimeProducers) 'Runtime producer changed during collection.'
        [void](Test-FinalRuntimeFact $inputs.audio $AudioPath)
        [void](Test-FinalRuntimeFact $inputs.baseline $BaselineExePath)
    } catch { $failure=($_.Exception.Message+' '+$failure).Trim() }
    $record=[pscustomobject][ordered]@{schema='singleeditor.fixed-portable-runtime-collection';version=1
        scope='existing-warm-runtime-subset';release_eligible=$false;stage_root=$context.stage;payload_root=$context.payload
        before=$before;after=$after;producers=$producers;inputs=$inputs;receipts=[pscustomobject]$receiptFacts
        completed=($null -eq $failure);error=$failure;completed_utc=[DateTime]::UtcNow.ToString('o')}
    $stream=[IO.File]::Open((Join-Path $output 'collection.json'),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($record|ConvertTo-Json -Depth 30)+"`n");$stream.Write($bytes,0,$bytes.Length) }
    finally {$stream.Dispose()}
    if ($failure) { throw $failure }
    Test-PortableFinalRuntimeEvidence -StageRoot $StageRoot -EvidenceDirectory $output
}
Export-ModuleMember -Function Test-PortableFinalRuntimeEvidence,Invoke-FixedPortableRuntimeCollection
