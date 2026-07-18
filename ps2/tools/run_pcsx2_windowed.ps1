# Boot a PS2 ELF in PCSX2 WINDOWED (DWM-composited so CopyFromScreen works over
# RDP) with the software GS renderer, and capture the GS output window.
#
# The fullscreen-exclusive path (run_pcsx2.ps1) bypasses DWM -> GDI capture
# returns a blank/invalid-handle grab over RDP. Windowed + software renderer
# composites through DWM, which CopyFromScreen can read.
param(
  [string]$Elf     = "C:\Users\arin\Documents\Github\unodos\ps2\build\unodos-ps2-uui.elf",
  [string]$Out     = "C:\Users\arin\Documents\Github\unodos\ps2\shots\aurora_pcsx2.png",
  [string]$Pcsx2   = "C:\Users\arin\ps2-tools\pcsx2\pcsx2-qt.exe",
  [string]$Bios    = "ps2-0200a-20040614.bin",
  [int]$WaitSec    = 20
)
$root = Split-Path $Pcsx2
$ini  = Join-Path $root "inis\PCSX2.ini"

# windowed + software renderer (Renderer=13) so the frame composites through DWM.
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
Renderer = 13
VsyncEnable = false
"@ | Set-Content -Encoding UTF8 $ini
Write-Output "wrote windowed/software ini $ini"

Stop-Process -Name pcsx2-qt -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Start-Process -FilePath $Pcsx2 -ArgumentList @("-fastboot","-elf",$Elf) | Out-Null
Start-Sleep -Seconds $WaitSec

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
 public struct R { public int Left,Top,Right,Bottom; }
}
"@
$proc = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) {
  Write-Output "NO_WINDOW - check $root\logs\emulog.txt"
} else {
  $h = $proc.MainWindowHandle
  Write-Output ("title=" + $proc.MainWindowTitle)
  [W]::ShowWindow($h, 9) | Out-Null   # SW_RESTORE
  [W]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 900
  $r = New-Object W+R; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
  if ($w -gt 0 -and $hh -gt 0) {
    $bmp = New-Object System.Drawing.Bitmap($w, $hh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $hh)))
    $g.Dispose(); $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Output ("saved $Out (" + $w + " x " + $hh + ")")
  } else { Write-Output ("BAD_SIZE " + $w + " x " + $hh) }
}
Stop-Process -Name pcsx2-qt -Force -ErrorAction SilentlyContinue
