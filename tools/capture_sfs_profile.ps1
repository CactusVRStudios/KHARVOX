param(
    [Parameter(Mandatory=$true)][string]$Runtime,
    [Parameter(Mandatory=$true)][string]$Game,
    [Parameter(Mandatory=$true)][string]$Output,
    [ValidateRange(5,60)][int]$Seconds=30
)
$ErrorActionPreference='Stop'
$Runtime=(Resolve-Path -LiteralPath $Runtime).Path
$Game=(Resolve-Path -LiteralPath $Game).Path
if(Get-Process DOOMx64vk -ErrorAction SilentlyContinue){throw 'Close the running DOOM instance before capture.'}
& "$PSScriptRoot/verify_package_integrations.ps1" -Package $Runtime
if($LASTEXITCODE -ne 0){throw 'Runtime integrations failed verification.'}
$null=New-Item -ItemType Directory -Path $Output -Force
$Output=(Resolve-Path -LiteralPath $Output).Path
# Restore every process-local override, including when the game exits early.
$overrides=@{
    VK_LAYER_PATH=$Runtime;VK_INSTANCE_LAYERS='VK_LAYER_KHARVOX_OPENXR';
    KHARVOX_ENABLE_LAYER='1';KHARVOX_VULKAN_SFS=$null;
    KHARVOX_SFS_CAPTURE_DIRECTORY=$Output;
    DISABLE_VK_LAYER_VALVE_steam_overlay_1='1';
    DISABLE_VK_LAYER_VALVE_steam_fossilize_1='1';
    DISABLE_VULKAN_OBS_CAPTURE='1';SteamAppId='379720'
}
$previous=@{}
$process=$null
try {
    foreach($name in $overrides.Keys){$previous[$name]=[Environment]::GetEnvironmentVariable($name,'Process');[Environment]::SetEnvironmentVariable($name,$overrides[$name],'Process')}
    $process=Start-Process -FilePath $Game -WorkingDirectory (Split-Path -Parent $Game) -ArgumentList '+r_renderAPI 1 +r_enableAsyncCompute 0 +com_skipIntroVideo 1 +r_fullscreen 0 +r_mode 19 +r_windowWidth 960 +r_windowHeight 540' -WindowStyle Hidden -PassThru
    $exited=$process.WaitForExit($Seconds*1000)
    if($exited){Write-Output "Game exit: $($process.ExitCode)"}else{Write-Output "Capture deadline reached; stopping owned process $($process.Id)."}
} finally {
    if($process -and -not $process.HasExited){$process.Kill();$process.WaitForExit()}
    foreach($name in $previous.Keys){[Environment]::SetEnvironmentVariable($name,$previous[$name],'Process')}
}
