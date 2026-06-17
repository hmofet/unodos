# UnoDOS 3.1 — migration status & handoff

**Two lines, one history.** UnoDOS has forked into a stable legacy line and a
forward contract-driven line. This document is the handoff for resuming the
forward work in a new session.

## The fork

| Line | What it is | Git |
|---|---|---|
| **UnoDOS 3 Legacy** | The shipped, real-code-validated OS: x86 reference + the asm/C ports (Amiga, Genesis, MacPlus, Mac, SNES, IIGS, Apple II, C64, PS2, Dreamcast, 8088) with their emulator/hardware evidence. Frozen, known-good. | branch `unodos-3-legacy`, tag `legacy-pre-3.1` (commit `2f0f261`) |
| **UnoDOS 3.1 (forward)** | The Contract-Driven Architecture: one machine-readable contract (`unodef/`) that every world is generated from or checked against, plus the new portable subsystems. | branch `master` (commits `55d5a7f … `) |

The legacy commit is the parent of all 3.1 work, so history is shared. To get
back to pure legacy at any time: `git checkout unodos-3-legacy` (or
`git checkout legacy-pre-3.1`).

## What UnoDOS 3.1 is (the design)

Per [CONTRACT-ARCH.md](CONTRACT-ARCH.md): invert PORT-SPEC from "prose extracted
from x86" into a single **machine-readable Contract** (`UNODEF`); a host generator
(`unogen`) emits per-world stubs/tables/struct-equates/constants from it; x86 is
demoted from "the definition" to "first consumer + conformance oracle." Four
layers: L0 Contract · L1 mechanism+drivers · L2 portable policy · L3 apps. The
universal boundary is **tall** (software floor + optional hardware-accel overrides).
The unit of "world" is the **subsystem**, not the port.

## What exists now (forward line)

```
unodef/                 the Contract (Layer 0) + tooling
  unodef.toml           THE single source of truth (106 syscalls, structs, enums,
                        FAT12 geometry, font, palette, profiles/caps/surfaces,
                        per-world [world.*] variants, abi31 scheme, .UNO v2 header)
  unogen.py             emits 6 worlds (x86/C/68k/6502/65816/z80) + app stubs +
                        profile manifest + 3.1 ordinal map; `--check` = trust anchor
  wmgen.py              GREENFIELD window model: derives a per-platform physical
                        window layout (SoA/AoS, widths, accessors) from the logical
                        [wmodel] — the 3.1 window ABI engine. -> gen/wm/<platform>/
  WMODEL.md             the window-model RFC + verification + 40-yr arch survey
  conformance/          PORT-SPEC §6 made executable (52/52, with bug-discrimination)
  gen/                  GENERATED per-world surfaces (do not edit); gen/wm/ = windows
  PHASES.md             the live phase-by-phase status ledger
unofs/                  storage policy over a `block` service (FAT12)         [§12]
uno2d/                  the tall 2D Primitive Vtable (floor + accel)          [§5/§6]
unosched/               COOP/SMP concurrency + sync + OFFLOAD job model       [§10]
unosound/               voice/score audio floor → PCM/WAV                     [§6]
unobus/                 service registry: enumerate → bind → register         [§7]
unonet/                 nic service + HEADLESS server composition             [§13]
```

Wired into shipped code (sourcing constants from the Contract, byte-identical):
`kernel/kernel.asm`, `boot/boot.asm`, `boot/stage2.asm`, `tools/add_floppy_fs.py`,
`tools/create_app_test.py`, `amiga/sysabi.i`, `snes/kernel.asm`,
`genesis/kernel.asm`, `macplus/kernel.asm` + `macplus/sysequ.i`, `iigs/sys.inc`,
`c64/sys.inc`, `apple2/sys.inc`.
(All seven reachable-toolchain asm ports now, across three dialects: vasm 68K =
amiga/genesis/macplus, ca65 65816 = snes/iigs, dasm 6502 = c64/apple2. IIGS also
single-sources its divergent FAT12 geometry + 16-byte directory entry. The 6502
ports are `single_app` — no window-entry struct / event queue by design — so they
source only the genuine overlap, the cell screen geometry; their no-WM shape is
Contract-declared via `[port.c64]`/`[port.apple2]` + conformance-checked.)

### New ports built FRESH on the 3.1 architecture
Two ports were then built from scratch on the contract-driven + greenfield-window
architecture (not migrated from legacy) — the proof that a new target costs a
small, generated surface:

