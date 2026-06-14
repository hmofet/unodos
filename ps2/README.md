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

## Toolchain status on this dev machine (important)

This machine has **no PS2 toolchain and no emulator**: no Docker, no
ee-gcc/PS2SDK, no PCSX2, no PS2 BIOS. So the **EE ELF cannot be built or run
here** — [main.c](main.c) and the [Makefile](Makefile) are written to
PS2SDK/gsKit conventions but are **UNVERIFIED**.

What *is* verified is the handoff's **host shim** (HANDOFF §3, "the family's
fastest inner loop"): the software-FB code (`fb.c` + `uno_splash.c`) builds
with a normal host compiler and renders to a PNG. On this Windows box that
runs under **WSL** (Ubuntu 24.04: gcc 13 + python3 12). The EE target shares
`fb.c` + `uno_splash.c` *verbatim*, so everything the host shim proves —
font, palette, every drawing primitive, the whole splash — carries to the
PS2 unchanged; only the present-the-frame + input layers are EE-only.

## Building

```sh
# host splash (VERIFIED) - run in WSL: regenerates the font, compiles fb.c +
# uno_splash.c + host_main.c, renders shots/m0_splash.png
./build.sh host          # optional: ./build.sh host <cursor_x> <cursor_y>

# PS2 ELF (needs PS2SDK; UNVERIFIED here)
PS2SDK=/usr/local/ps2dev/ps2sdk ./build.sh ee   # -> build/unodos-ps2.elf
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
  SIO2MAN+PADMAN moving the cursor. Written, not yet built/run (no toolchain).

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
| `main.c` | EE target: GS blit + DualShock 2 (PS2-only, unverified) |
| `mkfont_c.py` | shared font → `build/font_data.h` |
| `tools/ppm2png.py` | PPM → PNG (stdlib only) |
| `Makefile` / `build.sh` | EE (PS2SDK) / host build |

## Next

- **M0 on metal (prerequisites, user-owned):** install PS2SDK (ps2dev
  Docker image or release binaries) to build the ELF; a **PCSX2 BIOS dump**
  to run it in the emulator (the user owns a PS2). Verify the PCSX2
  batch-launch + screenshot recipe and record it here, then confirm the
  splash renders on GS and the DualShock 2 moves the cursor.
- **M1 — the desktop:** copy `mac/unodos.c` → `ps2/unodos.c`, do the Toolbox
  audit (HANDOFF §1), and route its `uno_*` draw wrappers onto `fb.*`;
  events from the pad/soft-keyboard through its existing queue. The host
  shim makes this iterable on the PC before any PS2 hardware is involved.
- **M2** memory-card FAT12 + USB keyboard + Files/Notepad; **M3** audsrv
  sound + Theme (true 32-bit colour) + the cooperative scheduler.

## Real hardware

FreeMcBoot launches `BOOT.ELF` from the memory card (or uLaunchELF from a
USB stick). The PCSX2-vs-metal watch list: interlace flicker (the 512×448
resolution fallback in `fb.h`), memory-card timing, pad pressure quirks.
