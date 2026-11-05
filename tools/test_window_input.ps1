# Starts MarioPartyDS.exe with a test save and works it with real keyboard and mouse events.
# Only sends anything while the game's own window is in front, otherwise it stops.
# Usage: powershell -ExecutionPolicy Bypass -File tools\test_window_input.ps1 -Steps "wait=12,key=X,wait=3,click=128;180" -Frames 1500 -Out run\test.rgba [-Save run\test.sav] [-Scale 1]
param([string]$Steps, [int]$Frames = 1500, [string]$Out = "run\window_input.rgba", [string]$Save = "run\window_input.sav", [int]$Scale = 2)
$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr w);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr w, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr w, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
$rom = (Resolve-Path "Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).nds").Path
$outFull = Join-Path (Get-Location) $Out
$saveFull = Join-Path (Get-Location) $Save
$p = Start-Process -FilePath "native\build\MarioPartyDS.exe" -ArgumentList "`"$rom`" --scale $Scale --frames $Frames --save `"$saveFull`" --screenshot `"$outFull`"" -PassThru
try {
    while ($p.MainWindowHandle -eq 0 -and -not $p.HasExited) { Start-Sleep -Milliseconds 200; $p.Refresh() }
    $w = $p.MainWindowHandle
    [void][Win]::SetForegroundWindow($w)
    Start-Sleep -Milliseconds 300
    foreach ($step in ($Steps -split ",")) {
        $name, $value = $step -split "=", 2
        if ($name -eq "wait") { Start-Sleep -Seconds ([double]$value); continue }
        if ([Win]::GetForegroundWindow() -ne $w) { throw "game window is not in front; nothing sent for $step" }
        if ($name -eq "key") {
            $vk = [byte][char]$value
            $scan = [byte][Win]::MapVirtualKey($vk, 0)
            [Win]::keybd_event($vk, $scan, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 150
            [Win]::keybd_event($vk, $scan, 2, [UIntPtr]::Zero)
        } elseif ($name -eq "click") {  # pixels on the DS's bottom screen
            $x, $y = $value -split ";"
            $rect = New-Object Win+RECT
            [void][Win]::GetClientRect($w, [ref]$rect)
            $factor = [math]::Min($rect.R / 256.0, $rect.B / 384.0)
            $left = ($rect.R - 256 * $factor) / 2
            $top = ($rect.B - 384 * $factor) / 2
            $point = New-Object Win+POINT
            $point.X = [int]($left + ([int]$x + 0.5) * $factor)
            $point.Y = [int]($top + ([int]$y + 192 + 0.5) * $factor)
            [void][Win]::ClientToScreen($w, [ref]$point)
            [void][Win]::SetCursorPos($point.X, $point.Y)
            Start-Sleep -Milliseconds 100
            [Win]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 200
            [Win]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
        }
    }
    if (-not $p.WaitForExit(120000)) { throw "the game did not end" }
    "exit code: $($p.ExitCode)"
} finally {
    if (-not $p.HasExited) { $p.Kill() }
}
