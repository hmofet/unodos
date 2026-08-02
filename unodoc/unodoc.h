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

/* ---- name comparison (exposed: the format layers sort names too) ---------- */
/* CFB directory order: shorter names first, then uppercased code unit by
 * code unit.  <0, 0, >0 like strcmp. */
int ud_name_cmp(const char *a, const char *b);

#endif /* UNODOC_H */
