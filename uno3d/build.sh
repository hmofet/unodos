#!/bin/sh
# uno3d build - the portable 3D library + the cube demo, per backend.
#
#   ./build.sh host    software backend on the PC -> build/cube.ppm + .png
#                      (the verifiable reference; same lib the consoles build)
#   ./build.sh ps2     PS2 Graphics Synthesizer (hardware 3D) via PS2SDK+gsKit
#                      -> build/uno3d-cube-ps2.elf  (PCSX2 / FMCB)
#   ./build.sh dc      Dreamcast PowerVR2 (hardware 3D) via KallistiOS
#                      -> build/uno3d-cube-dc.elf
#
# Write-once proof: uno3d.c + uno3d_demo.c are byte-identical across all three;
# only the backend (uno3d_soft.c / uno3d_ps2.c / uno3d_dc.c) and the tiny glue
# main differ.
set -e
cd "$(dirname "$0")"
mkdir -p build
PY="${PY:-python3}"

case "$1" in
  ps2|ps2-game)
    : "${PS2DEV:=$HOME/ps2dev/ps2dev}"
    export PS2DEV
    export PS2SDK="${PS2SDK:-$PS2DEV/ps2sdk}"
    export GSKIT="${GSKIT:-$PS2DEV/gsKit}"
    export PATH="$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2SDK/bin:$PATH"
    make -f Makefile.ps2 clean >/dev/null 2>&1 || true
    if [ "$1" = "ps2-game" ]; then
      make -f Makefile.ps2 EE_BIN=build/uno3d-runner-ps2.elf \
        EE_OBJS="build/uno3d.o build/uno3d_ps2.o build/uno3d_game.o build/uno3d_ps2_game.o"
      echo "done: build/uno3d-runner-ps2.elf"
    else
      make -f Makefile.ps2
      echo "done: build/uno3d-cube-ps2.elf"
    fi ;;

  dc|dc-game)
    # source the KallistiOS environment (sh-elf-gcc) if not already present
    if [ -z "$KOS_BASE" ]; then
      for E in /opt/toolchains/dc/kos/environ.sh "$HOME/KallistiOS/environ.sh" \
               "$HOME/dc/kos/environ.sh"; do
        [ -f "$E" ] && { . "$E"; break; }
      done
    fi
    if [ -z "$KOS_BASE" ]; then
      echo "ERROR: KallistiOS not found; set KOS_BASE / source environ.sh." >&2
      exit 1
    fi
    make -f Makefile.dc clean >/dev/null 2>&1 || true
    if [ "$1" = "dc-game" ]; then
      make -f Makefile.dc TARGET=build/uno3d-runner-dc.elf \
        OBJS="build/uno3d.o build/uno3d_dc.o build/uno3d_game.o build/uno3d_dc_game.o"
      echo "done: build/uno3d-runner-dc.elf"
    else
      make -f Makefile.dc
      echo "done: build/uno3d-cube-dc.elf"
    fi ;;

  host-game)
    # the game on the software backend; autopilot N frames -> PPM -> PNG.
    CC="${CC:-gcc}"
    F="${2:-40}"
    "$CC" -O2 -Wall -I. -I../ps2 \
        uno3d.c uno3d_soft.c uno3d_game.c host_game.c -lm -o build/host_game
    ./build/host_game build/game.ppm "$F"
    "$PY" ../ps2/tools/ppm2png.py build/game.ppm build/game.png
    echo "done: build/game.png" ;;

  *)
    # host software backend; render one demo frame to a PPM then PNG.
    CC="${CC:-gcc}"
    T="${2:-0.6}"
    "$CC" -O2 -Wall -I. -I../ps2 \
        uno3d.c uno3d_soft.c uno3d_demo.c host_demo.c -lm -o build/host_demo
    ./build/host_demo build/cube.ppm "$T"
    "$PY" ../ps2/tools/ppm2png.py build/cube.ppm build/cube.png
    echo "done: build/cube.png" ;;
esac
