# Greenfield window model — RFC / hardened prototype

A candidate for the UnoDOS 3.1 window ABI that replaces the "pick 32B or 16B"
decision with a cleaner architecture: **one logical model, an optimal physical
layout derived per platform, reached only through generated zero-cost
accessors.** This is the tall-vtable / "describe what ships" principle applied to
*data* instead of code.

## The idea
- **`[wmodel]` in `unodef.toml`** is *logical*: named fields with a `kind` and a
  `tier` (hot/cold), z-order as a separate **relation** (not a per-window field),
  and the **invariants** portable policy relies on. It carries **no `@offset` and
  no size**.
- **`wmgen.py`** reads a `[wmodel.platform.*]` descriptor (word size, ptr size,
  coord space, capacity, layout) and **derives** the physical representation +
  emits **accessors** (`win_get_*`/`win_set_*`) keyed by an integer **handle**.
- Portable L2 policy (`win_hit`, `win_reap`, …) is written **once** against the
  accessors; it never sees an offset.

## Same model → four derived layouts (all generated, none hand-written)

| platform | dialect | layout | coords | cap | table | notable |
|---|---|---|---|---|---|---|
| c64   | dasm (6502)  | SoA | cells u8  | 6  | 66 B   | handle indexes columns directly — **no stride multiply** |
| amiga | vasm (68000) | SoA | px u16    | 6  | 102 B  | u16 columns **double the index** (68000 has no scaled index); u8 don't |
| host  | C (x86_64)   | SoA | px u16    | 64 | 1344 B | contiguous columns → SIMD-composite friendly |
| x86   | C (16-bit)   | AoS | px u16    | 16 | 224 B  | fixed-offset struct — proves AoS is just a layout knob |

## Verified (host-first, nothing faked)
- **C / SoA (host):** compiles; the write-once `win_hit`/`win_reap` policy runs
  and all invariants hold (exit 0). At `-O2`, `win_hit` is four direct indexed
  column loads (`movzx [wc_x + i*2]`) and compares — **no calls, no indirection**:
  the accessor boundary is genuinely free.
- **6502 / dasm (c64):** `dasm` assembles the generated macros; each is a single
  `lda/sta wc_x,y` (stride 1 — the SoA win on a multiply-poor CPU).
- **68000 / vasm (amiga):** `vasm` assembles the generated macros; u16 accessors
  emit the `add.w d1,d1` index-double, u8 accessors don't.
- **C / AoS (x86):** compiles + runs the *same* policy; a `_Static_assert(sizeof
  == WIN_ENTRY_SIZE)` checks the generator's offset math against the compiler's
  struct layout at compile time.
- **Conformance:** `conformance.py` 52/52 — incl. 9 `rule wm` checks that run the
  reap + zorder-total invariants **through accessors over the SoA realization**
  (layout-independent), each paired with a discrimination case.

## vs the legacy `win_entry` (the "is it just another layout?" answer)
Honest finding: the clean model in **AoS** form is **14 B**, vs the legacy
`win_entry`'s **32 B** — so "fixed-offset" *is* just `layout = aos`, but it is
**not byte-identical** to the legacy entry, by design:

| field | legacy 32B | clean AoS 14B | delta |
|---|---|---|---|
| title | inline `char[12]` @12 | handle `u16` @10 | **−10 B** (the big one) |
| z-order | in-entry `u8` @10 | a side **relation** list | −1 B in entry |
| owner | @11 | @1 (into the hot prefix) | reconciles the old flags/owner @1 clash |
| flags | @1 | @12 (cold tail) | demoted |
| content_scale | @24 | → `render_hint` | rename |
| reserved | `bytes[7]` @25 | (none) | −7 B |

The inline title alone was 12 of the legacy 32 bytes; pulling it to a handle +
z-order to a relation makes the clean model **<half** the size even in AoS form.

## Adding a platform you haven't chosen yet
1. add a `[wmodel.platform.<name>]` descriptor (word/ptr bits, coord space,
   capacity, `layout`);
2. if it needs a new assembler, add a ~30-line emitter dialect to `wmgen.py`
   (the m68k one was exactly that).

No struct to re-litigate; the layout falls out of the descriptor.

## Open items before adopting engine-wide
- SoA "free" allocation/free touches N columns instead of one block (trivial code).
- The hot/cold split is declared but only *advisory* in this prototype (columns
  are emitted adjacent); a port that wants cold columns in slow RAM wires that in
  `window_storage.c`.
- AoS-on-asm (stride-multiply) is intentionally not emitted — SoA is the floor;
  AoS is the C-side demo of the layout knob.
