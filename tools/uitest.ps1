# UI test suite runner: two networked game instances (host + guest, separate towns, -NoPick map), the host uses every
# building-window control through the real UI handlers (src/uiscript.cpp), the guest tries them on the host's
# buildings. PASS needs: every "expect" passed, both instances executed the same commands, the simulations stayed
# identical, the guest's 4 attempts were refused, and the town hall profession change was sent.
#   .\tools\uitest.ps1
param([int]$Ticks = 17600, [string]$Gog0 = '', [string]$Gog1 = '')  # -Gog0 / -Gog1 DIR: host / guest run the GOG build
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot\paths.ps1"
$game = Get-BanishedDir
$hostDir = if ($Gog0) { $Gog0 } else { $game }
$guestDir = if ($Gog1) { $Gog1 } else { $game }
$gogArgs = @{}
if ($Gog0) { $gogArgs.Gog0 = $Gog0 }
if ($Gog1) { $gogArgs.Gog1 = $Gog1 }
$out = & "$PSScriptRoot\nettest.ps1" -InGameHost -NoPick -Separate '12345,1,2,0,0' -Calm -Speed 10 -NoSpeedChaos -Ticks $Ticks `
    -TimeoutMin 14 -UiScript "$root\tests\uitests\host_controls.txt" -UiScript1 "$root\tests\uitests\guest_foreign.txt" @gogArgs 2>&1
$fail = @()
$cmp = $out | Select-String 'IDENTICAL|diverged' | ForEach-Object Line
if (-not $cmp) { $fail += 'no comparison output (did the games run?)' }
$cmp | Where-Object { $_ -match 'diverged' } | ForEach-Object { $fail += "simulation: $_" }
foreach ($cd in $hostDir, $guestDir) { if ((Test-Path "$cd\crash.dmp") -and (Get-Item "$cd\crash.dmp").LastWriteTime -gt (Get-Date).AddMinutes(-15)) { $fail += 'a game crashed (crash.dmp)' } }
$host0 = Get-Content "$hostDir\bmp.log"
$tests = $host0 | Select-String 'UITEST (PASS|FAIL)' | ForEach-Object Line
$tests | Where-Object { $_ -match 'UITEST FAIL' } | ForEach-Object { $fail += $_ }
if (($tests | Measure-Object).Count -lt 7) { $fail += "only $(($tests | Measure-Object).Count) of 7 expects ran" }
$host0 | Select-String '!! uiscript' | ForEach-Object { $fail += $_.Line }
$guest = Get-Content "$guestDir\bmp_p1.log"
# a desync is fixed by an automatic resync (the comparison then only sees the reloaded session): count it as a failure
($host0 + $guest) | Select-String 'DESYNC at tick' | Select-Object -First 3 | ForEach-Object { $fail += "desync: $($_.Line)" }
# two hooks on one function (MinHook) silently break the later one (e.g. +/- worker buttons went local-only)
($host0 + $guest) | Select-String 'hook .*failed|-> MH_ERROR' | Where-Object { $_.Line -notmatch 'actionprobe' } |
    Select-Object -First 5 | ForEach-Object { $fail += "hook: $($_.Line)" }
$refused = ($guest | Select-String "window command on town 0's object ignored|profession change in town 0's town hall ignored" | Measure-Object).Count
if ($refused -ne 6) { $fail += "guest: $refused of 6 attempts refused" }
$c0 = Get-Content "$hostDir\bmp_commands.log"; $c1 = Get-Content "$guestDir\bmp_commands_p1.log"
if (Compare-Object $c0 $c1) { $fail += 'host and guest executed different commands' }
if (-not ($c0 | Select-String '^\d+ win 10 0 0 1 1 - 0 -> 1')) { $fail += 'town hall profession change was not sent' }
if (-not ($c0 | Select-String 'win 23 \S+ 5 1 0 - \S+ -> 1')) { $fail += 'trading post order move was not executed' }
if (-not ($c0 | Select-String 'win 22 .* -> 1')) { $fail += 'max-worker arrows were not sent' }
$rowsH = (($host0 | Select-String 'uiscript: rows TownHallUI' | Select-Object -Last 1).Line) -replace '^.*TownHallUI:', ''
$rowsG = (($guest | Select-String 'uiscript: rows TownHallUI' | Select-Object -Last 1).Line) -replace '^.*TownHallUI:', ''
if (-not $rowsH -or $rowsH -ne $rowsG) { $fail += "another town's town hall shows different numbers: host [$rowsH] guest view [$rowsG]" }
$shared = (($host0 | Select-String 'uiscript: town 1:' | Select-Object -Last 1).Line) -replace '^.*town 1: ', ''
if ($shared -notmatch 'shared by others [1-9]') { $fail += "shared storage: town 2 has [$shared]" }
Write-Host ''
Write-Host "  shared storage: town 2 has $shared"
Write-Host "  town hall numbers: host [$rowsH], guest's view of it [$rowsG]"
$tests | ForEach-Object { Write-Host "  $($_ -replace '^.*UITEST ', '')" }
Write-Host "  guest attempts refused: $refused of 6"
Write-Host "  commands: $(($c0 | Measure-Object).Count - 1) executed, identical on both: $(-not (Compare-Object $c0 $c1))"
if ($fail) { Write-Host "UI TEST FAILED:" -ForegroundColor Red; $fail | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }; exit 1 }
Write-Host 'UI TEST PASSED' -ForegroundColor Green
