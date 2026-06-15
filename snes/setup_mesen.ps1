# setup_mesen.ps1 - one-time Mesen2 prep for the headless screenshot rig.
#
# 1. Mesen shows a first-run "Configuration" dialog that blocks ROM display.
#    We dismiss it by invoking its CONFIRM button via UI Automation
#    (DPI-independent; synthesized clicks miss it on a scaled/RDP desktop).
# 2. Mesen draws the SNES picture on a GPU surface; PrintWindow returns it
#    black under a headless/RDP session. We force the software renderer so
#    the viewport is capturable (the Genesis "SDL_RENDER_DRIVER=software
#    under RDP" lesson, Mesen edition).
#
# Run once after installing Mesen. Safe to re-run.
param([string]$Mesen = "C:\Users\arin\snes-tools\mesen\Mesen.exe")
$ErrorActionPreference = "Stop"

# launch so it creates its data dir + shows the first-run dialog
Get-Process Mesen -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Start-Process -FilePath $Mesen
Start-Sleep -Seconds 5

# dismiss the first-run dialog via UI Automation
Add-Type -AssemblyName UIAutomationClient
$p = Get-Process Mesen | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($p) {
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)
    foreach ($b in $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) {
        if ($b.Current.Name -match 'CONFIRM') {
            $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
            Write-Output "dismissed first-run dialog"
        }
    }
}
Start-Sleep -Seconds 1
Get-Process Mesen -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
Start-Sleep -Seconds 2
Get-Process Mesen -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

# force the software renderer in settings.json
$cfg = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Mesen2\settings.json'
if (-not (Test-Path $cfg)) { throw "settings.json not found at $cfg - did Mesen launch?" }
$raw = Get-Content $cfg -Raw
$j = $raw | ConvertFrom-Json
$j.Video.UseSoftwareRenderer = $true
# preserve the UTF-8 BOM Mesen writes
$json = $j | ConvertTo-Json -Depth 40
[System.IO.File]::WriteAllText($cfg, $json, (New-Object System.Text.UTF8Encoding($true)))
Write-Output "UseSoftwareRenderer = true"
Write-Output "Mesen ready for the screenshot rig."
