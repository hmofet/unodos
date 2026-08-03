/* ===========================================================================
 * uow_layout.c - UnoWord's page layout (OFFICE97-PLAN §5 phase 7).
 *
 * Paginated flow: the window is a viewport onto pages laid out on a
 * pasteboard, and the WRAP TARGET IS THE PAGE, not the window.  That is the
 * one thing pc64's Editor cannot be taught, and the reason this file exists.
 *
 * Everything it knows about how wide a character is arrives through
 * uow_metrics, so it lays out identically on the host harness (8x8 bitmap)
 * and on pc64 (kerned TTF at the user's UI scale) and can be gated without
 * booting the OS.
 *
 * Coordinates in the output are DOCUMENT pixels: scrolling never relayouts,
 * which is the same rule unoweb states for the same reason.
 * ======================================================================== */
#include "uoword.h"

/* Word measures in twips - 1440 to the inch.  On screen a point is about a
 * pixel at 100%, so the divisor is 15 twips per pixel scaled by the zoom. */
static int tw2px(int twips, int zoom)
{
    long v = (long)twips * zoom;
    return (int)(v / (15 * 100));
}

static long l_min(long a, long b) { return a < b ? a : b; }

/* ---- one line's worth of runs ---------------------------------------------
 * A run is emitted at every formatting change AND at every word boundary.
 *
 * THE WORD SPLIT IS WHAT MAKES JUSTIFICATION POSSIBLE, and getting that wrong
 * is the bug the gate caught: the first version emitted one run per
 * FORMATTING change, so a paragraph in a single font was one run per line,
 * there were no gaps to spread the slack across, and justified text came out
 * ragged-right while every unit test passed.  A run therefore ends after the
 * last space of a space group, so runs still tile the line exactly (the caret
 * and hit-testing depend on that) while each boundary is a real word gap that
 * justification can widen. */
static int emit_runs(uow_layout *L, const uow_doc *d, const uow_metrics *m,
                     long cp, long n, int *nrun_out)
{
    char buf[256];
    long at = cp, end = cp + n;
    int x = 0, nrun = 0;
    while (at < end && L->nrun < UOW_MAXLRUN) {
        uow_chp c, c2;
        long run_end = at + 1, take;
        uow_chp_at(d, at, &c);
        while (run_end < end) {
            int prev, here;
            uow_chp_at(d, run_end, &c2);
            {   const char *a = (const char *)&c, *b = (const char *)&c2;
                int k, same = 1;
                for (k = 0; k < (int)sizeof c; k++)
                    if (a[k] != b[k]) { same = 0; break; }
                if (!same) break;
            }
            prev = uow_char_at(d, run_end - 1);
            here = uow_char_at(d, run_end);
            if ((prev == ' ' || prev == '\t') && here != ' ' && here != '\t')
                break;                       /* the word gap ends the run    */
            run_end++;
        }
        take = l_min(run_end - at, (long)sizeof buf - 1);
        uow_read(d, at, take, buf);
        buf[take] = 0;
        {
            uow_lrun *r = &L->run[L->nrun++];
            r->cp = at; r->n = take; r->x = x; r->chp = c;
            r->w = m->text_w(buf, take, &c, m->ctx);
            x += r->w;
        }
        nrun++;
        at += take;
    }
    if (nrun_out) *nrun_out = nrun;
    return x;
}

/* Width of [cp, cp+n) without emitting anything - the measurement the line
 * breaker does over and over while it hunts for the break. */
static int span_w(const uow_doc *d, const uow_metrics *m, long cp, long n)
{
    char buf[256];
    long at = cp, end = cp + n;
    int x = 0;
    while (at < end) {
        uow_chp c;
        long run_end = at + 1, take;
        uow_chp_at(d, at, &c);
        while (run_end < end) {
            uow_chp c2;
            uow_chp_at(d, run_end, &c2);
            {   const char *a = (const char *)&c, *b = (const char *)&c2;
                int k, same = 1;
                for (k = 0; k < (int)sizeof c; k++)
                    if (a[k] != b[k]) { same = 0; break; }
                if (!same) break;
            }
            run_end++;
        }
        take = l_min(run_end - at, (long)sizeof buf - 1);
        uow_read(d, at, take, buf);
        buf[take] = 0;
        x += m->text_w(buf, take, &c, m->ctx);
        at += take;
    }
    return x;
}

/* The tallest character in a span decides the line's height and baseline. */
static void span_metrics(const uow_doc *d, const uow_metrics *m,
                         long cp, long n, int *h, int *base)
{
    long i;
    int mh = 0, mb = 0;
    for (i = cp; i < cp + n; i++) {
        uow_chp c;
        int ch, cb;
        uow_chp_at(d, i, &c);
        ch = m->height(&c, m->ctx);
        cb = m->baseline(&c, m->ctx);
        if (ch > mh) mh = ch;
        if (cb > mb) mb = cb;
    }
    if (!mh) {                       /* an empty paragraph still has a height */
        uow_chp c;
        uow_chp_at(d, cp, &c);
        mh = m->height(&c, m->ctx);
        mb = m->baseline(&c, m->ctx);
    }
    *h = mh; *base = mb;
}

