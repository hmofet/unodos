# snapwin.ps1 - capture a window by process name to PNG
param([string]$ProcName = "winuae64", [string]$Out = "C:\Users\arin\unodos\amiga\build\shot.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Snap {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$p = Get-Process $ProcName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
[Win32Snap]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$rect = New-Object Win32Snap+RECT
[Win32Snap]::GetWindowRect($h, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left; $hh = $rect.Bottom - $rect.Top
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Win32Snap]::PrintWindow($h, $hdc, 2) | Out-Null   # 2 = PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "saved $Out ($w x $hh)"
