/* ===========================================================================
 * ud_pptw.c - writing a PowerPoint 97 presentation [MS-PPT], phase 5c.
 *
 * Reading a .ppt means surviving the append-only edit log; writing one means
 * NOT writing an edit log at all: a single UserEdit, one persist directory,
 * every object live.  That is what every fresh save from PowerPoint itself
 * looks like, and it is the only layout a writer should ever produce.
 *
 * The shape of the file mirrors what LibreOffice's own 97 filter emits for a
 * fresh presentation (read back record by record from the corpus, not from
 * anyone's code), minus what experiment showed Impress does not require:
 *
 *   PowerPoint Document stream:
 *     DocumentContainer        (persist id 1)
 *       DocumentAtom, DrawingGroup (the Escher Dgg shape-id ledger),
 *       SlideListWithText[master], SlideListWithText[slides], EndDocumentAtom
 *     MainMasterContainer      (persist id 2) - SlideAtom, colour schemes,
 *       minimal TxMasterStyleAtoms (title + body)
 *     SlideContainer x N       (persist ids 3..) - SlideAtom + PPDrawing
 *     PersistDirectoryAtom
 *     UserEditAtom             (the only edit)
 *   Current User stream: one CurrentUserAtom pointing at that UserEditAtom.
 *
 * Slide text goes out as PLAIN Escher textboxes (msosptTextBox SpContainers
 * with a ClientTextbox holding TextHeaderAtom + a text atom) - which is
 * exactly how LibreOffice writes slide text, what our reader's tree walk
 * finds, and what keeps this writer out of the placeholder/outline-text
 * machinery a first slice does not need.
 *
 * Text encoding is the either/or met three times on the read side: a
 * paragraph that is pure ASCII goes out as TextBytesAtom, anything else as
 * UTF-16 TextCharsAtom.  TextBytesAtom stores the LOW BYTE of a UTF-16 code
 * unit (Latin-1), and CP-1252's 0x80..0x9F are NOT Latin-1 - so the split is
 * on ASCII, not on "fits in a byte", and no CP-1252 special ever goes out
 * through the bytes form.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_ooxw_int.h"
#include <string.h>

/* PPT record types (writer's set) */
#define R_DOCUMENT       0x03E8
#define R_DOCATOM        0x03E9
#define R_ENDDOC         0x03EA
#define R_SLIDE          0x03EE
#define R_SLIDEATOM      0x03EF
#define R_MAINMASTER     0x03F8
#define R_SLIDEPERSIST   0x03F3
#define R_DRAWGROUP      0x040B
#define R_PPDRAWING      0x040C
#define R_COLORSCHEME    0x07F0
#define R_TXMASTERSTYLE  0x0FA3
#define R_TEXTHEADER     0x0F9F
#define R_TEXTCHARS      0x0FA0
#define R_TEXTBYTES      0x0FA8
#define R_SLIDELIST      0x0FF0
#define R_USEREDIT       0x0FF5
#define R_CURRENTUSER    0x0FF6
#define R_PERSISTDIR     0x1772

/* Escher record types */
#define E_DGGCONTAINER   0xF000
#define E_BSTORE         0xF001
#define E_DGCONTAINER    0xF002
#define E_SPGRCONTAINER  0xF003
#define E_SPCONTAINER    0xF004
#define E_DGG            0xF006
#define E_DG             0xF008
#define E_SPGR           0xF009
#define E_SP             0xF00A
#define E_OPT            0xF00B
#define E_CLIENTTEXTBOX  0xF00D
#define E_CLIENTANCHOR   0xF010

#define SPT_TEXTBOX      202          /* msosptTextBox                       */
#define SP_FLAGS_SHAPE   0x00000A00uL /* fHaveAnchor | fHaveSpt              */
#define SP_FLAGS_PATRI   0x00000005uL /* fGroup | fPatriarch                 */

#define CU_MAGIC         0xE391C05FuL
#define MASTER_SLIDEID   0x80000000uL
#define FIRST_SLIDEID    256uL

/* Slide coordinates are master units, 1/576 inch: the classic 10 x 7.5 inch
 * on-screen show.  The two text frames echo PowerPoint's own title/body
 * geometry closely enough to look intentional in any viewer. */
