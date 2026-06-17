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
gg/     Sega Game Gear (Z80, sjasmplus)      -- SMS silicon + the GB minimal layout;
                                                 M1-M3 + Dostris + PSG; 12-bit colour
gba/    Game Boy Advance (ARM7TDMI, GNU as)  -- FIRST ARM world; Mode-3 framebuffer;
                                                 M1-M3 + Dostris + APU; Unicorn-verified
vic20/  Commodore VIC-20 (6502, dasm)        -- 22x23 char-cell list launcher;
                                                 M1-M3 + Dostris + VIC tone; py65-verified
ws/     Bandai WonderSwan (NEC V30MZ, nasm)  -- FIRST x86 handheld; SCR1 tile launcher;
                                                 M1-M3 + Dostris + PSG; Unicorn-verified
pce/    NEC PC Engine (HuC6280, ca65)        -- FIRST HuC6280 world; VDC tile launcher;
                                                 M1-M3 + Dostris + PSG; py65+HuC6280-verified
rpi/    Raspberry Pi (ARM Cortex-A, GNU as)  -- FIRST AArch64 (64-bit) world; VideoCore
                                                 mailbox framebuffer; M1-M3 + Dostris + PWM;
                                                 Unicorn-AArch64-verified
pinephone/ PinePhone (Allwinner A64, GNU as)  -- reuses the rpi AArch64 core; DE2 mixer UI
                                                 layer, portrait 480x640; M1-M3 + Dostris;
                                                 Unicorn-AArch64-verified
ppcmac/ PowerPC Mac (32-bit PowerPC, GNU as)  -- FIRST PowerPC (big-endian) world; boots
                                                 over Open Firmware (client interface);
                                                 M1-M3 + Dostris; Unicorn-PPC-verified
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
- **Game Gear** is a study in *reuse*: SMS silicon (the same Z80 + 315-5124 VDP), so
  it consumes the same `gen/z80/` world and reuses the SMS hardware bring-up, 4bpp
  tiles, SN76489 PSG, and Dostris — but its LCD shows only the centre 160×144 = 20×18,
  exactly the Game Boy's panel, so it wears the **GB's `minimal` layout** (a vertical
  mini-icon list, drawn at a (6,3) offset). SMS hardware, Game-Boy-sized screen. The
  one hardware delta vs. the SMS is 12-bit CRAM colour. M1–M3 + Dostris (full colour)
  + PSG, Mesen2/GG-verified (`gg/build/{desktop,nav,app,clock,dostris,theme,music}.png`).

- **Game Boy Advance** is the **first ARM world** — a genuinely new unogen dialect
  (`arm`/GNU as, `.equ NAME, val`), consuming `[world.gba]`. The GBA hardware exceeds
  the minimal profile, but this is a `minimal` UnoDOS instance drawn into a flat
  **Mode-3 framebuffer** (240×160, 16bpp) with no hardware tiles — an 8×8 font + 16×16
  icons plotted pixel-by-pixel, each pixel's palette index in a 16-entry IWRAM table
  (Theme swaps the table). M1 launcher (4-col icon grid); M2 `REG_KEYINPUT` + a
  VCOUNT-polled loop + a d-pad highlight; M3 apps + Dostris on the square channel.
  mGBA's deferred-GL display won't grab under RDP, so it is verified on a **Unicorn
  ARM7TDMI** core running the real ROM (`gba/build/{desktop,nav,app,clock,dostris,theme,music}.png`).
- **VIC-20** reuses `gen/6502/` + dasm (like the NES) as a `minimal` **character-cell**
  launcher: the VIC 22×23 matrix at `$1E00` + colour RAM `$9600` + a custom charset at
  `$3000`, with an inverted charset for the title bar + selection. M1 mini-icon list;
  M2 the joystick on `$9111`/`$9120` + a raster-synced (`$9004`) loop + Up/Down highlight;
  M3 apps (live Clock, Theme-cycles-`$900F`-bg, Music on the VIC oscillator) + Dostris in
  colour. Verified on a **py65** ROM-free 6502 core
  (`vic20/build/{desktop,nav,app,clock,dostris,theme,music}.png`).
