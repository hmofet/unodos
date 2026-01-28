#Requires -RunAsAdministrator
# UnoDOS Floppy Writer
# Writes UnoDOS OS + Launcher to floppy disk
# Usage: .\floppy.ps1 [DriveLetter]

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

# Find image
$ImagePath = "$projectDir\build\unodos-144.img"
if (-not (Test-Path $ImagePath)) {
    $ImagePath = "$projectDir\build\unodos.img"
}
if (-not (Test-Path $ImagePath)) {
    Write-Error "No image found in build directory"
    exit 1
}

Write-Host "Writing $(Split-Path -Leaf $ImagePath) to ${DriveLetter}:..." -ForegroundColor Cyan

$drivePath = "\\.\${DriveLetter}:"
$imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)
$stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $stream.Write($imageBytes, 0, $imageBytes.Length)
    $stream.Flush()
} finally {
    $stream.Close()
}

Write-Host "Done! UnoDOS floppy ready (OS + Launcher)." -ForegroundColor Green
Write-Host ""
Write-Host "Boot from this floppy - launcher will auto-load!" -ForegroundColor Yellow
