# Packs a release: release\BanishedPerf-<version>.zip (dbghelp.dll, Install.bat, Uninstall.bat, install.ps1, README.txt).
#   .\tools\make_release.ps1 [-Force]
# The version is BPERF_VERSION in src\version.h: raise it for every release. Build first: .\build.ps1
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot

$version = [regex]::Match((Get-Content "$root\src\version.h" -Raw), 'BPERF_VERSION "([0-9.]+)"').Groups[1].Value
if (-not $version) { throw 'BPERF_VERSION not found in src\version.h' }

$dll = Get-Item "$root\build\dbghelp.dll"
$newestSrc = Get-ChildItem "$root\src" -File | Sort-Object LastWriteTime | Select-Object -Last 1
if ($dll.LastWriteTime -lt $newestSrc.LastWriteTime) { throw "build\dbghelp.dll is older than src\$($newestSrc.Name) - run build.ps1" }
if (-not [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($dll.FullName)).Contains('BanishedPerf')) {
    throw 'build\dbghelp.dll has no BanishedPerf marker - the installer would not recognise it'
}

$name = "BanishedPerf-$version"
$out = "$root\release"
$zip = "$out\$name.zip"
if ((Test-Path $zip) -and -not $Force) { throw "$zip exists - raise BPERF_VERSION in src\version.h for a new release (or -Force to rebuild this one)" }
$dir = "$out\$name"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $dll.FullName $dir
Copy-Item "$root\release_files\Install.bat", "$root\release_files\Uninstall.bat", "$root\release_files\install.ps1" $dir
(Get-Content "$root\release_files\README.txt" -Raw).Replace('{VERSION}', $version) | Set-Content "$dir\README.txt" -Encoding utf8
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$dir\*" -DestinationPath $zip
Remove-Item $dir -Recurse -Force
$h = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Write-Host "$zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB) sha256 $h"
