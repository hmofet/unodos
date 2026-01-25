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

# Ensure build directory exists
$buildDir = "$projectDir\build"
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

# Build clock.bin with NASM
Write-Host "Building clock.bin..." -ForegroundColor Cyan
$clockAsm = "$projectDir\apps\clock.asm"
$clockBin = "$buildDir\clock.bin"

if (-not (Test-Path $clockAsm)) {
    Write-Error "clock.asm not found in apps directory"
    exit 1
}

$nasmResult = & nasm -f bin -o $clockBin $clockAsm 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "NASM failed: $nasmResult"
    exit 1
}
Write-Host "Built clock.bin ($((Get-Item $clockBin).Length) bytes)" -ForegroundColor Green

# Create FAT12 floppy image with clock.bin
Write-Host "Creating clock-app.img..." -ForegroundColor Cyan
$clockImg = "$buildDir\clock-app.img"
$createScript = "$scriptDir\create_app_test.py"

$pythonResult = & python3 $createScript $clockImg $clockBin 2>&1
if ($LASTEXITCODE -ne 0) {
    # Try python instead of python3
    $pythonResult = & python $createScript $clockImg $clockBin 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Python failed: $pythonResult"
        exit 1
    }
}
Write-Host "Created clock-app.img" -ForegroundColor Green

# Write to floppy
Write-Host "Writing clock-app.img to ${DriveLetter}:..." -ForegroundColor Cyan

$drivePath = "\\.\${DriveLetter}:"
$imageBytes = [System.IO.File]::ReadAllBytes($clockImg)
$stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $stream.Write($imageBytes, 0, $imageBytes.Length)
    $stream.Flush()
} finally {
    $stream.Close()
}

Write-Host "Done! Clock app floppy ready (contains CLOCK.BIN)." -ForegroundColor Green
Write-Host ""
Write-Host "To test:" -ForegroundColor Yellow
Write-Host "  1. Boot UnoDOS from boot floppy"
Write-Host "  2. Type 'L' to list files on B:"
Write-Host "  3. Type 'R CLOCK.BIN' to run the clock"
Write-Host "  4. Press ESC to exit"
