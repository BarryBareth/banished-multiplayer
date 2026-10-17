# Builds the BMP data mod (.pkm) with the Banished Mod Kit (BANISHED_KIT, see tools\paths.ps1).
# The kit resolves inherited resources ("X : Dialog/Work.rsc:...") from SOURCE, so the mod is built from a full copy of
# the kit's resource folder (mod\work) with mod\src laid over it; the packager keeps only compiled files that differ
# from the game's own packages. Kit error dialogs are captured to mod\builderror.png and the tool is stopped.
#   .\mod\make_mod_sources.ps1; .\mod\build_mod.ps1
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\tools\paths.ps1"
$kit = Get-BanishedKit
$mod = $PSScriptRoot
$work = Join-Path $mod 'work'
$bin = Join-Path $mod 'bin'

robocopy "$kit\resource" $work /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $mod 'src') $work /E /NFL /NDL /NJH /NJS /NP | Out-Null
if (Test-Path $bin) { Remove-Item $bin -Recurse -Force }
New-Item -ItemType Directory -Force $bin | Out-Null

# paths are relative to the kit's bin folder
function Rel([string]$to) { [IO.Path]::GetRelativePath("$kit\bin", $to).Replace('\', '/') }
$res = Rel $work
$dat = Rel $bin

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class KitWin { [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f); }
'@ -ErrorAction SilentlyContinue

function RunTool([string]$toolArgs, [string]$label, [int]$timeoutSec = 600) {
    $log = Join-Path $mod "$label.log"
    $p = Start-Process -FilePath "$kit\bin\x64\Tools-x64.exe" -ArgumentList $toolArgs -WorkingDirectory $kit -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while (-not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if ($p.MainWindowTitle -like '*Error*') {
            $r = New-Object KitWin+R
            [KitWin]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
            $bmp = New-Object System.Drawing.Bitmap ($r.Rr - $r.L), ($r.B - $r.T)
            $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
            [KitWin]::PrintWindow($p.MainWindowHandle, $hdc, 2) | Out-Null
            $g.ReleaseHdc($hdc); $g.Dispose(); $bmp.Save((Join-Path $mod 'builderror.png')); $bmp.Dispose()
            Stop-Process -Id $p.Id -Force
            throw "$label failed: kit error dialog (see mod\builderror.png)"
        }
        if ((Get-Date) -gt $deadline) { Stop-Process -Id $p.Id -Force; throw "$label timed out" }
    }
    "$label exit $($p.ExitCode)"
}

RunTool "/build bmpResources.rsc /pathres $res /pathdat $dat" 'build'
RunTool "/mod Package.rsc:bmpShare /pathres $res /pathdat $dat" 'package'
Get-ChildItem $work, $bin, "$kit\bin" -Filter *.pkm -Recurse -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime

Copy-Item "$kit\bin\WinData\bmpShare.pkm" (Join-Path $mod 'bmpShare.pkm') -Force  # the copy to distribute
exit 0  # (robocopy leaves exit code 1-7 on success)
