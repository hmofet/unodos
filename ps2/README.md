# UnoDOS/PS2 — Sony PlayStation 2 port (milestone 0)

UnoDOS/PS2 is a FreeMcBoot-launched ELF with full hardware access —
"firmware-hosted bare-metal," the richest target in the family. The
strategy ([HANDOFF.md](HANDOFF.md), [../docs/PORTS-PLAN.md](../docs/PORTS-PLAN.md)
§4) is to **port the C core** in [../mac/unodos.c](../mac/unodos.c) — the
complete UnoDOS (11 apps, window manager, event model, cooperative
scheduler, device-abstracted FAT12) — by swapping the platform layer, not
rewriting.

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

**What still can't happen here: running it.** PCSX2 requires a 4 MB PS2 BIOS
and the BIOS folder provided (`C:\Users\arin\Downloads\Sony BIOS`) holds only
**512 KB PlayStation *1* BIOSes** (scph1001/5501/7502 — PS1 console models),
which PCSX2 rejects. So `main.c`'s **runtime behaviour on GS/pad is
UNVERIFIED** — to test it you need a 4 MB PS2 BIOS dump (e.g. `scph39001` ≈
4 MB) plus PCSX2, or real hardware via FMCB.

What *is* fully verified is the handoff's **host shim** (HANDOFF §3, "the
family's fastest inner loop"): the software-FB code (`fb.c` + `uno_splash.c`)
builds with WSL gcc 13 and renders to a PNG (`./build.sh host` →
`shots/m0_splash.png`). The EE target shares `fb.c` + `uno_splash.c`
*verbatim*, so everything the host shim proves — font, palette, every
drawing primitive, the whole splash — carries to the PS2 unchanged; only the
present-the-frame (GS) + input (pad) layers are EE-only and unrun.

## Building

```sh
# host splash (VERIFIED) - run in WSL: regenerates the font, compiles fb.c +
# uno_splash.c + host_main.c, renders shots/m0_splash.png
./build.sh host          # optional: ./build.sh host <cursor_x> <cursor_y>

# PS2 ELF (BUILDS; runtime unverified - no PS2 BIOS) -> build/unodos-ps2.elf
./build.sh ee            # PS2DEV defaults to ~/ps2dev/ps2dev; override to relocate
```

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
  with the installed toolchain (gsKit/libpad linked); runtime unverified
  pending a PS2 BIOS.

### Pad-as-pointer (M1 button map, stubbed in M0)

| Input | Role |
|---|---|
| D-pad / left analog stick | move the cursor |
| Cross | click / drag |
| Circle | Enter |
| Triangle | soft keyboard |
| Start | Esc |
| L/R shoulders | turbo cursor |

## Files

| File | Role |
|---|---|
| `fb.c` / `fb.h` | software framebuffer + drawing primitives (shared host+EE) |
| `uno_splash.c` | the M0 splash, drawn through `fb.*` (shared host+EE) |
| `host_main.c` | host shim: render → PPM (host-only) |
| `main.c` | EE target: GS blit + DualShock 2 (builds; runtime unverified, no BIOS) |
| `mkfont_c.py` | shared font → `build/font_data.h` |
| `tools/ppm2png.py` | PPM → PNG (stdlib only) |
| `Makefile` / `build.sh` | EE (PS2SDK) / host build |

## Next

- **Run M0 (blocked on a PS2 BIOS):** the ELF builds; running it needs a
  4 MB PS2 BIOS dump for PCSX2 (the provided folder had PS1 BIOSes only) or
  real hardware via FMCB. Then verify the PCSX2 batch-launch + screenshot
  recipe (HANDOFF §3) and confirm the splash on GS + the DualShock 2 cursor.
- **M1 — the desktop:** copy `mac/unodos.c` → `ps2/unodos.c` and route its
  platform layer onto `fb.*`. The audit (HANDOFF §1) is done: the drawing is
  ~25 QuickDraw calls (rects / pen / text / ovals via the 6 `uno_*` wrappers
  + scattered QD), plus `TickCount`, a couple of event calls, and File/Sound
  (deferrable to M2/M3). The plan is a small Mac-compat shim over `fb.*`
  (current-port colour/pen/text state, `PaintRect`/`FrameRect`/`MoveTo`/
  `LineTo`/`DrawText`/`InvertRect`/`PaintOval` → fb primitives) so `unodos.c`
  compiles nearly verbatim, driven by the **host shim** on the PC — no PS2
  hardware needed to bring up the desktop + apps; only the GS/pad/MC/audsrv
  glue waits on a BIOS or metal.
- **M2** memory-card FAT12 + USB keyboard + Files/Notepad; **M3** audsrv
  sound + Theme (true 32-bit colour) + the cooperative scheduler.

## Real hardware

FreeMcBoot launches `BOOT.ELF` from the memory card (or uLaunchELF from a
USB stick). The PCSX2-vs-metal watch list: interlace flicker (the 512×448
resolution fallback in `fb.h`), memory-card timing, pad pressure quirks.
