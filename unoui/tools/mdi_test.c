/* ===========================================================================
 * unoui MDI contract test (host).
 *
 * A UI_MDI widget hosts child frames inside its own rect. They are NOT windows
 * - they never enter ui->win[] - so everything about them (z-order, focus,
 * containment, geometry) is local to the app's unoui_mdi, and this test pins
 * that down where it is cheap: on the host, in seconds, with no QEMU.
 *
 * Two properties matter more than the rest and are asserted hardest:
 *
 *   CONTAINMENT. A child can never leave the container, however it is moved,
 *   resized or laid out. That is the entire promise of a container widget - if
 *   it leaks, a child paints and takes clicks over whatever is beside it.
 *
 *   ZERO MEANS EMPTY. z[] and focus hold index + 1, so a zero-initialised
 *   unoui_mdi reads as "no children" AND child 0 is still representable. A bare
 *   index terminated by -1 fails the second half silently, which is exactly the
 *   trap WM phase E paid for with a mid-gate reboot.
 *
 *   sh tools/mdi_test.sh
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

static unoui_mdi_child g_kids[6];
static unoui_mdi M;

static void ev_move(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_MOVE; e.x=x; e.y=y; unoui_handle(&UI,&e); }
static void ev_down(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y; unoui_handle(&UI,&e); }
static void ev_up(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_UP; e.x=x; e.y=y; unoui_handle(&UI,&e); }
static unoui_action ev_click(int x, int y)
{ unoui_event e; unoui_action a; memset(&e,0,sizeof e);
  e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y; a = unoui_handle(&UI,&e);
  e.kind=UI_EV_MOUSE_UP;   unoui_handle(&UI,&e); return a; }

static int inside(unoui_rect outer, unoui_rect q)
{
    return q.w > 0 && q.h > 0 &&
           q.x >= outer.x && q.y >= outer.y &&
           q.x + q.w <= outer.x + outer.w && q.y + q.h <= outer.y + outer.h;
}
static int overlaps(unoui_rect a, unoui_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

/* every live child is inside the container */
static int all_contained(unoui_rect r)
{
    int k, n = unoui_mdi_count(&M);
    for (k = 0; k < n; k++)
        if (!inside(r, unoui_mdi_child_rect(r, &M, unoui_mdi_zorder(&M, k))))
            return 0;
    return 1;
}

