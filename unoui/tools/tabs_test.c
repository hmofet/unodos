/* ===========================================================================
 * unoui tabbed-document contract test (host).
 *
 * UI_TABS is two controls in one. With no UI_TF_* flag it is the plain strip
 * it has always been - the Control Panel's page picker - and this test pins
 * that down first, because the whole design rests on a zero-flag model being
 * indistinguishable from the old one. With flags it grows close boxes, a "+",
 * equal widths and a ">>" overflow control.
 *
 * The assertion that matters most is the SWEEP: every x across the strip that
 * reports a hit must land inside the rect the painter would have drawn for
 * that tab. The old code re-derived tab widths independently in the painter
 * (unoui.c) and the hit test (unoui_input.c), so the two could disagree and
 * nothing would notice; unoui_tab_rect() is now the single source and this
 * sweep is what keeps it that way.
 *
 *   sh tools/tabs_test.sh
 * ======================================================================== */
#include "unoui_theme.h"
#include <stdio.h>
#include <string.h>

static unoui_ui  UI;
static unoui_window W;
static int fails;

#define CHECK(name, cond) do {                                              \
    if (cond) printf("  ok   %s\n", name);                                  \
    else { printf("  FAIL %s\n", name); fails++; }                          \
} while (0)

static const char *const g_lab[8] = {
    "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"
};

static unoui_action ev_click(int x, int y)
{ unoui_event e; unoui_action a; memset(&e,0,sizeof e);
  e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y; a = unoui_handle(&UI,&e);
  e.kind=UI_EV_MOUSE_UP;   unoui_handle(&UI,&e); return a; }
static unoui_action ev_key(int k)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_KEY; e.key=k; return unoui_handle(&UI,&e); }

