param(
    [Parameter(Mandatory=$true)][string]$ProviderRoot,
    [Parameter(Mandatory=$true)][string]$DoomProfile,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
# Local experiment only: do not redistribute the user's game shader collection.
# ProviderRoot contains Vk3DVision/, with the extracted original DLLs inside it.
if (Test-Path -LiteralPath $Destination) { throw 'Choose a new destination; existing profiles are never overwritten.' }
$driver = Join-Path $ProviderRoot 'Vk3DVision'
foreach ($name in @('Vk3DVision64.dll','Vk3DVision64.json','Spirv-Cross_V1.dll','Spirv-Cross_V2.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $driver $name))) { throw "Missing provider dependency: $name" }
}
$sourceIni = Join-Path $DoomProfile 'Vk3DVision.ini'
$text = [IO.File]::ReadAllText($sourceIni)
if ($text -notmatch '(?m)^SingleFrameStereo\s*=\s*true\s*$') { throw 'Expected the DOOM Single Frame Stereo profile.' }
New-Item -ItemType Directory -Force -Path (Join-Path $Destination 'Vk3DVision'),(Join-Path $Destination 'Profiles/DOOM') | Out-Null
foreach ($name in @('Vk3DVision64.dll','Vk3DVision64.json','Spirv-Cross_V1.dll','Spirv-Cross_V2.dll')) {
    Copy-Item -LiteralPath (Join-Path $driver $name) -Destination (Join-Path $Destination 'Vk3DVision')
}
Copy-Item -LiteralPath (Join-Path $DoomProfile 'ShaderSwap') -Destination (Join-Path $Destination 'Profiles/DOOM') -Recurse
$text = $text -replace '(?m)^Stereo3DViewMode\s*=.*$', 'Stereo3DViewMode = SBS_LEFT'
# A stereo texture array must not be substituted for the particle vector-field
# sampler3D. The supplied profile only repairs this helper in Fragment shaders.
# Additional injection slots were exercised with 4.25.5.608; retain the fragment rule.
$computeFix = @'
ShaderInjectionPoint11 = "vec4 tex3Dlod(sampler3D image, vec4 texcoord)\n{\n    return TEXTURE_LOD(image, texcoord.xyz, texcoord.w)"
ShaderStereoString11 = "vec4 tex3Dlod(sampler3D image, vec4 texcoord)\n{\n    return textureLod(image, texcoord.xyz, texcoord.w)"
ShaderOperation11 = replace
ShaderType11 = Compute

ShaderInjectionPoint12 = "vectorField.repeat = _678.particlevectorfields[_680].repeat;"
ShaderStereoString12 = "vectorField.repeat = (_678.particlevectorfields[_680].repeat != 0u);"
ShaderOperation12 = replace
ShaderType12 = Compute

'@
$text = $text.Replace('[Params]', $computeFix + "`r`n[Params]")
[IO.File]::WriteAllText((Join-Path $Destination 'Profiles/DOOM/Vk3DVision.ini'), $text)
$hashes = Get-ChildItem -LiteralPath (Join-Path $Destination 'Vk3DVision') -Filter *.dll | Get-FileHash -Algorithm SHA256 | Select-Object @{n='File';e={Split-Path $_.Path -Leaf}},Hash
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'provider-hashes.json')
Write-Output "Prepared local SFS provider: $Destination"
