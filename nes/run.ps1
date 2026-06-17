# run.ps1 — launch the NES ROM in Mesen2 and capture the framebuffer.
#
# Mesen renders through a GPU surface that a GDI/PrintWindow grab reads as black
# (same class of issue as BlastEm's GL surface). Its own F12 screenshot writes
# the clean PPU framebuffer but is focus-flaky over RDP for some ROMs. So this
# harness temporarily flips Mesen to its SOFTWARE renderer (which IS grabbable),
# grabs the window with the focus-independent helper, then restores the setting.
param(
  [string]$Rom = "build\unodos.nes",
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
  # enlarge the window so all 240 scanlines are visible, then grab it
  Add-Type @"
using System;using System.Runtime.InteropServices;
public class NesWin { [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int hh,bool r); }
"@
  # Mesen sizes its own window to the 2x frame; don't resize (resizing triggers
  # a fit-to-window rescale that clips a column). Just grab the natural window.
  & powershell -ExecutionPolicy Bypass -File $capture -Out $outPath -Window Mesen
} finally {
  if ($wasHardware) {
    (Get-Content $cfg -Raw) -replace '"UseSoftwareRenderer": true','"UseSoftwareRenderer": false' | Set-Content $cfg
  }
}