```
sms/    Sega Master System (Z80, sjasmplus)  -- M1 desktop + M2 WM + M3 apps
                                                 + Dostris game + PSG audio
nes/    Nintendo NES (6502/2A03, dasm)        -- M1 launcher + M2 directional nav
                                                 + M3 apps + Dostris game + APU audio
gb/     Game Boy / Color (Sharp SM83, rgbds) -- M1 list launcher + M2 directional nav
                                                 + M3 apps + Dostris + APU; DMG+GBC colour
```

- **SMS** consumes `gen/z80/` + `[world.sms]` (16 B window entry, 256×192 Mode 4):
  a VBlank cooperative loop, a hardware-sprite cursor, the Contract event queue,
  a tile window manager (create/draw/raise/**drag**/close, z-order), apps (SysInfo,
  live Clock, Notepad, Files, Theme-cycles-CRAM-live, Music), a playable **Dostris**
  (Tetris), and **SN76489 PSG** audio. Verified in BlastEm via AUTOTEST scripted-pad
  builds (`sms/build/{desktop,wm,dostris,music}.png`).
- **NES** is the Contract's **`minimal` profile** flagship (2 KB RAM, no WM,
  directional nav — the other end of the profile spectrum). It consumes `gen/6502/`
  + `[world.nes]` (256×240 PPU), reusing the dasm toolchain. **M1** launcher
  (inverted title bar + 11 icons in CHR-ROM); **M2** the standard pad on `$4016` +
  a vblank-NMI loop + a directional selection highlight (A launches full-screen, B
  returns — the §8 pointer-less model, *not* a WM); **M3** full-screen apps
  (SysInfo, live Clock, Notepad, Files, Theme-cycles-palette, Music on the 2A03 APU)
  + a from-scratch **Dostris** (the SMS falling-blocks algorithm ported to 6502). A
  PPU-free NMI + main-loop vblank partials keep updates flicker-free. Verified in
  Mesen2 (`nes/build/{desktop,nav,app,clock,dostris,theme,music}.png`).
- **Game Boy / Color** is the **first Sharp-SM83 (`gbz80`) world** — a genuinely new
  unogen dialect (rgbds, `DEF NAME EQU value`), consuming `gen/gbz80/` + `[world.gb]`.
  Also `minimal` profile (8 KB RAM, no WM), but the launcher is a **vertical list**
  with 8×8 mini-icons (the 160×144 LCD suits a list, not a grid — the same Contract,
  a port-chosen layout). M1 launcher; M2 the joypad on `$FF00` + a vblank-interrupt
  loop + an Up/Down selection highlight (A launches full-screen, B returns); M3 apps
  (SysInfo, live Clock, Notepad, Files, Theme-cycles-BG-palette, Music on the GB APU)
  + a from-scratch **Dostris** (the algorithm ported to SM83). One ROM = greyscale on
  DMG (BGP) / colour on GBC (BG palette RAM), detected at boot. Verified in Mesen2/GBC
  (`gb/build/{desktop,nav,app,clock,dostris,theme,music}.png`).

Together they exercise both ends of `unogen`'s reach: a **window-profile** Z80 port
(reusing the existing dialect) and a **minimal-profile** 6502 port (reusing the
6502 dialect) — neither needed a new generator dialect.

## Verification (host-first, all green together)

```
python unodef/unogen.py --check            # regenerate all worlds + x86 trust anchor
python unodef/conformance/conformance.py   # 43/43 PORT-SPEC §6 vectors
# byte-identical rebuilds (constants now come FROM the contract):
nasm  → kernel.bin, boot.bin, stage2.bin   (x86, three sites)
vasm  → amiga + genesis + macplus kernels  (68K, three ports)
ca65+ld65 → snes .sfc + iigs .po           (65816, two ports)
dasm  → c64 .prg/.d64 + apple2 .dsk        (6502, two ports — single_app)
# host C subsystems:
sh unofs/build.sh ; sh uno2d/build.sh ; sh unosound/build.sh
sh unobus/build.sh ; sh unonet/build.sh ; sh unosched/build.sh   # TSan needs `setarch -R`
```

The FAT12 geometry "five places" (PORT-SPEC §5) is now genuinely **one** place.
Generated equates are proven byte-identical across **three asm families** (nasm,
vasm, ca65) + the C header (`_Static_assert`s).

## Phase status (full detail in `unodef/PHASES.md`)

- **Fully host-proven (8):** 0 UNODEF · 1 unogen + x86 byte-identical · 2 conformance
  · 3 unofs · 4 asm consumption (**all 7 asm ports**: Amiga + Genesis + MacPlus via
  vasm, SNES + IIGS via ca65, C64 + Apple II via dasm) · 6 uno2d · 7 concurrency/SMP/
  TSan · 9 unosound.
