# snapscreen.ps1 - capture a window's on-screen pixels (CopyFromScreen).
# PrintWindow lies for Mini vMac (dirty-region renderer); this grabs the
# real composited desktop area, so the window must be frontmost.
param([string]$ProcName = "Mini vMac",
      [string]$Out = "C:\Users\arin\Documents\Github\unodos\macplus\shots\mnvm.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Snap2 {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT pt);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$p = Get-Process $ProcName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
[Win32Snap2]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400
$rect = New-Object Win32Snap2+RECT
[Win32Snap2]::GetClientRect($h, [ref]$rect) | Out-Null
$pt = New-Object Win32Snap2+POINT
[Win32Snap2]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = $rect.Right; $hh = $rect.Bottom
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size($w, $hh)))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "saved $Out ($w x $hh)"
