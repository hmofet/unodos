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
| 2a | `.xls` read (BIFF8): values | **landed**, `[EXPERIMENTAL]` |
| 2b | `.xls` read: formula ptg decompiler | **landed**, `[EXPERIMENTAL]` |
| 3a | `.xls` write: values, strings, formats | **landed**, `[EXPERIMENTAL]` |
| 3b | `.xls` write: the formula compiler | **landed**, `[EXPERIMENTAL]` |
| 4a | `.doc` read: FIB, piece table, text | **landed**, `[EXPERIMENTAL]` |
| 4b | `.doc` read: direct formatting (CHPX/PAPX, sprms) | **landed**, `[EXPERIMENTAL]` |
| 4b′ | `.doc` read: the STSH style hierarchy | **landed**, `[EXPERIMENTAL]` |
| 4c | `.doc` minimal writer | **landed**, `[EXPERIMENTAL]` |
| 5a | `.ppt` read: persist chain + slide text | **landed**, `[EXPERIMENTAL]` |
| 5b | Escher: shapes, properties, anchors | **landed**, `[EXPERIMENTAL]` |
| 5c | `.ppt` minimal writer | **landed**, `[EXPERIMENTAL]` |
| 6a | ZIP container + XML pull parser | **landed**, `[EXPERIMENTAL]` |
| 6b | `.xlsx` read | **landed**, `[EXPERIMENTAL]` |
| 6c | `.docx` read | **landed**, `[EXPERIMENTAL]` |
| 6d | `.pptx` read | **landed**, `[EXPERIMENTAL]` |
| 6e | `.xlsx` / `.docx` / `.pptx` write (stored zip) | **landed**, `[EXPERIMENTAL]` |

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
                        ud_xls_open  (phase 2)  │  ud_doc / ud_ppt (4, 5)
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
- Every allocation the reader makes is proportionate to the file: the FAT,
  the mini FAT and the directory each cost at most about one file's worth,
  so a crafted header cannot ask for gigabytes on behalf of a small file.

**Unproven, stated rather than implied:** the reader is written
sector-size-general and accepts a **version 4** (4096-byte sector) header,
but no v4 file has ever been through it — Office 97 does not write one and
neither does LibreOffice, so nothing in the corpus can produce one. The plan
asked for v3 only; treat v4 as untested code until a real file turns up.

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

Numbers become text in exactly one place, `ud_num_text`, and for one reason:
a formula can contain a literal, so decompiling one requires rendering a
double. It follows **Excel's own display convention — at most 15 significant
digits** — not shortest-round-trip, and it is not a general number formatter:
cell *values* are formatted by UnoCalc's `uoc_numfmt`, which owns Excel's
format-code language.

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

## Reading a workbook

```c
ud_cfb *c = ud_cfb_open(&src);
ud_xls *x = ud_xls_open(c);            /* finds the Workbook stream itself */
if (!x) { show(ud_error()); return; }  /* "password-protected", "BIFF5", ... */

for (int s = 0; s < ud_xls_sheets(x); s++)
    for (int i = 0; i < ud_xls_cell_count(x, s); i++) {
        int row, col; ud_xcell cell;
        ud_xls_cell_at(x, s, i, &row, &col, &cell);
        ...
    }
ud_xls_close(x);                       /* before ud_cfb_close */
```

Walk with `ud_xls_cell_at` when you want every cell — a sheet is sparse and
the cells that exist are held sorted. `ud_xls_cell(x, s, row, col, &out)` is
the random-access form, a binary search over the same array.

### What phase 2a covers

- The record layer, with `CONTINUE` folding **and the continuation
  boundaries kept**, because of the trap below.
- Globals: `BOUNDSHEET` (name, position, visibility, worksheet-vs-chart),
  `SST`, `FORMAT`, `XF`, `DATEMODE`, and `FILEPASS` — an encrypted workbook
  is refused with a plain message, never half-read.
