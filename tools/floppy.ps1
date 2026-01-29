#Requires -RunAsAdministrator
# UnoDOS Floppy Writer
# Writes UnoDOS OS + Launcher to floppy disk
# Usage: .\floppy.ps1 [DriveLetter]

param(
    [string]$DriveLetter = "A",
    [switch]$Verify,
    [switch]$v
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

# Read version and build from image
$imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)
$imageText = [System.Text.Encoding]::ASCII.GetString($imageBytes)

# Search for version string (e.g., "UnoDOS v3.13.0")
$versionMatch = [regex]::Match($imageText, 'UnoDOS v[\d.]+')
$versionString = if ($versionMatch.Success) { $versionMatch.Value } else { "Unknown" }

# Search for build string (e.g., "Build: 078")
$buildMatch = [regex]::Match($imageText, 'Build: \d+')
$buildString = if ($buildMatch.Success) { $buildMatch.Value } else { "Unknown" }

Write-Host ""
Write-Host "Image: $(Split-Path -Leaf $ImagePath)" -ForegroundColor Cyan
Write-Host "  Version: $versionString" -ForegroundColor Cyan
Write-Host "  $buildString" -ForegroundColor Cyan
Write-Host ""
Write-Host "Writing to ${DriveLetter}:..." -ForegroundColor Yellow

$drivePath = "\\.\${DriveLetter}:"
$stream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $stream.Write($imageBytes, 0, $imageBytes.Length)
    $stream.Flush()
} finally {
    $stream.Close()
}

Write-Host "Done! UnoDOS floppy ready (OS + Launcher)." -ForegroundColor Green

# Verify if requested
if ($Verify -or $v) {
    Write-Host ""
    Write-Host "Verifying written data..." -ForegroundColor Yellow

    try {
        # Read back first 51200 bytes (100 sectors) from floppy
        $readStream = [System.IO.File]::Open($drivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $readBuffer = New-Object byte[] 51200
        $bytesRead = $readStream.Read($readBuffer, 0, 51200)
        $readStream.Close()

        # Compare with source
        $differences = 0
        for ($i = 0; $i -lt [Math]::Min($bytesRead, 51200); $i++) {
            if ($readBuffer[$i] -ne $imageBytes[$i]) {
                $differences++
                if ($differences -le 5) {
                    $sector = [Math]::Floor($i / 512)
                    $offset = $i % 512
                    Write-Host "  Byte $i (sector $sector, offset $offset): Expected 0x$($imageBytes[$i].ToString('X2')), Got 0x$($readBuffer[$i].ToString('X2'))"
                }
            }
        }

        if ($differences -eq 0) {
            Write-Host "Verification PASSED! First 100 sectors match perfectly." -ForegroundColor Green
        } else {
            Write-Host "Verification FAILED! Found $differences byte differences." -ForegroundColor Red
            if ($differences -gt 5) {
                Write-Host "  (showing first 5 differences)" -ForegroundColor Yellow
            }
        }
    }
    catch {
        Write-Host "Verification failed: $_" -ForegroundColor Red
        Write-Host "  Note: Some drives may not support immediate read-back" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Boot from this floppy - launcher will auto-load!" -ForegroundColor Yellow
