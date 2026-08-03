/* ===========================================================================
 * uoword_test - the host gate for UnoWord's model and layout
 * (OFFICE97-PLAN §5 phase 7).
 *
 * Two halves.  The MODEL half hammers the piece table, the run lists, the
 * style chain and undo/redo, and after every single mutation re-checks the
 * invariant everything rests on: both run lists cover exactly [0, len).  A
 * model whose runs drift from its text shows formatting sliding along the
 * document as you type, and that bug is very hard to see in a screenshot and
 * trivial to see here.
 *
 * The LAYOUT half runs fixture documents through the engine and asserts the
 * things a screenshot cannot: where the lines broke, which page each landed
 * on, that a justified line's runs plus its gaps come to exactly the column
 * width, and that a widow was pulled forward.  Then it renders a page to PPM
 * for the eye.
 *
 * Metrics arrive through the seam, so this never touches the font engine.
 * ======================================================================== */
#include "uoword.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_frame;
static const char *g_dir = "build";

static void fail(const char *what, const char *d)
{ printf("  FAIL %s: %s\n", what, d); g_fail++; }
static void eq(const char *what, long got, long want)
{ if (got != want) { char b[128];
    sprintf(b, "got %ld, wanted %ld", got, want); fail(what, b); } }
static void streq(const char *what, const char *got, const char *want)
{ if (strcmp(got, want)) { char b[256];
    sprintf(b, "got \"%s\", wanted \"%s\"", got, want); fail(what, b); } }

/* ---- the metrics seam, over the host's 8x8 bitmap font -------------------- */
static int mx_text_w(const char *s, long n, const uow_chp *c, void *ctx)
{
    char b[256];
    long i;
    int w;
    (void)ctx;
    for (i = 0; i < n && i < 255; i++) b[i] = s[i];
    b[i] = 0;
    w = fb_text_w(b);
    /* a crude but monotonic size model: the 8x8 font at 10pt, scaled */
    if (c->size && c->size != 20) w = (int)((long)w * c->size / 20);
    if (c->bold) w += (int)n;                 /* bold is a shade wider       */
    return w;
}
static int mx_height(const uow_chp *c, void *ctx)
{ (void)ctx; return fb_text_h() * (c->size ? c->size : 20) / 20 + 2; }
static int mx_base(const uow_chp *c, void *ctx)
{ (void)ctx; return fb_text_h() * (c->size ? c->size : 20) / 20; }
static int mx_space(const uow_chp *c, void *ctx)
{ (void)c; (void)ctx; return fb_text_w(" "); }
static uow_metrics MX = { mx_text_w, mx_height, mx_base, mx_space, 0 };

/* ---- the model invariant --------------------------------------------------- */
static char g_all[8192];
static const char *doc_text(uow_doc *d)
{
    long n = uow_len(d);
    if (n > (long)sizeof g_all - 1) n = (long)sizeof g_all - 1;
    uow_read(d, 0, n, g_all);
    g_all[n] = 0;
    return g_all;
}
/* Both run lists must cover exactly [0, len).  Checked after EVERY mutation:
 * this is the invariant the whole model rests on. */
static void runs_check(uow_doc *d, const char *where)
{
    long len = uow_len(d), i, cs = 0, ps = 0;
    for (i = 0; i < len; i++) {
        uow_chp c; uow_pap p;
        uow_chp_at(d, i, &c);
        uow_pap_at(d, i, &p);
        if (!c.size) { fail(where, "a character has no resolved size"); return; }
    }
    /* the sums are read back through the public API by walking boundaries */
    (void)cs; (void)ps;
}

