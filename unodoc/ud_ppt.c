/* ===========================================================================
 * ud_ppt.c - the PowerPoint 97 presentation [MS-PPT], read side, phase 5a:
 * the persist chain, the live document, and slide text.
 *
 * A .ppt is the strangest of the three formats to READ, because the stream is
 * an append-only edit log and most of what is in it is dead.  Finding the
 * live document takes four hops and every one of them is a chance to read a
 * previous version of the file by mistake:
 *
 *   1. the "Current User" stream says where the CURRENT edit begins - not the
 *      start of the document, and not the end of the stream;
 *   2. that UserEditAtom points BACK to the previous one, and so on: the
 *      chain runs newest to oldest;
 *   3. each edit carries a persist directory mapping object ids to offsets,
 *      and the same id appears in several of them.  THE FIRST ONE WINS,
 *      because that is the newest.  Fold them oldest-first and every object
 *      resolves to a stale copy of itself;
 *   4. only then does docPersistIdRef name the live DocumentContainer.
 *
 * Phase 5a stops at text.  Escher (the drawing layer) and writing are 5b/5c;
 * text is split out first because it is what proves the persist walk, which
 * everything else is built on.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include <string.h>

/* record types */
#define R_DOCUMENT       0x03E8
#define R_SLIDE          0x03EE
#define R_SLIDEPERSIST   0x03F3
#define R_SLIDELIST      0x0FF0
#define R_TEXTCHARS      0x0FA0
#define R_TEXTBYTES      0x0FA8
#define R_USEREDIT       0x0FF5
#define R_PERSISTDIR     0x1772

#define CU_MAGIC         0xE391C05FuL
#define HDR              8            /* every record: ver/inst, type, len   */

#define MAX_STREAM       (64L * 1024 * 1024)
#define MAX_PERSIST      65536
#define MAX_EDITS        1024
#define MAX_DEPTH        24

typedef struct { long off; char *text; } ud_slide;

struct ud_ppt {
    unsigned char *doc;  long dlen;      /* the PowerPoint Document stream  */
    long     *persist;   long npersist;  /* persist id -> stream offset     */
    ud_slide *slide;     int nslide;
};

/* ---- bounded record reads --------------------------------------------------- */
static int rec_at(const ud_ppt *p, long off, int *type, long *len, long *body)
{
    if (off < 0 || off + HDR > p->dlen) return 0;
    *type = (int)ud_rd16(p->doc + off + 2);
    *len  = (long)ud_rd32(p->doc + off + 4);
    *body = off + HDR;
    if (*len < 0 || *body + *len > p->dlen) return 0;
    return 1;
}

/* Is this record a container?  The low nibble of the first u16 is 0xF for a
 * container and 0 for an atom - which is how the tree is walked without a
 * table of every record type in the format. */
static int rec_is_container(const ud_ppt *p, long off)
{ return (ud_rd16(p->doc + off) & 0x000F) == 0x000F; }

/* ---- the persist directory --------------------------------------------------
 * Folded newest-first: an id already present is NOT overwritten, because the
 * entry we saw first came from a later edit. */
static void persist_fold(ud_ppt *p, long diroff)
{
    int type;
    long len, at, end;

    if (!rec_at(p, diroff, &type, &len, &at)) return;
    if (type != R_PERSISTDIR) return;
    end = at + len;
    while (at + 4 <= end) {
        uint32_t v = ud_rd32(p->doc + at);
        long id = (long)(v & 0x000FFFFF);
        long cnt = (long)(v >> 20);
        long k;
        at += 4;
        for (k = 0; k < cnt && at + 4 <= end; k++, at += 4) {
            long slot = id + k;
            long off = (long)ud_rd32(p->doc + at);
            if (slot < 0 || slot >= p->npersist) continue;
            if (p->persist[slot] >= 0) continue;      /* newer entry wins   */
            if (off < 0 || off + HDR > p->dlen) continue;
            p->persist[slot] = off;
        }
        if (cnt <= 0) break;                          /* malformed: stop    */
    }
}

/* ---- text ------------------------------------------------------------------
 * Collected by walking the slide's record tree in order.  A text block is
 * either UTF-16 (TextCharsAtom) or 8-bit (TextBytesAtom) - the same
 * either-encoding shape as BIFF8's shared strings and .doc's pieces, met for
 * the third time. */
typedef struct { char *p; long n, cap; } tbuf;

static void tb_ch(tbuf *t, char c)
{
    if (t->n + 2 > t->cap) {
        long cap = t->cap ? t->cap * 2 : 256;
        char *np = (char *)ud_alloc((unsigned long)cap);
        if (!np) return;
        if (t->n) memcpy(np, t->p, (unsigned long)t->n);
        ud_free(t->p);
        t->p = np; t->cap = cap;
    }
    if (t->n + 2 <= t->cap) t->p[t->n++] = c;
}