- Cells: `BLANK`, `MULBLANK`, `NUMBER`, `RK`, `MULRK`, `LABELSST`, `LABEL`,
  `BOOLERR`, `FORMULA` (+ the `STRING` record carrying a string result), and
  `MERGEDCELLS`.
- Number formats: `ud_xls_xf_format` resolves an XF through the file's
  `FORMAT` records, falling back to Excel's built-in id table. This is what
  tells a date serial from a plain number.

**The trap this phase exists to get right.** A record caps at 8224 bytes, so
the shared string table is always split across `CONTINUE` records — and a
string may be cut at *any* character, with the continuation restating whether
the remaining text is 8-bit or UTF-16. One string can change encoding
halfway through. `sst_string()` in `ud_xls.c` is the single place that knows
this, and the same shape recurs in `.doc` (piece table, fc bit 30) and
`.ppt` (`TextBytesAtom` vs `TextCharsAtom`), so phases 4 and 5 reuse the
lesson rather than rediscovering it.

The generated corpus reaches the split (54 mid-string splits in `sst.xls`,
measured) but LibreOffice always restates the *same* flag, so the
encoding-*switch* case — which [MS-XLS] permits and Excel emits — is
unreachable from any file we can generate. `xlstest selftest` therefore
hand-assembles a workbook byte by byte with four strings split on purpose:
8→8, 16→16, **8→16 and 16→8**.

### Formulas (phase 2b)

`cell.formula` says a formula produced the value; `cell.ftext` is that
formula decompiled back to text, `"=SUM(A1:A9)"`. The cached result is
always there either way — `ftext` is NULL if the token stream held something
this build does not render, and a viewer still has a correct value to show.

Excel stores a formula as reverse-Polish tokens, not text, so `ud_ptg.c`
runs the RPN back through a stack of fragments each carrying the precedence
of its top-level operator, and re-inserts parentheses only where the tree
requires them. Two details are easy to get quietly wrong and are both
covered by fixtures: Excel's `^` is **left**-associative (`2^3^2` is 64, so
an equal-precedence right operand really is parenthesised in the source),
and the user's own parentheses are recorded as an explicit `PtgParen`, so
`=(1+2)*3` comes back with them where the author put them rather than merely
somewhere valid.

Covered: the full operator set, references in all four relative/absolute
combinations, areas, 3-D references through `EXTERNSHEET`/`SUPBOOK`, defined
names, array constants (which live in `rgbExtra` after the token stream),
the `Ftab` function table, and **shared formulas** — a filled-down column
stores its expression once in a `SHRFMLA` and every member cell carries only
a `PtgExp` pointing at it, so each one re-bases the relative `PtgRefN`
tokens against its own position. Because the `SHRFMLA` follows the *first*
member, those cells are resolved at end of sheet rather than in record
order.

External workbook references render as `#REF!` rather than a guess; v1 does
not follow links into other files.

## Writing a workbook (phase 3a)

```c
ud_xlsw *w = ud_xlsw_new();
int s = ud_xlsw_sheet(w, "Sheet1");
ud_xlsw_str(w, s, 0, 0, "name");
ud_xlsw_num(w, s, 1, 0, 1234.5);
ud_xlsw_format(w, s, 1, 0, "0.00");
long len; unsigned char *xls = ud_xlsw_save(w, &len);   /* a complete file */
ud_free(xls); ud_xlsw_free(w);
```

`ud_xlsw_save` hands back the BIFF8 stream **already wrapped in a compound
file** — one call from model to something `uno_fs_write` can take. Writing the
same cell twice replaces it, so a caller serialising a model does not have to
de-duplicate first.

The globals preamble is written **from the spec, not from a canned byte
blob**. Canning one is the usual trick and the plan allowed for it, but the
records Excel actually insists on are few enough to emit honestly — four
`FONT`s (BIFF8 numbers them 0,1,2,3 and then *skips 4*, which is the single
strangest thing about the format), the fifteen style `XF`s, the default cell
`XF` at 15, a Normal `STYLE` — and a blob nobody can read is a blob nobody
can fix.

