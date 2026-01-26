#Requires -RunAsAdministrator
# UnoDOS Clock App Floppy Writer
# Usage: .\clock.ps1 [DriveLetter]

param(
    [string]$DriveLetter = "A"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

# Git pull latest
Write-Host "Pulling latest from GitHub..." -ForegroundColor Cyan
Push-Location $projectDir
try {
    git fetch origin 2>&1 | Out-Null
    git reset --hard origin/master 2>&1 | Out-Null
    Write-Host "Updated!" -ForegroundColor Green
} catch {
    Write-Host "Git failed, using local version" -ForegroundColor Yellow
}
Pop-Location

# Find clock-app image
$ImagePath = "$projectDir\build\clock-app.img"
if (-not (Test-Path $ImagePath)) {
    Write-Error "clock-app.img not found in build directory"
    exit 1
}

Write-Host "Writing clock-app.img to ${DriveLetter}:..." -ForegroundColor Cyan

$drivePath = "\\.\${DriveLetter}:"
$imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)
$stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $stream.Write($imageBytes, 0, $imageBytes.Length)
    $stream.Flush()
} finally {
    $stream.Close()
}

Write-Host "Done! Clock app floppy ready (contains HELLO.BIN)." -ForegroundColor Green
Write-Host ""
Write-Host "To test:" -ForegroundColor Yellow
Write-Host "  1. Boot UnoDOS from boot floppy"
Write-Host "  2. Press 'L' to load app (loads HELLO.BIN)"
Write-Host "  3. Swap floppies when prompted"
Write-Host "  4. Press ESC to exit clock app"
