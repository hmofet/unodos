#!/bin/sh
# build-host-test.sh - the two host tests for the quickjs engine option.
#   qjs_host_test.exe      engine core + port layer (31 checks)
#   qjs_dispatch_test.exe  the browser-visible js_run() seam, both engines
# Links the SAME freestanding-compiled objects against the host CRT.
# Run from pc64/:
#   sh quickjs/test/build-host-test.sh
#   ./build/qjs_host_test.exe && ./build/qjs_dispatch_test.exe
set -e
cd "$(dirname "$0")/../.."
CC="${CC:-x86_64-w64-mingw32-gcc}"
# Must mirror build.sh's QJSCF exactly (one personality, no drift):
# __DJGPP not __wasi__ - both mean "single-threaded, no OS" to quickjs-ng,
# but __wasi__ also compiles out the C-stack overflow check, and quickjs
# recurses the C stack on the kernel's guard-pageless UEFI stack.
FLAGS="-O2 -w -ffreestanding -fno-stack-protector -fno-stack-check -nostdinc \
       -Iquickjs/compat -Iinclude -Iquickjs \
       -D__DJGPP -U_WIN32 -U_WIN64 -U__MINGW32__ -U__MINGW64__ \
       -Dalloca=__builtin_alloca -DJS_NAN_BOXING=0 -DUNO_PC64"
mkdir -p build
for f in quickjs libregexp libunicode dtoa qjs_port; do
    $CC $FLAGS -c "quickjs/$f.c" -o "build/qjs_$f.o"
done
$CC $FLAGS -c ../unojs/ujs_math.c -o build/qjs_t_ujsmath.o
$CC $FLAGS -c quickjs/test/qjs_host_test.c -o build/qjs_t_main.o
QJS_OBJS="build/qjs_quickjs.o build/qjs_libregexp.o build/qjs_libunicode.o \
          build/qjs_dtoa.o build/qjs_qjs_port.o"
# hosted link: the CRT supplies malloc/printf/str*; object definitions win
# over import-library ones for the handful of overlapping stdio names.
$CC -o build/qjs_host_test.exe build/qjs_t_main.o $QJS_OBJS build/qjs_t_ujsmath.o
echo "built build/qjs_host_test.exe"

# the dispatch test: js.c + qjsweb.c + the FULL unojs engine, so the same
# script runs through js_run() on both engines and the outputs are compared.
for f in ujs_core ujs_lex ujs_comp ujs_vm ujs_lib ujs_api; do
    $CC $FLAGS -c "../unojs/$f.c" -o "build/qjs_t_$f.o"
done
$CC $FLAGS -I. -c js.c -o build/qjs_t_js.o
$CC $FLAGS -I. -c qjsweb.c -o build/qjs_t_qjsweb.o
$CC $FLAGS -I. -c quickjs/test/qjs_dispatch_test.c -o build/qjs_t_dmain.o
UJS_OBJS="build/qjs_t_ujsmath.o build/qjs_t_ujs_core.o build/qjs_t_ujs_lex.o \
          build/qjs_t_ujs_comp.o build/qjs_t_ujs_vm.o build/qjs_t_ujs_lib.o \
          build/qjs_t_ujs_api.o"
$CC -o build/qjs_dispatch_test.exe build/qjs_t_dmain.o build/qjs_t_js.o \
    build/qjs_t_qjsweb.o $QJS_OBJS $UJS_OBJS
echo "built build/qjs_dispatch_test.exe"

# the M5 test: the live DOM bindings, on both engines. Needs unoweb too,
# since the whole point is that scripts change a real tree.
for f in uw_dom uw_html uw_css uw_style uw_layout; do
    $CC $FLAGS -w -c "../unoweb/$f.c" -o "build/qjs_t_$f.o"
done
$CC $FLAGS -I. -Wall -Wextra -c webjs.c -o build/qjs_t_webjs.o
$CC $FLAGS -I. -c quickjs/test/webjs_test.c -o build/qjs_t_wmain.o
$CC -o build/webjs_test.exe build/qjs_t_wmain.o build/qjs_t_webjs.o \
    build/qjs_t_js.o build/qjs_t_qjsweb.o $QJS_OBJS $UJS_OBJS \
    build/qjs_t_uw_dom.o build/qjs_t_uw_html.o build/qjs_t_uw_css.o \
    build/qjs_t_uw_style.o build/qjs_t_uw_layout.o
echo "built build/webjs_test.exe"

# the cookie jar: pure C, no engine - its own tiny link
$CC $FLAGS -Wall -Wextra -c pc64_cookie.c -o build/qjs_t_cookie.o
$CC $FLAGS -I. -c quickjs/test/cookie_test.c -o build/qjs_t_cmain.o
$CC -o build/cookie_test.exe build/qjs_t_cmain.o build/qjs_t_cookie.o
echo "built build/cookie_test.exe"

# the response cache: same shape as the jar - pure C, its own tiny link
$CC $FLAGS -Wall -Wextra -c pc64_cache.c -o build/qjs_t_cache.o
$CC $FLAGS -I. -c quickjs/test/cache_test.c -o build/qjs_t_camain.o
$CC -o build/cache_test.exe build/qjs_t_camain.o build/qjs_t_cache.o
echo "built build/cache_test.exe"
