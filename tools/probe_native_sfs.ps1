param(
    [Parameter(Mandatory=$true)][string]$Runtime,
    [Parameter(Mandatory=$true)][string]$Game,
    [Parameter(Mandatory=$true)][string]$Profile,
    [string]$OpenXrManifest,
    [ValidateRange(30,1800)][int]$Seconds=300,
    [switch]$CaptureEyes
)
$ErrorActionPreference='Stop'
# Local developer prototype, not the external Vk3DVision binary probe.
# Runtime must contain a layer built with KHARVOX_BUILD_SFS_COMPILER=ON.
$Runtime=(Resolve-Path -LiteralPath $Runtime).Path
$Game=(Resolve-Path -LiteralPath $Game).Path
$Profile=(Resolve-Path -LiteralPath $Profile).Path
if(Get-Process DOOMx64vk -ErrorAction SilentlyContinue){throw 'Close the existing DOOM process before starting a bounded SFS test.'}
& (Join-Path $PSScriptRoot 'verify_package_integrations.ps1') -Package $Runtime
foreach($name in @('KharvoxLayer.dll','KharvoxLayer.json','openxr_loader.dll','enable_xr_session')){
    if(!(Test-Path -LiteralPath (Join-Path $Runtime $name) -PathType Leaf)){throw "Missing native test runtime file: $name"}
}
if(!(Test-Path -LiteralPath (Join-Path $Runtime 'sfs-compiler-licenses') -PathType Container)){throw 'Bundle the SFS compiler licenses with this optional build.'}
$stamp=Join-Path $Runtime 'native_sfs_build.txt'
if(!(Test-Path -LiteralPath $stamp -PathType Leaf)){throw 'Native SFS build manifest is missing; use a compiler-enabled build.'}
$capability=Get-Content -LiteralPath $stamp
if($capability.Count -ne 2 -or $capability[0] -ne 'KHARVOX_NATIVE_SFS_1' -or
    $capability[1] -ne (Get-FileHash -LiteralPath (Join-Path $Runtime 'KharvoxLayer.dll') -Algorithm SHA256).Hash){
    throw 'Native SFS manifest does not match the runtime DLL.'
}
if(!(Get-ChildItem -LiteralPath $Profile -Filter '*.spv' -File | Select-Object -First 1)){throw 'Profile must contain locally compiled SPIR-V replacements.'}
$settings=@{
    VK_LAYER_PATH=$Runtime; VK_INSTANCE_LAYERS='VK_LAYER_KHARVOX_OPENXR';
    KHARVOX_ENABLE_LAYER='1'; KHARVOX_SFS_NATIVE_PROBE='1'; KHARVOX_SFS_NATIVE_VR='1';
    KHARVOX_SFS_PROFILE=$Profile; KHARVOX_EXTENDED_LOGGING='1'; KHARVOX_RENDER_SCALE='0.5';
    KHARVOX_CAPTURE_EYES=$(if($CaptureEyes){'1'}else{'0'});
    KHARVOX_SFS_CAPTURE_ONCE=$(if($CaptureEyes){'1'}else{'0'});
    DISABLE_VK_LAYER_VALVE_steam_overlay_1='1'; DISABLE_VK_LAYER_VALVE_steam_fossilize_1='1';
    DISABLE_VULKAN_OBS_CAPTURE='1'; SteamAppId='379720'
}
if($OpenXrManifest){$settings.XR_RUNTIME_JSON=(Resolve-Path -LiteralPath $OpenXrManifest).Path}
$saved=@{}
$owned=$null
try{
    foreach($key in $settings.Keys){
        $saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
        [Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')
    }
    $owned=Start-Process -FilePath $Game -WorkingDirectory (Split-Path $Game) -WindowStyle Hidden -PassThru -ArgumentList '+com_skipKeyPressOnLoadScreens 1 +r_renderAPI 1 +com_skipIntroVideo 1 +r_fullscreen 0 +r_mode 19 +r_windowWidth 960 +r_windowHeight 540'
    Write-Output "Native Vulkan SFS test PID $($owned.Id); deadline $Seconds seconds. Load the campaign with Enter, then Space."
    if($owned.WaitForExit($Seconds*1000)){Write-Output "Game exit code: $($owned.ExitCode)"}
    else{Write-Output "Test deadline reached; stopping only owned PID $($owned.Id)."; $owned.Kill(); $owned.WaitForExit()}
}finally{
    if($owned -and !$owned.HasExited){$owned.Kill(); $owned.WaitForExit()}
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
}
