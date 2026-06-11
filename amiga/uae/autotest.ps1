# autotest.ps1 - relaunch WinUAE (taking foreground), boot, drive keys, screenshot
param([string]$OutDir = "C:\Users\arin\unodos\amiga\build")
$ErrorActionPreference = "Stop"
$uae = "C:\Users\arin\amiga-tools\winuae\winuae64.exe"
$cfg = "C:\Users\arin\unodos\amiga\uae\unodos.uae"
$snap = "C:\Users\arin\unodos\amiga\uae\snapwin.ps1"
$py = "C:\Users\arin\AppData\Local\Programs\Python\Python312\python.exe"
$si = "C:\Users\arin\unodos\amiga\uae\sendinput.py"

Stop-Process -Name winuae64 -Force -ErrorAction SilentlyContinue
Start-Sleep 1
Start-Process $uae -ArgumentList "-f", $cfg
Start-Sleep 22
powershell -ExecutionPolicy Bypass -File $snap -Out "$OutDir\k1_desktop.png"

& $py $si enter
Start-Sleep 2
powershell -ExecutionPolicy Bypass -File $snap -Out "$OutDir\k2_sysinfo.png"

& $py $si right enter
Start-Sleep 2
powershell -ExecutionPolicy Bypass -File $snap -Out "$OutDir\k3_clock.png"

Start-Sleep 4
powershell -ExecutionPolicy Bypass -File $snap -Out "$OutDir\k4_clock_tick.png"

& $py $si esc
Start-Sleep 2
powershell -ExecutionPolicy Bypass -File $snap -Out "$OutDir\k5_closed.png"

& $py $si esc
Start-Sleep 2
powershell -ExecutionPolicy Bypass -File $snap -Out "$OutDir\k6_desktop.png"
Write-Output "autotest done"