int uow_layout_run(uow_layout *L, const uow_doc *d, const uow_metrics *m,
                   int zoom)
{
    const uow_sect *sc;
    int page_w, page_h, ml, mr, mt, mb, text_w, text_h;
    int px, py, gap = 12, ok = 1;
    long cp;

    if (!L || !d || !m) return 0;
    {   int i; char *p = (char *)L;
        for (i = 0; i < (int)sizeof *L; i++) p[i] = 0; }
    if (zoom <= 0) zoom = 100;
    L->zoom = zoom;
    L->rev = uow_revision(d);
    L->twips_per_px = 15;

    sc = uow_section((uow_doc *)d);
    page_w = tw2px(sc->page_w, zoom);
    page_h = tw2px(sc->page_h, zoom);
    ml = tw2px(sc->margin_l, zoom);
    mr = tw2px(sc->margin_r, zoom);
    mt = tw2px(sc->margin_t, zoom);
    mb = tw2px(sc->margin_b, zoom);
    text_w = page_w - ml - mr;
    text_h = page_h - mt - mb;
    if (text_w < 32) text_w = 32;
    if (text_h < 32) text_h = 32;

    px = gap;
    py = gap;
    /* the first page */
    L->page[0].x = px; L->page[0].y = py;
    L->page[0].w = page_w; L->page[0].h = page_h;
    L->page[0].text_x = px + ml; L->page[0].text_y = py + mt;
    L->page[0].text_w = text_w; L->page[0].text_h = text_h;
    L->page[0].line0 = 0; L->page[0].nline = 0;
    L->npage = 1;

    cp = 0;
    {
        int y = 0;                    /* the pen, relative to the text area  */
        while (cp <= uow_len(d) - 1) {
            uow_pap pp;
            long pstart = cp, pend = uow_para_end(d, cp);
            long at = pstart;
            int ind_l, ind_r, ind_first, before, after;
            int first_line = 1, para_line0 = L->nline;

            uow_pap_at(d, pstart, &pp);
            ind_l     = tw2px(pp.left, zoom);
            ind_r     = tw2px(pp.right, zoom);
            ind_first = tw2px(pp.first, zoom);
            before    = tw2px(pp.before, zoom);
            after     = tw2px(pp.after, zoom);

            if (pp.page_before && L->nline > 0) y = text_h;   /* force a break */
            y += before;

            do {
                long take, best = 0, i;
                int avail = text_w - ind_l - ind_r - (first_line ? ind_first : 0);
                int lw, lh, lb, nrun;
                long remain = pend - at;

                if (avail < 16) avail = 16;

                /* GREEDY WORD WRAP: the last break opportunity that still
                 * fits.  A single word wider than the column is allowed to
                 * overhang rather than being chopped - Word does the same,
                 * and chopping loses text a user typed. */
                if (remain <= 0) { take = 0; }
                else {
                    take = remain;
                    if (span_w(d, m, at, remain) > avail) {
                        for (i = 1; i <= remain; i++) {
                            int c = uow_char_at(d, at + i - 1);
                            if (c == ' ' || c == '\t' || c == '-') {
                                if (span_w(d, m, at, i) <= avail) best = i;
                                else break;
                            }
                        }
                        if (best > 0) take = best;
                        else {
                            take = 1;
                            while (take < remain &&
                                   span_w(d, m, at, take + 1) <= avail) take++;
                        }
                    }
                }

                span_metrics(d, m, at, take ? take : 1, &lh, &lb);
                if (pp.linerule == UOW_LS_ONEHALF) lh = lh * 3 / 2;
                else if (pp.linerule == UOW_LS_DOUBLE) lh *= 2;
                else if (pp.linerule == UOW_LS_EXACT && pp.lineval)
                    lh = tw2px(pp.lineval, zoom);
                else if (pp.linerule == UOW_LS_ATLEAST && pp.lineval) {
                    int e = tw2px(pp.lineval, zoom);
                    if (e > lh) lh = e;
                }

                /* a page break: start a new sheet and put the line on it */
                if (y + lh > text_h && L->nline > 0) {
                    if (L->npage >= UOW_MAXPAGE) { ok = 0; break; }
                    {
                        uow_page *prev = &L->page[L->npage - 1], *pg;
                        pg = &L->page[L->npage];
                        pg->x = prev->x;
                        pg->y = prev->y + page_h + gap;
                        pg->w = page_w; pg->h = page_h;
                        pg->text_x = pg->x + ml; pg->text_y = pg->y + mt;
                        pg->text_w = text_w; pg->text_h = text_h;
                        pg->line0 = L->nline; pg->nline = 0;
                        L->npage++;
                    }
                    y = 0;
                    /* WIDOW CONTROL: if this break would strand the FIRST
                     * line of a paragraph alone on the old page, pull it
                     * forward too.  Word's default, and the reason a heading
                     * never sits by itself at a page foot. */
                    if (pp.widow && !first_line && L->nline - para_line0 == 1) {
                        uow_line *ln = &L->line[L->nline - 1];
                        L->page[L->npage - 1].line0 = L->nline - 1;
                        L->page[L->npage - 1].nline = 1;
                        L->page[L->npage - 2].nline--;
                        ln->page = L->npage - 1;
                        ln->y = L->page[L->npage - 1].text_y;
                        y = ln->h;
                    }
                }

                if (L->nline >= UOW_MAXLINE) { ok = 0; break; }
                {
                    uow_page *pg = &L->page[L->npage - 1];
                    uow_line *ln = &L->line[L->nline];
                    int lx = pg->text_x + ind_l + (first_line ? ind_first : 0);
                    ln->cp = at; ln->n = take;
                    ln->run0 = L->nrun;
                    lw = emit_runs(L, d, m, at, take, &nrun);
                    ln->nrun = nrun;
                    ln->x = lx;
                    ln->y = pg->text_y + y;
                    ln->w = lw;
                    ln->h = lh;
                    ln->baseline = lb;
                    ln->page = L->npage - 1;
                    ln->first_of_para = (unsigned char)first_line;
                    ln->last_of_para = (unsigned char)(at + take >= pend);

                    /* alignment shifts the line; JUSTIFY instead spreads the
                     * slack across the gaps between its runs, which is what
                     * makes the assertion in the gate meaningful: the widths
                     * plus the gaps must equal the column exactly. */
                    if (pp.align == UOW_AL_CENTER)
                        ln->x += (avail - lw) / 2;
                    else if (pp.align == UOW_AL_RIGHT)
                        ln->x += avail - lw;
                    else if (pp.align == UOW_AL_JUSTIFY && !ln->last_of_para &&
                             nrun > 1 && lw < avail) {
                        int slack = avail - lw, k, gaps = nrun - 1;
                        int per = slack / gaps, extra = slack % gaps;
                        int shift = 0;
                        for (k = 0; k < nrun; k++) {
                            L->run[ln->run0 + k].x += shift;
                            if (k < gaps) {
                                shift += per + (k < extra ? 1 : 0);
                            }
                        }
                        ln->w = avail;
                    }
                    pg->nline++;
                    L->nline++;
                }
                y += lh;
                at += take;
                first_line = 0;
                if (take == 0) break;
            } while (at < pend);

            if (!ok) break;
            y += after;
            cp = pend + 1;            /* step over the paragraph mark        */
            if (pend >= uow_len(d)) break;
        }
    }

    /* the pasteboard's extent, for the scrollbars */
    L->doc_w = page_w + gap * 2;
    L->doc_h = L->npage ? (L->page[L->npage - 1].y + page_h + gap) : gap;
    return ok;
}