Two things the writer has to get right, both learned from the reader:

- The SST is split at the 8224-byte ceiling with `CONTINUE`, and a string cut
  mid-character **restates its encoding flag** in the new block. That is the
  writer half of the trap `ud_xls.c` reads.
- A string goes out 8-bit only when every character fits in one byte of
  UTF-16. CP-1252's `0x80`–`0x9F` map to code points above 255 (the euro sign
  is U+20AC), so those strings must go out wide or come back wrong.

### Writing formulas (phase 3b)

```c
ud_xcell cached = { UD_XV_NUM, 42.0, 0, 0, 0, 0, 0 };
ud_xlsw_formula(w, s, 0, 0, "=SUM(A1:A9)", &cached);
```

The text is compiled to tokens **immediately**, so a syntax error is reported
at the cell that caused it rather than surfacing much later as a failed save.
`cached` is the result stored alongside the expression — Excel keeps both, and
it is what a reader that does not calculate will show.

`ud_ptgc.c` is a recursive-descent parser that emits postfix directly: no
intermediate tree, because RPN is what the file wants and the recursion
already encodes the shape. One function per precedence rung, and an
operator's token is written after its operands have written theirs.

**Operand classes** are the subtle half, as the plan warned. Every
reference-ish token exists in three flavours — reference, value and array —
and Excel picks by how the operand is *consumed*, not by what it is. The rule
here: a reference used as a **direct function argument** goes out in reference
class, which is what lets `SUM` see a range instead of one dereferenced
value; everywhere else — arithmetic, comparison, the whole formula — it goes
out in value class. Every expression this build can construct is written,
read back by LibreOffice, and checked. It is **not** checked against real
Excel, which the plan reserves for a milestone with a VM; if a formula ever
comes back wrong there, this rule is the first place to look.

3-D references work because the writer emits an internal `SUPBOOK` and one
`EXTERNSHEET` entry per sheet, so a sheet index and its `ixti` are the same
number. Defined names are not written yet, so a formula referring to one is
refused rather than mis-compiled.

Not yet: defined names, fonts and colours beyond the default, column widths,
row heights, shared formulas on write (every formula is written in full,
which is correct, just larger than Excel would).

### Not in phase 2, stated rather than implied

- **`FONT`, `STYLE`, `COLINFO`, `ROW` and `WINDOW2` are skipped**, not
  parsed. Nothing consumes them yet and dead tables rot.
- **The 1904 epoch is read but untested** — `ud_xls_date1904` reports
  `DATEMODE` faithfully, but nothing in the corpus is a 1904 workbook, and
  unodoc does not convert serials to dates anyway (that is UnoCalc's).
- **BIFF5/BIFF7** are recognised and declined by name, not decoded.

## Reading a document (phase 4a)

```c
ud_doc *d = ud_doc_open(c);              /* c is an open ud_cfb */
puts(ud_doc_plain(d));                   /* what a person would read */
ud_doc_close(d);                         /* before ud_cfb_close */
```

`ud_doc_text()` is the body exactly as the file stores it — CP-1252, still
carrying Word's in-band control characters (`0x07` cell mark, `0x0D`
paragraph mark, `0x13`/`0x14`/`0x15` around fields). That is what a
formatting layer walks. `ud_doc_plain()` is the same text as reading matter:
paragraph marks become newlines, cell marks tabs, and a field's *code* is
dropped while its *cached result* is kept — which is why a page number shows
up as a number rather than as `PAGE`.

A `.doc` stores neither its text in one place nor in one encoding, and all
three consequences are where naive readers go wrong:

- **Document order is not file order.** The WordDocument stream holds runs
  wherever a quick-save left them; the piece table says which run supplies
  which part of the document, and it must be walked in *its* order.
