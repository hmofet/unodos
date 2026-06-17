# run.ps1 — launch the GBA ROM in mGBA and capture the framebuffer.
#
# mGBA renders through a surface a GDI/PrintWindow grab may read as black; if so
# we fall back to mGBA's own screenshot. We grab the window with the focus-
# independent helper (cc-capture). RDP-aware per ~/.claude/CLAUDE.md.
param(
  [string]$Rom = "build\unodos.gba",
  [string]$Out = "build\desktop.png",
  [int]$Seconds = 5
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$romPath = Join-Path $here $Rom
$outPath = Join-Path $here $Out
$capture = Join-Path $env:USERPROFILE '.claude\tools\cc-capture.ps1'

# locate mGBA
$mgba = @(
  "C:\Users\arin\gba-tools\mGBA-0.10.5-win64\mGBA.exe",
  "C:\Program Files\mGBA\mGBA.exe",
  "C:\Program Files (x86)\mGBA\mGBA.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $mgba) {
  $cmd = Get-Command mGBA -ErrorAction SilentlyContinue
  if ($cmd) { $mgba = $cmd.Source }
}
if (-not $mgba) { Write-Error "mGBA.exe not found"; exit 1 }

Get-Process mGBA -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Start-Process -FilePath $mgba -ArgumentList "-C","skipBios=1","-C","pauseOnFocusLost=0","-C","hwaccelVideo=0","`"$romPath`""
Start-Sleep -Seconds $Seconds
# foreground the mGBA window so emulation runs (mGBA pauses when inactive)
Add-Type @"
using System;using System.Runtime.InteropServices;
public class Fg { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
"@
$p = Get-Process mGBA | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($p) { [Fg]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 1500 }
& powershell -ExecutionPolicy Bypass -File $capture -Out $outPath -Window mGBA
