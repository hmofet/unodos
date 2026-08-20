/* ===========================================================================
 * unodoc - the UnoDOS document-format foundation: readers and writers for
 * the Office 97-2003 binary formats (.doc / .xls / .ppt), the shared
 * OfficeArt/Escher drawing layer, and the CFB container they all live in.
 *
 * unodoc is to documents what unomedia is to images and audio: a standalone
 * library with no OS dependency, linked privately into whichever .UNO module
 * needs it (the UnoOffice apps), or into the kernel if the kernel ever needs
 * it.  See docs/OFFICE97-PLAN.md §4 for the phase plan and UNODOC.md for the
 * contract, the changelog, and the stability markers.
 *
 * PHASE 1 (this header): the CFB container, read AND write.
 *
 * DESIGN RULES (inherited from unomedia, and they are load-bearing):
 *   - Freestanding C, no libc beyond mem-/str-.  Builds unchanged as a
 *     hosted object for the host tests and as part of a .UNO module.
 *   - IEEE doubles ARE used, unlike unomedia's integer-only rule: a
 *     spreadsheet cell IS a double and there is no honest way around it.
 *     What unodoc never does is call libm - only the four arithmetic
 *     operators and comparisons, which are hardware on x86-64 - and it
 *     never FORMATS a double (that
 *     is UnoCalc's uoc_numfmt, which owns Excel's format-code language).
 *   - Nothing loads a whole file: bytes stream through the caller's ud_src.
 *     (The one exception is ud_cfbw_serialize, which by definition produces
 *     a whole file - unofs has no append path today.)
 *   - Big or size-dependent buffers come from the registered allocator
 *     (ud_set_alloc -> the kernel heap in pc64, libc malloc on the host).
 *     Static .bss stays tiny: a .UNO module's mem_size is arena budget.
 *   - Written from scratch against the open specs ([MS-CFB] et al).  Apache
 *     POI and LibreOffice are studied as documentation; no GPL source is
 *     read and no third-party code is copied.
 *   - THE PARSERS ARE ATTACK SURFACE.  Every index out of a file is
 *     validated, every chain walk is bounded, every declared size is
 *     clamped to what the file can physically hold.  A malformed file must
 *     fail cleanly, never crash and never hang.
 *
 * INSTANCING: unlike unomedia's single global open stream, unodoc is
 * handle-based (ud_cfb *, ud_cfbw *).  UnoOffice is an MDI suite: several
 * documents are open at once in one address space, so a global "one open
 * file" instance would be wrong from day one.  The allocator and the error
 * surface are still process-global, matching unomedia.
 * ======================================================================== */
#ifndef UNODOC_H
#define UNODOC_H

#include <stdint.h>

/* ---- the byte source [STABLE] ---------------------------------------------
 * The app hands unodoc a random-access byte source (pc64: uno_fs_read_at
 * behind a struct; host tests: pread over a FILE, or ud_src_mem over a
 * buffer).  Same shape as unomedia's um_src, deliberately. */
typedef struct {
    /* copy up to n bytes from byte offset off into dst; returns bytes
       copied, 0 at/past EOF, <0 on I/O error. */
    long (*read)(void *ctx, long off, unsigned char *dst, long n);
    long  size;                             /* total file size in bytes      */
    void *ctx;
} ud_src;

/* A source over a caller-owned memory buffer.  The buffer must outlive every
 * handle opened on it.  (The round-trip gate and the writers use this.) */
void ud_src_mem(ud_src *s, const void *buf, long len);

/* Bounds-checked read through a source: clamps [off,off+n) to [0,size) so a
 * caller can never walk a source off its end.  Returns bytes copied. */
long ud_src_read(const ud_src *s, long off, void *dst, long n);

/* ---- the allocator [STABLE] -----------------------------------------------
 * Registered once by the host program / app before any open; idempotent, so
 * every consumer may call it (the unomedia rule).  ud_alloc makes no promise
 * about zeroing; ud_free tolerates NULL. */
void  ud_set_alloc(void *(*a)(unsigned long), void (*f)(void *));
void *ud_alloc(unsigned long n);
void  ud_free(void *p);

