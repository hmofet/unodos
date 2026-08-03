/* ===========================================================================
 * uoword - UnoWord's document model and page layout.       [EXPERIMENTAL]
 * (OFFICE97-PLAN §5 phase 7; the conformance items are OFFICE97-SPEC.md
 * S-UOW-04 and S-UOW-05.)
 *
 * TWO THINGS THIS IS NOT.
 *
 * It is not pc64's Editor (`pc64_write.c`).  That one is a genuinely useful
 * WordPad-class app and UnoWord is its descendant, but its model is parallel
 * arrays with a 16-bit style word and its layout wraps to the WINDOW.  A word
 * processor wraps to the PAGE, and no amount of teaching wr_layout gets there.
 *
 * It is not unoweb either.  unoweb is a real CSS block/inline formatter and
 * the only general text layout in the tree, but it is render-only - no caret,
 * no selection, no incremental relayout for typing - and it has no table
 * layout and no pagination.  Consuming it would mean asking its owner for all
 * three.  So the document lane is its own, and unoweb stays a neutral API.
 *
 * THE TEXT STORE IS A PIECE TABLE, which is the .doc lesson applied in
 * memory: edits append to a growing buffer and the piece list says what order
 * to read it in.  That is what makes undo cheap - an inverse command holds
 * offsets, not copies of the document.
 *
 * METRICS COME THROUGH A SEAM.  Layout never calls the font engine directly:
 * pc64 installs a uow_metrics wrapping uno_font_*_styled, the host gate
 * installs one over fb_text_w, and uow_layout.c never learns which.  Same
 * trick as unodoc's ud_src and uofile's uof_fs, and it is what lets the
 * layout engine be tested without booting the OS.
 * ======================================================================== */
#ifndef UOWORD_H
#define UOWORD_H

#include "uochrome.h"

/* ---- character formatting -------------------------------------------------- */
typedef struct {
    unsigned short face;      /* index into the document's font table         */
    unsigned short size;      /* half-points, so 20 is 10pt                   */
    unsigned char  bold, italic, underline, strike;
    unsigned char  caps, smallcaps, super, sub;
    unsigned char  hidden;
    fb_px          color;     /* 0 = automatic                                */
    fb_px          highlight; /* 0 = none                                     */
} uow_chp;

/* ---- paragraph formatting --------------------------------------------------
 * Alignment, the indents Word's ruler drags, spacing, and the flow controls
 * pagination has to honour. */
enum { UOW_AL_LEFT = 0, UOW_AL_CENTER, UOW_AL_RIGHT, UOW_AL_JUSTIFY };
enum { UOW_LS_SINGLE = 0, UOW_LS_ONEHALF, UOW_LS_DOUBLE, UOW_LS_EXACT,
       UOW_LS_ATLEAST };

typedef struct {
    unsigned short style;     /* index into the style table                   */
    unsigned char  align;     /* UOW_AL_*                                     */
    short left, right, first; /* indents in twips; first may be negative      */
    short before, after;      /* spacing in twips                             */
    unsigned char  linerule;  /* UOW_LS_*                                     */
    short lineval;            /* twips, for EXACT / ATLEAST                   */
    unsigned char  keep_next, keep_lines, page_before, widow;
    unsigned char  list_level, list_kind;   /* 0 none, 1 bullet, 2 number     */
} uow_pap;

/* ---- named styles ---------------------------------------------------------
 * Based-on chains resolve root-first, exactly as .doc's STSH does, so a
 * derived style overrides its parent and direct formatting overrides both. */
#define UOW_MAXSTYLE 64
#define UOW_STYLENAME 32

typedef struct {
    char name[UOW_STYLENAME];
    int  based_on;            /* -1 for a root style                          */
    int  next;                /* the style for the following paragraph        */
    uow_chp chp;
    uow_pap pap;
    unsigned char has_chp, has_pap, used;
} uow_style;

/* the built-ins UnoWord seeds a new document with */
enum { UOW_STY_NORMAL = 0, UOW_STY_H1, UOW_STY_H2, UOW_STY_H3,
       UOW_STY_BODY, UOW_STY_TITLE, UOW_STY_LIST, UOW_STY_HEADER,
       UOW_STY_FOOTER, UOW_STY_CAPTION, UOW_STY_NBUILTIN };

/* ---- the section: page geometry ------------------------------------------- */
typedef struct {
    int page_w, page_h;                 /* twips; Letter is 12240 x 15840     */
    int margin_l, margin_r, margin_t, margin_b;
    int header_from, footer_from;
    int columns, column_gap;
    unsigned char landscape, title_page, facing;
} uow_sect;

/* ---- the document ---------------------------------------------------------- */
#define UOW_MAXPIECE 512
#define UOW_MAXRUN   1024
#define UOW_MAXUNDO  64
#define UOW_ADDCAP   (192 * 1024)
#define UOW_ORIGCAP  (64 * 1024)