static int in_rect(unoui_rect r, int x, int y)
{ return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

/* every pixel of the strip that claims a tab must be inside that tab's rect,
 * and every visible tab must be reachable */
static int sweep_agrees(const unoui_theme *t, unoui_rect r,
                        const unoui_tabs_model *m, int *reached)
{
    int x, ok = 1, i, seen[8];
    int cy = r.y + r.h / 2;
    for (i = 0; i < 8; i++) seen[i] = 0;
    for (x = r.x; x < r.x + r.w; x++) {
        int k = -1, part = unoui_tabs_hit(t, r, m, x, cy, &k);
        if (part != UI_TAB_SEL && part != UI_TAB_CLOSE) continue;
        if (k < 0 || k >= m->n) { ok = 0; continue; }
        if (!in_rect(unoui_tab_rect(t, r, m, k), x, cy)) ok = 0;
        seen[k] = 1;
    }
    *reached = 0;
    for (i = 0; i < m->n; i++) if (seen[i]) (*reached)++;
    return ok;
}

int main(void)
{
    const unoui_theme *T = &theme_unodos;
    unoui_widget *tw;
    unoui_tabs_model m;
    unoui_rect r, q, c, p, o;
    int i, reached, nvis;

    unoui_ui_init(&UI, T, FB_W, FB_H);
    unoui_window_init(&W, "tabs", 20, 20, 300, 140);
    tw = unoui_add_tabs(&W, 8, 8, 260, (const char **)g_lab, 4, 0);
    tw->id = 42;
    unoui_ui_add(&UI, &W);
    UI.focus_win = 0; UI.focus_wi = 0;
    r = unoui_widget_rect(UI.theme, &W, tw);

    printf("plain strip (no flags): unchanged behaviour\n");
    unoui_tabs_model_of(tw, &m);
    CHECK("model reads flags off the widget", m.flags == 0);
    CHECK("strip height is the tab height", r.h == unoui_tabs_h(T));
    CHECK("no close box", unoui_tab_close_rect(T, r, &m, 0).w == 0);
    CHECK("no plus",      unoui_tabs_plus_rect(T, r, &m).w == 0);
    CHECK("no overflow",  unoui_tabs_over_rect(T, r, &m).w == 0);
    CHECK("never scrolls", unoui_tabs_maxfirst(T, r, &m) == 0);
    for (i = 0; i < 4; i++) {
        q = unoui_tab_rect(T, r, &m, i);
        if (q.w != fb_text_w(g_lab[i]) + 16) break;
    }
    CHECK("tab width is text + 16, as it always was", i == 4);
    CHECK("tabs abut with no gap",
          unoui_tab_rect(T, r, &m, 1).x ==
          unoui_tab_rect(T, r, &m, 0).x + unoui_tab_rect(T, r, &m, 0).w);
    CHECK("sweep: draw and hit agree", sweep_agrees(T, r, &m, &reached));
    CHECK("sweep reached every tab", reached == 4);
    { unoui_action a = ev_click(unoui_tab_rect(T, r, &m, 2).x + 3, r.y + r.h / 2);
      CHECK("click selects the tab under it", a.changed && a.value == 2 &&
            a.kind == UI_TABS && tw->sel == 2); }

    printf("document tabs: close + plus + elastic + overflow\n");
    tw->nitems = 8;
    tw->flags |= UI_TF_CLOSE | UI_TF_PLUS | UI_TF_ELASTIC | UI_TF_OVERFLOW;
    tw->sel = 0; tw->value = 0;
    unoui_tabs_model_of(tw, &m);
    nvis = unoui_tabs_visible(T, r, &m);
    p = unoui_tabs_plus_rect(T, r, &m);
    o = unoui_tabs_over_rect(T, r, &m);
    printf("  strip %dx%d, %d of %d tabs visible, maxfirst %d\n",
           r.w, r.h, nvis, m.n, unoui_tabs_maxfirst(T, r, &m));
    CHECK("eight tabs do not fit", nvis < 8 && nvis > 0);
    CHECK("the plus control appears", p.w > 0);
    CHECK("the overflow control appears", o.w > 0);
    CHECK("overflow sits at the right edge", o.x + o.w == r.x + r.w);
    CHECK("plus and overflow do not overlap", p.x + p.w <= o.x);
    CHECK("scrolling is possible", unoui_tabs_maxfirst(T, r, &m) > 0);

    for (i = 1; i < nvis; i++) {
        unoui_rect a1 = unoui_tab_rect(T, r, &m, i - 1);
        q = unoui_tab_rect(T, r, &m, i);
        if (q.w != a1.w || q.x != a1.x + a1.w) break;
    }
    CHECK("elastic tabs are equal width and abut", i == nvis);
    CHECK("elastic width respects the floor",
          unoui_tab_rect(T, r, &m, 0).w >= UI_TAB_MIN_W);
    CHECK("visible tabs stay inside the strip",
          unoui_tab_rect(T, r, &m, nvis - 1).x +
          unoui_tab_rect(T, r, &m, nvis - 1).w <= p.x);
    CHECK("a tab scrolled out has no rect", unoui_tab_rect(T, r, &m, 7).w == 0);

    c = unoui_tab_close_rect(T, r, &m, 0);
    q = unoui_tab_rect(T, r, &m, 0);
    CHECK("close box exists", c.w > 0);
    CHECK("close box is inside its tab",
          c.x >= q.x && c.x + c.w <= q.x + q.w &&
          c.y >= q.y && c.y + c.h <= q.y + q.h);
    CHECK("sweep: draw and hit agree with flags on", sweep_agrees(T, r, &m, &reached));
    CHECK("sweep reached every visible tab", reached == nvis);
    { int k = -1;
      CHECK("the close box reports CLOSE, not SEL",
            unoui_tabs_hit(T, r, &m, c.x + c.w/2, c.y + c.h/2, &k) == UI_TAB_CLOSE
            && k == 0);
      CHECK("the plus reports PLUS",
            unoui_tabs_hit(T, r, &m, p.x + p.w/2, p.y + p.h/2, &k) == UI_TAB_PLUS);
      CHECK("the overflow reports OVER",
            unoui_tabs_hit(T, r, &m, o.x + o.w/2, o.y + o.h/2, &k) == UI_TAB_OVER);
      CHECK("outside the strip is nothing",
            unoui_tabs_hit(T, r, &m, r.x - 1, r.y + 1, &k) == UI_TAB_NONE); }

    printf("the overflow control is stable under scrolling\n");
    { unoui_tabs_model s = m;
      int stable = 1, f;
      for (f = 0; f <= unoui_tabs_maxfirst(T, r, &m); f++) {
          s.first = f;
          if (unoui_tabs_over_rect(T, r, &s).w != o.w) stable = 0;
          if (unoui_tabs_plus_rect(T, r, &s).w != p.w) stable = 0;
      }
      CHECK("it does not blink in and out as you scroll", stable); }

    printf("reveal\n");
    CHECK("reveal clamps a wild first", unoui_tabs_reveal(T, r, &m, -1) == 0);
    { int f = unoui_tabs_reveal(T, r, &m, 7);
      unoui_tabs_model s = m; s.first = f;
      CHECK("revealing the last tab brings it into view",
            f > 0 && unoui_tab_rect(T, r, &s, 7).w > 0);
      CHECK("reveal never exceeds maxfirst", f <= unoui_tabs_maxfirst(T, r, &m)); }
    { unoui_tabs_model s = m; s.first = 3;
      CHECK("revealing a tab already visible does not move the strip",
            unoui_tabs_reveal(T, r, &s, 3) == 3); }

    printf("the widget path: clicks and keys\n");
    { unoui_action a = ev_click(p.x + p.w / 2, p.y + p.h / 2);
      CHECK("clicking plus raises UI_ACT_TABNEW",
            a.changed && a.kind == UI_ACT_TABNEW && a.value == 8); }
    { unoui_action a = ev_click(c.x + c.w / 2, c.y + c.h / 2);
      CHECK("clicking a close box raises UI_ACT_TABCLOSE",
            a.changed && a.kind == UI_ACT_TABCLOSE && a.value == 0); }
    { unoui_action a;
      tw->sel = 0; tw->value = 0;
      a = ev_click(unoui_tab_rect(T, r, &m, 1).x + 2, r.y + r.h / 2);
      CHECK("clicking a tab still selects it",
            a.changed && a.kind == UI_TABS && tw->sel == 1); }
    { int before = tw->value;
      ev_click(o.x + o.w / 2, o.y + o.h / 2);
      CHECK("clicking overflow scrolls the strip", tw->value == before + 1); }
    { tw->sel = 0; tw->value = 0;
      UI.focus_wi = 0;
      for (i = 0; i < 7; i++) ev_key(UI_KEY_RIGHT);
      CHECK("arrows walk to the last tab", tw->sel == 7);
      CHECK("and scroll it into view", tw->value > 0);
      { unoui_tabs_model s; unoui_tabs_model_of(tw, &s);
        CHECK("the selected tab is really visible",
              unoui_tab_rect(T, r, &s, 7).w > 0); } }

    printf("a tab too narrow for a close box does not get one\n");
    { unoui_tabs_model s;
      unoui_rect tiny = r;
      tiny.w = 60;                       /* one elastic tab, clipped short */
      unoui_tabs_model_of(tw, &s);
      s.first = 0;
      for (i = 0; i < s.n; i++) {
          unoui_rect tr2 = unoui_tab_rect(T, tiny, &s, i);
          unoui_rect cr2 = unoui_tab_close_rect(T, tiny, &s, i);
          if (cr2.w && (cr2.x < tr2.x || cr2.x + cr2.w > tr2.x + tr2.w)) break;
      }
      CHECK("no close box ever escapes its tab", i == s.n); }

    printf("\ntabs_test: %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
