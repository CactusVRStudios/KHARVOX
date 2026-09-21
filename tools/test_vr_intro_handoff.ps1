param(
    [string]$Launcher = (Join-Path $PSScriptRoot '../out/beta02-repack/launcher/bin/KharvoxLauncher/release/KharvoxLauncher.exe'),
    [string]$Stub = (Join-Path $PSScriptRoot '../out/beta02/native/intro-test-helper/Release/KharvoxGameIntro.exe'),
    [switch]$HangTeardown
)
$ErrorActionPreference = 'Stop'
$testDirectory = Join-Path ([IO.Path]::GetTempPath()) ('kharvox-intro-handoff-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDirectory | Out-Null
Copy-Item -LiteralPath $Stub -Destination (Join-Path $testDirectory 'KharvoxLauncher.exe')
New-Item -ItemType File -Path (Join-Path $testDirectory 'KharvoxLayer.dll') | Out-Null
if ($HangTeardown) { New-Item -ItemType File -Path (Join-Path $testDirectory 'hang-teardown') | Out-Null }
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
    if (!$HangTeardown -and !$released.WaitOne(0)) { throw 'Intro resources were not released before DOOM launch gate opened' }
    $exitedFile = Join-Path $testDirectory 'helper-exited.txt'
    if (!$HangTeardown -and !(Test-Path -LiteralPath $exitedFile)) { throw 'Launch gate opened before helper process exit' }
    $helperId = [int][IO.File]::ReadAllText((Join-Path $testDirectory 'helper-pid.txt'))
    if (Get-Process -Id $helperId -ErrorAction SilentlyContinue) { throw 'Intro process still alive at launch gate' }
    $repeat = $start.Invoke($null, [object[]]@([string]$testDirectory, $null, [string]$marker, $true)).GetAwaiter().GetResult()
    if ($null -ne $repeat) { throw 'Intro repeated without reset' }
    $release.Set() | Out-Null
    if (!$HangTeardown -and !$released.WaitOne(5000)) { throw 'Game handoff did not release helper' }
    $reset.Invoke($null, [object[]]@([string]$marker)) | Out-Null
    if (Test-Path -LiteralPath $marker) { throw 'Reset did not clear first-launch state' }
    $dismiss.Dispose(); $release.Dispose(); $released.Dispose()
    'PASS: no launch before dismissal or process exit, resources released, persistence and reset'
} finally { if ($null -ne $session) { $session.Dispose() } }
