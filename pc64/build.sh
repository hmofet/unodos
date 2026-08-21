#!/bin/sh
# UnoDOS/pc64 build - the modern-PC (x86-64 / UEFI) world.
#
#   ./build.sh          build build/BOOTX64.EFI + the ESP tree build/esp/
#   ./build.sh run      build, then boot it headless in QEMU + OVMF (VNC :0)
#
# Toolchain: x86_64-w64-mingw32-gcc (UEFI apps are PE32+ images with the MS
# x64 calling convention - the mingw target's native output, so no gnu-efi
# or EDK2 is needed). Verify: qemu-system-x86_64 + OVMF (harness.py drives
# the QMP screendump/sendkey loop the family's other harnesses use).
set -e
cd "$(dirname "$0")"
PY="${PY:-python3}"

# ---- compiler cache (transparent) --------------------------------------
# ccache ships symlinks for the mingw cross-compiler in /usr/lib/ccache, so
# putting that dir first on PATH turns every `x86_64-w64-mingw32-gcc` call into
# a cache lookup with NO change to the compile lines below. Any file you didn't
# touch (BearSSL's 294, MicroPython's 127, uACPI, and every kernel file off the
# edit path) becomes an instant hit -> incremental rebuilds are seconds, not a
# full recompile. Harmless if ccache isn't installed. Set NOCCACHE=1 to skip.
if [ "${NOCCACHE:-0}" = "0" ] && [ -d /usr/lib/ccache ]; then
    case ":$PATH:" in *:/usr/lib/ccache:*) ;; *) PATH="/usr/lib/ccache:$PATH";; esac
fi
CC="${CC:-x86_64-w64-mingw32-gcc}"

# ---- bounded parallel compile ------------------------------------------
# The compile loops below are independent `gcc -c` calls; running them serially
# wastes an otherwise-idle multi-core box. `pc` launches one in the background
# throttled to $JOBS in flight; `pcwait` drains the batch and FAILS the build if
# any compile did (so a broken file can never slip through to the link). Object
# paths are deterministic, so the OBJS list and the link are unchanged. JOBS=1
# restores the old fully-serial behaviour.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
_PC_PIDS=""
pc() {
    "$@" &
    _PC_PIDS="$_PC_PIDS $!"
    # drain when the pool is full. MUST end returning 0: a bare `[ ] && pcwait`
    # yields the test's exit status, and a false test (pool not yet full - the
    # common case) would return 1 and trip `set -e` on the caller.
    if [ "$(echo $_PC_PIDS | wc -w)" -ge "$JOBS" ]; then pcwait; fi
    return 0
}
pcwait() {
    _rc=0
    for _p in $_PC_PIDS; do wait "$_p" || _rc=1; done
    _PC_PIDS=""
    [ "$_rc" -eq 0 ] || { echo "build: a parallel compile failed - aborting"; exit 1; }
}
mkdir -p build shots

echo "[1/3] exporting the shared font to a C array..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i >/dev/null)
"$PY" mkfont_c.py
# the Clock's world map (public-domain Natural Earth land mask). The GeoJSON is
# cached in tools/, so this is offline after the first run.
[ -f build/world_map.h ] || "$PY" tools/mkworldmap.py

# UNO_DESKTOP=native starts the desktop at the panel's own size (1:1) instead
# of the chunky half-res default.  For EMULATORS: see set_geometry() in
# uefi_main.c for why a real panel keeps the chunky default.  Pairs with
# UNO_BIOS_PREF below, which picks the surface that size is measured against.
DESKDEF=""
case "${UNO_DESKTOP:-half}" in
    native) DESKDEF="-DUNO_DESKTOP_NATIVE=1"
            echo "[build] desktop starts NATIVE (1:1), not half-res" ;;
    half)   ;;
    *)      echo "UNO_DESKTOP must be native or half, got '$UNO_DESKTOP'" >&2; exit 1 ;;
esac

CFLAGS="-O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-stack-check \
        -nostdinc -Iinclude -I. -I../uno3d -Ibearssl/inc \
        -DUNO_COLOR=1 -DUNO_PC64 -Dmain=uno_main $DESKDEF ${UNO_EXTRA:-}"

# ============================================================================
# DEBUG BUILD (branch pc64-debug-stress) - crash reports, watchdog, kernel log,
# boot env block, perf HUD, fuzz/stress driver, sanitizer traps, symbolized
# backtraces.  OFF by default on master: a bare `./build.sh` builds the plain
# shippable OS.  Opt into the debug/test harness with `UNO_DEBUG=1 ./build.sh`
# (which is what the SPECTEST / stress / dbg_crash test scripts and the flasher's
# Developer options use).
#   UNO_DEBUG=1   the debug harness (opt-in)
#   UNO_UBSAN=1   (default when debug) trap signed-overflow/OOB/shift/null on
#                 FIRST-PARTY pc64 code (not bearssl/uacpi/upy)
#   UNO_DBGCON=1  also mirror the log + reports to QEMU debugcon (port 0x402)
#     -> METAL-UNSAFE (SMM-trapped on some laptops); QEMU verification only.
# The report families and how to read them live in pc64/DEBUG.md.
# ============================================================================
UNO_DEBUG="${UNO_DEBUG:-0}"
[ "$UNO_DEBUG" = "0" ] && echo "[build] PRODUCTION build (set UNO_DEBUG=1 for the debug/test harness)"
DBGDEF=""; DBGSAN=""; DBG_ID=""
if [ "$UNO_DEBUG" != "0" ]; then
    # -fno-omit-frame-pointer: the crash handler walks the RBP chain.
    # -mgeneral-regs-only on uno_debug.c only (the interrupt file); the rest of
    #  the kernel keeps SSE (fb blits use it).  Applied per-file below.
    # HHMM matters: every build on a given day was otherwise stamped identically
    # ("debug-local-20260720"), so the splash could say "a debug build" but not
    # WHICH one - useless for confirming that a reflash actually took.
    DBG_ID="debug-$(git rev-parse --short HEAD 2>/dev/null || echo local)-$(date -u +%Y%m%d-%H%M 2>/dev/null || echo x)"
    DBGDEF="-DUNO_DEBUG -DUNO_BUILD_ID=\"$DBG_ID\" -fno-omit-frame-pointer"
    [ "${UNO_DBGCON:-0}" != "0" ] && DBGDEF="$DBGDEF -DUNO_DBGCON"
    # DETACH ON BY DEFAULT IN THE DEBUG BUILD, as of 2026-08-06.
    #
    # It used to be off, on finding F8: "the native stack has no USB mass-storage
    # driver, so ExitBootServices on a USB-booted system strands its own boot
    # volume" - telemetry unwritable, STRESS.CFG unreadable, and every test boot
    # is from USB. That reason stopped being true when usbmsc.c landed:
    # try_detach() has read "a USB boot detaches by DEFAULT as of 2026-07-30,
    # confirmed on metal" since, and the ZimaBlade runs detached off a stick with
    # storage, network, keyboard and mouse all native.
    #
    # Leaving the old default in place had a cost that outweighed the risk it was
    # still guarding: the debug build is the ONLY build that produces a BOOTLOG,
    # so the one build that can observe detach was the one build that compiled it
    # out, and anyone testing detach had to know to say UNO_DETACH=1 (requests
    # file, 2026-08-04). The risk itself now has a RUNTIME escape hatch that
    # needs no rebuild: `DETACH.CFG: off` (never detach) or `nousb` (detach
    # unless this is a USB boot), dropped on the stick from any other machine.
    #
    # UNO_DETACH=0 restores the old compile-time behaviour.
    if [ "${UNO_DETACH:-1}" = "0" ]; then
        DBGDEF="$DBGDEF -DUNO_NO_DETACH"
        echo "[dbg] detach compiled OUT (UNO_DETACH=0)."
    else
        echo "[dbg] detach ENABLED (default since 2026-08-06; DETACH.CFG: off|nousb to override at runtime)."
    fi
    if [ "${UNO_UBSAN:-1}" != "0" ]; then
        # trap-on-error routes every caught UB to a ud2 -> our #UD handler ->
        # a "UBSAN TRAP" crash report with the exact faulting RIP.  Only the
        # bug classes that are already undefined behaviour are trapped.
        DBGSAN="-fsanitize=signed-integer-overflow,bounds,shift,integer-divide-by-zero,null \
                -fsanitize-undefined-trap-on-error -fstack-protector-strong"
    fi
    echo "[dbg] DEBUG build: id=$DBG_ID ubsan=${UNO_UBSAN:-1} dbgcon=${UNO_DBGCON:-0}"
fi