- **Each piece picks its own encoding.** Bit 30 of a piece's offset means
  "this run is 8-bit", and the real offset is then the remaining bits
  *halved*. One document mixes 8-bit and UTF-16 runs freely — the same shape
  as BIFF8's shared strings, which is why `ud_xls.c` met it first.
- **Which table stream** holds the piece table is one bit in the FIB, naming
  `0Table` or `1Table`. Both may exist. If the named one is missing, unodoc
  tries the other rather than giving up on an otherwise readable document.

Every document in the corpus comes back with `pieces=1` — LibreOffice writes a
single text run, and nothing we can generate produces the multi-piece,
mixed-encoding layout a real quick-saved Word file has. So `doctest selftest`
builds one **by hand**, with the runs stored in a different order from the one
the piece table gives and the encodings alternating. A reader that walks the
file instead of the table gets the text scrambled; one that decides the
encoding once gets half of it as mojibake. Both are caught.

## Formatting (phase 4b)

```c
ud_chp ch; ud_pap pa;
ud_doc_chp_at(d, cp, &ch);      /* bold, italic, size, colour, ...        */
ud_doc_pap_at(d, cp, &pa);      /* alignment, indents, spacing, style id  */
```

Word stores formatting as **runs of exceptions**, not per character: CHPX for
characters and PAPX for paragraphs, packed into 512-byte pages, with a bin
table saying which page covers which stretch of the file. Note *of the file* —
formatting is indexed by byte offset, not by character position, so a lookup
goes through the piece table first. The hand-built selftest checks exactly
that: its bold run covers the piece stored **first** in the stream but reading
**third** in the document, so a reader that indexed by character position
would embolden the wrong words.

The one thing in here that must be exactly right is the sprm operand-size
table — the top three bits of the opcode — because getting it wrong
desynchronises the rest of the run rather than losing one property. Same for
`PapxInFkp`'s two-level length: a leading word count, and when that is zero
the real count is the *next* byte and the blob starts one further in.

### Styles (the STSH), and the order that matters

Most formatting in a real document does not live in the direct exceptions at
all — it lives in the style sheet, and the exceptions only override it. So
character formatting resolves in **four layers, outermost first**:

1. the **paragraph's** style, and everything that style is based on, applied
   root-first so a derived style overrides its parent;
2. any **character style** the run names, via `sprmCIstd` in its own CHPX;
3. the run's **direct** exceptions.

Paragraph formatting is the same minus the character-style layer. Get the
order backwards and direct formatting silently loses to the style it was
meant to override — which is why `doctest selftest` builds a document with a
base style, a style based on it, a character style and a direct exception all
touching different properties: any two layers applied in the wrong order lose
one.

## Writing a document (phase 4c)

```c
ud_docw *w = ud_docw_new();
ud_docw_para(w, "A heading", 1, 0, 1);      /* bold, centred */
ud_docw_para(w, "Body text.", 0, 0, 0);
long n; unsigned char *doc = ud_docw_save(w, &n);
```

Reading a `.doc` means coping with every layout Word has ever emitted; writing
one means picking the single simplest layout both Word and LibreOffice accept
and emitting it exactly: **one** 8-bit text piece, **one** exception page each
for characters and paragraphs, a style sheet holding just Normal (a document
with no STSH at all is rejected), and one section with no properties of its
own. Bold, italic and alignment go out as sprm deltas over that Normal style.

The awkward part is not any single structure — it is that the FIB has to be
written twice: once to reserve its space, and again at the end once every
other structure's offset and length is known.

Still absent: the sprm set is the common ~15 rather than the full ~50;
sections beyond one, tables and pictures are untouched; and the writer emits
no font table or document properties, which LibreOffice tolerates but real
Word has not been asked about.

### A note on the corpus, so nobody re-derives it

