# verify-win.ps1 — read back the first sectors of a physical disk and check the
# UnoDOS/PinePhone card signatures (MBR 0x55AA, eGON.BT0 SPL at byte 8196, FAT type).
param([Parameter(Mandatory=$true)][int]$DiskNumber)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RawR {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern SafeFileHandle CreateFile(string name, uint access, uint share,
      IntPtr sec, uint disp, uint flags, IntPtr templ);
}
"@
$GENERIC_READ = [uint32]2147483648   # 0x80000000
$SHARE_RW     = [uint32]3
$OPEN_EXISTING= [uint32]3
$h = [RawR]::CreateFile("\\.\PhysicalDrive$DiskNumber", $GENERIC_READ, $SHARE_RW, [IntPtr]::Zero, $OPEN_EXISTING, 0, [IntPtr]::Zero)
if ($h.IsInvalid) { throw "open read failed (err $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
$fs = New-Object System.IO.FileStream($h, [System.IO.FileAccess]::Read)
$buf = New-Object byte[] 16384
$off = 0
while ($off -lt $buf.Length) { $n = $fs.Read($buf,$off,$buf.Length-$off); if ($n -eq 0){break}; $off += $n }
$fs.Close()

$mbrOk = ($buf[510] -eq 0x55 -and $buf[511] -eq 0xAA)
$ptype = $buf[450]                                   # partition 1 type byte
$egon  = [System.Text.Encoding]::ASCII.GetString($buf, 8196, 8)
Write-Host ("MBR 0x55AA : {0}" -f $mbrOk)
Write-Host ("part1 type : 0x{0:X2}  (0x0C = FAT32-LBA)" -f $ptype)
Write-Host ("SPL @8196  : '{0}'  (expect eGON.BT0)" -f $egon)
if ($mbrOk -and $ptype -eq 0x0C -and $egon -eq 'eGON.BT0') { Write-Host "CARD_OK" } else { Write-Host "CARD_MISMATCH" }
