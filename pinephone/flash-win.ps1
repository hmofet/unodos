# flash-win.ps1 — guarded raw image -> physical disk writer (Windows, no dd needed).
# Handles REMOVABLE media (SD cards) by locking + dismounting the volume(s) and
# holding the handle open while writing \\.\PhysicalDriveN directly.
# Usage:  powershell -ExecutionPolicy Bypass -File flash-win.ps1 -DiskNumber 2 -Image C:\path\to.img
# Safety: refuses non-USB / system / boot disks and anything larger than -MaxGB (default 8).
param(
  [Parameter(Mandatory=$true)][int]$DiskNumber,
  [Parameter(Mandatory=$true)][string]$Image,
  [double]$MaxGB = 8,
  [string]$Log = "C:\Users\arin\flash.log"
)
$ErrorActionPreference = 'Stop'
function Note($m) { $line = "{0}  {1}" -f (Get-Date -Format HH:mm:ss), $m; Write-Host $line; Add-Content -Path $Log -Value $line }
Set-Content -Path $Log -Value ("{0}  FLASH_START disk={1} image={2}" -f (Get-Date -Format HH:mm:ss), $DiskNumber, $Image)
try {

Add-Type @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class Raw {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern SafeFileHandle CreateFile(string name, uint access, uint share,
      IntPtr sec, uint disp, uint flags, IntPtr templ);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf,
      uint inSz, IntPtr outBuf, uint outSz, out uint ret, IntPtr ov);
}
"@

$GENERIC_READWRITE = [uint32]3221225472   # 0xC0000000
$GENERIC_WRITE     = [uint32]1073741824   # 0x40000000
$SHARE_RW          = [uint32]3
$OPEN_EXISTING     = [uint32]3
$FSCTL_LOCK        = [uint32]0x00090018
$FSCTL_DISMOUNT    = [uint32]0x00090020

$d = Get-Disk -Number $DiskNumber
$imgLen = (Get-Item $Image).Length
Note ("Target disk {0}: '{1}'  {2} bytes  BusType={3}  IsBoot={4} IsSystem={5}" -f `
  $DiskNumber, $d.FriendlyName, $d.Size, $d.BusType, $d.IsBoot, $d.IsSystem)
Note ("Image: {0}  ({1} bytes)" -f $Image, $imgLen)

if ($d.BusType -ne 'USB')       { throw "ABORT: disk $DiskNumber BusType=$($d.BusType), expected USB (refusing)" }
if ($d.IsSystem -or $d.IsBoot)  { throw "ABORT: disk $DiskNumber is system/boot (refusing)" }
if ($d.Size -gt ($MaxGB * 1GB)) { throw "ABORT: disk $DiskNumber is $($d.Size) bytes (> $MaxGB GB), refusing" }
if ($d.Size -lt $imgLen)        { throw "ABORT: disk ($($d.Size)) smaller than image ($imgLen)" }

# Lock + dismount every mounted volume on the disk, keeping the handles open.
$letters = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue |
             Where-Object { $_.DriveLetter } | ForEach-Object { $_.DriveLetter })
$volHandles = @()
foreach ($L in $letters) {
  Note "Locking + dismounting volume $L`:"
  $h = [Raw]::CreateFile("\\.\$L`:", $GENERIC_READWRITE, $SHARE_RW, [IntPtr]::Zero, $OPEN_EXISTING, 0, [IntPtr]::Zero)
  if ($h.IsInvalid) { throw "open volume $L failed (err $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
  $ret = 0
  [Raw]::DeviceIoControl($h, $FSCTL_LOCK,     [IntPtr]::Zero,0,[IntPtr]::Zero,0,[ref]$ret,[IntPtr]::Zero) | Out-Null
  [Raw]::DeviceIoControl($h, $FSCTL_DISMOUNT, [IntPtr]::Zero,0,[IntPtr]::Zero,0,[ref]$ret,[IntPtr]::Zero) | Out-Null
  $volHandles += $h
}

$ph = [Raw]::CreateFile("\\.\PhysicalDrive$DiskNumber", $GENERIC_WRITE, $SHARE_RW, [IntPtr]::Zero, $OPEN_EXISTING, 0, [IntPtr]::Zero)
if ($ph.IsInvalid) { throw "open PhysicalDrive$DiskNumber failed (err $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }

$dst = New-Object System.IO.FileStream($ph, [System.IO.FileAccess]::Write)
$src = [System.IO.File]::OpenRead($Image)
$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
  $buf = New-Object byte[] (4MB)
  $total = 0L
  while ($true) {
    $off = 0
    while ($off -lt $buf.Length) {
      $n = $src.Read($buf, $off, $buf.Length - $off)
      if ($n -eq 0) { break }
      $off += $n
    }
    if ($off -eq 0) { break }
    if ($off % 512 -ne 0) {                      # pad final short block to a sector
      $pad = 512 - ($off % 512)
      [Array]::Clear($buf, $off, $pad)
      $off += $pad
    }
    $dst.Write($buf, 0, $off)
    $total += $off
    if ($total % (128MB) -lt 4MB) { Note ("  {0} MB..." -f [math]::Round($total/1MB)) }
  }
  $dst.Flush()
  $sw.Stop()
  Note ("WROTE {0} bytes in {1}s ({2} MB/s)" -f $total, [math]::Round($sw.Elapsed.TotalSeconds,1), `
    [math]::Round(($total/1MB)/$sw.Elapsed.TotalSeconds,1))
} finally {
  $dst.Close(); $src.Close()
  foreach ($h in $volHandles) { $h.Close() }   # release volume locks
}
Note "FLASH_COMPLETE"
} catch {
  Note ("ERROR: " + $_.Exception.Message)
}
