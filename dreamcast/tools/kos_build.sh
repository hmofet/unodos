#!/bin/bash
set -x
LOG=/mnt/c/Users/arin/Documents/Github/unodos/dreamcast/tools/kos_build.log
exec > "$LOG" 2>&1
echo "=== KOS toolchain + libkos build $(date) ==="
export DEBIAN_FRONTEND=noninteractive
cd "$HOME" || exit 1
[ -d KallistiOS ] || git clone --depth 1 https://github.com/KallistiOS/KallistiOS.git || { echo CLONE_FAILED; exit 1; }
cd "$HOME/KallistiOS/utils/kos-chain" || { echo NO_KOSCHAIN; exit 1; }
cp -f Makefile.dreamcast.cfg Makefile.cfg
echo "--- kos-chain make (toolchain) $(date) ---"
make -j"$(nproc)" || { echo TOOLCHAIN_BUILD_FAILED; exit 1; }
echo "--- toolchain done $(date) ---"
ls -la /opt/toolchains/dc/sh-elf/bin/ 2>&1 | head
# configure + build KOS itself
cd "$HOME/KallistiOS"
[ -f environ.sh ] || cp doc/environ.sh.sample environ.sh
source environ.sh
echo "KOS_BASE=$KOS_BASE  kos-cc=$(which kos-cc 2>/dev/null)"
make -j"$(nproc)" || { echo KOS_BUILD_FAILED; exit 1; }
echo "=== ALL DONE $(date) ==="
