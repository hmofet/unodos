# run_mesen.ps1 - the UnoDOS/SNES scripted regression rig (the Genesis
# snapretry.ps1 role, retargeted to Mesen2).
#
# Launches Mesen2 on a ROM, lets it run a fixed number of seconds, captures
# the window with PrintWindow, and (optionally) closes the emulator. Mesen
# renders the SNES picture on a GPU surface that PrintWindow returns black
# under a headless/RDP desktop - so the rig REQUIRES Mesen's software
# renderer. setup_mesen.ps1 flips that flag once; this script asserts it.
#
# Usage:
#   ./run_mesen.ps1 -Rom build\unodos_test.sfc -Out build\m0_test.png
#   ./run_mesen.ps1 -Rom build\unodos.sfc -Out shot.png -Seconds 6 -KeepOpen
#
# Input is verified by the AUTOTEST builds (synthetic joypad in the NMI),
# not by injecting host keystrokes - Mesen's key IDs are internal scancodes
# and headless focus is unreliable. This mirrors the Genesis AUTOTEST path.
param(
    [Parameter(Mandatory=$true)][string]$Rom,
    [string]$Out = "build\shot.png",
    [int]$Seconds = 6,
    [string]$Mesen = "C:\Users\arin\snes-tools\mesen\Mesen.exe",
    [switch]$KeepOpen
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class WinShot {
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
 public struct RECT{public int Left,Top,Right,Bottom;}
}
"@

# assert the software renderer (PrintWindow can't grab the GPU surface here)
$cfg = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Mesen2\settings.json'
if (Test-Path $cfg) {
    $j = Get-Content $cfg -Raw | ConvertFrom-Json
    if (-not $j.Video.UseSoftwareRenderer) {
        Write-Warning "Mesen UseSoftwareRenderer is OFF - run setup_mesen.ps1 first or the capture will be black."
    }
}

$RomFull = (Resolve-Path $Rom).Path
Get-Process Mesen -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Start-Process -FilePath $Mesen -ArgumentList $RomFull
Start-Sleep -Seconds $Seconds

$p = Get-Process Mesen | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
[WinShot]::ShowWindow($h, 9) | Out-Null
[WinShot]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 700
$r = New-Object WinShot+RECT
[WinShot]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[WinShot]::PrintWindow($h, $dc, 2) | Out-Null
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "saved $Out ($w x $hh)"

if (-not $KeepOpen) {
    Get-Process Mesen -ErrorAction SilentlyContinue | Stop-Process -Force
}
