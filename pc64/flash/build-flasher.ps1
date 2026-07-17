# Build the UnoDOS USB Flasher - a single self-contained Windows exe.
#
# Bundles the UEFI disk image (build/unodos-uefi.img) as one gzip-compressed
# embedded resource (the mostly-empty image compresses to a few MB), so the exe
# stays small and runs from anywhere with no install.  Compiles with the in-box
# .NET Framework csc (no SDK needed).
#
# Pipeline (all under WSL, which has the mingw cross-compiler + sgdisk + mtools):
#   1. ./build.sh          -> build/esp/ + build/esp/EFI/BOOT/BOOTX64.EFI
#   2. tools/mkuefi.py N   -> build/unodos-uefi.img  (GPT + ESP FAT32, N MiB)
#   3. gzip + csc          -> build/UnoDosFlasher.exe
#
# Usage:  pc64/flash/build-flasher.ps1 [-SizeMiB 512] [-SkipBuild]
#   -SkipBuild : reuse build/esp/ as-is (don't re-run ./build.sh)
param(
    [int]$SizeMiB = 512,      # capacity of the release image (documents get room)
    [switch]$SkipBuild        # reuse the already-built ESP tree
)
$ErrorActionPreference = "Stop"
$pc64  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $pc64 "build"

# C:\...\unodos\pc64  ->  /mnt/c/.../unodos/pc64  (no wslpath: arg quoting eats backslashes)
$wslPc64 = "/mnt/" + $pc64.Substring(0,1).ToLower() + ($pc64.Substring(2) -replace '\\','/')

# ---- 1+2. build the EFI image and pack it into a raw UEFI disk image --------
if (-not $SkipBuild) {
    Write-Host "Building UnoDOS/pc64 (BOOTX64.EFI + ESP) under WSL..."
    & wsl bash -lc "cd '$wslPc64' && ./build.sh"
    if ($LASTEXITCODE -ne 0) { throw "pc64 build failed (try: wsl bash -lc 'cd pc64 && ./build.sh')" }
}
Write-Host "Packing UEFI disk image ($SizeMiB MiB) under WSL..."
& wsl bash -lc "cd '$wslPc64' && python3 tools/mkuefi.py $SizeMiB"
if ($LASTEXITCODE -ne 0) { throw "mkuefi.py failed (needs sgdisk + mtools in WSL)" }

$img = Join-Path $build "unodos-uefi.img"
if (-not (Test-Path $img)) { throw "Missing image: $img" }

# ---- 3. gzip the image into an embeddable resource --------------------------
$gz = Join-Path $build "unodos_uefi.img.gz"
Write-Host "Compressing $([IO.Path]::GetFileName($img))..."
# share ReadWrite so a lingering drvfs/WSL handle on the image doesn't block us
$in  = [IO.File]::Open($img, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
$out = [IO.File]::Create($gz)
$gzs = New-Object IO.Compression.GZipStream($out, [IO.Compression.CompressionLevel]::Optimal)
$in.CopyTo($gzs)
$gzs.Dispose(); $out.Dispose(); $in.Dispose()

# ---- locate csc (in-box .NET Framework) -------------------------------------
$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe" }
if (-not (Test-Path $csc)) { throw "csc.exe (.NET Framework 4.x) not found" }

$src      = Join-Path $PSScriptRoot "UnoDosFlash.cs"
$manifest = Join-Path $PSScriptRoot "app.manifest"
$icon     = Join-Path $PSScriptRoot "unodos.ico"
$exe      = Join-Path $build "UnoDosFlasher.exe"

$args = @(
    "/target:winexe",
    "/out:$exe",
    "/win32manifest:$manifest",
    "/reference:System.Management.dll",
    "/reference:System.Windows.Forms.dll",
    "/reference:System.Drawing.dll",
    "/resource:$gz,unodos_uefi"
)
if (Test-Path $icon) { $args += "/win32icon:$icon" }
$args += @("/optimize+", "$src")

Write-Host "Compiling $([IO.Path]::GetFileName($exe))..."
& $csc $args
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }

Remove-Item $gz -ErrorAction SilentlyContinue
$mb = [math]::Round((Get-Item $exe).Length / 1MB, 1)
Write-Host "Built $exe  ($mb MB)"
