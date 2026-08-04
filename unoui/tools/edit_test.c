/* ===========================================================================
 * unoui editable-text geometry contract test (host), plus the one other
 * input-layer promise that had no gate: that UI_F_DISABLED disables.
 *
 * THE CONTRACT.  Three pieces of code have to agree on where glyph number N
 * of an editable field starts: the painter's pen, ui_text_caret_xy (where the
 * caret is drawn), and ui_text_index_at (which glyph a click landed on).  If
 * they disagree the caret appears somewhere other than where you clicked.
 *
 * WHY THIS FILE EXISTS.  ui_text_index_at used to walk the line one glyph at a
 * time, adding ui_seg_w(buf, i, i+1) - the width of that character ON ITS OWN.
 * With a proportional font that is wrong twice over: fb_text_w rounds its
 * fractional pen to whole pixels once per call, so a per-character sum banks up
 * to half a pixel of error EVERY character, and it never sees the kerning
 * between a pair.  A dozen characters in, the click was landing a glyph or two
 * away from the pointer.  Reported from metal as "the cursor jumps to the wrong
 * spot when entering the WiFi password"; it was every text field in the OS.
 *
 * The bitmap font cannot show any of this - 8 px per glyph, no kerning, no
 * fractions - so the test registers a SYNTHETIC proportional provider with the
 * same shape as pc64_font.c's (26.6 fixed-point pen, per-glyph fractional
 * advances, a kern table) and pins the contract under it.
 *
 *   cc -I. -I../pc64 unoui.c unoui_input.c themes/theme_unodos.c \
 *      ../pc64/fb.c tools/edit_test.c -o build/edit_test
 * ======================================================================== */
#include "unoui_theme.h"
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(name, cond) do {                                              \
    if (cond) printf("  ok   %s\n", name);                                  \
    else { printf("  FAIL %s\n", name); fails++; }                          \
} while (0)

/* ---- a synthetic proportional font ---------------------------------------
 * Advances are deliberately fractional (26.6) and all different, and there is
 * a kern pair, so a per-character sum and a whole-prefix measure diverge fast.
 * Nothing is drawn: only the pen matters here. */
static int adv26_of(int cp)
{
    if (cp < 32 || cp > 126) return 6 * 64;
    /* whole widths vary per glyph, but every fraction sits just under half a
     * pixel - the shape a real hinted face has, and the one where rounding
     * per character (rather than measuring the whole prefix once) drifts the
     * SAME way every time instead of cancelling out */
    return (5 + (cp % 5)) * 64 + 26 + (cp % 6);  /* 5.41 .. 9.48 px */
}
static int kern26_of(int a, int b)
{
    if (a == 'A' && b == 'V') return -18;        /* -0.28 px */
    if (a == 'r' && b == 'a') return -7;
    if (((a ^ b) & 3) == 0)   return -5;
    return 0;
}
static int pen_run(int x, const char *s)
{
    int pen26 = x * 64, prev = 0;
    for (; *s; s++) {
        int cp = (unsigned char)*s;
        if (prev) pen26 += kern26_of(prev, cp);
        pen26 += adv26_of(cp);
        prev = cp;
    }
    return (pen26 + 32) >> 6;
}
static int p_text_w(const char *s)          { return pen_run(0, s); }
static int p_text(int x, int y, const char *s, fb_px fg, long bg)
{ (void)y; (void)fg; (void)bg; return pen_run(x, s); }
static int p_glyph(int x, int y, int cp, fb_px fg, long bg)
{ (void)y; (void)fg; (void)bg; return x + ((adv26_of(cp) + 32) >> 6); }
static int p_height(void) { return 14; }
static const fb_font g_prop = { p_glyph, p_text_w, p_height, p_text };

/* ---- the field under test ------------------------------------------------ */
static char        g_buf[128];
static unoui_text  g_t;
static unoui_rect  g_inner;

static void field_set(const char *s, int w)
{
    unoui_rect r;
    const unoui_theme *th = &theme_unodos;
    snprintf(g_buf, sizeof g_buf, "%s", s);
    unoui_text_init(&g_t, g_buf, (int)sizeof g_buf, 0);
    r.x = 40; r.y = 60; r.w = w; r.h = ui_field_h();
    g_inner = ui_edit_inner(r, th);
}

/* every caret gap, clicked exactly where the caret is drawn, must resolve back
 * to its own index - the round trip that makes a click land where you pointed */
static int roundtrip_worst(void)
{
    int i, worst = 0;
    for (i = 0; i <= g_t.len; i++) {
        int cx, cy, got;
        ui_text_caret_xy(g_inner, &g_t, i, &cx, &cy);
        got = ui_text_index_at(g_inner, &g_t, cx, cy);
        if (got - i >  worst) worst = got - i;
        if (i - got >  worst) worst = i - got;
    }
    return worst;
}

