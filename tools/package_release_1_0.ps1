param(
    [Parameter(Mandatory=$true)][string]$BaselineArchive,
    [Parameter(Mandatory=$true)][string]$NativeOutput,
    [Parameter(Mandatory=$true)][string]$LauncherOutput,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [string]$PackageName = 'KHARVOX-1.0',
    [string]$LauncherFileVersion = '1.0.0.1000',
    [string]$NativeFileVersion = '1.0.0.1000',
    [string]$ProductVersion = '1.0.0',
    [string]$ReleaseNotes = 'Docs/RELEASE_1_0.md'
)
$ErrorActionPreference='Stop'
$source=Split-Path $PSScriptRoot -Parent
$package=Join-Path $OutputRoot $PackageName
$archive=Join-Path $OutputRoot ($PackageName+'.zip')
if((Test-Path -LiteralPath $package) -or (Test-Path -LiteralPath $archive)){throw 'Release output already exists; do not overwrite a delivered package.'}
Expand-Archive -LiteralPath $BaselineArchive -DestinationPath $package
# Inherit the verified integration binaries, never SDK stubs from test builds.
& "$PSScriptRoot/verify_package_integrations.ps1" -Package $package
foreach($name in @('enable_live_ammo_calibration','enable_xr_session','TEST-NOTES.txt','fsr1_status.txt','hand_calibration_status.txt','renderer_status.txt','two_hand_status.txt')){
    $path=Join-Path $package $name
    if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path}
}
foreach($name in @('KharvoxLayer.dll','native_sfs_build.txt')){Copy-Item -LiteralPath (Join-Path $NativeOutput $name) -Destination $package}
if(Test-Path -LiteralPath (Join-Path $NativeOutput 'KharvoxIntro.exe')){
    Copy-Item -LiteralPath (Join-Path $NativeOutput 'KharvoxIntro.exe') -Destination $package
}
foreach($name in @('KharvoxLauncher.exe','KharvoxLauncher.exe.config')){Copy-Item -LiteralPath (Join-Path $LauncherOutput $name) -Destination $package}
Copy-Item -LiteralPath "$source/src/vulkan/KharvoxLayer.json" -Destination $package
foreach($name in @('hand_models_calibration_default.cfg','offhand_hud_default.cfg')){
    Copy-Item -LiteralPath "$source/config/$name" -Destination $package
}
Copy-Item -LiteralPath "$source/config/offhand_hud_default.cfg" -Destination "$package/offhand_hud.cfg"
Copy-Item -LiteralPath "$source/README.md" -Destination $package
$notice=Get-Content -LiteralPath "$source/THIRD_PARTY_NOTICES.md" -Raw
$profile=(Get-Content -LiteralPath "$source/Docs/THIRD_PARTY_NOTICES.txt" -Raw).Split(@('SFS external shader-profile provenance'),[StringSplitOptions]::None)[1]
($notice.TrimEnd()+"`n`nSFS external shader-profile provenance"+$profile) | Set-Content -Encoding utf8 "$package/THIRD_PARTY_NOTICES.md"
Copy-Item -LiteralPath "$source/$ReleaseNotes" -Destination "$package/RELEASE-NOTES.md"
foreach($name in @('KharvoxLayer.dll','KharvoxLauncher.exe')){
    $info=(Get-Item -LiteralPath "$package/$name").VersionInfo
    $expectedVersion=if($name -eq 'KharvoxLauncher.exe'){$LauncherFileVersion}else{$NativeFileVersion}
    if($info.FileVersion -ne $expectedVersion -or !$info.ProductVersion.StartsWith($ProductVersion)){throw "Wrong release version: $name"}
}
$stamp=Get-Content -LiteralPath "$package/native_sfs_build.txt"
$hash=(Get-FileHash -LiteralPath "$package/KharvoxLayer.dll" -Algorithm SHA256).Hash
if($stamp.Count -ne 2 -or $stamp[0] -ne 'KHARVOX_NATIVE_SFS_1' -or $stamp[1] -ne $hash){throw 'SFS capability stamp mismatch'}
foreach($name in @('hand_models_calibration_default.cfg','offhand_hud_default.cfg')){
    if((Get-FileHash "$package/$name").Hash -ne (Get-FileHash "$source/config/$name").Hash){throw "Wrong calibration: $name"}
}
& "$PSScriptRoot/verify_package_integrations.ps1" -Package $package
$self=Start-Process -FilePath "$package/KharvoxLauncher.exe" -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
if($self.ExitCode -ne 0){throw 'Packaged launcher self-test failed'}
$revision=git -C $source rev-parse HEAD
@($PackageName,"Source commit: $revision",'Based on Test38 with accepted Normal/Left HUD and hand calibration defaults.',"KharvoxLayer.dll SHA256: $hash",'Release versions, default configuration hashes, launcher self-test and bundled integrations verified.','Local release; not uploaded.') | Set-Content -Encoding utf8 "$package/BUILD.txt"
Compress-Archive -Path "$package/*" -DestinationPath $archive -CompressionLevel Optimal
Write-Output "Release: $archive"
