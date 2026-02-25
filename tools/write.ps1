#Requires -RunAsAdministrator
# UnoDOS Disk Image Writer - Unified TUI
# Writes floppy or HD images to any target drive
#
# Usage: .\tools\write.ps1 [-ImagePath path] [-DriveLetter X] [-DiskNumber N] [-Verify]
#
# No arguments = full interactive mode (image selection + drive selection)

param(
    [string]$ImagePath,
    [string]$DriveLetter,
    [int]$DiskNumber = -1,
    [switch]$Verify
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

# ─── TUI Helper Functions ────────────────────────────────────────────────────

function Write-At {
    param([int]$X, [int]$Y, [string]$Text,
          [ConsoleColor]$FG = [Console]::ForegroundColor,
          [ConsoleColor]$BG = [Console]::BackgroundColor)
    [Console]::SetCursorPosition($X, $Y)
    $oldFG = [Console]::ForegroundColor
    $oldBG = [Console]::BackgroundColor
    [Console]::ForegroundColor = $FG
    [Console]::BackgroundColor = $BG
    [Console]::Write($Text)
    [Console]::ForegroundColor = $oldFG
    [Console]::BackgroundColor = $oldBG
}

function Clear-Row {
    param([int]$Y, [int]$StartX = 0)
    $w = [Console]::WindowWidth
    Write-At $StartX $Y (" " * ($w - $StartX))
}

function Format-Size {
    param([long]$Bytes)
    if ($Bytes -le 0) { return "? MB" }
    $gb = [math]::Round($Bytes / 1GB, 1)
    $mb = [math]::Round($Bytes / 1MB, 1)
    if ($gb -ge 1) { return "$gb GB" } else { return "$mb MB" }
}

function Truncate-String {
    param([string]$Str, [int]$Max)
    if ($null -eq $Str) { return "" }
    if ($Str.Length -le $Max) { return $Str }
    return $Str.Substring(0, $Max - 1) + "~"
}

function Draw-Banner {
    param([int]$Y = 0)
    $lines = @(
        "+========================================+",
        "|        UnoDOS Disk Writer               |",
        "+========================================+"
    )
    $w = [Console]::WindowWidth
    $pad = [Math]::Max(0, [int](($w - $lines[0].Length) / 2))
    for ($i = 0; $i -lt $lines.Count; $i++) {
        Write-At $pad ($Y + $i) $lines[$i] Yellow
    }
    return ($Y + $lines.Count)
}

# ─── Drive Detection ─────────────────────────────────────────────────────────

function Get-FloppyDrives {
    $floppies = @()

    # Method 1: WMI Win32_LogicalDisk — look for removable drives with small size or A:/B: letters
    try {
        $logicalDisks = Get-CimInstance Win32_LogicalDisk -ErrorAction SilentlyContinue | Where-Object {
            ($_.DeviceID -match '^[AB]:$') -or
            ($_.DriveType -eq 2 -and $_.Size -gt 0 -and $_.Size -le 3MB)
        }
        foreach ($ld in $logicalDisks) {
            $letter = $ld.DeviceID.TrimEnd(':')
            $name = if ($ld.VolumeName) { $ld.VolumeName } else { "Floppy Drive ($letter`:)" }
            $size = if ($ld.Size -gt 0) { $ld.Size } else { 1474560 }
            $floppies += [PSCustomObject]@{
                TargetType = "Floppy"
                Letter     = $letter
                DiskNumber = -1
                Name       = $name
                Size       = $size
                BusType    = "Floppy"
                WritePath  = "\\.\${letter}:"
            }
        }
    } catch {}

    # Method 2: WMI Win32_DiskDrive — look for floppy media type
    try {
        $physFloppy = Get-CimInstance Win32_DiskDrive -ErrorAction SilentlyContinue | Where-Object {
            $_.MediaType -like '*Floppy*'
        }
        foreach ($pd in $physFloppy) {
            # Map to drive letter via partition/logical disk association
            try {
                $partitions = Get-CimInstance -Query "ASSOCIATORS OF {Win32_DiskDrive.DeviceID='$($pd.DeviceID)'} WHERE AssocClass=Win32_DiskDriveToDiskPartition" -ErrorAction SilentlyContinue
                foreach ($part in $partitions) {
                    $logicals = Get-CimInstance -Query "ASSOCIATORS OF {Win32_DiskPartition.DeviceID='$($part.DeviceID)'} WHERE AssocClass=Win32_LogicalDiskToPartition" -ErrorAction SilentlyContinue
                    foreach ($log in $logicals) {
                        $letter = $log.DeviceID.TrimEnd(':')
                        # Skip if already found in Method 1
                        if (-not ($floppies | Where-Object { $_.Letter -eq $letter })) {
                            $floppies += [PSCustomObject]@{
                                TargetType = "Floppy"
                                Letter     = $letter
                                DiskNumber = -1
                                Name       = $pd.Caption
                                Size       = $pd.Size
                                BusType    = "Floppy"
                                WritePath  = "\\.\${letter}:"
                            }
                        }
                    }
                }
            } catch {}
        }
    } catch {}

    # Method 3: Probe A: and B: directly if not already found
    foreach ($letter in @('A', 'B')) {
        if (-not ($floppies | Where-Object { $_.Letter -eq $letter })) {
            $testPath = "${letter}:\"
            if (Test-Path $testPath -ErrorAction SilentlyContinue) {
                $floppies += [PSCustomObject]@{
                    TargetType = "Floppy"
                    Letter     = $letter
                    DiskNumber = -1
                    Name       = "Floppy Drive ($letter`:)"
                    Size       = 1474560
                    BusType    = "Floppy"
                    WritePath  = "\\.\${letter}:"
                }
            }
        }
    }

    return $floppies
}

function Get-DiskDrives {
    $drives = @()
    try {
        $allDisks = @(Get-Disk | Where-Object { -not $_.IsBoot -and -not $_.IsSystem })
        foreach ($d in $allDisks) {
            $drives += [PSCustomObject]@{
                TargetType = "Disk"
                Letter     = ""
                DiskNumber = $d.Number
                Name       = $d.FriendlyName
                Size       = $d.Size
                BusType    = "$($d.BusType)"
                WritePath  = "\\.\PhysicalDrive$($d.Number)"
            }
        }
    } catch {}
    return $drives
}

function Get-AllWriteTargets {
    $targets = @()
    $targets += @(Get-FloppyDrives)
    $targets += @(Get-DiskDrives)
    return $targets
}

# ─── Image Utilities ─────────────────────────────────────────────────────────

function Get-ImageInfo {
    param([string]$Path)
    $info = Get-Item $Path
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    $vMatch = [regex]::Match($text, 'UnoDOS v[\d.]+')
    $bMatch = [regex]::Match($text, 'Build: \d+')
    return [PSCustomObject]@{
        Path       = $Path
        FileName   = $info.Name
        Bytes      = $bytes
        Size       = $info.Length
        Version    = if ($vMatch.Success) { $vMatch.Value } else { "" }
        Build      = if ($bMatch.Success) { $bMatch.Value } else { "" }
    }
}

function Get-ImageDescription {
    param([string]$FileName)
    switch -Wildcard ($FileName) {
        "unodos-144*"      { return "OS + Apps (floppy)" }
        "launcher-floppy*" { return "Apps-only floppy" }
        "unodos-hd*"       { return "HD / CF card" }
        "apps-floppy*"     { return "Apps-only floppy" }
        default            { return "" }
    }
}

function Find-AvailableImages {
    $buildDir = "$projectDir\build"
    $images = @()
    # Look for known images in preferred order
    $names = @("unodos-144.img", "launcher-floppy.img", "unodos-hd.img")
    foreach ($name in $names) {
        $path = "$buildDir\$name"
        if (Test-Path $path) {
            $images += $path
        }
    }
    # Also find any other .img files not already listed
    $others = Get-ChildItem "$buildDir\*.img" -ErrorAction SilentlyContinue | Where-Object { $_.Name -notin $names }
    foreach ($other in $others) {
        $images += $other.FullName
    }
    return $images
}

# ─── Render Functions ────────────────────────────────────────────────────────

function Render-ImageList {
    param([array]$Images, [int]$Sel, [int]$Top)
    for ($i = 0; $i -lt $Images.Count; $i++) {
        $img = $Images[$i]
        $y = $Top + $i
        $name = (Split-Path -Leaf $img.Path).PadRight(22)
        $size = (Format-Size $img.Size).PadRight(10)
        $ver = if ($img.Version) { $img.Version } else { "" }
        $bld = if ($img.Build) { $img.Build } else { "" }
        $desc = Get-ImageDescription (Split-Path -Leaf $img.Path)
        $verStr = "$ver $bld".Trim()

        $lineW = [Math]::Min(78, [Console]::WindowWidth - 3)
        # Two-column layout: name+size on left, version+desc on right
        $descCol = 8 + 20 + 8 + [Math]::Max(26, $verStr.Length + 1)
        if ($descCol -gt $lineW - 10) { $descCol = $lineW - 10 }
        if ($i -eq $Sel) {
            Write-At 3 $y (" " * $lineW) Black White
            Write-At 4 $y "[>]" Black White
            Write-At 8 $y (Truncate-String $name 19).PadRight(20) Black White
            Write-At 28 $y (Format-Size $img.Size).PadRight(8) Black White
            Write-At 36 $y $verStr Black White
            Write-At $descCol $y (Truncate-String $desc ($lineW - $descCol)) Black White
        } else {
            Clear-Row $y 3
            Write-At 4 $y "[ ]" DarkGray
            Write-At 8 $y (Truncate-String $name 19).PadRight(20) Gray
            Write-At 28 $y (Format-Size $img.Size).PadRight(8) DarkGray
            Write-At 36 $y $verStr DarkCyan
            Write-At $descCol $y (Truncate-String $desc ($lineW - $descCol)) DarkGray
        }
    }
}

function Render-DriveList {
    param([array]$Drives, [int]$Sel, [int]$Top, [long]$ImageSize)

    for ($i = 0; $i -lt $Drives.Count; $i++) {
        $d = $Drives[$i]
        $y = $Top + $i
        $type = $d.TargetType.PadRight(8)
        $id = if ($d.TargetType -eq "Floppy") { "$($d.Letter):".PadRight(5) } else { "$($d.DiskNumber)".PadRight(5) }
        $name = (Truncate-String $d.Name 30).PadRight(30)
        $size = (Format-Size $d.Size).PadRight(10)
        $bus = if ($d.BusType) { "$($d.BusType)".PadRight(8) } else { "?".PadRight(8) }

        # Floppy drives: WMI reports filesystem capacity (smaller than raw device),
        # so skip the too-small check — raw floppy device is always 1.44MB
        $tooSmall = ($d.TargetType -ne "Floppy" -and $d.Size -gt 0 -and $d.Size -lt $ImageSize)
        $isLarge = ($d.Size -gt 256GB)
        $suffix = ""
        if ($tooSmall) { $suffix = "(too small)" }
        elseif ($isLarge) { $suffix = "[!] >256GB" }

        $lineW = [Math]::Min(78, [Console]::WindowWidth - 3)

        if ($i -eq $Sel) {
            Write-At 3 $y (" " * $lineW) Black White
            Write-At 4 $y $type Black White
            Write-At 13 $y $id Black White
            Write-At 19 $y $name Black White
            Write-At 50 $y $size Black White
            Write-At 61 $y $bus Black White
            if ($suffix) { Write-At 70 $y $suffix Black Yellow }
        } elseif ($isLarge) {
            Clear-Row $y 3
            Write-At 4 $y $type DarkGray
            Write-At 13 $y $id DarkGray
            Write-At 19 $y $name DarkGray
            Write-At 50 $y $size DarkGray
            Write-At 61 $y $bus DarkGray
            Write-At 70 $y $suffix DarkYellow
        } elseif ($tooSmall) {
            Clear-Row $y 3
            Write-At 4 $y $type DarkGray
            Write-At 13 $y $id DarkGray
            Write-At 19 $y $name DarkGray
            Write-At 50 $y $size DarkGray
            Write-At 61 $y $bus DarkGray
            Write-At 70 $y $suffix DarkRed
        } else {
            Clear-Row $y 3
            Write-At 4 $y $type Gray
            Write-At 13 $y $id Gray
            Write-At 19 $y $name Gray
            Write-At 50 $y $size DarkGray
            Write-At 61 $y $bus DarkGray
        }
    }

    # Info area below list
    $infoRow = $Top + $Drives.Count + 1
    Clear-Row $infoRow
    Clear-Row ($infoRow + 1)

    if ($Drives.Count -gt 0) {
        $d = $Drives[$Sel]
        if ($d.Size -gt 0 -and $d.Size -lt $ImageSize) {
            Write-At 3 $infoRow "ERROR: Drive is too small for this image!" Red
        }
        elseif ($d.Size -gt 256GB) {
            Write-At 3 $infoRow "WARNING: Drive is larger than 256 GB - verify this is correct!" Yellow
        }
        elseif ($d.Size -gt 32GB) {
            Write-At 3 $infoRow "NOTE: Drive is larger than 32 GB" DarkYellow
        }
    }
}

function Render-Buttons {
    param([int]$Sel, [int]$Y)
    if ($Sel -eq 0) {
        Write-At 10 $Y "[ YES ]" Black Green
        Write-At 22 $Y "  No    " Gray
    } else {
        Write-At 10 $Y "  Yes  " Gray
        Write-At 22 $Y "[  NO  ]" Black Red
    }
}

# ─── Main ────────────────────────────────────────────────────────────────────

$origBG = [Console]::BackgroundColor
$origFG = [Console]::ForegroundColor
$origCursorVisible = [Console]::CursorVisible
[Console]::CursorVisible = $false

try {
    # ── Screen 1: Startup ────────────────────────────────────────────────────
    [Console]::Clear()
    $row = Draw-Banner
    $row++

    Start-Sleep -Milliseconds 300

    # ── Resolve command-line arguments ────────────────────────────────────
    $selectedImage = $null
    $selectedTarget = $null

    if ($ImagePath) {
        if (-not (Test-Path $ImagePath)) {
            Write-At 3 $row "ERROR: Image not found: $ImagePath" Red
            $row += 2
            Write-At 3 $row "Press any key to exit..." DarkGray
            [Console]::ReadKey($true) | Out-Null
            exit 1
        }
        $ImagePath = (Resolve-Path $ImagePath).Path
        $selectedImage = Get-ImageInfo $ImagePath
    }

    if ($DriveLetter) {
        $selectedTarget = [PSCustomObject]@{
            TargetType = "Floppy"
            Letter     = $DriveLetter.TrimEnd(':')
            DiskNumber = -1
            Name       = "Floppy Drive ($DriveLetter`:)"
            Size       = 1474560
            BusType    = "Floppy"
            WritePath  = "\\.\$($DriveLetter.TrimEnd(':')):"
        }
    }
    elseif ($DiskNumber -ge 0) {
        $disk = Get-Disk -Number $DiskNumber
        $selectedTarget = [PSCustomObject]@{
            TargetType = "Disk"
            Letter     = ""
            DiskNumber = $DiskNumber
            Name       = $disk.FriendlyName
            Size       = $disk.Size
            BusType    = "$($disk.BusType)"
            WritePath  = "\\.\PhysicalDrive$DiskNumber"
        }
    }

    # ── Interactive screen navigation ────────────────────────────────────
    # Determine starting screen based on CLI args
    $firstScreen = "image"
    if ($selectedImage) { $firstScreen = "drive" }
    if ($selectedImage -and $selectedTarget) { $firstScreen = "confirm" }
    $navScreen = $firstScreen

    $confirmed = $false
    $imageInfos = $null

    while ($true) {  # Outer loop: allows writing multiple images

    while ($true) {  # Navigation loop
        switch ($navScreen) {

            "image" {
                # ── Screen: Image Selection ──────────────────────────────
                [Console]::Clear()
                $row = Draw-Banner
                $row++

                Write-At 3 $row "Select image to write:" White
                Write-At 55 $row "Step 1 of 3" DarkGray
                $row++
                Write-At 3 $row "Up/Down = navigate   Enter = next   G = git pull   Esc = exit" DarkGray
                $row += 2

                if (-not $imageInfos) {
                    $imagePaths = @(Find-AvailableImages)
                    if ($imagePaths.Count -eq 0) {
                        Write-At 3 $row "No images found in build/ directory!" Red
                        $row++
                        Write-At 3 $row "Run 'git pull' to get latest binaries." Yellow
                        $row += 2
                        Write-At 3 $row "Press any key to exit..." DarkGray
                        [Console]::ReadKey($true) | Out-Null
                        exit 1
                    }
                    Write-At 3 $row "Loading image info..." Cyan
                    $imageInfos = @()
                    foreach ($ip in $imagePaths) {
                        $imageInfos += Get-ImageInfo $ip
                    }
                    Clear-Row $row
                }

                $listTop = $row
                $imgSel = 0
                Render-ImageList $imageInfos $imgSel $listTop

                $screenDone = $false
                while (-not $screenDone) {
                    $key = [Console]::ReadKey($true)
                    if ($key.Key -eq 'UpArrow' -and $imgSel -gt 0) {
                        $imgSel--
                        Render-ImageList $imageInfos $imgSel $listTop
                    }
                    elseif ($key.Key -eq 'DownArrow' -and $imgSel -lt ($imageInfos.Count - 1)) {
                        $imgSel++
                        Render-ImageList $imageInfos $imgSel $listTop
                    }
                    elseif ($key.Key -eq 'Enter') {
                        $selectedImage = $imageInfos[$imgSel]
                        $navScreen = "drive"
                        $screenDone = $true
                    }
                    elseif ($key.Key -eq 'Escape') {
                        [Console]::Clear()
                        [Console]::CursorVisible = $origCursorVisible
                        Write-Host "Exited."
                        exit 0
                    }
                    elseif ($key.Key -eq 'G') {
                        $pullRow = $listTop + $imageInfos.Count + 2
                        Clear-Row $pullRow
                        Write-At 3 $pullRow "Running git pull..." Cyan
                        try {
                            $gitOutput = & git -C $projectDir pull 2>&1 | Out-String
                            Clear-Row $pullRow
                            $pullMsg = $gitOutput.Trim()
                            if ($pullMsg.Length -gt 70) { $pullMsg = $pullMsg.Substring(0, 70) }
                            Write-At 3 $pullRow $pullMsg Green
                        } catch {
                            Clear-Row $pullRow
                            Write-At 3 $pullRow "Git pull failed!" Red
                        }
                        Start-Sleep -Milliseconds 1500
                        $imageInfos = $null
                        $navScreen = "image"
                        $screenDone = $true
                    }
                }
            }

            "drive" {
                # ── Screen: Drive Selection ──────────────────────────────
                [Console]::Clear()
                $row = Draw-Banner
                $row++

                $imageSizeMB = [math]::Round($selectedImage.Size / 1MB, 1)
                $canGoBack = ($firstScreen -eq "image")
                $stepNum = if ($canGoBack) { 2 } else { 1 }
                $stepTotal = if ($canGoBack) { 3 } else { 2 }

                Write-At 3 $row "Image:  $($selectedImage.FileName) ($imageSizeMB MB)" White
                $row++
                $verLine = "$($selectedImage.Version) $($selectedImage.Build)".Trim()
                if ($verLine) {
                    Write-At 3 $row "        $verLine" Cyan
                    $row++
                }
                $row++

                Write-At 3 $row "Select target drive:" White
                Write-At 55 $row "Step $stepNum of $stepTotal" DarkGray
                $row++
                $hints = "Up/Down = navigate   Enter = next   R = refresh"
                if ($canGoBack) { $hints += "   Bksp = back" }
                $hints += "   Esc = cancel"
                Write-At 3 $row $hints DarkGray
                $row += 2

                # Table header
                Write-At 4 $row "Type" DarkCyan
                Write-At 13 $row "#" DarkCyan
                Write-At 19 $row "Name" DarkCyan
                Write-At 50 $row "Size" DarkCyan
                Write-At 61 $row "Bus" DarkCyan
                $row++
                $w = [Console]::WindowWidth
                Write-At 3 $row ("-" * [Math]::Min(74, $w - 6)) DarkGray
                $row++

                $targets = @(Get-AllWriteTargets)

                if ($targets.Count -eq 0) {
                    Write-At 3 $row "No eligible drives found!" Red
                    $row++
                    Write-At 3 $row "(System and boot drives are automatically excluded)" DarkGray
                    $row += 2
                    Write-At 3 $row "Connect a floppy, CF card, or USB drive and try again." Yellow
                    $row += 2
                    Write-At 3 $row "Press any key to exit..." DarkGray
                    [Console]::ReadKey($true) | Out-Null
                    exit 1
                }

                $listTop = $row
                $drvSel = 0
                Render-DriveList $targets $drvSel $listTop $selectedImage.Size

                $screenDone = $false
                while (-not $screenDone) {
                    $key = [Console]::ReadKey($true)
                    if ($key.Key -eq 'UpArrow' -and $drvSel -gt 0) {
                        $drvSel--
                        Render-DriveList $targets $drvSel $listTop $selectedImage.Size
                    }
                    elseif ($key.Key -eq 'DownArrow' -and $drvSel -lt ($targets.Count - 1)) {
                        $drvSel++
                        Render-DriveList $targets $drvSel $listTop $selectedImage.Size
                    }
                    elseif ($key.Key -eq 'Enter') {
                        $t = $targets[$drvSel]
                        # Block if drive too small (skip for floppies — WMI reports FS capacity, not raw)
                        if ($t.TargetType -ne "Floppy" -and $t.Size -gt 0 -and $t.Size -lt $selectedImage.Size) {
                            # Flash error, don't proceed
                        } else {
                            $selectedTarget = $t
                            $navScreen = "confirm"
                            $screenDone = $true
                        }
                    }
                    elseif ($key.Key -eq 'Backspace' -and $canGoBack) {
                        $navScreen = "image"
                        $screenDone = $true
                    }
                    elseif ($key.Key -eq 'Escape') {
                        [Console]::Clear()
                        [Console]::CursorVisible = $origCursorVisible
                        Write-Host "Operation cancelled."
                        exit 0
                    }
                    elseif ($key.Key -eq 'R') {
                        $navScreen = "drive"
                        $screenDone = $true
                    }
                }
            }

            "confirm" {
                # ── Screen: Confirmation ─────────────────────────────────
                [Console]::Clear()
                $row = Draw-Banner
                $row++

                $imageSizeMB = [math]::Round($selectedImage.Size / 1MB, 1)
                $canGoBackToDrive = (-not $DriveLetter -and $DiskNumber -lt 0)

                # Step indicator
                $stepTotal = 1
                if ($firstScreen -eq "image") { $stepTotal = 3 }
                elseif ($firstScreen -eq "drive") { $stepTotal = 2 }

                Write-At 3 $row "CONFIRM WRITE" Red
                Write-At 55 $row "Step $stepTotal of $stepTotal" DarkGray
                $row += 2

                Write-At 5 $row "Image:" DarkGray
                Write-At 14 $row "$($selectedImage.FileName) ($imageSizeMB MB)" White
                $row++
                $verLine = "$($selectedImage.Version) $($selectedImage.Build)".Trim()
                if ($verLine) {
                    Write-At 14 $row $verLine Cyan
                    $row++
                }
                $row++

                $targetLabel = if ($selectedTarget.TargetType -eq "Floppy") {
                    "$($selectedTarget.Letter): - $($selectedTarget.Name)"
                } else {
                    "Disk $($selectedTarget.DiskNumber) - $($selectedTarget.Name)"
                }
                $targetSize = Format-Size $selectedTarget.Size
                Write-At 5 $row "Target:" DarkGray
                Write-At 14 $row (Truncate-String $targetLabel 50) White
                $row++
                Write-At 14 $row "$targetSize   $($selectedTarget.BusType)" DarkGray
                $row += 2

                # Size warnings
                if ($selectedTarget.Size -gt 256GB) {
                    Write-At 5 $row "WARNING: This drive is very large (>256 GB)!" Red
                    $row++
                    Write-At 5 $row "Double-check this is the correct target!" Red
                    $row += 2
                } elseif ($selectedTarget.Size -gt 32GB) {
                    Write-At 5 $row "NOTE: Drive is larger than 32 GB - verify this is correct." Yellow
                    $row += 2
                }

                Write-At 3 $row "+----------------------------------------------+" Red
                $row++
                Write-At 3 $row "|  ALL DATA ON THIS DISK WILL BE ERASED!       |" Red
                $row++
                Write-At 3 $row "|  THIS CANNOT BE UNDONE!                      |" Red
                $row++
                Write-At 3 $row "+----------------------------------------------+" Red
                $row += 2

                Write-At 5 $row "Erase this disk and write UnoDOS?" White
                $row += 2

                # Y/N buttons (default to No)
                $btnRow = $row
                $btnSel = 1  # 0 = Yes, 1 = No
                Render-Buttons $btnSel $btnRow

                $row += 2
                $hints = "Left/Right or Y/N   Enter = confirm"
                if ($canGoBackToDrive) { $hints += "   Bksp = back" }
                $hints += "   Esc = cancel"
                Write-At 3 $row $hints DarkGray

                $screenDone = $false
                while (-not $screenDone) {
                    $key = [Console]::ReadKey($true)
                    if ($key.Key -eq 'LeftArrow' -or $key.Key -eq 'RightArrow') {
                        $btnSel = 1 - $btnSel
                        Render-Buttons $btnSel $btnRow
                    }
                    elseif ($key.Key -eq 'Y') {
                        $btnSel = 0
                        Render-Buttons $btnSel $btnRow
                        $confirmed = $true
                        $navScreen = "write"
                        $screenDone = $true
                    }
                    elseif ($key.Key -eq 'N' -or $key.Key -eq 'Escape') {
                        [Console]::Clear()
                        [Console]::CursorVisible = $origCursorVisible
                        Write-Host "Operation cancelled."
                        exit 0
                    }
                    elseif ($key.Key -eq 'Backspace' -and $canGoBackToDrive) {
                        $navScreen = "drive"
                        $screenDone = $true
                    }
                    elseif ($key.Key -eq 'Enter') {
                        if ($btnSel -eq 0) {
                            $confirmed = $true
                            $navScreen = "write"
                            $screenDone = $true
                        } else {
                            # "No" selected — cancel
                            [Console]::Clear()
                            [Console]::CursorVisible = $origCursorVisible
                            Write-Host "Operation cancelled."
                            exit 0
                        }
                    }
                }
            }
        }

        if ($navScreen -eq "write") { break }
    }

    # Extra confirmation for drives >256 GB
    if ($confirmed -and $selectedTarget.Size -gt 256GB) {
        $row = $btnRow + 4
        Write-At 5 $row "This drive is >256 GB. Are you REALLY sure?" Red
        $row += 2
        $btn2Row = $row
        $btn2Sel = 1  # Default No

        Render-Buttons $btn2Sel $btn2Row

        $confirmed = $false
        $btn2Done = $false
        while (-not $btn2Done) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq 'LeftArrow' -or $key.Key -eq 'RightArrow') {
                $btn2Sel = 1 - $btn2Sel
                Render-Buttons $btn2Sel $btn2Row
            }
            elseif ($key.Key -eq 'Y') {
                $btn2Sel = 0
                Render-Buttons $btn2Sel $btn2Row
                $confirmed = $true
                $btn2Done = $true
            }
            elseif ($key.Key -eq 'N' -or $key.Key -eq 'Escape') {
                $btn2Done = $true
            }
            elseif ($key.Key -eq 'Enter') {
                if ($btn2Sel -eq 0) { $confirmed = $true }
                $btn2Done = $true
            }
        }

        if (-not $confirmed) {
            [Console]::Clear()
            [Console]::CursorVisible = $origCursorVisible
            Write-Host "Operation cancelled."
            exit 0
        }
    }

    # Set variables needed by write screen
    $imageSizeMB = [math]::Round($selectedImage.Size / 1MB, 1)
    $targetLabel = if ($selectedTarget.TargetType -eq "Floppy") {
        "$($selectedTarget.Letter): - $($selectedTarget.Name)"
    } else {
        "Disk $($selectedTarget.DiskNumber) - $($selectedTarget.Name)"
    }

    # ── Screen 5: Writing ────────────────────────────────────────────────────
    [Console]::Clear()
    $row = Draw-Banner
    $row++

    Write-At 3 $row "Writing $($selectedImage.FileName) to $targetLabel..." White
    $row += 2

    $imageBytes = $selectedImage.Bytes
    $totalBytes = $imageBytes.Length

    if ($selectedTarget.TargetType -eq "Disk") {
        # ── Disk write: offline, clear, write, online ────────────────────────
        Write-At 5 $row "[1/3] Preparing disk..." Cyan
        try {
            Set-Disk -Number $selectedTarget.DiskNumber -IsOffline $true -ErrorAction SilentlyContinue
            Clear-Disk -Number $selectedTarget.DiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction SilentlyContinue
        } catch {}
        Write-At 5 $row "[1/3] Preparing disk... Done     " Green
        $row++

        Write-At 5 $row "[2/3] Writing image..." Cyan
        $row++

        $barLeft = 5
        $barWidth = [Math]::Min(50, [Console]::WindowWidth - 20)
        $barRow = $row
        $row++
        $statusRow = $row
        $row++

        try {
            $stream = [System.IO.File]::Open(
                $selectedTarget.WritePath,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Write,
                [System.IO.FileShare]::None
            )

            $chunkSize = 1024 * 1024  # 1MB
            $bytesWritten = 0
            $startTime = Get-Date

            Write-At $barLeft $barRow ("[" + ("-" * $barWidth) + "]") DarkGray

            while ($bytesWritten -lt $totalBytes) {
                $remaining = $totalBytes - $bytesWritten
                $writeSize = [Math]::Min($chunkSize, $remaining)
                $stream.Write($imageBytes, $bytesWritten, $writeSize)
                $bytesWritten += $writeSize

                $percent = [int](($bytesWritten / $totalBytes) * 100)
                $filled = [int](($bytesWritten / $totalBytes) * $barWidth)
                $bar = "#" * $filled + "-" * ($barWidth - $filled)
                Write-At $barLeft $barRow ("[" + $bar + "]") Cyan
                Write-At ($barLeft + $barWidth + 3) $barRow "$percent%" White

                $elapsed = ((Get-Date) - $startTime).TotalSeconds
                if ($elapsed -gt 0) {
                    $speed = [math]::Round($bytesWritten / 1MB / $elapsed, 1)
                    $mbDone = [math]::Round($bytesWritten / 1MB, 1)
                    $mbTotal = [math]::Round($totalBytes / 1MB, 1)
                    Write-At 5 $statusRow "$mbDone / $mbTotal MB   ($speed MB/s)     " DarkGray
                }
            }

            $stream.Flush()
            $stream.Close()

            $row = $statusRow + 1
            Write-At 5 $row "[2/3] Write complete!" Green
            $row++

        } catch {
            $row = $statusRow + 2
            Write-At 5 $row "ERROR: Write failed!" Red
            $row++
            Write-At 5 $row $_.Exception.Message Red
            $row += 2
            Write-At 5 $row "Press any key to exit..." DarkGray
            [Console]::ReadKey($true) | Out-Null
            exit 1
        }

        Write-At 5 $row "[3/3] Finalizing..." Cyan
        try {
            Set-Disk -Number $selectedTarget.DiskNumber -IsOffline $false -ErrorAction SilentlyContinue
        } catch {}
        Write-At 5 $row "[3/3] Finalizing... Done     " Green
        $row += 2

    } else {
        # ── Floppy write: direct stream ──────────────────────────────────────
        Write-At 5 $row "Writing to $($selectedTarget.Letter):..." Cyan
        $row++

        $barLeft = 5
        $barWidth = [Math]::Min(50, [Console]::WindowWidth - 20)
        $barRow = $row
        $row++
        $statusRow = $row
        $row++

        try {
            $stream = [System.IO.File]::Open(
                $selectedTarget.WritePath,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Write,
                [System.IO.FileShare]::None
            )

            $chunkSize = 32 * 1024  # 32KB chunks for floppy (smaller for progress updates)
            $bytesWritten = 0
            $startTime = Get-Date

            Write-At $barLeft $barRow ("[" + ("-" * $barWidth) + "]") DarkGray

            while ($bytesWritten -lt $totalBytes) {
                $remaining = $totalBytes - $bytesWritten
                $writeSize = [Math]::Min($chunkSize, $remaining)
                $stream.Write($imageBytes, $bytesWritten, $writeSize)
                $bytesWritten += $writeSize

                $percent = [int](($bytesWritten / $totalBytes) * 100)
                $filled = [int](($bytesWritten / $totalBytes) * $barWidth)
                $bar = "#" * $filled + "-" * ($barWidth - $filled)
                Write-At $barLeft $barRow ("[" + $bar + "]") Cyan
                Write-At ($barLeft + $barWidth + 3) $barRow "$percent%" White

                $elapsed = ((Get-Date) - $startTime).TotalSeconds
                if ($elapsed -gt 0) {
                    $speed = [math]::Round($bytesWritten / 1KB / $elapsed, 1)
                    $mbDone = [math]::Round($bytesWritten / 1KB, 1)
                    $mbTotal = [math]::Round($totalBytes / 1KB, 1)
                    Write-At 5 $statusRow "$mbDone / $mbTotal KB   ($speed KB/s)     " DarkGray
                }
            }

            $stream.Flush()
            $stream.Close()

            $row = $statusRow + 1
            Write-At 5 $row "Write complete!" Green
            $row += 2

        } catch {
            $row = $statusRow + 2
            Write-At 5 $row "ERROR: Write failed!" Red
            $row++
            Write-At 5 $row $_.Exception.Message Red
            $row += 2
            Write-At 5 $row "Press any key to exit..." DarkGray
            [Console]::ReadKey($true) | Out-Null
            exit 1
        }
    }

    # ── Verification ─────────────────────────────────────────────────────────
    if ($Verify) {
        Write-At 5 $row "Verifying..." Cyan

        try {
            $readStream = [System.IO.File]::Open(
                $selectedTarget.WritePath,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::ReadWrite
            )

            # Read first 100 sectors (51200 bytes) for floppy, first 1MB for HD
            $verifySize = if ($selectedTarget.TargetType -eq "Floppy") { 51200 } else { [Math]::Min(1MB, $totalBytes) }
            $readBuffer = New-Object byte[] $verifySize
            $bytesRead = $readStream.Read($readBuffer, 0, $verifySize)
            $readStream.Close()

            $differences = 0
            for ($i = 0; $i -lt [Math]::Min($bytesRead, $verifySize); $i++) {
                if ($readBuffer[$i] -ne $imageBytes[$i]) {
                    $differences++
                    if ($differences -le 5) {
                        $sector = [Math]::Floor($i / 512)
                        $offset = $i % 512
                        $row++
                        Write-At 7 $row "Byte $i (sector $sector +$offset): expected 0x$($imageBytes[$i].ToString('X2')), got 0x$($readBuffer[$i].ToString('X2'))" DarkYellow
                    }
                }
            }

            if ($differences -eq 0) {
                Write-At 5 $row "Verifying... PASSED!                   " Green
            } else {
                Write-At 5 $row "Verifying... FAILED! $differences byte differences" Red
            }
        } catch {
            Write-At 5 $row "Verifying... skipped (read-back not supported)" Yellow
        }
        $row += 2
    }

    # ── Success ──────────────────────────────────────────────────────────────
    $mbWritten = [math]::Round($totalBytes / 1MB, 1)
    Write-At 3 $row "+========================================+" Green
    $row++
    $successMsg = "     Write Complete! ($mbWritten MB)"
    $padLen = 41 - $successMsg.Length - 1
    if ($padLen -lt 0) { $padLen = 0 }
    Write-At 3 $row ("|" + $successMsg + (" " * $padLen) + "|") Green
    $row++
    Write-At 3 $row "+========================================+" Green
    $row += 2

    # Context-sensitive next steps
    if ($selectedTarget.TargetType -eq "Floppy") {
        Write-At 5 $row "Next steps:" White
        $row++
        Write-At 5 $row "1. Boot from this floppy - launcher will auto-load!" Gray
        $row++
        $desc = Get-ImageDescription $selectedImage.FileName
        if ($desc -like "*Apps*") {
            Write-At 5 $row "2. Or swap to this disk while launcher is running" Gray
            $row++
        }
    } else {
        Write-At 5 $row "Next steps:" White
        $row++
        Write-At 5 $row "1. Remove the CF card / USB drive" Gray
        $row++
        Write-At 5 $row "2. Install in target machine" Gray
        $row++
        Write-At 5 $row "3. Boot from hard drive to run UnoDOS" Gray
        $row += 2
        Write-At 5 $row "Windows may ask to format the disk - click Cancel!" Yellow
    }
    $row += 2
    Write-At 5 $row "Enter = write another   Esc = exit" DarkGray
    $loopBack = $false
    while ($true) {
        $key = [Console]::ReadKey($true)
        if ($key.Key -eq 'Escape') { break }
        if ($key.Key -eq 'Enter') { $loopBack = $true; break }
    }
    if (-not $loopBack) { break }  # Exit outer loop

    # Reset state for next write
    $selectedImage = $null
    $selectedTarget = $null
    $confirmed = $false
    $imageInfos = $null
    $navScreen = "image"
    }  # End outer write loop

} finally {
    [Console]::CursorVisible = $origCursorVisible
    [Console]::ForegroundColor = $origFG
    [Console]::BackgroundColor = $origBG
}
