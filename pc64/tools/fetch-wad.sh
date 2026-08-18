#!/bin/sh
# fetch-wad.sh - get an IWAD for Duum.
#
#   ./tools/fetch-wad.sh              # Freedoom (default)
#   ./tools/fetch-wad.sh --shareware  # id Software's shareware DOOM1.WAD
#
# DEFAULTS TO FREEDOOM, and the reason is redistribution. Freedoom is a
# from-scratch, Doom-compatible IWAD under a BSD licence, so it can be BUILT
# INTO the ISO and the flasher and handed to anyone. id's shareware WAD may be
# passed around unmodified but is not ours to bake into another product, so it
# stays an opt-in for local use.
#
# Either way the file lands as wads/freedoom1.wad or wads/DOOM1.WAD, and
# build.sh stages whichever it finds onto the boot disk as DOOM1.WAD. wads/ is
# gitignored: game data does not belong in the source tree.
set -e
cd "$(dirname "$0")/.."
mkdir -p wads

if [ "$1" = "--shareware" ]; then
    OUT=wads/DOOM1.WAD
    # Mirrors of the id shareware doom1.wad (IWAD header 'IWAD', ~4 MB).
    MIRRORS="
https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad
https://github.com/Akbar30Bill/DOOM_wad/raw/master/DOOM1.WAD
"
else
    OUT=wads/freedoom1.wad
    # Freedoom Phase 1, from the project's own release assets.
    MIRRORS="
https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
"
fi
if [ -f "$OUT" ]; then echo "already have $OUT"; exit 0; fi
for u in $MIRRORS; do
    echo "fetching $u"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 2 -o "$OUT" "$u" && break || true
    else
        wget -O "$OUT" "$u" && break || true
    fi
done

# Freedoom ships as a zip; take the one lump we want out of it.
case "$OUT" in
  *freedoom1.wad)
    if [ -s "$OUT" ] && head -c4 "$OUT" | grep -q PK; then
        command -v unzip >/dev/null 2>&1 || { echo "need unzip for the Freedoom archive"; exit 1; }
        tmp=$(mktemp -d)
        mv "$OUT" "$tmp/freedoom.zip"
        unzip -q -j "$tmp/freedoom.zip" "*/freedoom1.wad" -d wads/ || \
            unzip -q -j "$tmp/freedoom.zip" "freedoom1.wad" -d wads/
        rm -rf "$tmp"
    fi
    ;;
esac

if [ ! -s "$OUT" ]; then echo "download failed - see wads/README.md"; exit 1; fi
# sanity: WAD magic is 'IWAD' or 'PWAD'
magic=$(dd if="$OUT" bs=1 count=4 2>/dev/null)
case "$magic" in
    IWAD|PWAD) echo "ok: $OUT ($(wc -c < "$OUT") bytes, $magic)";;
    *) echo "warning: $OUT does not start with IWAD/PWAD (got '$magic')"; exit 1;;
esac
