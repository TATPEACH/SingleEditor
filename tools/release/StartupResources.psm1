Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ReleasePayload.psm1')
. (Join-Path $PSScriptRoot 'PortablePayloadContext.ps1')

function New-StartupResourceContext([string]$StageRoot, [string]$GuiPath) {
    if (-not [Environment]::Is64BitProcess) { throw 'Startup resource observation requires a 64-bit PowerShell host.' }
    $stage = (Resolve-Path -LiteralPath $StageRoot).Path
    $policyPath = Join-Path $PSScriptRoot 'payload-policy.json'
    $verified = Test-ReleasePayload $stage $policyPath
    $payload = Join-Path $stage 'payload'
    $actualGui=(Resolve-Path -LiteralPath $GuiPath).Path
    $fixedContext=$null
    if ($actualGui -ine (Join-Path $payload 'Single Editor.exe')) {
        if ($actualGui -ine (Join-Path (Get-FixedPortablePath) 'Single Editor.exe')) {
            throw 'Resource observation must launch the selected sealed payload GUI or the exact fixed portable GUI.'
        }
        $fixedContext=New-FixedPortablePayloadContext $stage
        $payload=$fixedContext.payload
    }
    $bom = Get-Content -LiteralPath (Join-Path $stage 'bom.json') -Raw | ConvertFrom-Json
    $byPath = @{}
    foreach ($entry in $bom.entries) { $byPath[(Join-Path $payload $entry.path)] = $entry }
    if (-not ('SingleEditor.StartupResources' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
namespace SingleEditor {
    public sealed class ModulePages {
        public string Name, Path;
        public long ImageBytes, BaseAddress, QueryBeginQpc, QueryEndQpc;
        public int Pages, ResidentPages, ShareableResidentPages;
    }
    public sealed class ResourceSnapshot {
        public long BeginQpc, EndQpc, WorkingSetBytes, PrivateBytes;
        public int ProcessId, PageBytes;
        public ModulePages[] Modules;
    }
    public static class StartupResources {
        [StructLayout(LayoutKind.Sequential)]
        private struct WorkingSetEntry { public IntPtr Address; public UIntPtr Flags; }
        [DllImport("psapi.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool QueryWorkingSetEx(IntPtr process,
            [In, Out] WorkingSetEntry[] entries, int bytes);
        public static ResourceSnapshot Capture(Process process) {
            var result = new ResourceSnapshot();
            result.BeginQpc = Stopwatch.GetTimestamp();
            result.ProcessId = process.Id;
            result.PageBytes = Environment.SystemPageSize;
            process.Refresh();
            if (process.HasExited) throw new InvalidOperationException("Observed process exited.");
            var modules = process.Modules;
            if (modules.Count == 0 || modules.Count > 512)
                throw new InvalidOperationException("Module inventory outside observation bound.");
            var rows = new List<ModulePages>();
            long totalPages = 0;
            foreach (ProcessModule module in modules) {
                var row = new ModulePages();
                row.Name = module.ModuleName; row.Path = module.FileName;
                row.BaseAddress = module.BaseAddress.ToInt64();
                row.ImageBytes = module.ModuleMemorySize;
                if (row.ImageBytes <= 0 || row.ImageBytes > 512L * 1024 * 1024 ||
                    row.BaseAddress % result.PageBytes != 0)
                    throw new InvalidOperationException("Unsupported module image span: " + row.Name);
                row.Pages = checked((int)((row.ImageBytes + result.PageBytes - 1) / result.PageBytes));
                totalPages += row.Pages;
                if (totalPages > 2L * 1024 * 1024 * 1024 / result.PageBytes)
                    throw new InvalidOperationException("Total image span exceeds observation bound.");
                var entries = new WorkingSetEntry[row.Pages];
                for (int i = 0; i < entries.Length; ++i)
                    entries[i].Address = new IntPtr(row.BaseAddress + (long)i * result.PageBytes);
                row.QueryBeginQpc = Stopwatch.GetTimestamp();
                if (!QueryWorkingSetEx(process.Handle, entries,
                    checked(entries.Length * Marshal.SizeOf(typeof(WorkingSetEntry)))))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "QueryWorkingSetEx: " + row.Name);
                row.QueryEndQpc = Stopwatch.GetTimestamp();
                foreach (var entry in entries) {
                    ulong flags = entry.Flags.ToUInt64();
                    if ((flags & 1UL) == 0) continue;
                    ++row.ResidentPages;
                    if ((flags & (1UL << 15)) != 0) ++row.ShareableResidentPages;
                }
                rows.Add(row);
            }
            process.Refresh();
            if (process.HasExited) throw new InvalidOperationException("Observed process exited during capture.");
            result.WorkingSetBytes = process.WorkingSet64;
            result.PrivateBytes = process.PrivateMemorySize64;
            result.Modules = rows.ToArray();
            result.EndQpc = Stopwatch.GetTimestamp();
            return result;
        }
    }
}
'@
    }
    if ($null -ne $fixedContext) { return $fixedContext }
    [pscustomobject]@{stage=$stage;payload=$payload;policy_path=$policyPath;before=$verified;by_path=$byPath}
}

function Get-StartupResourceSnapshot($Context, [Diagnostics.Process]$Process, [long]$CoreReadyQpc) {
    if ($Context.PSObject.Properties['mode']) {
        Assert-FixedPortableContextIdentity $Context
        if ([IO.Path]::GetFullPath($Process.MainModule.FileName) -ine $Context.gui_path) {
            throw 'Resource process did not start from the fixed portable GUI.'
        }
    }
    $requestQpc = [Diagnostics.Stopwatch]::GetTimestamp()
    if ($CoreReadyQpc -le 0 -or $CoreReadyQpc -gt $requestQpc) { throw 'A real, completed CoreReady timestamp is required.' }
    $capture = [SingleEditor.StartupResources]::Capture($Process)
    $issues = [Collections.Generic.List[string]]::new()
    $seen = @{}
    $rows = @(
        foreach ($module in $capture.Modules) {
            $entry = $Context.by_path[$module.Path]
            $isPayload = $module.Path.StartsWith($Context.payload + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
            if ($isPayload -and $null -eq $entry) { $issues.Add('unclassified-payload-module:' + $module.Path) }
            if ($null -ne $entry) {
                $seen[$module.Path] = $true
                if ($entry.group -notin @('gui','vc-runtime')) { $issues.Add('optional-payload-module-present:' + $entry.path) }
                if ($module.ResidentPages -le 0) { $issues.Add('required-image-without-resident-pages:' + $entry.path) }
            }
            [pscustomobject][ordered]@{
                name=$module.Name;path=$module.Path
                group=$(if ($null -eq $entry) {'external-to-payload'} else {$entry.group})
                payload_sha256=$(if ($null -eq $entry) {$null} else {$entry.sha256})
                image_bytes=$module.ImageBytes;page_count=$module.Pages
                resident_pages=$module.ResidentPages;resident_bytes=([long]$module.ResidentPages * $capture.PageBytes)
                shareable_resident_pages=$module.ShareableResidentPages
                nonshareable_resident_pages=($module.ResidentPages - $module.ShareableResidentPages)
                query_begin_qpc=$module.QueryBeginQpc;query_end_qpc=$module.QueryEndQpc
            }
        }
    )
    foreach ($path in $Context.by_path.Keys) {
        $entry = $Context.by_path[$path]
        if ($entry.group -in @('gui','vc-runtime') -and -not $seen.ContainsKey($path)) {
            $issues.Add('required-app-local-module-missing:' + $entry.path)
        }
    }
    $frequency = [Diagnostics.Stopwatch]::Frequency
    [pscustomobject][ordered]@{
        status=$(if ($issues.Count -eq 0) {'pass'} else {'failed'})
        issues=$issues.ToArray();process_id=$capture.ProcessId;page_bytes=$capture.PageBytes
        core_ready_qpc=$CoreReadyQpc;request_qpc=$requestQpc
        capture_begin_qpc=$capture.BeginQpc;capture_end_qpc=$capture.EndQpc;qpc_frequency=$frequency
        begin_after_core_ms=[Math]::Round(1000.0 * ($capture.BeginQpc - $CoreReadyQpc) / $frequency, 3)
        capture_elapsed_ms=[Math]::Round(1000.0 * ($capture.EndQpc - $capture.BeginQpc) / $frequency, 3)
        process_working_set_bytes=$capture.WorkingSetBytes;process_private_bytes=$capture.PrivateBytes
        modules=$rows
        scope='Sequential module/page snapshot after real CoreReady; not an atomic snapshot or a history of pre-Core loads, file reads, optional instantiation or first touch. Shared flag means shareable, not a count of actual sharing processes. Timings with this observer are not performance acceptance.'
    }
}

function Complete-StartupResourceContext($Context) {
    if ($Context.PSObject.Properties['mode']) {
        Assert-FixedPortableContextIdentity $Context
        $after=New-FixedPortablePayloadContext $Context.stage
        if ($after.before.bom_sha256 -cne $Context.before.bom_sha256 -or
            $after.before.policy_sha256 -cne $Context.before.policy_sha256) {
            throw 'Fixed portable or sealed source changed during resource observation.'
        }
        return [pscustomobject]@{status='pass';stage=$after.stage;bom_sha256=$after.before.bom_sha256
            policy_sha256=$after.before.policy_sha256;payload_unchanged=$true;mode='fixed-portable'
            payload_root=$after.payload;gui_path=$after.gui_path}
    }
    $after = Test-ReleasePayload $Context.stage $Context.policy_path
    if ($after.bom_sha256 -ne $Context.before.bom_sha256 -or $after.policy_sha256 -ne $Context.before.policy_sha256) {
        throw 'Sealed payload changed during resource observation.'
    }
    [pscustomobject]@{status='pass';stage=$Context.stage;bom_sha256=$after.bom_sha256
        policy_sha256=$after.policy_sha256;payload_unchanged=$true}
}
Export-ModuleMember -Function New-StartupResourceContext,Get-StartupResourceSnapshot,Complete-StartupResourceContext
