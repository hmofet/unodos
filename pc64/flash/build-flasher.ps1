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

# ---- 1+2. build BOTH the production and debug OS, pack the release image -----
# The flasher embeds TWO ESP trees:
#   - PRODUCTION (UNO_DEBUG=0): what it flashes by default (a clean OS, no
#     \CRASH, no \STRESS.CFG, no stress driver).
#   - DEBUG      (UNO_DEBUG=1): flashed only when Developer options is on -
#     crash reports to \CRASH, the stress driver, and the test harness the
#     dev-options test toggles arm via \STRESS.CFG.
# (This supersedes the old "ship ONE flasher = the debug build" rule.)
if (-not $SkipBuild) {
    # build.sh populates build/esp INCREMENTALLY (no wipe), so a stale CRASH /
    # STRESS.CFG / FIRMWARE from a prior debug build would leak into the
    # production snapshot. Wipe build/esp before each build to keep them clean.
    Write-Host "Building PRODUCTION OS (UNO_DEBUG=0) under WSL..." -ForegroundColor Yellow
    Invoke-Native { & wsl bash -lc "cd '$wslPc64' && rm -rf build/esp && UNO_DEBUG=0 ./build.sh" } "production build failed"
    Invoke-Native { & wsl bash -lc "cd '$wslPc64' && rm -rf build/esp-prod && cp -r build/esp build/esp-prod" } "snapshot prod ESP"
    Write-Host "Building DEBUG / stress OS (UNO_DEBUG=1) under WSL..." -ForegroundColor Yellow
    Invoke-Native { & wsl bash -lc "cd '$wslPc64' && rm -rf build/esp && UNO_DEBUG=1 ./build.sh" } "debug build failed"
    Invoke-Native { & wsl bash -lc "cd '$wslPc64' && rm -rf build/esp-debug && cp -r build/esp build/esp-debug" } "snapshot debug ESP"
}
# The raw dd/Rufus image is the PRODUCTION build (the default a normal user
# wants). build/esp is left as PRODUCTION for mkuefi.
Invoke-Native { & wsl bash -lc "cd '$wslPc64' && rm -rf build/esp && cp -r build/esp-prod build/esp" } "restore prod ESP for the raw image"
Write-Host "Packing UEFI disk image ($SizeMiB MiB, production) under WSL..."
Invoke-Native { & wsl bash -lc "cd '$wslPc64' && python3 tools/mkuefi.py $SizeMiB" } "mkuefi.py failed (needs sgdisk + mtools in WSL)"

$img = Join-Path $build "unodos-uefi.img"
if (-not (Test-Path $img)) { throw "Missing image: $img" }

# ---- 3. zip BOTH ESP trees into embeddable resources ------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
$espProd  = Join-Path $build "esp-prod"
$espDebug = Join-Path $build "esp-debug"
foreach ($p in @($espProd, $espDebug)) {
    if (-not (Test-Path (Join-Path $p "EFI\BOOT\BOOTX64.EFI"))) {
        throw "$p\EFI\BOOT\BOOTX64.EFI missing - run without -SkipBuild"
    }
}
$zipProd  = Join-Path $build "unodos_esp_prod.zip"
$zipDebug = Join-Path $build "unodos_esp_debug.zip"
Remove-Item $zipProd, $zipDebug -ErrorAction SilentlyContinue
Write-Host "Zipping both ESP trees for embedding..."
[IO.Compression.ZipFile]::CreateFromDirectory($espProd,  $zipProd,  [IO.Compression.CompressionLevel]::Optimal, $false)
[IO.Compression.ZipFile]::CreateFromDirectory($espDebug, $zipDebug, [IO.Compression.CompressionLevel]::Optimal, $false)
Write-Host ("  prod  = {0} MB, debug = {1} MB" -f `
    [math]::Round((Get-Item $zipProd).Length / 1MB, 1),
    [math]::Round((Get-Item $zipDebug).Length / 1MB, 1))

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
    "/resource:$zipProd,unodos_esp_prod",
    "/resource:$zipDebug,unodos_esp_debug"
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

Remove-Item $zipProd, $zipDebug -ErrorAction SilentlyContinue
$mb = [math]::Round((Get-Item $exe).Length / 1MB, 1)
Write-Host "Built $exe  ($mb MB)"
