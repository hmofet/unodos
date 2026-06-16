# unodef/gen/ — GENERATED contract surfaces (do not edit)

Everything under this directory is emitted by [`../unogen.py`](../unogen.py) from
[`../unodef.toml`](../unodef.toml). **Do not hand-edit** — change the Contract and
regenerate:

```
python unodef/unogen.py            # emit all worlds
python unodef/unogen.py --check    # emit + assert x86 == kernel.asm literals (trust anchor)
```

## The first 5 worlds (the CPU families that cover every shipped port)

| Dir | World | Dialect | Equate syntax | Consumers |
|---|---|---|---|---|
| `x86/`   | x86      | NASM      | `NAME equ V` | `kernel/` (reference + trust anchor) |
| `c/`     | C core   | C header  | `#define` + packed structs w/ `_Static_assert` | ps2, dreamcast, mac, … (all C-world) |
| `m68k/`  | 68000    | vasm      | `NAME equ V` | amiga, genesis, macplus |
| `6502/`  | 6502     | dasm      | `NAME EQU V` | apple2, c64 |
| `65816/` | 65816    | ca65/cc65 | `NAME = V`   | snes, iigs |

Each file carries the **same** symbols — syscall ordinals (`SYS_*`), struct
offsets/sizes (`WIN_OFF_*`, `FILE_OFF_*`, `EVENT_*`, `DIRENT_OFF_*`, `BIN_OFF_*`),
FAT12 geometry (`FAT12_*`), font metrics (`FONT_*`, incl. `FONT_DEFAULT_ADVANCE=8`),
screen/palette consts, and the enums — rendered in that world's dialect. The C
header additionally emits real packed `struct` typedefs whose `_Static_assert`s fail
compilation if any offset or size ever drifts.

## What unogen does NOT emit

Only the *shape* of the boundary (CONTRACT-ARCH §3.2): no syscall bodies, drivers,
context-switch code, or CPU logic. Those stay hand-written per world.

## Trust anchor

`--check` asserts that 22 generated x86 constants (the struct sizes, the
five-places FAT12 geometry, the font advance, the slot count) equal the literals
currently hand-written in `kernel/kernel.asm`. This is the Phase-1 guarantee in
miniature: the generator reproduces the known-good values before anything depends
on it. Full byte-identical x86 rebuild is the next step (swap the hand-written
equate blocks for `%include "unodef/gen/x86/unodef.inc"`).
