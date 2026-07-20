# md5-disk.ps1 — MD5 the first N bytes of a physical disk (to confirm a flash landed).
param(
  [Parameter(Mandatory=$true)][int]$DiskNumber,
  [Parameter(Mandatory=$true)][long]$Length,
  [string]$Log = "C:\Users\arin\md5-disk.log"
)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RawM {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern SafeFileHandle CreateFile(string name, uint access, uint share,
      IntPtr sec, uint disp, uint flags, IntPtr templ);
}
"@
$h = [RawM]::CreateFile("\\.\PhysicalDrive$DiskNumber", [uint32]2147483648, [uint32]3, [IntPtr]::Zero, [uint32]3, 0, [IntPtr]::Zero)
if ($h.IsInvalid) { throw "open read failed (err $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
$fs  = New-Object System.IO.FileStream($h, [System.IO.FileAccess]::Read)
$md5 = [System.Security.Cryptography.MD5]::Create()
$buf = New-Object byte[] (4MB)
$remain = $Length
while ($remain -gt 0) {
  $want = [math]::Min([long]$buf.Length, $remain)
  $off = 0
  while ($off -lt $want) { $n = $fs.Read($buf,$off,[int]($want-$off)); if ($n -eq 0){break}; $off += $n }
  if ($off -eq 0) { break }
  $md5.TransformBlock($buf,0,$off,$null,0) | Out-Null
  $remain -= $off
}
$md5.TransformFinalBlock((New-Object byte[] 0),0,0) | Out-Null
$hash = ([BitConverter]::ToString($md5.Hash) -replace '-','').ToLower()
$fs.Close()
$msg = "disk$DiskNumber first $Length bytes MD5 = $hash"
Write-Host $msg
Set-Content -Path $Log -Value $hash
