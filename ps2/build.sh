#!/bin/sh
# UnoDOS/PS2 build.
#
#   ./build.sh [host]   software-framebuffer splash via the host compiler,
#                       renders shots/m0_splash.png. VERIFIED on the PC (run
#                       in WSL on this Windows box: gcc 13 + python3 13 there).
#   ./build.sh ee       the PS2 ELF via PS2SDK (ee-gcc + gsKit). Needs $PS2SDK
#                       (the ps2dev Docker image or a PS2SDK install). UNVERIFIED
#                       here - no ee-gcc / PCSX2 / BIOS on the dev machine.
#
# Both regenerate build/font_data.h from the shared font first.
set -e
cd "$(dirname "$0")"
PY="${PY:-python3}"
mkdir -p build shots

echo "[1/2] exporting the shared font to a C array..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i >/dev/null)
"$PY" mkfont_c.py

case "$1" in
  ee)
    echo "[2/2] building the PS2 ELF (PS2SDK)..."
    # default to the prebuilt ps2dev toolchain layout (this machine: WSL,
    # ~/ps2dev/ps2dev from the ps2dev v2.0.0 release). Override PS2DEV to point
    # elsewhere.
    : "${PS2DEV:=$HOME/ps2dev/ps2dev}"
    export PS2DEV
    export PS2SDK="${PS2SDK:-$PS2DEV/ps2sdk}"
    export GSKIT="${GSKIT:-$PS2DEV/gsKit}"
    export PATH="$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2SDK/bin:$PATH"
    make clean >/dev/null 2>&1 || true
    make
    echo "done: build/unodos-ps2.elf" ;;
  *)
    echo "[2/2] building + running the host splash..."
    CC="${CC:-gcc}"
    "$CC" -O2 -Wall -o build/host_splash fb.c uno_splash.c host_main.c
    ./build/host_splash shots/m0_splash.ppm "${2:-320}" "${3:-224}"
    "$PY" tools/ppm2png.py shots/m0_splash.ppm shots/m0_splash.png
    echo "done: shots/m0_splash.png" ;;
esac