#define SLIDE_CX 5760
#define SLIDE_CY 4320
#define TITLE_T   300
#define TITLE_L   460
#define TITLE_R  (SLIDE_CX - 460)
#define TITLE_B  1060
#define BODY_T   1360
#define BODY_L    460
#define BODY_R   (SLIDE_CX - 460)
#define BODY_B   (SLIDE_CY - 300)

/* ---- the model ------------------------------------------------------------- */
typedef struct { char *title; char *body; } pwslide;

struct ud_pptw {
    pwslide *sl;
    int      nsl, cap;
    int      bad;
};

ud_pptw *ud_pptw_new(void)
{
    ud_pptw *w = (ud_pptw *)ud_alloc(sizeof(ud_pptw));
    if (!w) { ud_set_error("out of memory"); return 0; }
    memset(w, 0, sizeof *w);
    return w;
}

void ud_pptw_free(ud_pptw *w)
{
    int i;
    if (!w) return;
    for (i = 0; i < w->nsl; i++) { ud_free(w->sl[i].title); ud_free(w->sl[i].body); }
    ud_free(w->sl);
    ud_free(w);
}

int ud_pptw_slide(ud_pptw *w)
{
    if (!w) return -1;
    if (w->nsl >= w->cap) {
        int cap = w->cap ? w->cap * 2 : 8;
        pwslide *n = (pwslide *)ud_alloc((unsigned long)cap * sizeof(pwslide));
        if (!n) { ud_set_error("out of memory"); w->bad = 1; return -1; }
        if (w->nsl) memcpy(n, w->sl, (unsigned long)w->nsl * sizeof(pwslide));
        ud_free(w->sl);
        w->sl = n; w->cap = cap;
    }
    memset(&w->sl[w->nsl], 0, sizeof(pwslide));
    return w->nsl++;
}

static int set_text(ud_pptw *w, char **slot, const char *text)
{
    unsigned long n;
    char *c;
    if (!w || !text) return 0;
    n = (unsigned long)strlen(text);
    c = (char *)ud_alloc(n + 1);
    if (!c) { ud_set_error("out of memory"); w->bad = 1; return 0; }
    memcpy(c, text, n + 1);
    ud_free(*slot);
    *slot = c;
    return 1;
}

int ud_pptw_title(ud_pptw *w, int slide, const char *text)
{
    if (!w || slide < 0 || slide >= w->nsl) return 0;
    return set_text(w, &w->sl[slide].title, text);
}

int ud_pptw_body(ud_pptw *w, int slide, const char *text)
{
    if (!w || slide < 0 || slide >= w->nsl) return 0;
    return set_text(w, &w->sl[slide].body, text);
}

/* ---- a byte buffer with container fixups -----------------------------------
 * Records nest and every header carries the length of everything under it, so
 * a container is OPENED (header written with length 0, offset remembered) and
 * CLOSED (length patched) around its children.  Depth 8 covers the deepest
 * nesting this writer produces (document > drawing > dgg > bstore). */
typedef struct {
    unsigned char *p;
    long n, cap;
    int bad;
    long fix[8];
    int nfix;
} pwbuf;

static int pwneed(pwbuf *b, long n)
{
    if (b->bad) return 0;
    if (b->n + n > b->cap) {
        long cap = b->cap ? b->cap * 2 : 4096;
        unsigned char *np;
        while (cap < b->n + n) cap *= 2;
        np = (unsigned char *)ud_alloc((unsigned long)cap);
        if (!np) { b->bad = 1; return 0; }
        if (b->n) memcpy(np, b->p, (unsigned long)b->n);
        ud_free(b->p);
        b->p = np; b->cap = cap;
    }
    return 1;
}
static void pwput(pwbuf *b, const void *d, long n)
{ if (pwneed(b, n)) { memcpy(b->p + b->n, d, (unsigned long)n); b->n += n; } }
static void pw16(pwbuf *b, unsigned v)
{ if (pwneed(b, 2)) { ud_wr16(b->p + b->n, (uint16_t)v); b->n += 2; } }
static void pw32(pwbuf *b, unsigned long v)
{ if (pwneed(b, 4)) { ud_wr32(b->p + b->n, (uint32_t)v); b->n += 4; } }

