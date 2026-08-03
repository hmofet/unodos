/* ===========================================================================
 * uoshow_test - the host gate for UnoShow's model, geometry and renderer
 * (OFFICE97-PLAN §7 phase 11).
 *
 * Four halves, and the mix is the point (the same argument uochrome_test's
 * header makes):
 *
 *  - MODEL: slides, z-order, grouping, layouts, and the pool invariant the
 *    whole store rests on - a shape's paragraphs and text are contiguous and
 *    sorted together.  Break it and compaction quietly moves one body's text
 *    into another's, which no screenshot shows and every deck would suffer.
 *  - GEOMETRY: every autoshape returns a closed path inside its box.  A path
 *    that leaves the box draws over the shape next to it at every zoom.
 *  - RENDER: pixels sampled where the claim lives - the scheme background, a
 *    shape's fill, the B&W bands - plus render-twice determinism.
 *  - SCALE: the same slide drawn at two sizes must put the same shape at
 *    proportionally the same place.  That is the ONE claim that makes a
 *    sorter thumbnail trustworthy, and it cannot be checked by eye.
 * ======================================================================== */
#include "uoshow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks, g_frame;
static const char *g_dir = "build";

static void fail(const char *what, const char *d)
{ printf("  FAIL %s: %s\n", what, d); g_fail++; }
static void eq(const char *what, long got, long want)
{ g_checks++; if (got != want) { char b[160];
    sprintf(b, "got %ld, wanted %ld", got, want); fail(what, b); } }
static void ok(const char *what, int cond, const char *d)
{ g_checks++; if (!cond) fail(what, d); }

/* ---- the metrics seam, over the host's 8x8 bitmap font --------------------
 * `px` is the pixel size the run wants; the bitmap font has exactly one, so
 * the seam scales its answers.  Crude, monotonic, and enough for the layout
 * assertions - which is the whole reason the seam exists. */
static int mx_w(const char *s, int n, const uos_chp *c, int px, void *ctx)
{
    char b[512];
    int i, w;
    (void)ctx; (void)c;
    for (i = 0; i < n && i < 511; i++) b[i] = s[i];
    b[i] = 0;
    w = fb_text_w(b);
    if (px > 0) w = w * px / 12;
    return w;
}
static int mx_h(const uos_chp *c, int px, void *ctx)
{ (void)c; (void)ctx; return fb_text_h() * (px > 0 ? px : 12) / 12 + 1; }
static void mx_draw(int x, int y, const char *s, int n, const uos_chp *c,
                    int px, fb_px col, void *ctx)
{
    char b[512];
    int i;
    (void)ctx; (void)c; (void)px;
    for (i = 0; i < n && i < 511; i++) b[i] = s[i];
    b[i] = 0;
    fb_text(x, y, b, col, -1);
}
static uos_metrics MX = { mx_w, mx_h, mx_draw, 0 };

