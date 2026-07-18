# retro-shot — headless libretro render-verify for the retro ports

Renders a built ROM to a PNG **with no display and no root**, for the ports that
have no in-repo `harness.py` and no accurate standalone emulator on the dev box:
**genesis, sms, gg** (via the `genesis_plus_gx` core) and **snes** (`snes9x`), plus
**gb** (`gambatte`). Same idea as the py65/Unicorn `harness.py` files, but driven by
official libretro cores so the VDP/PPU/vblank timing is accurate (it catches
dropped-write / vblank-overrun bugs, not just logic).

Used to render-verify the cross-port performance fixes (see `AUDIT-*.md` §1): build
the `AUTOTEST_*` variant, render N frames, and pixel-diff against a baseline built
from the pre-change source. A correct redraw refactor is **byte-identical**.

## Files
- `retro_shot.c` — ~200-line libretro frontend: dlopen a core, load a ROM, run N
  frames with no input (the AUTOTEST ROMs self-drive), capture the last
  `video_refresh` frame, write a PPM. Handles 0RGB1555 / RGB565 / XRGB8888.
- `ppm2png.py` — PPM → PNG (stdlib only).
- `shot.sh <genesis|snes|gb|.so> <rom> <out.png> [frames]` — wraps the two.

## Setup (once)
```sh
# cores from the official libretro buildbot (no root):
BB=https://buildbot.libretro.com/nightly/linux/x86_64/latest
mkdir -p ~/uno-emu/cores && cd ~/uno-emu/cores
for c in genesis_plus_gx snes9x gambatte; do
  curl -sSLO "$BB/${c}_libretro.so.zip" && python3 -c "import zipfile,sys;zipfile.ZipFile(sys.argv[1]).extractall()" ${c}_libretro.so.zip
done
# libretro.h + build the frontend
curl -sSLo ~/uno-emu/libretro.h https://raw.githubusercontent.com/libretro/RetroArch/master/libretro-common/include/libretro.h
gcc -O2 -I ~/uno-emu -o ~/uno-emu/retro_shot retro_shot.c -ldl
```

## Verify pattern
```sh
# baseline from unedited source (stash the fix), then the fixed build, then diff:
git stash push <port>/<edited files>
<port>/build.sh dostris && ./shot.sh genesis <port>/build/unodos_dt.gen /tmp/base.ppm 600
git stash pop
<port>/build.sh dostris && ./shot.sh genesis <port>/build/unodos_dt.gen /tmp/fix.ppm  600
cmp /tmp/base.ppm /tmp/fix.ppm   # identical == the refactor preserves every pixel
```

ROM extensions the cores autodetect: genesis `.gen`, sms `.sms`, gg `.gg`, snes
`.sfc`, gb `.gb`.

> DC (Flycast) and PS2 (PCSX2) use their own rigs — `dreamcast/tools/emu_run.sh` /
> `capture_apps.sh` and `ps2/tools/run_pcsx2_windowed.ps1`. Mac uses
> `mac/shots/runshot.sh` (Executor). This directory is only the retro-console rig.