/* An atom whose length is known: header, then the caller writes the body. */
static void pwatom(pwbuf *b, unsigned verinst, unsigned type, unsigned long len)
{ pw16(b, verinst); pw16(b, type); pw32(b, len); }

/* A container (or an atom sized after the fact). */
static void pwopen(pwbuf *b, unsigned verinst, unsigned type)
{
    if (b->nfix >= 8) { b->bad = 1; return; }
    pwatom(b, verinst, type, 0);
    b->fix[b->nfix++] = b->n;                /* body starts here             */
}
static void pwclose(pwbuf *b)
{
    long at;
    if (b->bad || b->nfix <= 0) return;
    at = b->fix[--b->nfix];
    ud_wr32(b->p + at - 4, (uint32_t)(b->n - at));
}

/* ---- pieces ---------------------------------------------------------------- */

/* One paragraph run of text: ASCII goes out 8-bit, anything else UTF-16.
 * '\n' in the input is the caller's paragraph break and becomes 0x0D, which
 * is what PPT text atoms use. */
static void put_text_atom(pwbuf *b, const char *s)
{
    long i, n = (long)strlen(s);
    int wide = 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)s[i] >= 0x80) { wide = 1; break; }
    if (!wide) {
        pwatom(b, 0x0000, R_TEXTBYTES, (unsigned long)n);
        for (i = 0; i < n; i++) {
            unsigned char c = (s[i] == '\n') ? 0x0D : (unsigned char)s[i];
            pwput(b, &c, 1);
        }
    } else {
        pwatom(b, 0x0000, R_TEXTCHARS, (unsigned long)n * 2);
        for (i = 0; i < n; i++) {
            uint16_t u = (s[i] == '\n') ? 0x0D
                       : ud_cp1252_to_uc((unsigned char)s[i]);
            pw16(b, u);
        }
    }
}

/* One plain textbox: Sp + OPT(lTxid) + ClientAnchor + ClientTextbox.  The
 * anchor is PowerPoint's own client-anchor shape: four int16, TOP LEFT RIGHT
 * BOTTOM - the 8-byte form the reader distinguishes from Excel's 16-byte one
 * by length. */
static void put_textbox(pwbuf *b, unsigned long spid, unsigned long txid,
                        int t, int l, int r, int bo, const char *text)
{
    pwopen(b, 0x000F, E_SPCONTAINER);
    pwatom(b, (unsigned)(0x0002 | (SPT_TEXTBOX << 4)), E_SP, 8);
    pw32(b, spid);
    pw32(b, SP_FLAGS_SHAPE);
    pwatom(b, (unsigned)(0x0003 | (1u << 4)), E_OPT, 6);   /* one property   */
    pw16(b, 0x0080);                                       /* lTxid          */
    pw32(b, txid);
    pwatom(b, 0x0000, E_CLIENTANCHOR, 8);
    pw16(b, (unsigned)t); pw16(b, (unsigned)l);
    pw16(b, (unsigned)r); pw16(b, (unsigned)bo);
    pwopen(b, 0x000F, E_CLIENTTEXTBOX);
    pwatom(b, 0x0000, R_TEXTHEADER, 4);
    pw32(b, 4);                                            /* Tx_TYPE_OTHER  */
    put_text_atom(b, text);
    pwclose(b);                                            /* ClientTextbox  */
    pwclose(b);                                            /* SpContainer    */
}

/* The eight scheme colours, BGR0 as the file stores them: background, text,
 * shadow, title, fill, accent, accent/hyperlink, accent/followed. */
static void put_scheme(pwbuf *b, unsigned inst)
{
    static const unsigned long c[8] = {
        0x00FFFFFFuL, 0x00000000uL, 0x00808080uL, 0x00000000uL,
        0x0099CC00uL, 0x00CC3333uL, 0x00CC00CCuL, 0x00999999uL
    };
    int i;
    pwatom(b, (unsigned)(inst << 4), R_COLORSCHEME, 32);
    for (i = 0; i < 8; i++) pw32(b, c[i]);
}

/* A master text style with one level and empty property masks: every value
 * defaults, honestly, instead of a canned blob nobody can read. */
