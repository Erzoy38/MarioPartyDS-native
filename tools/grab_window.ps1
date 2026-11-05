# Saves a picture of the melonDS window only, so a tool can see what the game shows.
# Usage: powershell -ExecutionPolicy Bypass -File tools\grab_window.ps1 <out.png>
param([Parameter(Mandatory = $true)][string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MpdsWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
$p = Get-Process melonDS -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw "melonDS has no window" }
$r = New-Object MpdsWin+RECT
[void][MpdsWin]::GetWindowRect($p.MainWindowHandle, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# 2 = PW_RENDERFULLCONTENT, needed for windows drawn by the GPU
$ok = [MpdsWin]::PrintWindow($p.MainWindowHandle, $dc, 2)
$g.ReleaseHdc($dc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved $Out, $($r.R - $r.L)x$($r.B - $r.T), PrintWindow=$ok"
