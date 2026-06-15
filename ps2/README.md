# UnoDOS/PS2 — Sony PlayStation 2 port (milestone 1)

UnoDOS/PS2 is a FreeMcBoot-launched ELF with full hardware access —
"firmware-hosted bare-metal," the richest target in the family. The
strategy ([HANDOFF.md](HANDOFF.md), [../docs/PORTS-PLAN.md](../docs/PORTS-PLAN.md)
§4) is to **port the C core** in [../mac/unodos.c](../mac/unodos.c) — the
complete UnoDOS (11 apps, window manager, event model, cooperative
scheduler, device-abstracted FAT12) — by swapping the platform layer, not
rewriting.

**Status: M1 done.** The whole desktop / window manager / all 11 apps run —
verified both on the host shim (`build.sh desktop`, `shots/m1_*.png`) and on
the **emulated PS2 GS** in PCSX2 (`shots/m1_pcsx2_pacman.png`). The port is
[../mac/unodos.c](../mac/unodos.c) copied to [unodos.c](unodos.c) over a
**Mac-compat shim** ([mac_compat.h](mac_compat.h)/[mac_compat.c](mac_compat.c) +
[mac_io.c](mac_io.c)) that re-implements the ~40 Toolbox calls it uses over
`fb.*`. M2 (File Manager + a FAT12 RAM-disk round-trip) and M3 Theme (32-bit
colour) come along through the shim. Remaining: EE audio (audsrv) and a
memory-card file backend — see [Next](#next).

## Platform design: software framebuffer, GS as a blitter

All UnoDOS drawing happens in software against a **640×448×32 framebuffer**
in EE RAM (~1.1 MB of 32 MB), via the plain-C primitives in
[fb.c](fb.c)/[fb.h](fb.h) (`fb_fill_rect`/`fb_frame_rect`/`fb_invert_rect`/
`fb_text`/`fb_big_text` + the 4-colour PORT-SPEC palette). Each vsync the EE
target uploads the buffer to GS VRAM as one textured fullscreen sprite
([main.c](main.c)) — the GS does init + texture upload + flip, nothing more.

Why: `unodos.c` draws *incrementally* (event-driven partial repaints, XOR
drag outlines, invert highlights). A software FB preserves those semantics
exactly (`uno_invert` is a real XOR) and shrinks the GS role to a blit,
which is also why gsKit-vs-raw-GIF is low-stakes here. M1's `uno_*` draw
wrappers will sit directly on these primitives.

## Toolchain status on this dev machine

The **PS2 toolchain is installed and the EE ELF builds.** The prebuilt
ps2dev release (`ps2dev-ubuntu-latest`, v2.0.0 — ee-gcc + PS2SDK + gsKit) is
unpacked under WSL at `~/ps2dev/ps2dev`; `./build.sh ee` regenerates the
font and links `build/unodos-ps2.elf` (a real MIPS R5900 / N32 executable).
Docker was unavailable, so the prebuilt release replaced the ps2dev image.

**Running it: VERIFIED on the emulated GS.** PCSX2 **v2.6.3** + a **4 MB PS2
BIOS** (`ps2-0200a-20040614.bin`, NTSC-US) boots `build/unodos-ps2.elf` and
renders the M0 splash through the real GS pipeline — `shots/m0_pcsx2.png`,
captured by `tools/run_pcsx2.ps1`. So `main.c`'s GS/pad runtime path (gsKit
init, 640×448 FB→GS blit, primitives, font) is now hardware-path-verified, not
just host-shim-verified. (The 4 MB PS2 BIOS was the missing piece — the earlier
`Sony BIOS` folder held only **512 KB PlayStation *1* BIOSes**, which PCSX2
rejects.) **Rig gotcha:** PCSX2 v2.x validates `[UI] SettingsVersion` and pops a
*"Settings failed to load / incorrect version"* modal — which silently blocks
the boot — unless `PCSX2.ini` carries `SettingsVersion = 1`; `run_pcsx2.ps1`
writes a known-good ini when that key is missing. Real hardware (FMCB) is still
the remaining frontier.

