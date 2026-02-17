#Requires -RunAsAdministrator
# UnoDOS Hard Drive Image Writer - Interactive TUI
# Writes UnoDOS HD image to CF card or hard drive
#
# Usage: .\tools\hd.ps1 [-ImagePath path]
#
# If -ImagePath is not specified, auto-finds build\unodos-hd.img
# WARNING: This will ERASE ALL DATA on the target disk!

param(
    [string]$ImagePath
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
    $gb = [math]::Round($Bytes / 1GB, 1)
    $mb = [math]::Round($Bytes / 1MB, 0)
    if ($gb -ge 1) { return "$gb GB" } else { return "$mb MB" }
}

function Truncate-String {
    param([string]$Str, [int]$Max)
    if ($null -eq $Str) { return "" }
    if ($Str.Length -le $Max) { return $Str }
    return $Str.Substring(0, $Max - 1) + "~"
}

# ─── Draw banner centered at top ─────────────────────────────────────────────

function Draw-Banner {
    param([int]$Y = 0)
    $lines = @(
        "+========================================+",
        "|    UnoDOS Hard Drive Image Writer       |",
        "+========================================+"
    )
    $w = [Console]::WindowWidth
    $pad = [Math]::Max(0, [int](($w - $lines[0].Length) / 2))
    for ($i = 0; $i -lt $lines.Count; $i++) {
        Write-At $pad ($Y + $i) $lines[$i] Yellow
    }
    return ($Y + $lines.Count)
}

# ─── Main ─────────────────────────────────────────────────────────────────────

# Save original console state
$origBG = [Console]::BackgroundColor
$origFG = [Console]::ForegroundColor
$origCursorVisible = [Console]::CursorVisible
[Console]::CursorVisible = $false

