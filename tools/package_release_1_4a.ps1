param(
    [Parameter(Mandatory=$true)][string]$BaselineArchive,
    [Parameter(Mandatory=$true)][string]$NativeOutput,
    [Parameter(Mandatory=$true)][string]$LauncherOutput,
    [Parameter(Mandatory=$true)][string]$OutputRoot
)
$ErrorActionPreference='Stop'
$source=Split-Path $PSScriptRoot -Parent
$package=Join-Path $OutputRoot 'KHARVOX-v1.4a'
$archive=$package+'.zip'
if((Test-Path -LiteralPath $package) -or (Test-Path -LiteralPath $archive)){throw 'Output already exists'}
Expand-Archive -LiteralPath $BaselineArchive -DestinationPath $package
& "$PSScriptRoot/verify_package_integrations.ps1" -Package $package
foreach($name in @('KharvoxLayer.dll','native_sfs_build.txt')){
    Copy-Item -LiteralPath (Join-Path $NativeOutput $name) -Destination $package
}
foreach($name in @('KharvoxLauncher.exe','KharvoxLauncher.exe.config')){
    Copy-Item -LiteralPath (Join-Path $LauncherOutput $name) -Destination $package
}
Copy-Item -LiteralPath "$source/src/vulkan/KharvoxLayer.json" -Destination $package
foreach($name in @('KharvoxLayer.dll','KharvoxLauncher.exe')){
    if((Get-Item -LiteralPath "$package/$name").VersionInfo.FileVersion -ne '1.4.1.1410'){throw "Wrong version: $name"}
}
$stamp=Get-Content -LiteralPath "$package/native_sfs_build.txt"
if($stamp.Count -ne 2 -or $stamp[0] -ne 'KHARVOX_NATIVE_SFS_1' -or
    $stamp[1] -ne (Get-FileHash -LiteralPath "$package/KharvoxLayer.dll").Hash){throw 'Invalid SFS capability stamp'}
$self=Start-Process -FilePath "$package/KharvoxLauncher.exe" -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
if($self.ExitCode -ne 0){throw 'Packaged launcher self-test failed'}
foreach($name in @('fsr1_status.txt','hand_calibration_status.txt','renderer_status.txt','two_hand_status.txt')){
    $path=Join-Path $package $name
    if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path}
}
& "$PSScriptRoot/verify_package_integrations.ps1" -Package $package
Compress-Archive -Path "$package/*" -DestinationPath $archive -CompressionLevel Optimal
Write-Output "Release: $archive"
