param(
    [Parameter(Mandatory=$true)][string]$BaselineArchive,
    [Parameter(Mandatory=$true)][string]$NativeOutput,
    [Parameter(Mandatory=$true)][string]$LauncherOutput,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [ValidateSet('1.4a','1.4b','1.5')][string]$Version = '1.4a'
)
$ErrorActionPreference='Stop'
$source=Split-Path $PSScriptRoot -Parent
$package=Join-Path $OutputRoot "KHARVOX-v$Version"
$fileVersion=@{'1.4a'='1.4.1.1410';'1.4b'='1.4.2.1420';'1.5'='1.5.0.1510'}[$Version]
$archive=$package+'.zip'
if((Test-Path -LiteralPath $package) -or (Test-Path -LiteralPath $archive)){throw 'Output already exists'}
Expand-Archive -LiteralPath $BaselineArchive -DestinationPath $package
& "$PSScriptRoot/verify_package_integrations.ps1" -Package $package
$nativeFiles=@('KharvoxLayer.dll','native_sfs_build.txt')
if($Version -eq '1.4b'){$nativeFiles+=@('KharvoxBhapticsBridge.exe','bhaptics_library.dll')}
foreach($name in $nativeFiles){
    Copy-Item -LiteralPath (Join-Path $NativeOutput $name) -Destination $package
}
foreach($name in @('KharvoxLauncher.exe','KharvoxLauncher.exe.config')){
    Copy-Item -LiteralPath (Join-Path $LauncherOutput $name) -Destination $package
}
Copy-Item -LiteralPath "$source/src/vulkan/KharvoxLayer.json" -Destination $package
foreach($name in @('KharvoxLayer.dll','KharvoxLauncher.exe')){
    if((Get-Item -LiteralPath "$package/$name").VersionInfo.FileVersion -ne $fileVersion){throw "Wrong version: $name"}
}
$stamp=Get-Content -LiteralPath "$package/native_sfs_build.txt"
if($stamp.Count -ne 2 -or $stamp[0] -ne 'KHARVOX_NATIVE_SFS_1' -or
    $stamp[1] -ne (Get-FileHash -LiteralPath "$package/KharvoxLayer.dll").Hash){throw 'Invalid SFS capability stamp'}
if($Version -eq '1.5'){
    # These notices are now resources in the launcher's license viewer.
    $notices=@('THIRD_PARTY_NOTICES.md','sfs-compiler-licenses/README.md','sfs-compiler-licenses/glslang.txt',
        'sfs-compiler-licenses/SPIRV-Cross.txt','sfs-compiler-licenses/SPIRV-Tools.txt')
    foreach($name in $notices){
        $path=Join-Path $package $name
        if(Test-Path -LiteralPath $path -PathType Leaf){Remove-Item -LiteralPath $path}
    }
    $compilerNotices=Join-Path $package 'sfs-compiler-licenses'
    if(Test-Path -LiteralPath $compilerNotices){[IO.Directory]::Delete((Resolve-Path -LiteralPath $compilerNotices).Path, $false)}
}
$self=Start-Process -FilePath "$package/KharvoxLauncher.exe" -ArgumentList '--self-test' -WorkingDirectory $package -WindowStyle Hidden -Wait -PassThru
if($self.ExitCode -ne 0){throw 'Packaged launcher self-test failed'}
foreach($name in @('fsr1_status.txt','hand_calibration_status.txt','renderer_status.txt','two_hand_status.txt')){
    $path=Join-Path $package $name
    if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path}
}
& "$PSScriptRoot/verify_package_integrations.ps1" -Package $package
Compress-Archive -Path "$package/*" -DestinationPath $archive -CompressionLevel Optimal
Write-Output "Release: $archive"
