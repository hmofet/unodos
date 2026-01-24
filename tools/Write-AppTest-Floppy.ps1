# Write-AppTest-Floppy.ps1
# Writes the app test floppy image (HELLO.BIN) to a physical floppy disk
# Run as Administrator in PowerShell

param(
    [string]$DriveLetter = "A"
)

$ErrorActionPreference = "Stop"

Write-Host "=== UnoDOS App Test Floppy Writer ===" -ForegroundColor Cyan
Write-Host ""

# Normalize drive letter
$DriveLetter = $DriveLetter.TrimEnd(':').ToUpper()
$DrivePath = "\\.\${DriveLetter}:"

# Find the repository root
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir

# Check for pre-built image first
$AppTestImage = Join-Path $RepoRoot "build\app-test.img"
$HelloBin = Join-Path $RepoRoot "build\hello.bin"

# If app-test.img doesn't exist, try to create it
if (-not (Test-Path $AppTestImage)) {
    Write-Host "App test image not found. Attempting to create..." -ForegroundColor Yellow

    # Check if hello.bin exists
    if (-not (Test-Path $HelloBin)) {
        Write-Host "ERROR: hello.bin not found at $HelloBin" -ForegroundColor Red
        Write-Host "Please build the project first: make apps" -ForegroundColor Yellow
        exit 1
    }

    # Try to create the image using Python
    $CreateScript = Join-Path $ScriptDir "create_app_test.py"
    if (Test-Path $CreateScript) {
        Write-Host "Creating app test image with Python..." -ForegroundColor Cyan
        python3 $CreateScript $AppTestImage $HelloBin
        if ($LASTEXITCODE -ne 0) {
            # Try python instead of python3
            python $CreateScript $AppTestImage $HelloBin
            if ($LASTEXITCODE -ne 0) {
                Write-Host "ERROR: Failed to create app test image" -ForegroundColor Red
                exit 1
            }
        }
    } else {
        Write-Host "ERROR: create_app_test.py not found" -ForegroundColor Red
        exit 1
    }
}

# Verify image exists and has correct size
if (-not (Test-Path $AppTestImage)) {
    Write-Host "ERROR: App test image not found at $AppTestImage" -ForegroundColor Red
    exit 1
}

$ImageSize = (Get-Item $AppTestImage).Length
$ExpectedSize = 1474560  # 1.44MB

if ($ImageSize -ne $ExpectedSize) {
    Write-Host "WARNING: Image size is $ImageSize bytes (expected $ExpectedSize)" -ForegroundColor Yellow
}

Write-Host "Image: $AppTestImage" -ForegroundColor Green
Write-Host "Size: $ImageSize bytes" -ForegroundColor Green
Write-Host "Target: Drive ${DriveLetter}:" -ForegroundColor Green
Write-Host ""

# Confirm with user
Write-Host "This will ERASE all data on floppy drive ${DriveLetter}:!" -ForegroundColor Red
$confirm = Read-Host "Continue? (y/N)"
if ($confirm -ne 'y' -and $confirm -ne 'Y') {
    Write-Host "Aborted." -ForegroundColor Yellow
    exit 0
}

# Check if running as administrator
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "ERROR: This script must be run as Administrator" -ForegroundColor Red
    Write-Host "Right-click PowerShell and select 'Run as Administrator'" -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "Writing to floppy..." -ForegroundColor Cyan

try {
    # Read the image file
    $imageData = [System.IO.File]::ReadAllBytes($AppTestImage)

    # Open the floppy drive for raw write
    $fileStream = [System.IO.File]::Open($DrivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)

    # Write the image
    $fileStream.Write($imageData, 0, $imageData.Length)
    $fileStream.Flush()
    $fileStream.Close()

    Write-Host ""
    Write-Host "SUCCESS! App test floppy written to drive ${DriveLetter}:" -ForegroundColor Green
    Write-Host ""
    Write-Host "This floppy contains:" -ForegroundColor Cyan
    Write-Host "  - HELLO.BIN (test application)" -ForegroundColor White
    Write-Host ""
    Write-Host "To test:" -ForegroundColor Cyan
    Write-Host "  1. Boot UnoDOS from drive A:" -ForegroundColor White
    Write-Host "  2. Press 'L' for app loader test" -ForegroundColor White
    Write-Host "  3. Swap floppies when prompted" -ForegroundColor White
    Write-Host "  4. Press any key to load HELLO.BIN" -ForegroundColor White
    Write-Host "  5. You should see an 'H' pattern on screen" -ForegroundColor White
}
catch {
    Write-Host ""
    Write-Host "ERROR: Failed to write to floppy" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host ""
    Write-Host "Troubleshooting:" -ForegroundColor Yellow
    Write-Host "  - Make sure a floppy disk is inserted" -ForegroundColor White
    Write-Host "  - Make sure the disk is not write-protected" -ForegroundColor White
    Write-Host "  - Make sure drive ${DriveLetter}: exists" -ForegroundColor White
    Write-Host "  - Run PowerShell as Administrator" -ForegroundColor White
    exit 1
}