# ============================================================================
# DEFAULT build = the unoui shell: the cross-platform unoui toolkit AS the
# whole UI (a themed desktop + WM + widgets). A lean image with NO legacy UI -
# platform + fb + RAM-disk FS + unoui + 8 themes.
#   ./build.sh          unoui shell -> build/esp
#   ./build.sh run      unoui shell, then boot in QEMU
#   ./build.sh legacy [run]   the old core + 14 apps + net/TLS/3D (below)
# ============================================================================
if [ "$1" != "legacy" ]; then
    echo "[2/3] compiling the unoui shell (default)..."
    # UNO_I2C_TRACKPAD: the native trackpad driver is now self-configuring
    # (enumerates LPSS I2C + probes HID), bounded, and inert when no pad is
    # found (e.g. QEMU), so it ships enabled - it just needs pc64_pci.
    # UNO_ACPI: the unoacpi AML/ACPI stack (vendored uACPI + shared handlers +
    # the pc64 host layer) - battery %/lid via _BST/_LID on real laptops.
    # Read-only, NO_ACPI_MODE, every EC wait bounded -> inert/safe on QEMU.
    # UNO_XHCI: the native USB stack (xHCI host + HID + mass storage). It used
    # to be opt-in because uno_xhci_init() takes the controller away from the
    # firmware, which is fatal while the firmware is still carrying the boot
    # volume. xhci.c now refuses to touch hardware until uno_pc64_detached(),
    # so the stack is inert while attached and ships in every build - which is
    # what lets a USB-booted machine reclaim its own stick at detach (F8/P4)
    # and what gives detached machines USB keyboards and mice.
    UCF="$CFLAGS $DBGDEF -DUNO_UUI -DUNO_I2C_TRACKPAD -DUNO_BG_CACHE -DUNO_ACPI -DUNO_XHCI \
         -I../unoui -I../unosound -I../unomedia -I../unoacpi -I../unoacpi/uacpi/include"
    OBJS=""
    # UnoSound live sequencer (game/app audio over the PC-speaker voice)
    pc "$CC" $UCF -c -o "build/unosound_seq.o" "../unosound/unosound_seq.c"; OBJS="$OBJS build/unosound_seq.o"
    # platform + shell + the legacy-app bridge (mac_compat = Toolbox over fb).
    # $DBGSAN (UBSan trap + stack canary) rides on this FIRST-PARTY set only -
    # third-party bearssl/uacpi/upy below build without it (they do defined
    # unsigned wraparound the sanitizer must not trap).
    # unosecure = the security subsystem (identity/RBAC/escalation/audit);
    # unoscript = the production scripting surface it adjudicates.  These land
    # together (unosecure's strong unosec_* symbols replace unoscript.c's weak
    # fail-closed fallbacks - the r8169 pattern), so tier>=1 script surfaces
    # light up.  See UNOSECURE-SPEC.md / UNOSCRIPT.md.
    # BROWSER_ENGINE=uw now picks which renderer the browser STARTS on; it no
    # longer decides what is built.  Both renderers are in every kernel and the
    # user switches between them on the uno:engine page.  Kept because the gates
    # and harnesses say BROWSER_ENGINE=uw to mean "boot with the engine", and
    # because the flow painter remains the default.
    if [ "${BROWSER_ENGINE:-}" = "uw" ]; then UCF="$UCF -DUW_ENGINE"; fi
    for f in pc64_fetch pc64_cookie pc64_cache webjs fb mac_compat pc64_libc pc64_io pc64_pci uno_devmgr pc64_math pc64_fs blkdev ahci nvme sdhci ide fat unostorage hid_kbd uno_binds i2c_hid xhci usbio usbboot usbmsc usbhid detachgate unovirt hv_svm hv_vmx unovdev unoamp_out unoamp_in unoamp_skin unoamp_vis unoamp_dsp unoamp_enc unoamp_mod unoamp_app unoamp_ui pc64_mtrr ax88179 rtl8152 iwlwifi rtwifi mrvlwifi wifi_wpa wifi_sae uefi_main bios_entry pc64_native pc64_uui pc64_uui_apps pc64_write pc64_files pc64_music pc64_clock pc64_media pc64_modload pc64_games js pc64_http pc64_font pc64_browser pc64_icons pc64_qoi e1000 e1000e igb r8169 net netdisc tls tls_entropy tls_ca acpi_host installer snd_pcm snd_mus hdaudio ac97 unosecure unoscript unoscript_path pc64_accounts unoauto unoauto_compat unoauto_gate unoauto_probe unoauto_remote unoauto_serial unoauto_screen ed25519 unossh_wire unossh unossh_auth unossh_store unossh_cmd sshapp_ui unolog unovdev_pc hv_phases unovdev_net unovirt_mgr unostream; do
        pc "$CC" $UCF $DBGSAN -c -o "build/$f.o" "$f.c"; OBJS="$OBJS build/$f.o"
    done
    # unojs: the JavaScript engine, its own subsystem (unojs/UNOJS.md).  Plain
    # portable C99 with NO OS dependency and NO libm - it carries its own
    # double-precision math because pc64's is float-only and the number
    # formatter needs double exactness.  The browser reaches it only through
    # the js_run() shim in js.c.
    for f in ujs_core ujs_math ujs_lex ujs_comp ujs_vm ujs_lib ujs_api; do
        pc "$CC" $UCF $DBGSAN -c -o "build/$f.o" "../unojs/$f.c"; OBJS="$OBJS build/$f.o"
    done
    # unoweb: the web core (DOM + HTML parser today; CSS, layout and paint to
    # come).  Its own subsystem - see unoweb/UNOWEB.md - and deliberately holds
    # no JavaScript vocabulary: the browser is what joins it to unojs.
    for f in uw_dom uw_html uw_css uw_style uw_layout; do
        pc "$CC" $UCF $DBGSAN -c -o "build/$f.o" "../unoweb/$f.c"; OBJS="$OBJS build/$f.o"
    done
    # quickjs: the vendored second JS engine behind js_run()'s dispatch (see
    # pc64/quickjs/VENDOR.md for the personality + layout pins - do not trim
    # them). Its OWN flag set: compat/ FIRST on the include path so the
    # engine sees double math + the missing limit macros; qjsweb.c is ours
    # but must share the set (same JSValue ABI as the engine objects) - it
    # gets warnings + the debug sanitizer, the vendored files get -w like
    # uACPI. Both engines always compile, same rule as the browser's two
    # painters.
    QJSCF="-O2 -ffreestanding -fno-stack-protector -fno-stack-check -nostdinc \
           -Iquickjs/compat -Iinclude -Iquickjs -I. -I../unojs \
           -D__DJGPP -U_WIN32 -U_WIN64 -U__MINGW32__ -U__MINGW64__ \
           -Dalloca=__builtin_alloca -DJS_NAN_BOXING=0 -DUNO_COLOR=1 -DUNO_PC64 ${UNO_EXTRA:-} $DBGDEF"
    for f in quickjs libregexp libunicode dtoa qjs_port; do
        pc "$CC" $QJSCF -w -c -o "build/qjs_$f.o" "quickjs/$f.c"; OBJS="$OBJS build/qjs_$f.o"
    done
    pc "$CC" $QJSCF -Wall -Wextra $DBGSAN -c -o "build/qjsweb.o" "qjsweb.c"; OBJS="$OBJS build/qjsweb.o"
    # csslib: the vendored MIT CSS stack + the unoweb bridge - the browser's
    # second cascade (csslib/VENDOR.md; registered at runtime via
    # uwx_libcss_register, default stays the built-in cascade until CS3).
    # The flag set MIRRORS csslib/test/build-host-test.sh - do not let them
    # drift. Vendored files -w like uACPI; the bridge gets warnings + DBGSAN.
    CSSCF="-O2 -ffreestanding -fno-stack-protector -fno-stack-check -nostdinc \
           -I../csslib/compat -Iinclude \
           -I../csslib/css/include -I../csslib/parserutils/include -I../csslib/wapcaplet/include \
           -DWITHOUT_ICONV_FILTER -D_ALIGNED= -DNDEBUG ${UNO_EXTRA:-} $DBGDEF"
    for f in $(find ../csslib/css/src -name '*.c' | sort); do
        o="build/$(echo "${f#../}" | tr / _ | sed 's/\.c$/.o/')"
        pc "$CC" $CSSCF -w -I../csslib/css/src -c "$f" -o "$o"; OBJS="$OBJS $o"
    done
    for f in $(find ../csslib/parserutils/src -name '*.c' | sort); do
        o="build/$(echo "${f#../}" | tr / _ | sed 's/\.c$/.o/')"
        pc "$CC" $CSSCF -w -I../csslib/parserutils/src -c "$f" -o "$o"; OBJS="$OBJS $o"
    done
    pc "$CC" $CSSCF -w -c ../csslib/wapcaplet/src/libwapcaplet.c -o build/csslib_wapcaplet.o
    OBJS="$OBJS build/csslib_wapcaplet.o"
    pc "$CC" $CSSCF -Wall -Wextra -c ../csslib/css_port.c -o build/csslib_port.o
    OBJS="$OBJS build/csslib_port.o"
    pc "$CC" $CSSCF -Wall -Wextra $DBGSAN -c ../csslib/uw_select.c -o build/csslib_uwsel.o
    OBJS="$OBJS build/csslib_uwsel.o"
    pc "$CC" $CSSCF -Wall -Wextra $DBGSAN -c ../csslib/uw_cascade.c -o build/csslib_uwcas.o
    OBJS="$OBJS build/csslib_uwcas.o"
    # the DEBUG core: crash reports + watchdog + stress driver.  uno_debug.c is
    # the interrupt file -> -mgeneral-regs-only (no SSE in the fault paths) and
    # NO sanitizer (its ud2 handler must not itself be instrumented).
    if [ "$UNO_DEBUG" != "0" ]; then
        pc "$CC" $UCF -mgeneral-regs-only -c -o "build/uno_debug.o" "uno_debug.c"
        OBJS="$OBJS build/uno_debug.o"
        # PCH TCO hardware watchdog (unodevices): the guard's hardware backstop
        # for a cli-spin / bus hang no software firing path can catch.  See
        # HWWATCHDOG.md; unoautomate wires it into the guard via a weak stub.
        pc "$CC" $UCF $DBGSAN -c -o "build/uno_hw_wdt.o" "uno_hw_wdt.c"
        OBJS="$OBJS build/uno_hw_wdt.o"
        pc "$CC" $UCF $DBGSAN -c -o "build/pc64_stress.o" "pc64_stress.c"
        OBJS="$OBJS build/pc64_stress.o"
        pc "$CC" $UCF $DBGSAN -c -o "build/pc64_nettest.o" "pc64_nettest.c"
        OBJS="$OBJS build/pc64_nettest.o"
        pc "$CC" $UCF $DBGSAN -c -o "build/pc64_spectest.o" "pc64_spectest.c"
        OBJS="$OBJS build/pc64_spectest.o"
        # NOTE: unoautomate itself (unoauto*, incl. the URC remote channel, its
        # serial + screen backends and the privilege gate) is NO LONGER built
        # here - it ships in PRODUCTION as of 2026-08-03 and lives in the
        # unconditional file list above.  What stays debug-only is the test
        # HARNESS around it: the fuzz driver, the conformance suites, the crash
        # /watchdog core and the live network test.  See unoauto_gate.h.
    fi
    # unomedia AUDIO half (core + WAV/MIDI/MP3/AAC) - linked into the kernel
    # plus um_inflate, which UnoAmp's skin engine needs for ZIP method 8 in a
    # .wsz.  um_inflate is standalone; the IMAGE decoders are NOT pulled in
    # with it (unoamp_skin.c reads skin BMPs itself - see the note there).
    # for the native Music app. The IMAGE half ships inside PHOTOS.UNO below,
    # a second private instance of the library, so the two never collide.
    for u in unomedia um_audio um_wav um_midi um_mp3 um_aac um_inflate; do
        pc "$CC" $UCF -c -o "build/uml_$u.o" "../unomedia/$u.c"; OBJS="$OBJS build/uml_$u.o"
    done
    # unomedia IMAGE half - the browser decodes <img> behind unoweb's uw_images
    # hook.  UNCONDITIONAL since the renderer became a runtime switch: the
    # engine path is now reachable in every kernel, so a kernel without the
    # decoders would offer a renderer that cannot draw a picture.  This was the
    # ONLY real link difference between the two BROWSER_ENGINE settings - the
    # unoweb pipeline, quickjs and csslib were always compiled in - and it costs
    # the standard build about 100 KB.
    # NO um_inflate here: the audio half above always compiles it since
    # UnoAmp's skin engine landed (2026-08-03), and a second copy made
    # every BROWSER_ENGINE=uw build fail at link (found by CS3).
    for u in um_image um_stub um_png um_jpg um_gif um_bmp um_tga um_pnm um_qoi um_ico um_webp um_vp8; do
        pc "$CC" $UCF -c -o "build/umi_$u.o" "../unomedia/$u.c"; OBJS="$OBJS build/umi_$u.o"
    done
    # unoacpi: shared AML/ACPI power stack (verbatim from writers-unlock) + the
    # vendored uACPI interpreter (third-party -> -w).
    for u in acpi_arena ec_handler smbus_handler acpi_power; do
        pc "$CC" $UCF -c -o "build/unoacpi_$u.o" "../unoacpi/$u.c"; OBJS="$OBJS build/unoacpi_$u.o"
    done
    for c in $(find ../unoacpi/uacpi/source -name '*.c' | sort); do
        b=$(basename "$c" .c)
        pc "$CC" $UCF -w -c -o "build/uacpi_$b.o" "$c"; OBJS="$OBJS build/uacpi_$b.o"
    done
    # uno3d: the write-once 3D pipeline + soft rasteriser + Intel scaffold + game
    # (Runner3D is a native game canvas that drives these directly).
    for u in uno3d uno3d_soft uno3d_intel uno3d_game; do
        pc "$CC" $UCF -c -o "build/uui_$u.o" "../uno3d/$u.c"; OBJS="$OBJS build/uui_$u.o"
    done
    for u in unoui unoui_input unoui_anim unoui_wmanim; do
        pc "$CC" $UCF -c -o "build/uui_$u.o" "../unoui/$u.c"; OBJS="$OBJS build/uui_$u.o"
    done
    for t in $(find ../unoui/themes -name '*.c' | sort); do
        b=$(basename "$t" .c)
        pc "$CC" $UCF -c -o "build/uui_$b.o" "$t"; OBJS="$OBJS build/uui_$b.o"
    done
    # NOTE: no app objects here - apps ship as .UNO modules (built below);
    # the kernel image contains no app code at all.
    # BearSSL (TLS for the Network app) - portable C only; skip the CPU-accel /
    # OS-entropy files (portable equivalents build instead).
    BSSL_SKIP=" ghash_pclmul sysrng aes_x86ni aes_x86ni_cbcdec aes_x86ni_cbcenc aes_x86ni_ctr aes_x86ni_ctrcbc chacha20_sse2 "
    BSSLF="-O2 -ffreestanding -fno-stack-protector -fno-stack-check -nostdinc -Iinclude -Ibearssl/inc -Ibearssl/src -DUNO_PC64"
    for c in $(find bearssl/src -name '*.c' | sort); do
        base=$(basename "$c" .c)
        case "$BSSL_SKIP" in *" $base "*) continue;; esac
        pc "$CC" $BSSLF -c -o "build/bssl_$base.o" "$c"; OBJS="$OBJS build/bssl_$base.o"
    done
    pcwait                     # barrier: every kernel object above must exist before linking
    echo "[3/3] linking the unoui image..."
    # -lgcc supplies the compiler-runtime helpers (__udivti3, __floatuntidf)
    # that 128-bit integer arithmetic lowers to.  unojs uses __int128 in its
    # decimal<->double conversion, where 64 bits cannot keep adjacent doubles
    # distinct.  PYRT links it for the same reason (see [3d]); it is a static
    # archive, so only the referenced helpers land in the image.
    LINK="-nostdlib -Wl,--subsystem,10 -e efi_main -Wl,--dynamicbase,--nxcompat"
    if [ "$UNO_DEBUG" != "0" ]; then
        # two-pass symbolization: link once with the empty stub, extract .text
        # RVAs from that image, bake them into build/dbg_syms.c, relink.  The
        # table is const (.rdata, after .text), so .text RVAs are stable across
        # the relink - build.sh asserts this below.
        "$CC" $UCF -c -o "build/dbg_syms_stub.o" "tools/dbg_syms_stub.c"
        "$CC" $LINK -o build/BOOTX64.EFI $OBJS build/dbg_syms_stub.o -lgcc
        NM="${NM:-x86_64-w64-mingw32-nm}"
        "$NM" -n build/BOOTX64.EFI | awk '$2=="T"||$2=="t"{print $1}' | sort > build/syms_pass1.txt
        "$PY" tools/mksyms.py build/BOOTX64.EFI build/dbg_syms.c build/SYMBOLS.TXT "$NM"
        "$CC" $UCF -c -o "build/dbg_syms.o" "build/dbg_syms.c"
        "$CC" $LINK -o build/BOOTX64.EFI $OBJS build/dbg_syms.o -lgcc
        # the BIOS link below needs the same extra object: uno_debug.c
        # references the baked symbol table unconditionally, so a link without
        # it fails on four undefined refs
        LINK_EXTRA="build/dbg_syms.o"
        # assert .text RVAs did not move (else the baked symbols are wrong)
        "$NM" -n build/BOOTX64.EFI | awk '$2=="T"||$2=="t"{print $1}' | sort > build/syms_pass2.txt
        if ! cmp -s build/syms_pass1.txt build/syms_pass2.txt; then
            echo "WARN: .text addresses shifted between link passes - baked symbols"
            echo "      may be off by a section delta; SYMBOLS.TXT + crashsym.py"
            echo "      (regenerated from the FINAL image) remain authoritative."
            "$PY" tools/mksyms.py build/BOOTX64.EFI build/dbg_syms.c build/SYMBOLS.TXT "$NM"
        fi
        mkdir -p build/esp/DOCS; cp build/SYMBOLS.TXT build/esp/DOCS/SYMBOLS.TXT
    else
        "$CC" $LINK -o build/BOOTX64.EFI $OBJS -lgcc
    fi
    mkdir -p build/esp/EFI/BOOT; cp build/BOOTX64.EFI build/esp/EFI/BOOT/BOOTX64.EFI

    # sample docs on the ESP (a real FAT volume) for the browser to open
    printf '# Hello from the disk\n\nThis file lives on the **FAT ESP**, read via the\nEFI Simple File System - the browser opened it from a *local disk*, not the\nRAM disk.\n\n- FAT12/16/32 supported (firmware driver)\n- read-only for now\n\n> UnoDOS pc64\n' > build/esp/HELLO.MD
    printf '<h1>Disk HTML</h1><p>An <b>HTML</b> file loaded from the FAT volume by the pc64 browser.</p><ul><li>local disk</li><li>FAT32</li></ul>' > build/esp/PAGE.HTML
    # bundle the open TrueType fonts on the ESP (the TTF engine loads them at runtime)
    # demo media for the Music app. Generated by tools/mkdemo_media.py and
    # committed under media/ - public domain (CC0), no attribution required;
    # see media/README.TXT for why that's unambiguous.
    # rm first: without it, files dropped from media/ linger in build/esp and
    # end up on a flashed stick long after they stopped existing in the repo
    if [ -d media ]; then rm -rf build/esp/MEDIA; mkdir -p build/esp/MEDIA; cp media/* build/esp/MEDIA/; fi
    cp fonts/Sans.ttf       build/esp/SANS.TTF
    cp fonts/Mono.ttf       build/esp/MONO.TTF
    cp fonts/Ubuntu.ttf     build/esp/UBUNTU.TTF
    cp fonts/ChiKareGo2.ttf build/esp/CHICAGO.TTF   # the default (Chicago-style) UI face
    # licensing notices ship on EVERY image - the Apache-2.0 AAC tables and
    # the MIT components require their notices to travel with distributions
    # (System > View licenses opens this in the Browser). The assert keeps
    # the shipped notices in lockstep with the third-party manifest: every
    # component in THIRD-PARTY.md must appear in LICENSES.MD or the build
    # stops here (same spirit as the module-import and no-app-code asserts).
    "$PY" ../tools/check_licenses.py
    mkdir -p build/esp/DOCS
    cp docs_esp/LICENSES.MD build/esp/DOCS/

    # ---- Intel WiFi firmware ----------------------------------------------
    # The iwlwifi driver loads FIRMWARE\IWL*.UCO from the ESP, so an image
    # without them cannot bring WiFi up at all. The blobs are NOT in the repo
    # (Intel's licence forbids redistribution; fw-blobs/ is gitignored), so
    # this bundles whatever the developer staged there - via tools/fetch-fw.sh
    # or uno-wifi-fw.py - and a clean clone simply has none.
    #
    # 2026-07-29 (user ruling): PRODUCTION images get them too. Restricting
    # this to debug builds meant every production stick booted with a dead
    # radio, which is not a shippable OS - and the blobs on YOUR stick are
    # your own copy, not redistribution. The licence constraint applies when
    # an image is PUBLISHED: do not upload a built image containing them.
    # Set UNO_NOFW=1 to build a firmware-free image for that case.
    if [ "${UNO_NOFW:-0}" = "0" ] && ls fw-blobs/*.UCO >/dev/null 2>&1; then
        echo "[fw] bundling Intel WiFi firmware from fw-blobs/ (UNO_NOFW=1 to omit)"
        mkdir -p build/esp/FIRMWARE
        cp fw-blobs/*.UCO build/esp/FIRMWARE/ 2>/dev/null || true
        cp fw-blobs/*.PNV build/esp/FIRMWARE/ 2>/dev/null || true
        # (no starter WIFI.CFG here - see the dbg staging step: a shipped
        # placeholder shadows the flasher-staged wifi.txt credentials)
    elif [ "${UNO_NOFW:-0}" != "0" ]; then
        # UNO_NOFW has to REMOVE, not merely skip. build/esp is populated
        # incrementally and is not wiped between builds, so a default build
        # (which bundles the blobs) leaves build/esp/FIRMWARE/ behind, and a
        # later UNO_NOFW=1 build silently inherits it. Every image cut from
        # that tree - the hybrid .img and the ISO alike - then carries Intel's
        # firmware into a public release, which is the exact thing the flag
        # exists to prevent. Skipping the copy is not enough; delete it.
        if [ -d build/esp/FIRMWARE ]; then
            echo "[fw] UNO_NOFW=1: removing firmware staged by an earlier build"
            rm -rf build/esp/FIRMWARE
        fi
        echo "[fw] firmware-free image (safe to publish)"
    fi

    # ---- .UNO app modules: every app is loaded from storage at runtime -----
    # (apps/<name>.c -> object -> import thunks -> linked DLL -> APPS/<N>.UNO)
    echo "[3b] building the .UNO app modules..."
    NM="${NM:-x86_64-w64-mingw32-nm}"
    mkdir -p build/apps build/esp/APPS
    grep -oE 'KX\([A-Za-z_0-9]+\)' pc64_modload.c | sed 's/KX(//;s/)//' \
        | sort -u > build/apps/kexports.txt
    # the compiler embeds this legal-import list so an app that calls an
    # unexported kernel function is a compile-time error, not a load failure
    { echo "/* generated by build.sh from pc64_modload.c - do not edit */";
      echo "static const char *const kKexports[] = {";
      sed 's/.*/    "&",/' build/apps/kexports.txt;
      echo "};";
      echo "#define KEXPORTS_N ((int)(sizeof kKexports / sizeof kKexports[0]))";
    } > build/apps/ucc_kexports.h
    for app in dostris pacman outlast music tracker paint network; do
        "$CC" $UCF -DUNO_APP_SYM=uno_app_main_$app -c -o "build/apps/$app.o" "apps/$app.c"
        "$NM" -u "build/apps/$app.o" | awk '{print $2}' | sort -u > "build/apps/$app.syms"
        # decoupling assert: every import must be in the kernel export table
        while read -r s; do
            grep -qx "$s" build/apps/kexports.txt || {
                echo "FAIL: $app imports '$s' which pc64_modload.c does not export"; exit 1; }
        done < "build/apps/$app.syms"
        "$PY" tools/mkuno.py thunks "build/apps/$app.syms" "build/apps/${app}_thunks.s"
        "$CC" -c -o "build/apps/${app}_thunks.o" "build/apps/${app}_thunks.s"
        "$CC" -shared -nostdlib -e uno_app_main_$app -Wl,--exclude-all-symbols \
            -o "build/apps/$app.dll" "build/apps/$app.o" "build/apps/${app}_thunks.o"
        UP=$(echo "$app" | tr '[:lower:]' '[:upper:]')
        "$PY" tools/mkuno.py convert "build/apps/$app.dll" "build/esp/APPS/$UP.UNO"
    done
    # ---- STUDIO.UNO: the IDE, a unoui-CLASS module (flags bit 0) -----------
    # A bigger module: the editor + the on-device UnoC compiler (ucc).  Same
    # pipeline as the apps above, but multi-object and header-flag 0x1, and it
    # imports the wider unoui/fs/shell surface (all still in kexports.txt).
    if [ "${UNO_STUDIO:-1}" != "0" ]; then
        echo "[3c] building STUDIO.UNO (the IDE)..."
        SOBJ=""
        for s in studio studio_hl studio_py studio_ai studio_json ucc ucc_x64; do
            pc "$CC" $UCF -DUNO_APP_SYM=uno_app_main \
                  -DUCC_KEXPORTS_H='"build/apps/ucc_kexports.h"' \
                  -c -o "build/apps/$s.o" "apps/$s.c"
            SOBJ="$SOBJ build/apps/$s.o"
        done
        pcwait                 # barrier: all STUDIO objects before the nm/link
        # imports = symbols undefined across ALL objects minus those any object
        # defines (studio.o references studio_ai.o etc; those resolve at link)
        "$NM" $SOBJ | awk '$1=="U"&&$2!=""{u[$2]=1} \
            $1!="U"&&NF>=3{d[$3]=1} \
            END{for(s in u) if(!(s in d)) print s}' \
            | sort -u > build/apps/studio.syms
        while read -r s; do
            [ -z "$s" ] && continue
            grep -qx "$s" build/apps/kexports.txt || {
                echo "FAIL: STUDIO imports '$s' which pc64_modload.c does not export"; exit 1; }
        done < build/apps/studio.syms
        "$PY" tools/mkuno.py thunks "build/apps/studio.syms" "build/apps/studio_thunks.s"
        "$CC" -c -o "build/apps/studio_thunks.o" "build/apps/studio_thunks.s"
        "$CC" -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols \
            -o "build/apps/studio.dll" $SOBJ "build/apps/studio_thunks.o"
        "$PY" tools/mkuno.py convert "build/apps/studio.dll" "build/esp/APPS/STUDIO.UNO" 1
        # the SDK + developer docs ride on the ESP for Studio to open
        mkdir -p build/esp/SDK build/esp/DOCS
        cp sdk/UNO.H sdk/SAMPLE.C sdk/DOSTRIS.C sdk/TIMER.C sdk/LIFE.C build/esp/SDK/ 2>/dev/null || true
        # Python SDK: sources (.py/.PY) + type stubs (.pyi).  Copy each match
        # individually so a missing glob never drops a valid sibling, and both
        # letter cases are picked up (the on-disk names are upper-case).
        for f in sdk/*.py sdk/*.PY sdk/*.pyi sdk/*.pyi; do
            [ -f "$f" ] && cp "$f" build/esp/SDK/
        done
        [ -d docs_esp ] && cp docs_esp/*.MD build/esp/DOCS/ 2>/dev/null || true
    fi

    # ---- UNOCODE.UNO: the VS Code-class editor, a unoui-CLASS module -------
    # Fourteen objects under unocode/ - the workbench, the editor, the JSONC
    # parser, the regex engine the grammars run on, and the extension host
    # over unojs.  Same pipeline as STUDIO.UNO; the only build difference is
    # -Iunocode (its private header) and -I../unojs (the embedding API, whose
    # entry points are exported from pc64_modload.c's kExports).
    if [ "${UNO_UNOCODE:-1}" != "0" ]; then
        echo "[3c2] building UNOCODE.UNO (the editor)..."
        COBJ=""
        for s in uc_main uc_util uc_json uc_rx uc_theme uc_cfg uc_lang uc_doc \
                 uc_edit uc_view uc_cmd uc_term uc_ext uc_api; do
            # -mno-stack-arg-probe: mingw calls ___chkstk_ms for any frame over
            # 4 KB, and a loadable module has nothing to link that against - it
            # surfaces as an unresolvable import at the kExports check, three
            # files away from its cause.  The buffers that would need it here
            # are static or heap for exactly that reason; the flag keeps a
            # future one from becoming a link failure.
            pc "$CC" $UCF -mno-stack-arg-probe -DUNO_APP_SYM=uno_app_main \
                  -Iunocode -I../unojs -c -o "build/apps/$s.o" "unocode/$s.c"
            COBJ="$COBJ build/apps/$s.o"
        done
        # This machine's answer to the editor's network seam (uc_net.h).  It is
        # in unocode/plat/ rather than beside the uc_*.c files because the
        # DESKTOP build globs core/uc_*.c and would compile a pc64
        # implementation on a host that has none of these symbols; #ifdef does
        # not help, because that build defines UNO_PC64 too.
        #
        # -Ibearssl/inc is here and nowhere else in this loop: it takes the
        # BR_ERR_X509_* certificate codes from the real header rather than
        # copying their values, which got all three wrong when it was tried,
        # two of them in a way that swapped two error messages.  Header only -
        # no link surface, since the crypto is already in the kernel.
        pc "$CC" $UCF -mno-stack-arg-probe -DUNO_APP_SYM=uno_app_main \
              -Iunocode -I../unojs -Ibearssl/inc \
              -c -o "build/apps/uc_net_pc64.o" "unocode/plat/uc_net_pc64.c"
        COBJ="$COBJ build/apps/uc_net_pc64.o"
        pcwait
        "$NM" $COBJ | awk '$1=="U"&&$2!=""{u[$2]=1} \
            $1!="U"&&NF>=3{d[$3]=1} \
            END{for(s in u) if(!(s in d)) print s}' \
            | sort -u > build/apps/unocode.syms
        while read -r s; do
            [ -z "$s" ] && continue
            grep -qx "$s" build/apps/kexports.txt || {
                echo "FAIL: UNOCODE imports '$s' which pc64_modload.c does not export"; exit 1; }
        done < build/apps/unocode.syms
        "$PY" tools/mkuno.py thunks "build/apps/unocode.syms" "build/apps/unocode_thunks.s"
        "$CC" -c -o "build/apps/unocode_thunks.o" "build/apps/unocode_thunks.s"
        "$CC" -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols \
            -o "build/apps/unocode.dll" $COBJ "build/apps/unocode_thunks.o"
        "$PY" tools/mkuno.py convert "build/apps/unocode.dll" "build/esp/APPS/UNOCODE.UNO" 1
        # the sample extensions ride on the ESP so a fresh stick has something
        # in the Extensions view to look at and read
        # Names go on the ESP UPPER-CASED, directories included: the volume is
        # FAT with 8.3 names, and a manifest that says "THEMES/NORD.JSN" has
        # to find it whatever case the host tree used.  Copying the tree
        # verbatim shipped `themes/` once and the theme silently did not load.
        if [ -d unocode/ext ]; then
            for e in unocode/ext/*; do
                [ -d "$e" ] || continue
                E=$(basename "$e" | tr '[:lower:]' '[:upper:]')
                mkdir -p "build/esp/EXT/$E"
                ( cd "$e" && find . -mindepth 1 -type d -printf '%P
' ) |                 while read -r d; do
                    mkdir -p "build/esp/EXT/$E/$(echo "$d" | tr '[:lower:]' '[:upper:]')"
                done
                ( cd "$e" && find . -type f -printf '%P
' ) | while read -r f; do
                    cp "$e/$f" "build/esp/EXT/$E/$(echo "$f" | tr '[:lower:]' '[:upper:]')"
                done
            done
        fi
    fi

    # ---- PYRT.UNO: the Python runtime, a vendored-MicroPython module -------
    # (optional, header-flag 0x2 = UNO_MODF_PY).  Built like STUDIO.UNO but
    # multi-hundred-object: the whole py/ core + the port + the `uno` bindings.
    # Codegen (mkupy.py) makes MicroPython's qstr/module/version headers first.
    if [ "${UNO_PYRT:-1}" != "0" ]; then
        echo "[3d] building PYRT.UNO (the Python runtime)..."
        PB=build/pyrt
        mkdir -p "$PB"
        PYF="$UCF -w -Iupy -Iupy_port -I$PB"
        PSRC="$(ls upy/py/*.c) upy/shared/runtime/gchelper_native.c \
              upy_port/pc64_upy_port.c upy_port/mod_uno.c upy_port/mod_unoauto.c \
              upy_port/mod_unoscript.c \
              upy_port/pc64_upy_stubs.c apps/pyrt.c"
        "$PY" upy_port/mkupy.py --top upy --port upy_port --build "$PB" \
              --cpp "$CC -E" --cflags "$PYF" -- $PSRC
        POBJ=""
        for s in $PSRC; do
            o="$PB/$(echo "$s" | tr '/.' '__').o"
            pc "$CC" $PYF -DUNO_APP_SYM=uno_app_main -c -o "$o" "$s"
            POBJ="$POBJ $o"
        done
        pcwait                 # barrier: all PYRT objects before the nm/link below
        # imports = undefined-across-all minus defined-by-any, EXCLUDING the
        # compiler/libgcc runtime helpers (__*), which -lgcc resolves at link
        # (soft float/int conversions), not the kernel export table.
        "$NM" $POBJ | awk '$1=="U"&&$2!=""&&$2!~/^__/{u[$2]=1} \
            $1!="U"&&NF>=3{d[$3]=1} \
            END{for(s in u) if(!(s in d)) print s}' \
            | sort -u > "$PB/pyrt.syms"
        while read -r s; do
            [ -z "$s" ] && continue
            grep -qx "$s" build/apps/kexports.txt || {
                echo "FAIL: PYRT imports '$s' which pc64_modload.c does not export"; exit 1; }
        done < "$PB/pyrt.syms"
        "$PY" tools/mkuno.py thunks "$PB/pyrt.syms" "$PB/pyrt_thunks.s"
        "$CC" -c -o "$PB/pyrt_thunks.o" "$PB/pyrt_thunks.s"
        # -lgcc embeds libgcc's compiler-runtime helpers (soft float/int
        # conversions MicroPython's float code needs, e.g. __extendhfsf2)
        # statically - no PE imports, so the flattened .UNO stays self-contained.
        "$CC" -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols \
            -o "$PB/pyrt.dll" $POBJ "$PB/pyrt_thunks.o" -lgcc
        "$PY" tools/mkuno.py convert "$PB/pyrt.dll" "build/esp/APPS/PYRT.UNO" 2
        ls -l build/esp/APPS/PYRT.UNO
    fi

    # ---- SAMPLE.UNO: the reference LOADABLE DRIVER (unodevices phase 4) ----
    # Proves the whole loadable-driver path end to end: a module in \DRIVERS    # flagged UNO_MODF_DRV (8), receiving the versioned services struct and
    # binding a device the built-ins all declined. It claims the SMBus function
    # because nothing needs it - a sample that bound something load-bearing
    # would be a sample that can break the machine.
    #
    # Imports NOTHING from the kernel (everything it can do arrives in the
    # services struct), so unlike the apps above it needs no thunks and no
    # kexports check.
    if [ "${UNO_SAMPLEDRV:-1}" != "0" ]; then
        echo "[3c2] building SAMPLE.UNO (the reference loadable driver)..."
        mkdir -p build/drivers build/esp/DRIVERS
        "$CC" $UCF -c -o "build/drivers/sample.o" "drivers/sample.c"
        "$CC" -shared -nostdlib -e uno_drv_main -Wl,--exclude-all-symbols             -o "build/drivers/sample.dll" "build/drivers/sample.o"
        "$PY" tools/mkuno.py convert "build/drivers/sample.dll"             "build/esp/DRIVERS/SAMPLE.UNO" 8
    fi

    # ---- PHOTOS.UNO: the image viewer, a unoui-CLASS module ----------------
    # The app plus the whole unomedia image-decoding library (top-level
    # unomedia/ - PNG incl. its own inflate, baseline JPEG, GIF, BMP, TGA,
    # PNM, QOI, ICO, all from scratch) statically linked into the module:
    # the kernel gains no decoder code, just the fb_blit export.
    # LOGVIEW.UNO - unolog's viewer (pc64/UNOLOG.md).  A unoui-CLASS module
    # like Photos: its own desktop slot and window title, which a PYAPP cannot
    # have (one shared EX_PYAPP slot, and pyrt names the window after the FILE).
    echo "[3d] building LOGVIEW.UNO (the system log viewer)..."
    pc "$CC" $UCF -DUNO_APP_SYM=uno_app_main -c -o build/apps/logview.o apps/logview.c
    pcwait
    "$NM" -u build/apps/logview.o | awk '{print $2}' | sort -u > build/apps/logview.syms
    while read -r s; do
        [ -z "$s" ] && continue
        grep -qx "$s" build/apps/kexports.txt || {
            echo "FAIL: LOGVIEW imports '$s' which pc64_modload.c does not export"; exit 1; }
    done < build/apps/logview.syms
    "$PY" tools/mkuno.py thunks build/apps/logview.syms build/apps/logview_thunks.s
    "$CC" -c -o build/apps/logview_thunks.o build/apps/logview_thunks.s
    "$CC" -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols -o build/apps/logview.dll build/apps/logview.o build/apps/logview_thunks.o
    "$PY" tools/mkuno.py convert build/apps/logview.dll build/esp/APPS/LOGVIEW.UNO 1
    # VMGR.UNO - the unovirt appliance manager (pc64/UNOVIRT.md).  Same
    # unoui-CLASS shape as LOGVIEW: it owns a real window because it has a
    # console somebody types into, which a shared PYAPP slot cannot give it.
    echo "[3d] building VMGR.UNO (the appliance manager)..."
    pc "$CC" $UCF -DUNO_APP_SYM=uno_app_main -c -o build/apps/vmgr.o apps/vmgr.c
    pcwait
    "$NM" -u build/apps/vmgr.o | awk '{print $2}' | sort -u > build/apps/vmgr.syms
    while read -r s; do
        [ -z "$s" ] && continue
        grep -qx "$s" build/apps/kexports.txt || {
            echo "FAIL: VMGR imports '$s' which pc64_modload.c does not export"; exit 1; }
    done < build/apps/vmgr.syms
    "$PY" tools/mkuno.py thunks build/apps/vmgr.syms build/apps/vmgr_thunks.s
    "$CC" -c -o build/apps/vmgr_thunks.o build/apps/vmgr_thunks.s
    "$CC" -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols -o build/apps/vmgr.dll build/apps/vmgr.o build/apps/vmgr_thunks.o
    "$PY" tools/mkuno.py convert build/apps/vmgr.dll build/esp/APPS/VMGR.UNO 1
    # VMGR ships its OWN artwork rather than naming one of the kernel's emblems
    # - it is the app that arrived without a compiled-in slot, so it is the
    # right one to prove that an app from disk can bring its own icon too
    # (`icon: file:VMGR.QOI` in its descriptor; decoded by pc64_qoi.c).
    "$PY" tools/mkicon.py --demo build/esp/APPS/VMGR.QOI
    if [ "${UNO_PHOTOS:-1}" != "0" ]; then
        echo "[3d] building PHOTOS.UNO (the image viewer + unomedia)..."
        POBJ="build/apps/photos.o"
        pc "$CC" $UCF -DUNO_APP_SYM=uno_app_main \
              -c -o "build/apps/photos.o" "apps/photos.c"
        # core + the IMAGE half only (the AUDIO half links into the kernel)
        for b in unomedia um_image um_inflate um_png um_jpg um_gif um_bmp \
                 um_ico um_tga um_pnm um_qoi um_webp um_vp8 um_stub; do
            pc "$CC" $UCF -c -o "build/apps/um_$b.o" "../unomedia/$b.c"
            POBJ="$POBJ build/apps/um_$b.o"
        done
        pcwait                 # barrier: all PHOTOS objects before the nm/link
        "$NM" $POBJ | awk '$1=="U"&&$2!=""{u[$2]=1} \
            $1!="U"&&NF>=3{d[$3]=1} \
            END{for(s in u) if(!(s in d)) print s}' \
            | sort -u > build/apps/photos.syms
        while read -r s; do
            [ -z "$s" ] && continue
            grep -qx "$s" build/apps/kexports.txt || {
                echo "FAIL: PHOTOS imports '$s' which pc64_modload.c does not export"; exit 1; }
        done < build/apps/photos.syms
        "$PY" tools/mkuno.py thunks "build/apps/photos.syms" "build/apps/photos_thunks.s"
        "$CC" -c -o "build/apps/photos_thunks.o" "build/apps/photos_thunks.s"
        "$CC" -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols \
            -o "build/apps/photos.dll" $POBJ "build/apps/photos_thunks.o"
        "$PY" tools/mkuno.py convert "build/apps/photos.dll" "build/esp/APPS/PHOTOS.UNO" 1
        # demo pictures ride on the ESP (committed under pictures/, all
        # procedurally generated by tools/mkdemo_pics.py - CC0, see README)
        if [ -d pictures ]; then
            rm -rf build/esp/PICTURES; mkdir -p build/esp/PICTURES
            for f in pictures/*; do
                case "$f" in *README*) ;; *) cp "$f" build/esp/PICTURES/;; esac
            done
        fi
    fi

    # ---- UOWORD.UNO: the word processor, a unoui-CLASS module --------------
    # UnoWord plus the WHOLE uoffice chrome lane (command bars, dialogs,
    # ruler/status/Assistant, the file dialog, the document model and page
    # layout) and unodoc's Word half, statically linked into the module -
    # the PHOTOS pattern.  The kernel gains no document code at all.
    if [ "${UNO_UOWORD:-1}" != "0" ]; then
        echo "[3d2] building UOWORD.UNO (the word processor)..."
        WOBJ="build/apps/uoword.o"
        # -mno-stack-arg-probe: unodoc's .doc reader keeps the 4 KB FIB on
        # the stack, and mingw emits ___chkstk_ms for any frame past 4 KB.
        # That probe walks Windows' guard page - a mechanism this OS does
        # not have and cannot provide - so it is a host artifact rather
        # than a safety net, and a freestanding module must not import it.
        UWCF="$UCF -mno-stack-arg-probe -I../unoui -Iuoffice -I../unodoc"
        pc "$CC" $UWCF -DUNO_APP_SYM=uno_app_main               -c -o "build/apps/uoword.o" "apps/uoword.c"
        for b in uochrome uoicons uodlg uobars uofile uow_doc uow_layout; do
            pc "$CC" $UWCF -c -o "build/apps/uo_$b.o" "uoffice/$b.c"
            WOBJ="$WOBJ build/apps/uo_$b.o"
        done
        # APPENDED: the OOXML half.  ud_zip/ud_xml are the container and
        # the parser both formats share; ud_docx/ud_docxw are .docx in and
        # out; ud_ooxz is the package writer.  um_inflate is linked HERE
        # rather than imported from the kernel deliberately - a module that
        # called the kernel's um_set_alloc would repoint the allocator the
        # browser and UnoAmp are using.
        for b in unodoc ud_cfb ud_doc ud_docw \
                 ud_zip ud_xml ud_docx ud_ooxz ud_docxw; do
            pc "$CC" $UWCF -c -o "build/apps/ud_$b.o" "../unodoc/$b.c"
            WOBJ="$WOBJ build/apps/ud_$b.o"
        done
        for b in unomedia um_inflate; do
            pc "$CC" $UWCF -I../unomedia -c -o "build/apps/uoword_um_$b.o" "../unomedia/$b.c"
            WOBJ="$WOBJ build/apps/uoword_um_$b.o"
        done
        pcwait                 # barrier: all UOWORD objects before the nm/link
        "$NM" $WOBJ | awk '$1=="U"&&$2!=""{u[$2]=1}             $1!="U"&&NF>=3{d[$3]=1}             END{for(s in u) if(!(s in d)) print s}'             | sort -u > build/apps/uoword.syms
        while read -r s; do
            [ -z "$s" ] && continue
            grep -qx "$s" build/apps/kexports.txt || {
                echo "FAIL: UOWORD imports '$s' which pc64_modload.c does not export"; exit 1; }
        done < build/apps/uoword.syms
        "$PY" tools/mkuno.py thunks "build/apps/uoword.syms" "build/apps/uoword_thunks.s"
        "$CC" -c -o "build/apps/uoword_thunks.o" "build/apps/uoword_thunks.s"
        "$CC" -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols             -o "build/apps/uoword.dll" $WOBJ "build/apps/uoword_thunks.o"
        "$PY" tools/mkuno.py convert "build/apps/uoword.dll" "build/esp/APPS/UOWORD.UNO" 1
    fi

    # ---- UOCALC.UNO: the spreadsheet, a unoui-CLASS module ----------------
    # UnoCalc plus the uoffice chrome lane and the workbook/calculator
    # (uxl_*), statically linked into the module - the PHOTOS pattern.  It
    # carries unodoc's EXCEL half (ud_xls/ud_ptg) where UnoWord carries the
    # Word half, so neither module pays for the other's format.
    if [ "${UNO_UOCALC:-1}" != "0" ]; then
        echo "[3d3] building UOCALC.UNO (the spreadsheet)..."
        WOBJ="build/apps/uocalc.o"
        # -mno-stack-arg-probe: unodoc's .doc reader keeps the 4 KB FIB on
        # the stack, and mingw emits ___chkstk_ms for any frame past 4 KB.
        # That probe walks Windows' guard page - a mechanism this OS does
        # not have and cannot provide - so it is a host artifact rather
        # than a safety net, and a freestanding module must not import it.
        UWCF="$UCF -mno-stack-arg-probe -I../unoui -Iuoffice -I../unodoc"
        pc "$CC" $UWCF -DUNO_APP_SYM=uno_app_main               -c -o "build/apps/uocalc.o" "apps/uocalc.c"
        for b in uochrome uoicons uodlg uobars uofile uxl_sheet uxl_calc uxl_numfmt; do
            pc "$CC" $UWCF -c -o "build/apps/uo_$b.o" "uoffice/$b.c"
            WOBJ="$WOBJ build/apps/uo_$b.o"
        done
        # APPENDED: the OOXML half - see the UOWORD block for why
        # um_inflate is statically linked into each module.
        for b in unodoc ud_cfb ud_xls ud_xlsw ud_ptg ud_ptgc \
                 ud_zip ud_xml ud_xlsx ud_ooxz ud_xlsxw; do
            pc "$CC" $UWCF -c -o "build/apps/ud_$b.o" "../unodoc/$b.c"
            WOBJ="$WOBJ build/apps/ud_$b.o"
        done
        for b in unomedia um_inflate; do
            pc "$CC" $UWCF -I../unomedia -c -o "build/apps/uocalc_um_$b.o" "../unomedia/$b.c"
            WOBJ="$WOBJ build/apps/uocalc_um_$b.o"
        done
        pcwait                 # barrier: all UOCALC objects before the nm/link
        "$NM" $WOBJ | awk '$1=="U"&&$2!=""{u[$2]=1}             $1!="U"&&NF>=3{d[$3]=1}             END{for(s in u) if(!(s in d)) print s}'             | sort -u > build/apps/uocalc.syms
        while read -r s; do
            [ -z "$s" ] && continue
            grep -qx "$s" build/apps/kexports.txt || {
                echo "FAIL: UOCALC imports '$s' which pc64_modload.c does not export"; exit 1; }
        done < build/apps/uocalc.syms
        "$PY" tools/mkuno.py thunks "build/apps/uocalc.syms" "build/apps/uocalc_thunks.s"
        "$CC" -c -o "build/apps/uocalc_thunks.o" "build/apps/uocalc_thunks.s"
        "$CC" -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols             -o "build/apps/uocalc.dll" $WOBJ "build/apps/uocalc_thunks.o"
        "$PY" tools/mkuno.py convert "build/apps/uocalc.dll" "build/esp/APPS/UOCALC.UNO" 1
    fi

    # ---- UOSHOW.UNO: the presentation app, a unoui-CLASS module ----------------
    # UnoShow plus the uoffice chrome lane and the presentation model,
    # geometry and renderer (uos_*), statically linked into the module - the
    # PHOTOS pattern.  It carries unodoc's POWERPOINT half, so none of the
    # three apps pays for another's format.
    if [ "${UNO_UOSHOW:-1}" != "0" ]; then
        echo "[3d4] building UOSHOW.UNO (the presentation app)..."
        WOBJ="build/apps/uoshow.o"
        # -mno-stack-arg-probe: unodoc's .doc reader keeps the 4 KB FIB on
        # the stack, and mingw emits ___chkstk_ms for any frame past 4 KB.
        # That probe walks Windows' guard page - a mechanism this OS does
        # not have and cannot provide - so it is a host artifact rather
        # than a safety net, and a freestanding module must not import it.
        UWCF="$UCF -mno-stack-arg-probe -I../unoui -Iuoffice -I../unodoc"
        pc "$CC" $UWCF -DUNO_APP_SYM=uno_app_main               -c -o "build/apps/uoshow.o" "apps/uoshow.c"
        for b in uochrome uoicons uodlg uobars uofile uos_geom uos_model uos_render; do
            pc "$CC" $UWCF -c -o "build/apps/uo_$b.o" "uoffice/$b.c"
            WOBJ="$WOBJ build/apps/uo_$b.o"
        done
        # APPENDED: the OOXML half - see the UOWORD block for why
        # um_inflate is statically linked into each module.
        for b in unodoc ud_cfb ud_ppt ud_pptw ud_escher \
                 ud_zip ud_xml ud_pptx ud_ooxz ud_pptxw; do
            pc "$CC" $UWCF -c -o "build/apps/ud_$b.o" "../unodoc/$b.c"
            WOBJ="$WOBJ build/apps/ud_$b.o"
        done
        for b in unomedia um_inflate; do
            pc "$CC" $UWCF -I../unomedia -c -o "build/apps/uoshow_um_$b.o" "../unomedia/$b.c"
            WOBJ="$WOBJ build/apps/uoshow_um_$b.o"
        done
        pcwait                 # barrier: all UOSHOW objects before the nm/link
        "$NM" $WOBJ | awk '$1=="U"&&$2!=""{u[$2]=1}             $1!="U"&&NF>=3{d[$3]=1}             END{for(s in u) if(!(s in d)) print s}'             | sort -u > build/apps/uoshow.syms
        while read -r s; do
            [ -z "$s" ] && continue
            grep -qx "$s" build/apps/kexports.txt || {
                echo "FAIL: UOSHOW imports '$s' which pc64_modload.c does not export"; exit 1; }
        done < build/apps/uoshow.syms
        "$PY" tools/mkuno.py thunks "build/apps/uoshow.syms" "build/apps/uoshow_thunks.s"
        "$CC" -c -o "build/apps/uoshow_thunks.o" "build/apps/uoshow_thunks.s"
        "$CC" -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols             -o "build/apps/uoshow.dll" $WOBJ "build/apps/uoshow_thunks.o"
        "$PY" tools/mkuno.py convert "build/apps/uoshow.dll" "build/esp/APPS/UOSHOW.UNO" 1
    fi

    # ---- DUUM.UNO: the Python Doom engine (a PYAPP; needs PYRT + a WAD) -----
    # Duum is a Python app, so it packages like any .py (source -> PYAPP
    # container).  The WAD is developer-supplied game data, never committed
    # (wads/ is gitignored, like the Wi-Fi fw-blobs): staged onto the ESP only
    # when present.  Freedoom (BSD) is the shippable free default.
    if [ "${UNO_PYRT:-1}" != "0" ] && [ -f apps/DUUM.PY ]; then
        echo "[3e] packaging DUUM.UNO (the Python Doom engine)..."
        # DUUM.DESC is what earns Duum a desktop icon and a Start-menu row.
        # It is a file BESIDE the source rather than a comment inside it
        # because apps/DUUM.PY is generated upstream and vendored verbatim -
        # anything written into it is lost at the next sync_duum.py.
        "$PY" tools/mkuno.py pyapp apps/DUUM.PY build/esp/APPS/DUUM.UNO \
                             apps/DUUM.DESC
        "$PY" tools/mkicon.py --duum build/esp/APPS/DUUM.QOI
        mkdir -p build/esp/SDK; cp apps/DUUM.PY build/esp/SDK/ 2>/dev/null || true
        if   [ -f wads/DOOM1.WAD ];     then WADSRC=wads/DOOM1.WAD
        elif [ -f wads/freedoom1.wad ]; then WADSRC=wads/freedoom1.wad
        else WADSRC=""; fi
        if [ -n "$WADSRC" ]; then
            cp "$WADSRC" build/esp/DOOM1.WAD
            echo "[duum] staged $WADSRC -> ESP DOOM1.WAD ($(wc -c < "$WADSRC") bytes)"
        else
            echo "[duum] no WAD in wads/ (run tools/fetch-wad.sh) - Duum will say so"
        fi
    fi

    # decoupling assert: no app code linked into the kernel image
    if "$NM" build/BOOTX64.EFI 2>/dev/null | grep -q "uno_app_main_"; then
        echo "FAIL: uno_app_main_* found in the kernel image - apps must be .UNO only"
        exit 1
    fi

    # ---- DEBUG build staging: CRASH dir, fuzz corpus, a default DEBUG.CFG --
    if [ "$UNO_DEBUG" != "0" ]; then
        echo "[dbg] staging CRASH\\, fuzz corpus, DEBUG.CFG onto the ESP..."
        # PURGE dev-run telemetry first: QEMU regression runs (vvfat fat:rw)
        # write their BOOTLOG/PF/NETLOG *back into build/esp*, and one shipped
        # image carried a QEMU boots.txt (with vvfat cluster garbage) onto the
        # Yoga's stick - indistinguishable from that machine's own results.
        # A shipped image must carry an EMPTY CRASH dir, exactly like a fresh
        # flash. Same for the root-level artifacts those runs leave.
        rm -rf build/esp/CRASH
        rm -f  build/esp/BOOTENV.TXT
        mkdir -p build/esp/CRASH
        printf 'Per-machine telemetry lands in subfolders here (one per machine,\r\nnamed from SMBIOS: X13YOGA, X1CARBON, ...): CR/HG/RS reports, PF perf\r\nsnapshots, BOOTLOG/BOOTENV/BOOTS and the network test NETLOG.\r\nCopy this whole folder to amanuensis for a Claude Code agent to read.\r\n' \
            > build/esp/CRASH/README.TXT
        # NO starter WIFI.CFG in the image: the flasher's developer-options
        # copy stages the real creds as wifi.txt, and a shipped placeholder
        # WIFI.CFG would SHADOW it (the driver checks WIFI.CFG first). End
        # users get a starter from tools/uno-wifi-fw.py instead.
        rm -f build/esp/WIFI.CFG
        "$PY" tools/mkcorpus.py build/esp || echo "[dbg] mkcorpus warning (non-fatal)"
        # DEBUG.CFG present = the stress driver is ARMED.  Ship a SAFE default
        # (no allow-force, so it never self-crashes; it only drives the OS and
        # lets real bugs surface).  Rename/delete to boot a quiet desktop; add
        # 'allow-force' to prove the crash pipeline end to end.  See DEBUG.md.
        # ALWAYS rewritten (no -f guard): the QEMU harnesses used to leave
        # their own config in build/esp, and an if-absent guard shipped a
        # dev-run DEBUG.CFG (allow-force + endless fast) on real sticks -
        # same class of leak as the purged telemetry above.
        # BOUNDED by default (passes=3): while the driver is running the
        # operator cannot reach Start > Shut Down, so an unbounded run can
        # only be ended by pulling the power. After N passes the driver
        # goes idle and hands back a usable desktop. F12 stops it early.
        # Keys are listed one-per-line commented out, NOT in a prose list -
        # the parser ignores comments now, but a key hiding in a comment is
        # exactly the bug that made every "safe" stick self-crash (F1).
        #
        # KEYS FIRST, COMMENTS LAST, and it matters.  Readers of this file parse
        # a bounded window: pc64_stress.c takes CFG_MAX bytes, and iwlwifi.c's
        # file_has_ssid() searches only the first 255 for `ssid=`.  With the
        # header on top, this file's own comment block (526 bytes) pushed
        # `passes=3` to byte 526 and any appended wifi.creds past 536 - outside
        # BOTH windows, so the shipped config was inert and a WiFi boot join
        # reported creds:MISSING with the credentials sitting right there in the
        # file.  Emitting keys first keeps the file correct for every reader,
        # whatever window each one uses, and however long the header grows.
        { printf 'passes=3\r\n'
          # local Wi-Fi credentials if present (pc64/wifi.creds is .gitignored
          # so the psk is never committed; the driver reads ssid=/psk= from
          # DEBUG.CFG via firmware_volume). No file = no creds staged.
          if [ -f wifi.creds ]; then cat wifi.creds; fi
          printf '#\r\n'
          printf '# UnoDOS pc64 stress driver config (presence of this file = armed)\r\n'
          printf '# Press F12 on the machine to stop the driver early.\r\n'
          printf '# Keys go ABOVE this header - readers parse a bounded window.\r\n'
          printf '#\r\n'
          printf '# passes=N     stop after N passes, then POWER OFF by itself\r\n'
          printf '#              (0/absent = run forever, F12 to stop)\r\n'
          printf '# nostress     disable the fuzz driver (net/spec tests still run)\r\n'
          printf '# noshutdown   finish the run but leave the desktop up\r\n'
          printf '# fast | slow  action cadence\r\n'
          printf '# allow-force  self-test: force a #PF (proves the crash pipeline)\r\n'
          printf '# force-hang   self-test: force a freeze (proves the watchdog)\r\n'
        } > build/esp/DEBUG.CFG
        if [ -f wifi.creds ]; then echo "[dbg] staged wifi.creds -> DEBUG.CFG"; fi
        # BUILD.TXT stamp so a report can be tied back to an exact image.
        # cfgver = the STRESS.CFG key generation this OS understands; the
        # flasher's Reconfigure refuses to write settings onto a disk stamped
        # older than the keys it writes (UnoReconfig.CFG_GENERATION - keep the
        # two in sync). Bump it whenever a config key is added or renamed.
        printf 'UnoDOS pc64 DEBUG build\r\nid: %s\r\ncfgver: 2\r\nubsan: %s  dbgcon: %s\r\n' \
            "$DBG_ID" "${UNO_UBSAN:-1}" "${UNO_DBGCON:-0}" > build/esp/BUILD.TXT
    fi

    # ---- the legacy-BIOS image, from the SAME objects --------------------
    # Same kernel, second link: flat at 0x100000, entry uno_bios_main, file
    # alignment equal to section alignment so the file is a byte-for-byte image
    # of memory (boot/bios_stage2.asm copies bytes; it is not a PE loader).
    # Built from $OBJS, so the two front ends can never drift apart - a BIOS
    # image is never a stale build of a UEFI kernel.  See docs/BIOS-BOOT-PLAN.md.
    #
    # IT RUNS HERE, at the END, because it packages build/esp into a FAT
    # partition and that tree is not complete until this point - the fonts,
    # media, .UNO modules and (in a debug build) the CRASH dir and fuzz
    # corpus are all added above. Built any earlier it shipped a partial
    # system: a desktop that comes up with most of its apps missing.
    if [ "${UNO_NOBIOS:-0}" = "0" ]; then
        echo "[bios] linking the legacy-BIOS image..."
        "$CC" -nostdlib -e uno_bios_main \
              -Wl,--image-base,0x100000 -Wl,--disable-reloc-section \
              -Wl,--section-alignment,0x1000 -Wl,--file-alignment,0x1000 \
              -o build/UNODOS.SYS $OBJS ${LINK_EXTRA:-} -lgcc
        # UNO_BIOS_VERBOSE=1 builds a stage2 that narrates each step and WAITS
        # FOR A KEY before the video switch. That switch is one-way and takes
        # the text console with it, so a machine that dies after it can only be
        # diagnosed by what was on screen before - and on real hardware the
        # text scrolls past too fast to read. Diagnostic only; never ship it.
        # UNO_BIOS_NOVIDEO=1: skip the VBE mode entirely, stay in TEXT mode, and
        # prove long mode by writing to 0xB8000 from 64-bit code. The one probe
        # that still has an output channel when the framebuffer is not being
        # displayed. Diagnostic only - it never boots the kernel.
        NASMV=""
        [ "${UNO_BIOS_VERBOSE:-0}" = "0" ] || NASMV="-dVERBOSE"
        [ "${UNO_BIOS_NOVIDEO:-0}" = "0" ] || NASMV="$NASMV -dVERBOSE -dNOVIDEO"
        # UNO_BIOS_PREF=WxH overrides the preferred VBE mode stage2 asks for.
        # ONLY for targets with no panel that could fail to sync it - an
        # emulator. On real hardware a mode the display cannot take is a black
        # screen with no way back, which is why the default stays 1024x768.
        if [ -n "${UNO_BIOS_PREF:-}" ]; then
            _pw=${UNO_BIOS_PREF%%x*}
            _ph=${UNO_BIOS_PREF##*x}
            case "$_pw$_ph" in
                *[!0-9]*|"") echo "UNO_BIOS_PREF must be WxH, got '$UNO_BIOS_PREF'" >&2; exit 1 ;;
            esac
            NASMV="$NASMV -dPREF_W=$_pw -dPREF_H=$_ph"
            echo "[bios] preferred mode overridden to ${_pw}x${_ph}"
        fi
        nasm -f bin -o build/bios_boot.bin   boot/bios_boot.asm
        nasm -f bin $NASMV -o build/bios_stage2.bin boot/bios_stage2.asm
        "$PY" tools/mkbios.py build/bios_boot.bin build/bios_stage2.bin \
                              build/UNODOS.SYS build/unodos-hybrid.img build/esp
    fi

    ls -l build/BOOTX64.EFI; echo "done: unoui shell (default) -> build/esp/"
    if [ "$1" = "run" ]; then
        OVMF=/usr/share/OVMF/OVMF_CODE_4M.fd
        cp /usr/share/OVMF/OVMF_VARS_4M.fd build/vars.fd
        exec qemu-system-x86_64 -machine q35 -m 256 \
            -drive if=pflash,format=raw,readonly=on,file=$OVMF \
            -drive if=pflash,format=raw,file=build/vars.fd \
            -drive format=raw,file=fat:rw:build/esp \
            -device qemu-xhci -device usb-tablet -vnc :0
    fi
    exit 0
fi

echo "[2/3] compiling the LEGACY core + subsystems + apps..."
OBJS=""
for f in fb mac_compat pc64_io pc64_libc pc64_math pc64_modload_static pc64_pci pc64_fs blkdev ahci nvme sdhci ide fat tls_ca e1000 net tls tls_entropy hid_kbd i2c_hid xhci usbio usbboot usbmsc usbhid detachgate unovirt hv_svm hv_vmx unovdev unoamp_out unoamp_in unoamp_skin unoamp_vis unoamp_dsp unoamp_enc unoamp_mod unoamp_app unoamp_ui pc64_mtrr uefi_main pc64_native unodos snd_pcm hdaudio ac97 unovdev_pc hv_phases unovdev_net unovirt_mgr; do
    "$CC" $CFLAGS -c -o "build/$f.o" "$f.c"
    OBJS="$OBJS build/$f.o"
done
# uno3d: portable pipeline + software rasteriser + Intel scaffold + the game
for u in uno3d uno3d_soft uno3d_intel uno3d_game; do
    "$CC" $CFLAGS -c -o "build/$u.o" "../uno3d/$u.c"
    OBJS="$OBJS build/$u.o"
done
# BearSSL (TLS) - portable C only; the 8 CPU-accel / OS-entropy files that
# pull intrinsics or OS headers are excluded (portable equivalents are built).
echo "      compiling BearSSL..."
BSSL_SKIP=" ghash_pclmul sysrng aes_x86ni aes_x86ni_cbcdec aes_x86ni_cbcenc aes_x86ni_ctr aes_x86ni_ctrcbc chacha20_sse2 "
BSSLF="-O2 -ffreestanding -fno-stack-protector -fno-stack-check -nostdinc \
       -Iinclude -Ibearssl/inc -Ibearssl/src -DUNO_PC64"
for c in $(find bearssl/src -name '*.c' | sort); do
    base=$(basename "$c" .c)
    case "$BSSL_SKIP" in *" $base "*) continue;; esac
    "$CC" $BSSLF -c -o "build/bssl_$base.o" "$c"
    OBJS="$OBJS build/bssl_$base.o"
done
for app in sysinfo clock files notepad music dostris outlast pacman tracker paint theme settings network runner; do
    "$CC" $CFLAGS -DUNO_APP_SYM=uno_app_main_$app -c -o "build/app_$app.o" "apps/$app.c"
    OBJS="$OBJS build/app_$app.o"
done

echo "[3/3] linking the UEFI image..."
"$CC" -nostdlib -Wl,--subsystem,10 -e efi_main -Wl,--dynamicbase,--nxcompat \
    -o build/BOOTX64.EFI $OBJS -lgcc

mkdir -p build/esp/EFI/BOOT
cp build/BOOTX64.EFI build/esp/EFI/BOOT/BOOTX64.EFI
ls -l build/BOOTX64.EFI
echo "done: LEGACY build/esp/ (boot with ./build.sh legacy run)"

if [ "$2" = "run" ]; then
    OVMF=/usr/share/OVMF/OVMF_CODE_4M.fd
    cp /usr/share/OVMF/OVMF_VARS_4M.fd build/vars.fd
    exec qemu-system-x86_64 -machine q35 -m 256 \
        -drive if=pflash,format=raw,readonly=on,file=$OVMF \
        -drive if=pflash,format=raw,file=build/vars.fd \
        -drive format=raw,file=fat:rw:build/esp \
        -device qemu-xhci -device usb-tablet \
        -vnc :0
fi
