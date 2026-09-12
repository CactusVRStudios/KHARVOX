param([string]$Launcher = 'D:\KHARVOX\out\launcher-amiga\KharvoxLauncher.exe', [string]$Output = 'D:\KHARVOX\tmp\amiga-test')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
New-Item -ItemType Directory -Force $Output | Out-Null
$assembly = [Reflection.Assembly]::LoadFrom($Launcher)
$flags = [Reflection.BindingFlags]'Instance,Public,NonPublic'
$rendererType = $assembly.GetType('KharvoxLauncher.AmigaCracktroRenderer', $true)
$renderer = [Activator]::CreateInstance($rendererType, $true)
try {
    foreach ($seconds in @(0.0, 2.0, 6.0, 12.0, 20.0, 80.0)) {
        $rendererType.GetMethod('Render', $flags).Invoke($renderer, @($seconds)) | Out-Null
        $frame = $rendererType.GetProperty('Frame', $flags).GetValue($renderer, $null)
        $frame.Save((Join-Path $Output ('frame-'+$seconds+'.png')), [Drawing.Imaging.ImageFormat]::Png)
    }
} finally { $renderer.Dispose() }
$engineType = $assembly.GetType('KharvoxLauncher.AmigaAudioEngine', $true)
$engine = [Activator]::CreateInstance($engineType, $true)
$samples = New-Object System.Int16[] 4410
$fill = $engineType.GetMethod('Fill', $flags)
$peak = 0
$wave = [IO.BinaryWriter]::new([IO.File]::Create((Join-Path $Output 'sid-test.wav')))
try {
    $count = 44100 * 10
    $wave.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $wave.Write([int](36+$count*2))
    $wave.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $wave.Write([int]16)
    $wave.Write([int16]1); $wave.Write([int16]1); $wave.Write([int]44100); $wave.Write([int]88200)
    $wave.Write([int16]2); $wave.Write([int16]16); $wave.Write([Text.Encoding]::ASCII.GetBytes('data')); $wave.Write([int]($count*2))
    for ($i=0; $i -lt 100; $i++) {
        $fill.Invoke($engine, [object[]]@(,$samples.PSObject.BaseObject)) | Out-Null
        foreach ($sample in $samples) { $peak = [Math]::Max($peak, [Math]::Abs([int]$sample)); $wave.Write([int16]$sample) }
    }
} finally { $wave.Dispose() }
if ($peak -lt 100 -or $peak -ge 32767) { throw "SID output silent or clipping: $peak" }
$resources = $assembly.GetManifestResourceNames() | Where-Object { $_ -like 'Kharvox.Cracktro.*' }
if ($resources -notcontains 'Kharvox.Cracktro.Amiga.possessed.paula.gz') { throw 'Paula sequence not embedded' }
if ($resources -match '(?i)brainwalker') { throw 'Unwanted Brainwalker resource' }
if ($resources -match '\.wav$|\.dll$|\.js$') { throw 'Unexpected runtime dependency' }
$formType = $assembly.GetType('KharvoxLauncher.CracktroForm', $true)
for ($i=0; $i -lt 3; $i++) {
    $form = [Activator]::CreateInstance($formType, $flags, $null, [object[]]@($true), $null)
    if ($form.ClientSize.Width -ne 800 -or $form.ClientSize.Height -ne 600 -or $form.FormBorderStyle -ne 'None') { throw 'Incorrect intro window dimensions or border' }
    $argsForKey = [object[]]@([Windows.Forms.Message]::new(), [Windows.Forms.Keys]::Escape)
    $handled = $formType.GetMethod('ProcessCmdKey', $flags).Invoke($form, $argsForKey)
    if (!$handled -or !$form.IsDisposed) { throw 'ESC did not close and dispose intro' }
}
Write-Output "PASS: six animation frames, ten seconds live Paula synthesis; audio peak=$peak; $($resources.Count) embedded resources; three 800x600 borderless windows closed/disposed through ESC."

# Invoke the actual footer action, including replacing an intro that is still open.
$mainType = $assembly.GetType('KharvoxLauncher.MainForm', $true)
$main = [Activator]::CreateInstance($mainType, $true)
$previous = $null
try {
    foreach ($expectedAmiga in @($false, $true, $false, $true)) {
        $mainType.GetMethod('ShowNextCracktro', $flags).Invoke($main, @()) | Out-Null
        $current = $mainType.GetField('cracktroForm', $flags).GetValue($main)
        $isAmiga = $null -ne $formType.GetField('amigaAudio', $flags).GetValue($current)
        if ($isAmiga -ne $expectedAmiga) { throw 'Wrong intro alternation order' }
        if ($null -ne $previous -and !$previous.IsDisposed) { throw 'Previous intro was not disposed' }
        $previous = $current
    }
    $formType.GetMethod('ProcessCmdKey', $flags).Invoke($previous, [object[]]@([Windows.Forms.Message]::new(), [Windows.Forms.Keys]::Escape)) | Out-Null
    if (!$previous.IsDisposed) { throw 'Visible Amiga intro did not close with ESC' }
} finally { if ($null -ne $previous) {$previous.Dispose()}; $main.Dispose() }
Write-Output 'PASS: footer alternates C64 -> Amiga -> C64 -> Amiga, previous window disposed, final ESC closes playback.'





