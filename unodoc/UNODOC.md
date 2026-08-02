# unodoc, the UnoDOS document-format foundation

Readers and writers for the **Office 97-2003 binary formats** — `.doc`,
`.xls`, `.ppt`, the shared OfficeArt/Escher drawing layer, and the CFB
container all three live in. unodoc is to documents what `unomedia` is to
images and audio: a standalone library with no OS dependency, linked
privately into whichever `.UNO` module needs it.

This file is the **contract**: the source of truth for consumers, per
[`/AGENTS.md`](../AGENTS.md) §6. Re-read it (and the changelog at the bottom)
after every pull. The plan that produced it is
[`docs/OFFICE97-PLAN.md`](../docs/OFFICE97-PLAN.md) §4; the conformance
yardstick is [`docs/OFFICE97-SPEC.md`](../docs/OFFICE97-SPEC.md).

Owner: the unodoc lane (worker A). Apps consume this as a neutral API — file
a request rather than editing it.

## Status

| Phase | Surface | State |
|---|---|---|
| 1 | CFB container, read + write | **landed**, `[EXPERIMENTAL]` |
| 2 | `.xls` read (BIFF8) | not started |
| 3 | `.xls` write + formula compiler | not started |
| 4 | `.doc` read + minimal writer | not started |
| 5 | Escher + `.ppt` | not started |

Everything is `[EXPERIMENTAL]` until a consuming app has shipped on it. The
core surface (`ud_src`, the allocator, the error surface) is the part least
likely to move; the CFB API below is expected to gain calls, not change the
ones it has.

## The shape

```
   ud_src (random-access bytes)          ud_cfb_open(src)
        │  read(ctx, off, dst, n)               │
        ▼                                       ▼
   header ─► DIFAT ─► FAT ─► mini FAT ─► directory tree
                                                │
                        ud_cfb_find("/WordDocument")
                                                │
                        ud_cfb_read / ud_cfb_load ─► stream bytes
                                                │
                                                ▼
                        (phase 2+) ud_xls / ud_doc / ud_ppt
```

Writing runs the other way and never in place: build a `ud_cfbw` model, call
`ud_cfbw_serialize`, get one buffer.

## Rules every consumer follows

- **Register the allocator first.** `ud_set_alloc(malloc, free)` — the kernel
  heap in pc64, libc `malloc` on the host. It is idempotent, so *every*
  consumer calls it before first use rather than assuming someone else did.
  This is the same rule as `um_set_alloc`; forgetting it makes every open
  fail with an out-of-memory error rather than crashing.
- **`ud_error()` after any NULL / 0 return.** The strings are static and
  specific: "not a compound file (bad signature)" and "compound file:
  looping directory chain" are different problems and the user deserves to
  be told which.
- **Handles, not a global instance.** Unlike unomedia's single open stream,
  `ud_cfb *` / `ud_cfbw *` are handles: UnoOffice is MDI, several documents
  are open at once in one address space.
- **The source must outlive the handle.** `ud_cfb_open` copies the `ud_src`
  struct, not the bytes.

## Reading a container

```c
ud_set_alloc(kmalloc, kfree);

ud_src src = { my_read, file_size, my_ctx };
ud_cfb *c = ud_cfb_open(&src);
if (!c) { show(ud_error()); return; }

int id = ud_cfb_find(c, "/WordDocument");
long n; unsigned char *doc = ud_cfb_load(c, id, &n);
...
ud_free(doc);
ud_cfb_close(c);
```

`ud_cfb_read(c, id, off, dst, n)` is the streaming form — random access,
bounded by the entry's size. It keeps a cursor per chain, so a sequential
walk of a 10 MB stream costs O(1) chain steps per read rather than O(n).

Children come back in **CFB directory order** (short names first, then
uppercased), which is also the order `ud_name_cmp` defines:

```c
for (int k = ud_cfb_first(c, id); k != UD_CFB_NONE; k = ud_cfb_next(c, k))
    printf("%s %ld\n", ud_cfb_name(c, k), ud_cfb_size(c, k));
```

### What the reader guarantees against a hostile file

This parser is the first thing that touches a `.doc` off a USB stick, so the
guarantees matter more than the features:

- Every sector index is range-checked against the sectors that **physically
  exist in the file** before it is used — not against what the header claims.
- Every chain walk carries a step budget of one table's worth, so a FAT or
  mini-FAT loop ends in a short read instead of a hang.
- The directory's red-black sibling trees are flattened with a **visited
  bitmap**, so a cyclic or cross-linked tree is truncated rather than walked
  forever. Unreachable entries are simply dropped.
- Declared stream sizes are **clamped** to what the file can hold. Whether a
  stream is mini or regular is decided from the *declared* size before
  clamping, so a fuzzed size cannot move a stream into the other table.
- Header fields that cannot be guessed at are refused outright: bad
  signature, a sector shift other than 9/12, a mini sector shift other than
  6, a mini-stream cutoff other than 4096, a zero or absurd FAT sector
  count, a DIFAT that does not describe every FAT sector it promised.
- `ud_cfb_load` refuses anything over `UD_MAX_STREAM` (64 MB).

A damaged file may still open and yield what is readable — salvage is the
right behaviour for a document the user cares about — but it can never make
unodoc read outside its buffers or fail to terminate. The host gate enforces
exactly that (see below).

## Writing a container