typedef struct { int buf; long off, len; } uow_piece;   /* buf 0=orig 1=add   */
typedef struct { long len; uow_chp chp; } uow_crun;
typedef struct { long len; uow_pap pap; } uow_prun;

typedef struct uow_doc uow_doc;

/* Create an empty document seeded with the built-in styles, or one holding
 * `text` as a single Normal paragraph run. */
uow_doc *uow_new(void);
void     uow_free(uow_doc *d);

long        uow_len(const uow_doc *d);
/* Copy `n` characters from `cp` into `out`; returns how many were copied.
 * The piece table means this is the ONLY way to read text - there is no flat
 * buffer to point at, by design. */
long        uow_read(const uow_doc *d, long cp, long n, char *out);
int         uow_char_at(const uow_doc *d, long cp);

/* Every mutation goes through one of these, and every one of them records an
 * inverse on the undo stack.  Nothing else may touch the model. */
int  uow_insert(uow_doc *d, long cp, const char *s, long n);
int  uow_delete(uow_doc *d, long cp, long n);
int  uow_format(uow_doc *d, long cp, long n, const uow_chp *c);
int  uow_format_para(uow_doc *d, long cp, long n, const uow_pap *p);
int  uow_set_style(uow_doc *d, long cp, long n, int style);

int  uow_undo(uow_doc *d);
int  uow_redo(uow_doc *d);
int  uow_can_undo(const uow_doc *d);
int  uow_can_redo(const uow_doc *d);
/* The name of what undo would reverse, for the Edit menu's "Undo Typing". */
const char *uow_undo_name(const uow_doc *d);

/* Resolved formatting at a character position: the style chain first, then
 * the direct run.  Same four-layer order as unodoc's .doc reader, and for the
 * same reason - get it backwards and direct formatting loses to the style it
 * was meant to override. */
void uow_chp_at(const uow_doc *d, long cp, uow_chp *out);
void uow_pap_at(const uow_doc *d, long cp, uow_pap *out);

/* Paragraph boundaries.  A paragraph ends at '\r', .doc's own mark. */
long uow_para_start(const uow_doc *d, long cp);
long uow_para_end(const uow_doc *d, long cp);
int  uow_para_count(const uow_doc *d);

/* Styles */
uow_style *uow_style_at(uow_doc *d, int i);
int        uow_style_find(const uow_doc *d, const char *name);
int        uow_style_add(uow_doc *d, const char *name, int based_on);

uow_sect  *uow_section(uow_doc *d);

/* A revision counter, bumped by every mutation.  Layout caches against it. */
unsigned uow_revision(const uow_doc *d);

/* ---- the metrics seam ------------------------------------------------------
 * The only way layout learns how wide anything is. */
typedef struct {
    int (*text_w)(const char *s, long n, const uow_chp *c, void *ctx);
    int (*height)(const uow_chp *c, void *ctx);
    int (*baseline)(const uow_chp *c, void *ctx);
    int (*space_w)(const uow_chp *c, void *ctx);
    void *ctx;
} uow_metrics;

/* ---- layout ----------------------------------------------------------------
 * The output is a display list a painter walks: pages, each holding lines,
 * each holding runs of one formatting.  Coordinates are DOCUMENT pixels, so
 * scrolling never relayouts - the same rule unoweb states. */
#define UOW_MAXLINE  1024
#define UOW_MAXPAGE  64
#define UOW_MAXLRUN  8192

typedef struct {
    long cp, n;               /* the text this run draws                      */
    int  x, w;                /* relative to the line's left edge             */
    uow_chp chp;
} uow_lrun;

typedef struct {
    long cp, n;               /* the whole line's text range                  */
    int  x, y, w, h, baseline;/* document pixels; x/y absolute in the page    */
    int  run0, nrun;
    int  page;
    unsigned char first_of_para, last_of_para;
} uow_line;

typedef struct {
    int x, y, w, h;           /* the paper, in document pixels                */
    int text_x, text_y, text_w, text_h;
    int line0, nline;
} uow_page;

typedef struct {
    uow_line line[UOW_MAXLINE];
    uow_lrun run[UOW_MAXLRUN];
    uow_page page[UOW_MAXPAGE];
    int nline, nrun, npage;
    int zoom;                 /* percent                                      */
    int doc_w, doc_h;         /* the whole pasteboard's extent                */
    unsigned rev;             /* the document revision this reflects          */
    int twips_per_px;
} uow_layout;

/* Lay `d` out at `zoom` percent.  Returns 0 if it ran out of line or page
 * capacity, having laid out as much as it could - a long document is
 * truncated honestly rather than silently. */
int  uow_layout_run(uow_layout *L, const uow_doc *d, const uow_metrics *m,
                    int zoom);

/* Which line holds `cp`, and where the caret sits inside it. */
int  uow_line_of(const uow_layout *L, long cp);
int  uow_caret_x(const uow_layout *L, const uow_metrics *m, long cp);
/* The character position nearest a document point - what a click means. */
long uow_cp_at(const uow_layout *L, const uow_metrics *m, int x, int y);

#endif /* UOWORD_H */
