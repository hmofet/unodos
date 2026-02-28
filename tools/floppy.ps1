# UnoDOS Floppy Writer (legacy wrapper)
# Now calls write.ps1 with floppy defaults
# Usage: .\floppy.ps1 [DriveLetter] [-Verify]

param(
    [string]$DriveLetter = "A",
    [switch]$Verify,
    [switch]$v
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$writeArgs = @{
    ImagePath   = "$scriptDir\..\build\unodos-144.img"
    DriveLetter = $DriveLetter
}
if ($Verify -or $v) { $writeArgs.Verify = $true }
& "$scriptDir\write.ps1" @writeArgs
