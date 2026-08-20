/* ===========================================================================
 * ud_pptx.c - the .pptx reader (PresentationML, [MS-PPTX] / ECMA-376).
 *
 * It builds the SAME `ud_ppt` ud_ppt.c builds from the PowerPoint Document
 * stream, so ud_ppt_slides() and ud_ppt_slide_text() - and UnoShow above them
 * - work on either.
 *
 * SLIDE ORDER COMES FROM THE PRESENTATION, NOT FROM THE FILE NAMES.
 * `ppt/presentation.xml` lists `<p:sldId r:id="rId2"/>` in show order, and the
 * relationships part says which file each rId is.  Sorting `slide1.xml,
 * slide2.xml, ...` gets the right answer only on a deck nobody has reordered:
 * PowerPoint keeps a slide's part name when it is moved, so a deck whose third
 * slide lives in slide7.xml is entirely ordinary, and a reader that sorts by
 * name silently shuffles the presentation.  That is the same lesson the .xlsx
 * reader states about sheets, and it is the same indirection.
 *
 * THE TEXT MODEL is `<a:p>` paragraphs of `<a:r><a:t>` runs inside each shape's
 * `<p:txBody>` - the DrawingML text layout the whole format shares.  Notes and
 * masters live in their own parts and are not read: what a reader wants from a
 * deck is the slides.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_ppt_int.h"
#include <string.h>

#define MAXSLIDE  1024
#define TXTBUF    (256L * 1024)
#define RUNBUF    8192

typedef struct { char id[24]; char target[112]; } prel;

static void scopy(char *d, const char *s, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int load_rels(ud_zip *z, prel *out, int max)
{
    int idx = ud_zip_find(z, "ppt/_rels/presentation.xml.rels"), n = 0;
    unsigned char *buf;
    long len = 0;
    ud_xml x;
    if (idx < 0) return 0;
    buf = ud_zip_load(z, idx, &len);
    if (!buf) return 0;
    ud_xml_init(&x, (const char *)buf, len);
    while (ud_xml_next(&x) && n < max) {
        char t[112];
        if (x.kind != UD_XML_START || !ud_xml_is(&x, "Relationship")) continue;
        if (!ud_xml_attr_str(&x, "Id", out[n].id, (int)sizeof out[0].id)) continue;
        if (!ud_xml_attr_str(&x, "Target", t, (int)sizeof t)) continue;
        if (t[0] == '/') {
            scopy(out[n].target, t + 1, (int)sizeof out[0].target);
        } else {
            const char *rel = t;
            int at, k = 0;
            if (rel[0] == '.' && rel[1] == '/') rel += 2;
            scopy(out[n].target, "ppt/", (int)sizeof out[0].target);
            at = (int)strlen(out[n].target);
            while (rel[k] && at < (int)sizeof out[0].target - 1)
                out[n].target[at++] = rel[k++];
            out[n].target[at] = 0;
        }
        n++;
    }
    ud_free(buf);
    return n;
}

/* ---- one slide's text ----------------------------------------------------------
 * Paragraphs are separated by a newline, which is what the binary reader
 * produces and what UnoShow lays out. */