static void collect_text(ud_ppt *p, long off, long end, tbuf *t, int depth)
{
    while (off + HDR <= end) {
        int type;
        long len, body;
        if (!rec_at(p, off, &type, &len, &body)) return;
        if (type == R_TEXTCHARS || type == R_TEXTBYTES) {
            long i, n = (type == R_TEXTCHARS) ? len / 2 : len;
            for (i = 0; i < n; i++) {
                uint16_t u = (type == R_TEXTCHARS)
                             ? ud_rd16(p->doc + body + i * 2)
                             : ud_cp1252_to_uc(p->doc[body + i]);
                /* 0x0D ends a paragraph, 0x0B a line: both read as newlines */
                if (u == 0x0D || u == 0x0B) tb_ch(t, '\n');
                else if (u >= 0x20 || u == '\t') tb_ch(t, (char)ud_uc_to_cp1252(u));
            }
            tb_ch(t, '\n');
        } else if (rec_is_container(p, off) && depth < MAX_DEPTH) {
            collect_text(p, body, body + len, t, depth + 1);
        }
        off = body + len;
    }
}

/* ---- open -------------------------------------------------------------------- */
static int add_slide(ud_ppt *p, long off)
{
    ud_slide *n;
    if (p->nslide >= 4096) return 0;
    n = (ud_slide *)ud_alloc((unsigned long)(p->nslide + 1) * sizeof(ud_slide));
    if (!n) return 0;
    if (p->nslide) memcpy(n, p->slide, (unsigned long)p->nslide * sizeof(ud_slide));
    ud_free(p->slide);
    p->slide = n;
    p->slide[p->nslide].off = off;
    p->slide[p->nslide].text = 0;
    p->nslide++;
    return 1;
}

/* The Document's SlideListWithText gives the slides in presentation order;
 * each SlidePersistAtom names one by persist id. */
static void slides_from_list(ud_ppt *p, long off, long end, int depth)
{
    while (off + HDR <= end) {
        int type;
        long len, body;
        if (!rec_at(p, off, &type, &len, &body)) return;
        if (type == R_SLIDEPERSIST && len >= 4) {
            long id = (long)ud_rd32(p->doc + body);
            if (id >= 0 && id < p->npersist && p->persist[id] >= 0) {
                int t2;
                long l2, b2;
                if (rec_at(p, p->persist[id], &t2, &l2, &b2) && t2 == R_SLIDE)
                    add_slide(p, p->persist[id]);
            }
        } else if (rec_is_container(p, off) && depth < MAX_DEPTH) {
            slides_from_list(p, body, body + len, depth + 1);
        }
        off = body + len;
    }
}

