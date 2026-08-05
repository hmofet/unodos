#!/bin/sh
# build-host-test.sh - link the freestanding quickjs objects + qjs_port +
# unojs's real ujs_math into a host exe (see qjs_host_test.c). Run from pc64/:
#   sh quickjs/test/build-host-test.sh && ./build/qjs_host_test.exe
set -e
cd "$(dirname "$0")/../.."
CC="${CC:-x86_64-w64-mingw32-gcc}"
FLAGS="-O2 -w -ffreestanding -fno-stack-protector -fno-stack-check -nostdinc \
       -Iquickjs/compat -Iinclude -Iquickjs \
       -D__wasi__ -U_WIN32 -U_WIN64 -U__MINGW32__ -U__MINGW64__ \
       -Dalloca=__builtin_alloca -DUNO_PC64"
mkdir -p build
for f in quickjs libregexp libunicode dtoa qjs_port; do
    $CC $FLAGS -c "quickjs/$f.c" -o "build/qjs_$f.o"
done
$CC $FLAGS -c ../unojs/ujs_math.c -o build/qjs_t_ujsmath.o
$CC $FLAGS -c quickjs/test/qjs_host_test.c -o build/qjs_t_main.o
# hosted link: the CRT supplies malloc/printf/str*; object definitions win
# over import-library ones for the handful of overlapping stdio names.
$CC -o build/qjs_host_test.exe build/qjs_t_main.o build/qjs_quickjs.o \
    build/qjs_libregexp.o build/qjs_libunicode.o build/qjs_dtoa.o \
    build/qjs_qjs_port.o build/qjs_t_ujsmath.o
echo "built build/qjs_host_test.exe"
