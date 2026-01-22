#Requires -RunAsAdministrator
<#
.SYNOPSIS
    UnoDOS Floppy Writer for Windows PowerShell

.DESCRIPTION
    Writes UnoDOS disk images to physical floppy disks.
    Must be run as Administrator.

.PARAMETER ImagePath
    Path to the disk image file. Defaults to build\unodos-144.img

.PARAMETER DriveLetter
    Target floppy drive letter. Defaults to A

.PARAMETER NoVerify
    Skip verification after writing

.EXAMPLE
    .\Write-Floppy.ps1
    Writes default image to drive A:

.EXAMPLE
    .\Write-Floppy.ps1 -ImagePath build\unodos.img -DriveLetter B
    Writes specified image to drive B:

.NOTES
    Requires Administrator privileges for raw disk access.
#>

param(
    [string]$ImagePath,
    [string]$DriveLetter = "A",
    [switch]$NoVerify
)

$ErrorActionPreference = "Stop"

Write-Host "========================================"
Write-Host "UnoDOS Floppy Writer for Windows"
Write-Host "========================================"
Write-Host ""

# Find image file
if (-not $ImagePath) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $projectDir = Split-Path -Parent $scriptDir

    if (Test-Path "$projectDir\build\unodos-144.img") {
        $ImagePath = "$projectDir\build\unodos-144.img"
    }
    elseif (Test-Path "$projectDir\build\unodos.img") {
        $ImagePath = "$projectDir\build\unodos.img"
    }
    else {
        Write-Error "No image file found. Run 'make' first or specify -ImagePath"
        exit 1
    }
}

# Validate image exists
if (-not (Test-Path $ImagePath)) {
    Write-Error "Image file not found: $ImagePath"
    exit 1
}

$imageInfo = Get-Item $ImagePath
Write-Host "Image:  $($imageInfo.Name) ($($imageInfo.Length) bytes)"
Write-Host "Drive:  ${DriveLetter}:"
Write-Host ""

# Construct physical drive path
$drivePath = "\\.\${DriveLetter}:"

# Check if drive exists and is a floppy
try {
    $drive = Get-WmiObject Win32_LogicalDisk | Where-Object { $_.DeviceID -eq "${DriveLetter}:" }
    if (-not $drive) {
        Write-Warning "Drive ${DriveLetter}: not found. Make sure a floppy disk is inserted."
    }
    elseif ($drive.DriveType -ne 2) {
        Write-Warning "Drive ${DriveLetter}: may not be a removable drive (DriveType=$($drive.DriveType))"
    }
}
catch {
    Write-Warning "Could not verify drive type: $_"
}

# Confirm with user
Write-Host "WARNING: All data on drive ${DriveLetter}: will be destroyed!" -ForegroundColor Yellow
Write-Host ""
$confirm = Read-Host "Continue? (Y/N)"
if ($confirm -ne "Y" -and $confirm -ne "y") {
    Write-Host "Aborted."
    exit 0
}

Write-Host ""
Write-Host "Writing image to ${DriveLetter}:..."

try {
    # Read the image file
    $imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)

    # Open the floppy drive for writing
    $stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)

    try {
        $stream.Write($imageBytes, 0, $imageBytes.Length)
        $stream.Flush()
    }
    finally {
        $stream.Close()
    }

    Write-Host "Write complete!" -ForegroundColor Green
}
catch {
    Write-Host ""
    Write-Error "Failed to write to floppy: $_"
    Write-Host ""
    Write-Host "Troubleshooting:"
    Write-Host "  1. Make sure a floppy disk is inserted"
    Write-Host "  2. Check that the disk is not write-protected"
    Write-Host "  3. Verify you are running as Administrator"
    Write-Host "  4. Try a different floppy disk"
    exit 1
}

# Verify if requested
if (-not $NoVerify) {
    Write-Host ""
    Write-Host "Verifying..."

    try {
        $stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)

        try {
            $readBytes = New-Object byte[] $imageBytes.Length
            $bytesRead = $stream.Read($readBytes, 0, $imageBytes.Length)

            if ($bytesRead -ne $imageBytes.Length) {
                throw "Only read $bytesRead bytes, expected $($imageBytes.Length)"
            }

            $match = $true
            for ($i = 0; $i -lt $imageBytes.Length; $i++) {
                if ($imageBytes[$i] -ne $readBytes[$i]) {
                    $match = $false
                    break
                }
            }

            if ($match) {
                Write-Host "Verification PASSED!" -ForegroundColor Green
            }
            else {
                Write-Host "Verification FAILED! Disk may be bad." -ForegroundColor Red
                exit 1
            }
        }
        finally {
            $stream.Close()
        }
    }
    catch {
        Write-Warning "Could not verify: $_"
    }
}

Write-Host ""
Write-Host "========================================"
Write-Host "Success! Floppy is ready to boot."
Write-Host "========================================"