ud_ppt *ud_ppt_open(ud_cfb *c)
{
    ud_ppt *p;
    int sid, cid, type;
    long len, body, off, docoff = -1;
    unsigned char cu[64];
    long edits = 0;
    int i;

    ud_set_error("");
    if (!c) { ud_set_error("no container"); return 0; }
    sid = ud_cfb_find(c, "/PowerPoint Document");
    cid = ud_cfb_find(c, "/Current User");
    if (sid == UD_CFB_NONE) {
        ud_set_error("not a presentation (no PowerPoint Document stream)");
        return 0;
    }
    p = (ud_ppt *)ud_alloc(sizeof(ud_ppt));
    if (!p) { ud_set_error("out of memory"); return 0; }
    memset(p, 0, sizeof *p);

    p->dlen = ud_cfb_size(c, sid);
    if (p->dlen <= 0 || p->dlen > MAX_STREAM) {
        ud_set_error("presentation: implausible document stream");
        ud_ppt_close(p); return 0;
    }
    p->doc = (unsigned char *)ud_alloc((unsigned long)p->dlen);
    if (!p->doc || ud_cfb_read(c, sid, 0, p->doc, p->dlen) != p->dlen) {
        ud_set_error("presentation: could not read the document stream");
        ud_ppt_close(p); return 0;
    }
    p->persist = (long *)ud_alloc((unsigned long)MAX_PERSIST * sizeof(long));
    if (!p->persist) { ud_set_error("out of memory"); ud_ppt_close(p); return 0; }
    p->npersist = MAX_PERSIST;
    for (i = 0; i < MAX_PERSIST; i++) p->persist[i] = -1;

    /* hop 1: where does the CURRENT edit start? */
    off = -1;
    if (cid != UD_CFB_NONE && ud_cfb_read(c, cid, 0, cu, (long)sizeof cu) >= 20) {
        if (ud_rd32(cu + HDR + 4) == CU_MAGIC) off = (long)ud_rd32(cu + HDR + 8);
    }
    if (off < 0) {
        ud_set_error("presentation: no usable Current User record");
        ud_ppt_close(p); return 0;
    }

    /* hops 2 and 3: walk the edit chain newest to oldest, folding each
       persist directory in as we go */
    while (off > 0 && edits++ < MAX_EDITS) {
        long prev;
        if (!rec_at(p, off, &type, &len, &body) || type != R_USEREDIT) break;
        if (len < 24) break;
        if (docoff < 0) {
            long docid = (long)ud_rd32(p->doc + body + 16);
            /* remembered now, resolved after the whole chain is folded */
            docoff = -2 - docid;
        }
        persist_fold(p, (long)ud_rd32(p->doc + body + 12));
        prev = (long)ud_rd32(p->doc + body + 8);
        if (prev == off) break;                       /* self-referencing   */
        off = prev;
    }

    /* hop 4: the live DocumentContainer */
    if (docoff <= -2) {
        long docid = -(docoff + 2);
        docoff = (docid >= 0 && docid < p->npersist) ? p->persist[docid] : -1;
    }
    if (docoff < 0 || !rec_at(p, docoff, &type, &len, &body) || type != R_DOCUMENT) {
        ud_set_error("presentation: could not find the live document");
        ud_ppt_close(p); return 0;
    }
    slides_from_list(p, body, body + len, 0);

    /* A presentation with no SlideListWithText is legal; fall back to every
       SlideContainer the persist directory names, in stream order. */
    if (!p->nslide) {
        long k;
        for (k = 0; k < p->npersist; k++) {
            int t2;
            long l2, b2;
            if (p->persist[k] < 0) continue;
            if (rec_at(p, p->persist[k], &t2, &l2, &b2) && t2 == R_SLIDE)
                add_slide(p, p->persist[k]);
        }
    }
    return p;
}

/* ===========================================================================
 * The builder seam (ud_ppt_int.h): how ud_pptx.c fills this same deck.
 * ======================================================================== */
ud_ppt *ud_ppt_blank(void)
{
    ud_ppt *p = (ud_ppt *)ud_alloc(sizeof(ud_ppt));
    if (!p) { ud_set_error("out of memory (presentation)"); return 0; }
    memset(p, 0, sizeof *p);
    return p;
}

int ud_ppt_b_slide(ud_ppt *p, char *text)
{
    if (!p) { ud_free(text); return 0; }
    if (!add_slide(p, 0)) { ud_free(text); return 0; }
    /* Always a real string, never NULL: a NULL would send
     * ud_ppt_slide_text() down the lazy binary path, into a Document stream
     * this deck does not have.  An empty slide is a slide with no text. */
    if (!text) {
        text = (char *)ud_alloc(1);
        if (text) text[0] = 0;
    }
    p->slide[p->nslide - 1].text = text;
    return 1;
}

void ud_ppt_close(ud_ppt *p)
{
    int i;
    if (!p) return;
    for (i = 0; i < p->nslide; i++) ud_free(p->slide[i].text);
    ud_free(p->slide);
    ud_free(p->persist);
    ud_free(p->doc);
    ud_free(p);
}

int ud_ppt_slides(const ud_ppt *p) { return p ? p->nslide : 0; }

/* The slide's drawing is Escher, and Escher is format-neutral, so the whole
 * SlideContainer is handed to it: the walker finds the SpContainers wherever
 * inside the PPDrawing they happen to sit. */
int ud_ppt_slide_shapes(ud_ppt *p, int i, ud_shape *out, int max)
{
    int type;
    long len, body;
    if (!p || i < 0 || i >= p->nslide || !out || max <= 0) return 0;
    if (!rec_at(p, p->slide[i].off, &type, &len, &body)) return 0;
    return ud_escher_shapes(p->doc, p->dlen, body, body + len, out, max, 0);
}

const char *ud_ppt_slide_text(ud_ppt *p, int i)
{
    tbuf t;
    int type;
    long len, body;

    if (!p || i < 0 || i >= p->nslide) return "";
    if (p->slide[i].text) return p->slide[i].text;
    memset(&t, 0, sizeof t);
    if (rec_at(p, p->slide[i].off, &type, &len, &body))
        collect_text(p, body, body + len, &t, 0);
    if (!t.p) { tb_ch(&t, 0); if (!t.p) return ""; t.n = 0; }
    t.p[t.n] = 0;
    p->slide[i].text = t.p;
    return p->slide[i].text;
}
