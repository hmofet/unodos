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
                        # Try WSL make first (faster)
                        $hasMake = $false
                        try {
                            wsl which make 2>&1 | Out-Null
                            if ($LASTEXITCODE -eq 0) {
                                Write-Host "Building with WSL make..." -ForegroundColor Yellow
                                wsl make clean
                                wsl make floppy144
                                if ($LASTEXITCODE -eq 0) {
                                    $hasMake = $true
                                    Write-Host "Build successful!" -ForegroundColor Green
                                }
                            }
                        } catch {
                            # WSL not available, skip
                        }

                        if (-not $hasMake) {
                            Write-Warning "WSL make not available - using pre-built image"
                            Write-Warning "For best results, build manually with: wsl make floppy144"
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
