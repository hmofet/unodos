#!/bin/sh
# fetch-wad.sh - download the Doom shareware IWAD (DOOM1.WAD) for Duum.
#
# The shareware "Knee-Deep in the Dead" WAD is freely distributable by id
# Software but is NOT committed to this repo (wads/ is gitignored).  This
# grabs it from a stable public mirror into wads/DOOM1.WAD.  It is the
# official shareware data Duum is tested against.
set -e
cd "$(dirname "$0")/.."
mkdir -p wads
OUT=wads/DOOM1.WAD
if [ -f "$OUT" ]; then echo "already have $OUT"; exit 0; fi

# Mirrors of the id shareware doom1.wad (IWAD header 'IWAD', ~4 MB).
MIRRORS="
https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad
https://github.com/Akbar30Bill/DOOM_wad/raw/master/DOOM1.WAD
"
for u in $MIRRORS; do
    echo "fetching $u"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 2 -o "$OUT" "$u" && break || true
    else
        wget -O "$OUT" "$u" && break || true
    fi
done

if [ ! -s "$OUT" ]; then echo "download failed - see wads/README.md"; exit 1; fi
# sanity: WAD magic is 'IWAD' or 'PWAD'
magic=$(dd if="$OUT" bs=1 count=4 2>/dev/null)
case "$magic" in
    IWAD|PWAD) echo "ok: $OUT ($(wc -c < "$OUT") bytes, $magic)";;
    *) echo "warning: $OUT does not start with IWAD/PWAD (got '$magic')"; exit 1;;
esac
