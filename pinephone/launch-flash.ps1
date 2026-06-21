# launch-flash.ps1 — start the flasher DETACHED so it survives the ssh session closing.
param(
  [int]$DiskNumber = 2,
  [string]$Image = "C:\Users\arin\pine-fresh.img"
)
Start-Process -FilePath "powershell" -ArgumentList @(
  "-NoProfile","-ExecutionPolicy","Bypass","-File","C:\Users\arin\flash-win.ps1",
  "-DiskNumber","$DiskNumber","-Image","$Image"
) -WindowStyle Hidden
Write-Host "LAUNCHED flasher (disk $DiskNumber, $Image) -> log C:\Users\arin\flash.log"
