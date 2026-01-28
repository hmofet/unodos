# hd.ps1 - Write UnoDOS hard drive image to CF card or hard drive
# REQUIRES ADMINISTRATOR PRIVILEGES
#
# Usage:
#   .\tools\hd.ps1 -ImagePath build\unodos-hd.img -DiskNumber N
#
# To find disk number:
#   Get-Disk
#
# WARNING: This will ERASE ALL DATA on the target disk!

param(
    [Parameter(Mandatory=$true)]
    [string]$ImagePath,

    [Parameter(Mandatory=$true)]
    [int]$DiskNumber
)

# Check if running as administrator
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "ERROR: This script requires administrator privileges." -ForegroundColor Red
    Write-Host "Please run PowerShell as Administrator and try again."
    exit 1
}

# Check if image file exists
if (-not (Test-Path $ImagePath)) {
    Write-Host "ERROR: Image file not found: $ImagePath" -ForegroundColor Red
    exit 1
}

# Get disk information
try {
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
} catch {
    Write-Host "ERROR: Disk $DiskNumber not found." -ForegroundColor Red
    Write-Host "Available disks:"
    Get-Disk | Format-Table Number, FriendlyName, Size, PartitionStyle
    exit 1
}

# Get image size
$imageInfo = Get-Item $ImagePath
$imageSizeMB = [math]::Round($imageInfo.Length / 1MB, 2)
$diskSizeMB = [math]::Round($disk.Size / 1MB, 2)

# Display warning
Write-Host ""
Write-Host "========================================" -ForegroundColor Yellow
Write-Host "     UnoDOS Hard Drive Image Writer     " -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Yellow
Write-Host ""
Write-Host "Image: $ImagePath ($imageSizeMB MB)"
Write-Host ""
Write-Host "Target Disk:"
Write-Host "  Number: $DiskNumber"
Write-Host "  Name:   $($disk.FriendlyName)"
Write-Host "  Size:   $diskSizeMB MB"
Write-Host "  Bus:    $($disk.BusType)"
Write-Host ""

# Safety checks
if ($disk.Size -lt $imageInfo.Length) {
    Write-Host "ERROR: Target disk is smaller than image file!" -ForegroundColor Red
    exit 1
}

# Extra warning for large disks (might be system drive)
if ($disk.Size -gt 32GB) {
    Write-Host "WARNING: Target disk is larger than 32GB." -ForegroundColor Yellow
    Write-Host "This might be a system drive or important storage."
    Write-Host ""
}

# Check for system/boot disk
if ($disk.IsBoot -or $disk.IsSystem) {
    Write-Host "ERROR: Cannot write to system/boot disk!" -ForegroundColor Red
    Write-Host "This disk contains your operating system."
    exit 1
}

# Final confirmation
Write-Host "WARNING: This will ERASE ALL DATA on disk $DiskNumber!" -ForegroundColor Red
Write-Host ""
$confirm = Read-Host "Type 'ERASE' to continue (or anything else to cancel)"
if ($confirm -ne "ERASE") {
    Write-Host "Operation cancelled."
    exit 0
}

# Take disk offline and clear
Write-Host ""
Write-Host "Preparing disk..."

try {
    # Set disk offline first
    Set-Disk -Number $DiskNumber -IsOffline $true -ErrorAction SilentlyContinue

    # Clear existing data
    Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction SilentlyContinue
} catch {
    # Ignore errors - disk might already be clear
}

# Write image to disk
Write-Host "Writing image to disk $DiskNumber..."
Write-Host "This may take a few minutes..."

$physicalPath = "\\.\PhysicalDrive$DiskNumber"

try {
    # Read entire image into memory
    $imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)

    # Open disk for raw write access
    $stream = [System.IO.File]::Open(
        $physicalPath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None
    )

    # Write in chunks with progress
    $chunkSize = 1024 * 1024  # 1MB chunks
    $bytesWritten = 0
    $totalBytes = $imageBytes.Length

    while ($bytesWritten -lt $totalBytes) {
        $remaining = $totalBytes - $bytesWritten
        $writeSize = [Math]::Min($chunkSize, $remaining)

        $stream.Write($imageBytes, $bytesWritten, $writeSize)
        $bytesWritten += $writeSize

        $percent = [math]::Round(($bytesWritten / $totalBytes) * 100)
        Write-Progress -Activity "Writing image" -Status "$percent% complete" -PercentComplete $percent
    }

    $stream.Flush()
    $stream.Close()

    Write-Progress -Activity "Writing image" -Completed

    Write-Host ""
    Write-Host "SUCCESS: $bytesWritten bytes written to disk $DiskNumber" -ForegroundColor Green

} catch {
    Write-Host ""
    Write-Host "ERROR: Failed to write image!" -ForegroundColor Red
    Write-Host $_.Exception.Message
    exit 1
}

# Set disk back online
try {
    Set-Disk -Number $DiskNumber -IsOffline $false -ErrorAction SilentlyContinue
} catch {
    # Ignore - might not need to do this
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "           Write Complete!              " -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "You can now:"
Write-Host "1. Remove the CF card/disk from this computer"
Write-Host "2. Install it in your target machine (via IDE adapter)"
Write-Host "3. Boot from the hard drive to run UnoDOS"
Write-Host ""
Write-Host "Note: Windows may ask to format the disk - click Cancel!"
