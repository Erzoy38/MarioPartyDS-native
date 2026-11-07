# Starts MarioPartyDS.exe with a test save and works it with real keyboard and mouse events.
# Only sends anything while the game's own window is in front, otherwise it stops.
# Usage: powershell -ExecutionPolicy Bypass -File tools\test_window_input.ps1 -Steps "wait=12,key=X,wait=3,click=128;180" -Frames 1500 -Out run\test.rgba [-Save run\test.sav] [-Scale 1] [-ExtraArgs "--menu --settings run\a.ini"]
# Steps:
#   wait=SECONDS
#   key=X          a letter or digit key; key=ENTER, key=ESC, key=BACK, key=UP, key=DOWN, key=LEFT, key=RIGHT
#   text=ABC       types letters and digits
#   click=X;Y      pixels on the DS's bottom screen (0..255, 0..191)
#   tap=X;Y        pixels on both screens together (0..255, 0..383), for the menu
#   shot=FILE.png  a picture of the window
#   until=TEXT     waits up to 60 s until the log (next to -ExtraArgs' --settings file) holds TEXT
param([string]$Steps, [int]$Frames = 1500, [string]$Out = "run\window_input.rgba", [string]$Save = "run\window_input.sav",
      [int]$Scale = 2, [string]$ExtraArgs = "", [int]$ExitWait = 120)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr w, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr w, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr w);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr w, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr w, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w, IntPtr dc, uint flags);
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
$log = $null
if ($ExtraArgs -match "--settings\s+(\S+)") { $log = [IO.Path]::ChangeExtension((Join-Path (Get-Location) $Matches[1]), ".log") }
$p = Start-Process -FilePath "native\build\MarioPartyDS.exe" -ArgumentList "`"$rom`" --scale $Scale --frames $Frames --save `"$saveFull`" --screenshot `"$outFull`" $ExtraArgs" -PassThru
$named = @{ ENTER = 0x0D; ESC = 0x1B; BACK = 0x08; LEFT = 0x25; UP = 0x26; RIGHT = 0x27; DOWN = 0x28 }

function Send-Key([byte]$vk) {
    $scan = [byte][Win]::MapVirtualKey($vk, 0)
    $extended = if ($vk -ge 0x25 -and $vk -le 0x28) { 1 } else { 0 }
    [Win]::keybd_event($vk, $scan, $extended, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    [Win]::keybd_event($vk, $scan, 2 + $extended, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 80
}

function Send-Click($w, [int]$x, [int]$y) {
    $rect = New-Object Win+RECT
    [void][Win]::GetClientRect($w, [ref]$rect)
    $factor = [math]::Min($rect.R / 256.0, $rect.B / 384.0)
    $left = ($rect.R - 256 * $factor) / 2
    $top = ($rect.B - 384 * $factor) / 2
    $point = New-Object Win+POINT
    $point.X = [int]($left + ($x + 0.5) * $factor)
    $point.Y = [int]($top + ($y + 0.5) * $factor)
    [void][Win]::ClientToScreen($w, [ref]$point)
    # Nothing is clicked when another window covers this spot.
    $hit = [Win]::GetAncestor([Win]::WindowFromPoint($point), 2)
    if ($hit -ne $w) {
        $title = New-Object System.Text.StringBuilder 200
        [void][Win]::GetWindowText($hit, $title, 200)
        throw "another window covers the game window at $x;$y`: '$title'"
    }
    [void][Win]::SetCursorPos($point.X, $point.Y)
    Start-Sleep -Milliseconds 100
    [Win]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    [Win]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
}

function Save-Shot($w, [string]$file) {
    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($w, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [void][Win]::PrintWindow($w, $dc, 2)
    $g.ReleaseHdc($dc)
    $g.Dispose()
    $bmp.Save((Join-Path (Get-Location) $file), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

try {
    while ($p.MainWindowHandle -eq 0 -and -not $p.HasExited) { Start-Sleep -Milliseconds 200; $p.Refresh() }
    $w = $p.MainWindowHandle
    # Top left of the screen, away from dialogs that Windows opens in the middle.
    [void][Win]::SetWindowPos($w, [IntPtr]::Zero, 0, 0, 0, 0, 0x0001 -bor 0x0004)
    [void][Win]::SetForegroundWindow($w)
    Start-Sleep -Milliseconds 300
    foreach ($step in ($Steps -split ",")) {
        $name, $value = $step -split "=", 2
        if ($name -eq "wait") { Start-Sleep -Seconds ([double]$value); continue }
        if ($name -eq "shot") { Save-Shot $w $value; continue }
        if ($name -eq "until") {
            $deadline = (Get-Date).AddSeconds(60)
            while (-not ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch $value -Quiet))) {
                if ((Get-Date) -gt $deadline) { throw "log never showed: $value" }
                Start-Sleep -Milliseconds 300
            }
            continue
        }
        $front = [Win]::GetForegroundWindow()
        if ($front -ne $w) {
            $title = New-Object System.Text.StringBuilder 200
            [void][Win]::GetWindowText($front, $title, 200)
            throw "game window is not in front ('$title' is); nothing sent for $step"
        }
        if ($name -eq "key") {
            if ($named.ContainsKey($value)) { Send-Key ([byte]$named[$value]) } else { Send-Key ([byte][char]$value) }
        } elseif ($name -eq "text") {
            foreach ($c in $value.ToUpper().ToCharArray()) { Send-Key ([byte]$c) }
        } elseif ($name -eq "click") {
            $x, $y = $value -split ";"
            Send-Click $w ([int]$x) ([int]$y + 192)
        } elseif ($name -eq "tap") {
            $x, $y = $value -split ";"
            Send-Click $w ([int]$x) ([int]$y)
        }
    }
    if (-not $p.WaitForExit($ExitWait * 1000)) { throw "the game did not end" }
    "exit code: $($p.ExitCode)"
} finally {
    if (-not $p.HasExited) { $p.Kill() }
}