/* ---- the error surface [STABLE] -------------------------------------------
 * Why the last call failed, or "".  Strings are static.  A reader that
 * RECOGNISES a file but declines it says so precisely - the difference
 * between "that is not a compound file" and "that is a perfectly good
 * compound file this build declines" matters to whoever is looking at the
 * screen. */
const char *ud_error(void);
void        ud_set_error(const char *why);

/* ===========================================================================
 * CFB - the Compound File Binary container [MS-CFB]        [EXPERIMENTAL]
 *
 * A CFB file is a FAT filesystem in a file: fixed-size sectors, a sector
 * allocation table, a second "mini" allocation table for streams below a
 * 4096-byte cutoff, and a directory of storages (folders) and streams
 * (files) held as a red-black tree.  .doc, .xls and .ppt are each a
 * particular set of streams inside one.
 * ======================================================================== */

/* Directory entry object types (the on-disk encoding). */
enum {
    UD_ENT_EMPTY   = 0,
    UD_ENT_STORAGE = 1,
    UD_ENT_STREAM  = 2,
    UD_ENT_ROOT    = 5
};

#define UD_CFB_NONE       (-1)   /* "no such entry" / end of a sibling list  */
#define UD_CFB_ROOT_ID      0    /* the root storage is always directory 0   */

/* The mini-stream cutoff: a stream strictly smaller than this lives in the
 * mini stream, everything else in whole sectors. */
#define UD_CFB_MINI_CUTOFF  4096

/* A directory entry name is at most 31 UTF-16 code units plus a terminator.
 * unodoc holds names as 8-bit CP-1252 (the v1 internal text encoding, see
 * UNODOC.md); CP-1252 is single-byte and wholly inside the BMP, so the byte
 * length of a unodoc name equals its UTF-16 length - which is what CFB's
 * directory ordering compares.  Code units outside CP-1252 fold to '?'. */
#define UD_NAME_MAX        31
#define UD_NAME_BUF        32

/* Guard for the "load the whole stream" convenience call. */
#define UD_MAX_STREAM      (64L * 1024 * 1024)

typedef struct ud_cfb ud_cfb;

/* ---- reading -------------------------------------------------------------- */

/* Open `src` as a compound file: validate the header, load the FAT, the
 * mini FAT and the directory, and flatten the directory's red-black trees
 * into child lists.  Returns a handle, or NULL (ud_error says why).  `src`
 * is copied into the handle; the underlying bytes must stay readable until
 * ud_cfb_close. */
ud_cfb *ud_cfb_open(const ud_src *src);
void    ud_cfb_close(ud_cfb *c);

int         ud_cfb_version(const ud_cfb *c);   /* 3 (512B) or 4 (4096B)      */
int         ud_cfb_count(const ud_cfb *c);     /* directory entries present  */
int         ud_cfb_type(const ud_cfb *c, int id);        /* UD_ENT_*         */
const char *ud_cfb_name(const ud_cfb *c, int id);        /* "" if invalid    */
long        ud_cfb_size(const ud_cfb *c, int id);        /* stream bytes     */
int         ud_cfb_parent(const ud_cfb *c, int id);      /* UD_CFB_NONE root */
/* 16 raw CLSID bytes, or NULL.  Word/Excel/PowerPoint stamp the root. */
const unsigned char *ud_cfb_clsid(const ud_cfb *c, int id);

/* Children, in CFB directory order (short names first, then uppercased).
 *   for (int k = ud_cfb_first(c, s); k != UD_CFB_NONE; k = ud_cfb_next(c, k)) */
int ud_cfb_first(const ud_cfb *c, int parent);
int ud_cfb_next (const ud_cfb *c, int id);

/* One direct child by name, compared the way CFB compares names (length
 * first, then uppercased).  UD_CFB_NONE if absent. */
int ud_cfb_child(const ud_cfb *c, int parent, const char *name);

/* Resolve a '/'-separated path from the root, e.g. "/Workbook",
 * "WordDocument", "ObjectPool/_1234/\001Ole".  A leading '/' is optional.
 * UD_CFB_NONE if any component is missing. */
int ud_cfb_find(const ud_cfb *c, const char *path);

/* Stream bytes.  Random access, bounded by the entry's size; returns the
 * number of bytes copied (short or 0 at end of stream, or on a broken
 * chain - the file is damaged, not the caller). */
long ud_cfb_read(ud_cfb *c, int id, long off, void *dst, long n);