static void put_txmasterstyle(pwbuf *b, unsigned inst)
{
    pwatom(b, (unsigned)(inst << 4), R_TXMASTERSTYLE, 10);
    pw16(b, 1);                       /* cLevels                             */
    pw32(b, 0);                       /* paragraph mask: nothing set         */
    pw32(b, 0);                       /* character mask: nothing set         */
}

/* ---- save ------------------------------------------------------------------ */
unsigned char *ud_pptw_save(ud_pptw *w, long *len)
{
    pwbuf d;
    unsigned char cu[64];
    long off_master, off_pdir, off_ue, culen;
    long *off_slide = 0;
    int i, nsl;
    unsigned long total_sp;
    ud_cfbw *cw;
    unsigned char *out = 0;
    static const unsigned char ppt_clsid[16] = {
        0x10, 0x8d, 0x81, 0x64, 0x9b, 0x4f, 0xcf, 0x11,
        0x86, 0xea, 0x00, 0xaa, 0x00, 0xb9, 0x29, 0xe8
    };

    ud_set_error("");
    if (len) *len = 0;
    if (!w || w->bad) { ud_set_error("presentation writer in error state"); return 0; }
    if (!w->nsl) { ud_set_error("a presentation needs at least one slide"); return 0; }
    nsl = w->nsl;

    off_slide = (long *)ud_alloc((unsigned long)nsl * sizeof(long));
    if (!off_slide) { ud_set_error("out of memory"); return 0; }

    memset(&d, 0, sizeof d);

    /* Each slide's drawing: a patriarch group plus up to two textboxes.
     * Shape ids live in per-drawing clusters of 1024, drawing i+1 owning
     * (i+1)*1024.., exactly the ledger the Dgg advertises below. */
    total_sp = 0;
    for (i = 0; i < nsl; i++)
        total_sp += 1u + (w->sl[i].title ? 1u : 0u) + (w->sl[i].body ? 1u : 0u);

    /* ---- DocumentContainer (persist id 1) -------------------------------- */
    pwopen(&d, 0x000F, R_DOCUMENT);

    pwatom(&d, 0x0001, R_DOCATOM, 40);
    pw32(&d, SLIDE_CX); pw32(&d, SLIDE_CY);       /* slide size              */
    pw32(&d, SLIDE_CY); pw32(&d, SLIDE_CX);       /* notes size (portrait)   */
    pw32(&d, 1); pw32(&d, 2);                     /* server zoom 1:2         */
    pw32(&d, 0);                                  /* no notes master         */
    pw32(&d, 0);                                  /* no handout master       */
    pw16(&d, 1);                                  /* first slide number      */
    pw16(&d, 0);                                  /* SS_OnScreen             */
    pw16(&d, 0);                                  /* saveWithFonts, omitTitle*/
    pw16(&d, 0);                                  /* rightToLeft, comments   */

    /* The Escher drawing-group ledger.  Impress checks the ids it hands out
     * against this, so it is bookkeeping, not decoration: one FIDCL cluster
     * per slide drawing, cidcl one MORE than the cluster count (the format
     * counts a phantom, as the corpus files confirm). */
    pwopen(&d, 0x000F, R_DRAWGROUP);
    pwopen(&d, 0x000F, E_DGGCONTAINER);
    pwatom(&d, 0x0000, E_DGG, (unsigned long)(16 + 8 * nsl));
    pw32(&d, (unsigned long)nsl * 1024uL + total_sp);  /* spidMax           */
    pw32(&d, (unsigned long)nsl + 1);                  /* cidcl             */
    pw32(&d, total_sp);                                /* cspSaved          */
    pw32(&d, (unsigned long)nsl);                      /* cdgSaved          */
    for (i = 0; i < nsl; i++) {
        pw32(&d, (unsigned long)i + 1);                /* dgid              */
        pw32(&d, 1u + (w->sl[i].title ? 1u : 0u)
                    + (w->sl[i].body ? 1u : 0u));      /* shapes used       */
    }
    pwatom(&d, (unsigned)(0x000F | (1u << 4)), E_BSTORE, 0);  /* no pictures */
    pwclose(&d);                                  /* DggContainer            */
    pwclose(&d);                                  /* DrawingGroup            */

    /* the master list, then the slide list, each a SlidePersistAtom row */
    pwopen(&d, (unsigned)(0x000F | (1u << 4)), R_SLIDELIST);
    pwatom(&d, 0x0000, R_SLIDEPERSIST, 20);
    pw32(&d, 2);                                  /* persist id of the master*/
    pw32(&d, 0); pw32(&d, 0);
    pw32(&d, MASTER_SLIDEID);
    pw32(&d, 0);
    pwclose(&d);

    pwopen(&d, 0x000F, R_SLIDELIST);
    for (i = 0; i < nsl; i++) {
        pwatom(&d, 0x0000, R_SLIDEPERSIST, 20);
        pw32(&d, (unsigned long)i + 3);           /* persist id              */
        pw32(&d, 4);                              /* has non-placeholder shapes */
        pw32(&d, 0);
        pw32(&d, FIRST_SLIDEID + (unsigned long)i);
        pw32(&d, 0);
    }
    pwclose(&d);

    pwatom(&d, 0x0000, R_ENDDOC, 0);
    pwclose(&d);                                  /* DocumentContainer       */

    /* ---- MainMasterContainer (persist id 2) ------------------------------ */
    off_master = d.n;
    pwopen(&d, 0x000F, R_MAINMASTER);
    pwatom(&d, 0x0002, R_SLIDEATOM, 24);
    pw32(&d, 1);                                  /* geom: title+body        */
    pw32(&d, 0x00000201uL); pw32(&d, 0);          /* placeholder id bytes    */
    pw32(&d, 0);                                  /* a master follows nobody */
    pw32(&d, 0);                                  /* no notes                */
    pw16(&d, 0); pw16(&d, 0);                     /* flags, unused           */
    put_scheme(&d, 6);                            /* the scheme gallery      */
    put_scheme(&d, 1);                            /* the applied scheme      */
    put_txmasterstyle(&d, 0);                     /* title defaults          */
    put_txmasterstyle(&d, 1);                     /* body defaults           */
    pwclose(&d);

    /* ---- the slides (persist ids 3..) ------------------------------------ */
    for (i = 0; i < nsl; i++) {
        unsigned long spid = ((unsigned long)i + 1) * 1024uL;
        unsigned long txid = (unsigned long)i * 8uL + 1uL;
        int nshape = 1 + (w->sl[i].title ? 1 : 0) + (w->sl[i].body ? 1 : 0);

        off_slide[i] = d.n;
        pwopen(&d, 0x000F, R_SLIDE);
        pwatom(&d, 0x0002, R_SLIDEATOM, 24);
        pw32(&d, 16);                             /* geom: blank             */
        pw32(&d, 0); pw32(&d, 0);                 /* no placeholders         */
        pw32(&d, MASTER_SLIDEID);                 /* follows the master      */
        pw32(&d, 0);                              /* no notes                */
        pw16(&d, 7); pw16(&d, 0);                 /* master bg+scheme+objects*/

        pwopen(&d, 0x000F, R_PPDRAWING);
        pwopen(&d, 0x000F, E_DGCONTAINER);
        pwatom(&d, (unsigned)((unsigned)(i + 1) << 4), E_DG, 8);
        pw32(&d, (unsigned long)nshape);
        pw32(&d, spid + (unsigned long)nshape - 1);   /* last spid used      */
        pwopen(&d, 0x000F, E_SPGRCONTAINER);

        pwopen(&d, 0x000F, E_SPCONTAINER);        /* the patriarch group     */
        pwatom(&d, 0x0001, E_SPGR, 16);
        pw32(&d, 0); pw32(&d, 0); pw32(&d, 0); pw32(&d, 0);
        pwatom(&d, 0x0002, E_SP, 8);
        pw32(&d, spid);
        pw32(&d, SP_FLAGS_PATRI);
        pwclose(&d);

        if (w->sl[i].title)
            put_textbox(&d, spid + 1, txid,
                        TITLE_T, TITLE_L, TITLE_R, TITLE_B, w->sl[i].title);
        if (w->sl[i].body)
            put_textbox(&d, spid + (w->sl[i].title ? 2 : 1), txid + 1,
                        BODY_T, BODY_L, BODY_R, BODY_B, w->sl[i].body);

        pwclose(&d);                              /* SpgrContainer           */
        pwclose(&d);                              /* DgContainer             */
        pwclose(&d);                              /* PPDrawing               */
        put_scheme(&d, 1);                        /* the applied scheme      */
        pwclose(&d);                              /* SlideContainer          */
    }

    /* ---- one persist directory, one user edit ---------------------------- */
    off_pdir = d.n;
    pwatom(&d, 0x0000, R_PERSISTDIR, (unsigned long)(4 + 4 * (nsl + 2)));
    pw32(&d, 1uL | ((unsigned long)(nsl + 2) << 20));  /* ids 1.., n entries */
    pw32(&d, 0);                                  /* id 1: the document      */
    pw32(&d, (unsigned long)off_master);          /* id 2: the master        */
    for (i = 0; i < nsl; i++)
        pw32(&d, (unsigned long)off_slide[i]);    /* ids 3..: the slides     */

    off_ue = d.n;
    pwatom(&d, 0x0000, R_USEREDIT, 28);
    pw32(&d, FIRST_SLIDEID);                      /* last slide viewed       */
    pw16(&d, 0x03F4);                             /* version                 */
    pw16(&d, 0x0300);                             /* minor 0, major 3        */
    pw32(&d, 0);                                  /* NO previous edit        */
    pw32(&d, (unsigned long)off_pdir);
    pw32(&d, 1);                                  /* the document persist id */
    pw32(&d, (unsigned long)nsl + 3);             /* next free persist id    */
    pw16(&d, 2); pw16(&d, 0);                     /* slide view, unused      */

    ud_free(off_slide);
    off_slide = 0;
    if (d.bad) { ud_free(d.p); ud_set_error("out of memory"); return 0; }

    /* ---- the Current User stream ----------------------------------------- */
    memset(cu, 0, sizeof cu);
    ud_wr16(cu + 0, 0x0000);
    ud_wr16(cu + 2, R_CURRENTUSER);
    ud_wr32(cu + 8,  0x14);                       /* size of the fixed part  */
    ud_wr32(cu + 12, CU_MAGIC);                   /* not encrypted           */
    ud_wr32(cu + 16, (unsigned long)off_ue);
    ud_wr16(cu + 20, 6);                          /* user name length        */
    ud_wr16(cu + 22, 0x03F4);                     /* docFileVersion          */
    cu[24] = 3; cu[25] = 0;                       /* major, minor            */
    ud_wr16(cu + 26, 0);
    memcpy(cu + 28, "unodoc", 6);
    ud_wr32(cu + 34, 8);                          /* release version         */
    culen = 38;
    ud_wr32(cu + 4, (unsigned long)(culen - 8));  /* record length           */

    /* ---- wrap both streams in a container -------------------------------- */
    cw = ud_cfbw_new();
    if (!cw) { ud_free(d.p); return 0; }
    ud_cfbw_clsid(cw, UD_CFB_ROOT_ID, ppt_clsid);
    /* UD_CFB_NONE is -1, so these are compared, not truth-tested.  A failed
     * _take never took ownership, which is what makes the free here safe. */
    if (ud_cfbw_stream(cw, UD_CFB_ROOT_ID, "Current User",
                       cu, culen) == UD_CFB_NONE ||
        ud_cfbw_stream_take(cw, UD_CFB_ROOT_ID, "PowerPoint Document",
                            d.p, d.n) == UD_CFB_NONE) {
        ud_free(d.p);
        ud_cfbw_free(cw);
        return 0;
    }
    /* d.p now belongs to the container model */
    out = ud_cfbw_serialize(cw, len);
    ud_cfbw_free(cw);
    return out;
}

/* ---- the read-back seam (ud_ooxw_int.h) ------------------------------------
 * ud_ooxw.c serialises this same model as .pptx. */
int ud_pptw_nslides(const ud_pptw *w) { return w ? w->nsl : 0; }

const char *ud_pptw_title_at(const ud_pptw *w, int i)
{
    if (!w || i < 0 || i >= w->nsl) return 0;
    return w->sl[i].title;
}

const char *ud_pptw_body_at(const ud_pptw *w, int i)
{
    if (!w || i < 0 || i >= w->nsl) return 0;
    return w->sl[i].body;
}
