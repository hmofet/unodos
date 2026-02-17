#Requires -RunAsAdministrator
# UnoDOS Apps Floppy Writer (legacy wrapper)
# Now calls write.ps1 with launcher-floppy.img
# Usage: .\apps.ps1 [DriveLetter]

param(
    [string]$DriveLetter = "A"
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
& "$scriptDir\write.ps1" -ImagePath "$scriptDir\..\build\launcher-floppy.img" -DriveLetter $DriveLetter
