#!/bin/bash
# UnoDOS/Mac build. Run inside WSL/Linux with the Retro68 toolchain built.
#   R68=/opt/Retro68-build ./build.sh
set -e
cd "$(dirname "$0")"
R68="${R68:-/opt/Retro68-build}"
TC="$R68/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake"

if [ ! -f "$TC" ]; then
    echo "Retro68 toolchain not found at $TC" >&2
    echo "Build it first: <Retro68>/build-toolchain.bash --no-ppc --no-carbon" >&2
    exit 1
fi

rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build . -j"$(nproc)"

echo "---"
ls -la UnoDOS7.bin UnoDOS7.dsk UnoDOSClassic.bin UnoDOSClassic.dsk 2>/dev/null || ls -la
echo "built UnoDOS7 (color) and UnoDOSClassic (mono)"
