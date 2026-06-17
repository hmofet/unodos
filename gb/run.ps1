# run.ps1 — launch the Game Boy ROM in Mesen2 and capture the framebuffer.
#
# Mesen renders through a GPU surface that a GDI/PrintWindow grab reads as black
# (same class of issue as BlastEm's GL surface). Its own F12 screenshot writes
# the clean LCD framebuffer but is focus-flaky over RDP for some ROMs. So this
# harness temporarily flips Mesen to its SOFTWARE renderer (which IS grabbable),
# grabs the window with the focus-independent helper, then restores the setting.
# Unlike the NES (256x240 -> 3x overflows), the GB frame is 160x144 so even at
# Mesen's 3x (480x432) the whole frame fits the window — no top-left crop.
param(
  [string]$Rom = "build\unodos.gb",
  [string]$Out = "build\desktop.png",
  [int]$Seconds = 5,
  [string]$Mesen = "C:\Users\arin\snes-tools\mesen\Mesen.exe"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$romPath = Join-Path $here $Rom
$outPath = Join-Path $here $Out
$cfg = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Mesen2\settings.json'
$capture = Join-Path $env:USERPROFILE '.claude\tools\cc-capture.ps1'

Get-Process Mesen -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600

# flip to the software renderer (remember the prior value)
$raw = Get-Content $cfg -Raw
$wasHardware = $raw -match '"UseSoftwareRenderer": false'
if ($wasHardware) { ($raw -replace '"UseSoftwareRenderer": false','"UseSoftwareRenderer": true') | Set-Content $cfg }

try {
  Start-Process -FilePath $Mesen -ArgumentList "`"$romPath`""
  Start-Sleep -Seconds $Seconds
  # Mesen opens GB at 5x (160x144 -> 800x720, filling the 720px screen height),
  # which overflows the window and crops. Size the window to a 4x frame so the
  # whole 160x144 frame fits on this 1280x720 host.
  Add-Type @"
using System;using System.Runtime.InteropServices;
public class GbWin { [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int hh,bool r); }
"@
  $h = (Get-Process Mesen | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1).MainWindowHandle
  if ($h -ne 0) { [GbWin]::MoveWindow($h, 20, 10, 672, 690, $true) | Out-Null; Start-Sleep -Milliseconds 900 }
  & powershell -ExecutionPolicy Bypass -File $capture -Out $outPath -Window Mesen
} finally {
  if ($wasHardware) {
    (Get-Content $cfg -Raw) -replace '"UseSoftwareRenderer": true','"UseSoftwareRenderer": false' | Set-Content $cfg
  }
}
