Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'

function Read-RecoveredInstaller([string]$Path) {
    $sections=@{}; $current=''
    foreach ($raw in Get-Content -LiteralPath $Path) {
        $line=$raw.Trim()
        if (-not $line -or $line.StartsWith(';')) { continue }
        if ($line -match '^\[([^]]+)\]$') {
            $current=$Matches[1]; $sections[$current]=[Collections.Generic.List[object]]::new(); continue
        }
        if (-not $current) { continue }
        if ($current -eq 'Setup') {
            $pair=$line.Split('=',2)
            if ($pair.Length -ne 2) { throw 'Unrecognized recovered Setup entry.' }
            $sections[$current].Add(@{name=$pair[0];value=$pair[1]}); continue
        }
        $row=@{}
        foreach ($field in [regex]::Matches($line,'(?:^|;)\s*([A-Za-z][A-Za-z0-9]*)\s*:\s*("(?:[^"]|"")*"|[^;]*)\s*(?=;|$)')) {
            $value=$field.Groups[2].Value.Trim()
            if ($value.StartsWith('"') -and $value.EndsWith('"')) { $value=$value.Substring(1,$value.Length-2).Replace('""','"') }
            $key=$field.Groups[1].Value
            if ($row.ContainsKey($key)) { throw 'Duplicate recovered installer field.' }
            $row[$key]=$value
        }
        if ($row.Count) { $sections[$current].Add($row) }
    }
    $sections
}
function Test-InstallerAssociationContract([string]$RecoveredScript) {
    $sections=Read-RecoveredInstaller $RecoveredScript
    $rows=@($sections.Registry)
    $extensions=@('.wav','.flac','.aif','.aiff','.ogg','.wem','.bnk')
    foreach ($row in $rows) {
        if ($row.Subkey -match '(^|\\)\.[^\\]+$' -or $row.Subkey -match '(^|\\)UserChoice(\\|$)') {
            throw 'Installer writes shared extension defaults or UserChoice; user defaults must be preserved.'
        }
    }
    if ($rows.Count -ne 20) { throw 'Installer association inventory is incomplete or has unclassified entries.' }
    $prog='Software\Classes\SingleEditor.AssociatedFile'
    $app='Software\Classes\Applications\Single Editor.exe'
    $expected=@{}
    $expected[$prog+'|']=@{data='Single Editor File';flags='uninsdeletekey'}
    $expected[$prog+'\DefaultIcon|']=@{data='{app}\icon.ico';flags=''}
    $expected[$prog+'\shell\open\command|']=@{data='"{app}\Single Editor.exe" "%1"';flags=''}
    $expected[$app+'|FriendlyAppName']=@{data='Single Editor';flags='uninsdeletekey'}
    $expected[$app+'\DefaultIcon|']=@{data='{app}\icon.ico';flags=''}
    $expected[$app+'\shell\open\command|']=@{data='"{app}\Single Editor.exe" "%1"';flags=''}
    foreach ($ext in $extensions) {
        $expected[$app+'\SupportedTypes|'+$ext]=@{data='';flags=''}
        $expected['Software\Classes\'+$ext+'\OpenWithProgids|SingleEditor.AssociatedFile']=@{data='';flags='uninsdeletevalue'}
    }
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($row in $rows) {
        $name=if ($row.ContainsKey('ValueName')) { $row.ValueName } else { '' }
        $data=if ($row.ContainsKey('ValueData')) { $row.ValueData } else { '' }
        $flags=if ($row.ContainsKey('Flags')) { $row.Flags } else { '' }
        $key=$row.Subkey+'|'+$name
        if (-not $expected.ContainsKey($key) -or -not $seen.Add($key) -or $row.Root -ne 'HKA' -or
            $row.Tasks -ne 'associatefiles' -or $row.ValueType -ne 'String') { throw 'Unclassified or unconditional installer association entry.' }
        if ($data -cne $expected[$key].data -or $flags -ne $expected[$key].flags) {
            throw 'Installer association command, icon or uninstall ownership differs from the contract.'
        }
    }
    $setup=@{}; foreach ($pair in $sections.Setup) { $setup[$pair.name]=$pair.value }
    if ($setup.ArchitecturesAllowed -ne 'x64compatible' -or $setup.ArchitecturesInstallIn64BitMode -ne 'x64compatible') {
        throw 'Installer must admit only the architecture supported by the x64 application.'
    }
    [pscustomobject]@{status='compiled-metadata-verified';association_entries=$rows.Count;supported_extensions=$extensions
        extension_defaults_written=$false;all_registrations_task_gated=$true;uninstall_cleanup='private keys and own OpenWith values only'
        setup=$setup;registry_entries=$rows;tasks=@($sections.Tasks);shortcuts=@($sections.Icons);run_entries=@($sections.Run)
        scope='Recovered compiled metadata; not actual installation, Shell launch or uninstall behavior.'}
}
Export-ModuleMember -Function Test-InstallerAssociationContract
