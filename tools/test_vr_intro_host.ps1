param([Parameter(Mandatory=$true)][string]$Package)
$ErrorActionPreference = 'Stop'
# Exercise the actual launcher's CLR import before explicitly loading the layer.
# The invalid token exits before OpenXR initialization, so no headset is needed.
$launcher = [Reflection.Assembly]::LoadFrom((Join-Path ([IO.Path]::GetFullPath($Package)) 'KharvoxLauncher.exe'))
$hostType = $launcher.GetType('KharvoxLauncher.VrIntroHost', $true)
$nativeCall = $hostType.GetMethod('RunIntro', [Reflection.BindingFlags]'Static,NonPublic')
if ($null -eq $nativeCall -or $nativeCall.Invoke($null, [object[]]@('invalid-token', [uint32]0)) -ne 1) {
    throw 'Launcher native import/argument validation failed'
}
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class IntroHostCheck {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern IntPtr LoadLibraryEx(string path,IntPtr file,uint flags);
 [DllImport("kernel32.dll",CharSet=CharSet.Ansi,ExactSpelling=true)] public static extern IntPtr GetProcAddress(IntPtr module,string name);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindResource(IntPtr module,IntPtr name,IntPtr type);
 [DllImport("kernel32.dll")] public static extern uint SizeofResource(IntPtr module,IntPtr resource);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl,CharSet=CharSet.Unicode)] public delegate int Run([MarshalAs(UnmanagedType.LPWStr)] string token,uint pid);
}
'@
$module = [IntroHostCheck]::LoadLibraryEx((Join-Path ([IO.Path]::GetFullPath($Package)) 'KharvoxLayer.dll'),[IntPtr]::Zero,0x1100)
if ($module -eq [IntPtr]::Zero) { throw 'Cannot load packaged native layer' }
$entry = [IntroHostCheck]::GetProcAddress($module,'KharvoxRunVrIntro')
if ($entry -eq [IntPtr]::Zero) { throw 'Integrated VR intro export missing' }
foreach ($id in 101,102,103,104) {
    $resource = [IntroHostCheck]::FindResource($module,[IntPtr]$id,[IntPtr]10)
    if ($resource -eq [IntPtr]::Zero -or [IntroHostCheck]::SizeofResource($module,$resource) -eq 0) { throw "Embedded intro asset missing: $id" }
}
$run = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($entry,[IntroHostCheck+Run])
if ($run.Invoke('invalid-token',0) -ne 1) { throw 'Native intro ABI/argument validation failed' }
if (Test-Path -LiteralPath (Join-Path $Package 'KharvoxGameIntro.exe')) { throw 'Standalone intro EXE is still packaged' }
if (!(Test-Path -LiteralPath (Join-Path $Package 'KharvoxIntro.exe'))) { throw 'Standalone KharvoxIntro.exe is missing' }
Write-Output 'PASS: integrated native entry point, all embedded assets and ABI; standalone KharvoxIntro.exe included'
