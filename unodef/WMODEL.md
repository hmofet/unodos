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
| macplus | vasm (68000) | AoS | px u16  | 6  | 96 B   | reproduces the shipped 16 B entry — **wired** |
| amiga | vasm (68000) | AoS | px u16  | 6  | 96 B   | **wired** (`wintab(pc)`) |
| genesis | vasm (68000) | AoS | px u16 | 6  | 96 B   | **wired** (`VARS+v_wintab`) |
| snes  | ca65 (65816) | AoS | px u16  | 6  | 96 B   | **wired** (index→X `asl`×4) |
| iigs  | ca65 (65816) | AoS | px u16  | 6  | 96 B   | **wired** (index→X `asl`×4) |
| amd64 | C (x86_64)   | SoA | px u16  | 256| 5376 B | the 64-bit-C/SoA realization (see below) |

## Wired into ALL five windowing ports (byte-identical)
Every shipped port that has a window table now sources its window ADDRESSING from
this model. The compact 16 B port layout **is** the greenfield model in AoS form
(`state@0 owner@1 x@2 y@4 w@6 h@8 title@10`, `WIN_ENTRY_SIZE=16`); the x86 32 B
legacy is the outlier because it inlines the title. wmgen emits a per-port macro
that expands to the kernel's existing instructions, so every rebuild is
byte-identical:

| port | dialect | generated macro | hand-written sites replaced | result |
|---|---|---|---|---|
| macplus | vasm | `win_entry_ptr` (`lsl #4`/`lea wintab(pc)`) | `win_ptr_raw`, `zwin_ptr` | byte-identical |
| amiga | vasm | `win_entry_ptr` (`lsl #4`/`lea wintab(pc)`) | `win_ptr_raw`, `zwin_ptr` | byte-identical |
| genesis | vasm | `win_entry_ptr` (`lsl #4`/`lea VARS+v_wintab`) | `zwin_ptr` | byte-identical |
| snes | ca65 | `win_index_to_x` (`asl`×4 + `tax`) | `ent_x`, `zent_x` | byte-identical |
| iigs | ca65 | `win_index_to_x` (`asl`×4 + `tax`) | `ent_x`, `zent_x` | byte-identical |

Field offsets already came from each port's `[world.*]`; the index→address
arithmetic was the last hand-written piece, so each port's whole window boundary
is now Contract-generated. The two 68000 base-addressing modes (PC-relative vs
absolute `VARS+`) and the 65816 shift-index form are all just descriptor fields.

## AMD64 — is it "just an expansion of x86"?
Mostly yes, with one correction: AMD64's window realization is the **64-bit-C SoA**
shape (`[wmodel.platform.amd64]` = `host` with a bigger capacity) — it expands the
**host/C** pattern, **not** the 16-bit-real-mode `x86` pattern (which is AoS-16,
`ptr16`, `INT 0x80`). Same logical model, same C dialect, only the descriptor
changes — nothing new to design at the window layer; the generated header compiles
and runs like `host`. The genuinely new work for a *full* AMD64 port is the L1
**mechanism** (long mode, 64-bit call ABI, paging, GOP framebuffer, drivers), which
is orthogonal to the contract. The one new *window* surface AMD64 invites is an
**optional SIMD-composite accel** override — and that is the tall-vtable's opt-in
high-altitude part, sitting behind these same accessors, not a floor requirement.

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

## Full field access (every dialect) + write-once policy
The boundary is now complete, not just addressing:
- **C worlds** emit inline `win_get_*`/`win_set_*` for every field + the z-relation,
  and a **write-once L2 policy library** — `win_hit`, `win_move`, `win_resize`,
  `win_raise` (z-order promote), `win_topmost_at` (z-walk hit-test), `win_reap`.
  Identical source on every C platform; compiles and runs (host SoA verified).
