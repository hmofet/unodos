# snapretry.ps1 - PrintWindow capture with retries: BlastEm's GL surface
# intermittently yields an all-white PrintWindow result; sample pixels
# and retry until real content appears (or give up after ~12s).
param([string]$ProcName = "blastem", [string]$Out = "shot.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Snap3 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$p = Get-Process $ProcName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
[Win32Snap3]::SetForegroundWindow($h) | Out-Null
$rect = New-Object Win32Snap3+RECT
[Win32Snap3]::GetWindowRect($h, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left; $hh = $rect.Bottom - $rect.Top
for ($try = 0; $try -lt 24; $try++) {
    Start-Sleep -Milliseconds 500
    $bmp = New-Object System.Drawing.Bitmap($w, $hh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [Win32Snap3]::PrintWindow($h, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    # sample a grid of pixels inside the client area; all-white = retry
    $allwhite = $true
    foreach ($fx in 0.2, 0.5, 0.8) {
        foreach ($fy in 0.2, 0.5, 0.8) {
            $px = $bmp.GetPixel([int]($w * $fx), [int]($hh * $fy))
            if ($px.R -lt 250 -or $px.G -lt 250 -or $px.B -lt 250) { $allwhite = $false }
        }
    }
    if (-not $allwhite) {
        $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        Write-Output "saved $Out ($w x $hh) after $($try+1) tries"
        exit 0
    }
    $bmp.Dispose()
}
Write-Output "STILL WHITE after 24 tries - captured anyway"
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Win32Snap3]::PrintWindow($h, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