/* The whole stream in one ud_alloc'd buffer (caller ud_free's it), with a
 * NUL byte appended past *len for the convenience of text scanners.
 * Refuses streams over UD_MAX_STREAM.  NULL on failure. */
unsigned char *ud_cfb_load(ud_cfb *c, int id, long *len);

/* ---- writing --------------------------------------------------------------
 * NEVER in place.  A writer builds an in-memory model and serialises a fresh
 * file from it: sectors laid out from scratch, mini vs regular chosen by the
 * cutoff, FAT / mini FAT / DIFAT / directory built to match, directory
 * emitted as a balanced all-black tree in CFB name order.  That ordering is
 * the thing real Office actually checks. */

typedef struct ud_cfbw ud_cfbw;

ud_cfbw *ud_cfbw_new(void);          /* a model holding just the root storage */
void     ud_cfbw_free(ud_cfbw *w);

/* Add a child storage / stream under `parent` (UD_CFB_ROOT_ID for the root).
 * Names are CP-1252, at most UD_NAME_MAX code units, and may begin with the
 * 0x01 / 0x05 bytes Office uses ("\005SummaryInformation").  Duplicate names
 * within one storage are rejected (CFB compares case-insensitively).
 * Returns the new entry's id, or UD_CFB_NONE. */
int ud_cfbw_storage(ud_cfbw *w, int parent, const char *name);
int ud_cfbw_stream (ud_cfbw *w, int parent, const char *name,
                    const void *data, long len);
/* As ud_cfbw_stream but TAKES OWNERSHIP of a ud_alloc'd buffer instead of
 * copying it - the format writers build multi-megabyte streams and should
 * not pay for a second copy.  The buffer is ud_free'd by ud_cfbw_free.  On
 * failure the buffer is NOT taken (the caller still owns it). */
int ud_cfbw_stream_take(ud_cfbw *w, int parent, const char *name,
                        unsigned char *data, long len);

/* Stamp an entry's CLSID (16 raw bytes).  Office puts the application's
 * class id on the root storage. */
int ud_cfbw_clsid(ud_cfbw *w, int id, const unsigned char clsid[16]);

/* Serialise the model into one ud_alloc'd buffer of *len bytes (caller
 * ud_free's it).  Always version 3 / 512-byte sectors, which is what Office
 * 97 writes.  NULL on failure. */
unsigned char *ud_cfbw_serialize(ud_cfbw *w, long *len);

/* ===========================================================================
 * .xls - the BIFF8 workbook [MS-XLS]                       [EXPERIMENTAL]
 *
 * A BIFF8 workbook is a flat stream of (type, length, payload) records
 * inside the container's "Workbook" stream: one globals substream (fonts,
 * formats, cell formats, the shared string table, the sheet directory)
 * followed by one substream per sheet.  Records longer than 8224 bytes are
 * split with CONTINUE records - and a shared string may split at ANY
 * character, with the continuation restating whether it is 8-bit or
 * UTF-16.  That last sentence is the single most common BIFF8 bug and
 * ud_xls.c handles it in one place.
 *
 * PHASE 2 (this surface) is READ ONLY, and reads values: a formula cell
 * reports its cached result and that it is a formula.  Decompiling the ptg
 * array back to "=SUM(A1:A9)" is phase 2b; writing is phase 3.
 * ======================================================================== */

/* cell value kinds */
enum {
    UD_XV_EMPTY = 0,      /* no cell, or a blank cell carrying only format  */
    UD_XV_NUM   = 1,
    UD_XV_STR   = 2,
    UD_XV_BOOL  = 3,
    UD_XV_ERR   = 4
};

/* Excel's seven error values, in their on-disk encoding */
enum {
    UD_XE_NULL  = 0x00,   /* #NULL!  */
    UD_XE_DIV0  = 0x07,   /* #DIV/0! */
    UD_XE_VALUE = 0x0F,   /* #VALUE! */
    UD_XE_REF   = 0x17,   /* #REF!   */
    UD_XE_NAME  = 0x1D,   /* #NAME?  */
    UD_XE_NUM   = 0x24,   /* #NUM!   */
    UD_XE_NA    = 0x2A    /* #N/A    */
};

