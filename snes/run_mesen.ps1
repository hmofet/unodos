# run_mesen.ps1 - the UnoDOS/SNES scripted regression rig (Mesen2).
#
# Launches Mesen2 on a ROM, lets it run, then triggers Mesen's own
# TakeScreenshot (F12) to dump the ACCURATE PPU framebuffer to disk and
# copies it to -Out. This is the reference render: it is the emulation
# core's output, independent of the display path.
#
# Why F12 and not a window grab: on a headless/RDP host the emulator's GPU
# surface comes back black through PrintWindow, and forcing Mesen's software
# renderer (which IS grabbable) introduces a display-blit artifact (it drops
# BG palette bits below ~scanline 160). Mesen's F12 screenshot sidesteps both
# - it writes the framebuffer the PPU produced. Verified byte-correct.
#
# Focus is forced with AttachThreadInput (plain SetForegroundWindow loses the
# foreground race from a background process, so the F12 keystroke is dropped).
#
# Usage:
#   ./run_mesen.ps1 -Rom build\unodos_test.sfc -Out build\m1.png
#   ./run_mesen.ps1 -Rom build\unodos.sfc -Out shot.png -Seconds 6 -KeepOpen
#
# Input is exercised by the AUTOTEST builds (synthetic joypad in the NMI),
# not by injecting host keystrokes - mirrors the Genesis AUTOTEST path.
param(
    [Parameter(Mandatory=$true)][string]$Rom,
    [string]$Out = "build\shot.png",
    [int]$Seconds = 6,
    [string]$Mesen = "C:\Users\arin\snes-tools\mesen\Mesen.exe",
    [switch]$KeepOpen
)
$ErrorActionPreference = "Stop"
Add-Type @"
using System;using System.Runtime.InteropServices;
public class MesenRig {
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a,uint b,bool c);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk,byte sc,uint f,IntPtr e);
}
"@

$RomFull  = (Resolve-Path $Rom).Path
$RomName  = [System.IO.Path]::GetFileNameWithoutExtension($RomFull)
$ShotDir  = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Mesen2\Screenshots'
if (Test-Path $ShotDir) { Get-ChildItem "$ShotDir\$RomName*.png" -ErrorAction SilentlyContinue | Remove-Item -Force }

Get-Process Mesen -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Start-Process -FilePath $Mesen -ArgumentList $RomFull
Start-Sleep -Seconds $Seconds

$p = Get-Process Mesen | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
# force foreground so the F12 keystroke is delivered
$fg = [MesenRig]::GetForegroundWindow()
$dummy = [uint32]0
$fgT = [MesenRig]::GetWindowThreadProcessId($fg, [ref]$dummy)
$tgtT = [MesenRig]::GetWindowThreadProcessId($h, [ref]$dummy)
$myT = [MesenRig]::GetCurrentThreadId()
[MesenRig]::ShowWindow($h, 9) | Out-Null
[MesenRig]::AttachThreadInput($myT, $tgtT, $true) | Out-Null
[MesenRig]::AttachThreadInput($myT, $fgT, $true) | Out-Null
[MesenRig]::BringWindowToTop($h) | Out-Null
[MesenRig]::SetForegroundWindow($h) | Out-Null
[MesenRig]::AttachThreadInput($myT, $tgtT, $false) | Out-Null
[MesenRig]::AttachThreadInput($myT, $fgT, $false) | Out-Null
Start-Sleep -Milliseconds 500
# F12 = TakeScreenshot (Mesen keycode 101)
[MesenRig]::keybd_event(0x7B, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 100
[MesenRig]::keybd_event(0x7B, 0, 2, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900

$shot = Get-ChildItem "$ShotDir\$RomName*.png" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
if ($shot) {
    Copy-Item $shot.FullName $Out -Force
    Write-Output "saved $Out (from $($shot.Name))"
} else {
    Write-Warning "no screenshot produced - check Mesen window focus / F12 binding"
}

if (-not $KeepOpen) { Get-Process Mesen -ErrorAction SilentlyContinue | Stop-Process -Force }
