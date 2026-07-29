/* ===========================================================================
 * unoui list contract test (host).
 *
 * UI_LIST scrolls: it shows a window of its items starting at the widget's
 * `value`, and the SAME geometry drives the painter, the hit test and the
 * input layer. This test pins that contract down where it is cheap to run -
 * on the host, with no framebuffer output to eyeball:
 *
 *   - geometry: rows in a box, the largest legal first row, y -> index
 *   - the wheel scrolls and clamps at both ends
 *   - a click selects the row UNDER the pointer after scrolling (the bug that
 *     made the WiFi network list unusable: y mapped straight to index 0..)
 *   - keyboard: arrows / PgUp / PgDn / Home / End keep the selection in view
 *   - the inline scrollbar's arrows and thumb scroll the list
 *
 *   cc -I. -I../ps2 unoui.c unoui_input.c themes/theme_unodos.c \
 *      ../ps2/fb.c ../ps2/fb_aa.c tools/list_test.c -o build/list_test
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

static const char *g_items[64];
static char g_item_buf[64][8];

static void ev_move(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_MOVE; e.x=x; e.y=y; unoui_handle(&UI,&e); }
static void ev_wheel(int x, int y, int n)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_WHEEL; e.x=x; e.y=y; e.wheel=n; unoui_handle(&UI,&e); }
static unoui_action ev_click(int x, int y)
{ unoui_event e; unoui_action a; memset(&e,0,sizeof e);
  e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y; a = unoui_handle(&UI,&e);
  e.kind=UI_EV_MOUSE_UP;   unoui_handle(&UI,&e); return a; }
static unoui_action ev_key(int k)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_KEY; e.key=k; return unoui_handle(&UI,&e); }

int main(void)
{
    unoui_widget *lw;
    unoui_rect r;
    int i, rows, n = 40;

    for (i = 0; i < 64; i++) {
        sprintf(g_item_buf[i], "row %d", i);
        g_items[i] = g_item_buf[i];
    }

    unoui_ui_init(&UI, &theme_unodos, FB_W, FB_H);
    unoui_window_init(&W, "list", 20, 20, 200, 200);
    lw = unoui_add_list(&W, 8, 8, 160, 70, g_items, n, 0);
    lw->id = 77;
    unoui_ui_add(&UI, &W);
    UI.focus_win = 0; UI.focus_wi = 0;

    r = unoui_widget_rect(UI.theme, &W, lw);
    rows = unoui_list_rows(r);

    printf("geometry (row pitch %d px, box %dx%d -> %d rows)\n",
           ui_row_h(), r.w, r.h, rows);
    CHECK("rows fit the box",        rows >= 1 && rows * ui_row_h() <= r.h);
    CHECK("maxtop = n - rows",       unoui_list_maxtop(r, n) == n - rows);
    CHECK("short list never scrolls", unoui_list_maxtop(r, rows - 1) == 0);
    CHECK("bar appears only when it overflows",
          unoui_list_bar(r, n).w == UI_LIST_BAR_W && unoui_list_bar(r, 2).w == 0);
    CHECK("first row hits index top", unoui_list_index_at(r, n, 5, r.y + 4) == 5);
    CHECK("second row hits top+1",
          unoui_list_index_at(r, n, 5, r.y + 4 + ui_row_h()) == 6);
    CHECK("index never leaves the list",
          unoui_list_index_at(r, n, n - 1, r.y + r.h - 1) == n - 1);
    CHECK("reveal pulls a row above the view down",
          unoui_list_reveal(r, n, 3, 20) == 3);
    CHECK("reveal pulls a row below the view up",
          unoui_list_reveal(r, n, 25, 0) == 25 - rows + 1);
    CHECK("reveal clamps to maxtop", unoui_list_reveal(r, n, -1, 999) == n - rows);

    printf("wheel\n");
    ev_move(r.x + 4, r.y + 4);
    CHECK("hovering the list", UI.hot_wi == 0);
    ev_wheel(r.x + 4, r.y + 4, 2);
    CHECK("wheel scrolls down", lw->value == 6);
    ev_wheel(r.x + 4, r.y + 4, -1);
    CHECK("wheel scrolls up",   lw->value == 3);
    ev_wheel(r.x + 4, r.y + 4, 99);
    CHECK("wheel clamps at the end", lw->value == n - rows);
    ev_wheel(r.x + 4, r.y + 4, -99);
    CHECK("wheel clamps at the top", lw->value == 0);

    printf("click selects the row under the pointer, scrolled or not\n");
    { unoui_action a = ev_click(r.x + 4, r.y + 4 + 2 * ui_row_h());
      CHECK("unscrolled: 3rd row -> index 2", a.changed && a.id == 77 && a.value == 2); }
    ev_wheel(r.x + 4, r.y + 4, 4);                     /* top = 12 */
    { unoui_action a = ev_click(r.x + 4, r.y + 4 + 2 * ui_row_h());
      CHECK("scrolled by 12: 3rd row -> index 14", a.changed && a.value == 14);
      CHECK("selection follows the click", lw->sel == 14); }

    printf("keyboard\n");
    lw->value = 0; lw->sel = 0;
    { unoui_action a = ev_key(UI_KEY_END);
      CHECK("End selects the last row", a.changed && a.value == n - 1);
      CHECK("End scrolls it into view",  lw->value == n - rows); }
    ev_key(UI_KEY_HOME);
    CHECK("Home returns to the top", lw->sel == 0 && lw->value == 0);
    for (i = 0; i < rows; i++) ev_key(UI_KEY_DOWN);
    CHECK("Down past the last visible row scrolls one row",
          lw->sel == rows && lw->value == 1);
    ev_key(UI_KEY_PGDN);
    CHECK("PgDn steps a screenful", lw->sel == 2 * rows);
    CHECK("PgDn keeps the selection visible",
          lw->sel >= lw->value && lw->sel < lw->value + rows);
    ev_key(UI_KEY_PGUP);
    CHECK("PgUp steps back", lw->sel == rows);
    ev_key(UI_KEY_UP);
    CHECK("Up moves one row", lw->sel == rows - 1);

    printf("inline scrollbar\n");
    lw->value = 5; lw->sel = 5;
    { unoui_rect bar = unoui_list_bar(r, n);
      ev_click(bar.x + 2, bar.y + 2);
      CHECK("up arrow scrolls one row",   lw->value == 4);
      ev_click(bar.x + 2, bar.y + bar.h - 2);
      CHECK("down arrow scrolls one row", lw->value == 5);
      ev_click(bar.x + 2, bar.y + bar.h - UI_LIST_BAR_W - 1);
      CHECK("thumb drag to the bottom",   lw->value == n - rows);
      CHECK("the scrollbar never changes the selection", lw->sel == 5); }

    printf(fails ? "\nlist_test: %d FAIL\n" : "\nlist_test: all checks passed\n", fails);
    return fails ? 1 : 0;
}