int main(void)
{
    const unoui_theme *T = &theme_unodos;
    unoui_widget *mw;
    unoui_rect r, a, b, c;
    int i, j, k, n, th, fw;

    printf("a zero-initialised unoui_mdi reads as empty\n");
    {   static unoui_mdi Z;
        CHECK("no children",        unoui_mdi_count(&Z) == 0);
        CHECK("nothing focused",    unoui_mdi_focused(&Z) == -1);
        CHECK("no z entry",         unoui_mdi_zorder(&Z, 0) == -1);
        { unoui_rect any = { 0, 0, 100, 100 };
          CHECK("child 0 has no rect", unoui_mdi_child_rect(any, &Z, 0).w == 0);
          CHECK("nothing under a point", unoui_mdi_at(any, &Z, 10, 10) == -1); } }

    unoui_ui_init(&UI, T, FB_W, FB_H);
    unoui_window_init(&W, "mdi", 20, 20, 400, 300);
    M.ch = g_kids; M.cap = 6;
    mw = unoui_add_mdi(&W, 8, 8, 340, 240, &M);
    mw->id = 55;
    unoui_ui_add(&UI, &W);
    UI.focus_win = 0; UI.focus_wi = 0;
    r = unoui_widget_rect(UI.theme, &W, mw);
    th = T->m.title_h; fw = T->m.frame_w;
    printf("container %dx%d at (%d,%d), title_h %d, closebox %d\n",
           r.w, r.h, r.x, r.y, th, T->m.closebox);

    printf("add, z-order, focus - and child 0 is representable\n");
    i = unoui_mdi_add(&M, "one", 10, 10, 140, 100, UI_MDI_RESIZE, 0);
    CHECK("the first child is index 0", i == 0);
    CHECK("count sees it",              unoui_mdi_count(&M) == 1);
    CHECK("index+1 keeps child 0 visible in the z-list",
          unoui_mdi_zorder(&M, 0) == 0);
    CHECK("adding focuses it",          unoui_mdi_focused(&M) == 0);
    j = unoui_mdi_add(&M, "two", 60, 40, 140, 100, UI_MDI_RESIZE, 0);
    k = unoui_mdi_add(&M, "three", 110, 70, 140, 100, 0, 0);
    CHECK("three children", unoui_mdi_count(&M) == 3 && j == 1 && k == 2);
    CHECK("z is back-to-front in insertion order",
          unoui_mdi_zorder(&M, 0) == 0 && unoui_mdi_zorder(&M, 1) == 1 &&
          unoui_mdi_zorder(&M, 2) == 2);
    CHECK("the newest is focused", unoui_mdi_focused(&M) == 2);

    printf("geometry is relative to the container\n");
    a = unoui_mdi_child_rect(r, &M, 0);
    CHECK("child rect is container origin + child offset",
          a.x == r.x + 10 && a.y == r.y + 10 && a.w == 140 && a.h == 100);
    {   unoui_rect moved = r;
        moved.x += 50; moved.y += 30;
        b = unoui_mdi_child_rect(moved, &M, 0);
        CHECK("moving the container carries its children",
              b.x == a.x + 50 && b.y == a.y + 30 && b.w == a.w && b.h == a.h); }
    c = unoui_mdi_content_rect(T, r, &M, 0);
    CHECK("content sits below the title bar and inside the frame",
          c.y == a.y + th && c.x == a.x + fw && inside(a, c));

    printf("hit-testing is front to back\n");
    /* all three overlap around (r.x+120, r.y+80) by construction */
    CHECK("the three overlap where we are about to probe",
          overlaps(unoui_mdi_child_rect(r, &M, 0), unoui_mdi_child_rect(r, &M, 2)));
    CHECK("the frontmost child wins",
          unoui_mdi_at(r, &M, r.x + 120, r.y + 80) == 2);
    unoui_mdi_raise(&M, 0);
    CHECK("raising child 0 puts it in front",
          unoui_mdi_zorder(&M, 2) == 0 && unoui_mdi_focused(&M) == 0);
    CHECK("and it now wins the same point",
          unoui_mdi_at(r, &M, r.x + 120, r.y + 80) == 0);
    CHECK("a point outside every child is nothing",
          unoui_mdi_at(r, &M, r.x + r.w - 2, r.y + r.h - 2) == -1);

    printf("containment: a child can never leave the box\n");
    M.ch[0].r.x = -400; M.ch[0].r.y = -400;
    unoui_mdi_clamp(r, &M, 0);
    CHECK("dragged far up-left, it comes back", all_contained(r));
    M.ch[0].r.x = 9999; M.ch[0].r.y = 9999;
    unoui_mdi_clamp(r, &M, 0);
    CHECK("dragged far down-right, it comes back", all_contained(r));
    M.ch[0].r.w = 5; M.ch[0].r.h = 5;
    unoui_mdi_clamp(r, &M, 0);
    CHECK("shrunk below the floor, it grows back",
          M.ch[0].r.w >= UI_MDI_MIN_W && M.ch[0].r.h >= UI_MDI_MIN_H);
    M.ch[0].r.w = 9999; M.ch[0].r.h = 9999;
    unoui_mdi_clamp(r, &M, 0);
    CHECK("grown past the container, it is cut to fit", all_contained(r));

    printf("tile\n");
    unoui_mdi_tile(T, r, &M);
    CHECK("every tile is inside the container", all_contained(r));
    {   int bad = 0;
        n = unoui_mdi_count(&M);
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++)
                if (overlaps(unoui_mdi_child_rect(r, &M, unoui_mdi_zorder(&M, i)),
                             unoui_mdi_child_rect(r, &M, unoui_mdi_zorder(&M, j))))
                    bad = 1;
        CHECK("tiles do not overlap each other", !bad); }
    {   int area = 0;
        n = unoui_mdi_count(&M);
        for (i = 0; i < n; i++) {
            unoui_rect q = unoui_mdi_child_rect(r, &M, unoui_mdi_zorder(&M, i));
            area += q.w * q.h;
        }
        /* a 3-child tile is a 2x2 grid with one cell empty: 3 of 4 quarters */
        CHECK("tiles cover three quarters of the box, seamlessly",
              area == (r.w / 2) * (r.h / 2) * 3 ||
              area >= (r.w * r.h * 7) / 10); }
    {   int centres_ok = 1;
        n = unoui_mdi_count(&M);
        for (i = 0; i < n; i++) {
            int ci = unoui_mdi_zorder(&M, i);
            unoui_rect q = unoui_mdi_child_rect(r, &M, ci);
            if (unoui_mdi_at(r, &M, q.x + q.w / 2, q.y + q.h / 2) != ci)
                centres_ok = 0;
        }
        CHECK("after tiling, each child's centre hits that child", centres_ok); }

    printf("cascade\n");
    unoui_mdi_cascade(T, r, &M);
    CHECK("every cascaded child is inside the container", all_contained(r));
    {   int stepped = 1;
        n = unoui_mdi_count(&M);
        for (i = 1; i < n; i++) {
            unoui_rect p = unoui_mdi_child_rect(r, &M, unoui_mdi_zorder(&M, i - 1));
            unoui_rect q = unoui_mdi_child_rect(r, &M, unoui_mdi_zorder(&M, i));
            if (q.x <= p.x || q.y <= p.y) stepped = 0;
        }
        CHECK("each one steps down and right of the last", stepped); }
    {   /* a deep stack must still fit: the step shrinks rather than the tail
         * piling up in the corner */
        int dense = 1, before = unoui_mdi_count(&M);
        while (unoui_mdi_count(&M) < 6)
            unoui_mdi_add(&M, "more", 0, 0, 120, 90, 0, 0);
        unoui_mdi_cascade(T, r, &M);
        if (!all_contained(r)) dense = 0;
        for (i = 1; i < unoui_mdi_count(&M); i++) {
            unoui_rect p = unoui_mdi_child_rect(r, &M, unoui_mdi_zorder(&M, i - 1));
            unoui_rect q = unoui_mdi_child_rect(r, &M, unoui_mdi_zorder(&M, i));
            if (q.x == p.x && q.y == p.y) dense = 0;
        }
        CHECK("six children still cascade inside the box", dense);
        while (unoui_mdi_count(&M) > before)
            unoui_mdi_close(&M, unoui_mdi_zorder(&M, unoui_mdi_count(&M) - 1)); }

    printf("close\n");
    unoui_mdi_tile(T, r, &M);
    unoui_mdi_raise(&M, 1);
    unoui_mdi_close(&M, 1);
    CHECK("the child is gone",   unoui_mdi_count(&M) == 2);
    CHECK("its slot is free",    !M.ch[1].used);
    CHECK("focus falls to the new front",
          unoui_mdi_focused(&M) == unoui_mdi_zorder(&M, 1));
    CHECK("it has no rect any more", unoui_mdi_child_rect(r, &M, 1).w == 0);
    CHECK("the z-list closed the gap",
          unoui_mdi_zorder(&M, 2) == -1 && unoui_mdi_zorder(&M, 1) >= 0);
    {   int re = unoui_mdi_add(&M, "reused", 4, 4, 120, 90, 0, 0);
        CHECK("a freed slot is reused", re == 1 && unoui_mdi_count(&M) == 3); }

    printf("the widget path: raise, drag, resize, close\n");
    unoui_mdi_tile(T, r, &M);
    {   int front = unoui_mdi_zorder(&M, 2), back = unoui_mdi_zorder(&M, 0);
        unoui_rect q = unoui_mdi_child_rect(r, &M, back);
        unoui_action act = ev_click(q.x + q.w / 2, q.y + th + 4);
        CHECK("clicking a child's body focuses it",
              unoui_mdi_focused(&M) == back && act.changed && act.value == back);
        CHECK("and raises it above the one that was in front",
              unoui_mdi_zorder(&M, 2) == back && front != back); }
    {   int ci = unoui_mdi_focused(&M);
        unoui_rect q0 = unoui_mdi_child_rect(r, &M, ci), q1;
        int gx = q0.x + q0.w / 2, gy = q0.y + 3;      /* on the title bar */
        ev_down(gx, gy);
        ev_move(gx + 25, gy + 18);
        ev_up(gx + 25, gy + 18);
        q1 = unoui_mdi_child_rect(r, &M, ci);
        CHECK("a title-bar drag moves the child",
              q1.x == q0.x + 25 && q1.y == q0.y + 18);
        CHECK("and it is still inside", all_contained(r)); }
    {   int ci = unoui_mdi_focused(&M);
        unoui_rect q0 = unoui_mdi_child_rect(r, &M, ci);
        int gx = q0.x + q0.w / 2, gy = q0.y + 3;
        ev_down(gx, gy);
        ev_move(r.x + r.w + 500, r.y + r.h + 500);   /* yank it off the edge */
        ev_up(r.x + r.w + 500, r.y + r.h + 500);
        CHECK("a drag past the edge is clamped, not lost", all_contained(r)); }
    {   int ci = unoui_mdi_add(&M, "sizer", 20, 20, 160, 120, UI_MDI_RESIZE, 0);
        unoui_rect q0;
        unoui_mdi_raise(&M, ci);
        q0 = unoui_mdi_child_rect(r, &M, ci);
        ev_down(q0.x + q0.w - 2, q0.y + q0.h - 2);   /* the corner grip */
        ev_move(q0.x + q0.w - 40, q0.y + q0.h - 30);
        ev_up(q0.x + q0.w - 40, q0.y + q0.h - 30);
        CHECK("the corner grip resizes the child",
              M.ch[ci].r.w == q0.w - 38 && M.ch[ci].r.h == q0.h - 28);
        ev_down(unoui_mdi_child_rect(r, &M, ci).x + 2,
                unoui_mdi_child_rect(r, &M, ci).y + unoui_mdi_child_rect(r, &M, ci).h - 2);
        ev_move(r.x - 900, r.y - 900);               /* shrink to nothing */
        ev_up(r.x - 900, r.y - 900);
        CHECK("a resize cannot go below the floor",
              M.ch[ci].r.w >= UI_MDI_MIN_W && M.ch[ci].r.h >= UI_MDI_MIN_H);
        unoui_mdi_close(&M, ci); }
    if (T->m.closebox > 0) {
        int ci = unoui_mdi_focused(&M);
        unoui_rect q = unoui_mdi_child_rect(r, &M, ci);
        int cs = T->m.closebox;
        unoui_action act = ev_click(q.x + fw + 4 + cs / 2,
                                    q.y + fw + (th - fw - cs) / 2 + cs / 2);
        CHECK("the close box raises UI_ACT_MDICLOSE with the child index",
              act.changed && act.kind == UI_ACT_MDICLOSE && act.value == ci);
        CHECK("unoui does NOT remove it - that is the app's call",
              M.ch[ci].used);
    } else printf("  --   theme has no close box, skipped\n");

    printf("\nmdi_test: %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
