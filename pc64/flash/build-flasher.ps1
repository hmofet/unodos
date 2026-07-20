# Build the UnoDOS USB Flasher - a single self-contained Windows exe.
#
# The flasher no longer clones a raw image: it BUILDS the volume on the target
# (GPT + one whole-disk FAT32 ESP) and copies the system files in, so a 32 GB
# stick becomes a 32 GB UnoDOS drive instead of a 512 MB one.  What it embeds is
# therefore the ESP *tree* as a .zip, not a disk image.
#
# The raw image is still built, because deploy-to-share.ps1 publishes it for
# Rufus / balenaEtcher / dd users and mkiso.py turns it into the hybrid ISO.
#
# Pipeline (all under WSL, which has the mingw cross-compiler + sgdisk + mtools):
#   1. ./build.sh          -> build/esp/ + build/esp/EFI/BOOT/BOOTX64.EFI
#   2. tools/mkuefi.py N   -> build/unodos-uefi.img  (GPT + ESP FAT32, N MiB)
#   3. zip build/esp + csc -> build/UnoDosFlasher.exe
#
# Usage:  pc64/flash/build-flasher.ps1 [-SizeMiB 512] [-SkipBuild] [-TestTool]
#   -SkipBuild : reuse build/esp/ as-is (don't re-run ./build.sh)
#   -TestTool  : also build build/UnoDiskTest.exe, which runs the same volume
#                builder into an image FILE so fsck.vfat / sgdisk / QEMU can
#                check it (see tools/diskboot_test.py)
param(
    [int]$SizeMiB = 512,      # capacity of the release image (documents get room)
    [switch]$SkipBuild,       # reuse the already-built ESP tree
    [switch]$TestTool         # also build the headless image-builder for tests
)
$ErrorActionPreference = "Stop"
$pc64  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $pc64 "build"

# C:\...\unodos\pc64  ->  /mnt/c/.../unodos/pc64  (no wslpath: arg quoting eats backslashes)
$wslPc64 = "/mnt/" + $pc64.Substring(0,1).ToLower() + ($pc64.Substring(2) -replace '\\','/')

# Run a native command, letting it write to stderr WITHOUT tripping
# $ErrorActionPreference='Stop' (in Windows PowerShell 5.1 native stderr is
# wrapped as a terminating NativeCommandError).  gcc warnings from build.sh go
# to stderr on a perfectly good build, so we gate on the exit code instead.
function Invoke-Native([scriptblock]$sb, [string]$what) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $sb 2>&1 | ForEach-Object { Write-Host $_ } }
    finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$what (exit $LASTEXITCODE)" }
}

# ---- 1+2. build the EFI image and pack it into a raw UEFI disk image --------
# NOTE (branch pc64-debug-stress): ./build.sh defaults to UNO_DEBUG=1 here, so
# this flasher embeds the DEBUG / stress-test OS - crash reports to \CRASH, the
# stress driver armed by \STRESS.CFG, symbols in \DOCS\SYMBOLS.TXT.  Per the
# standing rule we ship ONE flasher (this debug build), not every OS variant.
if (-not $SkipBuild) {
    Write-Host "Building UnoDOS/pc64 DEBUG build (BOOTX64.EFI + ESP) under WSL..." -ForegroundColor Yellow
    Invoke-Native { & wsl bash -lc "cd '$wslPc64' && ./build.sh" } "pc64 build failed (try: wsl bash -lc 'cd pc64 && ./build.sh')"
}
Write-Host "Packing UEFI disk image ($SizeMiB MiB) under WSL..."
Invoke-Native { & wsl bash -lc "cd '$wslPc64' && python3 tools/mkuefi.py $SizeMiB" } "mkuefi.py failed (needs sgdisk + mtools in WSL)"

$img = Join-Path $build "unodos-uefi.img"
if (-not (Test-Path $img)) { throw "Missing image: $img" }

# ---- 3. zip the ESP tree into an embeddable resource ------------------------
# This is what the flasher actually installs; the .img above is only for the
# dd-style tools.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$espDir = Join-Path $build "esp"
if (-not (Test-Path (Join-Path $espDir "EFI\BOOT\BOOTX64.EFI"))) {
    throw "build/esp/EFI/BOOT/BOOTX64.EFI missing - run ./build.sh first"
}
$zip = Join-Path $build "unodos_esp.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Write-Host "Zipping the ESP tree for embedding..."
[IO.Compression.ZipFile]::CreateFromDirectory(
    $espDir, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
$zmb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "  unodos_esp.zip = $zmb MB"

# ---- locate csc (in-box .NET Framework) -------------------------------------
$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe" }
if (-not (Test-Path $csc)) { throw "csc.exe (.NET Framework 4.x) not found" }

$src      = Join-Path $PSScriptRoot "UnoDosFlash.cs"
$disk     = Join-Path $PSScriptRoot "UnoDisk.cs"
$settings = Join-Path $PSScriptRoot "UnoSettings.cs"
$update   = Join-Path $PSScriptRoot "UnoUpdate.cs"
$manifest = Join-Path $PSScriptRoot "app.manifest"
$icon     = Join-Path $PSScriptRoot "unodos.ico"
$exe      = Join-Path $build "UnoDosFlasher.exe"

# Stamp this build so the flasher can compare itself to the staged copy on the
# share (self-update).  The generated build/UnoVersion.cs REPLACES the checked-in
# dev placeholder flash/UnoVersion.cs in the compile; flasher-build.txt is what
# deploy-to-share.ps1 publishes as flasher-version.txt.
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$verCs = Join-Path $build "UnoVersion.cs"
Set-Content -Path $verCs -Encoding ASCII -Value @(
    "// generated by build-flasher.ps1 - do not edit; flash/UnoVersion.cs is the dev placeholder",
    "static class UnoVersion { public const string Build = `"$stamp`"; }")
Set-Content -Path (Join-Path $build "flasher-build.txt") -Value $stamp -Encoding ASCII

$args = @(
    "/target:winexe",
    "/out:$exe",
    "/win32manifest:$manifest",
    "/reference:System.Management.dll",
    "/reference:System.Windows.Forms.dll",
    "/reference:System.Drawing.dll",
    "/reference:System.IO.Compression.dll",
    "/reference:System.IO.Compression.FileSystem.dll",
    "/resource:$zip,unodos_esp"
)
if (Test-Path $icon) { $args += "/win32icon:$icon" }
$args += @("/optimize+", "$src", "$disk", "$settings", "$update", "$verCs")

Write-Host "Compiling $([IO.Path]::GetFileName($exe))..."
& $csc $args
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }

# The headless twin: same UnoDisk.cs, writing to a file instead of a drive, so
# the filesystem can be checked by real tools rather than by flashing a stick.
if ($TestTool) {
    $testExe = Join-Path $build "UnoDiskTest.exe"
    Write-Host "Compiling UnoDiskTest.exe..."
    & $csc @("/target:exe", "/out:$testExe",
             "/reference:System.IO.Compression.dll",
             "/reference:System.IO.Compression.FileSystem.dll",
             "/optimize+", "$disk", (Join-Path $PSScriptRoot "UnoDiskTest.cs"))
    if ($LASTEXITCODE -ne 0) { throw "csc failed for UnoDiskTest ($LASTEXITCODE)" }
}

Remove-Item $zip -ErrorAction SilentlyContinue
$mb = [math]::Round((Get-Item $exe).Length / 1MB, 1)
Write-Host "Built $exe  ($mb MB)"
