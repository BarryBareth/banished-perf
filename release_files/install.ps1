# Banished Perf installer (Install.bat / Uninstall.bat run this).
#   dbghelp.dll -> the Banished folder (next to Application-steam-x64.exe)
# Installs into every Steam Banished it finds, or only into the folder this zip was unpacked into.
# A dbghelp.dll from another mod is kept as dbghelp.dll.bperf-backup and put back by Uninstall.
# Only the Steam build is supported: on the GOG build the mod would load but do nothing, so it isn't installed there.
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$exe = 'Application-steam-x64.exe'

function Is-Game([string]$dir) { return $dir -and (Test-Path (Join-Path $dir $exe)) }
function Is-Gog([string]$dir) { return $dir -and (Test-Path (Join-Path $dir 'Application-x64.exe')) -and -not (Is-Game $dir) }

function Find-Banished {
    if (Is-Game $here) { return @($here) }                       # unpacked straight into the game folder
    if (Is-Game (Split-Path $here)) { return @(Split-Path $here) }
    if ((Is-Gog $here) -or (Is-Gog (Split-Path $here))) { throw 'This is the GOG version of Banished. Banished Perf only supports the Steam version for now.' }
    $found = @()
    $libs = @()
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libs += $steam
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            $libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
        }
    }
    foreach ($l in $libs | Select-Object -Unique) {
        $g = Join-Path $l 'steamapps\common\Banished'
        if (Is-Game $g) { $found += (Resolve-Path $g).Path }
    }
    if ($found) { return @($found | Select-Object -Unique) }
    Add-Type -AssemblyName System.Windows.Forms
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = 'Where is Banished installed? (the folder with Application-steam-x64.exe)'
    if ($d.ShowDialog() -eq 'OK') {
        if (Is-Game $d.SelectedPath) { return @($d.SelectedPath) }
        if (Is-Gog $d.SelectedPath) { throw 'This is the GOG version of Banished. Banished Perf only supports the Steam version for now.' }
    }
    return @()
}

function Is-Ours([string]$dll) {  # our DLL carries the text "BanishedPerf" (UTF-16)
    $b = [IO.File]::ReadAllBytes($dll)
    return [Text.Encoding]::Unicode.GetString($b).Contains('BanishedPerf')
}

try {
    $games = @(Find-Banished)
    if (-not $games) { throw "Couldn't find Banished. Unpack this folder into the Banished folder (the one with Application-steam-x64.exe) and run it again." }
    if (Get-Process 'Application-steam-x64' -ErrorAction SilentlyContinue) { throw 'Banished is running - close it first, then run this again.' }
    if (-not $Uninstall -and -not (Test-Path (Join-Path $here 'dbghelp.dll'))) { throw 'dbghelp.dll is missing from this folder - unpack the whole zip first.' }
    foreach ($game in $games) {
        Write-Host "Banished folder: $game"
        $dll = Join-Path $game 'dbghelp.dll'
        $backup = Join-Path $game 'dbghelp.dll.bperf-backup'
        if ($Uninstall) {
            if ((Test-Path $dll) -and (Is-Ours $dll)) { Remove-Item $dll; Write-Host '  removed dbghelp.dll' }
            if (Test-Path $backup) { Move-Item $backup $dll; Write-Host '  put the other mod''s dbghelp.dll back' }
            continue
        }
        if ((Test-Path $dll) -and -not (Is-Ours $dll) -and -not (Test-Path $backup)) {
            Move-Item $dll $backup
            Write-Host '  another mod''s dbghelp.dll was there: kept as dbghelp.dll.bperf-backup (Uninstall puts it back)' -ForegroundColor Yellow
        }
        Copy-Item (Join-Path $here 'dbghelp.dll') $dll -Force
    }
    Write-Host ''
    if ($Uninstall) {
        Write-Host 'Banished Perf is uninstalled. (Its log, bperf*.log in the Banished folder, is left; delete it if you like.)' -ForegroundColor Green
    } else {
        Write-Host 'Banished Perf is installed. Nothing to set up: start Banished as usual.' -ForegroundColor Green
    }
} catch {
    Write-Host ''
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