Also fully verified is the handoff's **host shim** (HANDOFF §3, "the
family's fastest inner loop"): the software-FB code (`fb.c` + `uno_splash.c`)
builds with WSL gcc 13 and renders to a PNG (`./build.sh host` →
`shots/m0_splash.png`). The EE target shares `fb.c` + `uno_splash.c`
*verbatim*, so everything the host shim proves — font, palette, every
drawing primitive, the whole splash — also carries to the PS2 unchanged.

## Building

```sh
# --- M1: the full desktop --------------------------------------------------
# host desktop (VERIFIED) - the whole UnoDOS over the Mac-compat shim, via WSL
# gcc, rendered to shots/m1_<tag>.png. FEATURE bakes in a UNO_AUTOTEST_* app so
# the screenshot is self-driving (PACMAN/PAINT/THEME/DOSTRIS/TRACKER/FILES/
# OUTLAST/FAT12, or "stack" for Music+Files+Notepad; empty = the bare desktop).
./build.sh desktop                 # -> shots/m1_desktop.png
./build.sh desktop PACMAN          # -> shots/m1_pacman.png
bash tools/render_all.sh           # build + render every variant at once

# PS2 desktop ELF (BUILDS + RUNS on emulated GS) -> build/unodos-ps2.elf
./build.sh ee                      # interactive desktop (DualShock 2 driven)
./build.sh ee PACMAN               # self-driving screenshot variant
./build.sh ee-splash               # the M0 hello-GS splash ELF (reference)

# --- M0: the splash (reference) --------------------------------------------
./build.sh host          # host splash -> shots/m0_splash.png
```

### Running it (PCSX2, VERIFIED)

From Windows, boot the ELF in PCSX2 and screenshot the GS output:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ps2\tools\run_pcsx2.ps1
# -> ps2\shots\m0_pcsx2.png  (the splash on the real GS pipeline)
```

Prereqs: PCSX2 **v2.6.3** portable + a **4 MB PS2 BIOS** in `pcsx2\bios\`. The
script self-heals `PCSX2.ini` (writes `[UI] SettingsVersion = 1` if missing —
without it PCSX2 v2.x rejects the config and blocks the boot) and launches
`pcsx2-qt.exe -fullscreen -fastboot -elf <elf>`. **512 KB BIOSes are PS1, not
PS2** — PCSX2 rejects them.

### Installing the toolchain (what this machine did)

```sh
# prebuilt ps2dev release (no Docker needed) into WSL:
curl -L -o ps2dev.tgz \
  https://github.com/ps2dev/ps2dev/releases/download/v2.0.0/ps2dev-ubuntu-latest.tar.gz