- **Host core + hardware/SDK-blocked tail (4):** 5 hybrid policy (needs vbcc/WinUAE)
  · 8 display/profiles (now also *emulator-proven*: the SMS windowed desktop +
  the NES `minimal` directional-nav launcher + apps, see below) · 10 SMP/OFFLOAD pilots (Saturn/PS3) · 11
  drivers/buses (PCI/USB) · 13 new targets + networking — **three fresh ports landed**
  (SMS Z80, NES 6502, Game Boy SM83 — all M1–M3 + game + audio), console SDK backends
  still blocked.
- **Phase 12 (ship 3.1 ABI): PILOTED + SHIPPED on x86** — the greenfield window
  model + the clean 16 B `win_entry`, QEMU + real-hardware + cycle-accurate-8088
  validated. The other windowing ports already run the compact 16 B layout. (Event
  record + file_handle remain candidates for the same logical-model treatment.)

## The 3.1 window ABI — DECIDED + piloted (was the one open decision)

The ports shipped genuinely divergent window ABIs (x86's 32-byte entry vs the
compact 16-byte entry the others use). The resolution was **neither "32 nor 16"**:
a **greenfield window model** (`unodef/WMODEL.md`, `unodef/wmgen.py`, `[wmodel]`) —
one *logical* model (named fields + kinds + tier + relations + invariants, **no
offsets**) from which the *physical* layout is **derived per platform** (SoA floor /
AoS / field widths / capacity) and reached only through generated **zero-cost
accessors** keyed by an integer handle. The tall-vtable / "describe what ships"
principle applied to data. Verified across **9 platforms** (`gen/wm/ARCHITECTURES.txt`),
wired into all 6 windowing ports' addressing (byte-identical), with a 40-year
architecture survey (ARM/AArch64/SPARC/Alpha/MIPS/PPC/RISC-V/SH).

**Phase 12 piloted on x86 and shipped:** the Contract's `win_entry` is now the
**clean 16-byte layout** (pointer title into a kernel pool + compact fields; the
inline `char[12]` is gone). The kernel needed *no code edits* (it consumes
`WIN_OFF_*`/`WIN_ENTRY_SIZE` symbolically + the `win_entry_addr` stride macro), so
the 32→16 B break was a regenerate-only Contract change. **QEMU-verified, validated
on the user's real x86 laptop, and on a cycle-accurate 8088 (MartyPC).** A CGA
save-under cursor fix shipped alongside. The other ports already use the compact
16 B layout, so x86 was the only outlier to migrate.

## Toolchains (this environment)

- Reachable: `nasm` (x86), `vasmm68k_mot` (68K), `ca65/ld65` (65816), `dasm` (6502),
  WSL `gcc` incl. **ThreadSanitizer** (needs `setarch -R`).
- Blocked here: `vbcc` (68K C), the console SDKs (KOS/ee/devkit*/PSL1GHT/libyaul/…),
  and all emulators.

## Next directions (prioritized, for the new session)

1. ~~**Broaden asm consumption** to the remaining reachable ports: Genesis + MacPlus
   (vasm), IIGS (ca65), C64 + Apple II (dasm).~~ **DONE** — all wired, byte-identical
   (incl. disk apps + packed images); IIGS also sources its own FAT12 geometry. The
   6502 ports turned out NOT to need the feared "deeper refactor": investigation showed
   they're architecturally `single_app` (no window-entry struct, no event queue — one
   disk-loaded app at a time), so forcing a windowed window-entry ABI on them would FAKE
   an ABI they don't ship. Instead they source the genuine overlap (cell screen geometry)
   byte-identically, and their no-WM shape is now Contract-declared (`[port.c64]`/
   `[port.apple2]`, profile=single_app) + conformance-checked (rule 9). **All 7 reachable
   asm toolchains now consume the Contract.** The only un-consumed asm tail left is the
   blocked-toolchain ports (need vbcc / console SDKs) — see #3.
2. ~~**Decide the canonical 3.1 ABI**, then pilot Phase 12 end-to-end on one port.~~
   **DONE** — greenfield window model decided; x86 piloted the clean 16 B layout,
   QEMU + real-hardware + cycle-accurate-8088 (MartyPC) validated. See "The 3.1
   window ABI — DECIDED" above.
