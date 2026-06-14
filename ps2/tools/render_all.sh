#!/bin/sh
# Build + render every UnoDOS/PS2 host-desktop AUTOTEST variant to shots/m1_*.png.
# Run in WSL:  wsl.exe bash /mnt/c/.../ps2/tools/render_all.sh
set -e
cd "$(dirname "$0")/.."
export PATH="/usr/bin:/bin:$PATH"
for f in "" stack PACMAN PAINT THEME DOSTRIS TRACKER FILES OUTLAST FAT12; do
  if bash build.sh desktop "$f" >/dev/null 2>&1; then
    echo "ok ${f:-desktop}"
  else
    echo "FAIL ${f:-desktop}"
  fi
done
ls -la shots/m1_*.png
