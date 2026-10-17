# Two-instance lockstep test on one PC through the local relay.
#   .\tools\nettest.ps1 -Ticks 20000 [-Commands recon\cmdlogs\session6_commands.log]
# Instance slot 0: idle (sends the command file through the relay, if given). Slot 1: chaos.
# Requires <game>\steam_appid.txt (lets a second instance start without Steam relaunching it).
param(
    [string]$Save = 'Save0',
    [int]$Ticks = 20000,
    [int]$Speed = 5,
    [int]$Delay = 30,
    [string]$Commands = '',
    [string]$Commands1 = '',  # command file sent by slot 1
    [string]$Commands2 = '',  # command file sent by slot 2 (-Players 3)
    [int]$TimeoutMin = 25,
    [int]$PauseEvery = 1500,   # networked chaos: ~1 TogglePause tap per N frames
    [switch]$NoSpeedChaos,
    [string]$SharedNew = '',  # -InGameHost: ONE shared town on a new map, 'seed,size,start,terrain,climate'
    [string]$Separate = '',  # -InGameHost: separate towns on a new map, 'seed,size,start,terrain,climate' (e.g. 12345,1,2,0,0)
    [string]$ResyncAt = "-1",     # -InGameHost: host forces a resync (save + everybody reloads) at this tick
    [int]$LateJoin = 0,      # -InGameHost: host starts alone, slot 1 joins after this many seconds (join in progress)
    [string]$UiScript = '',   # UI test script run by slot 0 (src/uiscript.cpp)
    [string]$UiScript1 = '',  # UI test script run by slot 1
    [switch]$Calm,        # slot 1 idle instead of chaos (no random clicks interfering with UI scripts)
    [switch]$NoPick,      # -Separate: towns are placed automatically (no settling)
    [string]$Prewarm0 = '',  # slot 0 plays this save single-player first (slot 1 joins 60 s later); not with -Separate (new maps need the menu)
    [string]$Prewarm1 = '',  # the same for slot 1 (joins when its prewarm has run 60 s)
    [int]$DropAt1 = -1,
    [int]$StallAt1 = -1,
    [int]$DumpAt = -1,  # -InGameHost: both instances write their desync record (bmp_desync_*.log) at this tick  # -InGameHost: slot 1 freezes for 6 s at this session tick ("waiting for" notice test)  # -InGameHost: slot 1 cuts its connection at this session tick (reconnect test)
    [switch]$NoMod1,  # slot 1 plays without the companion data mod (bmp_nomod_p1.txt switches it off)
    [string]$Names = '',
    [string]$Extra = '',  # -InGameHost: more bmp_net.txt lines, ';'-separated (e.g. 'corruptsave@1=1')  # -InGameHost: player names by slot, e.g. 'Ann,Bob,Cid' (default slotN) - remembered town owners
    [switch]$Jitter,  # slots 1+ scramble their engine heap layout (bmp_heapjitter_pN.txt) like a different PC would
    [string]$FakeVersion1 = '',  # -InGameHost: slot 1 claims this mod build id (the host must refuse it; use -TimeoutMin 2)
    [switch]$InGameHost,  # no Python relay: slot 0 hosts in-process (TCP 7777) and sends its save to slot 1
    [switch]$ViaRelay,
    [string]$RelayAddr = '',  # -ViaRelay: use this relay server (host:port) instead of starting a local one    # -InGameHost through tools/bmp_relay.py (port 7780): host and guests connect out to it
    [ValidateRange(2, 3)][int]$Players = 2   # -InGameHost: 3 = a third instance (slot 2, chaos) plays too
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot\paths.ps1"
$game = Get-BanishedDir
$exeName = 'Application-steam-x64'
$out = "$root\recon\runs\net"
New-Item -ItemType Directory -Force $out | Out-Null
if (Get-Process $exeName -ErrorAction SilentlyContinue) { throw "Banished is already running - close it first" }
if (-not (Test-Path "$game\steam_appid.txt")) { '242920' | Set-Content "$game\steam_appid.txt" -Encoding ascii -NoNewline }
Set-ItemProperty 'HKCU:\Software\Shining Rock Software LLC\Banished' -Name VideoSettings -Value 'true'

$stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
$relayLog = "$out\${stamp}_relay.log"
if ($InGameHost) {
    $relay = $null
    $rport = if ($ViaRelay) { 7780 } else { 7777 }
    $raddr = if ($RelayAddr) { $RelayAddr } else { "127.0.0.1:$rport" }
    $p2 = if ($Players -ge 3) { "`nrelay@2=$raddr`nrecvname@2=MultiplayerTestP2`njoindelay@2=$LateJoin" } else { '' }
    $hostKey = if ($ViaRelay) { "relayhost@0=$raddr`nroom=T$((Get-Random -Maximum 99999))" } else { 'host@0=7777' }
    if ($ViaRelay -and -not $RelayAddr) {
        $relay = Start-Process python -ArgumentList '-I', "`"$root\tools\bmp_relay.py`"", '--port', '7780' -RedirectStandardOutput $relayLog -RedirectStandardError "$relayLog.err" -PassThru -WindowStyle Hidden
        Start-Sleep 2
    }
    "$hostKey`nplayers@0=$(if ($LateJoin) {1} else {$Players})$p2`nsave@0=$Save`nrelay@1=$raddr`nrecvname@1=MultiplayerTestP1$(if ($Delay) {"`ndelay=$Delay"})`nresyncat@0=$ResyncAt`njoindelay@1=$LateJoin$(if ($Separate) {"`nseparate@0=$Separate"})$(if ($SharedNew) {"`nsharednew@0=$SharedNew"})$(if ($NoPick) {"`npick@0=0"})$(if ($FakeVersion1) {"`nfakeversion@1=$FakeVersion1"})$(if ($DropAt1 -ge 0) {"`ndropat@1=$DropAt1"})$(if ($StallAt1 -ge 0) {"`nstallat@1=$StallAt1"})$(if ($DumpAt -ge 0) {"`ndumpat=$DumpAt"})$(if (($Prewarm0 -or $Prewarm1) -and -not $LateJoin) {"`njoindelay@1=60"})" | Set-Content "$game\bmp_net.txt" -Encoding ascii
    if ($Names) { $i = 0; foreach ($n in $Names.Split(',')) { Add-Content "$game\bmp_net.txt" "name@$i=$n" -Encoding ascii; $i++ } }
    if ($Extra) { foreach ($x in $Extra.Split(';')) { Add-Content "$game\bmp_net.txt" $x -Encoding ascii } }
} else {
    $relay = Start-Process python -ArgumentList '-I', "`"$root\tools\relay.py`"", '--players', '2' -RedirectStandardOutput $relayLog -RedirectStandardError "$relayLog.err" -PassThru -WindowStyle Hidden
    Start-Sleep 2
    "relay=127.0.0.1:7777`ndelay=$Delay" | Set-Content "$game\bmp_net.txt" -Encoding ascii
}
$cmdLine = if ($Commands) { "commands@0=$((Resolve-Path $Commands).Path)" } else { '' }
if ($Commands1) { $cmdLine += "`ncommands@1=$((Resolve-Path $Commands1).Path)" }
if ($Commands2) { $cmdLine += "`ncommands@2=$((Resolve-Path $Commands2).Path)" }
if ($UiScript) { $cmdLine += "`nuiscript@0=$((Resolve-Path $UiScript).Path)" }
if ($UiScript1) { $cmdLine += "`nuiscript@1=$((Resolve-Path $UiScript1).Path)" }
if ($Prewarm0) { $cmdLine += "`nprewarm@0=$Prewarm0" }
if ($Prewarm1) { $cmdLine += "`nprewarm@1=$Prewarm1" }
"save=$Save`nticks=$Ticks`nspeed=$Speed`nmode@0=idle`nmode@1=$(if ($Calm) {'idle'} else {'chaos'})`nmode@2=chaos`npauseevery=$PauseEvery`nspeedchaos=$(if ($NoSpeedChaos) {0} else {1})`n$cmdLine" | Set-Content "$game\bmp_autotest.txt" -Encoding ascii
$start = Get-Date
if ($NoMod1) { '1' | Set-Content "$game\bmp_nomod_p1.txt" }
if ($Jitter) { '1' | Set-Content "$game\bmp_heapjitter_p1.txt"; '1' | Set-Content "$game\bmp_heapjitter_p2.txt" }
try {
    Start-Process "$game\$exeName.exe" -WorkingDirectory $game | Out-Null
    Start-Sleep 8
    Start-Process "$game\$exeName.exe" -WorkingDirectory $game | Out-Null
    if ($Players -ge 3) { Start-Sleep 8; Start-Process "$game\$exeName.exe" -WorkingDirectory $game | Out-Null }
    $deadline = $start.AddMinutes($TimeoutMin)
    Start-Sleep 20
    while (Get-Process $exeName -ErrorAction SilentlyContinue) {
        if ((Get-Date) -gt $deadline) { Stop-Process -Name $exeName -Force; Write-Host 'TIMEOUT'; break }
        Start-Sleep 3
    }
} finally {
    Remove-Item "$game\bmp_net.txt", "$game\bmp_autotest.txt", "$game\bmp_nomod_p1.txt", "$game\bmp_heapjitter_p1.txt", "$game\bmp_heapjitter_p2.txt" -ErrorAction SilentlyContinue
    Remove-Item "$([Environment]::GetFolderPath('MyDocuments'))\Banished\Save\MultiplayerTestP1.sav", "$([Environment]::GetFolderPath('MyDocuments'))\Banished\Save\MultiplayerTestP2.sav" -ErrorAction SilentlyContinue
    if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
}
foreach ($s in '', '_p1', '_p2') {
    $t = Get-ChildItem $game -Filter "bmp_ticks_*$s.log" | Where-Object { $_.LastWriteTime -ge $start -and ($s -or $_.Name -notmatch '_p\d\.log$') } |
        Sort-Object Name | Select-Object -Last 1
    $label = if ($s) { $s.TrimStart('_') } else { 'p0' }
    if ($t) {
        Move-Item $t.FullName "$out\${stamp}_${label}_ticks.log"
        $d = $t.FullName -replace 'bmp_ticks_', 'bmp_draws_'
        if (Test-Path $d) { Move-Item $d "$out\${stamp}_${label}_draws.log" }
    }
    Copy-Item "$game\bmp$s.log" "$out\${stamp}_${label}_bmp.log" -ErrorAction SilentlyContinue
}
Write-Host '--- relay:'
if ($InGameHost) { Select-String -Path "$out\${stamp}_p0_bmp.log" -Pattern "net.host.|gameload: (loading|save loaded)" | Select-Object -Last 12 | ForEach-Object { $_.Line } }
else { Get-Content $relayLog | Select-Object -Last 12 }
Write-Host '--- instance logs (net/desync):'
foreach ($l in 'p0', 'p1', 'p2') { Select-String -Path "$out\${stamp}_${l}_bmp.log" -Pattern 'net:|DESYNC|quitting|chaos taps' -ErrorAction SilentlyContinue | Select-Object -Last 6 | ForEach-Object { "[$l] " + ($_.Line -split '\] ', 2)[1] } }
if ((Test-Path "$out\${stamp}_p0_ticks.log") -and (Test-Path "$out\${stamp}_p1_ticks.log")) {
    Write-Host '--- compare p0 vs p1:'
    python -I "$root\tools\compare_ticks.py" "$out\${stamp}_p0_ticks.log" "$out\${stamp}_p1_ticks.log"
}
if ((Test-Path "$out\${stamp}_p0_ticks.log") -and (Test-Path "$out\${stamp}_p2_ticks.log")) {
    Write-Host '--- compare p0 vs p2:'
    python -I "$root\tools\compare_ticks.py" "$out\${stamp}_p0_ticks.log" "$out\${stamp}_p2_ticks.log"
}
