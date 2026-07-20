# Writing a UnoDOS app

An app is a single UnoC source file that `#include "UNO.H"` and defines one
entry point. The desktop loads it, calls the entry, and drives it through a
small vtable. There is no `main`.

## The entry point

    #include "UNO.H"

    const AppInterface *uno_app_main(const KernelApi *k) {
        gK = k;              /* UNO.H's prelude uses this */
        return &kIface;
    }

`uno_app_main` receives the **KernelApi** — the kernel's table of callbacks
(drawing, text, sound, files) — stashes it in `gK` (the prelude macros expand
to `gK->...`), and returns your **AppInterface**.

## The AppInterface vtable

    static const AppInterface kIface = {
        my_draw,     /* void draw(UnoWin *w)                     */
        my_key,      /* Boolean key(char ch, short code, Boolean cmd) */
        0,           /* void click(UnoWin *w, Point p)  (0 = none)    */
        my_tick,     /* void tick(void)   — called ~60/sec            */
        0,           /* void opened(void)                             */
        0,           /* void closed(void)                             */
        "My App",    /* window title                                  */
        { 30, 30, 330, 260 }   /* win_rect: left, top, right, bottom  */
    };

The desktop owns the window frame, title bar, dragging and z-order. You draw
**inside** the window's `bounds` and react to the events routed to you.

- **`draw(w)`** — repaint your content. `w->bounds` is your window rect;
  content starts at `bounds.left`, `bounds.top + TBAR_H`.
- **`key(ch, code, cmd)`** — a key was pressed. `ch` is ASCII (arrows arrive
  as control codes; `code` carries a key code). Return `true` if you used it.
- **`tick()`** — a ~60 Hz heartbeat for animation. Call `find_app_window()` +
  `draw_window()` to request a redraw.
- **`opened()` / `closed()`** — set up / tear down.

## Drawing

Everything is drawn through the KernelApi (see [API.MD](API.MD)):

    Rect r;
    SetRect(&r, x, y, x + w, y + h);
    uno_fill(&r, C_BLUE);            /* one of C_BLUE/C_CYAN/C_MAG/C_WHITE */
    uno_box(&r, C_WHITE);            /* outline                            */
    text_at(x, y, "hello", C_WHITE, C_BLUE, false);
    fill_rgb(&r, &(GameRGB){240,200,40,C_WHITE});   /* true colour          */

Request a redraw of your own window from a tick or key handler:

    UnoWin *w = find_app_window(APP_RUNNER);   /* your slot */
    if (w) draw_window(w);

## Build and run

In Studio, **Ctrl-B** compiles the open `.C` file to a `.UNO` next to it and
**Ctrl-R** loads and runs it. A build error appears in the output pane with a
line number — click it to jump there.

See [SAMPLE.C](../SDK/SAMPLE.C) for a complete, commented app and
[DOSTRIS.C](../SDK/DOSTRIS.C) for a full game.
