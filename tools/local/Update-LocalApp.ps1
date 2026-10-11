param(
    [Parameter(Mandatory = $true)][string]$PayloadDirectory,
    [Parameter(Mandatory = $true)][string]$AppDirectory
)

$ErrorActionPreference = 'Stop'
$payloadRoot = (Resolve-Path -LiteralPath $PayloadDirectory).Path.TrimEnd('\')
$appRoot = [IO.Path]::GetFullPath($AppDirectory).TrimEnd('\')
if ($payloadRoot -eq $appRoot) { throw 'Use a separate payload directory as the update source.' }
foreach ($requiredFile in @('Single Editor.exe', 'SingleEditorCLI.exe', 'icon.ico')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot $requiredFile) -PathType Leaf)) {
        throw "Incomplete payload: $requiredFile is missing."
    }
}
$runningEditors = @(Get-Process -Name 'Single Editor', 'Single Editor Timing' -ErrorAction SilentlyContinue)
if ($runningEditors.Count -gt 0) {
    throw 'Close every Single Editor instance normally before updating. A running old copy can receive default-open requests.'
}

$payloadFiles = @(Get-ChildItem -LiteralPath $payloadRoot -File -Recurse)
foreach ($payloadFile in $payloadFiles) {
    $relativePath = $payloadFile.FullName.Substring($payloadRoot.Length).TrimStart('\')
    $destination = Join-Path $appRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $payloadFile.FullName -Destination $destination -Force
    if ((Get-FileHash -LiteralPath $payloadFile.FullName).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) {
        throw "Copy verification failed: $relativePath"
    }
}

$applicationKey = 'HKCU:\Software\Classes\Applications\Single Editor.exe'
$commandKey = $applicationKey + '\shell\open\command'
$iconKey = $applicationKey + '\DefaultIcon'
$appPathKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\Single Editor.exe'
foreach ($registryKey in @($applicationKey, $commandKey, $iconKey, $appPathKey)) {
    New-Item -Path $registryKey -Force | Out-Null
}
$appExe = Join-Path $appRoot 'Single Editor.exe'
Set-Item -LiteralPath $commandKey -Value ('"' + $appExe + '" "%1"')
Set-Item -LiteralPath $iconKey -Value (Join-Path $appRoot 'icon.ico')
Set-Item -LiteralPath $appPathKey -Value $appExe
New-ItemProperty -LiteralPath $applicationKey -Name 'FriendlyAppName' -Value 'Single Editor' -PropertyType String -Force | Out-Null
New-ItemProperty -LiteralPath $appPathKey -Name 'Path' -Value $appRoot -PropertyType String -Force | Out-Null

# Keep the diagnostic build out of Open With; retain its registration for rollback.
$timingKey = 'HKCU:\Software\Classes\Applications\Single Editor Timing.exe'
if (Test-Path -LiteralPath $timingKey) {
    New-ItemProperty -LiteralPath $timingKey -Name 'NoOpenWith' -Value '' -PropertyType String -Force | Out-Null
}
# UserChoice and its Windows-protected hash are intentionally left intact.
if (-not ('SingleEditorLocal.ShellNotification' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace SingleEditorLocal {
    public static class ShellNotification {
        [DllImport("shell32.dll")]
        public static extern void SHChangeNotify(uint eventId, uint flags, IntPtr item1, IntPtr item2);
    }
}
'@
}
[SingleEditorLocal.ShellNotification]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
[pscustomobject]@{
    AppDirectory = $appRoot
    VerifiedFiles = $payloadFiles.Count
    GuiSha256 = (Get-FileHash -LiteralPath $appExe).Hash
    UserChoiceModified = $false
}
