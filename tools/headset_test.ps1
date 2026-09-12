param(
    [ValidateSet('Prepare','Save','Analyze')][string]$Action = 'Prepare',
    [ValidateSet('None','Once','Sequence')][string]$Capture = 'None',
    [string]$Runtime,
    [string]$Evidence,
    [string]$EvidenceRoot = 'D:\KHARVOX-backups\headset-acceptance-r139'
)
# Preparation only: never launches a game, changes an OpenXR runtime, or edits
# saved launcher settings. Every preparation gets a fresh extracted package.
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path $PSScriptRoot -Parent
$release = 'KHARVOX-v0.1-test-r139'
$archive = Join-Path $sourceRoot "Releases\$release.rar"
$expectedArchiveHash = 'c4e352649ef567633f4fee852c64c20a5c4331780e7910505e76d07ea20c7135'
$id = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6)
if ($Action -eq 'Analyze') {
    if (!$Evidence) { throw 'Analyze requires -Evidence with the saved evidence directory.' }
    $python = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    if (!(Test-Path -LiteralPath $python)) { throw 'Bundled Python runtime not found; run analyze_headset_evidence.py with Python 3.9 or newer.' }
    & $python (Join-Path $PSScriptRoot 'analyze_headset_evidence.py') --evidence $Evidence --output (Join-Path $EvidenceRoot "analysis\$id")
    if ($LASTEXITCODE -ne 0) { throw 'Evidence analysis could not complete. Original evidence is unchanged.' }
    return
}
$active = @(Get-Process -Name DOOMx64vk,KharvoxLauncher -ErrorAction SilentlyContinue)
if ($Action -eq 'Prepare' -and $active.Count) {
    throw 'Close DOOM and the launcher before preparing a new test. Use -Action Save to preserve current evidence.'
}
if ($Action -eq 'Save' -and [string]::IsNullOrWhiteSpace($Runtime)) {
    throw 'Save requires -Runtime with the directory used for this test.'
}
if (!$Runtime) { $Runtime = Join-Path $sourceRoot "Releases\$release" }
$Runtime = [IO.Path]::GetFullPath($Runtime)
if (!(Test-Path -LiteralPath (Join-Path $Runtime 'KharvoxLayer.dll'))) {
    throw 'The runtime directory must contain KharvoxLayer.dll.'
}
New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null
$snapshot = Join-Path $EvidenceRoot "evidence\$id"
New-Item -ItemType Directory -Path $snapshot | Out-Null
$files = @()
$files += Get-ChildItem -LiteralPath $env:TEMP -Filter 'KHARVOX*.log' -File
$files += Get-ChildItem -LiteralPath $Runtime -File | Where-Object {
    $_.Name -match '^native_|^capture_|^enable_|\.log$|status|^KharvoxLayer\.json$'
}
$settings = Join-Path $env:LOCALAPPDATA 'KHARVOX\settings.json'
if (Test-Path -LiteralPath $settings) { $files += Get-Item -LiteralPath $settings }
$simulator = Join-Path $env:LOCALAPPDATA 'OpenXR-Simulator'
if (Test-Path -LiteralPath $simulator) {
    $files += Get-ChildItem -LiteralPath $simulator -File | Where-Object { $_.Extension -in '.json','.log','.txt' }
}
$dumps = Join-Path $env:LOCALAPPDATA 'CrashDumps'
if (Test-Path -LiteralPath $dumps) {
    $files += Get-ChildItem -LiteralPath $dumps -File | Where-Object { $_.Name -match '^DOOM.*\.(dmp|mdmp)$' }
}
$manifest = @()
foreach ($file in ($files | Sort-Object FullName -Unique)) {
    $relative = $file.FullName.Replace(':','').TrimStart('\')
    $copy = Join-Path $snapshot $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $copy -Parent) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $copy
    $hash = (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest += [pscustomobject]@{Source=$file.FullName;Copy=$copy;Bytes=(Get-Item -LiteralPath $copy).Length;SHA256=$hash}
}
$manifest | Export-Csv -LiteralPath (Join-Path $snapshot 'manifest.csv') -NoTypeInformation
$build = [ordered]@{}
foreach ($binary in @('KharvoxLayer.dll','KharvoxLauncher.exe')) {
    $binaryPath = Join-Path $Runtime $binary
    if (Test-Path -LiteralPath $binaryPath) {
        $build[$binary] = [pscustomobject]@{Version=(Get-Item -LiteralPath $binaryPath).VersionInfo.FileVersion;SHA256=(Get-FileHash -LiteralPath $binaryPath).Hash.ToLowerInvariant()}
    }
}
$runtimeSelection = @()
foreach ($key in @('HKLM:\SOFTWARE\Khronos\OpenXR\1','HKCU:\SOFTWARE\Khronos\OpenXR\1')) {
    if (Test-Path -LiteralPath $key) {
        $value = (Get-ItemProperty -LiteralPath $key).ActiveRuntime
        $runtimeSelection += [pscustomobject]@{RegistryKey=$key;ActiveRuntime=$value}
    }
}
[pscustomobject]@{
    CapturedAt=(Get-Date).ToString('o');Runtime=$Runtime;Processes=@($active | Select-Object ProcessName,Id);
    OpenXR=$runtimeSelection;Files=$manifest.Count;Build=$build;
    Consistency= $(if($active.Count){'Live snapshot; repeat after exit for complete final files'}else{'Game and launcher stopped'})
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $snapshot 'context.json') -Encoding UTF8
if ($Action -eq 'Save') {
    [pscustomobject]@{Action='Save';Evidence=$snapshot;Files=$manifest.Count} | ConvertTo-Json
    return
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedArchiveHash) {
    throw 'r139 archive differs from the accepted build. Existing evidence has been saved; no test prepared.'
}
$rar = 'C:\Program Files\WinRAR\Rar.exe'
if (!(Test-Path -LiteralPath $rar)) { throw 'The existing WinRAR packager is required.' }
$session = Join-Path $EvidenceRoot "runs\$id"
New-Item -ItemType Directory -Path $session | Out-Null
& $rar x -idq $archive ($session + '\')
if ($LASTEXITCODE -ne 0) { throw 'Package extraction failed. Do not start this session.' }
$testRuntime = Join-Path $session $release
$verified = 0
foreach ($entry in Get-Content -LiteralPath (Join-Path $testRuntime 'SHA256SUMS.txt')) {
    if ($entry -notmatch '^([a-f0-9]{64})  (.+)$') { throw 'Invalid package manifest.' }
    $expected = $Matches[1]
    $target = [IO.Path]::GetFullPath((Join-Path $testRuntime $Matches[2]))
    if (!$target.StartsWith($testRuntime + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escapes package.' }
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Package payload hash mismatch.' }
    $verified++
}
if ($verified -ne 40 -or @(Get-ChildItem -LiteralPath $testRuntime -Recurse -File).Count -ne 41) { throw 'Unexpected package file count.' }
# Opt in only in this new session; the immutable release and prior captures stay intact.
if ($Capture -ne 'None') {
    $marker = if ($Capture -eq 'Once') {'capture_native_stereo_once'} else {'capture_native_stereo_sequence'}
    New-Item -ItemType File -Path (Join-Path $testRuntime $marker) | Out-Null
}
$result = [pscustomobject]@{
    Action='Prepare';Release=$release;SourceCommit='e0b3767ea529f3145848d6fee7c1d1627c86db39';
    Runtime=$testRuntime;Launcher=(Join-Path $testRuntime 'KharvoxLauncher.exe');Capture=$Capture;
    EvidenceBeforeTest=$snapshot;VerifiedPayloadFiles=$verified;ArchiveSHA256=$expectedArchiveHash;
    Started=$false;RuntimeSelectionChanged=$false;SavedSettingsChanged=$false
}
$result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $session 'PREPARATION.json') -Encoding UTF8
$result | ConvertTo-Json
