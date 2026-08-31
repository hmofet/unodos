#!/bin/sh
# UnoDOS pc64-on-ARM (Cosmo Communicator, MT6771) build -- milestone M0.
#
# Toolchain: llvm-mingw's aarch64-w64-mingw32-clang on quill -- PE/COFF and
# LLP64, the same object format and data model as the x86 pc64, per the
# toolchain decision in research/pc64-arm-port-plan.md (hmofet/cosmo). The
# image links at LK's load address so the flat payload needs no relocation;
# flatten.py lays the PE out and cosmo/mkbootimg.py wraps it for the p38 slot.
#
#   ./build.sh          -> build/m0.bin + build/pc64arm-boot.img
#
# Verify with the asm port's harness (the M0 payload honours the same FBINFO
# contract, so all fifteen FDT combinations gate it unchanged):
#   python ../cosmo/harness.py build/m0.bin /tmp/m0.png 40
set -e
cd "$(dirname "$0")"

PY="${PY:-python3}"
QUILL="${QUILL:-arin@192.168.2.114}"
QDIR="/work/unodos-cosmo64"
LMBIN="/opt/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/bin"
CC="$LMBIN/aarch64-w64-mingw32-clang"

# The load-bearing flags (see the header of m0.c):
#   -mstrict-align  code runs BEFORE the MMU is on too (mmu.c's table builder,
#                   the FDT walk) and there all memory is Device: no unaligned
#                   accesses anywhere. After mmu_on, merely a minor cost.
#   -fno-builtin    our memset must not become a call to itself
# -mgeneral-regs-only was retired in M1: entry.s programs CPACR before any C.
CFLAGS="-O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-stack-check \
        -nostdinc -fno-builtin -mstrict-align"
LINK="-nostdlib -Wl,--image-base,0x40080000 -e _start"

mkdir -p build
echo "[1/3] cross-compiling (aarch64-w64-mingw32) on quill..."
ssh "$QUILL" "mkdir -p $QDIR/build"
scp -q entry.s cpu.s m0.c mmu.c "$QUILL:$QDIR/"
ssh "$QUILL" "cd $QDIR && \
  $CC $CFLAGS -c m0.c -o build/m0.o && \
  $CC $CFLAGS -c mmu.c -o build/mmu.o && \
  $CC -c entry.s -o build/entry.o && \
  $CC -c cpu.s -o build/cpu.o && \
  $CC $LINK -o build/m0.exe build/entry.o build/cpu.o build/m0.o build/mmu.o"
scp -q "$QUILL:$QDIR/build/m0.exe" build/

echo "[2/3] flattening PE -> LK payload..."
"$PY" flatten.py build/m0.exe build/m0.bin

echo "[3/3] wrapping in an LK boot image..."
# mkbootimg.py lives in the asm port's lane (cosmo/, branch cosmo-port, not yet
# on master) -- consume it from that worktree until the branches meet.
MKBOOTIMG="${MKBOOTIMG:-../../unodos-cosmo/cosmo/mkbootimg.py}"
[ -f "$MKBOOTIMG" ] || { echo "mkbootimg.py not found at $MKBOOTIMG -- set MKBOOTIMG" >&2; exit 1; }
"$PY" "$MKBOOTIMG" build/m0.bin build/pc64arm-boot.img
echo "    -> cosmo64/build/pc64arm-boot.img"
echo "    install:  scp build/pc64arm-boot.img the-cosmo:  then as root:"
echo "              dd if=pc64arm-boot.img of=/dev/mmcblk0p38 bs=1M conv=fsync"