try {
    # ── Screen 1: Startup ─────────────────────────────────────────────────────
    [Console]::Clear()
    $row = Draw-Banner
    $row++

    # Git pull
    Write-At 3 $row "Updating from GitHub..." Cyan
    Push-Location $projectDir
    try {
        git fetch origin 2>&1 | Out-Null
        git reset --hard origin/master 2>&1 | Out-Null
        Write-At 3 $row "Updating from GitHub... Done!       " Green
    } catch {
        Write-At 3 $row "Updating from GitHub... Skipped (using local)" Yellow
    }
    Pop-Location
    $row++

    # Find image
    if (-not $ImagePath) {
        $ImagePath = "$projectDir\build\unodos-hd.img"
    }
    if (-not (Test-Path $ImagePath)) {
        Write-At 3 $row "ERROR: Image not found: $ImagePath" Red
        $row += 2
        Write-At 3 $row "Press any key to exit..." DarkGray
        [Console]::ReadKey($true) | Out-Null
        exit 1
    }
    $ImagePath = (Resolve-Path $ImagePath).Path
    $imageInfo = Get-Item $ImagePath
    $imageSizeMB = [math]::Round($imageInfo.Length / 1MB, 1)

    # Read version/build from image
    Write-At 3 $row "Loading image..." Cyan
    $imageBytes = [System.IO.File]::ReadAllBytes($ImagePath)
    $imageText = [System.Text.Encoding]::ASCII.GetString($imageBytes)
    $versionMatch = [regex]::Match($imageText, 'UnoDOS v[\d.]+')
    $versionString = if ($versionMatch.Success) { $versionMatch.Value } else { "Unknown" }
    $buildMatch = [regex]::Match($imageText, 'Build: \d+')
    $buildString = if ($buildMatch.Success) { $buildMatch.Value } else { "Unknown" }
    Write-At 3 $row "Loading image... OK                " Green
    $row++

    Start-Sleep -Milliseconds 400

    # ── Screen 2: Drive Selection ─────────────────────────────────────────────
    [Console]::Clear()
    $row = Draw-Banner
    $row++

    # Image info
    Write-At 3 $row "Image:  $(Split-Path -Leaf $ImagePath) ($imageSizeMB MB)" White
    $row++
    Write-At 3 $row "        $versionString   $buildString" Cyan
    $row += 2

    # Get eligible drives (exclude system/boot)
    $allDrives = @(Get-Disk | Where-Object { -not $_.IsBoot -and -not $_.IsSystem })

    if ($allDrives.Count -eq 0) {
        Write-At 3 $row "No eligible drives found!" Red
        $row++
        Write-At 3 $row "(System and boot drives are automatically excluded)" DarkGray
        $row += 2
        Write-At 3 $row "Connect a CF card or USB drive and try again." Yellow
        $row += 2
        Write-At 3 $row "Press any key to exit..." DarkGray
        [Console]::ReadKey($true) | Out-Null
        exit 1
    }

    Write-At 3 $row "Select target drive:" White
    $row++
    Write-At 3 $row "Up/Down = navigate   Enter = select   Esc = cancel" DarkGray
    $row += 2

    # Table header
    $col1 = 4; $col2 = 12; $col3 = 46; $col4 = 58
    Write-At $col1 $row "Disk" DarkCyan
    Write-At $col2 $row "Name" DarkCyan
    Write-At $col3 $row "Size" DarkCyan
    Write-At $col4 $row "Bus" DarkCyan
    $row++
    $w = [Console]::WindowWidth
    Write-At 3 $row ("-" * [Math]::Min(68, $w - 6)) DarkGray
    $row++

    $listTop = $row
    $selected = 0

    # Draw drive list
    function Render-DriveList {
        param([array]$Drives, [int]$Sel, [int]$Top,
              [int]$C1, [int]$C2, [int]$C3, [int]$C4)
        for ($i = 0; $i -lt $Drives.Count; $i++) {
            $d = $Drives[$i]
            $y = $Top + $i
            $name = Truncate-String $d.FriendlyName 32
            $size = Format-Size $d.Size
            $bus = if ($d.BusType) { "$($d.BusType)" } else { "?" }

            if ($i -eq $Sel) {
                # Highlighted row
                $lineW = [Math]::Min(70, [Console]::WindowWidth - 3)
                Write-At 3 $y (" " * $lineW) Black White
                Write-At $C1 $y (" $($d.Number) ") Black White
                Write-At $C2 $y $name Black White
                Write-At $C3 $y $size Black White
                Write-At $C4 $y $bus Black White
            } else {
                Clear-Row $y 3
                Write-At $C1 $y " $($d.Number) " Gray
                Write-At $C2 $y $name Gray
                Write-At $C3 $y $size DarkGray
                Write-At $C4 $y $bus DarkGray
            }
        }

        # Info area below the list
        $infoRow = $Top + $Drives.Count + 1
        Clear-Row $infoRow
        Clear-Row ($infoRow + 1)

        $d = $Drives[$Sel]
        if ($d.Size -gt 32GB) {
            Write-At 3 $infoRow "WARNING: Drive is larger than 32GB - verify this is correct!" Yellow
        }
        if ($d.Size -lt $script:imageInfo.Length) {
            Write-At 3 $infoRow "ERROR: Drive is too small for this image!" Red
        }
    }

    Render-DriveList $allDrives $selected $listTop $col1 $col2 $col3 $col4

    # Key loop
    $done = $false
    while (-not $done) {
        $key = [Console]::ReadKey($true)

        if ($key.Key -eq 'UpArrow' -and $selected -gt 0) {
            $selected--
            Render-DriveList $allDrives $selected $listTop $col1 $col2 $col3 $col4
        }
        elseif ($key.Key -eq 'DownArrow' -and $selected -lt ($allDrives.Count - 1)) {
            $selected++
            Render-DriveList $allDrives $selected $listTop $col1 $col2 $col3 $col4
        }
        elseif ($key.Key -eq 'Enter') {
            # Check disk is big enough before proceeding
            if ($allDrives[$selected].Size -lt $imageInfo.Length) {
                # Flash error, don't proceed
            } else {
                $done = $true
            }
        }
        elseif ($key.Key -eq 'Escape') {
            [Console]::Clear()
            [Console]::CursorVisible = $origCursorVisible
            Write-Host "Operation cancelled."
            exit 0
        }
    }

    $targetDisk = $allDrives[$selected]
    $diskNum = $targetDisk.Number

    # ── Screen 3: Confirmation ────────────────────────────────────────────────
    [Console]::Clear()
    $row = Draw-Banner
    $row++

    Write-At 3 $row "CONFIRM WRITE" Red
    $row += 2

    Write-At 5 $row "Image:" DarkGray
    Write-At 14 $row "$(Split-Path -Leaf $ImagePath) ($imageSizeMB MB)" White
    $row++
    Write-At 14 $row "$versionString   $buildString" Cyan
    $row += 2

    $diskName = Truncate-String $targetDisk.FriendlyName 40
    $diskSize = Format-Size $targetDisk.Size
    Write-At 5 $row "Target:" DarkGray
    Write-At 14 $row "Disk $diskNum - $diskName" White
    $row++
    Write-At 14 $row "$diskSize   $($targetDisk.BusType)" DarkGray
    $row += 2

    if ($targetDisk.Size -gt 32GB) {
        Write-At 5 $row "NOTE: Drive is larger than 32GB - double check!" Yellow
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

    Write-At 5 $row "Type ERASE to continue, or Esc to cancel:" White
    $row++
    Write-At 5 $row "> " Yellow
    $inputCol = 7
    $inputRow = $row

    [Console]::CursorVisible = $true
    [Console]::SetCursorPosition($inputCol, $inputRow)

    $input = ""
    $confirmed = $false
    while ($true) {
        $key = [Console]::ReadKey($true)
        if ($key.Key -eq 'Escape') {
            [Console]::Clear()
            [Console]::CursorVisible = $origCursorVisible
            Write-Host "Operation cancelled."
            exit 0
        }
        if ($key.Key -eq 'Enter') {
            if ($input -eq "ERASE") { $confirmed = $true }
            break
        }
        if ($key.Key -eq 'Backspace') {
            if ($input.Length -gt 0) {
                $input = $input.Substring(0, $input.Length - 1)
                [Console]::SetCursorPosition($inputCol, $inputRow)
                [Console]::Write($input + " ")
                [Console]::SetCursorPosition($inputCol + $input.Length, $inputRow)
            }
            continue
        }
        if ($key.KeyChar -match '[a-zA-Z]') {
            $input += $key.KeyChar
            [Console]::SetCursorPosition($inputCol, $inputRow)
            [Console]::Write($input)
        }
    }

    [Console]::CursorVisible = $false

    if (-not $confirmed) {
        [Console]::Clear()
        [Console]::CursorVisible = $origCursorVisible
        Write-Host "Operation cancelled."
        exit 0
    }

    # ── Screen 4: Writing ─────────────────────────────────────────────────────
    [Console]::Clear()
    $row = Draw-Banner
    $row++

    Write-At 3 $row "Writing to Disk $diskNum ($diskName)..." White
    $row += 2

    # Prepare disk
    Write-At 5 $row "[1/3] Preparing disk..." Cyan
    try {
        Set-Disk -Number $diskNum -IsOffline $true -ErrorAction SilentlyContinue
        Clear-Disk -Number $diskNum -RemoveData -RemoveOEM -Confirm:$false -ErrorAction SilentlyContinue
    } catch {
        # Ignore - disk might already be clear
    }
    Write-At 5 $row "[1/3] Preparing disk... Done     " Green
    $row++

    # Write image
    Write-At 5 $row "[2/3] Writing image..." Cyan
    $row++

    $barLeft = 5
    $barWidth = [Math]::Min(50, [Console]::WindowWidth - 20)
    $barRow = $row
    $row++
    $statusRow = $row
    $row++

    $physicalPath = "\\.\PhysicalDrive$diskNum"

    try {
        $stream = [System.IO.File]::Open(
            $physicalPath,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None
        )

        $chunkSize = 1024 * 1024  # 1MB chunks
        $bytesWritten = 0
        $totalBytes = $imageBytes.Length
        $startTime = Get-Date

        # Initial empty bar
        Write-At $barLeft $barRow ("[" + ("-" * $barWidth) + "]") DarkGray

        while ($bytesWritten -lt $totalBytes) {
            $remaining = $totalBytes - $bytesWritten
            $writeSize = [Math]::Min($chunkSize, $remaining)

            $stream.Write($imageBytes, $bytesWritten, $writeSize)
            $bytesWritten += $writeSize

            $percent = [int](($bytesWritten / $totalBytes) * 100)
            $filled = [int](($bytesWritten / $totalBytes) * $barWidth)

            # Progress bar
            $bar = "#" * $filled + "-" * ($barWidth - $filled)
            Write-At $barLeft $barRow ("[" + $bar + "]") Cyan
            Write-At ($barLeft + $barWidth + 3) $barRow "$percent%" White

            # Speed/ETA
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

    # Bring disk back online
    Write-At 5 $row "[3/3] Finalizing..." Cyan
    try {
        Set-Disk -Number $diskNum -IsOffline $false -ErrorAction SilentlyContinue
    } catch {}
    Write-At 5 $row "[3/3] Finalizing... Done     " Green
    $row += 2

    # ── Success ───────────────────────────────────────────────────────────────
    $mbWritten = [math]::Round($bytesWritten / 1MB, 1)
    Write-At 3 $row "+========================================+" Green
    $row++
    Write-At 3 $row "|          Write Complete!                |" Green
    $row++
    Write-At 3 $row "|     $mbWritten MB written to Disk $diskNum" Green
    # Pad the line to fill the box
    $lineLen = "     $mbWritten MB written to Disk $diskNum".Length
    $padNeeded = 41 - $lineLen - 1
    if ($padNeeded -gt 0) {
        [Console]::Write(" " * $padNeeded + "|")
    } else {
        [Console]::Write(" |")
    }
    $row++
    Write-At 3 $row "+========================================+" Green
    $row += 2

    Write-At 5 $row "Next steps:" White
    $row++
    Write-At 5 $row "1. Remove the CF card/disk" Gray
    $row++
    Write-At 5 $row "2. Install in target machine (via IDE adapter)" Gray
    $row++
    Write-At 5 $row "3. Boot from hard drive to run UnoDOS" Gray
    $row += 2
    Write-At 5 $row "Windows may ask to format the disk - click Cancel!" Yellow
    $row += 2
    Write-At 5 $row "Press any key to exit..." DarkGray
    [Console]::ReadKey($true) | Out-Null

} finally {
    # Restore console state
    [Console]::CursorVisible = $origCursorVisible
    [Console]::ForegroundColor = $origFG
    [Console]::BackgroundColor = $origBG
}
