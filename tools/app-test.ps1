#Requires -RunAsAdministrator
# UnoDOS App Test Floppy Writer
# Usage: .\app-test.ps1 [DriveLetter]

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

# Find app-test image
$ImagePath = "$projectDir\build\app-test.img"
if (-not (Test-Path $ImagePath)) {
    Write-Error "app-test.img not found in build directory"
    exit 1
}

Write-Host "Writing app-test.img to ${DriveLetter}:..." -ForegroundColor Cyan

$drivePath = "\\.\${DriveLetter}:"
$imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)
$stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $stream.Write($imageBytes, 0, $imageBytes.Length)
    $stream.Flush()
} finally {
    $stream.Close()
}

Write-Host "Done! App test floppy ready (contains HELLO.BIN)." -ForegroundColor Green
