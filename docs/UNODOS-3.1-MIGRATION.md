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
  conformance/          PORT-SPEC §6 made executable (43/43, with bug-discrimination)
  gen/                  GENERATED per-world surfaces (do not edit)
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
- **Host core + hardware/SDK-blocked tail (5):** 5 hybrid policy (needs vbcc/WinUAE)
  · 8 display/profiles (NES/GB emulator) · 10 SMP/OFFLOAD pilots (Saturn/PS3) · 11
  drivers/buses (PCI/USB) · 12 ship 3.1 ABI (port re-issue) · 13 new targets +
  networking (console SDKs).

## The one strategic open decision

The non-x86 ports ship **genuinely divergent ABIs** (16-byte window entries vs 32,
4-byte events vs 3, their own syscall ordinals). The forward line handles this with
the **multi-world contract** (`[world.*]` — "describe what ships", each port
generates its own values byte-identically). **Unifying all ports onto one 3.1 ABI
is Phase 12's deliberate future work** (a dual-build re-issue, port by port). The
canonical 3.1 layout (e.g. adopt the compact 16-byte window 6/8 ports already use,
or keep x86's 32) is **not yet decided** — that decision gates the real migration.

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
2. **Decide the canonical 3.1 ABI** (the open decision above), then pilot Phase 12
   end-to-end on **one** port: migrate it to the 3.1 layout/ordinals, re-verify on
   its emulator, keep a dual-build window. This is the real start of the migration.
3. **Reach a blocked tail** when a toolchain is available: vbcc for Phase 5
   (compile `unofs_core` for Amiga, link hand-asm trackdisk, WinUAE); a console SDK
   for a C-world backend of uno2d/unosound.
4. **Retarget `unoui` onto `uno2d`** (Phase 6 tail) and add the **Amiga blitter**
   backend as the first hardware-accel proof of the tall vtable.
5. **Add rule-4 conformance vectors** (press-time latch + sequence-counter) to round
   out PORT-SPEC §6 coverage.

## Resume checklist

```
git checkout master
python unodef/unogen.py --check && python unodef/conformance/conformance.py
cat unodef/PHASES.md          # current ledger
# pick a "Next directions" item above and go.
```
