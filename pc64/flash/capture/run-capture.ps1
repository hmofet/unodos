<#
  run-capture.ps1 - trigger a headless flasher screenshot (no elevation needed).

  Requires the one-time setup (setup-capture-task.ps1, run once as admin).
  Fires the scheduled task, waits for the PNG, and reports where it landed.
#>
$ErrorActionPreference = 'Stop'
$out = "$env:USERPROFILE\unodos-flasher-shots\flasher-windows.png"

if (-not (Get-ScheduledTask -TaskName 'UnoDOSFlasherCapture' -ErrorAction SilentlyContinue)) {
  Write-Warning "Scheduled task 'UnoDOSFlasherCapture' is not registered. Run setup-capture-task.ps1 once as administrator first."
  exit 1
}

if (Test-Path $out) { Remove-Item $out -Force }
Start-ScheduledTask -TaskName 'UnoDOSFlasherCapture'

$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
  if (Test-Path $out) { Start-Sleep -Seconds 1; break }   # let the write finish
  Start-Sleep -Seconds 2
}

if (Test-Path $out) {
  Write-Host "Captured:" -ForegroundColor Green
  Get-Item $out | Select-Object FullName, Length, LastWriteTime
} else {
  Write-Warning "No screenshot was produced. See $env:USERPROFILE\unodos-flasher-shots\capture.log"
  exit 1
}