- **WonderSwan** is the **first x86 handheld**: its NEC V30MZ is an 80186-class CPU — the
  same family as the reference kernel — so it is built with **nasm** (16-bit real mode)
  and consumes the Contract's x86 surface + `[world.ws]`. A `minimal` **hardware-tile**
  launcher: 8×8 2bpp planar tiles in the 16 KB internal RAM (`0x2000`) + a 32×32 SCR1
  tilemap (`0x0800`), 224×144 mono LCD; tile colour resolves per-tile-palette → mono
  shade pool (ports `0x1C-0x1F`), so Theme recolours everything by rewriting the pool.
  M1 launcher (inverted title bar + 11-icon list); M2 the keypad on port `0xB5`
  (group-select) + a line-counter (`0x03`) loop + Up/Down highlight; M3 apps (live Clock,
  Theme-cycles-the-pool, Music on sound channel 1) + Dostris with a tile well. Verified on
  a **Unicorn x86/V30MZ** core that maps the ROM flush to `0xFFFFF` so the reset vector
  `0xFFFF0` runs the genuine JMP-FAR boot path
  (`ws/build/{desktop,nav,app,clock,theme,music,dostris}.png`).
- **PC Engine** is the **first HuC6280 world** (a 65C02 superset; `ca65 --cpu huc6280`),
  consuming `[world.pce]`. `minimal`: 256×224 = 32×28 BAT cells, so it reuses the NES's
  4-column icon-grid launcher + shared 6502 app/Dostris logic with the HuC6270 VDC as the
  draw layer (8×8 4bpp tiles to VRAM `$1000`; BAT entry `$0100+N`; 16-colour VCE palette,
  9-bit `GGGBBBRRR`, recoloured by Theme). The HuC6280 MMU maps the logical space via 8 MPR
  banks. M1 launcher; M2 joypad on `$3000` + a VDC-vblank loop + a directional highlight; M3
  apps (live Clock, Theme-cycles-VCE-palette, Music on the PSG) + Dostris. Mesen renders the
  PCE through a GPU surface a GDI grab reads as black, so it is verified on a **ROM-free
  HuC6280 harness** (py65 65C02 + the TAM/CSH opcodes + the MMU + a VDC/VCE model)
  (`pce/build/{desktop,nav,app,clock,theme,music,dostris}.png`).
- **Raspberry Pi** is the **first AArch64 (64-bit) world** — the same `aarch64`/GNU-as
  (GAS) dialect, but a genuinely new register width over the GBA's 32-bit ARM7TDMI (no
  conditional execution → `csel`, `stp`/`ldp` frames, 64-bit FB pointers), consuming
  `[world.rpi]`. `minimal`: there is no fixed framebuffer, so at boot the kernel asks the
  **VideoCore firmware over the mailbox property channel** (`0x3F00B880`) for a 640×480
  32bpp XRGB surface and plots an 8×8 font + 16×16 icons pixel-by-pixel (palette-index, so
  Theme recolours by swapping the table); frames are paced off the **BCM system timer**.
  M1 launcher (4-col icon grid); M2 a timer-paced loop + a d-pad highlight (A launches, B
  returns); M3 apps (live Clock, Theme-cycles-palette, Music on the PWM headphone jack) +
  Dostris. A real Pi renders to an HDMI surface no headless grab can read, so it is verified
  on a **Unicorn AArch64** core that emulates the mailbox + system-timer MMIO and renders the
  firmware-allocated framebuffer (`rpi/shots/{m1_boot,m2_nav,m3_sysinfo,m3_clock,m3_theme,m3_music,m3_dostris}.png`).
- **PinePhone** is the **second AArch64 world** — it reuses the Pi's AArch64 core (same GAS
  dialect, primitives, Dostris + app logic), retargeted to the **Allwinner A64** SoC and a
  **portrait** 480×640 panel, consuming `[world.pinephone]`. The A64 has no GPU mailbox, so
  (assuming the SPL/U-Boot stage brought up DRAM + the TCON0/MIPI-DSI panel clocks, as the Pi
  assumes the VideoCore firmware) the kernel programs the **Display Engine 2.0 mixer UI layer**
  to scan out an XRGB8888 framebuffer at `0x40400000`; frames pace off the **ARM generic timer**
  (`cntpct_el0`, no MMIO). M1 launcher; M2 d-pad highlight; M3 apps (live Clock, Theme, Music
  UI) + Dostris. Verified on a **Unicorn AArch64** core (DRAM + a DE2 RAM sink; the generic
  timer advances on its own) rendering the DE2 framebuffer
  (`pinephone/shots/{m1_boot,m2_nav,m3_sysinfo,m3_clock,m3_theme,m3_music,m3_dostris}.png`).
