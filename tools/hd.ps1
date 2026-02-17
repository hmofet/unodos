#Requires -RunAsAdministrator
# UnoDOS Hard Drive Image Writer (legacy wrapper)
# Now calls write.ps1 with HD image default
# Usage: .\hd.ps1 [-ImagePath path]

param(
    [string]$ImagePath
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$args = @()
if ($ImagePath) {
    $args += @("-ImagePath", $ImagePath)
} else {
    $args += @("-ImagePath", "$scriptDir\..\build\unodos-hd.img")
}
& "$scriptDir\write.ps1" @args
