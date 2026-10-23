# Builds build\bench\solvebench.exe (tools/solvebench.cpp: the exact solvers timed on recorded solves). Its own folder:
# next to build\dbghelp.dll (this mod) it would load that instead of the system's dbghelp.
#   .\tools\solvebench.ps1 [-Run <bperf_paths.bin>] [-Repeats 5]
param([string]$Run = '', [int]$Repeats = 5)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot\paths.ps1"
$vcvars = Get-VcVars
$out = "$root\build\bench"
New-Item -ItemType Directory -Force $out | Out-Null
$cl = "cl /nologo /O2 /Zi /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS /Fo`"$out\\`" /Fd`"$out\solvebench_c.pdb`" " +
      "/Fe`"$out\solvebench.exe`" `"$root\tools\solvebench.cpp`" /link /DEBUG /PDB:`"$out\solvebench.pdb`""
cmd /c "`"$vcvars`" >nul && $cl" | Where-Object { $_ -notmatch '^solvebench\.cpp$' }
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
if ($Run) { & "$out\solvebench.exe" $Run $Repeats }
