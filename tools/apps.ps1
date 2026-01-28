#Requires -RunAsAdministrator
# UnoDOS Apps Floppy Writer
# Creates a floppy with ONLY apps (no OS) for swapping while launcher is running
# Usage: .\apps.ps1 [DriveLetter]

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

# Check if Python is available
$python = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Error "Python not found. Please install Python 3."
    exit 1
}

# Build app floppy image with all apps
Write-Host "Creating app floppy image..." -ForegroundColor Cyan
$appFiles = @(
    "build\launcher.bin", "LAUNCHER.BIN",
    "build\clock.bin", "CLOCK.BIN",
    "build\browser.bin", "BROWSER.BIN",
    "build\mouse_test.bin", "MOUSE.BIN",
    "build\hello.bin", "TEST.BIN"
)

# Check if app binaries exist
$missingApps = @()
for ($i = 0; $i -lt $appFiles.Length; $i += 2) {
    $appPath = "$projectDir\$($appFiles[$i])"
    if (-not (Test-Path $appPath)) {
        $missingApps += $appFiles[$i]
    }
}

if ($missingApps.Length -gt 0) {
    Write-Host "ERROR: Missing app binaries:" -ForegroundColor Red
    $missingApps | ForEach-Object { Write-Host "  $_" }
    Write-Host "`nRun 'git pull' to get latest binaries" -ForegroundColor Yellow
    exit 1
}

# Build the command line for create_app_test.py
$outputImg = "$projectDir\build\apps-floppy.img"
$cmdArgs = @($outputImg)
for ($i = 0; $i -lt $appFiles.Length; $i += 2) {
    $cmdArgs += "$projectDir\$($appFiles[$i])"
    $cmdArgs += $appFiles[$i + 1]
}

& $python.Source "$projectDir\tools\create_app_test.py" $cmdArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to create app floppy image"
    exit 1
}

# Write to floppy
Write-Host "Writing apps-floppy.img to ${DriveLetter}:..." -ForegroundColor Cyan

$drivePath = "\\.\${DriveLetter}:"
$imageBytes = [System.IO.File]::ReadAllBytes($outputImg)
$stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $stream.Write($imageBytes, 0, $imageBytes.Length)
    $stream.Flush()
} finally {
    $stream.Close()
}

Write-Host "Done! Apps floppy ready (Launcher, Clock, Browser, Mouse, Test)." -ForegroundColor Green
Write-Host ""
Write-Host "Usage:" -ForegroundColor Yellow
Write-Host "  1. Boot from UnoDOS floppy (OS + Launcher auto-loads)"
Write-Host "  2. Swap to this apps floppy when you want different apps"
Write-Host "  3. Launcher will refresh and show apps from this disk"