int main(void)
{
    const char *pw = "AVeryLongWifiPassphrase2026";

    printf("unoui editable-text geometry\n");

    /* ---- 1. the bitmap font (8 px, no kerning): the easy case ------------ */
    fb_set_font(0);
    field_set(pw, 300);
    CHECK("bitmap: caret x <-> click index round-trips", roundtrip_worst() == 0);
    { int cx, cy;
      ui_text_caret_xy(g_inner, &g_t, 0, &cx, &cy);
      CHECK("bitmap: index 0 sits at the inner left inset", cx == g_inner.x + 3); }

    /* ---- 2. a PROPORTIONAL font: the case that was broken ---------------- */
    fb_set_font(&g_prop);
    field_set(pw, 300);
    CHECK("proportional: caret x <-> click index round-trips", roundtrip_worst() == 0);

    /* the measure the caret uses and the pen the painter runs must be the same
     * number, or the caret is drawn off the glyph flow whatever the hit test
     * does */
    { int i, bad = 0;
      for (i = 0; i <= g_t.len; i++) {
          char tmp[128]; int cx, cy;
          memcpy(tmp, g_buf, (size_t)i); tmp[i] = 0;
          ui_text_caret_xy(g_inner, &g_t, i, &cx, &cy);
          if (cx != g_inner.x + 3 + p_text(0, 0, tmp, 0, -1)) bad++;
      }
      CHECK("proportional: caret x tracks the painter's pen", bad == 0); }

    /* a click in the middle of glyph k snaps to the nearer gap, never past it */
    { int k, bad = 0;
      for (k = 0; k < g_t.len; k++) {
          int ax, ay, bx, by, got;
          ui_text_caret_xy(g_inner, &g_t, k,     &ax, &ay);
          ui_text_caret_xy(g_inner, &g_t, k + 1, &bx, &by);
          got = ui_text_index_at(g_inner, &g_t, ax + (bx - ax) / 4, ay);
          if (got != k) bad++;                       /* left quarter -> k     */
          got = ui_text_index_at(g_inner, &g_t, bx - (bx - ax) / 4, ay);
          if (got != k + 1) bad++;                   /* right quarter -> k+1  */
      }
      CHECK("proportional: a click snaps to the nearer glyph gap", bad == 0); }

    /* past the end of the text lands on the end, not somewhere inside it */
    { int cx, cy;
      ui_text_caret_xy(g_inner, &g_t, g_t.len, &cx, &cy);
      CHECK("proportional: a click past the last glyph lands at len",
            ui_text_index_at(g_inner, &g_t, cx + 500, cy) == g_t.len); }

    /* ---- 3. a SCROLLED field: the same contract with scroll_x nonzero ---- */
    field_set(pw, 90);                       /* narrower than the text */
    g_t.caret = g_t.sel = g_t.len;
    ui_text_reveal(g_inner, &g_t);
    CHECK("scrolled: reveal actually scrolled", g_t.scroll_x > 0);
    CHECK("scrolled: caret x <-> click index round-trips", roundtrip_worst() == 0);

    /* ---- 4. multi-line, since the textarea shares the geometry ----------- */
    fb_set_font(&g_prop);
    { unoui_rect r; const unoui_theme *th = &theme_unodos;
      snprintf(g_buf, sizeof g_buf, "%s", "first line\nsecond line is longer\nx");
      unoui_text_init(&g_t, g_buf, (int)sizeof g_buf, 1);
      r.x = 40; r.y = 60; r.w = 300; r.h = 80;
      g_inner = ui_edit_inner(r, th); }
    CHECK("multiline: caret x <-> click index round-trips", roundtrip_worst() == 0);

    /* ---- 5. UI_F_DISABLED actually disables ----------------------------- *
     * It used to be a paint-only flag: every theme dimmed the text and the
     * input layer hit, focused and fired the control regardless.  A greyed
     * button that still works is a lie about what a click will do. */
    fb_set_font(0);
    { static unoui_ui ui; static unoui_window win;
      unoui_widget *live, *dead;
      unoui_event e; unoui_action a;
      unoui_ui_init(&ui, &theme_unodos, 640, 480);
      unoui_window_init(&win, "T", 20, 20, 240, 120);
      live = unoui_add_button(&win, 8,  8, 100, "Live", 0);          live->id = 11;
      dead = unoui_add_button(&win, 8, 40, 100, "Dead", UI_F_DISABLED); dead->id = 22;
      unoui_ui_add(&ui, &win);

      memset(&e, 0, sizeof e); e.kind = UI_EV_MOUSE_DOWN;
      { unoui_rect r = unoui_widget_rect(ui.theme, &win, dead);
        e.x = r.x + r.w / 2; e.y = r.y + r.h / 2; }
      a = unoui_handle(&ui, &e);
      memset(&e, 0, sizeof e); e.kind = UI_EV_MOUSE_UP;
      { unoui_rect r = unoui_widget_rect(ui.theme, &win, dead);
        e.x = r.x + r.w / 2; e.y = r.y + r.h / 2; }
      { unoui_action b = unoui_handle(&ui, &e); if (b.changed) a = b; }
      CHECK("disabled: a click on it fires nothing", !(a.changed && a.id == 22));

      /* Tab must skip it rather than parking the keyboard on a dead control */
      ui.focus_wi = -1;
      memset(&e, 0, sizeof e); e.kind = UI_EV_KEY; e.key = UI_KEY_TAB;
      unoui_handle(&ui, &e);
      unoui_handle(&ui, &e);
      CHECK("disabled: Tab never lands on it",
            ui.focus_wi < 0 || win.w[ui.focus_wi].id != 22); }

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
