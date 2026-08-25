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
#include "unoui_wmanim.h"
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
    /* a FRESH field each case: re-init keeps the caret and the scroll when the
     * buffer is the same one, which is the point of it, and would otherwise
     * carry one case's scroll position into the next */
    memset(&g_t, 0, sizeof g_t);
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

    /* ---- 6. secret fields ----------------------------------------------- *
     * The model must hold the REAL text while everything geometric measures
     * the MASK. Getting that backwards is the bug the old per-app trick had:
     * feeding the widget '*' made its caret, selection and length belong to
     * the mask, so the app could not tell where in the PASSWORD it was. */
    fb_set_font(&g_prop);
    field_set("secret-pass", 300);
    unoui_text_secret(&g_t, '*');
    CHECK("secret: the buffer is still the real text",
          !strcmp(g_t.buf, "secret-pass") && g_t.len == 11);
    { int cx, cy, mx;
      char mask[16]; int i;
      for (i = 0; i < 5; i++) mask[i] = '*';
      mask[5] = 0;
      ui_text_caret_xy(g_inner, &g_t, 5, &cx, &cy);
      mx = g_inner.x + 3 + p_text(0, 0, mask, 0, -1);
      CHECK("secret: caret x follows the MASK, not the text", cx == mx); }
    CHECK("secret: caret x <-> click index still round-trips",
          roundtrip_worst() == 0);
    /* revealed, every measurement swaps back to the real glyphs */
    unoui_text_show(&g_t, 1);
    { int cx, cy;
      char pre[16]; memcpy(pre, g_buf, 5); pre[5] = 0;
      ui_text_caret_xy(g_inner, &g_t, 5, &cx, &cy);
      CHECK("revealed: caret x follows the real text",
            cx == g_inner.x + 3 + p_text(0, 0, pre, 0, -1)); }
    CHECK("revealed: caret x <-> click index still round-trips",
          roundtrip_worst() == 0);
    unoui_text_show(&g_t, 0);
    CHECK("secret: multiline is refused", g_t.multiline == 0);
    { unoui_rect eye = ui_edit_eye_rect(g_inner, &g_t);
      unoui_rect txt = ui_edit_text_rect(g_inner, &g_t);
      CHECK("secret: the eye is inside the field",
            eye.w > 0 && eye.x + eye.w <= g_inner.x + g_inner.w);
      CHECK("secret: the text area stops before the eye",
            txt.x + txt.w <= eye.x); }
    { unoui_text plain; static char pb[8] = "hi";
      unoui_text_init(&plain, pb, sizeof pb, 0);
      CHECK("plain: no eye, and the whole inner rect is text",
            ui_edit_eye_rect(g_inner, &plain).w == 0 &&
            ui_edit_text_rect(g_inner, &plain).w == g_inner.w); }
    /* `plain` above is a STACK LOCAL on purpose, and it caught the real bug
     * once by luck - whatever the stack held made unoui_text_init believe it
     * was a re-bind, so `secret` survived from the field before it. Luck is
     * not a gate, so this fills the struct with a pattern first and asks the
     * same question deterministically. */
    { union { unoui_text t; unsigned char b[sizeof(unoui_text)]; } u;
      static char gb[8] = "hi";
      unsigned i;
      for (i = 0; i < sizeof u.b; i++) u.b[i] = 0xA5;
      unoui_text_init(&u.t, gb, sizeof gb, 0);
      CHECK("garbage: an uninitialised struct is not believed",
            u.t.secret == 0 && u.t.revealed == 0 && u.t.multiline == 0);
      CHECK("garbage: and comes up with a sane caret and view",
            u.t.caret == 2 && u.t.sel == 2 &&
            u.t.scroll_x == 0 && u.t.scroll_y == 0); }
    /* a field too narrow to spare the room keeps all of it for the text */
    field_set("pw", 40);
    unoui_text_secret(&g_t, '*');
    CHECK("secret: a narrow field gives up the eye, not its text",
          ui_edit_eye_rect(g_inner, &g_t).w == 0 &&
          ui_edit_text_rect(g_inner, &g_t).w == g_inner.w);

    /* the eye is a CLICK TARGET, and clicking it must toggle the mask without
     * also dragging the caret to wherever the eye happens to sit */
    fb_set_font(0);
    { static unoui_ui ui; static unoui_window win;
      static char pb[32] = "hunter2";
      static unoui_text pt;
      unoui_widget *fld;
      unoui_event e;
      unoui_rect r, inner, eye;
      unoui_text_init(&pt, pb, sizeof pb, 0);
      unoui_text_secret(&pt, '*');
      unoui_ui_init(&ui, &theme_unodos, 640, 480);
      unoui_window_init(&win, "P", 20, 20, 300, 90);
      fld = unoui_add_edit(&win, 8, 8, 260, &pt);
      unoui_ui_add(&ui, &win);
      r = unoui_widget_rect(ui.theme, &win, fld);
      inner = ui_edit_inner(r, ui.theme);
      eye = ui_edit_eye_rect(inner, &pt);
      pt.caret = pt.sel = 3;

      memset(&e, 0, sizeof e); e.kind = UI_EV_MOUSE_DOWN;
      e.x = eye.x + eye.w / 2; e.y = eye.y + eye.h / 2;
      unoui_handle(&ui, &e);
      CHECK("eye: a click on it reveals", pt.revealed == 1);
      CHECK("eye: a click on it leaves the caret alone", pt.caret == 3);
      unoui_handle(&ui, &e);
      CHECK("eye: a second click hides again", pt.revealed == 0);

      /* a click in the TEXT still positions the caret */
      memset(&e, 0, sizeof e); e.kind = UI_EV_MOUSE_DOWN;
      e.x = inner.x + 3; e.y = inner.y + inner.h / 2;
      unoui_handle(&ui, &e);
      CHECK("eye: a click in the text still moves the caret",
            pt.caret == 0 && pt.revealed == 0);

      /* and losing the focus puts the mask back up, however it was lost */
      pt.revealed = 1;
      ui.focus_wi = -1;
      unoui_render_ui(&ui);
      CHECK("eye: reveal does not survive losing focus", pt.revealed == 0);

      /* THE TARGET IS THE FIELD'S FULL HEIGHT.
       *
       * It was a glyph-sized square centred vertically, which on a scaled
       * desktop is a ~10 px box floating in a field two or three times that
       * tall - reported from the Surface Laptop Go as "a really tiny hit box,
       * right in the dead centre". Every check above still passed while that
       * was true, because they all aimed at the exact middle. These aim at
       * the edges, which is where a real finger lands. */
      CHECK("eye: the target is as tall as the field",
            eye.h == inner.h);
      ui.focus_wi = 0;
      { int yy;
        for (yy = 0; yy < 2; yy++) {
            int wasrev = pt.revealed;
            memset(&e, 0, sizeof e); e.kind = UI_EV_MOUSE_DOWN;
            e.x = eye.x + eye.w / 2;
            e.y = yy ? eye.y + eye.h - 1 : eye.y;      /* top edge, bottom edge */
            unoui_handle(&ui, &e);
            CHECK("eye: a click at its top/bottom edge toggles too",
                  pt.revealed != wasrev);
        } }
      /* and it still must not steal the text's room on a normal field */
      CHECK("eye: the text area is still most of the field",
            ui_edit_text_rect(inner, &pt).w > inner.w / 2); }

    /* ---- 6b. a masked field NARROW enough to scroll ---------------------- *
     * Every secret check above uses a field wide enough to hold the whole
     * string, so none of them ever exercised scroll_x with a mask up. That is
     * the gap the Surface Laptop Go report ("entering the password makes the
     * cursor jump around to different points") points at, and it has to be
     * either closed or ruled out rather than guessed about.
     *
     * Type a long passphrase one event at a time into a field far too small
     * for it and assert, after EVERY character, that the caret is where the
     * model says it is and still drawn inside the field. A caret that jumps is
     * one that leaves the text rect or stops tracking t->caret. */
    fb_set_font(&g_prop);
    { static unoui_ui ui2; static unoui_window w2;
      static char nb[64] = "";
      static unoui_text nt;
      unoui_widget *fld;
      unoui_event e;
      const char *phrase = "correct-horse-battery-staple-42";
      int k, bad_x = 0, bad_caret = 0, bad_back = 0;
      unoui_rect r2, inner2, txt2;
      unoui_text_init(&nt, nb, sizeof nb, 0);
      unoui_text_secret(&nt, '*');
      unoui_ui_init(&ui2, &theme_unodos, 640, 480);
      unoui_window_init(&w2, "N", 10, 10, 200, 80);
      fld = unoui_add_edit(&w2, 8, 8, 90, &nt);     /* deliberately too narrow */
      unoui_ui_add(&ui2, &w2);
      ui2.focus_win = 0; ui2.focus_wi = 0;
      r2 = unoui_widget_rect(ui2.theme, &w2, fld);
      inner2 = ui_edit_inner(r2, ui2.theme);
      txt2 = ui_edit_text_rect(inner2, &nt);
      for (k = 0; phrase[k]; k++) {
          int cx, cy;
          memset(&e, 0, sizeof e);
          e.kind = UI_EV_CHAR; e.ch = phrase[k];
          unoui_handle(&ui2, &e);
          if (nt.caret != k + 1) bad_caret++;
          ui_text_caret_xy(txt2, &nt, nt.caret, &cx, &cy);
          if (cx < txt2.x || cx > txt2.x + txt2.w) bad_x++;
      }
      CHECK("secret+scroll: the caret counts every character typed",
            bad_caret == 0 && nt.len == (int)strlen(phrase));
      CHECK("secret+scroll: the buffer is the real passphrase",
            !strcmp(nb, phrase));
      CHECK("secret+scroll: the caret stays inside the field the whole way",
            bad_x == 0);
      /* and backspacing all the way out keeps it inside too - the scroll has
       * to unwind as well as wind */
      for (k = (int)strlen(phrase); k > 0; k--) {
          int cx, cy;
          memset(&e, 0, sizeof e);
          e.kind = UI_EV_KEY; e.key = UI_KEY_BACKSPACE;
          unoui_handle(&ui2, &e);
          ui_text_caret_xy(txt2, &nt, nt.caret, &cx, &cy);
          if (cx < txt2.x || cx > txt2.x + txt2.w) bad_back++;
      }
      CHECK("secret+scroll: and stays inside it backspacing out again",
            bad_back == 0 && nt.len == 0 && nt.scroll_x == 0); }
    fb_set_font(0);

    /* ---- 7. the busy indicator ------------------------------------------ */
    fb_set_font(0);
    { static unoui_window bw;
      unoui_widget *b;
      unoui_window_init(&bw, "B", 10, 10, 120, 80);
      b = unoui_add_busy(&bw, 8, 8, 16);
      CHECK("busy: sized as asked", b->r.w == 16 && b->r.h == 16);
      CHECK("busy: starts at phase 0", b->value == 0);
      { int i; for (i = 0; i < UI_BUSY_DOTS; i++) unoui_busy_step(b); }
      CHECK("busy: a full lap returns to phase 0", b->value == 0);
      unoui_busy_step(b);
      CHECK("busy: one step advances one dot", b->value == 1);
      b = unoui_add_busy(&bw, 8, 40, 0);
      CHECK("busy: size 0 means font-derived", b->r.w > 0 && b->r.w == b->r.h); }

    /* ---- 7b. re-init must not move the caret ---------------------------- *
     * A window builder runs on every rebuild and re-inits its fields, so this
     * is what stops the cursor jumping to the end of the text mid-word in any
     * window that rebuilds underneath you. */
    fb_set_font(0);
    { static char rb[32] = "hello world";
      static unoui_text rt;
      unoui_text_init(&rt, rb, sizeof rb, 0);
      CHECK("re-init: a FIRST bind puts the caret at the end", rt.caret == 11);
      rt.caret = rt.sel = 4; rt.scroll_x = 7;
      unoui_text_init(&rt, rb, sizeof rb, 0);      /* the rebuild */
      CHECK("re-init: re-binding the same buffer keeps the caret",
            rt.caret == 4 && rt.sel == 4 && rt.scroll_x == 7);
      /* ...clamped, because the text may have got shorter behind the model */
      rb[2] = 0;
      unoui_text_init(&rt, rb, sizeof rb, 0);
      CHECK("re-init: a caret past the new end is clamped, not left dangling",
            rt.len == 2 && rt.caret == 2 && rt.sel == 2);
      /* a DIFFERENT buffer is a different field: it resets */
      { static char ob[16] = "other";
        rt.caret = rt.sel = 1;
        unoui_text_init(&rt, ob, sizeof ob, 0);
        CHECK("re-init: binding a different buffer resets", rt.caret == 5); }
      /* and unoui_text_set is the explicit "replace it", which resets */
      { unoui_text_set(&rt, "abc");
        CHECK("text_set: replacing the contents resets the caret",
              rt.len == 3 && rt.caret == 3 && rt.scroll_x == 0); } }

    /* ---- 8. the reject gesture ------------------------------------------ *
     * A shake that does not come home is worse than no shake at all: whatever
     * it moved is left sitting to one side, permanently, and the layout is
     * wrong from then on. So the curve's endpoints matter more than its shape. */
    { int t, e, neg = 0, pos = 0;
      CHECK("shake: starts at rest", unoui_ease(UI_EASE_SHAKE, 0) == 0);
      CHECK("shake: ENDS at rest", unoui_ease(UI_EASE_SHAKE, UI_ANIM_ONE) == 0);
      for (t = 0; t <= UI_ANIM_ONE; t += 16) {
          e = unoui_ease(UI_EASE_SHAKE, t);
          if (e >  UI_ANIM_ONE / 8) pos++;
          if (e < -UI_ANIM_ONE / 8) neg++;
      }
      CHECK("shake: swings both ways", pos > 0 && neg > 0);
      /* decaying: the biggest excursion is in the first half */
      { int first = 0, second = 0;
        for (t = 0; t < UI_ANIM_ONE / 2; t += 16) {
            e = unoui_ease(UI_EASE_SHAKE, t); if (e < 0) e = -e;
            if (e > first) first = e; }
        for (t = UI_ANIM_ONE / 2; t <= UI_ANIM_ONE; t += 16) {
            e = unoui_ease(UI_EASE_SHAKE, t); if (e < 0) e = -e;
            if (e > second) second = e; }
        CHECK("shake: decays", first > second); } }

    { static unoui_ui ui; static unoui_anim ac; static unoui_window win;
      static char pb2[16] = "wrong";
      static unoui_text pt2;
      unoui_widget *fld;
      unoui_text_init(&pt2, pb2, sizeof pb2, 0);
      unoui_text_secret(&pt2, '*');
      unoui_anim_init(&ac);
      unoui_ui_init(&ui, &theme_unodos, 640, 480);
      unoui_window_init(&win, "R", 40, 40, 300, 90);
      fld = unoui_add_edit(&win, 8, 8, 200, &pt2);
      unoui_ui_add(&ui, &win);
      CHECK("reject: refused with no animator installed",
            unoui_reject_widget(&ui, &win, fld) == 0);
      unoui_wmanim_install(&ui, &ac);
      unoui_anim_tick(&ac, 1000);
      CHECK("reject: accepted once the animator is in",
            unoui_reject_widget(&ui, &win, fld) == 1);
      CHECK("reject: the text is selected, so typing replaces it",
            pt2.sel == 0 && pt2.caret == pt2.len);
      unoui_anim_tick(&ac, 1000 + 80);
      CHECK("reject: the widget has actually moved", fld->dx != 0);
      unoui_anim_tick(&ac, 1000 + 400);        /* past the end */
      CHECK("reject: and comes back to exactly where it was", fld->dx == 0);

      /* a window shake must land the window back on its own x */
      { int x0 = win.r.x;
        /* the tween's clock starts at the context's CURRENT time, which the
         * ticks above have already advanced to 1400 - sample relative to that */
        CHECK("reject: a window shake starts", unoui_reject_window(&ui, &win) == 1);
        unoui_anim_tick(&ac, 1400 + 80);
        CHECK("reject: the window moved", win.r.x != x0);
        unoui_anim_tick(&ac, 1400 + 400);
        CHECK("reject: the window came home", win.r.x == x0); } }

    /* ---- 9. scrolling window content ------------------------------------ *
     * The promise is that scrolling moves the CONTENT and nothing else: a
     * widget's rect, the hit test and the painter all come from
     * unoui_content_origin, so if they can disagree here they disagree on
     * screen too. */
    fb_set_font(0);
    { static unoui_ui ui; static unoui_window win;
      const unoui_theme *th = &theme_unodos;
      unoui_widget *top, *bot;
      unoui_rect r0, r1, bar;
      int viewh;
      unoui_ui_init(&ui, th, 640, 480);
      unoui_window_init(&win, "S", 20, 20, 260, 160);
      top = unoui_add_button(&win, 8, 0,   90, "top", 0);
      bot = unoui_add_button(&win, 8, 400, 90, "bottom", 0);
      unoui_ui_add(&ui, &win);

      CHECK("scroll: an ordinary window has no range",
            unoui_win_scroll_max(th, &win) == 0 &&
            unoui_win_bar(th, &win).w == 0);

      win.flags |= UI_WIN_VSCROLL;
      win.content_h = 460;
      viewh = win.r.h - th->m.title_h - th->m.pad - th->m.frame_w;
      CHECK("scroll: the range is content minus the view",
            unoui_win_scroll_max(th, &win) == 460 - viewh);
      bar = unoui_win_bar(th, &win);
      CHECK("scroll: the bar takes a strip off the content's right edge",
            bar.w == UI_WIN_BAR_W &&
            bar.x + bar.w == win.r.x + win.r.w - th->m.frame_w - th->m.pad);

      r0 = unoui_widget_rect(th, &win, top);
      unoui_win_scroll_to(th, &win, 50);
      r1 = unoui_widget_rect(th, &win, top);
      CHECK("scroll: scrolling moves the widgets up", r1.y == r0.y - 50);
      CHECK("scroll: and only vertically", r1.x == r0.x);

      /* The hit test has to move with them, or a click lands on whatever used
       * to be at those coordinates. Scrolled to the bottom, the widget that was
       * 400 px down is the one on screen - so clicking there must focus IT and
       * not the one that used to be there. */
      unoui_win_scroll_to(th, &win, unoui_win_scroll_max(th, &win));
      { unoui_rect rb = unoui_widget_rect(th, &win, bot);
        unoui_event e; memset(&e, 0, sizeof e);
        e.kind = UI_EV_MOUSE_DOWN;
        e.x = rb.x + 10; e.y = rb.y + rb.h / 2;
        unoui_handle(&ui, &e);
        CHECK("scroll: a click lands on the widget where it now IS",
              ui.focus_wi == 1); }
      unoui_win_scroll_to(th, &win, 50);

      unoui_win_scroll_to(th, &win, 99999);
      CHECK("scroll: it cannot be scrolled past the end",
            win.scroll_y == unoui_win_scroll_max(th, &win));
      unoui_win_scroll_to(th, &win, -50);
      CHECK("scroll: nor above the top", win.scroll_y == 0);

      /* the wheel anywhere over the window scrolls it */
      { unoui_event e; memset(&e, 0, sizeof e);
        e.kind = UI_EV_MOUSE_MOVE; e.x = win.r.x + 40; e.y = win.r.y + 60;
        unoui_handle(&ui, &e);
        memset(&e, 0, sizeof e); e.kind = UI_EV_WHEEL; e.wheel = 1;
        e.x = win.r.x + 40; e.y = win.r.y + 60;
        unoui_handle(&ui, &e);
        CHECK("scroll: the wheel scrolls the window", win.scroll_y > 0); }

      /* a FILL widget stops before the bar, and fills the CONTENT height */
      { unoui_widget *f = unoui_add_button(&win, 0, 0, 10, "f", 0);
        unoui_widget_fill(f);
        unoui_reflow_window(th, &win);
        CHECK("scroll: a fill widget stops before the scrollbar",
              f->r.w == win.r.w - 2 * (th->m.frame_w + th->m.pad) - UI_WIN_BAR_W);
        CHECK("scroll: and fills the content, not the frame",
              f->r.h == win.content_h); }
      (void)bot; }

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
