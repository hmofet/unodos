# snap_variant.ps1 - boot the disk-app rig (DF0 = a variant kernel ADF, DF1 =
# the FAT12 app disk) in WinUAE and screenshot the window. The AUTOTEST kernels
# auto-launch their app off DF1 at boot, so no key injection is needed.
param(
  [string]$Adf  = "C:\Users\arin\Documents\Github\unodos\amiga\build\unodos68k_test.adf",
  [string]$Out  = "C:\Users\arin\Documents\Github\unodos\amiga\build\shot.png",
  [int]$BootWait = 24
)
$ErrorActionPreference = "Stop"
$uae  = "C:\Users\arin\amiga-tools\winuae\winuae64.exe"
$cfg  = "C:\Users\arin\Documents\Github\unodos\amiga\uae\unodos_diskapp.uae"
$snap = "C:\Users\arin\Documents\Github\unodos\amiga\uae\snapwin.ps1"

Stop-Process -Name winuae64 -Force -ErrorAction SilentlyContinue
Start-Sleep 1
# DF0 floppy override on the command line so we can swap kernels per variant
Start-Process $uae -ArgumentList "-f", $cfg, "-s", "floppy0=$Adf"
Start-Sleep $BootWait
powershell -ExecutionPolicy Bypass -File $snap -Out $Out
Start-Sleep 1
Stop-Process -Name winuae64 -Force -ErrorAction SilentlyContinue
Write-Output "snapped $Out from $Adf"