/* ---- frames ------------------------------------------------------------------ */
static void shot(const char *tag, const char *caption)
{
    char path[256];
    FILE *f;
    int i, n = FB_W * FB_H;
    fb_fill_rect(0, FB_H - 14, FB_W, 14, FB_RGB(0x10, 0x10, 0x10));
    fb_text(6, FB_H - 11, caption, FB_RGB(0xFF, 0xFF, 0xFF), -1);
    sprintf(path, "%s/uos_%02d_%s.ppm", g_dir, g_frame, tag);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < n; i++) {
        unsigned p = fb[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(p & 0xFF);
        rgb[1] = (unsigned char)((p >> 8) & 0xFF);
        rgb[2] = (unsigned char)((p >> 16) & 0xFF);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    g_frame++;
}
static fb_px px_at(int x, int y) { return fb[y * FB_W + x]; }
static void px_is(const char *what, int x, int y, fb_px want)
{
    fb_px got = px_at(x, y);
    g_checks++;
    if (got != want) {
        char b[200];
        sprintf(b, "(%d,%d) is %06X, wanted %06X", x, y,
                (unsigned)(got & 0xFFFFFF), (unsigned)(want & 0xFFFFFF));
        fail(what, b);
    }
}

/* ---- 1. geometry --------------------------------------------------------------- */
static void test_geometry(void)
{
    int g, bad = 0, nline = 0;
    for (g = 0; g < UOS_G_COUNT; g++) {
        short xy[64];
        int n = uos_geom_path(g, 0, xy, 16), i;
        if (uos_geom_kind(g) == UOS_GK_LINE) nline++;
        if (n < 3) { char b[120];
            sprintf(b, "%s returned %d points", uos_geom_name(g), n);
            fail("geom path", b); continue; }
        for (i = 0; i < n * 2; i++)
            if (xy[i] < 0 || xy[i] > UOS_GEOM_BOX) {
                char b[160];
                sprintf(b, "%s point %d = %d, outside 0..%d",
                        uos_geom_name(g), i / 2, xy[i], UOS_GEOM_BOX);
                fail("geom box", b);
                bad++;
                break;
            }
        g_checks++;
    }
    ok("geom box", bad == 0, "some path left the 1000x1000 box");
    /* the adjustment must actually adjust the shapes that have one */
    {
        short a[64], b[64];
        int n1 = uos_geom_path(UOS_G_TRIANGLE, 200, a, 16);
        int n2 = uos_geom_path(UOS_G_TRIANGLE, 800, b, 16);
        eq("triangle points", n1, 3);
        eq("triangle points", n2, 3);
        ok("adjustment moves the apex", a[0] != b[0], "adj was ignored");
    }
    eq("one line-kind shape", nline, 1);
    printf("  %d autoshape geometries checked\n", UOS_G_COUNT);
}

/* ---- 2. the model ---------------------------------------------------------------- */
static void test_model(void)
{
    uos_pres *p = uos_new();
    int s1, s2, z, i;

    eq("a new deck has one slide", uos_slides(p), 1);
    eq("and it is a Title Slide", uos_slide_layout(p, 0), UOS_AL_TITLE);
    ok("with a centre title placeholder",
       uos_placeholder(p, 0, UOS_PH_CTRTITLE) >= 0, "no title holder");

    s1 = uos_slide_add(p, UOS_AL_BULLETS);
    s2 = uos_slide_add(p, UOS_AL_2COL);
    eq("three slides", uos_slides(p), 3);
    eq("slide 2 is Bulleted List", uos_slide_layout(p, s1), UOS_AL_BULLETS);
    ok("2 Column Text has two bodies",
       uos_placeholder(p, s2, UOS_PH_BODY) >= 0 &&
       uos_placeholder(p, s2, UOS_PH_BODY2) >= 0, "missing a column");

    /* Re-laying out keeps the title that is already there - which is what
     * PowerPoint does, and the reason a layout is a table and not a wipe. */
    uos_text_set(p, s1, uos_placeholder(p, s1, UOS_PH_TITLE), "Kept");
    uos_slide_set_layout(p, s1, UOS_AL_2COL);
    {
        int t = uos_placeholder(p, s1, UOS_PH_TITLE);
        int len = 0;
        const char *txt;
        ok("the title survived a layout change", t >= 0, "title holder gone");
        txt = uos_para_text(p, s1, t, 0, &len);
        g_checks++;
        if (len != 4 || strncmp(txt, "Kept", 4)) fail("layout keeps text", "title text lost");
    }

    /* z-order */
    z = uos_shape_add(p, s2, UOS_G_ELLIPSE, 100, 100, 200, 150);
    ok("a shape was added", z >= 0, "shape_add failed");
    {
        int n = uos_shapes(p, s2);
        const uos_shape *top = uos_shape_at_c(p, s2, n - 1);
        eq("the new shape is on top", top->geom, UOS_G_ELLIPSE);
        uos_shape_lower(p, s2, n - 1, 1);
        eq("send to back", uos_shape_at_c(p, s2, 0)->geom, UOS_G_ELLIPSE);
        uos_shape_raise(p, s2, 0, 1);
        eq("bring to front", uos_shape_at_c(p, s2, n - 1)->geom, UOS_G_ELLIPSE);
    }

    /* grouping */
    {
        int zs[2], g;
        zs[0] = 0; zs[1] = 1;
        g = uos_shape_group(p, s2, zs, 2);
        ok("two shapes grouped", g > 0, "group failed");
        eq("both carry the group", uos_shape_at_c(p, s2, 0)->group,
           uos_shape_at_c(p, s2, 1)->group);
        eq("ungroup releases both", uos_shape_ungroup(p, s2, 0), 2);
        eq("and clears the id", uos_shape_at_c(p, s2, 0)->group, 0);
    }

    /* slide order */
    uos_slide_move(p, 0, 2);
    eq("moving a slide reorders", uos_slide_layout(p, 2), UOS_AL_TITLE);
    uos_slide_move(p, 2, 0);
    eq("and back", uos_slide_layout(p, 0), UOS_AL_TITLE);

    uos_slide_delete(p, 2);
    eq("deleting leaves two", uos_slides(p), 2);

    /* hidden slides are model state, not a renderer trick */
    uos_slide_hide(p, 1, 1);
    eq("hide", uos_slide_hidden(p, 1), 1);
    uos_slide_hide(p, 1, 0);

    /* text bodies and outline levels */
    {
        int b = uos_placeholder(p, 1, UOS_PH_BODY);
        int len = 0;
        ok("body holder", b >= 0, "no body");
        uos_text_set(p, 1, b, "One\nTwo\nThree");
        eq("three paragraphs", uos_text_paras(p, 1, b), 3);
        uos_para_text(p, 1, b, 1, &len);
        eq("second paragraph length", len, 3);
        uos_para_set_level(p, 1, b, 1, 2);
        eq("demoted to level 2", uos_para_at(p, 1, b, 1)->level, 2);
        ok("and its size shrank",
           uos_para_at(p, 1, b, 1)->chp.size < uos_para_at(p, 1, b, 0)->chp.size,
           "level did not change the size");
        uos_para_add(p, 1, b, "Four", 1);
        eq("appending gives four", uos_text_paras(p, 1, b), 4);
        {
            const char *t = uos_para_text(p, 1, b, 3, &len);
            g_checks++;
            if (len != 4 || strncmp(t, "Four", 4)) fail("append", "wrong text");
            t = uos_para_text(p, 1, b, 0, &len);
            g_checks++;
            if (len != 3 || strncmp(t, "One", 3))
                fail("append re-homes the run", "the FIRST paragraph moved or corrupted");
        }
    }

    /* THE POOL TEST.  Rewrite one body far more often than the pools hold,
     * then check that a body nobody touched is still exactly itself.  This is
     * the UnoCalc token-pool lesson in the shape this model needs it. */
    {
        int b = uos_placeholder(p, 1, UOS_PH_BODY);
        int t = uos_placeholder(p, 0, UOS_PH_CTRTITLE);
        int len = 0;
        const char *txt;
        uos_text_set(p, 0, t, "Untouched Title");
        for (i = 0; i < 3000; i++) {
            char buf[64];
            sprintf(buf, "line %d\nsecond %d\nthird %d", i, i, i);
            if (!uos_text_set(p, 1, b, buf)) { fail("pool", "text_set ran out"); break; }
        }
        txt = uos_para_text(p, 0, t, 0, &len);
        g_checks++;
        if (len != 15 || strncmp(txt, "Untouched Title", 15)) {
            char d[200];
            sprintf(d, "after 3000 rewrites the other slide's title is \"%.*s\"", len, txt);
            fail("pool compaction", d);
        }
        eq("the rewritten body still has three", uos_text_paras(p, 1, b), 3);
        txt = uos_para_text(p, 1, b, 2, &len);
        g_checks++;
        if (strncmp(txt, "third", 5)) fail("pool compaction", "the live body is wrong");
        printf("  shared shape/paragraph/text pools: 3000 rewrites survived\n");
    }
    uos_free(p);
}

/* ---- 3. rendering ----------------------------------------------------------------- */
static uos_pres *fixture(void)
{
    uos_pres *p = uos_new();
    int s, z;

    uos_text_set(p, 0, uos_placeholder(p, 0, UOS_PH_CTRTITLE), "UnoShow");
    uos_text_set(p, 0, uos_placeholder(p, 0, UOS_PH_SUBTITLE),
                 "a PowerPoint 97 clone for UnoDOS");

    s = uos_slide_add(p, UOS_AL_BULLETS);
    uos_text_set(p, s, uos_placeholder(p, s, UOS_PH_TITLE), "What it draws");
    z = uos_placeholder(p, s, UOS_PH_BODY);
    uos_text_set(p, s, z, "Twenty autoshapes");
    uos_para_add(p, s, z, "Solid, gradient and pattern fills", 1);
    uos_para_add(p, s, z, "Shadows, dashes and dots", 1);
    uos_para_add(p, s, z, "Five outline levels", 2);

    s = uos_slide_add(p, UOS_AL_TITLE_ONLY);
    uos_text_set(p, s, uos_placeholder(p, s, UOS_PH_TITLE), "The shape battery");
    {
        int g, col = 0, row = 0;
        for (g = 0; g < UOS_G_COUNT; g++) {
            int zz = uos_shape_add(p, s, g, 40 + col * 130, 170 + row * 100, 100, 76);
            uos_shape *sh = uos_shape_at(p, s, zz);
            if (!sh) break;
            sh->fill.kind = (g % 4 == 1) ? UOS_F_GRAD_V
                          : (g % 4 == 2) ? UOS_F_PATTERN : UOS_F_SOLID;
            sh->fill.pattern = (unsigned char)(g % UOS_P_COUNT);
            sh->fill.c1 = UOS_SCHEME_COLOR(UOS_C_FILL);
            sh->fill.c2 = UOS_SCHEME_COLOR(UOS_C_ACCENT);
            sh->shadow.on = (unsigned char)(g % 3 == 0);
            sh->shadow.dx = 5; sh->shadow.dy = 5;
            if (++col == 5) { col = 0; row++; }
        }
    }
    return p;
}

static void test_render(void)
{
    uos_pres *p = fixture();
    uos_map m;
    fb_px bg, fill;
    int i;

    uos_set_metrics(&MX);

    /* --- the background IS the scheme's background role ------------------- */
    fb_clear(FB_RGB(0x20, 0x20, 0x28));
    uos_render(p, 0, 20, 10, FB_W - 40, FB_H - 40, UOS_R_PHFRAMES, &m);
    bg = uos_get_scheme(p)->c[UOS_C_BG];
    px_is("slide background", m.x + 4, m.y + 4, bg);
    ok("the slide is letterboxed inside the rectangle",
       m.w <= FB_W - 40 && m.h <= FB_H - 40 && m.w > 0, "fit failed");
    eq("and keeps 4:3", (long)(m.w * UOS_SLIDE_H / UOS_SLIDE_W), (long)m.h);
    shot("title", "1. the title slide: scheme background, centred title and subtitle");

    /* --- a shape's fill is its fill --------------------------------------- */
    fb_clear(FB_RGB(0x20, 0x20, 0x28));
    uos_render(p, 2, 0, 0, FB_W, FB_H - 16, 0, &m);
    fill = uos_get_scheme(p)->c[UOS_C_FILL];
    {   /* the first shape is a solid rectangle at slide (40,170,100,76) */
        int cx, cy;
        uos_to_screen(&m, 40 + 50, 170 + 38, &cx, &cy);
        px_is("a solid shape's centre", cx, cy, fill);
    }
    shot("shapes", "2. the shape battery: 20 autoshapes, 3 fills, shadows");

    /* --- determinism: the same state twice is the same pixels ------------- */
    {
        static fb_px first[FB_W * FB_H];
        int diff = 0;
        for (i = 0; i < FB_W * FB_H; i++) first[i] = fb[i];
        fb_clear(FB_RGB(0x20, 0x20, 0x28));
        uos_render(p, 2, 0, 0, FB_W, FB_H - 16, 0, &m);
        fb_fill_rect(0, FB_H - 14, FB_W, 14, FB_RGB(0x10, 0x10, 0x10));
        fb_text(6, FB_H - 11,
                "2. the shape battery: 20 autoshapes, 3 fills, shadows",
                FB_RGB(0xFF, 0xFF, 0xFF), -1);
        for (i = 0; i < FB_W * FB_H; i++) if (first[i] != fb[i]) diff++;
        ok("rendering the same slide twice is byte-identical", diff == 0,
           "the painter accumulates rather than drawing from scratch");
    }

    /* --- Black and White view --------------------------------------------- */
    fb_clear(FB_RGB(0x20, 0x20, 0x28));
    uos_render(p, 2, 0, 0, FB_W, FB_H - 16, UOS_R_BW, &m);
    {
        int cx, cy;
        uos_to_screen(&m, 40 + 50, 170 + 38, &cx, &cy);
        px_is("B&W collapses a fill to one of three", cx, cy, uos_bw(fill));
    }
    ok("B&W of black is black", uos_bw(FB_RGB(0,0,0)) == FB_RGB(0,0,0), "");
    ok("B&W of white is white", uos_bw(FB_RGB(255,255,255)) == FB_RGB(255,255,255), "");
    shot("bw", "3. Black and White view: three bands, not a greyscale photo");

    /* --- SCALE INVARIANCE, the claim the sorter rests on -------------------
     * The same shape drawn at two very different sizes must land at
     * proportionally the same place.  Anything that rounds in the wrong order
     * passes at 100% and drifts on a thumbnail. */
    {
        uos_map big, small;
        int bx, by, sx, sy;
        fb_clear(FB_RGB(0x30, 0x30, 0x38));
        uos_render(p, 2, 0, 0, FB_W, FB_H - 16, 0, &big);
        uos_to_screen(&big, 360, 270, &bx, &by);
        uos_render(p, 2, 8, 8, 160, 120, UOS_R_NOTEXT, &small);
        uos_to_screen(&small, 360, 270, &sx, &sy);
        g_checks++;
        {   /* centre of the slide is the centre of the rectangle, both times */
            int be = bx - (big.x + big.w / 2), se = sx - (small.x + small.w / 2);
            if (be > 1 || be < -1 || se > 1 || se < -1) {
                char d[200];
                sprintf(d, "slide centre lands %d px off at full size and %d px off at thumbnail",
                        be, se);
                fail("scale invariance", d);
            }
        }
        /* CLIPPING, which is what makes a sorter row safe: the battery's
         * bottom row deliberately hangs past the slide edge, and not one
         * pixel of it may land outside the slide rectangle. */
        {
            int bad = 0, xx, yy;
            fb_clear(FB_RGB(0x11, 0x22, 0x33));
            uos_render(p, 2, 60, 30, 300, 225, 0, &small);
            for (yy = 0; yy < FB_H; yy++)
                for (xx = 0; xx < FB_W; xx++) {
                    int inside = xx >= small.x && xx < small.x + small.w &&
                                 yy >= small.y && yy < small.y + small.h;
                    if (!inside && px_at(xx, yy) != FB_RGB(0x11, 0x22, 0x33)) bad++;
                }
            g_checks++;
            if (bad) {
                char d[160];
                sprintf(d, "%d pixels landed outside the slide rectangle", bad);
                fail("clipping", d);
            }
        }

        /* a sorter row of thumbnails, which is that claim used in anger */
        fb_clear(FB_RGB(0x40, 0x44, 0x50));
        for (i = 0; i < uos_slides(p); i++) {
            uos_map t;
            fb_fill_rect(12 + i * 160, 20, 148, 116, FB_RGB(0, 0, 0));
            uos_render(p, i, 14 + i * 160, 22, 144, 112, UOS_R_NOTEXT, &t);
            uos_render(p, i, 14 + i * 160, 150, 144, 112, 0, &t);
        }
        shot("sorter", "4. one renderer, two sizes: shapes-only and full, per slide");
    }

    uos_set_metrics(0);
    uos_free(p);
}

int main(int argc, char **argv)
{
    if (argc > 1) g_dir = argv[1];
    printf("uoshow model + geometry + render gate\n");
    test_geometry();
    test_model();
    test_render();
    printf(g_fail ? "\nuoshow gate: %d FAILURE(S) in %d checks\n"
                  : "\nuoshow gate: GREEN (%d checks, %d frames)\n",
           g_fail ? g_fail : g_checks, g_fail ? g_checks : g_frame);
    return g_fail ? 1 : 0;
}
