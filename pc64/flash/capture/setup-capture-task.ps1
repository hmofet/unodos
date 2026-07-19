<#
  setup-capture-task.ps1 - ONE-TIME setup. Run this once AS ADMINISTRATOR.

  Registers the scheduled task "UnoDOSFlasherCapture" that runs
  capture-flasher.ps1 with highest privileges in your interactive session. This
  is the one-time "approve to run as admin" step: afterwards the flasher (which
  requires Administrator) can be screenshotted headlessly with NO UAC prompt.

  After running this, capture any time (no elevation needed) with:
      Start-ScheduledTask -TaskName UnoDOSFlasherCapture
  or the convenience wrapper:
      powershell -ExecutionPolicy Bypass -File run-capture.ps1
#>
$ErrorActionPreference = 'Stop'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Warning "This must run elevated. Open PowerShell with 'Run as administrator' and run it again."
  exit 1
}

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'capture-flasher.ps1'
if (-not (Test-Path $script)) { throw "capture-flasher.ps1 not found next to this script ($script)" }

$userId    = [Security.Principal.WindowsIdentity]::GetCurrent().Name   # e.g. MACHINE\arin
$action    = New-ScheduledTaskAction -Execute 'powershell.exe' `
               -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}"' -f $script)
$principal = New-ScheduledTaskPrincipal -UserId $userId -LogonType Interactive -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable

Register-ScheduledTask -TaskName 'UnoDOSFlasherCapture' -Action $action -Principal $principal -Settings $settings -Force | Out-Null

Write-Host "Registered scheduled task 'UnoDOSFlasherCapture'." -ForegroundColor Green
Write-Host "Capture a screenshot with:  Start-ScheduledTask -TaskName UnoDOSFlasherCapture"
Write-Host ("Output PNG:                 {0}\unodos-flasher-shots\flasher-windows.png" -f $env:USERPROFILE)