/* The text of an error value ("#DIV/0!"), or "#ERR?" if unrecognised. */
const char *ud_xls_err_text(int err);

typedef struct {
    int         kind;     /* UD_XV_*                                        */
    double      num;      /* NUM, and BOOL as 0/1; a formula's CACHED result*/
    const char *str;      /* STR: CP-1252, owned by the workbook            */
    int         err;      /* UD_XE_*                                        */
    int         xf;       /* index into the workbook's XF table             */
    int         formula;  /* 1 if a formula produced this value             */
    /* The formula decompiled back to text, "=SUM(A1:A9)", owned by the
       workbook - or NULL when the cell is not a formula, or when its token
       stream held something this build does not render (the cached value is
       still good).  Never confuse NULL with "not a formula": read `formula`
       for that. */
    const char *ftext;
} ud_xcell;

typedef struct ud_xls ud_xls;

/* Open the workbook inside an already-open container.  Finds the "Workbook"
 * (BIFF8) or "Book" stream itself.  NULL on failure; ud_error() distinguishes
 * "not a workbook" from "BIFF5 - not decoded in this build" from "encrypted
 * (FILEPASS) - refused".  The ud_cfb must outlive the ud_xls. */
ud_xls *ud_xls_open(ud_cfb *c);
void    ud_xls_close(ud_xls *x);

int         ud_xls_sheets(const ud_xls *x);
const char *ud_xls_sheet_name(const ud_xls *x, int s);
int         ud_xls_sheet_visible(const ud_xls *x, int s);   /* 0 = hidden   */
int         ud_xls_date1904(const ud_xls *x);  /* the epoch this book uses  */

/* Used extent: one past the last row/column that carries a cell. */
int ud_xls_rows(const ud_xls *x, int s);
int ud_xls_cols(const ud_xls *x, int s);

/* One cell.  Returns 1 and fills *out when a cell record exists there,
 * 0 otherwise (out is zeroed).  Cheap: cells are held sorted and found by
 * binary search. */
int ud_xls_cell(const ud_xls *x, int s, int row, int col, ud_xcell *out);

/* Iterate only the cells that exist, in row-major order - the right way to
 * walk a sparse sheet.  `i` runs 0..ud_xls_cell_count-1. */
int ud_xls_cell_count(const ud_xls *x, int s);
int ud_xls_cell_at(const ud_xls *x, int s, int i, int *row, int *col,
                   ud_xcell *out);

/* The number-format an XF selects: the id, and its format code ("General",
 * "0.00", "d-mmm-yy").  A file stores FORMAT records only for the codes it
 * defines; Excel's built-in ids resolve through an internal table.  This is
 * what tells a date serial apart from a plain number - unodoc does not
 * format numbers itself (that is UnoCalc's uoc_numfmt). */
int         ud_xls_xf_format_id(const ud_xls *x, int xf);
const char *ud_xls_xf_format(const ud_xls *x, int xf);

/* Merged ranges, inclusive on all four bounds. */
int ud_xls_merges(const ud_xls *x, int s);
int ud_xls_merge(const ud_xls *x, int s, int i,
                 int *row0, int *col0, int *row1, int *col1);

/* Excel's row/column limits - the 97 grid, which is also the spec's. */
#define UD_XLS_MAXROW  65536
#define UD_XLS_MAXCOL  256

/* ---- writing a workbook (phase 3) ----------------------------- [EXPERIMENTAL]
 * Same rule as the container: never in place.  Build a model, serialise a
 * fresh file.  ud_xlsw_save hands back a complete .xls - the BIFF8 workbook
 * stream already wrapped in a compound file - ready for uno_fs_write. */
typedef struct ud_xlsw ud_xlsw;

ud_xlsw *ud_xlsw_new(void);
void     ud_xlsw_free(ud_xlsw *w);

/* Add a sheet; returns its index, or -1.  At least one is required. */
int ud_xlsw_sheet(ud_xlsw *w, const char *name);

/* Cells.  Each returns 1 on success.  Writing the same cell twice replaces
 * it, so a caller building from a model does not have to de-duplicate. */
