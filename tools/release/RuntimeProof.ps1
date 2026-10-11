# Dot-sourced by ReleasePayload.psm1. These checks run only during explicit packaging.
function Test-PayloadApplicationIdentity($Identity, [string]$Executable) {
    if ($Identity.configuration -cne 'Release' -or $Identity.timingInstrumentation -isnot [bool] -or
        $Identity.timingInstrumentation -or $Identity.processArchitecture -cne 'x64') {
        throw 'Actual application identity must be Release/x64 with timing disabled.'
    }
    if ($Identity.binarySha256 -ne (Get-PayloadHash $Executable) -or
        $Identity.version -ne [Diagnostics.FileVersionInfo]::GetVersionInfo($Executable).ProductVersion -or
        [string]::IsNullOrWhiteSpace($Identity.commit)) { throw 'Application identity does not cover the selected executable.' }
}
function Read-PayloadGuiIdentity([string]$BundlePath) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip=[IO.Compression.ZipFile]::OpenRead($BundlePath)
    try {
        $entry=$zip.GetEntry('manifest.json')
        if (-not $entry -or $entry.Length -gt 65536) { throw 'Expected a bounded GUI diagnostic manifest.' }
        $reader=[IO.StreamReader]::new($entry.Open())
        try { $manifest=$reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if ($manifest.format -ne 'single-editor-diagnostics' -or $manifest.coreSchemaMajor -ne 1 -or $manifest.origin -ne 'gui') {
            throw 'Expected a real GUI diagnostic bundle.'
        }
        $manifest.identity
    } finally { $zip.Dispose() }
}
function Get-PayloadPeInventory([string]$Root, $Policy, [string]$LinkerPath) {
    $rows=@(foreach ($spec in $Policy.files | Where-Object { [IO.Path]::GetExtension($_.destination) -in @('.exe','.dll') }) {
        $path=Resolve-PayloadChild $Root $spec.destination
        $headers=(& $LinkerPath /dump /headers $path 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "PE header inspection failed: $($spec.destination)" }
        $machine=[regex]::Match($headers,'(?m)^\s+[0-9A-F]+ machine \((x64|x86)\)\s*$')
        $size=[regex]::Match($headers,'(?m)^\s+([0-9A-F]+) size of image\s*$')
        if (-not $machine.Success -or -not $size.Success) { throw 'Unrecognized PE header output.' }
        $dependencies=(& $LinkerPath /dump /dependents $path 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "PE import inspection failed: $($spec.destination)" }
        $imports=@([regex]::Matches($dependencies,'(?m)^\s+([a-zA-Z0-9_.-]+\.dll)\s*$') |
            ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
        [pscustomobject][ordered]@{path=$spec.destination;sha256=(Get-PayloadHash $path);group=$spec.group
            machine=$machine.Groups[1].Value;image_bytes=[Convert]::ToInt64($size.Groups[1].Value,16)
            file_version=[Diagnostics.FileVersionInfo]::GetVersionInfo($path).FileVersion;imports=$imports}
    })
    Test-PayloadPeInventory $rows $Root $Policy
    $rows
}
function Test-PayloadPeInventory($Rows, [string]$Root, $Policy) {
    $expected=@($Policy.files | Where-Object { [IO.Path]::GetExtension($_.destination) -in @('.exe','.dll') })
    if ($Rows.Count -ne $expected.Count) { throw 'PE inventory is incomplete.' }
    $byPath=@{}; $totals=@{}
    foreach ($row in $Rows) {
        if ($byPath.ContainsKey($row.path)) { throw 'Duplicate PE inventory entry.' }
        $byPath[$row.path]=$row
    }
    foreach ($spec in $expected) {
        if (-not $byPath.ContainsKey($spec.destination)) { throw 'PE inventory is incomplete.' }
        $row=$byPath[$spec.destination]
        $group=@($Policy.groups | Where-Object id -eq $spec.group)[0]
        if ($row.sha256 -ne (Get-PayloadHash (Resolve-PayloadChild $Root $spec.destination)) -or
            $row.group -ne $spec.group -or $row.machine -ne $group.machine -or $row.image_bytes -le 0) {
            throw 'PE identity or architecture differs from the selected payload.'
        }
        if (-not $totals.ContainsKey($spec.group)) { $totals[$spec.group]=[long]0 }
        $totals[$spec.group]+=$row.image_bytes
        if ($group.PSObject.Properties.Name -contains 'max_direct_imports') {
            if ($row.imports.Count -gt $group.max_direct_imports) { throw 'Direct import budget exceeded.' }
        }
        $dir=[IO.Path]::GetDirectoryName($spec.destination).Replace('\','/')
        foreach ($dependency in $row.imports) {
            if ($Policy.system_imports -contains $dependency) { continue }
            $local=if ($dir) { "$dir/$dependency" } else { $dependency }
            if (-not $byPath.ContainsKey($local) -or $byPath[$local].machine -ne $row.machine) {
                throw "Unresolved app-local PE dependency: $($spec.destination) -> $dependency"
            }
        }
    }
    foreach ($key in $totals.Keys) {
        $group=@($Policy.groups | Where-Object id -eq $key)[0]
        if ($totals[$key] -gt $group.max_image_bytes) { throw "PE image byte budget exceeded: $key" }
    }
}
# Private helpers: ReleasePayload and PortableFinalRuntime do not export these functions.
$script:PayloadCliIdentityProducerPath=$PSCommandPath
function Get-PayloadCliFact([string]$Path) {
    $full=[IO.Path]::GetFullPath($Path)
    [pscustomobject][ordered]@{path=$full;bytes=(Get-Item -LiteralPath $full).Length;sha256=(Get-PayloadHash $full)}
}
function Assert-PayloadCliFact($Fact,[string]$ExpectedPath) {
    if ([IO.Path]::GetFullPath($Fact.path) -ine [IO.Path]::GetFullPath($ExpectedPath) -or
        $Fact.sha256 -ne (Get-PayloadHash $ExpectedPath) -or $Fact.bytes -ne (Get-Item -LiteralPath $ExpectedPath).Length) { throw 'CLI evidence file identity or path differs.' }
}
function Get-PayloadCliObjectHash($Value) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 30 -Compress))
    $hash=[Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($hash.ComputeHash($bytes)).Replace('-','') } finally { $hash.Dispose() }
}
function Write-PayloadCliExclusive([string]$Path,[byte[]]$Bytes) {
    $stream=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try { $stream.Write($Bytes,0,$Bytes.Length) } finally { $stream.Dispose() }
}
function Convert-PayloadCliApplication($Observed) {
    [pscustomobject][ordered]@{version=$Observed.version;commit=$Observed.commit;binarySha256=$Observed.binary_sha256
        configuration=$Observed.configuration;processArchitecture=$Observed.process_architecture;timingInstrumentation=$Observed.timing_instrumentation}
}
function Test-PayloadCliIdentityEvidence {
    param([string]$PayloadRoot,[string]$EvidenceDirectory,$Policy,$GuiIdentity)
    $root=[IO.Path]::GetFullPath($PayloadRoot); $evidence=[IO.Path]::GetFullPath($EvidenceDirectory)
    $cliPath=Resolve-PayloadChild $root 'SingleEditorCLI.exe'
    $runPath=Join-Path $evidence 'cli-identity-run.json'; $rawPath=Join-Path $evidence 'cli-identity.jsonl'
    $run=Get-Content -LiteralPath $runPath -Raw | ConvertFrom-Json
    if ($run.schema -cne 'singleeditor.cli-identity-run' -or $run.version -ne 2 -or $run.status -cne 'pass') { throw 'Unsupported or incomplete CLI identity evidence.' }
    Assert-PayloadCliFact $run.producer $script:PayloadCliIdentityProducerPath
    Assert-PayloadCliFact $run.executable $cliPath
    Assert-PayloadCliFact $run.raw_jsonl $rawPath
    if ($run.payload_root -ine $root -or $run.working_directory -ine $root -or
        $run.working_directory_source -cne 'ProcessStartInfo.WorkingDirectory' -or $run.process.image_path -ine $cliPath) { throw 'CLI did not originate from the selected exact payload path.' }
    $expectedExport=Join-Path $evidence 'cancelled-identity-export.zip'
    $expectedArguments='diagnostics export --output "'+$expectedExport+'" --jsonl'
    if ($run.arguments -cne $expectedArguments -or $run.export_path -ine $expectedExport -or
        $run.export_created -isnot [bool] -or $run.export_created -or (Test-Path -LiteralPath $expectedExport)) { throw 'CLI command or cancelled export differs.' }
    if ($run.process.pid -isnot [int] -and $run.process.pid -isnot [long]) { throw 'CLI process identity is missing.' }
    if ($run.process.pid -le 0 -or $run.process.exited -isnot [bool] -or -not $run.process.exited -or $run.process.exit_code -ne 6 -or
        $run.forced_cleanup -isnot [bool] -or $run.forced_cleanup -or $run.stderr -cne '' -or $run.error) { throw 'CLI did not complete ordinary EOF cancellation.' }
    $times=@($run.process.start_utc,$run.process.modules_observed_utc,$run.process.stdin_eof_utc,$run.completed_utc)
    $last=[DateTimeOffset]::MinValue
    foreach($value in $times){
        if([string]::IsNullOrWhiteSpace($value)){throw 'CLI execution timestamps are missing.'}
        $time=[DateTimeOffset]::Parse($value,[Globalization.CultureInfo]::InvariantCulture)
        if($time -lt $last){throw 'CLI execution timestamps are out of order.'};$last=$time
    }
    if ($run.policy_sha256 -ne (Get-PayloadCliObjectHash $Policy)) { throw 'CLI evidence uses another payload policy.' }
    Test-PayloadApplicationIdentity $GuiIdentity (Join-Path $root 'Single Editor.exe')
    if ((Get-PayloadCliObjectHash $run.gui_identity) -ne (Get-PayloadCliObjectHash $GuiIdentity)) { throw 'CLI evidence is paired with another GUI identity.' }
    if ((Get-Item -LiteralPath $rawPath).Length -gt 1048576) { throw 'CLI identity transcript is not bounded.' }
    $events=@(Get-Content -LiteralPath $rawPath | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json })
    if($events.Count -ne 3 -or $events[0].type -cne 'started' -or $events[1].type -cne 'snapshot-review' -or $events[2].type -cne 'error'){throw 'CLI transcript does not contain its ordinary identity cancellation sequence.'}
    for($i=0;$i -lt $events.Count;$i++){
        $event=$events[$i]
        if($event.schema -cne 'singleeditor.cli.event' -or $event.protocol_version -cne '1.0.0' -or $event.sequence -ne ($i+1) -or
            $event.command -cne 'diagnostics export' -or $event.terminal -isnot [bool] -or $event.terminal -ne ($i -eq 2)){throw 'CLI protocol identity or event order differs.'}
    }
    $terminal=$events[2]
    if($terminal.exit_code -ne 6 -or $terminal.error -cne 'snapshot-not-approved' -or $terminal.ok -isnot [bool] -or $terminal.ok){throw 'CLI terminal event is not ordinary cancellation.'}
    $cli=Convert-PayloadCliApplication $events[1].payload.application
    Test-PayloadApplicationIdentity $cli $cliPath
    if($GuiIdentity.commit -cne $cli.commit -or (Get-PayloadCliObjectHash $run.cli_identity) -ne (Get-PayloadCliObjectHash $cli)){throw 'Raw CLI/GUI source or application identities differ.'}
    $runtime=@($Policy.files | Where-Object group -eq 'vc-runtime')
    if(@($run.loaded_cli_runtime).Count -ne $runtime.Count -or $runtime.Count -eq 0){throw 'CLI local runtime observation is incomplete.'}
    foreach($spec in $runtime){
        $rows=@($run.loaded_cli_runtime | Where-Object path -ceq $spec.destination);$expected=Resolve-PayloadChild $root $spec.destination
        if($rows.Count -ne 1){throw 'CLI local runtime observation is duplicate or missing.'};$row=$rows[0]
        if($row.process_id -ne $run.process.pid -or $row.module_name -ine [IO.Path]::GetFileName($expected) -or $row.loaded_path -ine $expected -or
            $row.sha256 -ne (Get-PayloadHash $expected) -or $row.bytes -ne (Get-Item -LiteralPath $expected).Length){throw 'CLI did not load the selected app-local runtime path and bytes.'}
    }
    $run
}
function Invoke-PayloadCliIdentityEvidence {
    param([string]$PayloadRoot,[string]$EvidenceDirectory,$Policy,$GuiIdentity)
    $root=(Resolve-Path -LiteralPath $PayloadRoot).Path; $evidence=(Resolve-Path -LiteralPath $EvidenceDirectory).Path
    if($evidence -ieq $root -or $evidence.StartsWith($root.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'CLI evidence must stay outside the payload.'}
    $cliPath=Resolve-PayloadChild $root 'SingleEditorCLI.exe';$neverExport=Join-Path $evidence 'cancelled-identity-export.zip'
    foreach($name in @('cli-identity.jsonl','cli-identity-run.json','cancelled-identity-export.zip')){if(Test-Path -LiteralPath (Join-Path $evidence $name)){throw 'Preserve existing CLI identity evidence or export.'}}
    Test-PayloadApplicationIdentity $GuiIdentity (Join-Path $root 'Single Editor.exe')
    $producer=Get-PayloadCliFact $script:PayloadCliIdentityProducerPath;$executable=Get-PayloadCliFact $cliPath
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$cliPath;$start.WorkingDirectory=$root;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardInput=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.Arguments='diagnostics export --output "'+$neverExport+'" --jsonl'
    $process=[Diagnostics.Process]::new();$process.StartInfo=$start
    $lines=[Collections.Generic.List[string]]::new();$started=$false;$errorText='';$runtime=@();$cli=$null
    $run=[ordered]@{schema='singleeditor.cli-identity-run';version=2;status='failed';producer=$producer;payload_root=$root
        working_directory=$root;working_directory_source='ProcessStartInfo.WorkingDirectory';executable=$executable
        process=[ordered]@{pid=$null;start_utc=$null;image_path=$null;modules_observed_utc=$null;stdin_eof_utc=$null;exited=$false;exit_code=$null}
        arguments=$start.Arguments;gui_identity=$GuiIdentity;cli_identity=$null;policy_sha256=(Get-PayloadCliObjectHash $Policy)
        loaded_cli_runtime=@();raw_jsonl=$null;stderr='';forced_cleanup=$false;export_path=$neverExport;export_created=$false;completed_utc=$null;error=$null}
    try {
        if(-not $process.Start()){throw 'CLI identity process did not start.'};$started=$true
        $run.process.pid=$process.Id;$run.process.start_utc=$process.StartTime.ToUniversalTime().ToString('o');$stderr=$process.StandardError.ReadToEndAsync()
        $review=$null;$deadline=[DateTime]::UtcNow.AddSeconds(20)
        while($lines.Count -lt 16 -and -not $review){
            $read=$process.StandardOutput.ReadLineAsync();$remaining=[Math]::Max(1,[int]($deadline-[DateTime]::UtcNow).TotalMilliseconds)
            if(-not $read.Wait($remaining)){throw 'CLI identity review timed out.'};$line=$read.Result
            if($null -eq $line){throw 'CLI exited before its identity review.'};$lines.Add($line);$event=$line | ConvertFrom-Json
            if($event.type -eq 'snapshot-review'){$review=$event}
        }
        if(-not $review){throw 'CLI identity review was not observed.'}
        $cli=Convert-PayloadCliApplication $review.payload.application;Test-PayloadApplicationIdentity $cli $cliPath
        if($GuiIdentity.commit -cne $cli.commit){throw 'GUI and CLI report different source identities.'}
        $process.Refresh();$run.process.image_path=[IO.Path]::GetFullPath($process.MainModule.FileName)
        if($run.process.image_path -ine $cliPath){throw 'CLI actual process image path differs from selected payload.'}
        $loaded=@($process.Modules)
        $runtime=@(foreach($spec in $Policy.files | Where-Object group -eq 'vc-runtime'){
            $module=@($loaded | Where-Object ModuleName -ieq $spec.destination);$expected=Resolve-PayloadChild $root $spec.destination
            if($module.Count -ne 1 -or [IO.Path]::GetFullPath($module[0].FileName) -ine $expected){throw "CLI did not load its staged runtime: $($spec.destination)"}
            [pscustomobject][ordered]@{path=$spec.destination;module_name=$module[0].ModuleName;loaded_path=[IO.Path]::GetFullPath($module[0].FileName)
                bytes=(Get-Item -LiteralPath $module[0].FileName).Length;sha256=(Get-PayloadHash $module[0].FileName);process_id=$process.Id}
        })
        $run.process.modules_observed_utc=[DateTime]::UtcNow.ToString('o')
        $run.cli_identity=$cli;$run.loaded_cli_runtime=$runtime
        # EOF remains the public CLI's ordinary cancellation path. No export is approved.
        $process.StandardInput.Close();$run.process.stdin_eof_utc=[DateTime]::UtcNow.ToString('o')
        $tail=$process.StandardOutput.ReadToEndAsync()
        if(-not $process.WaitForExit(10000)){throw 'CLI did not finish identity cancellation.'}
        foreach($line in $tail.Result.Split("`n") | Where-Object { $_.Trim() }){$lines.Add($line.TrimEnd("`r"))}
        $errorText=$stderr.Result;$terminal=$lines[$lines.Count-1] | ConvertFrom-Json
        if($process.ExitCode -ne 6 -or $terminal.error -ne 'snapshot-not-approved' -or $errorText -or (Test-Path -LiteralPath $neverExport)){throw 'CLI identity cancellation did not finish normally without export.'}
        Assert-PayloadCliFact $producer $script:PayloadCliIdentityProducerPath;Assert-PayloadCliFact $executable $cliPath
        $run.status='pass'
    } catch { $run.error=$_.Exception.Message;throw } finally {
      try {
        if($started -and -not $process.HasExited){
            $run.forced_cleanup=$true
            try{$process.Kill()}catch{if(-not $process.HasExited){throw}}
            [void]$process.WaitForExit(5000)
        }
        if($started){$run.process.exited=$process.HasExited;if($process.HasExited){$run.process.exit_code=$process.ExitCode}}
        $run.stderr=$errorText;$run.export_created=[bool](Test-Path -LiteralPath $neverExport);$run.completed_utc=[DateTime]::UtcNow.ToString('o')
        $rawPath=Join-Path $evidence 'cli-identity.jsonl'
        $transcript=if($lines.Count){($lines -join [Environment]::NewLine)+[Environment]::NewLine}else{''}
        Write-PayloadCliExclusive $rawPath ([Text.UTF8Encoding]::new($false).GetBytes($transcript));$run.raw_jsonl=Get-PayloadCliFact $rawPath
        Write-PayloadCliExclusive (Join-Path $evidence 'cli-identity-run.json') ([Text.UTF8Encoding]::new($false).GetBytes(($run | ConvertTo-Json -Depth 14 -Compress)+"`n"))
      } finally { $process.Dispose() }
    }
    Test-PayloadCliIdentityEvidence -PayloadRoot $root -EvidenceDirectory $evidence -Policy $Policy -GuiIdentity $GuiIdentity
}
function New-PayloadRuntimeProof([string]$Stage, $Policy, [string]$GuiBundlePath, [string]$LinkerPath) {
    $root=Join-Path $Stage 'payload';$evidenceRoot=Join-Path $Stage 'evidence'
    $gui=Read-PayloadGuiIdentity $GuiBundlePath
    Test-PayloadApplicationIdentity $gui (Join-Path $root 'Single Editor.exe')
    $pe=@(Get-PayloadPeInventory $root $Policy $LinkerPath)
    $run=Invoke-PayloadCliIdentityEvidence -PayloadRoot $root -EvidenceDirectory $evidenceRoot -Policy $Policy -GuiIdentity $gui
    $runtime=@(foreach($row in $run.loaded_cli_runtime){[pscustomobject]@{path=$row.path;sha256=$row.sha256;loaded_from_payload=$true}})
    $proof=[ordered]@{schema='singleeditor.release-runtime-proof';version=1;gui=$gui;cli=$run.cli_identity;pe=$pe
        loaded_cli_runtime=$runtime;cli_exit_code=$run.process.exit_code;export_created=$false
        linker=@{path=$LinkerPath;sha256=(Get-PayloadHash $LinkerPath)}
        scope='Exact GUI/CLI build flags, PE static imports/image reservations and CLI app-local runtime. Not resident startup pages, clean-machine or complete media/installer acceptance.'}
    Write-PayloadJson (Join-Path $evidenceRoot 'runtime-proof.json') $proof
}
function Test-PayloadRuntimeProof([string]$Stage, $Policy) {
    $root=Join-Path $Stage 'payload'
    $proof=Get-Content -LiteralPath (Join-Path $Stage 'evidence/runtime-proof.json') -Raw | ConvertFrom-Json
    if ($proof.schema -ne 'singleeditor.release-runtime-proof' -or $proof.version -ne 1 -or
        $proof.cli_exit_code -ne 6 -or $proof.export_created) { throw 'Incomplete runtime proof.' }
    $gui=Read-PayloadGuiIdentity (Join-Path $Stage 'evidence/gui-identity.zip')
    Test-PayloadApplicationIdentity $gui (Join-Path $root 'Single Editor.exe')
    Test-PayloadApplicationIdentity $proof.gui (Join-Path $root 'Single Editor.exe')
    Test-PayloadApplicationIdentity $proof.cli (Join-Path $root 'SingleEditorCLI.exe')
    if ($proof.gui.commit -cne $proof.cli.commit -or $gui.commit -cne $proof.gui.commit) { throw 'Runtime source identities differ.' }
    Test-PayloadPeInventory $proof.pe $root $Policy
    $runtime=@($Policy.files | Where-Object group -eq 'vc-runtime')
    if ($proof.loaded_cli_runtime.Count -ne $runtime.Count) { throw 'Incomplete app-local runtime observation.' }
    foreach ($spec in $runtime) {
        $row=@($proof.loaded_cli_runtime | Where-Object path -eq $spec.destination)
        if ($row.Count -ne 1 -or -not $row[0].loaded_from_payload -or
            $row[0].sha256 -ne (Get-PayloadHash (Resolve-PayloadChild $root $spec.destination))) { throw 'App-local runtime observation differs from payload.' }
    }
    $proof
}