```c
ud_cfbw *w = ud_cfbw_new();                   /* holds "Root Entry"        */
ud_cfbw_clsid(w, UD_CFB_ROOT_ID, word_clsid);
ud_cfbw_stream(w, UD_CFB_ROOT_ID, "WordDocument", fib, fib_len);
int pool = ud_cfbw_storage(w, UD_CFB_ROOT_ID, "ObjectPool");
ud_cfbw_stream(w, pool, "\001Ole", ole, 20);

long len; unsigned char *img = ud_cfbw_serialize(w, &len);
uno_fs_write(path, img, len);
ud_free(img);
ud_cfbw_free(w);
```

`ud_cfbw_stream_take` is the same but adopts a `ud_alloc`'d buffer instead of
copying it — use it for the multi-megabyte streams the format writers build,
so a save does not hold two copies.

### What the writer guarantees

- **Always a fresh file**, version 3 / 512-byte sectors — what Office 97
  writes and what every reader has seen most of. Never a modification in
  place, so a bug can never corrupt the file being replaced.
- Streams under 4096 bytes go to the mini stream, the rest to whole sectors;
  the FAT sizes itself to a fixed point that includes its own sectors and its
  DIFAT sectors.
- The directory is emitted as a **balanced, all-black tree in CFB name
  order** — length first, then uppercased code units. The ordering is the
  part real Office checks; the colouring it ignores, and an all-black tree
  is the arrangement every implementation accepts.
- Duplicate names within one storage are rejected (compared
  case-insensitively, as CFB does), as are names over 31 code units and names
  containing `/ \ : !`.
- Serialisation is a **pure function of the model**: the same model always
  produces the same bytes. That is what makes a byte diff against a saved
  file a meaningful test rather than noise.

## Names and text

Phase 1-4 hold text as **8-bit CP-1252** (Office 97's own 8-bit encoding, and
what the font engine can draw today — CP-1252 glyph coverage beyond ASCII is
a filed request to the font lane). `ud_cp1252_to_uc` / `ud_uc_to_cp1252`
convert at the UTF-16 boundary the container and the formats sit on.
CP-1252 is single-byte and wholly inside the BMP, so **a unodoc name's byte
length is its UTF-16 length** — which is what CFB's ordering rule compares.
Code units with no CP-1252 form fold to `?`.

Known limit, stated rather than hidden: `ud_upper16` covers ASCII, Latin-1
and the cased CP-1252 specials. That is exact for every name Office
generates (they are ASCII, plus the `\001` and `\005` prefix bytes); for
anything else the ordering is still a stable total order, just not Unicode's
own casing. Widen it when a real file needs it, not before.

## The host gate

```bash
cd unodoc/test && python3 run_tests.py
```

Runs without booting the OS, over the same sources the `.UNO` module compiles
freestanding. Five stages: **build** (with `build.sh`'s sanitizer set plus
ASan, `-fno-sanitize-recover=all` — the UnoAmp EQ lesson, a harness without
the OS's flags tests different code), **selftest** (in-memory round-trip
across every size that straddles the 64 / 512 / 4096 boundaries, directory
ordering, the DIFAT overflow path at 16 MB, random access, and a battery of
corrupt containers), **corpus**, **rebuild**, **fuzz**.

The corpus is **generated, never committed**: `mkcorpus.py` writes flat-ODF
sources and hands them to LibreOffice headless, which saves them through its
MS Word 97 / Excel 97 / PowerPoint 97 filters. Those are real containers
written by someone who is not us — a round-trip gate over only our own
writer's output proves nothing.

The rebuild stage is the load-bearing one. Each corpus file is read, rebuilt
through *our* writer, and then handed back to LibreOffice: if it converts the
rebuilt container to the same document as the original, a third party has
agreed our container is well formed. The oracle converts the original
**twice** first and fails loudly if those two disagree — otherwise a volatile
field (Word's `Rsid`, which LibreOffice regenerates on every load) would
quietly turn the comparison into a no-op.

Requires `soffice` on PATH. On this machine that is WSL:

```bash
sudo apt install libreoffice-writer libreoffice-calc libreoffice-impress
```

## What unodoc is NOT

- **Not a renderer.** It hands back models and bytes; drawing them is
  `unoffice`'s lane (`pc64/uoffice/*`).
- **Not OOXML.** `.docx/.xlsx/.pptx` are Office 2007 and out of scope for v1
  (they need a deflate *compressor*; the ZIP reader half exists in
  `um_inflate` + the hardened central-directory walk in `unoamp_skin.c`).
- **Not encryption.** Encrypted files ([MS-OFFCRYPTO], BIFF `FILEPASS`) are
  to be detected and **refused with a clear message**, never half-read.
- **Not third-party code.** Written from scratch against the open specs
  ([MS-CFB], [MS-DOC], [MS-XLS], [MS-PPT], [MS-ODRAW] on
  learn.microsoft.com). Apache POI (Apache-2.0) and LibreOffice (MPL-2.0)
  are studied as documentation; **GPL sources are not read** (Antiword,
  wvWare, catdoc, the Gnumeric importer). Spec PDFs live under
  `unodoc/specs/` and are gitignored.

## Build integration

unodoc compiles into whatever needs it, PHOTOS-style: an Office app module
links its own private copy of the core plus the format halves it uses. There
is **no `pc64/build.sh` block yet** — the kernel does not need unodoc until
the first Office app lands, and per `/AGENTS.md` §2 a choke-point is touched
only when it is actually needed, as an append.

## Changelog

- **2026-08-01 — phase 1.** `unodoc.h` core (`ud_src`, allocator, error
  surface, CP-1252/UTF-16 boundary, `ud_name_cmp`) and `ud_cfb.c`: the CFB
  container, read and write. Gate green: selftest, 7-file LibreOffice
  corpus, rebuild + LibreOffice oracle, 28,000 fuzz mutations.
  All surface `[EXPERIMENTAL]`.