int ud_xlsw_num  (ud_xlsw *w, int s, int row, int col, double v);
int ud_xlsw_str  (ud_xlsw *w, int s, int row, int col, const char *t);
int ud_xlsw_bool (ud_xlsw *w, int s, int row, int col, int v);
int ud_xlsw_err  (ud_xlsw *w, int s, int row, int col, int err);   /* UD_XE_* */
int ud_xlsw_blank(ud_xlsw *w, int s, int row, int col);

/* The number format a cell displays through, as a format code ("0.00",
 * "d-mmm-yy").  Excel's built-in codes are recognised and reused; anything
 * else becomes a FORMAT record.  Applies to the cell already written. */
int ud_xlsw_format(ud_xlsw *w, int s, int row, int col, const char *code);

/* A formula, given as text ("=SUM(A1:A9)", the '=' optional), compiled to
 * tokens immediately - so a syntax error is reported here, at the cell that
 * caused it, rather than surfacing later as a failed save.  `cached` is the
 * result to store alongside it, which is what a reader that does not
 * calculate will display; pass NULL for zero.  0 on failure, ud_error() says
 * what was wrong with the expression. */
int ud_xlsw_formula(ud_xlsw *w, int s, int row, int col, const char *text,
                    const ud_xcell *cached);

int ud_xlsw_merge(ud_xlsw *w, int s, int row0, int col0, int row1, int col1);
int ud_xlsw_date1904(ud_xlsw *w, int on);

/* The whole file in one ud_alloc'd buffer (caller ud_free's it). */
unsigned char *ud_xlsw_save(ud_xlsw *w, long *len);

/* ===========================================================================
 * .doc - the Word 97 document [MS-DOC]                     [EXPERIMENTAL]
 *
 * PHASE 4a (this surface) is the FIB, the piece table and the TEXT.  A .doc
 * does not store its text in one place or one encoding: the WordDocument
 * stream holds runs wherever a quick-save left them, and a piece table in one
 * of the two table streams says which run supplies which part of the
 * document.  Document order is not file order, and each run picks 8-bit or
 * UTF-16 for itself.
 *
 * Formatting (CHPX/PAPX, sprms, styles) is phase 4b; writing is 4c.
 * ======================================================================== */
typedef struct ud_doc ud_doc;

/* Open the document inside an already-open container.  NULL on failure;
 * ud_error() distinguishes "not a Word document" from "Word 6/95 - not
 * decoded in this build" from "password-protected - not opened".  The
 * ud_cfb must outlive the ud_doc. */
ud_doc *ud_doc_open(ud_cfb *c);
void    ud_doc_close(ud_doc *d);

/* The body text exactly as the file stores it: CP-1252, NUL-terminated, and
 * still carrying Word's in-band control characters (0x07 cell mark, 0x0D
 * paragraph mark, 0x13/0x14/0x15 around fields...).  This is what a
 * formatting layer walks. */
long        ud_doc_text_len(const ud_doc *d);
const char *ud_doc_text(const ud_doc *d);

/* The same text as a person would read it: paragraph marks become newlines,
 * cell marks tabs, and a field's CODE is dropped while its CACHED RESULT is
 * kept.  Built once, on demand, and owned by the document. */
const char *ud_doc_plain(ud_doc *d);

/* How many text pieces the table held - introspection for the gate, and a
 * quick way to tell a quick-saved document from a freshly written one. */
int ud_doc_pieces(const ud_doc *d);

/* ---- formatting (phase 4b) -------------------------------------------------
 * Word does not store formatting per character.  It stores RUNS of exceptions
 * - CHPX for characters, PAPX for paragraphs - in 512-byte pages, and a bin
 * table says which page covers which part of the file.  Ask by character
 * position and unodoc does the two lookups.
 *
 * IMPORTANT, and stated here rather than discovered later: these report the
 * DIRECT formatting a document applies, over Word's built-in defaults.  The
 * style hierarchy (STSH, based-on chains) is not resolved yet, so a run whose
 * boldness comes from its paragraph STYLE rather than from direct formatting
 * reads as not-bold.  Resolving that is the next slice. */
typedef struct {
    int bold, italic, underline, strike, caps, smallcaps;
    int size;        /* half-points, so 20 is 10pt; 0 = not set directly   */
    int color;       /* Word's ico palette index, 0 = automatic            */
    int font;        /* index into the document's font table               */
    int super, sub;
} ud_chp;

