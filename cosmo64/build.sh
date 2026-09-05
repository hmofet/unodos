#!/bin/sh
# UnoDOS pc64-on-ARM (Cosmo Communicator, MT6771) build.
#
# Toolchain: llvm-mingw's aarch64-w64-mingw32-clang on quill -- PE/COFF and
# LLP64, the same object format and data model as the x86 pc64, per the
# toolchain decision in research/pc64-arm-port-plan.md (hmofet/cosmo). Images
# link at LK's load address so the flat payload needs no relocation;
# flatten.py lays the PE out (and stamps an ARM64 Image header, so the same
# binary boots under `qemu -kernel`) and the asm port's mkbootimg.py wraps it
# for the p38 slot.
#
#   ./build.sh          -> the m0/m1 TEST payload  (build/m0.bin + boot img)
#   ./build.sh shell    -> the pc64 SHELL          (build/shell.bin + boot img)
#   ./build.sh calib    -> the TOUCH CALIBRATION payload (build/calib.bin)
#   ./build.sh usb      -> the USB HOST PROBE      (build/usbprobe.bin)
#   ./build.sh apps     -> the .UNO APP MODULES    (build/apps/*.UNO, M8)
#
# Verify on quill (real QEMU; see qharness.py):
#   scp qharness.py build/<x>.bin quill:/work/unodos-cosmo64/ &&
#   ssh quill 'cd /work/unodos-cosmo64 && python3 qharness.py <x>.bin /tmp/x.png 3'
set -e
cd "$(dirname "$0")"

PY="${PY:-python3}"
QUILL="${QUILL:-arin@192.168.2.114}"
QDIR="/work/unodos-pc64arm"
LMBIN="/opt/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/bin"
CC="$LMBIN/aarch64-w64-mingw32-clang"

# Baseline flags (see m0.c / README for the history):
#   -fsigned-char   pc64/include/limits.h hardcodes signed char
#   -fno-builtin    pc64_libc.c defines memset/memcpy; the idiom recognizer
#                   must not rewrite their own loops into calls to themselves
# -mstrict-align is applied ONLY to code that can run before the MMU is on
# (mmu.c, and everything in the m0 payload); the shell's fb blits run MMU-on.
BASECF="-O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-stack-check \
        -nostdinc -fno-builtin -fsigned-char"
# /merge:.unodrv=.data: the UNO_DRIVER registration tables xhci.c and usbhid.c
# emit are 16 initialised bytes in a section of their own, and lld places an
# unknown section AFTER .data -- whose virtual extent is the shell's 72 MB of
# .bss. flatten.py ships everything up to the last non-zero byte, so those 16
# bytes made the payload 76 MB, which is the size that hung LK's decompressor
# at the splash in M1. Nothing here reads the table (no device manager), so
# it goes inside .data's initialised part where it costs nothing. flatten.py
# trips on any shipped image over 16 MB so this cannot recur silently.
# /merge:.pdata=.rdata: the compiler-runtime archive below brings ONE function
# with unwind data, and eight bytes of .pdata is enough to do what the .unodrv
# table did in M4 -- land after .data, whose virtual extent is the shell's
# ~100 MB of .bss, and drag flatten.py's trim point to a 119 MB shipped image.
# Nothing here unwinds (there is no SEH and no OS to run it), so the eight
# bytes go where every other read-only word goes and cost nothing.
LINK="-nostdlib -Wl,--image-base,0x40080000 -e _start \
      -Wl,-Xlink=/merge:.unodrv=.data -Wl,-Xlink=/merge:.pdata=.rdata"
# THE ONE LIBRARY THIS IMAGE LINKS, and only since the browser: unojs converts
# decimal to double through __int128 (64 bits cannot keep adjacent doubles
# distinct), which lowers to __udivti3 and __floatuntidf -- compiler-runtime
# helpers, not libc. x86 answers this with -lgcc; llvm-mingw ships the same
# helpers as compiler-rt, and the archive is asked for BY THE COMPILER
# (-print-libgcc-file-name) rather than named by path, so a toolchain bump
# cannot silently point at a stale version directory. It is a static archive:
# only the two helpers actually referenced land in the image.

