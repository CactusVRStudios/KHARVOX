param(
    [string]$Launcher = (Join-Path $PSScriptRoot '../out/beta02-repack/launcher/bin/KharvoxLauncher/release/KharvoxLauncher.exe'),
    [string]$Stub = (Join-Path $PSScriptRoot '../out/beta02/native/intro-test-helper/Release/KharvoxGameIntro.exe')
)
$ErrorActionPreference = 'Stop'
$testDirectory = Join-Path ([IO.Path]::GetTempPath()) ('kharvox-intro-handoff-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDirectory | Out-Null
Copy-Item -LiteralPath $Stub -Destination (Join-Path $testDirectory 'KharvoxLauncher.exe')
New-Item -ItemType File -Path (Join-Path $testDirectory 'KharvoxLayer.dll') | Out-Null
$marker = Join-Path $testDirectory 'seen'
$assembly = [Reflection.Assembly]::LoadFrom($Launcher)
$type = $assembly.GetType('KharvoxLauncher.VrGameIntroSession', $true)
$flags = [Reflection.BindingFlags]'Static,NonPublic'
$start = $type.GetMethod('StartAsync', $flags)
$reset = $type.GetMethod('Reset', $flags)
$task = $start.Invoke($null, [object[]]@([string]$testDirectory, $null, [string]$marker, $true))
$session = $null
try {
    $tokenFile = Join-Path $testDirectory 'helper-token.txt'
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $tokenFile) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
    if (!(Test-Path -LiteralPath $tokenFile)) { throw 'Helper was not started' }
    $token = [IO.File]::ReadAllText($tokenFile)
    if ($task.IsCompleted -or (Test-Path -LiteralPath $marker)) { throw 'DOOM launch gate opened before intro dismissal' }
    $prefix = 'Local\KHARVOX-VR-INTRO-' + $token
    $dismiss = [Threading.EventWaitHandle]::OpenExisting($prefix + '-test-dismiss')
    $dismiss.Set() | Out-Null
    $session = $task.GetAwaiter().GetResult()
    if (!(Test-Path -LiteralPath $marker)) { throw 'Dismissal did not persist first-launch state' }
    $releaseVersion = $type.GetProperty('ReleaseVersion', $flags).GetValue($null, $null).ToString()
    if ([IO.File]::ReadAllText($marker).Trim() -ne $releaseVersion) { throw 'Dismissal did not persist the release version' }
    $release = [Threading.EventWaitHandle]::OpenExisting($prefix + '-release')
    $released = [Threading.EventWaitHandle]::OpenExisting($prefix + '-released')
    if ($released.WaitOne(0)) { throw 'Black helper exited before game requested handoff' }
    $repeat = $start.Invoke($null, [object[]]@([string]$testDirectory, $null, [string]$marker, $true)).GetAwaiter().GetResult()
    if ($null -ne $repeat) { throw 'Intro repeated without reset' }
    $release.Set() | Out-Null
    if (!$released.WaitOne(5000)) { throw 'Game handoff did not release helper' }
    $reset.Invoke($null, [object[]]@([string]$marker)) | Out-Null
    if (Test-Path -LiteralPath $marker) { throw 'Reset did not clear first-launch state' }
    $dismiss.Dispose(); $release.Dispose(); $released.Dispose()
    'PASS: no launch before dismissal, black helper retained, explicit handoff, persistence and reset'
} finally { if ($null -ne $session) { $session.Dispose() } }
