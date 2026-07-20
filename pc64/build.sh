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
CC="${CC:-x86_64-w64-mingw32-gcc}"
mkdir -p build shots

echo "[1/3] exporting the shared font to a C array..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i >/dev/null)
"$PY" mkfont_c.py
# the Clock's world map (public-domain Natural Earth land mask). The GeoJSON is
# cached in tools/, so this is offline after the first run.
[ -f build/world_map.h ] || "$PY" tools/mkworldmap.py

CFLAGS="-O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-stack-check \
        -nostdinc -Iinclude -I. -I../uno3d -Ibearssl/inc \
        -DUNO_COLOR=1 -DUNO_PC64 -Dmain=uno_main ${UNO_EXTRA:-}"

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
    UCF="$CFLAGS -DUNO_UUI -DUNO_I2C_TRACKPAD -DUNO_BG_CACHE -DUNO_ACPI \
         -I../unoui -I../unosound -I../unomedia -I../unoacpi -I../unoacpi/uacpi/include"
    OBJS=""
    # UnoSound live sequencer (game/app audio over the PC-speaker voice)
    "$CC" $UCF -c -o "build/unosound_seq.o" "../unosound/unosound_seq.c"; OBJS="$OBJS build/unosound_seq.o"
    # platform + shell + the legacy-app bridge (mac_compat = Toolbox over fb)
    for f in fb mac_compat pc64_libc pc64_io pc64_pci pc64_math pc64_fs blkdev ahci nvme sdhci fat hid_kbd i2c_hid xhci usbhid ax88179 rtl8152 iwlwifi rtwifi mrvlwifi wifi_wpa uefi_main pc64_native pc64_uui pc64_uui_apps pc64_write pc64_files pc64_music pc64_clock pc64_media pc64_modload pc64_games js pc64_http pc64_font pc64_browser pc64_icons e1000 e1000e igb r8169 net tls tls_ca acpi_host installer snd_pcm hdaudio ac97; do
        "$CC" $UCF -c -o "build/$f.o" "$f.c"; OBJS="$OBJS build/$f.o"
    done
    # unomedia AUDIO half (core + WAV/MIDI/MP3/AAC) - linked into the kernel
    # for the native Music app. The IMAGE half ships inside PHOTOS.UNO below,
    # a second private instance of the library, so the two never collide.
    for u in unomedia um_audio um_wav um_midi um_mp3 um_aac; do
        "$CC" $UCF -c -o "build/uml_$u.o" "../unomedia/$u.c"; OBJS="$OBJS build/uml_$u.o"
    done
    # unoacpi: shared AML/ACPI power stack (verbatim from writers-unlock) + the
    # vendored uACPI interpreter (third-party -> -w).
    for u in acpi_arena ec_handler smbus_handler acpi_power; do
        "$CC" $UCF -c -o "build/unoacpi_$u.o" "../unoacpi/$u.c"; OBJS="$OBJS build/unoacpi_$u.o"
    done
    for c in $(find ../unoacpi/uacpi/source -name '*.c' | sort); do
        b=$(basename "$c" .c)
        "$CC" $UCF -w -c -o "build/uacpi_$b.o" "$c"; OBJS="$OBJS build/uacpi_$b.o"
    done
    # uno3d: the write-once 3D pipeline + soft rasteriser + Intel scaffold + game
    # (Runner3D is a native game canvas that drives these directly).
    for u in uno3d uno3d_soft uno3d_intel uno3d_game; do
        "$CC" $UCF -c -o "build/uui_$u.o" "../uno3d/$u.c"; OBJS="$OBJS build/uui_$u.o"
    done
    for u in unoui unoui_input; do
        "$CC" $UCF -c -o "build/uui_$u.o" "../unoui/$u.c"; OBJS="$OBJS build/uui_$u.o"
    done
    for t in $(find ../unoui/themes -name '*.c' | sort); do
        b=$(basename "$t" .c)
        "$CC" $UCF -c -o "build/uui_$b.o" "$t"; OBJS="$OBJS build/uui_$b.o"
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
        "$CC" $BSSLF -c -o "build/bssl_$base.o" "$c"; OBJS="$OBJS build/bssl_$base.o"
    done
    echo "[3/3] linking the unoui image..."
    "$CC" -nostdlib -Wl,--subsystem,10 -e efi_main -Wl,--dynamicbase,--nxcompat \
        -o build/BOOTX64.EFI $OBJS
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

    # ---- Intel WiFi firmware (TESTING only) --------------------------------
    # The iwlwifi driver loads FIRMWARE\IWL*.UCO from the ESP. Intel's licence
    # forbids redistribution, so the blobs are NOT in the repo (fw-blobs/ is
    # gitignored). If a developer has populated fw-blobs/ (via the fetch tool or
    # tools/fetch-fw.sh), bundle them so a flashed test stick can bring WiFi up
    # without a separate copy step. A clean clone has no fw-blobs/ -> no bundle.
    if ls fw-blobs/*.UCO >/dev/null 2>&1; then
        echo "[fw] bundling Intel WiFi firmware from fw-blobs/ (testing)"
        mkdir -p build/esp/FIRMWARE
        cp fw-blobs/*.UCO build/esp/FIRMWARE/ 2>/dev/null || true
        cp fw-blobs/*.PNV build/esp/FIRMWARE/ 2>/dev/null || true
        # a starter WIFI.CFG the user edits with their SSID/passphrase
        [ -f build/esp/WIFI.CFG ] || printf '# UnoDOS WiFi config - edit these two lines\nssid=YourNetwork\npsk=YourPassphrase\n' > build/esp/WIFI.CFG
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
            "$CC" $UCF -DUNO_APP_SYM=uno_app_main \
                  -DUCC_KEXPORTS_H='"build/apps/ucc_kexports.h"' \
                  -c -o "build/apps/$s.o" "apps/$s.c"
            SOBJ="$SOBJ build/apps/$s.o"
        done
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
        cp sdk/UNO.H sdk/SAMPLE.C sdk/DOSTRIS.C build/esp/SDK/ 2>/dev/null || true
        # Python SDK: sources (.py/.PY) + type stubs (.pyi).  Copy each match
        # individually so a missing glob never drops a valid sibling, and both
        # letter cases are picked up (the on-disk names are upper-case).
        for f in sdk/*.py sdk/*.PY sdk/*.pyi sdk/*.pyi; do
            [ -f "$f" ] && cp "$f" build/esp/SDK/
        done
        [ -d docs_esp ] && cp docs_esp/*.MD build/esp/DOCS/ 2>/dev/null || true
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
              upy_port/pc64_upy_port.c upy_port/mod_uno.c upy_port/pc64_upy_stubs.c \
              apps/pyrt.c"
        "$PY" upy_port/mkupy.py --top upy --port upy_port --build "$PB" \
              --cpp "$CC -E" --cflags "$PYF" -- $PSRC
        POBJ=""
        for s in $PSRC; do
            o="$PB/$(echo "$s" | tr '/.' '__').o"
            "$CC" $PYF -DUNO_APP_SYM=uno_app_main -c -o "$o" "$s"
            POBJ="$POBJ $o"
        done
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

    # ---- PHOTOS.UNO: the image viewer, a unoui-CLASS module ----------------
    # The app plus the whole unomedia image-decoding library (top-level
    # unomedia/ - PNG incl. its own inflate, baseline JPEG, GIF, BMP, TGA,
    # PNM, QOI, ICO, all from scratch) statically linked into the module:
    # the kernel gains no decoder code, just the fb_blit export.
    if [ "${UNO_PHOTOS:-1}" != "0" ]; then
        echo "[3d] building PHOTOS.UNO (the image viewer + unomedia)..."
        POBJ="build/apps/photos.o"
        "$CC" $UCF -DUNO_APP_SYM=uno_app_main \
              -c -o "build/apps/photos.o" "apps/photos.c"
        # core + the IMAGE half only (the AUDIO half links into the kernel)
        for b in unomedia um_image um_inflate um_png um_jpg um_gif um_bmp \
                 um_ico um_tga um_pnm um_qoi um_stub; do
            "$CC" $UCF -c -o "build/apps/um_$b.o" "../unomedia/$b.c"
            POBJ="$POBJ build/apps/um_$b.o"
        done
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

    # ---- DUUM.UNO: the Python Doom engine (a PYAPP; needs PYRT + a WAD) -----
    # Duum is a Python app, so it packages like any .py (source -> PYAPP
    # container).  The WAD is developer-supplied game data, never committed
    # (wads/ is gitignored, like the Wi-Fi fw-blobs): staged onto the ESP only
    # when present.  Freedoom (BSD) is the shippable free default.
    if [ "${UNO_PYRT:-1}" != "0" ] && [ -f apps/DUUM.PY ]; then
        echo "[3e] packaging DUUM.UNO (the Python Doom engine)..."
        "$PY" tools/mkuno.py pyapp apps/DUUM.PY build/esp/APPS/DUUM.UNO
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
for f in fb mac_compat pc64_io pc64_libc pc64_math pc64_modload_static pc64_pci pc64_fs blkdev ahci nvme sdhci fat tls_ca e1000 net tls hid_kbd i2c_hid xhci usbhid uefi_main pc64_native unodos snd_pcm hdaudio ac97; do
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
    -o build/BOOTX64.EFI $OBJS

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