- **PowerPC Mac** is the **first PowerPC (big-endian) world** — a brand-new ISA needing
  a new `ppc`/GNU-as dialect (`#` line comments) and a from-scratch harness, consuming
  `[world.ppcmac]`. It is also the first port to boot over **Open Firmware** (no Mac OS):
  OF enters `_start` with the IEEE-1275 client-interface entry in r5, and the kernel makes
  OF client calls — `finddevice "screen"`, then `getprop "address"` + `"linebytes"` — to
  obtain a 640×480 32bpp linear framebuffer, then draws into it directly (big-endian, so a
  `stw` of `0xFFRRGGBB` lands as `FF RR GG BB`). M1 launcher; M2 d-pad highlight; M3 apps
  (live Clock, Theme, Music UI) + Dostris. Verified on a **Unicorn PPC32 big-endian** core
  that emulates the OF client interface (r5 → a `blr` trampoline; a code hook services
  `finddevice`/`getprop`) and renders the framebuffer
  (`ppcmac/shots/{m1_boot,m2_nav,m3_sysinfo,m3_clock,m3_theme,m3_music,m3_dostris}.png`).
  The one bring-up trap: in PowerPC load/store addressing, `r0` in the base position means
  *literal 0*, so the address-load macros must never target `r0`.

Together they exercise the full span of `unogen`'s reach: a **window-profile** Z80 port,
**minimal-profile** ports on 6502, SM83, Z80, **ARM** (a new dialect), **x86**, and the
**HuC6280** — four of them (GBA, VIC-20, WonderSwan, PC Engine) verified on a **ROM-free
instruction-level harness** (Unicorn ARM / py65 / Unicorn x86 / py65+HuC6280) running the
real ROM, the pattern for any target whose emulator can't be captured headlessly under RDP.

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
  drivers/buses (PCI/USB) · 13 new targets + networking — **four fresh ports landed**
  (SMS Z80, NES 6502, Game Boy SM83, Game Gear Z80 — all M1–M3 + game + audio), console
  SDK backends still blocked.
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
6. ~~**NEW PORT: Sega Game Gear (Z80).**~~ **M1–M3 + game + audio DONE** — SMS silicon
   reusing `gen/z80/` + the SMS hardware bring-up / 4bpp tiles / PSG / Dostris, but the
   GB's `minimal` 160×144 layout (drawn at a (6,3) offset; 12-bit CRAM the one delta).
   Mesen2/GG-verified. See [../gg/README.md](../gg/README.md).
7. ~~**NEW PORT: Game Boy Advance (ARM7TDMI).**~~ **M1–M3 + game + audio DONE** — the
   FIRST ARM world (a new `arm`/GNU-as dialect), a `minimal` Mode-3 software framebuffer.
   mGBA won't grab under RDP, so it is verified on a **Unicorn ARM7TDMI** core running the
   real ROM. See [../gba/README.md](../gba/README.md). NEXT on GBA: real hw + audio-ear.
8. ~~**NEW PORT: Commodore VIC-20 (6502).**~~ **M1–M3 + game + audio DONE** — reuses the
   `gen/6502/` + dasm path as a 22×23 character-cell list launcher; **py65**-verified. See
   [../vic20/README.md](../vic20/README.md). NEXT on VIC-20: real hw / VICE.
9. ~~**NEW PORT: Bandai WonderSwan (NEC V30MZ).**~~ **M1–M3 + game + audio DONE** — the
   FIRST x86 handheld (V30MZ ≈ 80186, nasm, the Contract's x86 surface), a hardware-tile
   launcher. Verified on a **Unicorn x86** core that runs the genuine reset-vector boot
   path. See [../ws/README.md](../ws/README.md). NEXT on WonderSwan: real hw + audio-ear.
10. ~~**NEW PORT: NEC PC Engine (HuC6280).**~~ **M1–M3 + game + audio DONE** — the FIRST
    HuC6280 world (65C02 superset, `ca65 --cpu huc6280`), a VDC tile launcher. Mesen's GPU
    surface won't grab under RDP, so it is verified on a **ROM-free HuC6280 harness** (py65 +
    the TAM/CSH opcodes + the MMU + a VDC/VCE model). See [../pce/README.md](../pce/README.md).
    NEXT on PCE: real hw + audio-ear.
11. **More fresh ports / the next CPU family.** The next genuinely new ISA (e.g. a deeper
    ARM/RISC-V or PowerPC target) follows once its SDK is installed.
12. **Generalize the greenfield model to the next subsystem** — the event record
   (x86 3 B vs asm 4 B) and file_handle diverge across ports exactly like windows did;
   bring them under the logical-model + derived-layout treatment (host-verifiable).
13. **Reach a blocked tail** when a toolchain is available: vbcc for Phase 5 (Amiga
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
sh gg/build.sh  && powershell -File gg/run.ps1    # Game Gear (M1-M3 + game + audio), Mesen2/GG
# pick a "Next directions" item above and go. (the next CPU family, or real hw.)
```
