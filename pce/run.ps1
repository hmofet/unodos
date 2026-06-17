# run.ps1 — launch the PCE ROM in Mesen2 and capture the framebuffer.
#
# Mesen renders through a GPU surface that a GDI/PrintWindow grab reads as black
# (same class of issue as BlastEm's GL surface). Its own F12 screenshot writes
# the clean PPU framebuffer but is focus-flaky over RDP for some ROMs. So this
# harness temporarily flips Mesen to its SOFTWARE renderer (which IS grabbable),
# grabs the window with the focus-independent helper, then restores the setting.
param(
  [string]$Rom = "build\min.pce",
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
  # Mesen sizes its own window to its chosen (3x) scale and renders top-left
  # anchored; on this 1280x720 host the right/bottom of the frame sits past the
  # window edge, so the grab shows the top-left ~21x17 cells. Apps + the Dostris
  # board are laid out in that region so the capture proves them. (A capture
  # cosmetic, not a render defect — the PPU composes the full 256x240.)
  & powershell -ExecutionPolicy Bypass -File $capture -Out $outPath -Window Mesen
} finally {
  if ($wasHardware) {
    (Get-Content $cfg -Raw) -replace '"UseSoftwareRenderer": true','"UseSoftwareRenderer": false' | Set-Content $cfg
  }
}
