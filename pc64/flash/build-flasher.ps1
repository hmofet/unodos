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
# Pipeline (steps 1+2 on the Linux build box over SSH - see remote-build.ps1;
# it has the mingw cross-compiler + sgdisk + mtools, and WSL is broken here):
#   1. ./build.sh          -> build/esp/ + build/esp/EFI/BOOT/BOOTX64.EFI
#   2. tools/mkuefi.py N   -> build/unodos-uefi.img  (GPT + ESP FAT32, N MiB)
#      ... then the ESP trees + image are pulled back to the local pc64/build
#   3. zip build/esp + csc -> build/UnoDosFlasher.exe   (local, in-box .NET)
#
# Usage:  pc64/flash/build-flasher.ps1 [-SizeMiB 512] [-SkipBuild] [-TestTool] [-Publish]
#   -SkipBuild : reuse build/esp/ as-is (don't re-run ./build.sh)
#   -TestTool  : also build build/UnoDiskTest.exe, which runs the same volume
#                builder into an image FILE so fsck.vfat / sgdisk / QEMU can
#                check it (see tools/diskboot_test.py)
#   -Publish   : build a REDISTRIBUTABLE flasher: both embedded ESP trees are
#                built UNO_NOFW=1 (no Intel firmware) with the remote tree's
#                fw-blobs/, shareware WAD and skin removed first, Freedoom
#                fetched as the game data, and the pulled trees are gated
#                before embedding (any *.UCO/*.PNV/FIRMWARE, or a WAD that is
#                not Freedoom, fails the build). This is the ONLY build that
#                may be attached to a GitHub release; the default build embeds
#                Intel firmware for local sticks and must never be published.
param(
    [int]$SizeMiB = 512,      # capacity of the release image (documents get room)
    [switch]$SkipBuild,       # reuse the already-built ESP tree
    [switch]$TestTool,        # also build the headless image-builder for tests
    [switch]$Publish          # redistributable build: no firmware, Freedoom WAD
)
$ErrorActionPreference = "Stop"
$pc64  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $pc64 "build"

. (Join-Path $PSScriptRoot "remote-build.ps1")

# ---- 1+2. build BOTH the production and debug OS, pack the release image -----
# The flasher embeds TWO ESP trees:
#   - PRODUCTION (UNO_DEBUG=0): what it flashes by default (a clean OS, no
#     \CRASH, no \DEBUG.CFG, no stress driver).
#   - DEBUG      (UNO_DEBUG=1): flashed only when Developer options is on -
#     crash reports to \CRASH, the stress driver, and the test harness the
#     dev-options test toggles arm via \DEBUG.CFG.
# (This supersedes the old "ship ONE flasher = the debug build" rule.)
# With -SkipBuild the previously pulled build/esp-prod, build/esp-debug and
# build/unodos-uefi.img are reused as-is; nothing is rebuilt or re-packed.
if (-not $SkipBuild) {
    Push-SourceTree
    $envPrefix = ""
    if ($Publish) {
        # A publishable tree carries no Intel firmware, no id shareware WAD and
        # no Winamp skin. Removing them remotely is the belt; UNO_NOFW=1 is the
        # suspenders (it also DELETES firmware a prior build staged); the gate
        # after the pull is the proof. build.sh falls back to wads/freedoom1.wad
        # once DOOM1.WAD is gone, and fetch-wad.sh defaults to Freedoom.
        Write-Host "Publish build: scrubbing the remote tree + fetching Freedoom..." -ForegroundColor Yellow
        Invoke-Remote "cd $BuildDir/pc64 && rm -rf fw-blobs wads/DOOM1.WAD wads/*.WSZ && ./tools/fetch-wad.sh" "publish-tree scrub / Freedoom fetch failed"
        $envPrefix = "UNO_NOFW=1 "
    }
    # build.sh populates build/esp INCREMENTALLY (no wipe), so a stale CRASH /
    # DEBUG.CFG / FIRMWARE from a prior debug build would leak into the
    # production snapshot. Wipe build/esp before each build to keep them clean.
    Write-Host "Building PRODUCTION OS (UNO_DEBUG=0) on $BuildHost..." -ForegroundColor Yellow
    Invoke-Remote "cd $BuildDir/pc64 && rm -rf build/esp && ${envPrefix}UNO_DEBUG=0 ./build.sh" "production build failed"
    Invoke-Remote "cd $BuildDir/pc64 && rm -rf build/esp-prod && cp -r build/esp build/esp-prod" "snapshot prod ESP"
    Write-Host "Building DEBUG / stress OS (UNO_DEBUG=1) on $BuildHost..." -ForegroundColor Yellow
    Invoke-Remote "cd $BuildDir/pc64 && rm -rf build/esp && ${envPrefix}UNO_DEBUG=1 ./build.sh" "debug build failed"
    Invoke-Remote "cd $BuildDir/pc64 && rm -rf build/esp-debug && cp -r build/esp build/esp-debug" "snapshot debug ESP"
    # The raw dd/Rufus image is the PRODUCTION build (the default a normal user
    # wants). build/esp is left as PRODUCTION for mkuefi (and for mkiso later).
    Invoke-Remote "cd $BuildDir/pc64 && rm -rf build/esp && cp -r build/esp-prod build/esp" "restore prod ESP for the raw image"
    Write-Host "Packing UEFI disk image ($SizeMiB MiB, production) on $BuildHost..."
    Invoke-Remote "cd $BuildDir/pc64 && python3 tools/mkuefi.py $SizeMiB" "mkuefi.py failed (needs sgdisk + mtools on $BuildHost)"
    # esp is pulled too (a prod copy) so local tools that expect build/esp -
    # UnoDiskTest, diskboot_test.py - keep working.
    Pull-BuildArtifacts @('esp-prod', 'esp-debug', 'esp', 'unodos-uefi.img')
}

