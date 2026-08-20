/* ===========================================================================
 * ud_docx.c - the .docx reader (WordprocessingML, [MS-DOCX] / ECMA-376).
 *
 * It builds the SAME `ud_doc` ud_doc.c builds from a Word 97 FIB, so
 * ud_doc_plain(), ud_doc_chp_at() and ud_doc_pap_at() - and UnoWord above
 * them - work on either without knowing which parser ran.
 *
 * THE ONE STRUCTURAL DIFFERENCE, and it is why struct ud_doc gained two
 * fields rather than this file faking the binary format's data structures:
 *
 *   .doc  stores formatting as EXCEPTIONS in 512-byte FKP pages, found by a
 *         bin table keyed on the byte offset of the text.
 *   .docx stores it as the tree it is - `<w:r><w:rPr><w:b/></w:rPr>` - so the
 *         formatting is known while the text is being appended and there is
 *         nothing to look up.
 *
 * Writing FKP pages here to feed the existing lookup would mean encoding
 * Word's sprm language purely so that a decoder in the next file could decode
 * it back, with the bugs of both halves and the benefit of neither.  Instead
 * the document carries an optional array of formatting RUNS, and the two
 * accessors consult it when it is there.  One model, two ways of getting the
 * formatting into it.
 *
 * THE TEXT IS BUILT IN WORD'S OWN CONVENTION: a paragraph ends with '\r' and
 * a tab is '\t', exactly as the binary reader produces, because ud_doc_plain()
 * turns those into newlines and every consumer is written against that.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_doc_int.h"
#include <string.h>

#define TXTCHUNK   (64L * 1024)
#define MAXTEXT    (16L * 1024 * 1024)
#define RUNBUF     8192

/* A growable CP-1252 text buffer, which is what ud_doc holds. */
typedef struct { char *p; long n, cap; } tbuf;

static int tb_put(tbuf *t, const char *s, long n)
{
    if (n < 0) n = (long)strlen(s);
    if (t->n + n + 1 > t->cap) {
        long nc = t->cap ? t->cap : TXTCHUNK;
        char *np;
        while (nc < t->n + n + 1) nc *= 2;
        if (nc > MAXTEXT) return 0;
        np = (char *)ud_alloc((unsigned long)nc);
        if (!np) return 0;
        if (t->n) memcpy(np, t->p, (unsigned long)t->n);
        ud_free(t->p);
        t->p = np; t->cap = nc;
    }
    memcpy(t->p + t->n, s, (unsigned long)n);
    t->n += n;
    t->p[t->n] = 0;
    return 1;
}

/* ---- one run's character formatting -------------------------------------------
 * `<w:rPr>` holds toggles (`<w:b/>` is bold, `<w:b w:val="0"/>` is not) and
 * measured properties.  The toggle rule is the one that catches people: an
 * ABSENT val attribute means ON. */
static void read_rpr(ud_xml *p, ud_chp *chp)
{
    int depth = p->depth;
    if (p->empty) return;
    while (ud_xml_next(p)) {
        if (p->kind == UD_XML_END && p->depth < depth) return;
        if (p->kind != UD_XML_START) continue;
        if      (ud_xml_is(p, "b"))      chp->bold      = ud_xml_attr_bool(p, "val", 1);
        else if (ud_xml_is(p, "i"))      chp->italic    = ud_xml_attr_bool(p, "val", 1);
        else if (ud_xml_is(p, "u"))      chp->underline = 1;
        else if (ud_xml_is(p, "strike")) chp->strike    = ud_xml_attr_bool(p, "val", 1);
        else if (ud_xml_is(p, "caps"))   chp->caps      = ud_xml_attr_bool(p, "val", 1);
        else if (ud_xml_is(p, "smallCaps")) chp->smallcaps = ud_xml_attr_bool(p, "val", 1);
        else if (ud_xml_is(p, "sz"))     chp->size = (int)ud_xml_attr_int(p, "val", 0);
        else if (ud_xml_is(p, "vertAlign")) {
            char v[16];
            if (ud_xml_attr_str(p, "val", v, (int)sizeof v)) {
                chp->super = !strcmp(v, "superscript");
                chp->sub   = !strcmp(v, "subscript");
            }
        }
        if (!p->empty) ud_xml_skip(p);
    }
}

/* `<w:pPr>`: the paragraph's own properties.  Indents and spacing are already
 * in twips in the file, which is the unit ud_pap reports, so they pass
 * straight through - the one place OOXML is easier than the binary format. */
static void read_ppr(ud_xml *p, ud_pap *pap)
{
    int depth = p->depth;
    if (p->empty) return;
    while (ud_xml_next(p)) {
        if (p->kind == UD_XML_END && p->depth < depth) return;
        if (p->kind != UD_XML_START) continue;
        if (ud_xml_is(p, "jc")) {
            char v[16];
            if (ud_xml_attr_str(p, "val", v, (int)sizeof v)) {
                if      (!strcmp(v, "center"))  pap->align = 1;
                else if (!strcmp(v, "right"))   pap->align = 2;
                else if (!strcmp(v, "both") ||
                         !strcmp(v, "distribute")) pap->align = 3;
                else                            pap->align = 0;
            }
        } else if (ud_xml_is(p, "ind")) {
            pap->left  = (int)ud_xml_attr_int(p, "left",     pap->left);
            pap->left  = (int)ud_xml_attr_int(p, "start",    pap->left);
            pap->right = (int)ud_xml_attr_int(p, "right",    pap->right);
            pap->right = (int)ud_xml_attr_int(p, "end",      pap->right);
            pap->first = (int)ud_xml_attr_int(p, "firstLine", pap->first);
            {   /* a hanging indent is a NEGATIVE first-line indent */
                long h = ud_xml_attr_int(p, "hanging", 0);
                if (h) pap->first = (int)-h;
            }
        } else if (ud_xml_is(p, "spacing")) {
            pap->before = (int)ud_xml_attr_int(p, "before", pap->before);
            pap->after  = (int)ud_xml_attr_int(p, "after",  pap->after);
        } else if (ud_xml_is(p, "keepNext")) {
            pap->keep_next = ud_xml_attr_bool(p, "val", 1);
        } else if (ud_xml_is(p, "pageBreakBefore")) {
            pap->page_break_before = ud_xml_attr_bool(p, "val", 1);
        }
        if (!p->empty) ud_xml_skip(p);
    }
}