`fmt.doc` was authored with seven distinctly formatted runs and **most of that
formatting is not in the file**. LibreOffice's flat-ODF import drops the
majority of automatic text styles on the way to `.doc`: converting `fmt.doc`
back to ODF shows no bold, italic or font-size span at all, and a single
`text-align`. What survived is underline, strikethrough, and the Normal
style's 12pt.

So the gate asserts those three and no more, cross-checked against
LibreOffice's own read-back rather than against what the source asked for. It
is still a real test of the style machinery — `size` is 24 for *every* run and
arrives through the Normal style chain, not through any direct exception. The
layering itself is proven by the hand-built document, where the STSH is ours.

## Reading a presentation (phase 5a)

```c
ud_ppt *p = ud_ppt_open(c);
for (int i = 0; i < ud_ppt_slides(p); i++) puts(ud_ppt_slide_text(p, i));
ud_ppt_close(p);
```

A `.ppt` stream is an **append-only edit log**, and most of what is in it is a
previous version of the file. Finding the live document takes four hops, and
every one is a chance to read a stale version by mistake:

1. the **Current User** stream says where the *current* edit begins — not the
   start of the document and not the end of the stream;
2. that `UserEditAtom` points **back** to the previous one: the chain runs
   newest to oldest;
3. each edit carries a **persist directory** mapping object ids to offsets,
   and the same id appears in several of them. **The first one wins**, because
   that is the newest. Fold them oldest-first and every object in the
   presentation resolves to a stale copy of itself;
4. only then does `docPersistIdRef` name the live `DocumentContainer`.

Slides come from the document's `SlideListWithText` in presentation order,
falling back to every `SlideContainer` the persist directory names. Text is
collected by walking a slide's record tree — containers are identified by the
low nibble of the record header, so the walk needs no table of every record
type. A text block is either UTF-16 (`TextCharsAtom`) or 8-bit
(`TextBytesAtom`): the same either-encoding shape as BIFF8's shared strings
and `.doc`'s pieces, met here for the third time.

Not yet on the read side: pictures and placeholder roles.  Shape geometry is
phase 5b (`ud_ppt_slide_shapes`); the writer is phase 5c, below.

## Writing a presentation (phase 5c)

```c
ud_pptw *w = ud_pptw_new();
int s = ud_pptw_slide(w);
ud_pptw_title(w, s, "A heading");
ud_pptw_body (w, s, "First point\nSecond point");
long n; unsigned char *ppt = ud_pptw_save(w, &n);   /* a complete file */
```

Reading a `.ppt` means surviving the append-only edit log; writing one means
NOT writing an edit log at all.  `ud_pptw_save` emits the layout of a fresh
save — a **single UserEdit**, one persist directory, every object live —
which is the only layout a writer should ever produce.  The structure was
read record by record out of what LibreOffice's own 97 filter writes (the
corpus), then cut to what experiment showed a reader actually requires:
DocumentContainer (DocumentAtom, the Escher **Dgg shape-id ledger**, the
master and slide SlideListWithText rows), one MainMasterContainer with
colour schemes and one-level empty-mask TxMasterStyleAtoms (every value
defaults, honestly, instead of a canned blob nobody can read), one
SlideContainer per slide, the persist directory, the UserEditAtom, and a
Current User stream pointing at it.

Slide text goes out as **plain Escher textboxes** — an SpContainer per
frame with a ClientTextbox holding TextHeaderAtom plus a text atom — which
is how LibreOffice writes slide text, what our own tree walk reads back,
and what keeps this writer out of the placeholder/outline machinery.  The
title frame sits above the body frame at PowerPoint's classic geometry on a
5760x4320 (10 x 7.5 inch) slide.  `'\n'` in a title or body is a paragraph
break (0x0D in the atom).  Encoding is the split met three times on the
read side, applied in reverse: pure-ASCII text goes out as TextBytesAtom,
anything else as UTF-16 TextCharsAtom.  TextBytesAtom stores Latin-1 (the
low byte of a UTF-16 unit), and CP-1252's 0x80..0x9F are NOT Latin-1 — so
the split is on ASCII and no CP-1252 special ever rides the bytes form.

