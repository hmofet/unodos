# UnoDOS 3.1 — phase status ledger

Tracks CONTRACT-ARCH §16 phases. **Host-proven** = built/ran/verified in this
environment. **Blocked** = needs a toolchain/emulator/hardware not reachable here
(documented, not faked). Legacy restore point: tag `legacy-pre-3.1` (commit 2f0f261).

| Phase | What | Status |
|---|---|---|
| 0 | Author UNODEF | ✅ host-proven — `unodef/unodef.toml` parses; 106 syscalls, structs, enums |
| 1 | unogen MVP + trust anchor | ✅ host-proven — x86 kernel **byte-identical**; FAT12 "five places" (kernel+boot+stage2+2 tools) all single-sourced |
| 2 | Executable conformance | ✅ host-proven — `conformance.py` 29/29, discrimination vs historical bugs |
| 3 | `unofs` worked example | ✅ host-proven — reads the real floppy byte-identical; write/reap round-trip |
| 4 | Asm consumption | ✅ host-proven — **all 7 reachable-toolchain asm ports** byte-identical via per-world equates: **vasm 68K** = Amiga + Genesis + MacPlus, **ca65 65816** = SNES + IIGS (IIGS also sources its divergent FAT12 geom + 16B dir entry), **dasm 6502** = C64 + Apple II. Proof scope incl. disk apps + packed images (IIGS kernel+8 apps+.po; MacPlus kernel+boot+9 apps+.dsk; C64 kernel+10 apps+prg+d64; Apple II kernel+boot+8 apps+dsk). The 6502 ports are `single_app` (CONTRACT-ARCH §9) — no window-entry struct / event queue by design — so they source only the genuine overlap (cell screen geometry); their no-WM shape is now Contract-declared ([port.c64]/[port.apple2]) + conformance-checked (rule 9 single_app honesty), not faked |
| 5 | Hybrid policy pilot | ◐ partial — `unofs_core` compiles freestanding-strict (portable); vbcc+trackdisk+WinUAE blocked |
| 6 | `uno2d` tall vtable | ✅ host-proven — accel backend **pixel-identical** to the software floor; renders PPM |
| 7 | Concurrency floor + host SMP/TSan | ✅ host-proven — COOP==SMP==expected; guarded **TSan-clean**; race **caught** (`setarch -R`) |
| 8 | Display + profiles + directional | ◐ host-proven — multi-surface + profile/cap manifest generated | 8 | Display + profiles + directional | — | honest; directional-focus in conformance (35/35); NES/GB emulator validation blocked |
| 9 | `unosound` | ✅ host-proven — voice/score floor synths A440 (±3%), melody to WAV; chiptune accel (SID/Paula) blocked |
| 10 | SMP + OFFLOAD pilots (Saturn/PS3) | ◐ host-proven — OFFLOAD job floor/accel equivalence (COOP+SMP); real SH2/SPU hardware blocked (de-risked by Phase 7) |
| 11 | Drivers & buses | ◐ host-proven — enumerate→bind→register + registry-bound block read + FDS detect-pin scale-down; real PCI/USB hardware blocked |
| 12 | Ship 3.1 ABI | ◐ host-proven (additive) — categorized ordinal map (collision-free), .UNO v2 header generates w/ passing static-asserts; port re-issue is the future tail |
| 13 | New targets + Z80 + networking | ◐ in progress — **SEVEN new fresh ports landed.** **Game Boy Advance (ARM7TDMI): M1–M3 + game + audio, harness-verified** — the FIRST **ARM** world and a genuinely new unogen dialect (`arm`/GNU as, `.equ NAME, val`), consuming `[world.gba]`. `minimal` drawn into a flat **Mode-3 framebuffer** (240×160, 16bpp BGR555) — no HW tiles — plotting an 8×8 font + 16×16 icons pixel-by-pixel, each pixel's palette index in a 16-entry IWRAM table (so Theme recolours by swapping it). M1 launcher (4-col icon grid); M2 `REG_KEYINPUT` + a VCOUNT-polled loop + a d-pad highlight (A launches, B returns); M3 apps + Dostris on the square channel. mGBA's deferred-GL display won't grab under RDP, so it is verified on a **Unicorn ARM7TDMI** core running the real ROM (`gba/harness.py` → `gba/build/{desktop,nav,app,clock,dostris,theme,music}.png`). **Commodore VIC-20 (6502): M1–M3 + game + audio, harness-verified** — 6502 again (like the NES), reusing `gen/6502/` + dasm + `[world.vic20]`. `minimal`: the VIC 22×23 char matrix at `$1E00` + colour RAM `$9600` + a custom charset at `$3000`; an inverted charset gives the title bar + selection. M1 launcher (mini-icon list); M2 joystick on `$9111`/`$9120` + a raster-synced (`$9004`) loop + Up/Down highlight (Fire launches/returns); M3 apps (live Clock, Theme-cycles-`$900F`-bg, Music on the VIC oscillator) + Dostris in colour. Verified on a **py65** ROM-free 6502 core (`vic20/harness.py` → `vic20/build/{desktop,nav,app,clock,dostris,theme,music}.png`). **Bandai WonderSwan (NEC V30MZ): M1–M3 + game + audio, harness-verified** — the FIRST **x86 handheld**; the V30MZ is an 80186-class CPU, the same family as the reference kernel, so it is built with **nasm** (16-bit real mode) + the Contract's x86 surface + `[world.ws]`. `minimal` hardware-tile launcher: 8×8 2bpp planar tiles in the 16 KB internal RAM (`0x2000`) + a 32×32 SCR1 tilemap (`0x0800`), 224×144 mono LCD; tile colour resolves per-tile-palette → mono shade pool (`0x1C-0x1F`), so Theme recolours the whole screen by rewriting the pool. M1 launcher (inverted title bar + 11-icon list); M2 keypad on port `0xB5` (group-select) + a line-counter (`0x03`) loop + Up/Down highlight (A launches, B returns); M3 apps (live Clock, Theme-cycles-the-pool, Music on sound channel 1) + Dostris with a tile well. Verified on a **Unicorn x86/V30MZ** core that maps the ROM flush to `0xFFFFF` so the reset vector `0xFFFF0` exercises the real JMP-FAR boot path (`ws/harness.py` → `ws/build/{desktop,nav,app,clock,theme,music,dostris}.png`). **Game Gear (Z80 + 315-5124 VDP): M1–M3 + game + audio, emulator-verified** (Mesen2, GG mode): SMS silicon, so it reuses `gen/z80/` + the SMS hardware bring-up / 4bpp tiles / SN76489 PSG / Dostris — but the GG LCD shows only the centre 160×144 = 20×18, exactly the Game Boy's panel, so it wears the GB's `minimal` layout (vertical mini-icon list, drawn at a (6,3) offset). The one HW delta vs SMS is 12-bit CRAM. M1 launcher; M2 pad on `$DC` + frame-int loop + Up/Down highlight (button 1 launches, button 2 returns); M3 apps + Dostris in full colour. Proven via AUTOTEST scripted-pad (`gg/build/{desktop,nav,app,clock,dostris,theme,music}.png`). **Game Boy / Game Boy Color (Sharp SM83): M1–M3 + game + audio, emulator-verified** (Mesen2, GBC mode): the FIRST `gbz80` world — a genuinely new unogen dialect (rgbds, `DEF NAME EQU value`), consuming `gen/gbz80/` + `[world.gb]`. Also `minimal` profile (8KB RAM, no WM); the launcher is a VERTICAL LIST with 8×8 mini-icons (the 160×144 LCD suits a list, not a grid — same Contract, port-chosen layout). **M1** launcher; **M2** joypad on `$FF00` + a vblank-interrupt loop + an Up/Down selection highlight (A launches full-screen, B returns); **M3** apps (SysInfo, live Clock, Notepad, Files, Theme-cycles-BG-palette-live, Music on the GB APU) + **Dostris** (the falling-blocks algorithm ported to SM83). One ROM = greyscale on DMG (BGP) / colour on GBC (BG palette RAM), detected at boot. Proven via AUTOTEST scripted-pad builds (`gb/build/{desktop,nav,app,clock,dostris,theme,music}.png`). **NES (6502/2A03): M1–M3 + game + audio, emulator-verified** (Mesen2): the Contract's `minimal`-profile flagship (2KB RAM, no WM, directional nav), consuming `gen/6502/` + `[world.nes]`, dasm + iNES NROM-256. **M1** launcher (inverted title bar + 11 icons); **M2** standard pad on `$4016` + a vblank-NMI loop + a directional selection highlight (A launches full-screen, B returns — the §8 pointer-less model, *not* a WM); **M3** full-screen apps (SysInfo, live Clock, Notepad, Files, Theme-cycles-palette-live, Music on the 2A03 APU) + **Dostris** (the SMS falling-blocks algorithm ported to 6502). Minimal-floor rendering: a PPU-free NMI (tick + vblank flag) + main-loop vblank partials + rendering-off full redraws. Proven via AUTOTEST scripted-pad builds (`nes/build/{desktop,nav,app,clock,dostris,theme,music}.png`). **SMS (Z80) port: M1–M3 + game + audio, emulator-verified**: first port built fresh on the contract-driven + greenfield-window architecture; consumes `gen/z80/` + `[world.sms]`. **M1** desktop (title bar + 11 icons); **M2** WM (sprite cursor, Contract event queue, create/draw/raise/drag/close, z-order) + d-pad input; **M3** apps (SysInfo, live Clock, Notepad, Files, Theme-cycles-CRAM-live, Music); **Dostris** playable falling-blocks game; **PSG audio** (SN76489). Proven in BlastEm via AUTOTEST scripted-pad builds (`sms/build/{desktop,wm,dostris,music}.png`). Remaining toward Genesis parity: more games/Paint/Tracker/soft-kbd/SRAM. nic loopback + HEADLESS+NET compose; other console SDK backends still blocked |

