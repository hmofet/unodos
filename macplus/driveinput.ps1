# driveinput.ps1 - inject mouse/keyboard into the Mini vMac window.
# Ops (comma-separated): focus | move:X:Y | down | up | click:X:Y |
#   key:STR (SendKeys syntax) | sleep:MS     X/Y are client coords.
param([string]$Ops = "focus", [string]$ProcName = "Mini vMac")
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Drive {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr i);
  public struct POINT { public int X, Y; }
  public const uint DOWN = 0x0002, UP = 0x0004;
}
"@
$p = Get-Process $ProcName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
[Drive]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 250

function To-Screen([int]$x, [int]$y) {
  $pt = New-Object Drive+POINT; $pt.X = $x; $pt.Y = $y
  [Drive]::ClientToScreen($h, [ref]$pt) | Out-Null
  return $pt
}

foreach ($op in $Ops -split ",") {
  $a = $op -split ":"
  switch ($a[0]) {
    "focus" { }
    "move"  { $pt = To-Screen ([int]$a[1]) ([int]$a[2]); [Drive]::SetCursorPos($pt.X, $pt.Y) | Out-Null; Start-Sleep -Milliseconds 120 }
    "down"  { [Drive]::mouse_event([Drive]::DOWN,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 120 }
    "up"    { [Drive]::mouse_event([Drive]::UP,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 120 }
    "click" { $pt = To-Screen ([int]$a[1]) ([int]$a[2]); [Drive]::SetCursorPos($pt.X, $pt.Y) | Out-Null; Start-Sleep -Milliseconds 150
              [Drive]::mouse_event([Drive]::DOWN,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
              [Drive]::mouse_event([Drive]::UP,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 120 }
    "key"   { [System.Windows.Forms.SendKeys]::SendWait($a[1]); Start-Sleep -Milliseconds 150 }
    "sleep" { Start-Sleep -Milliseconds ([int]$a[1]) }
  }
}
Write-Output "ok"
