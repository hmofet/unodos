# Publish a UnoDOS/pc64 GitHub release: attaches the Windows flasher and the
# raw UEFI disk image (gzip-compressed - Rufus and Etcher read .gz directly; for
# plain `dd`, gunzip first).
#
# Prereqs: gh authenticated; the flasher + image already built
# (pc64/flash/build-flasher.ps1).
# Usage: pc64/flash/publish-release.ps1 [-Tag pc64-v1.0]
param([string]$Tag = "pc64-v1.0")
$ErrorActionPreference = "Stop"

$pc64  = Split-Path $PSScriptRoot -Parent
$repo  = Split-Path $pc64 -Parent
$build = Join-Path $pc64 "build"
$exe   = Join-Path $build "UnoDosFlasher.exe"
$img   = Join-Path $build "unodos-uefi.img"

if (-not (Test-Path $exe)) { throw "Flasher not built: run pc64/flash/build-flasher.ps1" }
if (-not (Test-Path $img)) { throw "Image not built: run pc64/flash/build-flasher.ps1" }

# gzip the image into build/release/
$rel = Join-Path $build "release"
New-Item -ItemType Directory -Force -Path $rel | Out-Null
$gz = Join-Path $rel "unodos-pc64-uefi.img.gz"
Write-Host "gzip unodos-uefi.img -> unodos-pc64-uefi.img.gz"
$in  = [IO.File]::Open($img, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
$out = [IO.File]::Create($gz)
$gzs = New-Object IO.Compression.GZipStream($out, [IO.Compression.CompressionLevel]::Optimal)
$in.CopyTo($gzs); $gzs.Dispose(); $out.Dispose(); $in.Dispose()

# the hybrid UEFI ISO (VM CD-ROM boot + Rufus/Etcher/dd to USB) - build if absent
$iso = Join-Path $build "unodos-pc64.iso"
if (-not (Test-Path $iso)) {
    Write-Host "Building the hybrid ISO (tools/mkiso.py under WSL)..."
    $wslPc64 = (& wsl wslpath -a ($pc64 -replace '\\','/')).Trim()
    & wsl bash -lc "cd '$wslPc64' && python3 tools/mkiso.py"
    if ($LASTEXITCODE -ne 0) { throw "mkiso.py failed (needs xorriso + mtools in WSL)" }
}
$assets = @($exe, $gz, $iso)

$notes = @"
**UnoDOS / pc64** - the modern-PC (x86-64 / UEFI) world of UnoDOS. Boots straight
into the unoui desktop shell.

### Flash a USB
Easiest: download **UnoDosFlasher.exe**, run it (Windows, no install), pick your
USB drive, and Install.

Or write the image yourself with Rufus / balenaEtcher (they read ``.gz`` directly)
or ``dd`` (gunzip first):

``gunzip -c unodos-pc64-uefi.img.gz | sudo dd of=/dev/sdX bs=4M status=progress``

### Or use the ISO
**unodos-pc64.iso** is a hybrid UEFI ISO: attach it to a VM (QEMU, VirtualBox,
VMware, Hyper-V) as a CD-ROM, or write it to a USB stick with Rufus,
balenaEtcher or ``dd`` - both ways boot the same desktop.

### Boot it
UEFI only (x86-64 PCs, ~2012 on). In firmware: disable **Secure Boot**, then pick
the USB from the boot menu. There is no BIOS/Legacy build - pc64 is UEFI-native.
"@

Push-Location $repo
try {
    Write-Host "Creating release $Tag..."
    & gh release create $Tag @assets --title "UnoDOS pc64 $Tag" --notes $notes
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }
    Write-Host "Published $Tag with:`n  $($assets -join "`n  ")"
} finally {
    Pop-Location
}