3. ~~**NEW PORT: Sega Master System (Z80).**~~ **DONE** — the first port built fresh
   on the contract-driven + greenfield-window architecture. Consumes `gen/z80/` +
   `[world.sms]` via sjasmplus. **M1** desktop, **M2** window manager (sprite cursor,
   Contract event queue, create/draw/raise/drag/close, z-order) + d-pad input, **M3**
   apps (SysInfo, live Clock, Notepad, Files, Theme, Music) + a playable **Dostris**
   game + **PSG audio** — all BlastEm-verified via AUTOTEST scripted-pad builds. See
   [../sms/README.md](../sms/README.md). (Game Boy is a *separate* CPU — Sharp
   LR35902 — needs rgbds + a new `gbz80` dialect; deferred until that toolchain is in.)
4. ~~**NEW PORT: Nintendo NES (6502/2A03).**~~ **M1–M3 + game + audio DONE** — the
   Contract's `minimal` profile flagship (2 KB RAM, no WM, directional nav). Consumes
   `gen/6502/` + `[world.nes]` via dasm. M1 launcher; M2 `$4016` pad + a vblank-NMI
   loop + a directional selection highlight (A launches full-screen, B returns); M3
   full-screen apps (SysInfo, live Clock, Notepad, Files, Theme, Music on the 2A03
   APU) + a from-scratch **Dostris**. Mesen2-verified via AUTOTEST scripted-pad
   builds. See [../nes/README.md](../nes/README.md). NEXT on NES: real hardware.
5. ~~**NEW PORT: Game Boy / Game Boy Color (Sharp SM83).**~~ **M1–M3 + game + audio
   DONE** — the FIRST `gbz80` world (a genuinely new unogen dialect, rgbds). `minimal`
   profile with a **vertical-list** launcher (the 160×144 LCD suits a list). M1
   launcher; M2 `$FF00` joypad + a vblank-interrupt loop + an Up/Down highlight; M3
   apps (SysInfo, live Clock, Notepad, Files, Theme, Music on the GB APU) + a
   from-scratch **Dostris**. One ROM = greyscale on DMG / colour on GBC. Mesen2/GBC-
   verified via AUTOTEST. See [../gb/README.md](../gb/README.md). NEXT on GB: real hw.
6. **More fresh ports / the next CPU family.** Game Gear (≈SMS hardware, reuses the
   sms/ code) for a quick win; VIC-20 (6502, `minimal`) reuses the dasm path; ARM
   (GBA, devkitARM) would add the next genuinely new ISA once that SDK is installed.
7. **Generalize the greenfield model to the next subsystem** — the event record
   (x86 3 B vs asm 4 B) and file_handle diverge across ports exactly like windows did;
   bring them under the logical-model + derived-layout treatment (host-verifiable).
8. **Reach a blocked tail** when a toolchain is available: vbcc for Phase 5 (Amiga
   `unofs_core` + trackdisk + WinUAE); a console SDK for a C-world uno2d/unosound
   backend. Also: retarget `unoui` onto `uno2d` + Amiga blitter; rule-4 conformance
   vectors.

### Screenshots / verification on this machine (RDP-aware)
GUI focus + SendKeys capture fails over RDP. Use a tool's control channel (QEMU
monitor `screendump` — `tools/qemu_test.py`) or the focus-independent helper
`%USERPROFILE%\.claude\tools\cc-capture.ps1` (`-Out f.png [-Window <substr>]`). See
`~/.claude/CLAUDE.md` (loads in every session). MartyPC (cycle-accurate 8088,
`xt-tools/`, harness `tools/xt/shot_xt.ps1`) needs a windowed launch +
ShowWindow-restore, then cc-capture (not its Ctrl+F5). QEMU at
`C:\Program Files\qemu\qemu-system-i386.exe`.

## Resume checklist

```
git checkout master
python unodef/unogen.py --check && python unodef/conformance/conformance.py  # trust anchor + 52/52
python unodef/wmgen.py        # regenerate the greenfield window layouts (gen/wm/)
cat unodef/PHASES.md ; cat unodef/WMODEL.md ; cat unodef/gen/wm/ARCHITECTURES.txt
# x86 bootable image (clean 16B layout + CGA cursor fix), QEMU-verified:
PATH=~/AppData/Local/bin/NASM:$PATH make floppy144   # -> build/unodos-144.img
# build/run the fresh ports:
sh sms/build.sh && powershell -File sms/run.ps1   # SMS (M1-M3 + game + audio), BlastEm
sh nes/build.sh && powershell -File nes/run.ps1   # NES (M1-M3 + game + audio), Mesen2
sh gb/build.sh  && powershell -File gb/run.ps1    # Game Boy (M1-M3 + game + audio), Mesen2/GBC
# pick a "Next directions" item above and go. (the next CPU family, or real hw.)
```
