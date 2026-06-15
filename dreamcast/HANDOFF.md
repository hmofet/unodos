# UnoDOS/Dreamcast — handoff

Where the port stands and what to do next. Companion to [README.md](README.md).

## §1 — Strategy (same as PS2)

Port the portable C core [../mac/unodos.c](../mac/unodos.c) by swapping the
platform layer, not rewriting. The core + the **Mac-compat shim**
([mac_compat.*](mac_compat.h), [mac_io.c](mac_io.c)) over the software
framebuffer [fb.*](fb.h) are shared, byte-for-byte, with the PS2 port; only
[dc_main.c](dc_main.c) (KallistiOS) and [fb.h](fb.h)'s resolution differ.
`unodos.c` owns `main()`; built with `-DUNO_DC` it drives three hooks:
`uno_dc_init()` / `uno_dc_poll()` / `uno_dc_present()` (in [dc_main.c](dc_main.c)),
mirroring the PS2's `uno_ee_*`.

## §2 — What's done (M1 + M2)

- **Host shim VERIFIED** at 640×480: splash, the full desktop, the window
  manager, and every app render to PNGs (`shots/m0_splash.png`,
  `shots/m1_desktop.png`, `m1_pacman.png`, `m1_paint.png`, `m1_theme.png`,
  `m1_files.png`). Same code the DC ELF compiles.
- **DC platform layer written** ([dc_main.c](dc_main.c)): 640×480 RGB565
  framebuffer present (fb → `vram_s` each vblank + cursor overlay); maple input
  (controller d-pad→arrows, A/Start→Return/Esc, analog stick + DC mouse →
  pointer, DC keyboard → text).
- **M2 VMU storage** ([mac_io.c](mac_io.c) DC branch): flush-on-close RAM-buffer
  backend over `/vmu/a1` (whole-file save/load matches UnoDOS's app model and the
  VMU's block flash); `opendir`/`readdir` for the Files listing.
- **Build system**: [Makefile](Makefile) (KOS) + [build.sh](build.sh)
  (`host`/`desktop`/`dc`/`cdi`).

## §3 — What's NOT verified

[dc_main.c](dc_main.c) has **not been compiled or run** — no `sh-elf-gcc` /
KallistiOS / DC emulator on the dev machine (the PS2 port shipped its EE target
the same way before ps2dev arrived). Risks to check first when KOS is available:

- **kbd_queue_pop xlat semantics** — confirm printables come back as ASCII and
  specials don't collide; wire keyboard arrow keys if desired.
- **`/vmu/a1` VFS** — confirm `fopen("wb")` + `opendir`/`readdir` behave; a brand
  new VMU may need a format/free-block check. Watch the 128 KB / block limits.
- **Tearing** — the present copies during vblank but the 600 KB copy may overrun
  it; if so, double-buffer or move to a PVR-textured-quad present.
- **`main(void)` vs KOS `main(argc,argv)`** — KOS calls `main(argc,argv)`; the
  core's `main(void)` ignores the args (works on SH4, as it did on the EE).

## §4 — Next

1. Build with KallistiOS (`./build.sh dc`), fix compile issues, boot the `.cdi`
   in Flycast/lxdream/redream, capture `shots/m1_dc_*.png`.
2. M3 audio: wire the Sound Manager shim to the AICA via KOS `snd_*`.
3. Real hardware (CD-R or dc-tool/BBA).
4. Optional: PVR-textured-quad present; keyboard arrow routing.
