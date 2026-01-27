#Requires -RunAsAdministrator
# UnoDOS Launcher App Floppy Writer
# Usage: .\launcher.ps1 [DriveLetter]

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

# Find launcher floppy image
$ImagePath = "$projectDir\build\launcher-floppy.img"
if (-not (Test-Path $ImagePath)) {
    Write-Error "launcher-floppy.img not found in build directory"
    exit 1
}

Write-Host "Writing launcher-floppy.img to ${DriveLetter}:..." -ForegroundColor Cyan

$drivePath = "\\.\${DriveLetter}:"
$imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)
$stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $stream.Write($imageBytes, 0, $imageBytes.Length)
    $stream.Flush()
} finally {
    $stream.Close()
}

Write-Host "Done! Launcher floppy ready (LAUNCHER.BIN, CLOCK.BIN, TEST.BIN)." -ForegroundColor Green
Write-Host ""
Write-Host "To test:" -ForegroundColor Yellow
Write-Host "  1. Boot UnoDOS from boot floppy"
Write-Host "  2. Press 'L' to load app (loads launcher)"
Write-Host "  3. Swap floppies when prompted"
Write-Host "  4. Use W/S to navigate, Enter to launch Clock"
Write-Host "  5. Press ESC in clock to return to launcher"
