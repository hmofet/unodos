# Create-TestFloppy.ps1
# Creates a FAT12 test floppy for UnoDOS filesystem testing
# Run as Administrator

param(
    [string]$Drive = "A:",
    [int]$FileSize = 1024  # Create 1024-byte file for multi-cluster test
)

# Check if running as Administrator
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "ERROR: This script must be run as Administrator!" -ForegroundColor Red
    Write-Host "Right-click PowerShell and select 'Run as Administrator'" -ForegroundColor Yellow
    exit 1
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "UnoDOS Test Floppy Creator" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check if drive exists
if (-not (Test-Path $Drive)) {
    Write-Host "ERROR: Drive $Drive not found!" -ForegroundColor Red
    Write-Host "Please insert a floppy disk into drive $Drive and try again." -ForegroundColor Yellow
    exit 1
}

Write-Host "Target drive: $Drive" -ForegroundColor Green
Write-Host ""

# Warn user about data loss
Write-Host "WARNING: This will FORMAT the floppy and ERASE ALL DATA!" -ForegroundColor Yellow
$confirm = Read-Host "Type 'YES' to continue, or anything else to cancel"
if ($confirm -ne "YES") {
    Write-Host "Operation cancelled." -ForegroundColor Yellow
    exit 0
}

Write-Host ""
Write-Host "Step 1: Formatting floppy as FAT12..." -ForegroundColor Cyan

# Format the floppy as FAT12
# Windows format command: format A: /FS:FAT /V:TEST /Q
$formatCmd = "format $Drive /FS:FAT /V:TEST /Q /Y"
Write-Host "Running: $formatCmd" -ForegroundColor Gray

try {
    # Use cmd.exe to run format command (PowerShell doesn't have native format)
    $process = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", "echo Y | format $Drive /FS:FAT /V:TEST /Q" -Wait -PassThru -NoNewWindow

    if ($process.ExitCode -ne 0) {
        Write-Host "ERROR: Format failed with exit code $($process.ExitCode)" -ForegroundColor Red
        exit 1
    }

    Write-Host "Format completed successfully!" -ForegroundColor Green
} catch {
    Write-Host "ERROR: Format failed: $_" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Step 2: Creating TEST.TXT file..." -ForegroundColor Cyan

# Create test file content
# Make it larger than 512 bytes to test multi-cluster reading
$content = "CLUSTER 1: " + ("A" * 500) + "`n"
$content += "CLUSTER 2: " + ("B" * 500) + "`n"

# Ensure file is at least the requested size
while ($content.Length -lt $FileSize) {
    $content += "Extra padding to reach multi-cluster size. "
}

# Trim to exact size
$content = $content.Substring(0, $FileSize)

# Write file to floppy
$filePath = Join-Path $Drive "TEST.TXT"
try {
    # Write as ASCII to match what FAT12 expects
    [System.IO.File]::WriteAllText($filePath, $content, [System.Text.Encoding]::ASCII)

    $fileInfo = Get-Item $filePath
    Write-Host "Created: $filePath" -ForegroundColor Green
    Write-Host "Size: $($fileInfo.Length) bytes" -ForegroundColor Green
    Write-Host "Clusters: $(([Math]::Ceiling($fileInfo.Length / 512)))" -ForegroundColor Green
} catch {
    Write-Host "ERROR: Failed to create TEST.TXT: $_" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Step 3: Verifying floppy..." -ForegroundColor Cyan

# Verify the file was created correctly
if (Test-Path $filePath) {
    $verify = Get-Content $filePath -Raw
    if ($verify.Length -ge 512) {
        Write-Host "Verification PASSED!" -ForegroundColor Green
        Write-Host "- File exists: YES" -ForegroundColor Green
        Write-Host "- Name format: TEST.TXT (8.3 format, uppercase)" -ForegroundColor Green
        Write-Host "- Size: $($verify.Length) bytes (multi-cluster)" -ForegroundColor Green
        Write-Host "- Content preview: $($verify.Substring(0, [Math]::Min(50, $verify.Length)))..." -ForegroundColor Gray
    } else {
        Write-Host "WARNING: File is smaller than expected" -ForegroundColor Yellow
    }
} else {
    Write-Host "ERROR: File verification failed - TEST.TXT not found!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "SUCCESS! Test floppy is ready." -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Testing Instructions:" -ForegroundColor Cyan
Write-Host "1. Boot UnoDOS from the first floppy" -ForegroundColor White
Write-Host "2. Wait for the keyboard demo prompt" -ForegroundColor White
Write-Host "3. Press F to start filesystem test" -ForegroundColor White
Write-Host "4. When prompted, remove UnoDOS floppy" -ForegroundColor White
Write-Host "5. Insert this TEST floppy (drive $Drive)" -ForegroundColor White
Write-Host "6. Press any key to continue" -ForegroundColor White
Write-Host "7. Observe test results:" -ForegroundColor White
Write-Host "   - Mount: OK" -ForegroundColor Gray
Write-Host "   - Open TEST.TXT: OK" -ForegroundColor Gray
Write-Host "   - Read: OK - File contents:" -ForegroundColor Gray
Write-Host "   - CLUSTER 1: AAA..." -ForegroundColor Gray
Write-Host "   - CLUSTER 2: BBB..." -ForegroundColor Gray
Write-Host ""
Write-Host "Expected: Both clusters should be displayed on screen" -ForegroundColor Yellow
Write-Host ""
