# sweep_xt.ps1 - UnoDOS 8088 multi-app sweep on the emulated IBM PC/XT.
#
# Boots ONCE, then for each requested app: clamps the launcher selection to the
# top-left icon (4x Up + 4x Left), navigates Down/Right to the target icon,
# Enter to launch, waits for the (slow, 4.77MHz) load, screenshots, then ESC to
# return to the desktop. One MartyPC framebuffer PNG per app in build/xt/.
#
# App spec: "label:downs:rights:loadwait[:closekey]"
#   closekey defaults to {ESC}; use "" to leave it open (for multitasking tests)
#
# Example (the CGA roster):
#   sweep_xt.ps1 -BootWait 35 -Apps @(
#     "files:1:0:5","clock:0:3:4","paint:0:2:5","music:1:1:4","notepad:2:1:5","pacman:3:0:6")
param(
    [int]$BootWait = 35,
    [string[]]$Apps = @(),
    [string]$Machine = "unodos_xt",
    [string]$Img = "build/unodos-144.img",
    [switch]$KeepOpen
)
$ErrorActionPreference = "Stop"
$repo  = (Resolve-Path "$PSScriptRoot\..\..").Path
$xt    = "C:\Users\arin\xt-tools"
$ssdir = "$xt\output\screenshots"
$img   = Join-Path $repo $Img
$dest  = Join-Path $repo "build\xt"
New-Item -ItemType Directory -Force $dest | Out-Null

Get-Process martypc -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

Add-Type -AssemblyName System.Windows.Forms
$sig = @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
'@
Add-Type -MemberDefinition $sig -Name SW -Namespace SWX | Out-Null

$margs = "--machine-config-name $Machine --auto-poweron --no_sound -m fd:0:`"$img`""
$p = Start-Process -FilePath "$xt\martypc.exe" -ArgumentList $margs -WorkingDirectory $xt -PassThru `
        -RedirectStandardOutput "$xt\run_out.txt" -RedirectStandardError "$xt\run_err.txt"

function Focus {
    $h = (Get-Process -Id $p.Id).MainWindowHandle
    [SWX.SW]::ShowWindow($h,9) | Out-Null
    [SWX.SW]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 400
}
function Send($k) { [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds 220 }

Write-Output "booting ($BootWait s)..."
Start-Sleep -Seconds $BootWait

foreach ($spec in $Apps) {
    $parts = $spec.Split(":")
    $label = $parts[0]; $downs = [int]$parts[1]; $rights = [int]$parts[2]; $lw = [int]$parts[3]
    $closekey = if ($parts.Count -ge 5) { $parts[4] } else { "{ESC}" }
    Focus
    # Clamp to top-left icon (navigation clamps at grid edges), then navigate.
    for ($i=0;$i -lt 4;$i++){ Send "{UP}" }
    for ($i=0;$i -lt 4;$i++){ Send "{LEFT}" }
    for ($i=0;$i -lt $downs;$i++){ Send "{DOWN}" }
    for ($i=0;$i -lt $rights;$i++){ Send "{RIGHT}" }
    Send "{ENTER}"
    Start-Sleep -Seconds $lw
    Focus
    [System.Windows.Forms.SendKeys]::SendWait("^{F5}")
    Start-Sleep -Milliseconds 1000
    $newest = Get-ChildItem $ssdir -Filter *.png | Sort-Object LastWriteTime | Select-Object -Last 1
    Copy-Item $newest.FullName (Join-Path $dest "app_$label.png") -Force
    Write-Output "captured app_$label.png ($($newest.Length) b)"
    if (-not $KeepOpen -and $closekey -ne "") { Focus; Send $closekey; Start-Sleep -Seconds 2 }
}

if (-not $KeepOpen) { $p.Kill() | Out-Null }
Write-Output "done"