stage_quill() {
  ssh "$QUILL" "mkdir -p $QDIR/cosmo64/build $QDIR/pc64/build $QDIR/pc64/tools $QDIR/unoui/themes $QDIR/uno3d"
  scp -q ./*.s ./*.c ./*.h ./*.py mkapps.sh "$QUILL:$QDIR/cosmo64/"
}

# The pc64 tree the shell and the .UNO modules are both built from, minus the
# big vendored stacks a shell-only build never sees (tar over ssh: Git Bash
# has no rsync). unojs used to be staged for its HEADERS alone (pc64_modload.c
# exports the engine's embedding surface by name); as of the browser it is
# COMPILED, and unoweb -- the renderer -- comes with it.
stage_tree() {
  echo "[stage] pc64 core + unoui + cosmo64 -> quill"
  stage_quill
  (cd .. && tar czf - \
      --exclude=pc64/upy --exclude=pc64/unocode --exclude=pc64/quickjs \
      --exclude=pc64/shots --exclude=pc64/flash --exclude=pc64/remote \
      --exclude=pc64/build --exclude=pc64/tools \
      pc64 unoui uno3d unosound unomedia unoacpi unojs unoweb unodoc) | ssh "$QUILL" "tar xzf - -C $QDIR"
  # csslib is NOT staged: 57k lines of vendored CSS for a second cascade this
  # image does not carry. But pc64_browser.c includes its embedder header, so
  # that ONE file goes over on its own and stubs.c answers its three functions.
  ssh "$QUILL" "mkdir -p $QDIR/csslib"
  scp -q ../csslib/uwx.h "$QUILL:$QDIR/csslib/"
  scp -q ../pc64/build/font_data.h ../pc64/build/world_map.h "$QUILL:$QDIR/pc64/build/"
  # two files from the excluded pc64/tools: the URC host client, for
  # qharness.py's QHARNESS_URC gate, and the module packer, for `apps`
  scp -q ../pc64/tools/unoauto_remote.py "$QUILL:$QDIR/cosmo64/"
  scp -q ../pc64/tools/mkuno.py "$QUILL:$QDIR/pc64/tools/"
}

mkdir -p build

case "$1" in
# ---------------------------------------------------------------------------
"" )
  echo "[m0] cross-compiling the test payload on quill..."
  stage_quill
  ssh "$QUILL" "cd $QDIR/cosmo64 && \
    $CC $BASECF -mstrict-align -c m0.c -o build/m0.o && \
    $CC $BASECF -mstrict-align -c videolfb.c -o build/videolfb.o && \
    $CC $BASECF -mstrict-align -c mmu.c -o build/mmu.o && \
    $CC $BASECF -mstrict-align -c log.c -o build/log.o && \
    $CC $BASECF -mstrict-align -c msdc.c -o build/msdc.o && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/m0.exe build/entry.o build/cpu.o build/m0.o \
        build/videolfb.o build/mmu.o build/log.o build/msdc.o"
  scp -q "$QUILL:$QDIR/cosmo64/build/m0.exe" build/
  "$PY" flatten.py build/m0.exe build/m0.bin
  OUT=build/m0.bin
  ;;
# ---------------------------------------------------------------------------
shell )
  stage_tree

  # The Tier-1 portable core (dependency survey 2026-08-31; the nine themes
  # are all named by kThemes[] in pc64_uui.c, so all nine link).
  UNOUI="unoui unoui_input unoui_anim unoui_wmanim"
  THEMES="theme_aurora theme_unodos theme_macos7 theme_macplus theme_win31 \
          theme_amiga theme_c64 theme_apple2 theme_next"
  PCORE="fb pc64_libc pc64_math pc64_font pc64_icons pc64_qoi pc64_uui_apps \
         mac_compat pc64_io pc64_write pc64_clock pc64_files pc64_uui \
         fat pc64_fs hid_kbd net uno_binds unolog"
  # The providers slice (after M8): three portable subsystems that were
  # export stubs, compiled for real now that modules can reach them.
  # uno_binds (key bindings + app prefs, persisted beside SHELL.CFG) and
  # unolog (the system log LOGVIEW.UNO shows) are plain pc64 files above;
  # uno3d is the software rasteriser plus Runner3D, whose kernel side
  # (pc64_games.c) names the Intel backend by default and takes the soft one
  # here by the UNO_U3D_BACKEND seam -- no PCI GPU exists on this SoC.
  U3D="uno3d uno3d_soft uno3d_game"
  GAMES="-DUNO_U3D_BACKEND=u3d_backend_soft"
  # M9: the browser. Four subsystems, none of them edited for this machine:
  # unoweb is the renderer (DOM / HTML / CSS / layout / paint), unojs the
  # script engine behind js.c's shim, the browser lane is the chrome plus the
  # HTTP client and the fetch/cookie/cache trio, and unomedia's IMAGE half
  # rides behind unoweb's uw_images hook so <img> decodes. unojs needs the
  # compiler runtime for __int128 (its decimal<->double conversion), which the
  # link line supplies -- see LINKRT below.
  #
  # TWO THINGS THIS IMAGE DELIBERATELY DOES NOT CARRY, both second engines
  # whose first engine has never run here: csslib (57k lines: the libcss
  # cascade, reached only from the uno:engine page) and quickjs (64k: the
  # other script backend). stubs.c answers uwx_libcss_* and js_run_qjs, so
  # the switches report them absent instead of lying about what they did.
  # BearSSL is a slice of its own, so tls_* stays stubbed: HTTP and uno:
  # pages now, HTTPS when the 294 files have been built for this toolchain.
  UJS="ujs_core ujs_math ujs_lex ujs_comp ujs_vm ujs_lib ujs_promise ujs_api"
  UWEB="uw_dom uw_html uw_css uw_style uw_layout"
  BROWSER="pc64_fetch pc64_cookie pc64_cache js webjs pc64_browser"
  UMI="unomedia um_image um_inflate um_png um_jpg um_gif um_bmp um_ico \
       um_tga um_pnm um_qoi um_webp um_vp8 um_stub"
  # pc64_http.c is the one file in the lane that needs a flag. Its
  # pc64_net_up/pc64_net_boot walk eight NIC families on PC bus timings; on
  # this machine an unanswered USB transfer costs its full multi-second
  # timeout, so netup.c has owned bring-up since M5 and this drops the pair
  # (the seam is #ifndef UNO_NET_BRINGUP_EXTERNAL, upstream in pc64_http.c).
  HTTPCF="-DUNO_NET_BRINGUP_EXTERNAL"
  # HTTPS. tls.c is BearSSL's glue, tls_ca.c the bundled roots, and
  # tls_entropy.c the fail-closed source behind both -- the last of which is
  # x86 asm (cpuid/rdrand/rdtsc) below its own primitive seam, so it is
  # compiled -DTLS_ENT_PLATFORM and cosmo64/entropy.c supplies the three
  # calls. The jitter engine, its health test and every refusal are the SAME
  # portable code x86 runs; the security argument is not forked per
  # architecture, which is the point of the seam.
  #
  # THE CLOCK IS PART OF THIS. br_x509_minimal_set_time decides whether a
  # certificate's window contains now, and this machine has no RTC -- against
  # tls.c's 1970 fallback every certificate on the internet is not yet valid.
  # cosmo64/clock.c is the answer (a monotonic software clock over CNTPCT,
  # seeded from the stamp below or from CLOCK.CFG on the card).
  TLS="tls tls_ca"                 # tls_entropy.c is compiled below, with $ENTCF
  ENTCF="-DTLS_ENT_PLATFORM"
  # The build stamp: seconds since the epoch, at build time. It is a lower
  # bound on "now" that costs nothing and is right within days for a fresh
  # image; clock.c takes the later of it and the saved time, so the clock can
  # never be dragged backwards by either.
  BUILD_EPOCH=$(date +%s)
  echo "[shell] clock seed: build stamp $BUILD_EPOCH ($(date -u -d @$BUILD_EPOCH '+%Y-%m-%d %H:%M UTC' 2>/dev/null || date -u))"
  # M8: the .UNO module loader, compiled unchanged but for two seams it
  # carries for this platform. -DUNO_MODLOAD_LOG routes its diagnostics
  # ("modload: bad crc", "unresolved import X") to uno_dbg_log and so to the
  # eMMC log, where a load that fails on the device can be read afterwards;
  # its aarch64 branch calls uno_pc64_code_sync (cpu.s) after writing a
  # module's code. The arena it allocates from is platform.c's.
  MODLOAD="-DUNO_MODLOAD_LOG"
  # M6: unoautomate + the URC remote channel, compiled UNCHANGED. The two
  # files that carry the privilege gate are built -DUNO_DEBUG (per file, like
  # the usb renames -- no shared header changes layout under it) because the
  # production arming path needs an account on a FAT volume this device does
  # not mount yet; urc.c explains, and URC_PIN=<6 digits> ./build.sh shell
  # keeps the production auth rules with a token from the build instead.
  # unoauto_compat.c is deliberately absent: urc.c supplies its symbols with
  # a real clock and a log that reaches the eMMC.
  URC="unoauto unoauto_probe unoauto_screen netdisc unostorage"
  URCDBG="unoauto_gate unoauto_remote"
  C64="videolfb display platform input stubs i2c kbd touch codi log msdc \
       pmic sdmmc blk ssusb pci usb netup urc clock rtc entropy"
  if [ -n "$URC_PIN" ]; then
    printf '#define C64_URC_PIN "%s"\n' "$URC_PIN" > urc_pin.h
    echo "[shell] URC gate: production auth with the build-time PIN"
  else
    rm -f urc_pin.h
    echo "[shell] URC gate: open (no PIN) -- set URC_PIN=<6 digits> to close it"
  fi
  # stage_quill ran before the header was decided: re-stage it, or remove a
  # stale one so a PIN never lingers on quill from an earlier build
  if [ -f urc_pin.h ]; then scp -q urc_pin.h "$QUILL:$QDIR/cosmo64/";
  else ssh "$QUILL" "rm -f $QDIR/cosmo64/urc_pin.h"; fi

  # KBDTEST=1: compile the scripted key pad (QEMU gate proof, never shipped)
  [ -n "$KBDTEST" ] && BASECF="$BASECF -DC64_KBDTEST"
  # TOUCHDBG=1: paint the raw touch report on the panel edge (bring-up aid)
  [ -n "$TOUCHDBG" ] && BASECF="$BASECF -DC64_TOUCHDBG"
  # BLKTEST=1: put fat.c + pc64_fs.c through a format/write/read/delete round
  # trip over a RAM transport, so the QEMU gate covers the storage stack the
  # virt board's missing MSDC otherwise leaves untested (36 MiB of .bss --
  # never ship a BLKTEST image)
  [ -n "$BLKTEST" ] && BASECF="$BASECF -DC64_BLKTEST"
  # PMIC_WRITE=0 builds an image that PHYSICALLY CANNOT write the PMIC: the
  # whitelist, the read-modify-write helper and the wrapper's write call all
  # live inside pmic.c's #if, so no instruction that sets the command word's
  # write bit is emitted at all. That was the default while the MT6358 address
  # map was unverified; it is 1 now, because the map was confirmed on hardware
  # on 2026-09-03 and a build that cannot switch the two SD rails on is a
  # build with no SD card. Reach for 0 on a NEW unit, or after touching any
  # address in pmic.c's table. pmic.c's own header carries the full argument.
  [ -n "$PMIC_WRITE" ] && BASECF="$BASECF -DC64_PMIC_WRITE=$PMIC_WRITE" \
      && echo "[shell] PMIC writes: $PMIC_WRITE"
  # -Wno-error=implicit-function-declaration: mingw-gcc merely warns on the
  # declared-later-in-the-same-file pattern pc64_uui.c uses; clang 16+ errors.
  # The linker still catches genuinely missing functions.
  # -Wno-error=incompatible-function-pointer-types: the same shape of
  # difference, one file down. unomedia.h types its allocator hook as
  # void *(*)(unsigned long) and the browser hands it malloc, which pc64's
  # stdlib.h types as void *(size_t) -- and on LLP64 those are 32 and 64 bits
  # wide. The call is benign (a size_t narrowed to 32 bits, for an image
  # smaller than 4 GB) and x86 has shipped it since the browser learned to
  # decode <img>; clang 16+ just makes it an error where gcc warns. Downgraded
  # rather than silenced: it still prints, so a REAL mismatch stays visible.
  # -mstrict-align FOR EVERYTHING on this device: the hardware bisect of
  # 2026-09-01 showed unaligned accesses wedge the core silently (no EL1
  # fault -- consistent with GenieZone's stage-2 imposing Device-type memory),
  # and each build compiled without it died at its first merged wide load
  # while the fully strict-align m0 runs the same path clean.
  # -DFB_MAX_W/-DFB_MAX_H: fb.h's ceiling sizes fb[] and unoui's cached
  # desktop background, and it defaults to a PC monitor's 1920x1200. This
  # panel's native desktop is 2160x1080 -- wider AND shorter -- so the default
  # would have clipped the desktop this port now starts in.
  SHCF="$BASECF -mstrict-align -DC64_BUILD_EPOCH=${BUILD_EPOCH}ll \
        -Wno-error=implicit-function-declaration \
        -Wno-error=incompatible-function-pointer-types \
        -DFB_MAX_W=2160 -DFB_MAX_H=1080 \
        -DUNO_COLOR=1 -DUNO_PC64 -DUNO_UUI -Dmain=uno_main \
        -I$QDIR/pc64/include -I$QDIR/pc64 -I$QDIR/unoui -I$QDIR/uno3d \
        -I$QDIR/pc64/bearssl/inc -I$QDIR/unosound -I$QDIR/unomedia \
        -I$QDIR/unoacpi -I$QDIR/unoacpi/uacpi/include -I$QDIR/cosmo64"

  # M5: ax88179.c joins them on the same terms. Its bulk calls are renamed as
  # well as its control ones, because uno_usb_bulk_in/out put the CALLER's
  # pointer straight into the TRB and the driver's tx[]/g_rx[] are ordinary
  # cached .bss. Moving the driver's own statics into .xdma with the xhci
  # pragma would also work and would then make it parse every received frame
  # out of Device memory a byte at a time; usb.c bounces instead, so the
  # staging area is uncached and the parse is not.
  #
  # M4: the USB lane's xhci.c and usbhid.c, compiled UNCHANGED for this
  # platform. -DUNO_XHCI turns the real driver on (it is inert stubs without
  # it); -include c64_usbglue.h moves every DMA buffer in xhci.c into the
  # ".xdma" section mmu.c maps uncached (C64_XDMA) and routes uno_dbg_log to
  # the eMMC log; the two -D renames send usbhid.c's control transfers
  # through usb.c's bounce buffer, because it passes a STACK buffer and the
  # controller cannot see through the cache to it. The tripwire after the
  # link fails the build if xhci.o still owns a .bss: a DMA structure left in
  # write-back memory does not fail, it corrupts.
  USBCF="$SHCF -DUNO_XHCI -include c64_usbglue.h"
  # BearSSL: vendored third-party portable C, so -w like uACPI and csslib --
  # its own warnings are not this port's to fix and they would drown the
  # kernel's. -mstrict-align comes along with $BASECF for the reason every
  # other file gets it (an unaligned access wedges this core silently), and
  # -Ibearssl/src is needed on top of inc/ because the implementation files
  # include their private headers by bare name.
  #
  # The SKIP LIST IS ARCHITECTURAL, not a preference: these are the x86
  # accelerated implementations (AES-NI, PCLMUL, SSE2 ChaCha20) plus sysrng,
  # which wants an OS. Every one has a portable equivalent in the same
  # directory that builds and is what this image uses. x86 skips exactly the
  # same set for the same reason (pc64/build.sh BSSL_SKIP) -- there are no
  # aarch64-accelerated files in this vendored tree to add back.
  BSSL_SKIP=" ghash_pclmul sysrng aes_x86ni aes_x86ni_cbcdec aes_x86ni_cbcenc \
              aes_x86ni_ctr aes_x86ni_ctrcbc chacha20_sse2 "
  # Objects are bs_*.o, NOT b_*.o: the `usb` payload case below already
  # writes build/b_<file>.o, so a stale usbprobe/mmu/log/msdc left there by
  # an earlier `./build.sh usb` was swept straight into the shell link by a
  # b_*.o glob -- which reads as two dozen duplicate-symbol errors naming
  # files that have nothing to do with what you just changed.
  BSSLCF="$BASECF -mstrict-align -w -DUNO_PC64 \
          -I$QDIR/pc64/include -I$QDIR/pc64/bearssl/inc -I$QDIR/pc64/bearssl/src"
  OBJDUMP="$LMBIN/llvm-objdump"

  # The list is computed ON quill (the tree is there, and Git Bash's find
  # would hand back Windows paths the cross-compiler cannot open).
  BSSL=$(ssh "$QUILL" "cd $QDIR/pc64 && find bearssl/src -name '*.c' | sort | \
      while read c; do b=\$(basename \$c .c); \
          case \"$BSSL_SKIP\" in *\" \$b \"*) continue;; esac; \
          echo ../pc64/\$c; done | tr '\n' ' '")
  echo "[shell] BearSSL: $(echo $BSSL | wc -w) files"

  ssh "$QUILL" "set -e; cd $QDIR/cosmo64 && \
    for f in $UNOUI; do $CC $SHCF -c ../unoui/\$f.c -o build/u_\$f.o; done && \
    for f in $THEMES; do $CC $SHCF -c ../unoui/themes/\$f.c -o build/t_\$f.o; done && \
    for f in $PCORE; do $CC $SHCF -c ../pc64/\$f.c -o build/p_\$f.o; done && \
    $CC $SHCF $MODLOAD -c ../pc64/pc64_modload.c -o build/p_pc64_modload.o && \
    $CC $SHCF $GAMES -c ../pc64/pc64_games.c -o build/p_pc64_games.o && \
    for f in $U3D; do $CC $SHCF -c ../uno3d/\$f.c -o build/p_\$f.o; done && \
    for f in $UJS; do $CC $SHCF -c ../unojs/\$f.c -o build/p_\$f.o; done && \
    for f in $UWEB; do $CC $SHCF -c ../unoweb/\$f.c -o build/p_\$f.o; done && \
    for f in $UMI; do $CC $SHCF -c ../unomedia/\$f.c -o build/p_\$f.o; done && \
    for f in $BROWSER; do $CC $SHCF -c ../pc64/\$f.c -o build/p_\$f.o; done && \
    $CC $SHCF $HTTPCF -c ../pc64/pc64_http.c -o build/p_pc64_http.o && \
    for f in $TLS; do $CC $SHCF -c ../pc64/\$f.c -o build/p_\$f.o; done && \
    $CC $SHCF $ENTCF -c ../pc64/tls_entropy.c -o build/p_tls_entropy.o && \
    for c in $BSSL; do \
        b=\$(basename \$c .c); \
        $CC $BSSLCF -c \$c -o build/bs_\$b.o; \
    done && \
    for f in $URC; do $CC $SHCF -c ../pc64/\$f.c -o build/p_\$f.o; done && \
    for f in $URCDBG; do $CC $SHCF -DUNO_DEBUG -c ../pc64/\$f.c -o build/p_\$f.o; done && \
    $CC $USBCF -DC64_XDMA -c ../pc64/xhci.c -o build/p_xhci.o && \
    $CC $USBCF -Duno_usb_get_config=c64_usb_get_config \
        -Duno_usb_control=c64_usb_control \
        -Duno_usb_setup_intr_in=c64_usb_setup_intr_in \
        -c ../pc64/usbhid.c -o build/p_usbhid.o && \
    $CC $USBCF -Duno_usb_get_config=c64_usb_get_config \
        -Duno_usb_control=c64_usb_control \
        -Duno_usb_bulk_in=c64_usb_bulk_in \
        -Duno_usb_bulk_out=c64_usb_bulk_out \
        -c ../pc64/ax88179.c -o build/p_ax88179.o && \
    if $OBJDUMP -h build/p_xhci.o | grep -Eq '\.bss +0*[1-9a-f]'; then \
        echo 'BUILD TRIPWIRE: xhci.o still has a .bss -- DMA memory would be cached' >&2; exit 1; fi && \
    { $OBJDUMP -h build/p_xhci.o | grep -q '\.xdma' || { echo 'BUILD TRIPWIRE: no .xdma section in xhci.o' >&2; exit 1; }; } && \
    for f in $C64; do $CC $SHCF -mstrict-align -c \$f.c -o build/c_\$f.o; done && \
    $CC $SHCF -mstrict-align -c mmu.c -o build/mmu_sa.o && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/shell.exe build/entry.o build/cpu.o build/mmu_sa.o \
        build/u_*.o build/t_*.o build/p_*.o build/c_*.o build/bs_*.o \
        \$($CC -rtlib=compiler-rt -print-libgcc-file-name) && \
    $OBJDUMP -h build/shell.exe | grep -E 'xdma|\.data|\.text'"
  scp -q "$QUILL:$QDIR/cosmo64/build/shell.exe" build/
  FLATTEN_IMGSZ=shipped "$PY" flatten.py build/shell.exe build/shell.bin
  OUT=build/shell.bin
  ;;
# ---------------------------------------------------------------------------
apps )
  # M8: the .UNO app modules, cross-built for aarch64 (build/apps/*.UNO).
  # Same pipeline as pc64/build.sh's [3b]/[3d] steps -- compile, list the
  # undefined symbols, refuse any the kernel does not export, thunk, link as a
  # DLL, flatten -- run by mkapps.sh ON quill with the shell's own flags, so a
  # module meets the same -mstrict-align, LLP64 and freestanding rules as the
  # kernel it will be loaded into. The modules are not part of the boot image:
  # they go to APPS\ on the SD card (URC `put 1 APPS\X.UNO`, or from Trixie),
  # or onto the RAM disk for the QEMU gate (QHARNESS_UNO=build/apps/X.UNO).
  echo "[apps] cross-compiling the .UNO modules on quill..."
  stage_tree
  APPCF="$BASECF -mstrict-align -Wno-error=implicit-function-declaration \
        -DFB_MAX_W=2160 -DFB_MAX_H=1080 \
        -DUNO_COLOR=1 -DUNO_PC64 -DUNO_UUI \
        -I$QDIR/pc64/include -I$QDIR/pc64 -I$QDIR/unoui -I$QDIR/uno3d \
        -I$QDIR/unosound -I$QDIR/unomedia -I$QDIR/cosmo64"
  ssh "$QUILL" "cd $QDIR/cosmo64 && LMBIN=$LMBIN APPCF='$APPCF' sh mkapps.sh"
  mkdir -p build/apps
  scp -q "$QUILL:$QDIR/cosmo64/build/apps/*.UNO" build/apps/
  ls -l build/apps/*.UNO
  exit 0
  ;;
# ---------------------------------------------------------------------------
calib )
  # The touch calibration payload: draws targets in RAW PANEL PIXELS and logs
  # the controller's RAW report, so the calibration path carries none of the
  # transform it exists to measure. Every cosmo64 driver it needs is
  # self-contained (cosmo64.h only), so this builds with the plain flags -- no
  # pc64 tree, no unoui, no 67 MB of .bss.
  echo "[calib] cross-compiling the calibration payload on quill..."
  stage_quill
  CAL="calib videolfb mmu log msdc i2c kbd touch"
  ssh "$QUILL" "set -e; cd $QDIR/cosmo64 && \
    for f in $CAL; do $CC $BASECF -mstrict-align -c \$f.c -o build/k_\$f.o; done && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/calib.exe build/entry.o build/cpu.o build/k_*.o"
  scp -q "$QUILL:$QDIR/cosmo64/build/calib.exe" build/
  "$PY" flatten.py build/calib.exe build/calib.bin
  OUT=build/calib.bin
  ;;
usb )
  # The USB host probe: reports what state LK leaves the SSUSB controller in,
  # which is what decides whether M4 is an adoption (like the eMMC) or a full
  # bring-up (like the SD card). Self-contained -- cosmo64.h only, no pc64
  # tree, no unoui -- and it needs no input drivers, so it links even less
  # than calib.
  echo "[usb] cross-compiling the USB host probe on quill..."
  stage_quill
  USBP="usbprobe videolfb mmu log msdc"
  ssh "$QUILL" "set -e; cd $QDIR/cosmo64 && \
    for f in $USBP; do $CC $BASECF -mstrict-align -c \$f.c -o build/b_\$f.o; done && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/usbprobe.exe build/entry.o build/cpu.o build/b_*.o"
  scp -q "$QUILL:$QDIR/cosmo64/build/usbprobe.exe" build/
  "$PY" flatten.py build/usbprobe.exe build/usbprobe.bin
  OUT=build/usbprobe.bin
  ;;
* )
  echo "usage: ./build.sh [shell|calib|usb|apps]" >&2
  exit 1
  ;;
esac

echo "[boot image] wrapping..."
# mkbootimg.py is the asm port's (cosmo/ lane, in-tree since the 2026-09-01
# merge) -- consumed, not edited.
MKBOOTIMG="${MKBOOTIMG:-../cosmo/mkbootimg.py}"
[ -f "$MKBOOTIMG" ] || { echo "mkbootimg.py not found at $MKBOOTIMG -- set MKBOOTIMG" >&2; exit 1; }
"$PY" "$MKBOOTIMG" "$OUT" build/pc64arm-boot.img
echo "    -> cosmo64/build/pc64arm-boot.img  (from $OUT)"
echo "    install:  scp build/pc64arm-boot.img the-cosmo:  then as root:"
echo "              dd if=pc64arm-boot.img of=/dev/mmcblk0p38 bs=1M conv=fsync"
