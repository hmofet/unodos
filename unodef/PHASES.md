# UnoDOS 3.1 — phase status ledger

Tracks CONTRACT-ARCH §16 phases. **Host-proven** = built/ran/verified in this
environment. **Blocked** = needs a toolchain/emulator/hardware not reachable here
(documented, not faked). Legacy restore point: tag `legacy-pre-3.1` (commit 2f0f261).

| Phase | What | Status |
|---|---|---|
| 0 | Author UNODEF | ✅ host-proven — `unodef/unodef.toml` parses; 106 syscalls, structs, enums |
| 1 | unogen MVP + trust anchor | ✅ host-proven — x86 kernel **byte-identical** after sourcing the contract |
| 2 | Executable conformance | ✅ host-proven — `conformance.py` 29/29, discrimination vs historical bugs |
| 3 | `unofs` worked example | ✅ host-proven — reads the real floppy byte-identical; write/reap round-trip |
| 4 | Asm consumption | ✅ host-proven — **Amiga 68K byte-identical** via `[world.amiga]` equates |
| 5 | Hybrid policy pilot | ◐ partial — `unofs_core` compiles freestanding-strict (portable); vbcc+trackdisk+WinUAE blocked |
| 6 | `uno2d` tall vtable | ✅ host-proven — accel backend **pixel-identical** to the software floor; renders PPM |
| 7 | Concurrency floor + host SMP/TSan | — |
| 8 | Display + profiles + directional | — |
| 9 | `unosound` | — |
| 10 | SMP + OFFLOAD pilots (Saturn/PS3) | — |
| 11 | Drivers & buses | — |
| 12 | Ship 3.1 ABI | — |
| 13 | New targets + Z80 + networking | — |

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
