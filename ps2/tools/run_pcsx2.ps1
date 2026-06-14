# Boot a PS2 ELF in PCSX2 and screen-capture the GS output window.
#
# VERIFIED 2026-06-14: PCSX2 v2.6.3 portable + 4 MB PS2 BIOS
# (ps2-0200a-20040614.bin) boots the EE ELF and renders the M0 splash on the
# real GS pipeline -> shots/m0_pcsx2.png.
#
# Two gotchas this script handles:
#  1. PCSX2 v2.x rejects a hand-authored PCSX2.ini that lacks
#     `[UI] SettingsVersion = 1` with a "Settings failed to load, or are the
#     incorrect version" modal that silently blocks the boot. We write a
#     known-good ini when that key is missing.
#  2. `-batch -nogui` yields only a ~400x80 status window with no GS surface.
#     Boot with `-fullscreen -fastboot -elf` and capture the GS window's
#     client area with CopyFromScreen (PrintWindow lies for GPU renderers -
#     the macplus snapscreen lesson).
param(
  [string]$Elf     = "C:\Users\arin\Documents\Github\unodos\ps2\build\unodos-ps2.elf",
  [string]$Out     = "C:\Users\arin\Documents\Github\unodos\ps2\shots\m0_pcsx2.png",
  [string]$Pcsx2   = "C:\Users\arin\ps2-tools\pcsx2\pcsx2-qt.exe",
  [string]$Bios    = "ps2-0200a-20040614.bin",
  [int]$WaitSec    = 16
)
$root = Split-Path $Pcsx2
$ini  = Join-Path $root "inis\PCSX2.ini"

# (1) self-heal: PCSX2 needs SettingsVersion or it refuses to boot.
if (-not (Test-Path $ini) -or -not (Select-String -Path $ini -Pattern 'SettingsVersion' -Quiet)) {
  New-Item -ItemType Directory -Force (Split-Path $ini) | Out-Null
  @"
[UI]
SettingsVersion = 1
SetupWizardIncomplete = false
ConfirmShutdown = false
StartFullscreen = false
HideMouseCursor = false

[Filenames]
BIOS = $Bios

[Folders]
Bios = bios
Snapshots = snaps

[EmuCore]
EnableFastBoot = true
EnableFastBootFastForward = false

[EmuCore/GS]
VsyncEnable = false
"@ | Set-Content -Encoding UTF8 $ini
  Write-Output "wrote known-good $ini"
}

Stop-Process -Name pcsx2-qt -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Start-Process -FilePath $Pcsx2 -ArgumentList @("-fullscreen","-fastboot","-elf",$Elf) | Out-Null
Start-Sleep -Seconds $WaitSec

# (2) capture the GS window's client area.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 public struct R { public int Left,Top,Right,Bottom; }
 public struct P { public int X,Y; }
}
"@
$proc = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) {
  Write-Output "NO_WINDOW - check $root\logs\emulog.txt"
} else {
  $h = $proc.MainWindowHandle
  Write-Output ("title=" + $proc.MainWindowTitle)
  [W]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 800
  $r = New-Object W+R; [W]::GetClientRect($h, [ref]$r) | Out-Null
  $pt = New-Object W+P; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
  $w = $r.Right; $hh = $r.Bottom
  if ($w -gt 0 -and $hh -gt 0) {
    $bmp = New-Object System.Drawing.Bitmap($w, $hh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size($w, $hh)))
    $g.Dispose(); $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Output ("saved $Out (" + $w + " x " + $hh + ")")
  } else { Write-Output ("BAD_SIZE " + $w + " x " + $hh) }
}
Stop-Process -Name pcsx2-qt -Force -ErrorAction SilentlyContinue
