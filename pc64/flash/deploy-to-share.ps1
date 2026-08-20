# Rebuild the UnoDOS/pc64 USB flasher and publish it to the network share so it
# can be flashed from any computer on the LAN.
#
#   \\behemoth\files\software\unodos\pc64\
#     UnoDosFlasher.exe            one-click Windows installer (image embedded)
#     unodos-pc64-uefi.img.gz      raw image for Rufus / balenaEtcher / dd
#     unodos-pc64.iso              hybrid UEFI ISO: VM CD-ROM boot AND
#                                  Rufus/Etcher/dd to USB (tools/mkiso.py)
#
# Publishing is OPT-IN (see the repo CLAUDE.md, 2026-07-23): network install
# supersedes the old "deploy after every build" rule - run this only when a
# fresh bootable USB flasher is actually wanted.
#
# Usage:  pc64\flash\deploy-to-share.ps1 [-SkipBuild] [-SizeMiB 512] [-Dest <path>]
#   -SkipBuild : reuse build/UnoDosFlasher.exe + build/unodos-uefi.img as-is
param(
    [switch]$SkipBuild,
    [int]$SizeMiB = 512,
    [string]$Dest = '\\behemoth\files\software\unodos\pc64'
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

# 2. make sure the SHARE is reachable, then ensure the folder tree exists.
# Reachability is tested at \\server\share, not $Dest's parent - intermediate
# folders (software\unodos\) may not exist yet and New-Item creates them.
$shareRoot = Split-Path $Dest -Parent
$share = '\\' + (($Dest -split '\\')[2..3] -join '\')
if (-not (Test-Path $share)) {
    throw "Share not reachable: $share  (is behemoth online?)"
}
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# 3. gzip the raw image (cross-platform flashers read .gz directly)
$gz = Join-Path $build "unodos-pc64-uefi.img.gz"
Write-Host "Compressing image for the share..."
$in  = [IO.File]::Open($img, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
$out = [IO.File]::Create($gz)
$gzs = New-Object IO.Compression.GZipStream($out, [IO.Compression.CompressionLevel]::Optimal)
$in.CopyTo($gzs); $gzs.Dispose(); $out.Dispose(); $in.Dispose()

# 3b. build the hybrid UEFI ISO from the remote build/esp (VM CD + dd-to-USB).
# The remote tree is the one build-flasher.ps1 shipped and built; its build/esp
# was left as the PRODUCTION tree, which is what the ISO should carry.
$iso = Join-Path $build "unodos-pc64.iso"
if (-not $SkipBuild -or -not (Test-Path $iso)) {
    . (Join-Path $PSScriptRoot "remote-build.ps1")
    Write-Host "Building the hybrid ISO (tools/mkiso.py on $BuildHost)..."
    Invoke-Remote "cd $BuildDir/pc64 && python3 tools/mkiso.py" "mkiso.py failed (needs xorriso + mtools on $BuildHost, and the tree build-flasher.ps1 ships there - run without -SkipBuild)"
    Pull-BuildArtifacts @('unodos-pc64.iso')
}

# 4. copy the flasher + image + ISO to the share
Write-Host "Copying to $Dest ..."
Copy-Item $exe -Destination $Dest -Force
Copy-Item $gz  -Destination $Dest -Force
Copy-Item $iso -Destination $Dest -Force
Remove-Item $gz -ErrorAction SilentlyContinue

# 4b. stamp the staged flasher so deployed copies can self-update against it.
# Written AFTER the exe so a client that checks mid-deploy sees the old stamp
# (and at worst a sha mismatch it retries), never a new stamp with an old exe.
$stampFile  = Join-Path $build "flasher-build.txt"
$buildStamp = if (Test-Path $stampFile) { (Get-Content $stampFile -TotalCount 1).Trim() }
              else { (Get-Item $exe).LastWriteTime.ToString('yyyyMMdd-HHmmss') }
$sha = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path (Join-Path $Dest "flasher-version.txt") -Encoding ASCII -Value @(
    "build=$buildStamp",
    "sha256=$sha",
    "size=$((Get-Item $exe).Length)")

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
