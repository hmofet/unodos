#!/bin/sh
# Build the legacy-BIOS boot chain, and optionally a disk image around it.
#
#   tools/mkbios.sh            assemble boot sector + stage2 into build/
#   tools/mkbios.sh test       ... plus the loader smoke payload -> build/biostest.img
#   tools/mkbios.sh run        ... and boot it in QEMU with SeaBIOS (headless, VNC)
#
# The `test` image carries boot/loadertest.c instead of the OS. Phase A of
# docs/BIOS-BOOT-PLAN.md is proven with that, on purpose: a failure then means
# the loader, not the kernel, and those are two entirely different searches.
set -e
cd "$(dirname "$0")/.."

PY="${PY:-python3}"
CC="${CC:-x86_64-w64-mingw32-gcc}"
mkdir -p build

echo "[bios] assembling the boot chain..."
nasm -f bin -o build/bios_boot.bin   boot/bios_boot.asm
nasm -f bin -o build/bios_stage2.bin boot/bios_stage2.asm
echo "[bios] boot sector $(wc -c < build/bios_boot.bin) B, stage2 $(wc -c < build/bios_stage2.bin) B"

[ "$1" = "test" ] || [ "$1" = "run" ] || exit 0

# The BIOS payload links FLAT at 0x100000 with no relocations: stage2 copies the
# image there byte for byte and jumps in, so the load address has to be the link
# address. --disable-reloc-section keeps a .reloc out of the image (nothing will
# ever apply it), and -nostdlib keeps the mingw CRT out (there is no CRT here -
# execution begins at the entry symbol with a stack and nothing else).
#
# FILE ALIGNMENT MUST EQUAL SECTION ALIGNMENT, and this is the whole reason a
# "flat" PE works at all. A normal PE packs sections tightly in the file (file
# alignment 0x200) and spreads them out in memory (section alignment 0x1000), so
# file offset != RVA and only a real PE loader can place them. stage2 is not a
# PE loader; it copies bytes. Making the two alignments equal makes the file a
# byte-for-byte image of memory, which is what lets it. Get this wrong and the
# symptom is a #UD at an address that is not even an instruction boundary in the
# disassembly, because what ran was never the code you compiled.
echo "[bios] building the loader smoke payload..."
"$CC" -O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-stack-check \
      -fno-pic -mno-red-zone -nostdlib -nostdinc -Iinclude \
      -Wl,--image-base,0x100000 -Wl,--disable-reloc-section \
      -Wl,--section-alignment,0x1000 -Wl,--file-alignment,0x1000 \
      -e uno_bios_loadertest \
      -o build/loadertest.exe boot/loadertest.c

"$PY" tools/mkbios.py build/bios_boot.bin build/bios_stage2.bin \
                      build/loadertest.exe build/biostest.img

[ "$1" = "run" ] || exit 0

echo "[bios] booting under SeaBIOS (VNC :0, ctrl-c to stop)..."
exec qemu-system-x86_64 -machine q35 -m 256 \
     -drive format=raw,file=build/biostest.img,if=ide \
     -display none -vnc :0 -serial stdio
