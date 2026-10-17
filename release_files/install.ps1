# Banished Multiplayer installer (Install.bat / Uninstall.bat run this).
#   dinput8.dll   -> the Banished folder (next to Application-steam-x64.exe)
#   bmpShare.pkm  -> Banished\WinData
# A dinput8.dll from another mod is kept as dinput8.dll.bmp-backup and put back by Uninstall.
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

function Find-Banished {
    $exe = 'Application-steam-x64.exe'
    if (Test-Path (Join-Path $here $exe)) { return $here }        # unpacked straight into the game folder
    if (Test-Path (Join-Path (Split-Path $here) $exe)) { return (Split-Path $here) }
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
        if (Test-Path (Join-Path $g $exe)) { return $g }
    }
    Add-Type -AssemblyName System.Windows.Forms
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = 'Where is Banished installed? (the folder with Application-steam-x64.exe)'
    if ($d.ShowDialog() -eq 'OK' -and (Test-Path (Join-Path $d.SelectedPath $exe))) { return $d.SelectedPath }
    return $null
}

function Is-Ours([string]$dll) {  # our DLL carries the text "BanishedMP" (UTF-16)
    $b = [IO.File]::ReadAllBytes($dll)
    return [Text.Encoding]::Unicode.GetString($b).Contains('BanishedMP')
}

try {
    $game = Find-Banished
    if (-not $game) { throw "Couldn't find Banished. Install the Steam version of Banished, or unpack this folder into the Banished folder and run it again." }
    Write-Host "Banished folder: $game"
    if (Get-Process 'Application-steam-x64' -ErrorAction SilentlyContinue) { throw 'Banished is running - close it first, then run this again.' }
    $dll = Join-Path $game 'dinput8.dll'
    $backup = Join-Path $game 'dinput8.dll.bmp-backup'
    $pkm = Join-Path $game 'WinData\bmpShare.pkm'

    if ($Uninstall) {
        if ((Test-Path $dll) -and (Is-Ours $dll)) { Remove-Item $dll; Write-Host 'removed dinput8.dll' }
        if (Test-Path $backup) { Move-Item $backup $dll; Write-Host 'put the other mod''s dinput8.dll back' }
        if (Test-Path $pkm) { Remove-Item $pkm; Write-Host 'removed WinData\bmpShare.pkm' }
        Write-Host ''
        Write-Host 'Banished Multiplayer is uninstalled. (Your settings and logs, bmp_*.txt / bmp*.log in the Banished folder, are left; delete them if you like.)' -ForegroundColor Green
    } else {
        foreach ($f in 'dinput8.dll', 'bmpShare.pkm') {
            if (-not (Test-Path (Join-Path $here $f))) { throw "$f is missing from this folder - unpack the whole zip first." }
        }
        if ((Test-Path $dll) -and -not (Is-Ours $dll) -and -not (Test-Path $backup)) {
            Move-Item $dll $backup
            Write-Host 'another mod''s dinput8.dll was there: kept as dinput8.dll.bmp-backup (Uninstall puts it back)' -ForegroundColor Yellow
        }
        Copy-Item (Join-Path $here 'dinput8.dll') $dll -Force
        # subscribed on the Steam Workshop: Steam keeps the mod file up to date - no second copy in WinData
        $workshop = Join-Path (Split-Path (Split-Path $game)) 'workshop\content\242920'
        $sub = if (Test-Path $workshop) { Get-ChildItem $workshop -Recurse -Filter 'bmpShare.pkm' -ErrorAction SilentlyContinue | Select-Object -First 1 }
        if ($sub) {
            if (Test-Path $pkm) { Remove-Item $pkm }
            Write-Host 'mod file: using your Steam Workshop subscription'
        } else {
            New-Item -ItemType Directory -Force (Split-Path $pkm) | Out-Null
            Copy-Item (Join-Path $here 'bmpShare.pkm') $pkm -Force
        }
        Write-Host ''
        Write-Host 'Banished Multiplayer is installed.' -ForegroundColor Green
        Write-Host 'Start Banished from Steam: the main menu now has a Multiplayer button. See README.txt for how to play.'
    }
} catch {
    Write-Host ''
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
