param(
    [Parameter(Mandatory=$true)][string]$NativeOutput,
    [Parameter(Mandatory=$true)][string]$LauncherOutput,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][string]$SourceCommit,
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent)
)
$ErrorActionPreference = 'Stop'
$release = 'KHARVOX-v0.8-beta'
if ($SourceCommit -notmatch '^[0-9a-f]{40}$') { throw 'A full source commit is required' }
$package = Join-Path $OutputRoot $release
$archive = Join-Path $OutputRoot ($release + '.zip')
if ((Test-Path -LiteralPath $package) -or (Test-Path -LiteralPath $archive)) {
    throw 'Release already exists; packages are immutable. Choose a new version.'
}
$nativeFiles = @(
    'KharvoxLayer.dll','KharvoxLayer.json','openxr_loader.dll','KharvoxIntro.exe',
    'KharvoxBhapticsBridge.exe','bhaptics_library.dll','KharvoxPsvr2Bridge.exe','psvr2_toolkit_capi_loader.dll',
    'Fsr1Easu.spv','Fsr1Rcas.spv','HandPbr.vert.spv','HandPbr.frag.spv',
    'hand_models.cfg','hand_models_calibration_default.cfg',
    'hud_flat_calibration_default.cfg','hud_profile_calibration_default.cfg','hud_quad_scale_default.cfg',
    'two_hand_weapon_profiles_default.cfg','weapon_pitch_default.cfg',
    'weapon_roll_default.cfg','weapon_yaw_default.cfg',
    'assets/3D/DOOM_LEFT_HAND_FIST.glb','assets/3D/DOOM_LEFT_HAND_GUN.glb',
    'assets/3D/DOOM_RIGHT_HAND_FIST.glb','assets/3D/DOOM_RIGHT_HAND_GUN.glb'
)
$sourceFiles = [ordered]@{
    'assets/branding/quest-controller-mapping.png'='assets/branding/quest-controller-mapping.png'
}
# Validate inputs before creating a new package. No game DLLs,
# transient status, personal settings or Native backend activation markers.
# Validated tuning defaults are built into the renderer. AER stays default.
& (Join-Path $PSScriptRoot 'verify_package_integrations.ps1') -Package $NativeOutput
foreach ($name in $nativeFiles) { if (!(Test-Path -LiteralPath (Join-Path $NativeOutput $name))) { throw "Missing native file: $name" } }
foreach ($name in @('KharvoxLauncher.exe','KharvoxLauncher.exe.config')) { if (!(Test-Path -LiteralPath (Join-Path $LauncherOutput $name))) { throw "Missing launcher file: $name" } }
foreach ($name in $sourceFiles.Keys) { if (!(Test-Path -LiteralPath (Join-Path $SourceRoot $name))) { throw "Missing source file: $name" } }
foreach ($binary in @((Join-Path $NativeOutput 'KharvoxLayer.dll'),(Join-Path $LauncherOutput 'KharvoxLauncher.exe'))) {
    if ((Get-Item -LiteralPath $binary).VersionInfo.FileVersion -ne '0.8.0.323') { throw "Incorrect file version: $binary" }
}
$layer = Get-Content -Raw -LiteralPath (Join-Path $NativeOutput 'KharvoxLayer.json') | ConvertFrom-Json
if ($layer.layer.implementation_version -ne '323') { throw 'Incorrect layer manifest version' }
$launcherVersion = (Get-Item -LiteralPath (Join-Path $LauncherOutput 'KharvoxLauncher.exe')).VersionInfo.ProductVersion
if (!$launcherVersion.Contains($SourceCommit)) { throw 'Launcher source revision does not match package source commit' }
$evidence = Join-Path $SourceRoot "out/$release"
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
New-Item -ItemType Directory -Path $package | Out-Null
foreach ($name in $nativeFiles) {
    $destination = Join-Path $package $name
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath (Join-Path $NativeOutput $name) -Destination $destination
}
foreach ($name in @('KharvoxLauncher.exe','KharvoxLauncher.exe.config')) { Copy-Item -LiteralPath (Join-Path $LauncherOutput $name) -Destination $package }
foreach ($name in $sourceFiles.Keys) {
    $destination=Join-Path $package $sourceFiles[$name]
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath (Join-Path $SourceRoot $name) -Destination $destination
}
@("Release: $release", "Source commit: $SourceCommit", 'Local test build; not published', 'Renderers: AER; Native Stereo Experimental', 'Default backend: AER', '0.8 Beta: PR #1 motion wheel with tracking recovery, stick priority and unified controller haptics; launcher opt-in', 'Approved 0.7 HUD/hand defaults, AMD fixes, VR intro and bundled integrations retained') | Set-Content -Encoding utf8 -LiteralPath (Join-Path $evidence 'SOURCE_REVISION.txt')

& (Join-Path $PSScriptRoot 'verify_package_integrations.ps1') -Package $package
& (Join-Path $PSScriptRoot 'test_vr_intro_host.ps1') -Package $package
$selfTest = Start-Process -FilePath (Join-Path $package 'KharvoxLauncher.exe') -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
if ($selfTest.ExitCode -ne 0) { throw "Packaged launcher self-test failed: $($selfTest.ExitCode)" }
@('0.8 Beta package assembly verified', 'Renderers: AER; Native Stereo Experimental', 'Launcher self-test: passed (exit 0)', 'Layer/launcher file version: 0.8.0.323; manifest: 323', 'All explicitly required runtime files present; hashes in SHA256SUMS.txt', 'bHaptics SDK and bridge + PSVR2 Toolkit loader and bridge: included; x64 and approved DLL hashes verified', "Source commit: $SourceCommit") | Set-Content -Encoding utf8 -LiteralPath (Join-Path $evidence 'PACKAGE-VALIDATION.txt')
$forbidden = Get-ChildItem -LiteralPath $package -Recurse | Where-Object {
    $_.Name -like 'native_test_*' -or $_.Name -eq 'validation' -or $_.Name -match '(?i)readme|analysis|analyze_pose' -or $_.Extension -in @('.md','.pdb','.cmd','.bat','.py') -or $_.Name -match '(?i)license|notices'
}
if ($forbidden) { throw 'Developer files, start scripts and standalone licenses are forbidden in tester packages' }
$hashes = Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($package.Length+1).Replace('\','/')
    (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant() + '  ' + $relative
}
$hashes | Set-Content -Encoding ascii -LiteralPath (Join-Path $evidence 'SHA256SUMS.txt')
Compress-Archive -LiteralPath $package -DestinationPath $archive -CompressionLevel Optimal
$zipCheck = Join-Path $evidence ('zip-check-' + [Guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $archive -DestinationPath $zipCheck
foreach ($file in (Get-ChildItem -LiteralPath $package -File -Recurse)) {
    $relative = $file.FullName.Substring($package.Length+1)
    $extracted = Join-Path (Join-Path $zipCheck $release) $relative
    if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $extracted).Hash) { throw "ZIP mismatch: $relative" }
}
$archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
"$archiveHash  $release.zip" | Set-Content -Encoding ascii -LiteralPath ($archive + '.sha256')
Write-Output $archive
Write-Output "SHA256 $archiveHash"
