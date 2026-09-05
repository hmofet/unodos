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

# ---- the Office suite: three unoui-class modules, the PHOTOS pattern -------
# Each carries the uoffice chrome lane plus ONLY its own format half of unodoc
# (Word / Excel / PowerPoint), statically linked, so no module pays for
# another's format and the kernel gains no document code. um_inflate is linked
# in rather than imported: a module that called the kernel's um_set_alloc would
# repoint the allocator the browser and UnoAmp use. Same as pc64/build.sh's
# [3d2]/[3d3]/[3d4], with the -I paths adjusted for the cosmo64 tree layout.
#
# -mno-stack-arg-probe stops the compiler emitting a __chkstk stack probe for
# unodoc's large on-stack frames (the .doc FIB and the OOXML buffers). x86
# mingw calls ___chkstk_ms and the aarch64 mingw target calls __chkstk; both
# probe Windows guard pages, a mechanism this OS does not have, and a
# freestanding module has nothing to resolve the call against (it is not a
# kernel export, and cpu.s's __chkstk lives in the shell image, not in a
# module). The flag drops the probe on both machines.
OFFCF="$APPCF -mno-stack-arg-probe -I../unoui -I../pc64/uoffice -I../unodoc -I../unomedia"
office() {          # <name> <uoffice-objs...> -- <unodoc-objs...>
    name=$1; shift
    uo=""; while [ "$1" != "--" ]; do uo="$uo $1"; shift; done; shift
    objs="$OUT/$name.o"
    $CC $OFFCF -DUNO_APP_SYM=uno_app_main -c -o "$OUT/$name.o" "../pc64/apps/$name.c"
    for b in $uo; do
        $CC $OFFCF -c -o "$OUT/${name}_$b.o" "../pc64/uoffice/$b.c"
        objs="$objs $OUT/${name}_$b.o"
    done
    for b in "$@"; do
        $CC $OFFCF -c -o "$OUT/${name}_$b.o" "../unodoc/$b.c"
        objs="$objs $OUT/${name}_$b.o"
    done
    # unomedia.c defines um_alloc/um_free/um_set_alloc that um_inflate.c uses;
    # both are linked into the module (as on x86), never imported, so the
    # module's own allocator is not the kernel's.
    for b in unomedia um_inflate; do
        $CC $OFFCF -c -o "$OUT/${name}_um_$b.o" "../unomedia/$b.c"
        objs="$objs $OUT/${name}_um_$b.o"
    done
    mod "$name" 1 $objs
}
office uoword uochrome uoicons uodlg uobars uofile uow_doc uow_layout \
       -- unodoc ud_cfb ud_doc ud_docw ud_zip ud_xml ud_docx ud_ooxz ud_docxw
office uocalc uochrome uoicons uodlg uobars uofile uxl_sheet uxl_calc uxl_numfmt \
       -- unodoc ud_cfb ud_xls ud_xlsw ud_ptg ud_ptgc ud_zip ud_xml ud_xlsx ud_ooxz ud_xlsxw
office uoshow uochrome uoicons uodlg uobars uofile uos_geom uos_model uos_render \
       -- unodoc ud_cfb ud_ppt ud_pptw ud_escher ud_zip ud_xml ud_pptx ud_ooxz ud_pptxw

echo "[mkapps] done:"; ls -l "$OUT"/*.UNO
