#!/bin/sh
# build-host-test.sh - the CS1 host test: the whole vendored CSS stack
# compiled with the kernel's freestanding flags, linked against the host
# CRT, driving parse -> cascade -> computed-style assertions.
# Run from the repo root:
#   sh csslib/test/build-host-test.sh && ./csslib/test/build/css_host_test.exe
set -e
cd "$(dirname "$0")/../.."
CC="${CC:-x86_64-w64-mingw32-gcc}"
# Must stay in lockstep with the eventual build.sh block (one flag set, no
# drift). See csslib/VENDOR.md for what each pin is for - especially
# -D_ALIGNED= (upstream's stray-global quirk) and -DWITHOUT_ICONV_FILTER.
BASE="-O2 -ffreestanding -fno-stack-protector -fno-stack-check -nostdinc \
      -Icsslib/compat -Ipc64/include \
      -Icsslib/css/include -Icsslib/parserutils/include -Icsslib/wapcaplet/include \
      -DWITHOUT_ICONV_FILTER -D_ALIGNED= -DNDEBUG ${SAN:-}"
B=csslib/test/build
rm -rf "$B"; mkdir -p "$B"
n=0
for f in $(find csslib/css/src -name '*.c' | sort); do
    $CC $BASE -w -Icsslib/css/src -c "$f" -o "$B/c$n.o"; n=$((n+1))
done
for f in $(find csslib/parserutils/src -name '*.c' | sort); do
    $CC $BASE -w -Icsslib/parserutils/src -c "$f" -o "$B/p$n.o"; n=$((n+1))
done
$CC $BASE -w -c csslib/wapcaplet/src/libwapcaplet.c -o "$B/wap.o"
$CC $BASE -Wall -Wextra -c csslib/css_port.c -o "$B/port.o"
$CC $BASE -Wall -c csslib/test/css_host_test.c -o "$B/main.o"
$CC ${SAN:-} -o "$B/css_host_test.exe" "$B"/*.o
echo "built $B/css_host_test.exe ($((n + 3)) objects)"
