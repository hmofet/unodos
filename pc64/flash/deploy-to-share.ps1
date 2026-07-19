# Rebuild the UnoDOS/pc64 USB flasher and publish it to the network share so it
# can be flashed from any computer on the LAN.
#
#   \\behemoth\unreplicated\unodos\pc64\
#     UnoDosFlasher.exe            one-click Windows installer (image embedded)
#     unodos-pc64-uefi.img.gz      raw image for Rufus / balenaEtcher / dd
#     unodos-pc64.iso              hybrid UEFI ISO: VM CD-ROM boot AND
#                                  Rufus/Etcher/dd to USB (tools/mkiso.py)
#
# STANDING RULE (see pc64/CLAUDE.md): run this after every new pc64 OS build so
# the shared flasher never goes stale.
#
# Usage:  pc64\flash\deploy-to-share.ps1 [-SkipBuild] [-SizeMiB 512] [-Dest <path>]
#   -SkipBuild : reuse build/UnoDosFlasher.exe + build/unodos-uefi.img as-is
param(
    [switch]$SkipBuild,
    [int]$SizeMiB = 512,
    [string]$Dest = '\\behemoth\unreplicated\unodos\pc64'
)
$ErrorActionPreference = "Stop"
$pc64  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $pc64 "build"
$exe   = Join-Path $build "UnoDosFlasher.exe"
$img   = Join-Path $build "unodos-uefi.img"

# 1. (re)build the flasher unless told to reuse the current artifacts
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build-flasher.ps1") -SizeMiB $SizeMiB
    if ($LASTEXITCODE -ne 0) { throw "build-flasher.ps1 failed" }
}
if (-not (Test-Path $exe)) { throw "Flasher not built: run without -SkipBuild (or run build-flasher.ps1)" }
if (-not (Test-Path $img)) { throw "Image not built: $img" }

# 2. make sure the share is reachable, then ensure the pc64/ folder exists
$shareRoot = Split-Path $Dest -Parent
if (-not (Test-Path $shareRoot)) {
    throw "Share not reachable: $shareRoot  (is \\behemoth mounted / online?)"
}
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# 3. gzip the raw image (cross-platform flashers read .gz directly)
$gz = Join-Path $build "unodos-pc64-uefi.img.gz"
Write-Host "Compressing image for the share..."
$in  = [IO.File]::Open($img, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
$out = [IO.File]::Create($gz)
$gzs = New-Object IO.Compression.GZipStream($out, [IO.Compression.CompressionLevel]::Optimal)
$in.CopyTo($gzs); $gzs.Dispose(); $out.Dispose(); $in.Dispose()

# 3b. build the hybrid UEFI ISO from the same build/esp (VM CD + dd-to-USB)
$iso = Join-Path $build "unodos-pc64.iso"
if (-not $SkipBuild -or -not (Test-Path $iso)) {
    Write-Host "Building the hybrid ISO (tools/mkiso.py under WSL)..."
    $wslPc64 = (& wsl wslpath -a ($pc64 -replace '\\','/')).Trim()
    & wsl bash -lc "cd '$wslPc64' && python3 tools/mkiso.py"
    if ($LASTEXITCODE -ne 0) { throw "mkiso.py failed (needs xorriso + mtools in WSL)" }
}

# 4. copy the flasher + image + ISO to the share
Write-Host "Copying to $Dest ..."
Copy-Item $exe -Destination $Dest -Force
Copy-Item $gz  -Destination $Dest -Force
Copy-Item $iso -Destination $Dest -Force
Remove-Item $gz -ErrorAction SilentlyContinue

# 5. keep a tiny build stamp next to the artifacts (commit + when)
$commit = (& git -C $pc64 rev-parse --short HEAD 2>$null)
$stamp  = "UnoDosFlasher.exe + unodos-pc64-uefi.img.gz + unodos-pc64.iso`r`n" +
          "pc64 commit: $commit`r`n" +
          "deployed:    $(Get-Date -Format 'yyyy-MM-dd HH:mm')`r`n"
Set-Content -Path (Join-Path $Dest "BUILD.txt") -Value $stamp -Encoding UTF8

# 6. add a pc64/ line to the share's MANIFEST.txt if it isn't already listed
$manifest = Join-Path $shareRoot "MANIFEST.txt"
if (Test-Path $manifest) {
    $lines = Get-Content $manifest
    if (-not ($lines | Where-Object { $_ -match '^\s*pc64/' })) {
        # ASCII '-' (not an em-dash) so Add-Content's encoding can't mangle it
        Add-Content -Path $manifest -Value 'pc64/       UnoDosFlasher.exe (Windows one-click, image embedded) + unodos-pc64-uefi.img.gz (Rufus/Etcher/dd) - x86-64 UEFI (Secure Boot off)'
        Write-Host "Added pc64/ line to MANIFEST.txt"
    }
}

$mb = [math]::Round((Get-Item (Join-Path $Dest 'UnoDosFlasher.exe')).Length / 1MB, 1)
Write-Host "Deployed to $Dest  (flasher $mb MB, pc64 commit $commit)"
