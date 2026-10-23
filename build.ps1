# Builds build\dbghelp.dll (MSVC x64). Usage: .\build.ps1 [-Deploy]
#   -Deploy copies it into the Banished folder (delete <Banished>\dbghelp.dll to uninstall).
param([switch]$Deploy)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. "$root\tools\paths.ps1"
$vcvars = Get-VcVars
New-Item -ItemType Directory -Force "$root\build" | Out-Null

$srcs = @(
    "$root\src\proxy.cpp", "$root\src\profiler.cpp", "$root\src\pathfast.cpp", "$root\src\parmove.cpp", "$root\src\movefast.cpp", "$root\src\stresstown.cpp", "$root\src\loopfast.cpp",
    "$root\third_party\minhook\src\hook.c", "$root\third_party\minhook\src\buffer.c",
    "$root\third_party\minhook\src\trampoline.c", "$root\third_party\minhook\src\hde\hde64.c"
) | ForEach-Object { "`"$_`"" }

$cl = "cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /I`"$root\third_party\minhook\include`" " +
      "/Fo`"$root\build\\`" /Fd`"$root\build\\`" /Fe`"$root\build\dbghelp.dll`" $($srcs -join ' ') " +
      "/link /DEF:`"$root\src\perf.def`" /DEBUG /PDB:`"$root\build\dbghelp.pdb`" user32.lib winmm.lib"
cmd /c "`"$vcvars`" >nul && $cl"
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

if ($Deploy) {
    $game = Get-BanishedDir
    Copy-Item "$root\build\dbghelp.dll" "$game\dbghelp.dll" -Force
    Write-Host "deployed to $game\dbghelp.dll"
}
