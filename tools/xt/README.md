# UnoDOS 8088 validation rig (MartyPC)

The x86 reference build of UnoDOS *is* the 8088 target — but for most of its
life it was only ever **run** on QEMU, which emulates a 486-class CPU and
silently hides every genuine 8088 / IBM PC-XT behaviour (186+/386+ opcodes
that decode as `JO`/`POP CS`/`RET imm16` on real silicon, the XT 8255 PPI
keyboard controller vs. the AT 8042, CGA snow, 4.77 MHz timing, the real RAM
envelope, serial vs. PS/2 mouse). The 2026-06 audit made the whole floppy
boot chain + kernel + apps assemble under `cpu 8086`, but that was only ever
*assembler-verified*.

This rig is the **"real emulator" tier** for the 8088 port:
[MartyPC](https://github.com/dbalsom/martypc), a cycle-accurate 8088 emulator
validated against real silicon (via the Arduino8088 project). It boots the
open-source **GLaBIOS** (no proprietary IBM ROMs needed — the same "ROM-free
harness first" house rule the macplus/Genesis/SNES ports follow).

## Install

MartyPC 0.4.1 (win64) lives at `C:\Users\arin\xt-tools` (download from the
[releases page](https://github.com/dbalsom/martypc/releases) and unzip there).
The UnoDOS machine configs are committed into the rig:

- `configs/machines/unodos_xt.toml` — `unodos_xt` (640K IBM 5160 / XT, CGA,
  1.44M floppies, Microsoft serial mouse on COM1) and `unodos_xt_256k`
  (256K IBM 5150-class, for RAM-floor probing).
- `configs/machines/config_overlays.toml` — adds the `pcxt_2_1440k_floppies`
  overlay (the stock overlays only define 360K/720K drives; the XT is given a
  1.44M drive so it can boot the project's primary `build/unodos-144.img`).

If you reinstall MartyPC, re-copy those two config additions from this repo's
`tools/xt/martypc/` mirror.

## Capture harness

`shot_xt.ps1` boots `build/unodos-144.img` on the emulated XT and grabs
**MartyPC's own framebuffer screenshots** (the `Ctrl+F5` event). MartyPC renders
the PNG from the emulated CGA framebuffer, so capture is clean under RDP — it
sidesteps the GPU-window-grab-is-black trap the SNES/Mesen rig hit.

```powershell
# Boot and grab the splash, the desktop, and the SysInfo window
tools\xt\shot_xt.ps1 -Waits 22,32,43 -Prefix boot

# Send keystrokes to the machine before a capture (launch the first icon)
tools\xt\shot_xt.ps1 -Waits 33,40 -Prefix kbd -Keys @("","{ENTER}")

# Probe the 256K RAM floor
tools\xt\shot_xt.ps1 -Waits 16,30,40 -Prefix ram256 -Machine unodos_xt_256k
```

PNGs land in `build/xt/<prefix>_<wait>s.png`. Canonical milestone shots are
committed under `tools/xt/shots/`.

### Why the waits are long

On a real 4.77 MHz 8088 with BIOS INT 13h floppy timing, loading the 104-sector
kernel and painting the CGA desktop genuinely takes ~30 s — that is the M3
performance target (the `draw_char` CGA row-blit fast path), not a rig bug.

## Status

See [docs/PORT-8088.md](../../docs/PORT-8088.md) for the milestone plan and
findings.
