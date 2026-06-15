# shot_xt.ps1 - UnoDOS 8088 port capture harness (MartyPC cycle-accurate XT)
#
# Boots build/unodos-144.img on an emulated IBM PC/XT (8088 @ 4.77MHz, CGA)
# and grabs MartyPC's own framebuffer screenshots (Ctrl+F5) at the requested
# wait points. MartyPC renders the PNG from the emulated CGA framebuffer, so
# capture is clean under RDP (unlike a GPU window grab - the SNES/Mesen trap).
#
# Usage:
#   shot_xt.ps1 -Waits 16,26,40 -OutDir build/xt -Prefix boot -Machine unodos_xt
#
param(
    [int[]]$Waits = @(16),
    [string]$OutDir = "build/xt",
    [string]$Prefix = "shot",
    [string]$Machine = "unodos_xt",
    [string]$Img = "build/unodos-144.img",
    [string[]]$Keys = @()   # optional keystrokes sent to the machine before captures
)

$ErrorActionPreference = "Stop"
$repo  = (Resolve-Path "$PSScriptRoot\..\..").Path
$xt    = "C:\Users\arin\xt-tools"
$ssdir = "$xt\output\screenshots"
$img   = Join-Path $repo $Img
$dest  = Join-Path $repo $OutDir
New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem $ssdir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force

$sig = @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
'@
$u = Add-Type -MemberDefinition $sig -Name UXT -Namespace WXT -PassThru
Add-Type -AssemblyName System.Windows.Forms

function Focus-Marty($proc) {
    $h = (Get-Process -Id $proc.Id).MainWindowHandle
    $null = $u::ShowWindow($h, 9)        # SW_RESTORE
    $null = $u::BringWindowToTop($h)
    $null = $u::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 500
}

$margs = "--machine-config-name $Machine --auto-poweron --no_sound -m fd:0:`"$img`""
$p = Start-Process -FilePath "$xt\martypc.exe" -ArgumentList $margs `
        -WorkingDirectory $xt -PassThru `
        -RedirectStandardOutput "$xt\run_out.txt" -RedirectStandardError "$xt\run_err.txt"

$prev = 0
$idx = 0
foreach ($w in $Waits) {
    Start-Sleep -Seconds ([Math]::Max(0, $w - $prev))
    $prev = $w
    Focus-Marty $p
    if ($Keys.Count -gt 0 -and $idx -lt $Keys.Count) {
        $k = $Keys[$idx]
        if ($k) { [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds 400; Focus-Marty $p }
    }
    [System.Windows.Forms.SendKeys]::SendWait("^{F5}")
    Start-Sleep -Milliseconds 1200
    $idx++
}

$p.Kill() | Out-Null
Start-Sleep -Milliseconds 300

# Rename the captured PNGs (screenshot0000.png ...) to <prefix>_<wait>s.png
$shots = Get-ChildItem $ssdir -Filter *.png | Sort-Object Name
for ($i = 0; $i -lt $shots.Count -and $i -lt $Waits.Count; $i++) {
    $out = Join-Path $dest ("{0}_{1}s.png" -f $Prefix, $Waits[$i])
    Copy-Item $shots[$i].FullName $out -Force
    Write-Output "saved $out ($($shots[$i].Length) bytes)"
}
