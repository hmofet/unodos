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

## §2 — What's done (at parity, emulator-verified)

- **Runs in Flycast** at native 640×480 / 60 fps, booted from `build/unodos-dc.cdi`
  via REIOS (HLE BIOS, no Sega BIOS file). Captured `shots/dc_*.png`: desktop +
  all 11 icons, Pac-Man, Dostris, Tracker, Paint, Theme, Files, OutLast, the
  Music+Files+Notepad stack.
- **VMU storage verified in-emulator**: the MCSAVE autotest types into Notepad,
  saves to `/vmu/a1`, clears, and reads back — the reloaded text appears
  (`shots/dc_vmu.png`). So Create/FSWrite/FSOpen/FSRead over the KOS `/vmu` VFS
  round-trip.
- **AICA audio wired** ([dc_main.c](dc_main.c) `uno_dc_snd_note/quiet`,
  [mac_io.c](mac_io.c) Sound Manager): a looping square-wave sample via `snd_sfx`,
  pitched per MIDI note; Music/Tracker/Dostris drive it. The build boots with
  audio live (`shots/dc_audio_boot.png`); the sound itself is an ear-check.
- **DC platform layer** ([dc_main.c](dc_main.c)): 640×480 RGB565 framebuffer
  present (fb → `vram_s` each vblank + cursor overlay); maple input (controller
  d-pad→arrows, A/Start→Return/Esc, analog stick + DC mouse → pointer, DC
  keyboard → text).
- **Build**: `sh-elf-gcc 15.2.0` + libkos built from source (`utils/kos-chain`);
  `build.sh dc` → ELF (SH-4, entry `0x8c010000`), `build.sh cdi` → `.cdi`
  (mkdcdisc), `build.sh iso` → selfboot ISO fallback. Host shim also verified
  (`shots/m1_*.png`).
- **Emulator rig**: [tools/emu_run.sh](tools/emu_run.sh) +
  [tools/capture_apps.sh](tools/capture_apps.sh) (Flycast under Xvfb + llvmpipe).

## §3 — Remaining / caveats

- **Real hardware** is the only unverified frontier (CD-R or dc-tool/BBA),
  including the audio ear-check. Everything emulator-verifiable is done.
- **Emulator rig flags that were load-bearing** (see README gotchas): Flycast
  needs `rend.EmulateFramebuffer = yes` (we draw straight to VRAM) AND
  `rend.vsync = no` (Xvfb reports 0 Hz, which otherwise gates frame swaps → black).
  A real Dreamcast / GPU display has neither issue.
- **Keyboard arrows** route through the controller d-pad today; a DC keyboard
  handles text entry but not its own arrow keys yet.
- **`main(void)` vs KOS `main(argc,argv)`** — KOS calls `main(argc,argv)`; the
  core's `main(void)` ignores the args (works on SH4, confirmed by the run).

## §4 — Next

1. Real hardware: burn `build/unodos-dc.cdi` to CD-R (or dc-tool/BBA), confirm
   boot + the audio ear-check.
2. Optional: PVR-textured-quad present (HW scaling; sidesteps the FB-emulation
   path in emulators); keyboard arrow routing.
