[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'Use a new ToolTestOnly output directory.'}
[void][IO.Directory]::CreateDirectory($output)
$build=Join-Path $output 'build'
function File-Fact([string]$Path) {
  $p=(Resolve-Path -LiteralPath $Path).Path
  return [ordered]@{path=$p;bytes=(Get-Item -LiteralPath $p).Length;sha256=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash}
}
$inputPaths=@('CMakeLists.txt','SingleEditorEtlDecode.cpp','DecoderContractTests.cpp','build-and-check.ps1')
$inputs=@($inputPaths | ForEach-Object {File-Fact (Join-Path $PSScriptRoot $_)})
$inputsJson=$inputs|ConvertTo-Json -Depth 6 -Compress
function Invoke-Native([string]$Command,[string[]]$Arguments,[string]$Log) {
  $preference=$ErrorActionPreference
  try {$ErrorActionPreference='Continue'; & $Command @Arguments *> $Log; $code=$LASTEXITCODE}
  finally {$ErrorActionPreference=$preference}
  return $code
}
$code=Invoke-Native 'cmake.exe' @('-S',$PSScriptRoot,'-B',$build,'-G','Visual Studio 18 2026','-A','x64','-DCMAKE_SYSTEM_VERSION=10.0.26100.0') (Join-Path $output 'configure.log')
if($code){throw "CMake configure failed: $code"}
$code=Invoke-Native 'cmake.exe' @('--build',$build,'--config','Release','--','/m:2') (Join-Path $output 'build.log')
if($code){throw "ToolTestOnly build failed: $code"}
$exe=Join-Path $build 'Release/SingleEditorEtlDecode.exe'
& (Join-Path $build 'Release/SingleEditorEtlDecodeContractTests.exe') > (Join-Path $output 'contract.jsonl')
if($LASTEXITCODE){throw 'Native contract checks failed.'}
$rows=@(Get-Content -LiteralPath (Join-Path $output 'contract.jsonl') | ForEach-Object {$_ | ConvertFrom-Json})
if($rows.Count -ne 3 -or $rows[0].timestamp_raw -cne '9007199254740993' -or $rows[0].payload_hex -cne '00FF225C' -or
 $rows[1].gaps.Count -ne 0 -or $rows[1].tdh.decoding_source -ne 0 -or $rows[2].real_etl_decoded){throw 'JSONL export contract changed.'}
$invalid=Join-Path $output 'invalid.etl'
[IO.File]::WriteAllBytes($invalid,[byte[]](0,1,2,3,4,5,6,7))
$invalidHash=(Get-FileHash -LiteralPath $invalid -Algorithm SHA256).Hash
$decode=Join-Path $output 'invalid-decode'
& $exe $invalid $decode *> (Join-Path $output 'invalid-decode.log')
if($LASTEXITCODE -ne 1){throw 'Malformed ETL did not fail.'}
$headerPath=Join-Path $decode 'header.json'
$header=Get-Content -LiteralPath $headerPath -Raw | ConvertFrom-Json
if($header.status -cne 'failed' -or $header.open_trace_status -eq 0 -or -not $header.input_unchanged -or $header.release_eligible -or
  $header.input.sha256 -cne $invalidHash -or $header.input_after.sha256 -cne $invalidHash -or $header.process_trace_mode -ne 268439552){throw 'Failed ETL receipt is not honest or correctly bound.'}
$headerHash=(Get-FileHash -LiteralPath $headerPath -Algorithm SHA256).Hash
$previousPreference=$ErrorActionPreference
try {
  $ErrorActionPreference='Continue'
  & $exe $invalid $decode *> (Join-Path $output 'existing-output.log')
  $repeatCode=$LASTEXITCODE
} finally {$ErrorActionPreference=$previousPreference}
if($repeatCode -ne 1 -or (Get-FileHash -LiteralPath $headerPath -Algorithm SHA256).Hash -cne $headerHash){throw 'Existing evidence was overwritten.'}
$inputsAfter=@($inputPaths | ForEach-Object {File-Fact (Join-Path $PSScriptRoot $_)})
if(($inputsAfter|ConvertTo-Json -Depth 6 -Compress) -cne $inputsJson){throw 'Compiler inputs changed during this ToolTestOnly build/check run.'}
$receipt=[ordered]@{schema='singleeditor.etl-decoder-focused-checks';version=1;status='pass';checks=@('native-export-five-cases','jsonl-roundtrip','invalid-etl-failure-and-input-binding','preserve-existing-output');real_etl_decoded=$false;collection_started=$false;product_started=$false;decoder_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
  inputs=$inputs;inputs_after=$inputsAfter;inputs_unchanged=$true;decoder=(File-Fact $exe)
  cmake=(File-Fact (Get-Command cmake.exe).Source);generator='Visual Studio 18 2026';architecture='x64';configuration='Release';msbuild_arguments=@('/m:2');sdk='10.0.26100.0'
  configure_log=(File-Fact (Join-Path $output 'configure.log'));build_log=(File-Fact (Join-Path $output 'build.log'));contract_output=(File-Fact (Join-Path $output 'contract.jsonl'));invalid_etl_header=(File-Fact $headerPath)}
[IO.File]::WriteAllText((Join-Path $output 'receipt.json'),($receipt|ConvertTo-Json -Depth 6)+"`n",[Text.UTF8Encoding]::new($false))
Write-Output ('TOOLTESTONLY_DECODER='+$exe)
