# UNODEF — the UnoDOS Contract (Layer 0)

This directory holds **`unodef.toml`**, the single machine-readable definition of
the UnoDOS ABI surface: the syscall call gate, the on-disk and in-memory structs,
the constants (FAT12 geometry, font metrics, palette), and the enums. Per
[docs/CONTRACT-ARCH.md](../docs/CONTRACT-ARCH.md) §3, this is **Layer 0** — the
contract every world is generated from or checked against. x86 (`kernel/`) is
demoted from "the definition" to **first consumer + conformance oracle**.

## Status: Phase 0 (authoring)

This is the *first* UNODEF: it encodes **what already ships** on x86, with **no
behavior change**. It is transcribed from the live source of truth:

- `kernel/kernel.asm` — `kernel_api_table` (the 106 call-gate slots), the struct
  equates (`WIN_*`, `FILE_ENTRY_SIZE`, `event_queue`), the FAT12 mount routine and
  data defaults, and `font_table` / `draw_font_advance`.
- `docs/PORT-SPEC.md` — the prose contract (the platform-independent law).
- `docs/API_REFERENCE.md` — per-call register signatures.

Phase 1 (`unogen` MVP) will emit a C header + NASM equates from this file,
regenerate the five-places FAT12 geometry + the font-advance constant, and prove
the x86 build is **byte-identical** — which is what makes the generator (and this
file) trustworthy (CONTRACT-ARCH §15, §16).

## Why TOML (and not a bespoke DSL)

CONTRACT-ARCH §3.1 sketches the contract in a custom DSL (`syscall gfx.fill_rect {
ordinal=0x12 ... }`). That reads well, but for the *first* artifact a bespoke DSL
is a net liability:

1. **It adds a second thing that can be wrong.** §17 names "`UNODEF` format &
   ownership … keeping *it* from drifting" and §15 names the meta-risk "the
   *generator* could be wrong." A hand-written parser/grammar is exactly that
   class of bug. TOML is parsed by stdlib in the tooling language already in this
   repo (Python `tomllib`, 3.11+; `tools/*.py` are all Python) and by a mature lib
   in every consumer language. Zero parser to author, review, or keep from
   drifting.
2. **The data is shaped like TOML.** "a list of syscalls / a list of structs" is
   arrays-of-tables; constants are tables; enums are tables. The mapping is direct.
3. **The DSL's only real win is inline field syntax** (`state:u8@0`). We keep that
   compactness with a *one-line mini-grammar* for the single compound value
   (struct fields), parsed by ~10 lines of regex — not a whole language. See below.

If authoring friction ever proves real, a thin sugar layer that **desugars to this
same TOML** can be added later without invalidating any consumer. Start boring.

### Field mini-grammar (the one compound value)

Each struct field is a single string:

```
"name : type @offset [le|be]"
```

- `type` ∈ `u8 u16 u32 i8 i16 i32 char[N] bytes[N]`
- `@offset` is the byte offset within the struct (the contract is byte-exact; the
  offsets *are* the law).
- endianness is `le` by default (declared once in `[contract] endian`); a field may
  override with a trailing `le`/`be`. Big-endian ports use byte-order accessors at
  the boundary only (PORT-SPEC §5).

## Conventions

- **Ordinals are the *current shipping* flat numbering (0–105).** The categorized
  ordinal scheme of CONTRACT-ARCH §11 (`0x00 gfx`, `0x30 window`, …) is a **3.1
  target**, not what ships today; each syscall carries a `category` so Phase 12 can
  re-map without re-deriving groupings.
- **`verified`** on each syscall: `true` = register signature transcribed and
  cross-checked against source/API_REFERENCE; `false` = name/ordinal/category/brief
  verified from `kernel_api_table`, full `args`/`ret` still to transcribe from
  `docs/API_REFERENCE.md`. Phase 1 conformance flips these all to `true`.
- See `[discrepancies]` in `unodef.toml` for the doc contradictions this file
  resolves (105-vs-106, font 12-vs-8, the stale handle-comment, geometry ×5).
