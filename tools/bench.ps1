# Compare configurations on identical work: the multiplayer mod's harness plays a save in idle mode (deterministic -
# every run simulates the same ticks) and this mod logs ms per 1000 in-game ticks ("bench" lines in bperf.log).
#   .\tools\bench.ps1 -Save BigCity -Ticks 7000 -Repeat 2 -Configs 'off=bperf_nopath.txt,bperf_nopick.txt', 'on='
# A config is name=flag files (comma-separated, file or file#content) created in the game folder for its runs. Runs alternate between the
# configs. Needs the multiplayer mod's dinput8.dll (it loads the save and exits at the tick limit).
param(
    [string]$Save = 'BigCity',
    [int]$Ticks = 7000,
    [int]$Speed = 10,
    [int]$Repeat = 2,
    [int]$SkipTicks = 1000,  # leave out the first ranges (loading, caches warming up)
    [string[]]$Configs = @('off=bperf_nopath.txt,bperf_nopick.txt', 'on='),
    [int]$TimeoutMin = 6
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\paths.ps1"
$game = Get-BanishedDir
$exeName = 'Application-steam-x64'
$log = "$game\bperf.log"
$results = [ordered]@{}

function Invoke-Bench([string]$name, [string[]]$flags) {
    if (Get-GameRunning) { throw "Banished is already running - close it first" }
    Set-ItemProperty 'HKCU:\Software\Shining Rock Software LLC\Banished' -Name VideoSettings -Value 'true'
    $cfg = "$game\bmp_autotest.txt"
    "save=$Save`nmode=idle`nticks=$Ticks`nspeed=$Speed`nseed=1`nspeedchaos=0" | Set-Content $cfg -Encoding ascii
    # a flag is file or file#content (e.g. bperf_delaypaths.txt#2)
    $made = @("$game\bmp_nologs.txt", "$game\bperf_bench.txt")
    $made | ForEach-Object { Set-Content $_ '1' }
    foreach ($f in $flags | Where-Object { $_ }) {
        $file, $content = $f -split '#', 2
        Set-Content "$game\$file" $(if ($content) { $content } else { '1' })
        $made += "$game\$file"
    }
    $before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
    $start = Get-Date
    try {
        Start-Process "$game\$exeName.exe" -WorkingDirectory $game | Out-Null
        $deadline = $start.AddMinutes($TimeoutMin)
        while (-not (Get-GameRunning)) {
            if ((Get-Date) -gt $deadline) { throw "game never started" }
            Start-Sleep -Milliseconds 500
        }
        # less noise from whatever else runs on this PC
        Get-Process $exeName -ErrorAction SilentlyContinue | ForEach-Object { try { $_.PriorityClass = 'High' } catch {} }
        $gone = 0
        while ($gone -lt 3) {
            if (Get-GameRunning) { $gone = 0 } else { $gone++ }
            if ((Get-Date) -gt $deadline) { Stop-Process -Name $exeName -Force; throw "timed out" }
            Start-Sleep -Seconds 2
        }
    } finally {
        Remove-Item (@($cfg) + $made) -ErrorAction SilentlyContinue
    }
    $ms = @(); $mc = @()
    foreach ($l in (Get-Content $log | Select-Object -Skip $before)) {
        if ($l -match 'bench: ticks (\d+)-(\d+) ([\d.]+) ms/tick(?: ([\d.]+) Mcycles/tick)?' -and [int]$matches[1] -ge $SkipTicks) {
            $ms += [double]$matches[3]
            if ($matches[4]) { $mc += [double]$matches[4] }
        }
    }
    if (-not $ms) { throw "no bench lines - see $log" }
    Write-Host ("{0,-10} {1:N3} ms/tick  {2:N3} Mcyc/tick (main thread)   ranges [{3}]" -f $name, (Median $ms), (Median $mc),
                (($ms | ForEach-Object { $_.ToString('N3') }) -join ' '))
    if (-not $results.Contains($name)) { $results[$name] = @{ ms = @(); mc = @() } }
    $results[$name].ms += $ms
    $results[$name].mc += $mc
}
function Median([double[]]$v) {
    if (-not $v) { return 0 }
    $s = $v | Sort-Object
    if ($s.Count % 2) { return $s[[int][math]::Floor($s.Count / 2)] }
    return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}

for ($r = 0; $r -lt $Repeat; $r++) {
    foreach ($c in $Configs) {
        $name, $files = $c -split '=', 2
        try {
            Invoke-Bench $name ($files -split ',')
        } catch {  # the harness occasionally sticks at the main menu: kill it and try once more
            Write-Host "$name : $($_.Exception.Message) - retrying once" -ForegroundColor Yellow
            Stop-Process -Name $exeName -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 5
            Invoke-Bench $name ($files -split ',')
        }
    }
}
Write-Host "`nsummary (median of every 1000-tick range of every run; sim speed at 1x = 60 ticks/s):"
$first = $results.Keys | Select-Object -First 1
$base = Median $results[$first].ms
$baseC = Median $results[$first].mc
foreach ($k in $results.Keys) {
    $m = Median $results[$k].ms
    $c = Median $results[$k].mc
    Write-Host ("{0,-10} {1:N3} ms/tick ({2:N0} ticks/s max) {3:+0.0;-0.0}%   main thread {4:N3} Mcyc/tick {5:+0.0;-0.0}%   vs {6}" -f
                $k, $m, (1000 / $m), (100 * ($base - $m) / $base), $c, $(if ($baseC) { 100 * ($baseC - $c) / $baseC } else { 0 }), $first)
}
