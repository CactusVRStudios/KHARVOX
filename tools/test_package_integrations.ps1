param([Parameter(Mandatory=$true)][string]$Package)
$ErrorActionPreference = 'Stop'
$verify = Join-Path $PSScriptRoot 'verify_package_integrations.ps1'
& $verify -Package $Package
$files = @('KharvoxBhapticsBridge.exe','bhaptics_library.dll','KharvoxPsvr2Bridge.exe','psvr2_toolkit_capi_loader.dll')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('kharvox-package-check-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    foreach ($missing in $files) {
        $directory = Join-Path $fixture $missing
        New-Item -ItemType Directory -Path $directory | Out-Null
        foreach ($name in $files) { if ($name -ne $missing) { Copy-Item -LiteralPath (Join-Path $Package $name) -Destination $directory } }
        $rejected = $false
        try { & $verify -Package $directory } catch {
            if (!$_.Exception.Message.Contains("Required integration file missing: $missing")) { throw }
            $rejected = $true
        }
        if (!$rejected) { throw "Package without $missing was accepted" }
    }
    foreach ($changed in @('bhaptics_library.dll','psvr2_toolkit_capi_loader.dll')) {
        $directory = Join-Path $fixture ('changed-' + $changed)
        New-Item -ItemType Directory -Path $directory | Out-Null
        foreach ($name in $files) { Copy-Item -LiteralPath (Join-Path $Package $name) -Destination $directory }
        $path = Join-Path $directory $changed
        $bytes = [IO.File]::ReadAllBytes($path);$bytes[$bytes.Length-1] = $bytes[$bytes.Length-1] -bxor 1
        [IO.File]::WriteAllBytes($path,$bytes)
        $rejected = $false
        try { & $verify -Package $directory } catch {
            if (!$_.Exception.Message.Contains('Unapproved integration DLL')) { throw }
            $rejected = $true
        }
        if (!$rejected) { throw "Modified $changed was accepted" }
    }
    Write-Output 'PASS: all four omissions and both modified DLLs rejected'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or !(Split-Path $resolved -Leaf).StartsWith('kharvox-package-check-')) { throw 'Unexpected fixture cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
