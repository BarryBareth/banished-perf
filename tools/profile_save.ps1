# Load a save through the multiplayer mod's autotest loader, let it run, take a bperf profile, close the game.
#   .\tools\profile_save.ps1 -Save BigCity -Speed 10 [-Seconds 30] [-Warmup 15]
# Needs the multiplayer mod's dinput8.dll in the game folder (it does the loading) and this mod's dbghelp.dll.
# Prints the report.py summary; the raw profile stays in the game folder.
param(
    [string]$Save = 'BigCity',
    [int]$Speed = 10,
    [int]$Seconds = 30,
    [int]$Warmup = 15,
    [int]$TimeoutMin = 8
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\paths.ps1"
$game = Get-BanishedDir
$exeName = 'Application-steam-x64'
if (Get-GameRunning) { throw "Banished is already running - close it first" }

Set-ItemProperty 'HKCU:\Software\Shining Rock Software LLC\Banished' -Name VideoSettings -Value 'true'
$cfg = "$game\bmp_autotest.txt"
"save=$Save`nmode=idle`nticks=100000000`nspeed=$Speed`nseed=1`nspeedchaos=0" | Set-Content $cfg -Encoding ascii
# the harness's per-tick/per-draw logs cost ~10% of a frame and are off in real games: keep them off here too
Set-Content "$game\bmp_nologs.txt" "1"
$bmpLog = "$game\bmp.log"
$start = Get-Date
try {
    Start-Process "$game\$exeName.exe" -WorkingDirectory $game | Out-Null
    $deadline = $start.AddMinutes($TimeoutMin)
    # the multiplayer mod logs "autotest: phase 2" once the loaded game is running
    while (-not ((Test-Path $bmpLog) -and (Get-Item $bmpLog).LastWriteTime -ge $start -and
                 (Select-String -Path $bmpLog -Pattern 'autotest: phase 2' -Quiet))) {
        if ((Get-Date) -gt $deadline) { throw "the save never finished loading - see $bmpLog" }
        Start-Sleep -Seconds 1
    }
    Write-Host "loaded; warming up $Warmup s at speed $Speed"
    Start-Sleep -Seconds $Warmup
    $before = Get-Date
    Set-Content "$game\bperf_profile.txt" "$Seconds"
    $prof = $null
    while (-not $prof) {
        if ((Get-Date) -gt $deadline) { throw "no profile written - see $game\bperf.log" }
        Start-Sleep -Seconds 2
        $prof = Get-ChildItem $game -Filter 'bperf_profile_*.txt' | Where-Object LastWriteTime -ge $before | Select-Object -First 1
    }
} finally {
    Stop-Process -Name $exeName -Force -ErrorAction SilentlyContinue
    Remove-Item $cfg, "$game\bmp_nologs.txt" -ErrorAction SilentlyContinue   # never leave the harness armed for normal play
}
Write-Host "speed $Speed, save $Save -> $($prof.FullName)"
python -I "$PSScriptRoot\report.py" $prof.FullName --top 35
