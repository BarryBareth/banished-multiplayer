# Where things are on this PC, for the build and test scripts (dot-source it: . "$PSScriptRoot\paths.ps1").
# Each path comes from, in this order: an environment variable, <repo>\local-paths.txt (key=value lines, not in git),
# or auto-detection.
#   BANISHED_DIR   the game folder (with Application-steam-x64.exe)      - else found through Steam's library list
#   VCVARS64       Visual Studio's vcvars64.bat                           - else found with vswhere
#   BANISHED_KIT   the Banished Mod Kit folder (with bin\x64\Tools-x64.exe) - needed only to build the mod file
$script:RepoRoot = Split-Path $PSScriptRoot
$script:LocalPaths = @{}
$lp = Join-Path $RepoRoot 'local-paths.txt'
if (Test-Path $lp) {
    foreach ($l in Get-Content $lp) {
        if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$') { $LocalPaths[$matches[1].ToUpper()] = $matches[2] }
    }
}
function Get-PathSetting([string]$name) {
    $v = [Environment]::GetEnvironmentVariable($name)
    if ($v) { return $v }
    if ($LocalPaths.ContainsKey($name)) { return $LocalPaths[$name] }
    return $null
}

function Get-BanishedDir {
    $exe = 'Application-steam-x64.exe'
    $d = Get-PathSetting 'BANISHED_DIR'
    if ($d) {
        if (-not (Test-Path (Join-Path $d $exe))) { throw "BANISHED_DIR=$d has no $exe" }
        return $d
    }
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
    throw "Banished not found - set BANISHED_DIR (environment or local-paths.txt) to the game folder"
}

function Get-VcVars {
    $v = Get-PathSetting 'VCVARS64'
    if ($v) { return $v }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) {
            $bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $bat) { return $bat }
        }
    }
    throw "Visual Studio C++ tools not found - install the 'Desktop development with C++' workload or set VCVARS64"
}

function Get-BanishedKit {
    $k = Get-PathSetting 'BANISHED_KIT'
    if ($k) { return $k }
    throw "Banished Mod Kit not found - set BANISHED_KIT (environment or local-paths.txt) to the kit folder"
}