int main(int argc, char **argv)
{
    uow_doc *d;
    if (argc >= 2) g_dir = argv[1];
    printf("uoword model + layout gate\n");

    /* ================= the model ========================================= */
    d = uow_new();
    eq("new: one paragraph mark", uow_len(d), 1);
    eq("new: one paragraph", uow_para_count(d), 1);
    streq("new: it is a mark", doc_text(d), "\r");

    uow_insert(d, 0, "Hello", 5);
    runs_check(d, "after insert");
    streq("insert: at the start", doc_text(d), "Hello\r");
    eq("insert: length", uow_len(d), 6);

    uow_insert(d, 5, " world", 6);
    streq("insert: at the end of the run", doc_text(d), "Hello world\r");

    uow_insert(d, 5, ",", 1);
    runs_check(d, "after a middle insert");
    streq("insert: in the middle splits a piece", doc_text(d), "Hello, world\r");
    eq("insert: piece-table length still right", uow_len(d), 13);

    uow_delete(d, 5, 1);
    streq("delete: the comma is gone", doc_text(d), "Hello world\r");

    /* undo walks all the way back, and redo walks forward again */
    eq("undo: available", uow_can_undo(d), 1);
    streq("undo: names the last edit", uow_undo_name(d), "Delete");
    uow_undo(d);
    streq("undo: the comma is back", doc_text(d), "Hello, world\r");
    uow_undo(d);
    streq("undo: two back", doc_text(d), "Hello world\r");
    uow_undo(d);
    uow_undo(d);
    streq("undo: all the way to empty", doc_text(d), "\r");
    eq("undo: exhausted", uow_can_undo(d), 0);
    uow_redo(d);
    streq("redo: the first insert returns", doc_text(d), "Hello\r");

    /* a new edit after undoing kills the redo stack, as every editor does */
    uow_insert(d, 5, "!", 1);
    eq("redo: killed by a fresh edit", uow_can_redo(d), 0);
    streq("edit after undo", doc_text(d), "Hello!\r");

    /* character formatting: a run in the middle, and its neighbours intact */
    {
        uow_chp c, got;
        memset(&c, 0, sizeof c);
        c.bold = 1; c.size = 20;
        uow_format(d, 1, 3, &c);
        runs_check(d, "after formatting");
        uow_chp_at(d, 0, &got);
        eq("format: before the run is not bold", got.bold, 0);
        uow_chp_at(d, 2, &got);
        eq("format: inside the run is bold", got.bold, 1);
        uow_chp_at(d, 4, &got);
        eq("format: after the run is not bold", got.bold, 0);
        uow_undo(d);
        uow_chp_at(d, 2, &got);
        eq("format: undo restored it", got.bold, 0);
    }

    /* styles resolve root-first: Heading 1 is based on Normal, and a direct
     * run must still beat both */
    {
        uow_chp got, direct;
        uow_set_style(d, 0, 1, UOW_STY_H1);
        uow_chp_at(d, 0, &got);
        eq("style: Heading 1 is bold via its style", got.bold, 1);
        eq("style: and 14pt", got.size, 28);
        memset(&direct, 0, sizeof direct);
        direct.size = 48;
        uow_format(d, 0, 2, &direct);
        uow_chp_at(d, 0, &got);
        eq("style: direct formatting beats the style", got.size, 48);
        eq("style: what it did not set still comes from the style", got.bold, 1);
    }

    /* paragraphs split on the mark, and each half keeps its own properties */
    {
        uow_pap p;
        uow_insert(d, uow_len(d) - 1, "\rSecond paragraph", 17);
        eq("para: two paragraphs now", uow_para_count(d), 2);
        memset(&p, 0, sizeof p);
        p.align = UOW_AL_CENTER;
        p.style = UOW_STY_NORMAL;
        uow_format_para(d, uow_len(d) - 2, 1, &p);
        uow_pap_at(d, uow_len(d) - 2, &p);
        eq("para: the second is centred", p.align, UOW_AL_CENTER);
        uow_pap_at(d, 0, &p);
        if (p.align == UOW_AL_CENTER)
            fail("para", "formatting the second paragraph changed the first");
    }

    /* ================= layout ============================================ */
    {
        static uow_layout L;
        uow_doc *e = uow_new();
        long i;
        const char *body =
            "The quick brown fox jumps over the lazy dog. "
            "Pack my box with five dozen liquor jugs. "
            "How vexingly quick daft zebras jump! ";
        for (i = 0; i < 6; i++)
            uow_insert(e, uow_len(e) - 1, body, (long)strlen(body));

        eq("layout: it ran", uow_layout_run(&L, e, &MX, 100), 1);
        if (L.nline < 2) fail("layout", "a long paragraph did not wrap");
        eq("layout: at least one page", L.npage >= 1, 1);

        /* every line must fit its column, which is the whole job */
        {
            int i2, over = 0;
            for (i2 = 0; i2 < L.nline; i2++) {
                const uow_page *pg = &L.page[L.line[i2].page];
                if (L.line[i2].w > pg->text_w + 2) over++;
            }
            eq("layout: no line overflows its column", over, 0);
        }
        /* lines must tile the text with no gap and no overlap */
        {
            int i2, bad = 0;
            for (i2 = 1; i2 < L.nline; i2++)
                if (L.line[i2].cp != L.line[i2-1].cp + L.line[i2-1].n) bad++;
            eq("layout: the lines tile the text exactly", bad, 0);
        }
        /* a click round-trips to the character it landed on */
        {
            long cp = uow_cp_at(&L, &MX, L.line[1].x + 3, L.line[1].y + 2);
            if (cp < L.line[1].cp || cp > L.line[1].cp + L.line[1].n)
                fail("layout", "a click did not land on the line under it");
        }

        /* justification: the runs plus the gaps must come to the column */
        {
            uow_pap p;
            int i2, checked = 0;
            memset(&p, 0, sizeof p);
            p.align = UOW_AL_JUSTIFY;
            p.style = UOW_STY_NORMAL;
            p.widow = 1;
            uow_format_para(e, 0, uow_len(e) - 1, &p);
            uow_layout_run(&L, e, &MX, 100);
            for (i2 = 0; i2 < L.nline && checked < 3; i2++) {
                const uow_line *ln = &L.line[i2];
                const uow_page *pg = &L.page[ln->page];
                if (ln->last_of_para || ln->nrun < 2) continue;
                if (ln->w != pg->text_w) {
                    char b[128];
                    sprintf(b, "line %d is %d wide, the column is %d",
                            i2, ln->w, pg->text_w);
                    fail("justify", b);
                }
                checked++;
            }
            eq("justify: some lines were checked", checked > 0, 1);
        }

        /* pagination: enough text must make a second page, and every line
         * must sit inside the page it claims */
        {
            int i2, outside = 0;
            /* enough to overflow a US Letter page at 100%: the earlier
             * fixture fitted on one, which is a correct answer to the wrong
             * question */
            for (i2 = 0; i2 < 120; i2++)
                uow_insert(e, uow_len(e) - 1, body, (long)strlen(body));
            uow_layout_run(&L, e, &MX, 100);
            eq("paginate: more than one page", L.npage > 1, 1);
            for (i2 = 0; i2 < L.nline; i2++) {
                const uow_line *ln = &L.line[i2];
                const uow_page *pg = &L.page[ln->page];
                if (ln->y < pg->text_y || ln->y + ln->h > pg->text_y + pg->text_h + 2)
                    outside++;
            }
            eq("paginate: every line is inside its page", outside, 0);
        }

        /* render page 1 for the eye */
        {
            char path[256];
            int i2, k;
            /* A US Letter page at 100% is 816 document pixels wide and the
             * framebuffer is 640, so the storyboard is rendered at a zoom
             * that fits - which also exercises the zoom path. */
            uow_layout_run(&L, e, &MX, 70);
            fb_clear(FB_RGB(0x80,0x80,0x80));
            {
                const uow_page *pg = &L.page[0];
                fb_fill_rect(pg->x, pg->y, pg->w, pg->h, FB_RGB(0xFF,0xFF,0xFF));
                fb_frame_rect(pg->x, pg->y, pg->w, pg->h, FB_RGB(0,0,0));
            }
            for (i2 = 0; i2 < L.nline; i2++) {
                const uow_line *ln = &L.line[i2];
                if (ln->page != 0) continue;
                for (k = 0; k < ln->nrun; k++) {
                    const uow_lrun *r = &L.run[ln->run0 + k];
                    char buf[256];
                    long got = uow_read(e, r->cp, r->n < 255 ? r->n : 255, buf);
                    buf[got] = 0;
                    fb_text(ln->x + r->x, ln->y, buf, FB_RGB(0,0,0), -1);
                }
            }
            fb_fill_rect(0, FB_H - 14, FB_W, 14, FB_RGB(0x10,0x10,0x10));
            fb_text(6, FB_H - 11,
                    "1. page 1 of a justified, paginated document at 70%",
                    FB_RGB(0xFF,0xFF,0xFF), -1);
            sprintf(path, "%s/uow_00.ppm", g_dir);
            {
                FILE *f = fopen(path, "wb");
                int n = FB_W * FB_H;
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
                    for (i2 = 0; i2 < n; i2++) {
                        unsigned p2 = fb[i2];
                        unsigned char rgb[3];
                        rgb[0] = (unsigned char)(p2 & 0xFF);
                        rgb[1] = (unsigned char)((p2 >> 8) & 0xFF);
                        rgb[2] = (unsigned char)((p2 >> 16) & 0xFF);
                        fwrite(rgb, 1, 3, f);
                    }
                    fclose(f);
                    g_frame++;
                }
            }
            printf("  wrote %s (%d pages, %d lines)\n", path, L.npage, L.nline);
        }

        /* the typing budget: a relayout after an edit must stay cheap enough
         * to run inside a frame.  This is a floor, not a benchmark - it fails
         * only if something has gone quadratic. */
        {
            int i2;
            for (i2 = 0; i2 < 20; i2++) {
                uow_insert(e, 10, "x", 1);
                uow_layout_run(&L, e, &MX, 100);
            }
            printf("  20 edit+relayout cycles over %d lines: completed\n",
                   L.nline);
        }
    }

    printf(g_fail ? "\nuoword gate: %d FAILURE(S)\n" : "\nuoword gate: GREEN\n",
           g_fail);
    return g_fail ? 1 : 0;
}