typedef struct {
    int style;                  /* istd, the paragraph's style index       */
    int align;                  /* 0 left, 1 centre, 2 right, 3 justified  */
    int left, right, first;     /* indents in twips; first may be negative */
    int before, after;          /* spacing in twips                        */
    int keep_next, page_break_before;
} ud_pap;

/* Fill *out with the formatting in force at character position `cp`.
 * Returns 1 if a lookup succeeded (out is always initialised to the
 * defaults first, so a 0 return still leaves something usable). */
int ud_doc_chp_at(ud_doc *d, long cp, ud_chp *out);
int ud_doc_pap_at(ud_doc *d, long cp, ud_pap *out);

/* ---- writing a document (phase 4c) ---------------------------- [EXPERIMENTAL]
 * The minimal layout Word and LibreOffice both accept: one 8-bit text piece,
 * one exception page each for characters and paragraphs, a style sheet with a
 * single Normal style, one section.  Paragraphs go in as text plus a little
 * direct formatting; anything richer waits for the app that needs it. */
typedef struct ud_docw ud_docw;

ud_docw *ud_docw_new(void);
void     ud_docw_free(ud_docw *w);

/* Append a paragraph.  `align` is 0 left, 1 centre, 2 right, 3 justified. */
int ud_docw_para(ud_docw *w, const char *text, int bold, int italic, int align);

/* The whole .doc in one ud_alloc'd buffer (caller ud_free's it). */
unsigned char *ud_docw_save(ud_docw *w, long *len);

/* ===========================================================================
 * .ppt - the PowerPoint 97 presentation [MS-PPT]           [EXPERIMENTAL]
 *
 * PHASE 5a: the persist chain, the live document, and slide text.
 *
 * A .ppt stream is an append-only edit log, and most of what is in it is a
 * previous version of the file.  Finding the live document takes four hops -
 * the Current User stream, the UserEdit chain running newest to oldest, the
 * persist directories folded newest-wins, and only then the document itself.
 * Fold those directories the wrong way round and every object in the
 * presentation resolves to a stale copy of itself.
 *
 * Escher is phase 5b (ud_shape, below); writing is phase 5c (ud_pptw).
 * ======================================================================== */
typedef struct ud_ppt ud_ppt;

ud_ppt *ud_ppt_open(ud_cfb *c);
void    ud_ppt_close(ud_ppt *p);

int ud_ppt_slides(const ud_ppt *p);

/* A slide's text, CP-1252, paragraphs separated by newlines.  Built once on
 * demand and owned by the presentation. */
const char *ud_ppt_slide_text(ud_ppt *p, int i);

/* ---- OfficeArt / Escher (phase 5b) ------------------------------ [EXPERIMENTAL]
 * The drawing layer shared by all three formats, so it takes a byte range
 * rather than a document and each format hands it whatever range holds its
 * drawing.  A shape's TYPE comes from its record header, not its body. */
typedef struct {
    int  kind;            /* the msosptXxx shape type from the Sp header    */
    long id;              /* the shape id, unique within the drawing        */
    int  group;           /* 1 if this is a group rather than a leaf shape  */
    long x0, y0, x1, y1;  /* the anchor rectangle, in the host's units      */
    long fill_color, line_color;   /* -1 when the shape does not set one    */
    long line_width;
    int  filled, lined;
    long text_id;         /* links the shape to its text, 0 if none         */
} ud_shape;

/* Collect the shapes in a byte range, descending into groups.  Returns how
 * many were written.  `depth` starts at 0. */
int ud_escher_shapes(const unsigned char *b, long n, long at, long end,
                     ud_shape *out, int max, int depth);

/* The shapes on one slide, read straight out of its drawing.  Returns how
 * many were written into `out`. */
int ud_ppt_slide_shapes(ud_ppt *p, int i, ud_shape *out, int max);

/* ---- writing a presentation (phase 5c) ------------------------ [EXPERIMENTAL]
 * A single UserEdit and one persist directory - the layout of a fresh save,
 * never an edit log.  Slides carry a title frame and a body frame as plain
 * Escher textboxes; '\n' inside either text is a paragraph break.  Text is
 * CP-1252 in, stored 8-bit when pure ASCII and UTF-16 otherwise. */
typedef struct ud_pptw ud_pptw;

ud_pptw *ud_pptw_new(void);
void     ud_pptw_free(ud_pptw *w);

