#Requires -RunAsAdministrator
# UnoDOS Full Test Workflow
# Pulls latest, writes boot floppy, prompts for swap, writes launcher floppy

param(
    [string]$DriveLetter = "A"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

# Step 1: Git pull
Write-Host "`n=== Step 1: Pulling latest from GitHub ===" -ForegroundColor Cyan
Push-Location $projectDir
try {
    git fetch origin 2>&1 | Out-Null
    git reset --hard origin/master 2>&1 | Out-Null
    Write-Host "Updated to latest!" -ForegroundColor Green
} catch {
    Write-Host "Git failed, using local version" -ForegroundColor Yellow
}
Pop-Location

# Step 2: Write boot floppy
Write-Host "`n=== Step 2: Writing boot floppy ===" -ForegroundColor Cyan
& "$scriptDir\boot.ps1" -DriveLetter $DriveLetter

# Step 3: Wait for disk swap
Write-Host "`n=== Step 3: Swap disks ===" -ForegroundColor Yellow
Write-Host "Remove the boot floppy and insert a blank floppy for the launcher."
Write-Host "Press any key when ready..." -ForegroundColor Yellow
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")

# Step 4: Write launcher floppy
Write-Host "`n=== Step 4: Writing launcher floppy ===" -ForegroundColor Cyan
& "$scriptDir\launcher.ps1" -DriveLetter $DriveLetter

Write-Host "`n=== Done! ===" -ForegroundColor Green
Write-Host "You now have two floppies ready for testing."
Write-Host ""
Write-Host "Test procedure:" -ForegroundColor Yellow
Write-Host "  1. Insert boot floppy, boot the computer"
Write-Host "  2. Verify build number on screen"
Write-Host "  3. Press 'L' to load launcher"
Write-Host "  4. Swap to launcher floppy when prompted"
Write-Host "  5. Use W/S to select, Enter to launch Clock"
Write-Host "  6. Press ESC in clock to return to launcher"
