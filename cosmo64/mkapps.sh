#!/bin/sh
# cosmo64/mkapps.sh -- runs ON quill (build.sh apps stages and invokes it):
# cross-build the .UNO app modules for aarch64 into build/apps/.
#
# This is pc64/build.sh's module pipeline with the target changed and nothing
# else: compile the app's objects, list every symbol they leave undefined,
# refuse any that pc64_modload.c does not export (a typo is a build failure,
# not an app that loads and then jumps nowhere), generate the import thunks
# for THIS machine, link a DLL with no libraries and no exports but the entry,
# and flatten it with mkuno.py -- which reads the machine off the PE header
# and stamps the ABI word so an x86-64 kernel refuses these files and this
# kernel refuses theirs.
#
# Env from build.sh: LMBIN (the llvm-mingw bin dir), APPCF (the shell's own
# compile flags, so a module obeys -mstrict-align and the rest exactly as the
# kernel does). Runs from $QDIR/cosmo64 with the pc64 tree beside it.
set -e
CC="$LMBIN/aarch64-w64-mingw32-clang"
NM="$LMBIN/llvm-nm"
PY="${PY:-python3}"
MKUNO=../pc64/tools/mkuno.py
OUT=build/apps
mkdir -p "$OUT"

# The legal-import list, from the kernel's own table. `n` is the KX() macro's
# parameter name and matches the grep; it is harmless in a whitelist.
grep -oE 'KX\([A-Za-z_0-9]+\)' ../pc64/pc64_modload.c | sed 's/KX(//;s/)//' \
    | sort -u > "$OUT/kexports.txt"

# cc <obj> <src>: one object with the module flags
cc() { $CC $APPCF -DUNO_APP_SYM=uno_app_main -c -o "$1" "$2"; }

# mod <name> <flags> <obj>...: imports check, thunks, link, flatten
mod() {
    name=$1; flags=$2; shift 2
    # imports = undefined across ALL objects minus what any object defines
    $NM "$@" | awk '$1=="U"&&$2!=""{u[$2]=1} $1!="U"&&NF>=3{d[$3]=1} \
        END{for(s in u) if(!(s in d)) print s}' | sort -u > "$OUT/$name.syms"
    while read -r s; do
        [ -z "$s" ] && continue
        grep -qx "$s" "$OUT/kexports.txt" || {
            echo "FAIL: $name imports '$s' which pc64_modload.c does not export" >&2
            exit 1; }
    done < "$OUT/$name.syms"
    $PY $MKUNO thunks "$OUT/$name.syms" "$OUT/${name}_thunks.s" aarch64
    $CC -c -o "$OUT/${name}_thunks.o" "$OUT/${name}_thunks.s"
    $CC -shared -nostdlib -e uno_app_main -Wl,--exclude-all-symbols \
        -o "$OUT/$name.dll" "$@" "$OUT/${name}_thunks.o"
    UP=$(echo "$name" | tr '[:lower:]' '[:upper:]')
    $PY $MKUNO convert "$OUT/$name.dll" "$OUT/$UP.UNO" "$flags"
}

# ---- the classic tier: a 4-colour Toolbox canvas each (flags 0) ------------
# The five the launcher rosters (pc64_uui_apps.c's kProc). x86 also packs
# MUSIC and NETWORK, but neither has had a launcher slot since their panes
# went native, so a file nothing can open is not built here.
for app in dostris pacman outlast tracker paint; do
    cc "$OUT/$app.o" "../pc64/apps/$app.c"
    mod "$app" 0 "$OUT/$app.o"
done

# ---- unoui-class modules (flags 1): a real desktop window each -------------
cc "$OUT/logview.o" ../pc64/apps/logview.c
mod logview 1 "$OUT/logview.o"

# Photos carries the IMAGE half of unomedia inside the module, as on x86 (the
# audio half links into the kernel there; here neither does yet)
POBJ="$OUT/photos.o"
cc "$OUT/photos.o" ../pc64/apps/photos.c
for b in unomedia um_image um_inflate um_png um_jpg um_gif um_bmp \
         um_ico um_tga um_pnm um_qoi um_webp um_vp8 um_stub; do
    $CC $APPCF -c -o "$OUT/um_$b.o" "../unomedia/$b.c"
    POBJ="$POBJ $OUT/um_$b.o"
done
mod photos 1 $POBJ

echo "[mkapps] done:"; ls -l "$OUT"/*.UNO
