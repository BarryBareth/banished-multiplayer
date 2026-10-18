# Banished Multiplayer installer (Install.bat / Uninstall.bat run this).
#   dinput8.dll   -> the Banished folder (next to Application-steam-x64.exe, or Application-x64.exe for GOG)
#   bmpShare.pkm  -> Banished\WinData
# Installs into every Banished it finds (Steam and GOG), or only into the folder this zip was unpacked into.
# A dinput8.dll from another mod is kept as dinput8.dll.bmp-backup and put back by Uninstall.
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$exes = 'Application-steam-x64.exe', 'Application-x64.exe'  # Steam, GOG

function Is-Game([string]$dir) {
    if (-not $dir) { return $false }
    foreach ($e in $exes) { if (Test-Path (Join-Path $dir $e)) { return $true } }
    return $false
}

function Find-Banished {
    if (Is-Game $here) { return @($here) }                       # unpacked straight into the game folder
    if (Is-Game (Split-Path $here)) { return @(Split-Path $here) }
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
    foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1207660783', 'HKLM:\SOFTWARE\GOG.com\Games\1207660783') {
        $g = (Get-ItemProperty $k -ErrorAction SilentlyContinue).path
        if (Is-Game $g) { $found += (Resolve-Path $g).Path }
    }
    if ($found) { return @($found | Select-Object -Unique) }
    Add-Type -AssemblyName System.Windows.Forms
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = 'Where is Banished installed? (the folder with Application-steam-x64.exe or Application-x64.exe)'
    if ($d.ShowDialog() -eq 'OK' -and (Is-Game $d.SelectedPath)) { return @($d.SelectedPath) }
    return @()
}

function Is-Ours([string]$dll) {  # our DLL carries the text "BanishedMP" (UTF-16)
    $b = [IO.File]::ReadAllBytes($dll)
    return [Text.Encoding]::Unicode.GetString($b).Contains('BanishedMP')
}

try {
    $games = @(Find-Banished)
    if (-not $games) { throw "Couldn't find Banished. Unpack this folder into the Banished folder (the one with Application-steam-x64.exe or Application-x64.exe) and run it again." }
    if (Get-Process 'Application-steam-x64', 'Application-x64' -ErrorAction SilentlyContinue) { throw 'Banished is running - close it first, then run this again.' }
    foreach ($f in 'dinput8.dll', 'bmpShare.pkm') {
        if (-not $Uninstall -and -not (Test-Path (Join-Path $here $f))) { throw "$f is missing from this folder - unpack the whole zip first." }
    }
    foreach ($game in $games) {
        Write-Host "Banished folder: $game"
        $dll = Join-Path $game 'dinput8.dll'
        $backup = Join-Path $game 'dinput8.dll.bmp-backup'
        $pkm = Join-Path $game 'WinData\bmpShare.pkm'
        if ($Uninstall) {
            if ((Test-Path $dll) -and (Is-Ours $dll)) { Remove-Item $dll; Write-Host '  removed dinput8.dll' }
            if (Test-Path $backup) { Move-Item $backup $dll; Write-Host '  put the other mod''s dinput8.dll back' }
            if (Test-Path $pkm) { Remove-Item $pkm; Write-Host '  removed WinData\bmpShare.pkm' }
            continue
        }
        if ((Test-Path $dll) -and -not (Is-Ours $dll) -and -not (Test-Path $backup)) {
            Move-Item $dll $backup
            Write-Host '  another mod''s dinput8.dll was there: kept as dinput8.dll.bmp-backup (Uninstall puts it back)' -ForegroundColor Yellow
        }
        Copy-Item (Join-Path $here 'dinput8.dll') $dll -Force
        # subscribed on the Steam Workshop: Steam keeps the mod file up to date - no second copy in WinData
        $workshop = Join-Path (Split-Path (Split-Path $game)) 'workshop\content\242920'
        $sub = if (Test-Path $workshop) { Get-ChildItem $workshop -Recurse -Filter 'bmpShare.pkm' -ErrorAction SilentlyContinue | Select-Object -First 1 }
        if ($sub) {
            if (Test-Path $pkm) { Remove-Item $pkm }
            Write-Host '  mod file: using your Steam Workshop subscription'
        } else {
            New-Item -ItemType Directory -Force (Split-Path $pkm) | Out-Null
            Copy-Item (Join-Path $here 'bmpShare.pkm') $pkm -Force
        }
    }
    Write-Host ''
    if ($Uninstall) {
        Write-Host 'Banished Multiplayer is uninstalled. (Your settings and logs, bmp_*.txt / bmp*.log in the Banished folder, are left; delete them if you like.)' -ForegroundColor Green
    } else {
        Write-Host 'Banished Multiplayer is installed.' -ForegroundColor Green
        Write-Host 'Start Banished: the main menu now has a Multiplayer button. See README.txt for how to play.'
    }
} catch {
    Write-Host ''
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
