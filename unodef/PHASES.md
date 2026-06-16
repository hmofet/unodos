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
| 4 | Asm consumption | ✅ host-proven — **Amiga 68K (vasm)** + **SNES 65816 (ca65)** byte-identical via per-world equates; 6502/dasm ports lack a clean equate seam (deeper refactor, deferred) |
| 5 | Hybrid policy pilot | ◐ partial — `unofs_core` compiles freestanding-strict (portable); vbcc+trackdisk+WinUAE blocked |
| 6 | `uno2d` tall vtable | ✅ host-proven — accel backend **pixel-identical** to the software floor; renders PPM |
| 7 | Concurrency floor + host SMP/TSan | ✅ host-proven — COOP==SMP==expected; guarded **TSan-clean**; race **caught** (`setarch -R`) |
| 8 | Display + profiles + directional | ◐ host-proven — multi-surface + profile/cap manifest generated | 8 | Display + profiles + directional | — | honest; directional-focus in conformance (35/35); NES/GB emulator validation blocked |
| 9 | `unosound` | ✅ host-proven — voice/score floor synths A440 (±3%), melody to WAV; chiptune accel (SID/Paula) blocked |
| 10 | SMP + OFFLOAD pilots (Saturn/PS3) | ◐ host-proven — OFFLOAD job floor/accel equivalence (COOP+SMP); real SH2/SPU hardware blocked (de-risked by Phase 7) |
| 11 | Drivers & buses | ◐ host-proven — enumerate→bind→register + registry-bound block read + FDS detect-pin scale-down; real PCI/USB hardware blocked |
| 12 | Ship 3.1 ABI | ◐ host-proven (additive) — categorized ordinal map (collision-free), .UNO v2 header generates w/ passing static-asserts; port re-issue is the future tail |
| 13 | New targets + Z80 + networking | ◐ host-proven — Z80 equates generated; nic loopback round-trip + HEADLESS+NET server compose; console SDK backends blocked |

## Toolchains reachable here
- **nasm** (`~/AppData/Local/bin/NASM`) — x86 ✅
- **vasmm68k_mot** (`~/amiga-tools`) — 68K ✅; **ca65/ld65** (`~/snes-tools/bin`), **dasm** (`~/apple2-tools`) ✅
- **WSL gcc** incl. **ThreadSanitizer** ✅ (host C + Phase-7 SMP oracle)
- **Blocked**: vbcc (68K C), console SDKs (KOS/ee/devkit*/PSL1GHT/libyaul…), all emulators

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
python unodef/conformance/conformance.py # 38/38 PORT-SPEC §6 vectors
nasm -f bin -Ikernel/ -o k.bin kernel/kernel.asm           # x86 kernel byte-identical
(cd amiga && vasmm68k_mot -Fhunkexe -nosym -opt-allbra -o a.exe kernel.asm)  # Amiga byte-identical
sh unofs/build.sh ; sh uno2d/build.sh ; sh unosound/build.sh   # host C subsystems
sh unobus/build.sh ; sh unonet/build.sh ; sh unosched/build.sh # (unosched needs setarch -R for TSan)
```

## Tally
- **Fully host-proven (8):** 0,1,2,3,4,6,7,9 — including two byte-identical kernel
  rebuilds (x86, Amiga) and the SMP+TSan oracle.
- **Host-proven core + hardware/SDK-blocked tail (5):** 5 (vbcc/WinUAE), 8 (NES/GB
  emulator), 10 (Saturn-SH2/PS3-SPU), 11 (PCI/USB), 12 (port re-issue), 13 (console
  SDK backends, real NICs). Each ships a working host model/generator + a documented
  blocked step — nothing faked.