mkdir -p ~/ps2dev && tar xzf ps2dev.tgz -C ~/ps2dev   # -> ~/ps2dev/ps2dev/{ee,iop,ps2sdk,gsKit}
# build.sh ee sets PS2DEV/PS2SDK/GSKIT/PATH from ~/ps2dev/ps2dev automatically
```

`build.sh` first runs `../amiga/mkdata.py` + [mkfont_c.py](mkfont_c.py) to
emit `build/font_data.h` (the shared 8×8 font as a C array — same font every
other port consumes). [tools/ppm2png.py](tools/ppm2png.py) converts the host
shim's PPM dump to PNG with only the stdlib.

## M0 — hello-GS (this milestone)

- **Software framebuffer + primitives** (`fb.c`): fill / frame / invert /
  hline / vline / 8×8 text / scaled text, clipped, over a 640×448 RGBA32
  buffer. The 4-colour gamut (blue desktop, cyan + magenta accents, white).
- **Splash** (`uno_splash.c`): UnoDOS-blue desktop, menu bar, a double-framed
  title panel with the big "UnoDOS" wordmark, the palette swatches, and the
  arrow cursor — the M0 "hello-GS" screen, **rendered and screenshotted on
  the PC** (`shots/m0_splash.png`) so the FB + font pipeline is proven.
- **EE target** (`main.c`): GS init (640×448 NTSC interlaced, double
  buffered), FB→GS textured-sprite blit per vsync, DualShock 2 read via
  SIO2MAN+PADMAN moving the cursor. **Builds** to `build/unodos-ps2.elf`
  with the installed toolchain (gsKit/libpad linked) and **runs on the
  emulated GS** in PCSX2 (`shots/m0_pcsx2.png`); real-hardware run pending.

## M1 — the desktop (this milestone)

`mac/unodos.c` (4139 lines — the complete UnoDOS) runs on the PS2 through the
**Mac-compat shim**, host-verified and confirmed on the emulated GS.

- **Shim** (`mac_compat.*`, `mac_io.c`): one implicit full-screen GrafPort over
  `fb.*`; QuickDraw rect/oval/line/text, pen + colour + transfer-mode state, a
  platform-fed event queue, `TickCount`, `NewPtr`; File Manager over a directory
  tree; a square-wave `Snd*` channel model.
- **`unodos.c`**: the core, copied verbatim apart from the include block, the
  Pascal-literal → octal-length-byte fix, and the scheduler guard (68K coroutine
  `ctx_switch` under `__m68k__`; a portable kernel-driven scheduler elsewhere).
- **Verified:** desktop + all 11 apps (Sys Info, Clock, Files, Notepad, Music,
  Dostris, OutLast, Pac-Man, Tracker, Paint, Theme/32-bit colour) + the FAT12
  RAM-disk write→read round-trip into Notepad — `shots/m1_*.png` on the host,
  `shots/m1_pcsx2_pacman.png` on the emulated PS2 GS.

### Pad-as-pointer (EE button map — `ee_platform.c`)

| Input | Role |
|---|---|
| D-pad | arrow keys — desktop icon nav / in-app movement |
| Cross / Circle | Return — launch icon / confirm |
| Start | Esc — close focused window |
| left stick / Triangle (soft kbd) | reserved (M2) |

## Files

| File | Role |
|---|---|
| `fb.c` / `fb.h` | software framebuffer + drawing primitives (shared host+EE) |
| `mac_compat.h` / `.c` | **Mac Toolbox shim** — QuickDraw/events/memory over `fb.*` |
| `mac_io.c` | File Manager (directory tree) + Sound channel model |
| `unodos.c` | the portable UnoDOS core (copied from `mac/unodos.c`) |
| `host_desktop.c` | host shim: run the desktop, dump `fb` → PPM (host-only) |
| `ee_platform.c` | EE target: GS present + DualShock 2 → events |
| `uno_splash.c` / `main.c` | the M0 splash + its standalone EE target (reference) |
| `mkfont_c.py` | shared font → `build/font_data.h` |
| `tools/ppm2png.py` | PPM → PNG (stdlib only) |
| `tools/render_all.sh` | build + render every host AUTOTEST variant |
| `tools/run_pcsx2.ps1` | boot an ELF in PCSX2 + screenshot the GS |
| `Makefile` / `build.sh` | EE (PS2SDK) / host build |

## M2 — memory-card storage (done on the EE)

The EE File Manager persists to the **PS2 memory card** via libmc, so
Files/Notepad save and load on hardware and across boots — verified in PCSX2
(`shots/m2_pcsx2_mcsave.png` writes + reloads byte-for-byte; `m2_pcsx2_mcload.png`
loads it back on a fresh no-save boot). `ee_platform.c` brings up MCMAN/MCSERV +
`mcInit` + `/UnoDOS`; `mac_io.c`'s EE branch uses `mcOpen`(`sceMcFileCreateFile`)/
`mcRead`/`mcWrite`/`mcClose`/`mcDelete` + `mcGetDir`. (The host build keeps the
`uno_disk/` directory backend; the FAT12 RAM volume also works on both.)

## Next

- **M3 audio on the EE** — drive **audsrv** from `SndDoImmediate`
  (`mac_io.c`'s square-wave model is silent today). Theme (32-bit colour) and
  the cooperative scheduler already came along through the shim. Audio can't be
  screenshot-verified — it needs a hardware/audio ear-check, like the MacPlus
  SE audio.
- **USB keyboard** (`ps2kbd`) for real typing (the pad soft-keyboard is the
  fallback).
- **Real hardware** — run on a PS2 via FMCB (`BOOT.ELF` on the memory card, or
  uLaunchELF from USB) and confirm the DualShock 2 navigation on metal. The
  PCSX2-vs-metal watch list: interlace flicker (the 512×448 fallback in
  `fb.h`), memory-card timing, pad pressure quirks.

  FreeMcBoot launches `BOOT.ELF` from the memory card (or uLaunchELF from a USB
  stick); name `build/unodos-ps2.elf` accordingly when installing.