static char *slide_text(ud_zip *z, const char *part)
{
    int idx = ud_zip_find(z, part);
    unsigned char *buf;
    long len = 0, o = 0;
    char *txt, *run, *out;
    ud_xml p;

    if (idx < 0) return 0;
    buf = ud_zip_load(z, idx, &len);
    if (!buf) return 0;
    txt = (char *)ud_alloc((unsigned long)TXTBUF);
    run = (char *)ud_alloc(RUNBUF);
    if (!txt || !run) { ud_free(txt); ud_free(run); ud_free(buf); return 0; }
    txt[0] = 0;

    ud_xml_init(&p, (const char *)buf, len);
    while (ud_xml_next(&p)) {
        if (p.kind != UD_XML_START) continue;
        if (ud_xml_is(&p, "p")) {
            int depth = p.depth;
            long start = o;
            if (p.empty) continue;
            while (ud_xml_next(&p)) {
                if (p.kind == UD_XML_END && p.depth < depth) break;
                if (p.kind != UD_XML_START) continue;
                if (ud_xml_is(&p, "t")) {
                    long n = ud_xml_inner_text(&p, run, RUNBUF);
                    if (n > 0 && o + n < TXTBUF - 2) {
                        memcpy(txt + o, run, (unsigned long)n);
                        o += n;
                    }
                } else if (ud_xml_is(&p, "br")) {
                    if (o < TXTBUF - 2) txt[o++] = '\n';
                }
            }
            /* an empty paragraph is a blank line, and a deck uses them for
             * spacing - dropping them runs two bullets together */
            if (o > start || o == start) {
                if (o < TXTBUF - 2) txt[o++] = '\n';
            }
            continue;
        }
        /* `<p:txBody>`, `<p:sp>` and the shape tree are descended into;
         * everything else - transitions, timing, the drawing geometry - is
         * stepped over whole. */
        if (ud_xml_is(&p, "txBody") || ud_xml_is(&p, "sp") ||
            ud_xml_is(&p, "spTree") || ud_xml_is(&p, "cSld") ||
            ud_xml_is(&p, "sld") || ud_xml_is(&p, "grpSp") ||
            ud_xml_is(&p, "graphicFrame") || ud_xml_is(&p, "graphic") ||
            ud_xml_is(&p, "graphicData") || ud_xml_is(&p, "tbl") ||
            ud_xml_is(&p, "tr") || ud_xml_is(&p, "tc"))
            continue;
        if (!p.empty) ud_xml_skip(&p);
    }
    txt[o] = 0;
    /* hand back a right-sized copy: a 256 KB scratch buffer per slide would
     * be most of the module arena on a fifty-slide deck */
    out = (char *)ud_alloc((unsigned long)o + 1);
    if (out) memcpy(out, txt, (unsigned long)o + 1);
    ud_free(txt);
    ud_free(run);
    ud_free(buf);
    return out;
}

ud_ppt *ud_pptx_open(ud_zip *z)
{
    ud_ppt *p;
    prel *rels;
    unsigned char *pres;
    long len = 0;
    int idx, nrel = 0;
    ud_xml x;

    if (!z) { ud_set_error("pptx: no container"); return 0; }
    idx = ud_zip_find(z, "ppt/presentation.xml");
    if (idx < 0) {
        ud_set_error("not a presentation (no ppt/presentation.xml)");
        return 0;
    }
    rels = (prel *)ud_alloc((unsigned long)MAXSLIDE * sizeof(prel));
    if (!rels) { ud_set_error("out of memory (pptx)"); return 0; }
    memset(rels, 0, (unsigned long)MAXSLIDE * sizeof(prel));
    nrel = load_rels(z, rels, MAXSLIDE);

    p = ud_ppt_blank();
    if (!p) { ud_free(rels); return 0; }

    pres = ud_zip_load(z, idx, &len);
    if (!pres) { ud_free(rels); ud_ppt_close(p); return 0; }
    ud_xml_init(&x, (const char *)pres, len);
    while (ud_xml_next(&x)) {
        char rid[24];
        int i;
        if (x.kind != UD_XML_START || !ud_xml_is(&x, "sldId")) continue;
        /* r:id, NOT id - the element carries both and the unprefixed one is
         * the slide's own number.  See ud_xml_attr_ns. */
        if (!ud_xml_attr_ns_str(&x, "r", "id", rid, (int)sizeof rid)) continue;
        for (i = 0; i < nrel; i++) {
            if (strcmp(rels[i].id, rid)) continue;
            /* A slide whose part is missing still counts: the deck says it is
             * there, and an empty slide is a better report than a short deck
             * that silently renumbers everything after it. */
            ud_ppt_b_slide(p, slide_text(z, rels[i].target));
            break;
        }
        if (i == nrel) ud_ppt_b_slide(p, 0);
    }
    ud_free(pres);
    ud_free(rels);
    return p;
}