- **asm worlds** (vasm/ca65) emit `WIN_<FIELD>` offset equates + `win_ld_*`/`win_st_*`
  (68000) and `win_lda_*`/`win_sta_*` (65816) convenience macros, alongside the
  addressing macro. They assemble. (The wired ports keep offset-direct access for
  fused ops like `cmp.w WX(a2),d0` — wrapping those in get/set macros would be
  worse; the offset *is* the natural asm accessor, and it's already Contract-sourced.)

## Architecture spec — 40 years of micros, one logical model
`gen/wm/ARCHITECTURES.txt` lists the derived layout for every platform. The model
is unchanged across all of them; only the **descriptor** differs (word/ptr width,
endianness, capacity, layout). Adding an architecture is a descriptor (+ a dialect
only if it needs a new assembler).

| platform | cpu | era / examples | bits | ptr | endian | layout | window table |
|---|---|---|---|---|---|---|---|
| c64 | 6502 | 1982 home micro | 8 | 16 | le | SoA | 66 B |
| z80/8088 | z80/x86 | CP/M, PC/XT | 8/16 | 16 | le | (single_app / AoS) | — |
| x86 | x86 | 16-bit real mode (shipping) | 16 | 16 | le | AoS | 224 B |
| 68000 | m68k | Amiga, Genesis, Mac Plus | 16/32 | 32 | **be** | AoS | 96 B |
| 65816 | 65816 | SNES, Apple IIGS | 16 | 16 | le | AoS | 96 B |
| arm | arm | Newton, PDAs, GBA-class | 32 | 32 | le | SoA | 1088 B |
| sh4 | SuperH | Dreamcast, set-top | 32 | 32 | le | SoA | 1088 B |
| mips | mips | SGI, PS1/PS2/PSP, N64 | 32 | 32 | **be** | SoA | 1088 B |
| ppc | PowerPC | Power Mac, GameCube/Wii, X360 | 32 | 32 | **be** | SoA | 1088 B |
| sparc | SPARC v8 | Sun workstations | 32 | 32 | **be** | SoA | 1088 B |
| alpha | Alpha | DEC 21064+ | 64 | 64 | le | SoA | 5376 B |
| riscv | RISC-V | rv64 | 64 | 64 | le | SoA | 5376 B |
| arm64 | AArch64 | phones, Apple silicon, servers | 64 | 64 | le | SoA | 5376 B |
| amd64 | x86_64 | modern PC | 64 | 64 | le | SoA | 5376 B |

Notes:
- **Endianness** is the only dimension the survey adds, and it does **not** change
  the generated *access* code: in-memory field access is native (the accessor is
  generated for the platform's order). The `.UNO` container / on-disk format stays
  **little-endian** (`contract.endian`), and big-endian ports byte-swap at *that*
  boundary only (the §1 "BE ports accessor-wrap at the boundary" rule). Portable
  policy never sees endianness.
- The 64-bit RISC targets (alpha/riscv/arm64/amd64) derive an identical layout —
  that *is* the point: they're one descriptor each over the proven C/SoA path, so
  emitting separate headers would be redundant (hence `spec_only`).
- **What's actually new per architecture is the L1 *mechanism*, not the window
  model**: a real port still needs its boot/mode-setup, call-gate, MMU/cache, and
  framebuffer/driver bring-up. The contract/window layer ports for free; that's
  the leverage. Architectures expressible the same way but omitted for brevity:
  PA-RISC, IA-64 (Itanium), VAX, S/390 — all just descriptors.

## SoA vs AoS, measured in a port (SNES / 65816)
Assembled a representative "read window x/y/w/h" routine both ways (ca65 → bin):

| form | index→access scaling | assembled | cycles* |
|---|---|---|---|
| AoS (shipped) | `index*16` = 4×`asl a` | **29 B** | **55** |
| SoA (uniform u16 cols) | `index*2` = 1×`asl a` | **26 B** | **49** |

\* 65816, 16-bit M: `and #imm` 3, `asl a` 2, `tax` 2, `lda abs,x` 5, `sta dp` 4, `rts` 6.

The *only* difference is the index-scaling: SoA eliminates 3 of the 4 shifts →
**−3 bytes, −6 cycles (~11%) per window access**. The field reads (`lda …,x`) are
identical in both. Runtime correctness of the SoA variant is **emulator-blocked**
here (behavior-changing), so this is a static size+cycle measurement, not a run.

**Honest verdict — and it validates the derived-layout design.** On these micros
the SoA win is *small*: the tables are tiny (6 windows), the AoS stride is already
a cheap power-of-two shift, and per-field reads don't change. SoA's real payoff is
elsewhere — free indexing when the stride *isn't* a power of two, SIMD column
processing on big machines (composite every window's `x` at once), and cache
density when touching one field across many windows — none of which a 6-window
8/16-bit table exercises. So the right call is exactly what the model already does:
**keep AoS for the micro ports (it's optimal there) and use SoA as the floor for
the C/big-machine targets where its ceiling matters.** Each platform gets its
optimal layout from one logical model — which is the whole point.

## x86 wired + QEMU-verified + real-hardware image
The x86 reference port now also sources its window **index→address arithmetic**
from the wmodel (`[wmodel.platform.x86nasm]` → `win_entry_addr` macro; 38 inline
`SHL_N reg,5 + add reg,window_table` sites replaced). Proven **byte-identical**
(HEAD vs working with the same build_info.inc → `e433e02b…`). **QEMU-verified**
(`qemu-system-i386`): boots to the desktop, opens the Sys Info window, launches
Clock — the WM works. **All six windowing ports** now consume the wmodel for
window addressing. Real-hardware boot guide: `docs/RUN-X86-REAL-HARDWARE.md`
(image: `build/unodos-144.img`). The window-entry *layout* still uses the shipping
`[struct] win_entry` (32 B); the greenfield clean layout is the future 3.1 break.

## x86 3.1 clean layout — SHIPPED + QEMU-verified
The x86 reference port now ships the **clean 16B window entry** (the real 3.1
break), done in two QEMU-verified increments:
1. **Pointer titles** — the inline `char[12]` title moved to a kernel title pool;
   the entry holds a near pointer (`win_title_pool` + handle*16). Removes the
   11-char limit and the 12 bytes that bloated the entry.
2. **Compaction 32B → 16B** — the Contract `[struct] win_entry` became the clean
   layout (`state@0 owner@1 x@2 y@4 w@6 h@8 title@10 zorder@12 flags@13 cscale@14`).
   `kernel.asm` needed **no code edits** (symbolic `WIN_OFF_*` + the `win_entry_addr`
   stride macro flip with the Contract); `window_table` shrank 512B → 256B.

z-order stays an **in-entry rank byte** — a valid per-platform realization of the
z relation (the entry pads to 16B for the `shl 4` stride regardless, so a side-list
buys no size and adds risk; the compact ports use the side-list realization).

Verified: trust anchor OK (updated expected values), conformance 52/52, C
`_Static_assert(sizeof==16)`, unofs compiles, all 5 other asm ports byte-identical;
QEMU full WM (open/close/reopen/drag/z-order, 3 distinct pointer-titles). This is
the first port shipping the 3.1 clean window layout. Real-hw guide:
`docs/RUN-X86-REAL-HARDWARE.md`.
