#!/bin/sh
# ===========================================================================
# Prove the EE overlay loader's relocation engine (ps2/ee_modload.c) is correct.
#
# For each app module it (1) compiles the source to an ET_REL .o, (2) links that
# .o with stub kernel symbols at a fixed text base via `ld` (the GOLDEN relocated
# bytes), and (3) runs tools/relotest.c - which applies the SAME relocation
# algorithm ee_modload.c uses - and compares the result to the golden link.
# All 11 must report "MATCH (relocated == ld golden)".
#
# The only EE-specific bits NOT covered here are the mc0: read + FlushCache
# (pure I/O around this proven math).  Requires the ps2dev toolchain on PATH.
# ===========================================================================
set -e
cd "$(dirname "$0")/.."
: "${PS2DEV:=/usr/local/ps2dev}"
export PS2DEV PS2SDK="${PS2SDK:-$PS2DEV/ps2sdk}"
export PATH="$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2SDK/bin:$PATH"
T=/tmp/uno_relotest; mkdir -p "$T"
GCC=mips64r5900el-ps2-elf-gcc; LD=mips64r5900el-ps2-elf-ld
EEFLAGS="-D_EE -G0 -O2 -fno-merge-constants -DUNO_COLOR=1 -DUNO_EE \
  -I$PS2SDK/ee/include -I$PS2SDK/common/include -I. -I$PS2DEV/gsKit/include -Ibuild"

# stub kernel symbols the modules import (Toolbox via mac_compat + libc helpers)
cat > "$T/stubs.c" <<'EOF'
void DisposePtr(){} void DrawText(){} void FrameOval(){} void FrameRect(){}
void GetMouse(){} void InsetRect(){} void LineTo(){} void MoveTo(){}
void NewPtr(){} void OffsetRect(){} void PaintOval(){} void PaintRect(){}
void PenMode(){} void PenNormal(){} void PtInRect(){} void RGBForeColor(){}
void Random(){} void SetRect(){} void StillDown(){} void TextMode(){}
void TextWidth(){} void TickCount(){}
void *memmove(void*a,const void*b,unsigned long n){return a;}
void *memset(void*a,int c,unsigned long n){return a;}
char *stpcpy(char*a,const char*b){return a;}
char *strcat(char*a,const char*b){return a;}
char *strcpy(char*a,const char*b){return a;}
unsigned long strlen(const char*a){return 0;}
EOF
$GCC -D_EE -G0 -O2 -c "$T/stubs.c" -o "$T/stubs.o"
gcc -O2 -w -o "$T/relobin" tools/relotest.c

APPS="00:sysinfo 01:clock 02:files 03:notepad 04:music 05:dostris \
      06:outlast 07:pacman 08:tracker 09:paint 10:theme"
pass=0; fail=0
for pair in $APPS; do
  id=${pair%%:*}; nm=${pair##*:}
  $GCC $EEFLAGS -DUNO_APP_SYM=uno_app_main_$nm -c apps/$nm.c -o "$T/a$id.o"
  $LD -Ttext-segment=0x01000000 -e uno_app_main_$nm "$T/a$id.o" "$T/stubs.o" -o "$T/a$id.elf"
  res=$("$T/relobin" "$T/a$id.o" "$T/a$id.elf" | tail -1)
  if echo "$res" | grep -q PASS; then echo "app$id $nm: PASS"; pass=$((pass+1));
  else echo "app$id $nm: $res"; fail=$((fail+1)); fi
done
echo "=== relotest: $pass PASS, $fail FAIL ==="
[ "$fail" -eq 0 ]
