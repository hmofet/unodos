#Requires -RunAsAdministrator
# UnoDOS Floppy Writer (legacy wrapper)
# Now calls write.ps1 with floppy defaults
# Usage: .\floppy.ps1 [DriveLetter] [-Verify]

param(
    [string]$DriveLetter = "A",
    [switch]$Verify,
    [switch]$v
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$args = @("-ImagePath", "$scriptDir\..\build\unodos-144.img", "-DriveLetter", $DriveLetter)
if ($Verify -or $v) { $args += "-Verify" }
& "$scriptDir\write.ps1" @args
