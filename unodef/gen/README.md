# unodef/gen/ — GENERATED contract surfaces (do not edit)

Everything under this directory is emitted by [`../unogen.py`](../unogen.py) from
[`../unodef.toml`](../unodef.toml). **Do not hand-edit** — change the Contract and
regenerate:

```
python unodef/unogen.py            # emit all worlds
python unodef/unogen.py --check    # emit + assert x86 == kernel.asm literals (trust anchor)
```

## Worlds (the CPU families that cover every shipped + planned port)

| Dir | World | Dialect | Equate syntax | Consumers |
|---|---|---|---|---|
| `x86/`   | x86      | NASM      | `NAME equ V` | `kernel/` (reference + trust anchor) |
| `c/`     | C core   | C header  | `#define` + packed structs w/ `_Static_assert` | ps2, dreamcast, mac, … (all C-world) |
| `m68k/`  | 68000    | vasm      | `NAME equ V` | amiga, genesis, macplus |
| `6502/`  | 6502     | dasm      | `NAME EQU V` | apple2, c64 |
| `65816/` | 65816    | ca65/cc65 | `NAME = V`   | snes, iigs |
| `z80/`   | Z80      | sjasmplus | `NAME EQU V` | sms, game boy (§13's one new asm family; planned) |

## Output kinds

- **`<world>/unodef.*`** — constants/offsets/enums (every world).
- **`<world>/unosys.*`** — call-gate **app stubs**: a macro per syscall that sets the
  selector and invokes the gate. Generated **only** for worlds whose call binding is
  declared in `[callgate.binding.*]` of the Contract — currently **x86** (`INT 0x80`,
  AH) and **m68k** (`TRAP #0`, D0.hi). For 6502/65816/Z80, unogen prints a note and
  skips: their call ABI isn't in the Contract yet, and fabricating one would defeat
  the single-source rule. Declare the binding to generate them.
- **`manifest.json`** — a world-neutral, machine-readable snapshot of the whole
  surface (ordinals, struct offsets, FAT12 geom, font advance, enums) for the
  conformance suite and doc-regen (§3.2 last row).

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
currently hand-written in `kernel/kernel.asm`.

**Byte-identical rebuild — DONE (Phase 1 anchor proven).** `kernel/kernel.asm` now
`%include`s `unodef/gen/x86/unodef.inc`; its hand-written `EVENT_*`, `FS_ERR_*`,
`WIN_*`, `FILE_*`, `FONT_*` equate blocks and its two FAT12 literal sites (mount
routine + BPB-cache defaults) are sourced from the Contract. Rebuilt with
`nasm -f bin -Ikernel/` (build_info.inc held constant), the kernel hashes
**identically** to the pre-change binary (SHA256 `04b9709a…`). The generator is
the source of truth, validated against the known-good build. (The FAT12 immediates
become Contract symbols/expressions of equal value so NASM folds them to the same
bytes — no instruction restructuring.) Remaining "five places": `boot/boot.asm`,
`boot/stage2.asm`, `tools/add_floppy_fs.py`, `tools/create_app_test.py`.
