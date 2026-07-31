<#
.SYNOPSIS
    Write a pc64 ESP tree to a USB stick as a single bootable FAT32 volume.

.DESCRIPTION
    The GUI flasher (build-flasher.ps1) embeds both ESP trees and is what you
    hand to a person. This is the headless equivalent for a machine you are
    already driving over SSH: point it at a disk number and an ESP directory
    and it partitions, formats and copies.

    IT REFUSES TO TOUCH A NON-USB DISK unless -Force is given. A disk number is
    a terrible safety interlock - it renumbers when you plug things in - so the
    bus type is checked as well, and the model and size are printed for the
    caller to eyeball before anything is destroyed. Getting this wrong wipes an
    internal drive, which is not a recoverable mistake.

    FAT32, MBR, one partition, marked active. That is what the standing rule in
    CLAUDE.md specifies and it is what UEFI removable-media boot wants: firmware
    looks for \EFI\BOOT\BOOTX64.EFI on the first FAT volume and needs no boot
    entry, no NVRAM change and no signature.

.PARAMETER DiskNumber
    Target disk. Check it with Get-Disk FIRST, every time.

.PARAMETER EspPath
    Directory whose CONTENTS become the volume root (i.e. it should contain
    EFI\BOOT\BOOTX64.EFI).

.PARAMETER ExtraPath
    Optional second directory copied to the volume root after the ESP - test
    media, skins, anything the stick should carry.

.PARAMETER Label
    Volume label. FAT32 allows 11 characters.

.EXAMPLE
    .\flash-stick.ps1 -DiskNumber 1 -EspPath C:\esp -ExtraPath C:\media
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$DiskNumber,
    [Parameter(Mandatory = $true)][string]$EspPath,
    [string]$ExtraPath,
    [string]$Label = "UNODOS",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path (Join-Path $EspPath "EFI\BOOT\BOOTX64.EFI"))) {
    throw "no EFI\BOOT\BOOTX64.EFI under $EspPath - that is not an ESP tree"
}

$disk = Get-Disk -Number $DiskNumber
$gb = [math]::Round($disk.Size / 1GB, 1)
Write-Host "target: disk $DiskNumber  $($disk.FriendlyName)  $gb GB  $($disk.BusType)"

# The interlock. USB-only by default; a big disk is suspicious even on USB.
if ($disk.BusType -ne "USB" -and -not $Force) {
    throw "disk $DiskNumber is $($disk.BusType), not USB - refusing (use -Force if you are certain)"
}
if ($disk.Size -gt 256GB -and -not $Force) {
    throw "disk $DiskNumber is $gb GB - too large for a stick, refusing (use -Force if you are certain)"
}
if ($disk.IsBoot -or $disk.IsSystem) {
    throw "disk $DiskNumber is the boot/system disk - refusing unconditionally"
}

Write-Host "clearing..."
Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction SilentlyContinue
# Clear-Disk leaves the style alone when the disk was already RAW.
try { Initialize-Disk -Number $DiskNumber -PartitionStyle MBR -ErrorAction Stop }
catch { Set-Disk -Number $DiskNumber -PartitionStyle MBR }

Write-Host "partitioning..."
$part = New-Partition -DiskNumber $DiskNumber -UseMaximumSize -IsActive -AssignDriveLetter
Start-Sleep -Seconds 2

Write-Host "formatting FAT32 as $Label..."
$vol = Format-Volume -Partition $part -FileSystem FAT32 -NewFileSystemLabel $Label -Confirm:$false -Force
$drive = "$($part.DriveLetter):"
Write-Host "volume $drive ready"

# Copy file by file rather than with one recursive Copy-Item.
#
# The ESP contains STRESS\CORPUS\NUL.MD, and NUL is a reserved DOS device name.
# Win32 path parsing rejects it outright, so Copy-Item aborts the whole tree
# partway through - which is how a "successful" flash ends up missing whatever
# came after it alphabetically. The fix is the \\?\ prefix, which bypasses that
# parsing entirely and lets the file be written under its real name. That file
# exists ON PURPOSE (UnoDOS tests reserved names), so dropping it would quietly
# weaken the corpus the stick is meant to carry.
function Copy-Tree($src, $dstRoot) {
    Get-ChildItem -LiteralPath $src -Recurse -File | ForEach-Object {
        # Bind the pipeline item to a NAMED variable before the try. Inside a
        # catch block $_ is the ErrorRecord, not the item, so using $_.FullName
        # in the fallback silently passes an empty source path.
        $f   = $_
        $rel = $f.FullName.Substring($src.Length).TrimStart("\")
        $dst = Join-Path $dstRoot $rel
        $dir = Split-Path $dst -Parent
        if (-not (Test-Path -LiteralPath $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
        try {
            Copy-Item -LiteralPath $f.FullName -Destination $dst -Force -ErrorAction Stop
        } catch {
            # Reserved name, or any other Win32 parsing objection.
            [System.IO.File]::Copy($f.FullName, "\\?\$dst", $true)
            Write-Host "  (via \\?\) $rel"
        }
    }
}

Write-Host "copying ESP..."
Copy-Tree $EspPath $drive

if ($ExtraPath -and (Test-Path $ExtraPath)) {
    Write-Host "copying extras..."
    Copy-Tree $ExtraPath $drive
}

# Verify per file rather than trusting the copy: a stick that reports success
# and boots to nothing is the failure this catches. Reserved names need the
# \\?\ prefix to be STAT'd too, not just written.
$bad = 0
Get-ChildItem -LiteralPath $EspPath -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($EspPath.Length).TrimStart("\")
    $dst = Join-Path $drive $rel
    $len = -1
    if (Test-Path -LiteralPath $dst) { $len = (Get-Item -LiteralPath $dst).Length }
    else {
        try { $len = (New-Object System.IO.FileInfo("\\?\$dst")).Length } catch { $len = -1 }
    }
    if ($len -lt 0) { Write-Host "MISSING $rel"; $bad++ }
    elseif ($len -ne $_.Length) { Write-Host "SIZE    $rel"; $bad++ }
}

$files = (Get-ChildItem -Path $drive -Recurse -File).Count
Write-Host ""
Write-Host "$drive  $files files  $bad mismatches"
if ($bad -gt 0) { throw "$bad files did not verify" }
Write-Host "OK - $($disk.FriendlyName) is bootable UnoDOS"