Not yet: StyleTextPropAtom (written text takes the viewer's defaults —
faces, sizes and colours are UnoShow's concern and need the style atom),
placeholders, pictures, notes, transitions.  The gate: `ppttest wtest`
round-trips a deck through our own reader (slides, both encodings, shape
count and title-above-body geometry), and the `written ppt` stage hands
the file to LibreOffice, which must find every checked string on exactly
the pages we wrote.

## OOXML (phases 6a-6e)

`.xlsx`, `.docx` and `.pptx` are a zip of XML parts rather than a compound
file, and that is the whole of the difference as far as everything above
unodoc is concerned: **the OOXML readers build the same models the binary
readers build**, so `ud_xls_cell_at`, `ud_doc_plain` and `ud_ppt_slide_text`
work on either, and an app that reads `.xls` reads `.xlsx` by opening a
different container.

```c
ud_src src;  ud_xls *x;
ud_src_mem(&src, buf, len);
switch (ud_sniff(&src)) {
case UD_C_CFB: { ud_cfb *c = ud_cfb_open(&src); x = ud_xls_open(c);  break; }
case UD_C_ZIP: { ud_zip *z = ud_zip_open(&src); x = ud_xlsx_open(z); break; }
}
```

**Sniff the container, do not trust the extension.** A `.xls` that is really a
`.xlsx` is common enough that every real reader handles it, and the first four
bytes settle it for free.

### The one new obligation on a caller

A zip part is a DEFLATE stream, inflated by unomedia's `um_inflate`, which
keeps its ~44 KB working state in an allocation of its own. So a program that
opens an OOXML file must register **both** allocators:

```c
ud_set_alloc(malloc, free);
um_set_alloc(malloc, free);      /* <- the OOXML path needs this too */
```

Forgetting the second one makes every OOXML file fail to open with an
out-of-memory that is really a forgotten registration. This bit the gate's own
`cfbtest` first, which is why it is written here where a caller will read it.

### Relationships, not file names

A part refers to another part through an `r:id` that the corresponding
`_rels/*.rels` part resolves. Slide and sheet ORDER comes from that
indirection, never from sorting `slide1.xml, slide2.xml, ...` - Office keeps a
slide's part name when the slide is moved, so a deck whose third slide lives
in `slide7.xml` is entirely ordinary and a reader that sorts by name silently
shuffles the presentation.

The prefix on an attribute matters in exactly one place and it is this one:
`<p:sldId id="256" r:id="rId2"/>` carries two attributes whose LOCAL name is
`id`, so a local-name match returns the slide's own number, no part resolves,
and a deck reads as N empty slides. `ud_xml_attr_ns()` exists for that.

### Writing: stored entries

`ud_xlsxw_save`, `ud_docxw_save` and `ud_pptxw_save` take the SAME model
objects the binary writers take - build a workbook once, choose the format at
the point you save. Parts are written **stored (uncompressed)**, which is
valid OOXML and is what lets unodoc write these formats without carrying a
DEFLATE compressor it would then have to maintain. The cost is file size on a
format whose bulk is repetitive XML, paid by a file the user is about to open
in Excel rather than by anything long-lived.

Text is transcoded CP-1252 -> UTF-8 on the way out (and back on the way in).
unodoc's models hold CP-1252 because that is what both binary formats store;
an XML part is UTF-8 by declaration. Skipping that step writes a file that
reads back byte-identical through a tolerant parser and mojibake through a
correct one - which is exactly the bug the `.xlsx` reader had in the other
direction, caught by dumping the same spreadsheet from its `.xls` and `.xlsx`
twins and diffing.

A `.pptx` also carries a slide master, a layout and a theme, written verbatim
from constants in `ud_pptxw.c`. Those are not decoration: they are where a
slide's inherited text properties come from, and PowerPoint will not open a
deck without them.

### What the gate proves

`mkcorpus.py` saves every flat-ODF source as BOTH the binary format and its
OOXML twin, and `run_tests.py` runs the same assertions over both, so a claim
that holds for one and not the other shows up as a failure rather than as a
gap. The writers are judged the same way twice: `wxtest` reads our own output
back with our own reader (they could be consistently wrong together), and the
`written` stages hand the file to LibreOffice, which has to find the same
content. `cfbtest fuzz` walks a mutated zip part by part - without that it
opened OOXML files as compound files, rejected every mutation at the
signature, and reported a confident "0 opened" while testing nothing.

## What unodoc is NOT

- **Not a renderer.** It hands back models and bytes; drawing them is
  `unoffice`'s lane (`pc64/uoffice/*`).
- **~~Not OOXML~~ — superseded 2026-08-20.** `.docx/.xlsx/.pptx` are now read
  and written; see *OOXML* below. The objection this bullet raised was that
  they need a deflate *compressor*, and the answer turned out to be that they
  do not: OOXML requires a zip container, not a compressed one, so the writers
  emit STORED entries and only the *de*compressor (`um_inflate`) is needed.
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

- **2026-08-20 — phases 6a-6e.** OOXML: `.xlsx`, `.docx` and `.pptx`, read and
  written. `ud_zip.c` (central-directory reader over `ud_src`, stored +
  deflate via unomedia's `um_inflate`, refusing encryption and ZIP64),
  `ud_xml.c` (a one-pass, zero-allocation pull parser - a tree was not an
  option in a module arena), `ud_xlsx.c` / `ud_docx.c` / `ud_pptx.c` building
  the SAME models as the binary readers through builder seams, and
  `ud_ooxz.c` + `ud_xlsxw.c` / `ud_docxw.c` / `ud_pptxw.c` writing them back
  as stored zips. `ud_sniff()` picks the container from the bytes.
  Three things this cost that were not obvious going in: the parts are UTF-8
  and unodoc is CP-1252 (a silent mojibake in both directions), `r:id` and
  `id` are different attributes on the same element (a deck of empty slides),
  and `um_set_alloc` is now part of a caller's contract (every part failing to
  inflate with an out-of-memory that was really a missing registration).

- **2026-08-02 — phase 5c.** `ud_pptw.c`: writing a presentation, and with it
  **the .ppt lane is complete in both directions** and every format unodoc
  planned for v1 reads AND writes.  A single UserEdit, the Dgg shape-id
  ledger, plain-textbox slides, both text encodings.  Gate: `ppttest wtest`
  (our reader gets back slides, text, both encodings, 3 shapes with the
  title above the body) and LibreOffice reads back every checked string on
  exactly 2 pages.  The structure came from dumping the corpus files record
  by record, not from anyone's code; the master's text styles are honest
  one-level empty-mask atoms, not a canned blob.
- **2026-08-02 — phase 5a.** `ud_ppt.c`: the persist chain and slide text.
  Gate: every slide line we extract is present in LibreOffice's own
  extraction, plus 4000 fuzz mutations. Also fixes a harness bug worth
  knowing about — `soffice_flat` read its output path without clearing it
  first, so a failed conversion silently compared against the PREVIOUS run's
  file. It invented one failure before it was caught; it could just as easily
  have hidden a real one.
- **2026-08-02 — phase 4c.** `ud_docw.c`: writing a `.doc`. The minimal
  layout Word and LibreOffice both accept - one 8-bit text piece, one
  exception page each for characters and paragraphs, a Normal style, one
  section - plus bold, italic and alignment as sprm deltas. Gate: our reader
  round-trips it AND **LibreOffice opens it** and finds all four paragraphs
  with the right formatting. UBSan caught an out-of-bounds FIB table on the
  first run.
- **2026-08-02 — phase 4b'.** The STSH: style sheet parsing, based-on
  chains, and four-layer resolution (paragraph style, character style,
  direct). Proven by a hand-built document where each layer sets a different
  property, so any two applied out of order lose one. Also found, and worth
  knowing before authoring corpus documents: LibreOffice's flat-ODF import
  drops most automatic text styles on the way to `.doc`, so `fmt.doc` holds
  far less formatting than its source asked for.
- **2026-08-02 — phase 4b.** Direct character and paragraph formatting:
  the CHPX/PAPX bin tables, the FKP pages, and a sprm interpreter. Also
  **closes phase 4a's open gap** — the multi-piece walk is now proven by a
  hand-built document with the runs out of order and the encodings
  alternating. New limit, and a bigger one than it sounds: LibreOffice routes
  most formatting through Word *styles*, so until the STSH is read, most
  formatting in a LibreOffice-authored document is invisible. Measured: 2
  CHPX for 7 formatted runs.
- **2026-08-02 — phase 4a.** `ud_doc.c`: the FIB, the piece table and the
  text. Gate: every corpus document's reading text is **identical to
  LibreOffice's own extraction** (900 lines on the largest), plus 9000 fuzz
  mutations. Caveat recorded above and worth repeating: every corpus file is
  single-piece, so the multi-piece walk is unproven.
- **2026-08-02 — phase 3b.** `ud_ptgc.c`: the formula compiler, and with it
  **the `.xls` lane is complete in both directions**. Gate: 28 expressions go
  text → tokens → file → tokens → text and come back unchanged, 9 malformed
  ones are refused, cached results of all four kinds survive, and LibreOffice
  re-renders every compiled formula correctly — which is what actually
  validates the operand classes.
- **2026-08-02 — phase 3a.** `ud_xlsw.c`: writing a workbook — sheets,
  every value kind, the interned shared string table with correct `CONTINUE`
  splitting, number formats, merged ranges, both date epochs. Gate: a demo
  workbook survives save-and-reload through our own reader (2500 shared
  strings, both epochs), and then **LibreOffice opens the file we wrote** and
  finds all twelve checked features — the half our own reader cannot judge.
  Also fixes the phase-2a gate, where the `xlstest` selftest had silently
  never been wired in (a patch script lost its backslashes; see the requests
  file).
- **2026-08-01 — phase 2b.** `ud_ptg.c`: the formula decompiler — operators
  with precedence-aware parenthesisation, every reference form, 3-D
  references, defined names, array constants, the `Ftab` function table, and
  shared formulas. Plus `ud_num_text`/`ud_int_text` in the core, because a
  formula literal has to be rendered and unodoc links no libc. Gate: 47
  formulas whose expected Excel text is written independently of the ODF
  source they are built from. The workbook fuzzer found a double-free on
  stack underflow. All surface `[EXPERIMENTAL]`.
- **2026-08-01 — phase 2a.** `ud_xls.c`: the BIFF8 record layer, globals,
  the shared string table (including mid-string `CONTINUE` encoding
  switches), every cell record type, merged ranges, and number-format
  resolution. Read only, values only. Gate: 4 fixture workbooks (19,043
  cells) checked against expectations derived from the SOURCE documents, a
  hand-built encoding-switch selftest, and 12,000 workbook fuzz mutations —
  which is what found the duplicate-`SST` leak. All surface
  `[EXPERIMENTAL]`.
- **2026-08-01 — phase 1.** `unodoc.h` core (`ud_src`, allocator, error
  surface, CP-1252/UTF-16 boundary, `ud_name_cmp`) and `ud_cfb.c`: the CFB
  container, read and write. Gate green: selftest, 7-file LibreOffice
  corpus, rebuild + LibreOffice oracle, 28,000 fuzz mutations.
  All surface `[EXPERIMENTAL]`.
