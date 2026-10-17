# Packs a release: release\BanishedMultiplayer-<version>.zip (dinput8.dll, bmpShare.pkm, Install.bat, Uninstall.bat,
# install.ps1, README.txt).
#   .\tools\make_release.ps1 [-Force]
# The version is kModVersion in src\net.cpp: raise it for every release (the update check only offers HIGHER numbers).
# Build first: .\mod\make_mod_sources.ps1; .\mod\build_mod.ps1; .\build.ps1
# Publish: a GitHub release tagged v<version> with the zip attached (docs\RELEASING.md). Every installed copy sees it
# at its next start: the update check reads the repository's latest release (repo.txt).
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot

$version = [regex]::Match((Get-Content "$root\src\net.cpp" -Raw), 'kModVersion = "([0-9.]+)"').Groups[1].Value
if (-not $version) { throw 'kModVersion not found in src\net.cpp' }

# the DLL and the mod file must belong together (the DLL warns about any other bmpShare.pkm)
$dll = Get-Item "$root\build\dinput8.dll"
$pkm = Get-Item "$root\mod\bmpShare.pkm"
$want = [regex]::Match((Get-Content "$root\src\modversion.h" -Raw), 'BMP_MOD_VERSION "([0-9a-f]+)"').Groups[1].Value
$have = (Get-ChildItem "$root\mod\src\BMP" -Filter 'Version_*.rsc' | Select-Object -First 1).BaseName -replace '^Version_', ''
if ($want -ne $have) { throw "the mod sources are build $have but the DLL expects $want - rebuild: mod\build_mod.ps1, build.ps1" }
$marker = Get-Item "$root\mod\src\BMP\Version_$have.rsc"
if ($pkm.LastWriteTime -lt $marker.LastWriteTime) { throw 'mod\bmpShare.pkm is older than its sources - run mod\build_mod.ps1' }
$newestSrc = Get-ChildItem "$root\src" -File | Sort-Object LastWriteTime | Select-Object -Last 1
if ($dll.LastWriteTime -lt $newestSrc.LastWriteTime) { throw "build\dinput8.dll is older than src\$($newestSrc.Name) - run build.ps1" }

$name = "BanishedMultiplayer-$version"
$out = "$root\release"
$zip = "$out\$name.zip"
if ((Test-Path $zip) -and -not $Force) { throw "$zip exists - raise kModVersion in src\net.cpp for a new release (or -Force to rebuild this one)" }
$dir = "$out\$name"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $dll.FullName, $pkm.FullName $dir
$relay = Get-Item "$root\build\BanishedRelay.exe"  # a relay server anyone can run (README: "Running your own relay")
if ($relay.LastWriteTime -lt (Get-Item "$root\server\relay\BanishedRelay.cpp").LastWriteTime) { throw 'build\BanishedRelay.exe is older than its source - run build.ps1' }
Copy-Item $relay.FullName $dir
Copy-Item "$root\release_files\Install.bat", "$root\release_files\Uninstall.bat", "$root\release_files\install.ps1" $dir
$repo = (Get-Content "$root\repo.txt" -TotalCount 1).Trim()
(Get-Content "$root\release_files\README.txt" -Raw).Replace('{VERSION}', $version).Replace('{REPO}', "https://github.com/$repo") |
    Set-Content "$dir\README.txt" -Encoding utf8
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$dir\*" -DestinationPath $zip

"release $version (mod build $have): $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
"next: a GitHub release tagged v$version with this zip attached (docs\RELEASING.md); bmpShare.pkm also goes to the Workshop"
