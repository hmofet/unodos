# Build the UnoDOS Remote Desktop client - a single self-contained Windows exe.
#
# Mirrors pc64/flash/build-flasher.ps1: locates the in-box .NET Framework 4.x
# csc and compiles the WinForms sources to build/UnoRemote.exe.  WinForms (not
# WPF) so the whole thing builds with csc alone - no MSBuild / XAML compiler -
# exactly like the flasher.
#
#   pc64\remote\build-remote.ps1                 # -> build\UnoRemote.exe
#   pc64\remote\build-remote.ps1 -Ffmpeg C:\path\to\ffmpeg.exe
#       copies ffmpeg.exe beside the exe so recording writes real .mp4 out of
#       the box (otherwise recording falls back to a PNG frame sequence).
[CmdletBinding()]
param([string]$Ffmpeg)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot          # pc64\
$build = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $build | Out-Null

# ---- locate csc (in-box .NET Framework), same as the flasher -----------------
$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe" }
if (-not (Test-Path $csc)) { throw "csc.exe (.NET Framework 4.x) not found" }

$exe      = Join-Path $build "UnoRemote.exe"
$manifest = Join-Path $PSScriptRoot "app.manifest"
$sources  = @("Urc.cs", "Qoi.cs", "Recorder.cs", "RemoteMain.cs") |
            ForEach-Object { Join-Path $PSScriptRoot $_ }

$cscArgs = @(
    "/target:winexe",
    "/out:$exe",
    "/win32manifest:$manifest",
    "/reference:System.Windows.Forms.dll",
    "/reference:System.Drawing.dll",
    "/optimize+"
) + $sources

Write-Host "Compiling UnoRemote.exe..."
& $csc $cscArgs
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }

if ($Ffmpeg) {
    if (-not (Test-Path $Ffmpeg)) { throw "ffmpeg not found: $Ffmpeg" }
    Copy-Item $Ffmpeg (Join-Path $build "ffmpeg.exe") -Force
    Write-Host "Bundled ffmpeg.exe beside the client (recording -> .mp4)."
}

Write-Host "Built $exe"
