# run.ps1 — launch the SMS ROM in BlastEm and capture a focus-independent
# screenshot (PrintWindow with white-retry; BlastEm's surface intermittently
# yields an all-white grab). RDP-aware per ~/.claude/CLAUDE.md: forces the SDL
# software renderer so PrintWindow can read the client area.
param(
  [string]$Rom = "build\unodos.sms",
  [string]$Out = "build\desktop.png"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$blastem = "C:\Users\arin\genesis-tools\blastem-win32-0.6.2\blastem.exe"
$romPath = Join-Path $here $Rom
$outPath = Join-Path $here $Out

Get-Process blastem -ErrorAction SilentlyContinue | Stop-Process -Force
$env:SDL_RENDER_DRIVER = "software"   # so PrintWindow can read the client area over RDP
$env:SDL_AUDIODRIVER = "dummy"        # RDP often has no audio endpoint; avoids a fatal dialog
Start-Process -FilePath $blastem -ArgumentList "`"$romPath`""
Start-Sleep -Seconds 4

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$p = Get-Process blastem | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
[W32]::SetForegroundWindow($h) | Out-Null
$r = New-Object W32+RECT
[W32]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
for ($i = 0; $i -lt 24; $i++) {
  Start-Sleep -Milliseconds 400
  $bmp = New-Object System.Drawing.Bitmap($w, $hh)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [W32]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
  $white = $true
  foreach ($fx in 0.3,0.5,0.7) { foreach ($fy in 0.3,0.5,0.7) {
    $px = $bmp.GetPixel([int]($w*$fx), [int]($hh*$fy))
    if ($px.R -lt 250 -or $px.G -lt 250 -or $px.B -lt 250) { $white = $false }
  } }
  if (-not $white) { $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Output "saved $outPath ($w x $hh) after $($i+1) tries"; exit 0 }
  $bmp.Dispose()
}
Write-Output "STILL WHITE after 24 tries"
exit 1
