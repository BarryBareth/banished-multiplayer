# Automated determinism test.
#   .\tools\autotest.ps1                       # pair: A=idle, B=chaos, then compare
#   .\tools\autotest.ps1 -Single -Mode chaos   # one run
# Logs land in recon\runs\auto\<stamp>_<label>_{ticks,draws,bmp}.log
param(
    [string]$Save = 'Save0',
    [int]$Ticks = 20000,
    [int]$Speed = 5,
    [int]$Seed = 1,
    [ValidateSet('idle', 'chaos')][string]$Mode = 'idle',
    [switch]$Single,
    [int]$TimeoutMin = 20,
    [switch]$NoSpeedChaos,
    [string]$Commands = '',  # replay a bmp_commands.log file in both runs
    [string]$AllocLog = '',  # tick range for sim-heap alloc/free logging, e.g. 54000-55100
    [string]$WarmupA = '',   # run A first plays this save for -WarmupTicks, then loads -Save in the same process
    [int]$WarmupTicks = 3000,
    [string]$ExtraConfig = ''      # extra bmp_autotest.txt lines for both runs, '|'-separated (e.g. 'newgame=1,0,2,0,0|towns=2,1')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot\paths.ps1"
$game = Get-BanishedDir
$exeName = 'Application-steam-x64'
$out = "$root\recon\runs\auto"
New-Item -ItemType Directory -Force $out | Out-Null

function Invoke-Run([string]$label, [string]$mode, [string]$extra = '') {
    if (Get-Process $exeName -ErrorAction SilentlyContinue) { throw "Banished is already running - close it first" }
    # skip the launcher dialog (shown while VideoSettings=false; clicking Play sets it to true)
    Set-ItemProperty 'HKCU:\Software\Shining Rock Software LLC\Banished' -Name VideoSettings -Value 'true'
    $cfg = "$game\bmp_autotest.txt"
    "save=$Save`nmode=$mode`nticks=$Ticks`nspeed=$Speed`nseed=$Seed`nspeedchaos=$([int](-not $NoSpeedChaos))`ncommands=$Commands`nalloclog=$AllocLog`n$extra`n$($ExtraConfig -replace '\|', "`n")" | Set-Content $cfg -Encoding ascii
    $start = Get-Date
    try {
        Start-Process "$game\$exeName.exe" -WorkingDirectory $game | Out-Null
        # SteamStub may relaunch through Steam: wait for any instance to appear, then for all to exit
        $deadline = $start.AddMinutes($TimeoutMin)
        while (-not (Get-Process $exeName -ErrorAction SilentlyContinue)) {
            if ((Get-Date) -gt $deadline) { throw "game never started" }
            Start-Sleep -Milliseconds 500
        }
        $gone = 0
        while ($gone -lt 5) {   # 10 s without any instance = really closed (SteamStub relaunch leaves a gap)
            if (Get-Process $exeName -ErrorAction SilentlyContinue) { $gone = 0 } else { $gone++ }
            if ((Get-Date) -gt $deadline) { Stop-Process -Name $exeName -Force; throw "timed out after $TimeoutMin min" }
            Start-Sleep -Seconds 2
        }
    } finally {
        Remove-Item $cfg -ErrorAction SilentlyContinue   # never leave the harness armed for normal play
    }
    $stamp = $start.ToString('yyyyMMdd_HHmmss')
    $ticksLog = Get-ChildItem $game -Filter 'bmp_ticks_*.log' | Where-Object LastWriteTime -ge $start | Sort-Object Name | Select-Object -Last 1
    if (-not $ticksLog) { throw "no tick log produced - see $game\bmp.log" }
    $draws = $ticksLog.FullName -replace 'bmp_ticks_', 'bmp_draws_'
    $dst = "$out\${stamp}_$label"
    Move-Item $ticksLog.FullName "${dst}_ticks.log"
    if (Test-Path $draws) { Move-Item $draws "${dst}_draws.log" }
    Copy-Item "$game\bmp.log" "${dst}_bmp.log"
    $quit = Select-String -Path "${dst}_bmp.log" -Pattern 'autotest: quitting' | Select-Object -Last 1
    Write-Host "[$label] $($quit.Line)"
    return "${dst}_ticks.log"
}

if ($Single) {
    Invoke-Run $Mode $Mode
} else {
    $warm = if ($WarmupA) { "warmup=$WarmupA`nwarmupticks=$WarmupTicks" } else { '' }
    $a = Invoke-Run 'A-idle' 'idle' $warm
    $b = Invoke-Run 'B-chaos' 'chaos'
    python -I "$root\tools\compare_ticks.py" $a $b
}