$img = Join-Path $build "unodos-uefi.img"
if (-not (Test-Path $img)) { throw "Missing image: $img (run without -SkipBuild)" }

# ---- 3. zip BOTH ESP trees into embeddable resources ------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
$espProd  = Join-Path $build "esp-prod"
$espDebug = Join-Path $build "esp-debug"
foreach ($p in @($espProd, $espDebug)) {
    if (-not (Test-Path (Join-Path $p "EFI\BOOT\BOOTX64.EFI"))) {
        throw "$p\EFI\BOOT\BOOTX64.EFI missing - run without -SkipBuild"
    }
}
# ---- publish gate: refuse to embed anything unredistributable ---------------
# Mirrors tools/mkrelease.sh's firmware gate, extended to the WAD. Runs on the
# PULLED trees, so it proves what actually gets embedded, not what was asked.
if ($Publish) {
    foreach ($tree in @($espProd, $espDebug)) {
        $fw = Get-ChildItem -Recurse -Path $tree -Include *.UCO, *.PNV -ErrorAction SilentlyContinue
        if ($fw) { throw "PUBLISH GATE: $tree still contains Intel firmware: $($fw[0].FullName)" }
        if (Test-Path (Join-Path $tree "FIRMWARE")) {
            throw "PUBLISH GATE: $tree still contains a FIRMWARE directory"
        }
        $wad = Join-Path $tree "DOOM1.WAD"
        if (Test-Path $wad) {
            $len = (Get-Item $wad).Length
            if ($len -eq 11159840 -or $len -eq 4196020) {
                throw "PUBLISH GATE: $wad is id Software's shareware WAD ($len bytes), not Freedoom"
            }
            $ascii = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($wad))
            if ($ascii.IndexOf("BSD-3-Clause") -lt 0 -and $ascii.IndexOf("Freedoom") -lt 0) {
                throw "PUBLISH GATE: $wad does not look like Freedoom (no licence marker found)"
            }
        }
        $wsz = Get-ChildItem -Recurse -Path $tree -Include *.WSZ -ErrorAction SilentlyContinue
        if ($wsz) { throw "PUBLISH GATE: $tree contains a Winamp skin: $($wsz[0].FullName)" }
    }
    Write-Host "Publish gate passed: no firmware, no shareware WAD, no skins in either tree." -ForegroundColor Green
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
$reconfig = Join-Path $PSScriptRoot "UnoReconfig.cs"
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
$args += @("/optimize+", "$src", "$disk", "$settings", "$reconfig", "$update", "$verCs")

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
