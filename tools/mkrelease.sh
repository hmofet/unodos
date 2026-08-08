#!/bin/sh
# Collect every port's shipping artifact into one staging directory, under the
# names the download page and the GitHub release use.
#
#   tools/mkrelease.sh [OUTDIR]        default: build/release
#
# This does NOT build anything. Build first, then run this; it fails loudly on
# anything missing rather than quietly shipping a short release.
#
# Two reasons this script exists rather than a hand-written `gh release create`
# line:
#
#  1. Ports collide on filenames. pinephone and ppcmac both emit `unodos.bin`;
#     c64 and vic20 both emit `unodos.prg`. A GitHub release is one flat
#     namespace, so the mapping below renames on collection. Get this wrong by
#     hand once and someone downloads a PowerPC image for their phone.
#
#  2. The firmware gate. pc64/build.sh bundles Intel Wi-Fi firmware by DEFAULT
#     (UNO_NOFW defaults to 0). Publishing that would redistribute Intel's
#     blobs. The check below refuses to stage a pc64 image whose ESP still
#     carries them, so the rule cannot be forgotten. See CLAUDE.md.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/release}

cd "$ROOT"

# port|source path relative to repo root|name in the release
SPECS='
x86|build/unodos-144.img|unodos-144.img
c64|c64/build/unodos_c64.d64|unodos_c64.d64
vic20|vic20/build/unodos.prg|unodos_vic20.prg
nes|nes/build/unodos.nes|unodos.nes
snes|snes/build/unodos.sfc|unodos.sfc
gb|gb/build/unodos.gb|unodos.gb
gba|gba/build/unodos.gba|unodos.gba
sms|sms/build/unodos.sms|unodos.sms
gg|gg/build/unodos.gg|unodos.gg
genesis|genesis/build/unodos.gen|unodos.gen
dreamcast|dreamcast/build/unodos-dc-uui.iso|unodos-dc-uui.iso
pce|pce/build/unodos.pce|unodos.pce
ws|ws/build/unodos.ws|unodos.ws
ps2|ps2/build/unodos-ps2.elf|unodos-ps2.elf
amiga|amiga/build/unodos68k.adf|unodos68k.adf
amiga|amiga/build/unodos-data.adf|unodos-data.adf
apple2|apple2/build/unodos_apple2.dsk|unodos_apple2.dsk
apple2|apple2/build/unodos_apple2.woz|unodos_apple2.woz
iigs|iigs/build/unodos_iigs.po|unodos_iigs.po
macplus|macplus/build/unodos_macplus.dsk|unodos_macplus.dsk
ppcmac|ppcmac/build/unodos.bin|unodos_ppcmac.bin
rpi|rpi/build/kernel8.img|kernel8.img
pinephone|pinephone/build/unodos.bin|unodos_pinephone.bin
pc64|pc64/build/unodos-pc64.iso|unodos-pc64.iso
pc64|pc64/build/unodos-hybrid.img.gz|unodos-pc64-hybrid.img.gz
'

# There is no separate BIOS ISO, and there does not need to be. mkbios.py emits
# build/unodos-hybrid.img: a 96 MiB disk image with a BIOS boot sector in the
# MBR and an ESP/FAT32 partition at LBA 16384, so the same file boots a legacy
# BIOS machine and a UEFI one. The .iso is the convenience artifact for
# attaching to a VM's CD-ROM; the .img.gz is the one you write to a USB stick.

# ---- firmware gate ------------------------------------------------------
# Refuse to stage a pc64 image built with the Intel blobs still in it.
if [ -d pc64/build/esp ]; then
    if find pc64/build/esp -iname '*.UCO' | grep -q .; then
        echo "REFUSING TO STAGE: pc64/build/esp still contains Intel Wi-Fi" >&2
        echo "firmware (*.UCO). Rebuild with UNO_NOFW=1 before releasing." >&2
        find pc64/build/esp -iname '*.UCO' | sed 's/^/  /' >&2
        exit 1
    fi
    echo "[fw] gate passed: no firmware blobs staged in pc64/build/esp"
else
    echo "[fw] warning: pc64/build/esp not present, gate not exercised" >&2
fi

rm -rf "$OUT"
mkdir -p "$OUT"

missing=0
staged=0
echo "$SPECS" | while IFS='|' read -r port src dst; do
    [ -n "${port:-}" ] || continue
    if [ -f "$src" ]; then
        cp "$src" "$OUT/$dst"
        printf '  %-10s %-28s %10s\n' "$port" "$dst" "$(wc -c < "$src")"
        staged=$((staged + 1))
    else
        printf '  %-10s %-28s %10s\n' "$port" "$dst" "MISSING"
        missing=$((missing + 1))
    fi
done

# The while loop above runs in a subshell, so recount here for the exit status.
absent=0
for line in $(echo "$SPECS" | tr -d ' '); do
    [ -n "$line" ] || continue
    src=$(echo "$line" | cut -d'|' -f2)
    [ -f "$src" ] || absent=$((absent + 1))
done

( cd "$OUT" && sha256sum ./* > SHA256SUMS 2>/dev/null || true )

echo
echo "staged into $OUT"
echo "  $(find "$OUT" -type f ! -name SHA256SUMS | wc -l) artifacts, $absent missing"

if [ "$absent" -gt 0 ]; then
    echo
    echo "Some artifacts are absent. Build them, or drop them from SPECS and"
    echo "record the gap in PLATFORMS.md and the site's downloads.json." >&2
    exit 2
fi
