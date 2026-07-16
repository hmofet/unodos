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
        -nostdinc -Iinclude -I. -I../uno3d \
        -DUNO_COLOR=1 -DUNO_PC64 -Dmain=uno_main ${UNO_EXTRA:-}"

echo "[2/3] compiling the kernel + subsystems + apps..."
OBJS=""
for f in fb mac_compat pc64_io pc64_libc pc64_math pc64_modload pc64_pci e1000 net uefi_main unodos; do
    "$CC" $CFLAGS -c -o "build/$f.o" "$f.c"
    OBJS="$OBJS build/$f.o"
done
# uno3d: portable pipeline + software rasteriser + Intel scaffold + the game
for u in uno3d uno3d_soft uno3d_intel uno3d_game; do
    "$CC" $CFLAGS -c -o "build/$u.o" "../uno3d/$u.c"
    OBJS="$OBJS build/$u.o"
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
echo "done: build/esp/ (boot with ./build.sh run, or harness.py for the scripted pass)"

if [ "$1" = "run" ]; then
    OVMF=/usr/share/OVMF/OVMF_CODE_4M.fd
    cp /usr/share/OVMF/OVMF_VARS_4M.fd build/vars.fd
    exec qemu-system-x86_64 -machine q35 -m 256 \
        -drive if=pflash,format=raw,readonly=on,file=$OVMF \
        -drive if=pflash,format=raw,file=build/vars.fd \
        -drive format=raw,file=fat:rw:build/esp \
        -device qemu-xhci -device usb-tablet \
        -vnc :0
fi