/* Append a slide; returns its index (or -1). */
int ud_pptw_slide(ud_pptw *w);

int ud_pptw_title(ud_pptw *w, int slide, const char *text);
int ud_pptw_body (ud_pptw *w, int slide, const char *text);

/* The whole .ppt (container included) in one ud_alloc'd buffer. */
unsigned char *ud_pptw_save(ud_pptw *w, long *len);

/* ===========================================================================
 * OOXML - .docx / .xlsx / .pptx                            [EXPERIMENTAL]
 *
 * The 2007 formats are a ZIP of XML parts where the 97 formats are a CFB of
 * binary streams.  Everything above this line is unchanged by them: an OOXML
 * workbook opens into the SAME `ud_xls` handle a BIFF8 workbook does, an
 * OOXML document into the same `ud_doc`, a presentation into the same
 * `ud_ppt`.  That is the whole design.  A consumer written against the 97
 * readers gains the 2007 formats by choosing a different opener, and every
 * accessor, every cell walk and every piece of app code above it is untouched.
 *
 * WHICH OPENER: ask the bytes, never the file name.
 *
 *     ud_src src = { my_read, size, ctx };
 *     switch (ud_sniff(&src)) {
 *     case UD_C_CFB: { ud_cfb *c = ud_cfb_open(&src);  x = ud_xls_open(c);  break; }
 *     case UD_C_ZIP: { ud_zip *z = ud_zip_open(&src);  x = ud_xlsx_open(z); break; }
 *     }
 *     ... identical from here ...
 *
 * ONE NEW OBLIGATION ON CALLERS.  Inflating a ZIP part uses unomedia's
 * um_inflate (PNG's engine, exported by unomedia.h for exactly this), so a
 * program that opens OOXML links unomedia and registers BOTH allocators:
 *
 *     ud_set_alloc(malloc, free);
 *     um_set_alloc(malloc, free);
 *
 * Forgetting the second one makes every part fail to inflate, and ud_error()
 * says so.
 * ======================================================================== */

/* What container a file is, from its first bytes (ud_sniff). */
enum { UD_C_UNKNOWN = 0, UD_C_CFB, UD_C_ZIP };

/* What KIND of document a ZIP holds, from the parts it carries (ud_zip_kind).
 * By its parts and never by its extension: the extension is a hint somebody
 * typed, the parts are what is in the file. */
enum { UD_K_UNKNOWN = 0, UD_K_DOCX, UD_K_XLSX, UD_K_PPTX };

int ud_sniff(const ud_src *src);

typedef struct ud_zip ud_zip;

/* Open a ZIP container: reads the CENTRAL DIRECTORY, not the local headers,
 * because a local header may carry zeroed sizes with the truth in a trailing
 * data descriptor - which is what the streaming writers, Word among them,
 * produce.  NULL on failure; ud_error() distinguishes "not a zip" from
 * "encrypted" from "ZIP64, not supported".  The ud_src must outlive it. */
ud_zip *ud_zip_open(const ud_src *src);
void    ud_zip_close(ud_zip *z);

int         ud_zip_kind (const ud_zip *z);              /* UD_K_*            */
int         ud_zip_parts(const ud_zip *z);
const char *ud_zip_name (const ud_zip *z, int i);       /* "xl/workbook.xml" */
long        ud_zip_size (const ud_zip *z, int i);       /* uncompressed, -1  */

/* Find a part by name.  A leading '/' is accepted (the CFB half of this
 * library spells paths that way) and the match falls back to case-insensitive,
 * because writers disagree about the case of some part names. */
int ud_zip_find(const ud_zip *z, const char *name);

/* One part, inflated, in one ud_alloc'd buffer with a NUL appended - every
 * part unodoc reads is XML, and a NUL means the parser never needs a length
 * it might get wrong.  ud_free it. */
unsigned char *ud_zip_load(ud_zip *z, int i, long *len);

/* ---- the three readers ------------------------------------------------------
 * Each returns the SAME handle type its 97 counterpart does, so everything
 * downstream is shared.  The ud_zip must outlive the handle, exactly as the
 * ud_cfb must outlive a ud_xls. */
