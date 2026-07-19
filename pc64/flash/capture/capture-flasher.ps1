<#
  capture-flasher.ps1 - headlessly screenshot the UnoDOS Windows USB flasher.

  Downloads the latest released UnoDosFlasher.exe, launches it, captures its
  main window with the Win32 PrintWindow API (which renders the window even when
  the physical display is off/asleep), then closes it and deletes the download.
  It NEVER clicks Install, so no drive is ever written.

  The flasher requires Administrator, so this script must run elevated. It is
  meant to be driven by the scheduled task registered by setup-capture-task.ps1,
  which runs it with highest privileges and no UAC prompt. Safe to run per release.

  Output: <OutDir>\flasher-windows.png  (+ capture.log)
#>
param(
  [string]$Url        = 'https://github.com/hmofet/unodos/releases/latest/download/UnoDosFlasher.exe',
  [string]$OutDir     = "$env:USERPROFILE\unodos-flasher-shots",
  [string]$Title      = 'UnoDOS - USB Installer',
  [int]   $TimeoutSec = 40
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $OutDir 'capture.log'
function Log($m) { "$(Get-Date -Format o)  $m" | Tee-Object -FilePath $log -Append | Out-Null }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;
public static class UnoCap {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(string title) { return FindWindow(null, title); }
  public static string Shot(IntPtr h, string path) {
    RECT r; GetWindowRect(h, out r);
    int w = r.R - r.L, ht = r.B - r.T;
    if (w < 40 || ht < 40) return "window too small";
    var b = new Bitmap(w, ht, PixelFormat.Format32bppArgb);
    using (var g = Graphics.FromImage(b)) {
      IntPtr d = g.GetHdc();
      PrintWindow(h, d, 2);   /* PW_RENDERFULLCONTENT: paints even off-screen */
      g.ReleaseHdc(d);
    }
    b.Save(path, ImageFormat.Png);
    b.Dispose();
    return w + "x" + ht;
  }
}
"@ -ReferencedAssemblies System.Drawing

$exe = Join-Path $env:TEMP ('UnoDosFlasher-{0}.exe' -f ([guid]::NewGuid().ToString('N')))
$proc = $null
try {
  Log "downloading $Url"
  Invoke-WebRequest -Uri $Url -OutFile $exe -UseBasicParsing
  Log ("downloaded {0:N0} bytes" -f (Get-Item $exe).Length)

  Log "launching flasher (elevated)"
  $proc = Start-Process -FilePath $exe -PassThru

  $h = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    $h = [UnoCap]::Find($Title)
    if ($h -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 300
  }
  if ($h -eq [IntPtr]::Zero) { throw "flasher window '$Title' did not appear within $TimeoutSec s" }

  Log "window found; letting the USB drive scan settle"
  Start-Sleep -Seconds 2

  $out = Join-Path $OutDir 'flasher-windows.png'
  $dim = [UnoCap]::Shot($h, $out)
  Log "captured $out ($dim)"
}
finally {
  # close the flasher WITHOUT ever clicking Install (nothing is written)
  if ($proc) { try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch {} }
  Get-Process -Name 'UnoDosFlasher*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Remove-Item $exe -Force -ErrorAction SilentlyContinue   # do not keep the downloaded build artifact
  Log "cleaned up"
}
