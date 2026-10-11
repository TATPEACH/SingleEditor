param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$speakerCheckVoice = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
    $speakerCheckVoice.SelectVoice('Microsoft Zira Desktop')
    $speakerCheckVoice.Rate = 0
    $speakerCheckVoice.Volume = 100
    $labels = [ordered]@{
        FL='Front left'; FR='Front right'; FC='Center';
        BL='Back left'; BR='Back right'; SL='Side left'; SR='Side right';
        TFL='Top front left'; TFR='Top front right';
        TBL='Top back left'; TBR='Top back right'
    }
    # Use the explicit sample-rate overload; the Python assembler checks this rate.
    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
        48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono)
    foreach ($label in $labels.GetEnumerator()) {
        $path = Join-Path $OutputDirectory ($label.Key + '.wav')
        if (Test-Path -LiteralPath $path) { throw "Output already exists: $path" }
        $speakerCheckVoice.SetOutputToWaveFile($path, $format)
        $speakerCheckVoice.Speak($label.Value)
        $speakerCheckVoice.SetOutputToNull()
    }
} finally {
    $speakerCheckVoice.Dispose()
}
