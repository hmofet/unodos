# launch_one.ps1 - Reliably launch ONE UnoDOS app on the emulated IBM PC/XT.
#
# Boots fresh, navigates the launcher grid from the clean idx0 state with
# deliberate gaps (SendKeys drops rapid keys under RDP), launches, waits for the
# slow 4.77MHz load, optionally sends post-launch keystrokes (typing/saving),
# then captures one MartyPC framebuffer PNG to build/xt/app_<Label>.png.
#
# Grid is 4 wide; idx = row*4+col. From boot the selection is idx0 (top-left),
# so -Downs/-Rights navigate straight to the target (no clamp needed).
#
#   launch_one.ps1 -Label clock -Rights 3 -LoadWait 6
#   launch_one.ps1 -Label notepad -Downs 2 -Rights 1 -LoadWait 7 -Post @("Hi XT")
param(
    [string]$Label = "app",
    [int]$Downs = 0,
    [int]$Rights = 0,
    [int]$LoadWait = 6,
    [int]$BootWait = 35,
    [string[]]$Post = @(),     # keystrokes sent (with gaps) AFTER the app loads
    [int]$PostWait = 2,        # seconds to wait after Post before capturing
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
Add-Type -MemberDefinition $sig -Name SW1 -Namespace SWO | Out-Null
$margs = "--machine-config-name $Machine --auto-poweron --no_sound -m fd:0:`"$img`""
$p = Start-Process -FilePath "$xt\martypc.exe" -ArgumentList $margs -WorkingDirectory $xt -PassThru `
        -RedirectStandardOutput "$xt\run_out.txt" -RedirectStandardError "$xt\run_err.txt"
function Focus { $h=(Get-Process -Id $p.Id).MainWindowHandle; [SWO.SW1]::ShowWindow($h,9)|Out-Null; [SWO.SW1]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 350 }
function K($k){ [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds 260 }
Write-Output "booting ($BootWait s)..."
Start-Sleep -Seconds $BootWait
Focus
for($i=0;$i -lt $Downs;$i++){ K "{DOWN}" }
for($i=0;$i -lt $Rights;$i++){ K "{RIGHT}" }
K "{ENTER}"
Start-Sleep -Seconds $LoadWait
if ($Post.Count -gt 0) { Focus; foreach($k in $Post){ K $k }; Start-Sleep -Seconds $PostWait }
Focus
[System.Windows.Forms.SendKeys]::SendWait("^{F5}")
Start-Sleep -Milliseconds 1100
$newest = Get-ChildItem $ssdir -Filter *.png | Sort-Object LastWriteTime | Select-Object -Last 1
Copy-Item $newest.FullName (Join-Path $dest "app_$Label.png") -Force
Write-Output "captured app_$Label.png ($($newest.Length) b)"
if (-not $KeepOpen) { $p.Kill() | Out-Null }
