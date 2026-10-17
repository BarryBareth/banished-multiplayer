# Capture the Banished window to a PNG (default: recon\screen.png). Usage: .\tools\screenshot.ps1 [-Out path]
param([string]$Out = "$(Split-Path $PSScriptRoot)\recon\screen.png", [int]$Index = 0, [int]$ProcessId = 0)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
'@
if ($ProcessId) { $p = Get-Process -Id $ProcessId -ErrorAction Stop } else { $p = Get-Process Application-steam-x64 -ErrorAction Stop | Where-Object MainWindowHandle -ne 0 | Sort-Object StartTime | Select-Object -Skip $Index -First 1 }
$r = New-Object W+RECT
[W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[W]::PrintWindow($p.MainWindowHandle, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
# downscale to keep the image small
$small = New-Object System.Drawing.Bitmap ([int]($w / 2)), ([int]($h / 2))
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.DrawImage($bmp, 0, 0, $small.Width, $small.Height)
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"$Out ($($small.Width)x$($small.Height))"