## Toolchains reachable here
- **nasm** (`~/AppData/Local/bin/NASM`) — x86 ✅
- **vasmm68k_mot** (`~/amiga-tools`) — 68K ✅; **ca65/ld65** (`~/snes-tools/bin`), **dasm** (`~/apple2-tools`) ✅
- **sjasmplus** (`~/z80-tools`) — Z80 ✅; **rgbds 1.0.1** (`~/gb-tools`) — Sharp SM83 / Game Boy ✅
- **WSL gcc** incl. **ThreadSanitizer** ✅ (host C + Phase-7 SMP oracle)
- **Blocked**: vbcc (68K C), console SDKs (KOS/ee/devkit*/PSL1GHT/libyaul…)

## Phase 5 detail
The hybrid-policy pilot compiles the shared `unofs_core` for a constrained target.
vbcc (Amiga C cross-compiler) is not installed, so the literal Amiga build + WinUAE
run is deferred. The load-bearing claim — that `unofs_core` is portable enough to
drop into a tight target — is shown instead by a freestanding strict compile:

```
gcc -std=c11 -ffreestanding -fno-builtin -Os -Wall -Wextra -Werror \
    -I unodef/gen/c -I unofs -c unofs/unofs_core.c
```

Clean, ~2.7 KB of code, with no host dependencies beyond `string.h` + an allocator
a port supplies (the kernel's `mem_alloc`). Remaining tail: vbcc compile, link the
hand-asm trackdisk `block` backend, verify on WinUAE.

## Reproduce the full verification sweep
```
python unodef/unogen.py --check          # regenerate all worlds + x86 trust anchor
python unodef/conformance/conformance.py # 43/43 PORT-SPEC §6 vectors
nasm -f bin -Ikernel/ -o k.bin kernel/kernel.asm           # x86 kernel byte-identical
(cd amiga && vasmm68k_mot -Fhunkexe -nosym -opt-allbra -o a.exe kernel.asm)  # Amiga byte-identical
sh genesis/build.sh ; sh macplus/build.sh ; sh iigs/build.sh  # Genesis/MacPlus(vasm)+IIGS(ca65) byte-identical
sh c64/build.sh ; sh apple2/build.sh                       # C64 + Apple II (dasm 6502) byte-identical
sh unofs/build.sh ; sh uno2d/build.sh ; sh unosound/build.sh   # host C subsystems
sh unobus/build.sh ; sh unonet/build.sh ; sh unosched/build.sh # (unosched needs setarch -R for TSan)
```

## Greenfield window model + Phase 12 pilot (the 3.1 ABI, shipped on x86)
The 3.1 window-ABI decision landed as a **greenfield window model** (`WMODEL.md`,
`wmgen.py`, `[wmodel]`): one logical model → per-platform DERIVED physical layout
(SoA floor / AoS / widths / capacity) reached via generated zero-cost accessors,
proven across 9 platforms (`gen/wm/ARCHITECTURES.txt`) + a 40-yr arch survey, wired
into all 6 windowing ports' addressing (byte-identical). **Phase 12 piloted + shipped
on x86:** `[struct] win_entry` is now the clean 16 B layout (pointer title into a
kernel pool + compact fields); kernel needed no code edits (symbolic offsets + the
`win_entry_addr` stride macro). **QEMU + real-hardware (user's laptop) + cycle-
accurate-8088 (MartyPC) verified.** Shipped with a CGA save-under cursor fix. The
other windowing ports already run the compact 16 B layout.

## Tally
- **Fully host-proven (8):** 0,1,2,3,4,6,7,9 — including byte-identical rebuilds of
  **all 8 reachable-toolchain ports** (x86 + the 7 asm ports across nasm/vasm/ca65/dasm)
  and the SMP+TSan oracle.
- **Phase 12 (ship 3.1 ABI): SHIPPED on x86** — clean 16 B window layout, real-hardware
  + cycle-accurate-8088 validated (see above).
- **Host-proven core + hardware/SDK-blocked tail (4):** 5 (vbcc/WinUAE), 8 (NES/GB
  emulator), 10 (Saturn-SH2/PS3-SPU), 11 (PCI/USB), 13 (console SDK backends, real
  NICs). Each ships a working host model/generator + a documented blocked step.
