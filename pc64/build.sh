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
         -I../unoui -I../unosound -I../unoacpi -I../unoacpi/uacpi/include"
    OBJS=""
    # UnoSound live sequencer (game/app audio over the PC-speaker voice)
    "$CC" $UCF -c -o "build/unosound_seq.o" "../unosound/unosound_seq.c"; OBJS="$OBJS build/unosound_seq.o"
    # platform + shell + the legacy-app bridge (mac_compat = Toolbox over fb)
    for f in fb mac_compat pc64_libc pc64_io pc64_pci pc64_math pc64_fs blkdev ahci fat hid_kbd i2c_hid xhci usbhid ax88179 uefi_main pc64_native pc64_uui pc64_uui_apps pc64_modload pc64_games js pc64_http pc64_font pc64_browser pc64_icons e1000 net tls tls_ca acpi_host installer; do
        "$CC" $UCF -c -o "build/$f.o" "$f.c"; OBJS="$OBJS build/$f.o"
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
    cp fonts/Sans.ttf   build/esp/SANS.TTF
    cp fonts/Mono.ttf   build/esp/MONO.TTF
    cp fonts/Ubuntu.ttf build/esp/UBUNTU.TTF

    # ---- .UNO app modules: every app is loaded from storage at runtime -----
    # (apps/<name>.c -> object -> import thunks -> linked DLL -> APPS/<N>.UNO)
    echo "[3b] building the .UNO app modules..."
    NM="${NM:-x86_64-w64-mingw32-nm}"
    mkdir -p build/apps build/esp/APPS
    grep -oE 'KX\([A-Za-z_0-9]+\)' pc64_modload.c | sed 's/KX(//;s/)//' \
        | sort -u > build/apps/kexports.txt
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
for f in fb mac_compat pc64_io pc64_libc pc64_math pc64_modload_static pc64_pci pc64_fs blkdev ahci fat tls_ca e1000 net tls hid_kbd i2c_hid xhci usbhid uefi_main pc64_native unodos; do
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
