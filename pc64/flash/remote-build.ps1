# Shared remote-build plumbing for the flasher scripts (dot-source this file).
#
# The pc64 OS is a mingw cross-build that used to run under WSL on this box.
# WSL2 is permanently broken on amanuensis (nested Hyper-V on a no-MBEC host;
# see the machine-level CLAUDE.md), so the build now runs on a Linux build box
# over SSH and the artifacts are pulled back. Provides:
#
#   $BuildHost / $BuildDir       where the build happens; defaults quill +
#                                /work/unodos-flasher, overridden with the
#                                UNO_BUILD_HOST / UNO_BUILD_DIR env vars
#   Invoke-Remote <cmd> <what>   run a command there, streaming its output.
#                                ssh writes banners to stderr, which PowerShell
#                                5.1 under $ErrorActionPreference='Stop' turns
#                                into a terminating NativeCommandError on a
#                                perfectly good run - so stderr is tolerated
#                                and the exit code is what gates.
#   Push-SourceTree              ship this checkout (minus .git + pc64/build)
#                                into a FRESH $BuildDir
#   Pull-BuildArtifacts <paths>  fetch paths under $BuildDir/pc64/build back
#                                into the local pc64/build
#
# The tar pipes run under cmd.exe, NOT the PowerShell pipeline: PS 5.1
# re-encodes a native-to-native pipe as text and corrupts binary data.

$script:BuildHost = if ($env:UNO_BUILD_HOST) { $env:UNO_BUILD_HOST } else { 'quill' }
$script:BuildDir  = if ($env:UNO_BUILD_DIR)  { $env:UNO_BUILD_DIR }  else { '/work/unodos-flasher' }

function Invoke-Remote([string]$cmd, [string]$what) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & ssh -o BatchMode=yes $script:BuildHost $cmd 2>&1 | ForEach-Object { Write-Host $_ } }
    finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$what (exit $LASTEXITCODE)" }
}

function Push-SourceTree {
    $repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    Write-Host "Shipping the source tree to ${script:BuildHost}:${script:BuildDir} ..."
    $line = 'tar -cz --exclude .git --exclude pc64/build -C "' + $repo + '" . | ' +
            'ssh -o BatchMode=yes ' + $script:BuildHost +
            ' "rm -rf ' + $script:BuildDir + ' && mkdir -p ' + $script:BuildDir +
            ' && tar xzf - -C ' + $script:BuildDir +
            " && find $($script:BuildDir) -name '*.sh' -exec chmod +x {} +`""
    & $env:ComSpec /c $line
    if ($LASTEXITCODE -ne 0) { throw "shipping the source tree failed (exit $LASTEXITCODE)" }
}

function Pull-BuildArtifacts([string[]]$paths) {
    $build = Join-Path (Split-Path $PSScriptRoot -Parent) 'build'
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    foreach ($p in $paths) {   # a stale local copy would shadow the fresh pull
        $local = Join-Path $build ($p -replace '/', '\')
        if (Test-Path $local) { Remove-Item -Recurse -Force $local }
    }
    Write-Host ("Pulling {0} back from {1} ..." -f ($paths -join ', '), $script:BuildHost)
    $line = 'ssh -o BatchMode=yes ' + $script:BuildHost +
            ' "cd ' + $script:BuildDir + '/pc64/build && tar czf - ' + ($paths -join ' ') + '" | ' +
            'tar -xzf - -C "' + $build + '"'
    & $env:ComSpec /c $line
    if ($LASTEXITCODE -ne 0) { throw "pulling build artifacts failed (exit $LASTEXITCODE)" }
}
