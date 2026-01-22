#Requires -RunAsAdministrator
# Quick UnoDOS Floppy Writer - No verification
# Usage: .\Write-Floppy-Quick.ps1 [ImagePath] [DriveLetter]

param(
    [string]$ImagePath,
    [string]$DriveLetter = "A"
)

$ErrorActionPreference = "Stop"

# Find image file
if (-not $ImagePath) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $projectDir = Split-Path -Parent $scriptDir
    if (Test-Path "$projectDir\build\unodos-144.img") {
        $ImagePath = "$projectDir\build\unodos-144.img"
    } elseif (Test-Path "$projectDir\build\unodos.img") {
        $ImagePath = "$projectDir\build\unodos.img"
    } else {
        Write-Error "No image found. Run 'make' first."
        exit 1
    }
}

if (-not (Test-Path $ImagePath)) {
    Write-Error "Image not found: $ImagePath"
    exit 1
}

Write-Host "Writing $(Split-Path -Leaf $ImagePath) to ${DriveLetter}:..."

$drivePath = "\\.\${DriveLetter}:"
$imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)
$stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $stream.Write($imageBytes, 0, $imageBytes.Length)
    $stream.Flush()
} finally {
    $stream.Close()
}

Write-Host "Done!" -ForegroundColor Green