ud_xls *ud_xlsx_open(ud_zip *z);      /* close with ud_xls_close  */
ud_doc *ud_docx_open(ud_zip *z);      /* close with ud_doc_close  */
ud_ppt *ud_pptx_open(ud_zip *z);      /* close with ud_ppt_close  */

/* ---- writing ---------------------------------------------------------------
 * The writers mirror the 97 writers: build a model, serialise once.  They
 * take the SAME model objects, so an app that can save .xls can save .xlsx by
 * calling a different serialiser.
 *
 * Parts are written STORED (uncompressed).  A .xlsx with stored parts is a
 * valid .xlsx - the format requires a zip, not a compressed one - and it means
 * unodoc needs no deflate COMPRESSOR to write, only unomedia's decompressor to
 * read.  The cost is file size; a spreadsheet that a user will open in Excel
 * five seconds later is not the place to care. */
unsigned char *ud_xlsxw_save(ud_xlsw *w, long *len);
unsigned char *ud_docxw_save(ud_docw *w, long *len);
unsigned char *ud_pptxw_save(ud_pptw *w, long *len);

/* ===========================================================================
 * The XML pull parser [INTERNAL-ish]
 *
 * Exposed because the readers above are three files that all need it and
 * because a consumer occasionally wants to read a part unodoc does not model.
 * It reports events by pointing INTO the caller's buffer and allocates
 * nothing; see ud_xml.c for why a tree was not an option here.
 * ======================================================================== */
#define UD_XML_ATTRS 24

enum { UD_XML_NONE = 0, UD_XML_START, UD_XML_END, UD_XML_TEXT };

typedef struct {
    const char *name; long nlen;      /* local name, prefix stripped        */
    const char *pfx;  long plen;      /* the prefix, "" when there is none  */
    const char *val;  long vlen;      /* raw, still escaped                 */
} ud_xattr;

typedef struct {
    const char *p;    long n, at;
    int   kind;                       /* UD_XML_*                           */
    const char *name; long nlen;      /* element local name                 */
    const char *pfx;  long plen;
    const char *text; long tlen;      /* UD_XML_TEXT, raw                   */
    ud_xattr attr[UD_XML_ATTRS];
    int   nattr;
    int   depth;                      /* elements currently open            */
    int   empty;                      /* the start tag was self-closing     */
    int   cdata;
} ud_xml;

void ud_xml_init(ud_xml *x, const char *src, long len);
int  ud_xml_next(ud_xml *x);                       /* 0 at end of document  */
int  ud_xml_is  (const ud_xml *x, const char *local);

const char *ud_xml_attr    (const ud_xml *x, const char *name, long *len);
/* Match on PREFIX AND local name.  Needed exactly where an element carries two
 * attributes with the same local name - `<p:sldId id="256" r:id="rId2"/>` is
 * the one that matters, and reading the wrong one there makes every slide
 * resolve to no part.  `pfx` NULL or "" means the unprefixed one. */
const char *ud_xml_attr_ns (const ud_xml *x, const char *pfx, const char *name,
                            long *len);
int         ud_xml_attr_ns_str(const ud_xml *x, const char *pfx,
                               const char *name, char *out, int cap);
int         ud_xml_attr_str(const ud_xml *x, const char *name, char *out, int cap);
long        ud_xml_attr_int(const ud_xml *x, const char *name, long dflt);
/* An OOXML toggle: absent attribute means TRUE (`<w:b/>` is bold), and
 * "0"/"false"/"off" mean false. */
int         ud_xml_attr_bool(const ud_xml *x, const char *name, int dflt);

/* Un-escape into a caller buffer; returns the length written. */
long ud_xml_unescape(const char *s, long n, char *out, long cap);

/* The text of everything inside the current element, entities resolved and
 * nested tags followed - how a Word paragraph or a shared string is read,
 * because the sentence is scattered across runs. */
long ud_xml_inner_text(ud_xml *x, char *out, long cap);

/* Step past the current element and everything in it.  This is what keeps an
 * unknown subtree from derailing a walk. */
void ud_xml_skip(ud_xml *x);

/* ---- name comparison (exposed: the format layers sort names too) ---------- */
/* CFB directory order: shorter names first, then uppercased code unit by
 * code unit.  <0, 0, >0 like strcmp. */
int ud_name_cmp(const char *a, const char *b);

#endif /* UNODOC_H */