/* ---- the body ------------------------------------------------------------------ */
static int read_body(ud_doc *d, const char *xml, long len)
{
    ud_xml p;
    tbuf t;
    char *run;
    int ok = 1;

    memset(&t, 0, sizeof t);
    run = (char *)ud_alloc(RUNBUF);
    if (!run) return 0;

    ud_xml_init(&p, xml, len);
    while (ud_xml_next(&p)) {
        if (p.kind != UD_XML_START) continue;

        if (ud_xml_is(&p, "p")) {
            ud_pap pap;
            int pdepth = p.depth, empty = p.empty;
            long pstart = t.n;

            memset(&pap, 0, sizeof pap);
            if (!empty) {
                while (ud_xml_next(&p)) {
                    if (p.kind == UD_XML_END && p.depth < pdepth) break;
                    if (p.kind != UD_XML_START) continue;

                    if (ud_xml_is(&p, "pPr")) { read_ppr(&p, &pap); continue; }

                    if (ud_xml_is(&p, "r")) {
                        ud_chp chp;
                        int rdepth = p.depth, rempty = p.empty;
                        long rstart = t.n;
                        memset(&chp, 0, sizeof chp);
                        if (rempty) continue;
                        while (ud_xml_next(&p)) {
                            if (p.kind == UD_XML_END && p.depth < rdepth) break;
                            if (p.kind != UD_XML_START) continue;
                            if (ud_xml_is(&p, "rPr")) { read_rpr(&p, &chp); continue; }
                            if (ud_xml_is(&p, "t")) {
                                long n = ud_xml_inner_text(&p, run, RUNBUF);
                                if (n && !tb_put(&t, run, n)) { ok = 0; goto done; }
                                continue;
                            }
                            if (ud_xml_is(&p, "tab")) {
                                if (!tb_put(&t, "\t", 1)) { ok = 0; goto done; }
                            } else if (ud_xml_is(&p, "br")) {
                                /* a line break inside a paragraph: Word's own
                                 * text uses \v for this and ud_doc_plain turns
                                 * it into a newline, same as the binary path */
                                if (!tb_put(&t, "\v", 1)) { ok = 0; goto done; }
                            } else if (ud_xml_is(&p, "noBreakHyphen")) {
                                if (!tb_put(&t, "-", 1)) { ok = 0; goto done; }
                            }
                            if (!p.empty) ud_xml_skip(&p);
                        }
                        if (t.n > rstart) ud_doc_b_chp(d, rstart, t.n, &chp);
                        continue;
                    }

                    /* Everything else in a paragraph - bookmarks, comment
                     * anchors, revision marks, drawings - is stepped over
                     * whole.  A `<w:hyperlink>` is the one that matters: its
                     * runs are INSIDE it, so it is descended into rather than
                     * skipped, or every link's text disappears. */
                    if (ud_xml_is(&p, "hyperlink") || ud_xml_is(&p, "smartTag") ||
                        ud_xml_is(&p, "ins") || ud_xml_is(&p, "sdtContent") ||
                        ud_xml_is(&p, "sdt"))
                        continue;
                    if (ud_xml_is(&p, "del")) { ud_xml_skip(&p); continue; }
                    if (!p.empty) ud_xml_skip(&p);
                }
            }
            /* Word's paragraph mark, which is what ud_doc_plain turns into a
             * newline.  It is part of the text, and every cp-to-formatting
             * lookup counts it. */
            if (!tb_put(&t, "\r", 1)) { ok = 0; goto done; }
            ud_doc_b_pap(d, pstart, t.n, &pap);
            continue;
        }

        /* A table cell's paragraphs are ordinary `<w:p>` elements one level
         * down, so tables are descended into and their cells simply become
         * paragraphs.  That loses the grid, which this build does not model,
         * and keeps the text, which is the thing a reader came for. */
        if (ud_xml_is(&p, "tbl") || ud_xml_is(&p, "tr") || ud_xml_is(&p, "tc") ||
            ud_xml_is(&p, "body") || ud_xml_is(&p, "document"))
            continue;
        if (!p.empty) ud_xml_skip(&p);
    }

done:
    ud_free(run);
    if (!ok) { ud_free(t.p); return 0; }
    ud_doc_b_text(d, t.p, t.n);          /* the document takes the buffer */
    return 1;
}

ud_doc *ud_docx_open(ud_zip *z)
{
    ud_doc *d;
    unsigned char *body;
    long len = 0;
    int idx;

    if (!z) { ud_set_error("docx: no container"); return 0; }
    idx = ud_zip_find(z, "word/document.xml");
    if (idx < 0) {
        ud_set_error("not a Word document (no word/document.xml)");
        return 0;
    }
    body = ud_zip_load(z, idx, &len);
    if (!body) return 0;

    d = ud_doc_blank();
    if (!d) { ud_free(body); return 0; }
    if (!read_body(d, (const char *)body, len)) {
        ud_free(body);
        ud_doc_close(d);
        ud_set_error("docx: out of memory reading the body");
        return 0;
    }
    ud_free(body);
    return d;
}
