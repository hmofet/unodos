#Requires -RunAsAdministrator
# Write UnoDOS Test Floppy - Single floppy with TEST.TXT included
# Usage: .\Write-Test-Floppy.ps1 [DriveLetter]

param(
    [string]$DriveLetter = "A"
)

$ErrorActionPreference = "Stop"

# Determine project directory
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

# Git pull
Write-Host "Checking for updates from GitHub..." -ForegroundColor Cyan
Push-Location $projectDir
try {
    Write-Host "Fetching latest changes..." -ForegroundColor Yellow
    git fetch origin 2>$null
    Write-Host "Pulling latest version (force)..." -ForegroundColor Yellow
    git reset --hard origin/master 2>$null
    Write-Host "Repository updated!" -ForegroundColor Green
} finally {
    Pop-Location
}

# Use the combined test image
$ImagePath = Join-Path $projectDir "build\unodos-test.img"

if (-not (Test-Path $ImagePath)) {
    Write-Host "Error: Test image not found at $ImagePath" -ForegroundColor Red
    exit 1
}

# Format drive letter
$DriveLetter = $DriveLetter.TrimEnd(':').ToUpper()
$drivePath = "\\.\${DriveLetter}:"

Write-Host "Writing unodos-test.img to ${DriveLetter}:..." -ForegroundColor Yellow
Write-Host "(This image includes TEST.TXT - no disk swap needed!)" -ForegroundColor Cyan

# Read image and write to floppy
$imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)
$handle = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $handle.Write($imageBytes, 0, $imageBytes.Length)
    $handle.Flush()
} finally {
    $handle.Close()
}

Write-Host "Done! Boot from this floppy and press F to test." -ForegroundColor Green
Write-Host "No disk swap needed - TEST.TXT is already on this floppy." -ForegroundColor Green