int uow_line_of(const uow_layout *L, long cp)
{
    int i;
    if (!L) return -1;
    for (i = 0; i < L->nline; i++)
        if (cp >= L->line[i].cp && cp <= L->line[i].cp + L->line[i].n) {
            /* a cp on a boundary belongs to the line that STARTS there,
             * unless that is the end of the document */
            if (cp == L->line[i].cp + L->line[i].n && i + 1 < L->nline &&
                L->line[i + 1].cp == cp) return i + 1;
            return i;
        }
    return L->nline ? L->nline - 1 : -1;
}

int uow_caret_x(const uow_layout *L, const uow_metrics *m, long cp)
{
    int i = uow_line_of(L, cp), k;
    const uow_line *ln;
    if (i < 0) return 0;
    ln = &L->line[i];
    for (k = 0; k < ln->nrun; k++) {
        const uow_lrun *r = &L->run[ln->run0 + k];
        if (cp >= r->cp && cp <= r->cp + r->n) {
            if (cp == r->cp) return ln->x + r->x;
            /* the width of the part of the run before the caret */
            return ln->x + r->x + (int)((long)r->w * (cp - r->cp) / (r->n ? r->n : 1));
        }
    }
    (void)m;
    return ln->x + ln->w;
}

long uow_cp_at(const uow_layout *L, const uow_metrics *m, int x, int y)
{
    int i, best = -1, k;
    if (!L || L->nline <= 0) return 0;
    for (i = 0; i < L->nline; i++) {
        const uow_line *ln = &L->line[i];
        if (y >= ln->y && y < ln->y + ln->h) { best = i; break; }
        if (y < ln->y) { best = i; break; }
    }
    if (best < 0) best = L->nline - 1;
    {
        const uow_line *ln = &L->line[best];
        if (x <= ln->x) return ln->cp;
        for (k = 0; k < ln->nrun; k++) {
            const uow_lrun *r = &L->run[ln->run0 + k];
            int rx = ln->x + r->x;
            if (x >= rx && x < rx + r->w && r->w > 0)
                return r->cp + (long)(x - rx) * r->n / r->w;
        }
        (void)m;
        return ln->cp + ln->n;
    }
}
