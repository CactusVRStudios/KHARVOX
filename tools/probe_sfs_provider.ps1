param(
    [Parameter(Mandatory=$true)][string]$GameExe,
    [Parameter(Mandatory=$true)][string]$ProviderRoot,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(5,60)][int]$Seconds = 45
)
$ErrorActionPreference = 'Stop'
$GameExe = (Resolve-Path -LiteralPath $GameExe).Path
$ProviderRoot = (Resolve-Path -LiteralPath $ProviderRoot).Path
if ((Split-Path $GameExe -Leaf) -ne 'DOOMx64vk.exe') { throw 'This probe only supports DOOM (2016) Vulkan.' }
if (Get-Process DOOMx64vk -ErrorAction SilentlyContinue) { throw 'Close the existing DOOM process first.' }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a new evidence directory.' }
$driver = Join-Path $ProviderRoot 'Vk3DVision'
if (!(Test-Path -LiteralPath (Join-Path $driver 'Vk3DVision64.dll'))) { throw 'Prepare the local provider first.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$settings = @{
    Vk3DVision=$ProviderRoot
    VK_LAYER_PATH=$driver
    VK_INSTANCE_LAYERS='VK_LAYER_Vk3DVision'
    VK_LOADER_DEBUG='error,warn,layer'
    KHARVOX_DISABLE_LAYER='1'
    DISABLE_VK_LAYER_VALVE_steam_overlay_1='1'
    DISABLE_VK_LAYER_VALVE_steam_fossilize_1='1'
    DISABLE_VULKAN_OBS_CAPTURE='1'
    SteamAppId='379720'
}
$previous = @{}
$process = $null
try {
    foreach ($name in $settings.Keys) {
        $previous[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
        [Environment]::SetEnvironmentVariable($name,$settings[$name],'Process')
    }
    $process = Start-Process -FilePath $GameExe -WorkingDirectory (Split-Path $GameExe) -WindowStyle Hidden -PassThru `
        -ArgumentList '+r_renderAPI 1 +r_enableAsyncCompute 0 +com_skipIntroVideo 1 +r_fullscreen 0 +r_mode 19 +r_windowWidth 960 +r_windowHeight 540' `
        -RedirectStandardOutput (Join-Path $OutputDirectory 'stdout.log') -RedirectStandardError (Join-Path $OutputDirectory 'loader.log')
    $ended = $process.WaitForExit($Seconds*1000)
    $result = [ordered]@{
        ProcessId=$process.Id; ProviderOnly=$true; NaturalExit=$ended
        ExitCode=$(if($ended){$process.ExitCode}else{$null})
        TimedOut=(-not $ended); ObservationSeconds=$Seconds
        ProviderSha256=(Get-FileHash -LiteralPath (Join-Path $driver 'Vk3DVision64.dll')).Hash
        Note='Process survival is not proof of stereo rendering, gameplay, or VR compatibility.'
    }
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json')
    $result | ConvertTo-Json
} finally {
    # Own process object only; never terminate another game by process name.
    if ($process) {
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name,$previous[$name],'Process') }
    $providerLog = Join-Path $driver 'Vk3DVision64.log'
    if(Test-Path -LiteralPath $providerLog) { Copy-Item -LiteralPath $providerLog -Destination (Join-Path $OutputDirectory 'provider.log') }
}
