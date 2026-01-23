#Requires -RunAsAdministrator
# Quick UnoDOS Floppy Writer - No verification
# Usage: .\Write-Floppy-Quick.ps1 [ImagePath] [DriveLetter]

param(
    [string]$ImagePath,
    [string]$DriveLetter = "A",
    [switch]$SkipPull
)

$ErrorActionPreference = "Stop"

# Determine project directory
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

# Git pull and rebuild if not skipped
if (-not $SkipPull) {
    Write-Host "Checking for updates from GitHub..." -ForegroundColor Cyan

    Push-Location $projectDir
    try {
        # Check if this is a git repository
        $isGitRepo = Test-Path ".git"
        if ($isGitRepo) {
            # Fetch latest changes (with retry)
            Write-Host "Fetching latest changes..." -ForegroundColor Yellow
            $fetchSuccess = $false
            for ($i = 1; $i -le 3; $i++) {
                try {
                    git fetch origin 2>&1 | Out-Null
                    $fetchSuccess = $true
                    break
                } catch {
                    if ($i -lt 3) {
                        Write-Host "Fetch failed, retrying ($i/3)..." -ForegroundColor Yellow
                        Start-Sleep -Seconds 2
                    }
                }
            }

            if (-not $fetchSuccess) {
                Write-Warning "Failed to fetch after 3 attempts"
                Write-Host "Continuing with current version..." -ForegroundColor Yellow
            } else {
                # Force pull (reset to origin/master)
                Write-Host "Pulling latest version (force)..." -ForegroundColor Yellow
                git reset --hard origin/master
                if ($LASTEXITCODE -ne 0) {
                    Write-Warning "Git reset failed!"
                    Write-Host "Continuing with current version..." -ForegroundColor Yellow
                } else {
                    Write-Host "Repository updated!" -ForegroundColor Green

                    # Rebuild the image
                    Write-Host "Building image..." -ForegroundColor Cyan
                    if (Test-Path "Makefile") {
                        # Check if WSL is installed
                        $wslInstalled = $false
                        try {
                            wsl --version 2>&1 | Out-Null
                            if ($LASTEXITCODE -eq 0) {
                                $wslInstalled = $true
                            }
                        } catch {
                            # WSL not available
                        }

                        if (-not $wslInstalled) {
                            Write-Warning "WSL not installed!"
                            Write-Host ""
                            Write-Host "WSL is required to build UnoDOS images." -ForegroundColor Yellow
                            Write-Host "To install WSL:" -ForegroundColor Cyan
                            Write-Host "  1. Open PowerShell as Administrator" -ForegroundColor White
                            Write-Host "  2. Run: wsl --install" -ForegroundColor White
                            Write-Host "  3. Restart your computer" -ForegroundColor White
                            Write-Host "  4. Run this script again" -ForegroundColor White
                            Write-Host ""
                            Write-Host "Using existing pre-built image (may be outdated)..." -ForegroundColor Yellow
                        } else {
                            # Check if make is available
                            $hasMake = $false
                            try {
                                wsl which make 2>&1 | Out-Null
                                if ($LASTEXITCODE -eq 0) {
                                    $hasMake = $true
                                }
                            } catch {
                                # Make not found
                            }

                            if (-not $hasMake) {
                                Write-Warning "Build tools not installed in WSL!"
                                Write-Host ""
                                Write-Host "Installing build tools (nasm, make)..." -ForegroundColor Cyan
                                wsl sudo apt-get update
                                wsl sudo apt-get install -y nasm make
                                if ($LASTEXITCODE -eq 0) {
                                    Write-Host "Build tools installed!" -ForegroundColor Green
                                    $hasMake = $true
                                } else {
                                    Write-Warning "Failed to install build tools"
                                    Write-Host "Please run manually in WSL: sudo apt-get install nasm make" -ForegroundColor Yellow
                                    Write-Host "Using existing pre-built image (may be outdated)..." -ForegroundColor Yellow
                                }
                            }

                            if ($hasMake) {
                                Write-Host "Building with WSL make..." -ForegroundColor Yellow
                                wsl make clean
                                wsl make floppy144
                                if ($LASTEXITCODE -eq 0) {
                                    Write-Host "Build successful!" -ForegroundColor Green
                                } else {
                                    Write-Error "Build failed! Check output above for errors."
                                    exit 1
                                }
                            }
                        }
                    }
                }
            }
        } else {
            Write-Host "Not a git repository, skipping update." -ForegroundColor Yellow
        }
    } catch {
        Write-Warning "Git operation failed: $_"
        Write-Host "Continuing with current version..." -ForegroundColor Yellow
    } finally {
        Pop-Location
    }
}

# Find image file
if (-not $ImagePath) {
    if (Test-Path "$projectDir\build\unodos-144.img") {
        $ImagePath = "$projectDir\build\unodos-144.img"
    } elseif (Test-Path "$projectDir\build\unodos.img") {
        $ImagePath = "$projectDir\build\unodos.img"
    } else {
        Write-Error "No image found in build directory."
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
