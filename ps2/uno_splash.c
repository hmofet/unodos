/* ===========================================================================
 * UnoDOS/PS2 milestone-0 splash - the "hello-GS" screen (HANDOFF SS4).
 *
 * Renders entirely through the software framebuffer primitives (fb.h): the
 * UnoDOS-blue desktop, a menu bar, a centred title panel, the four-colour
 * palette swatches that prove the PORT-SPEC gamut, and an arrow cursor at the
 * pad position. Shared verbatim by the host shim (renders once -> PNG) and the
 * EE target (renders each vsync at the live pad cursor -> GS upload). M1
 * replaces this with the ported unodos.c desktop on the same primitives.
 * ===========================================================================
 */
#include "fb.h"

/* classic up-left arrow: 'B' = black edge, 'W' = white fill, ' ' = clear */
static const char *kCursor[] = {
    "B",
    "BB",
    "BWB",
    "BWWB",
    "BWWWB",
    "BWWWWB",
    "BWWWWWB",
    "BWWWWWWB",
    "BWWWWBBBB",
    "BWWBWB",
    "BWB BWB",
    "BB  BWB",
    "B    BWB",
    "      BWB",
    "       BB",
    0
};

void uno_draw_cursor(int cx, int cy)
{
    int r, c;
    for (r = 0; kCursor[r]; r++) {
        const char *row = kCursor[r];
        for (c = 0; row[c]; c++) {
            if (row[c] == 'B')      fb_fill_rect(cx + c, cy + r, 1, 1, UNO_BLACK);
            else if (row[c] == 'W') fb_fill_rect(cx + c, cy + r, 1, 1, UNO_WHITE);
        }
    }
}

static void center_text(int cy, const char *s, fb_px fg, long bg)
{
    fb_text((FB_W - fb_text_w(s)) / 2, cy, s, fg, bg);
}

void uno_render_splash(int cx, int cy)
{
    static const fb_px sw[4] = { UNO_BLUE, UNO_CYAN, UNO_MAG, UNO_WHITE };
    static const char *swn[4] = { "blue", "cyan", "mag", "white" };
    const char *title = "UnoDOS";
    int tw, px, py, pw, ph, i, x;

    fb_clear(UNO_BLUE);

    /* menu bar */
    fb_fill_rect(0, 0, FB_W, 20, UNO_WHITE);
    fb_text(8, 6, "UnoDOS", UNO_BLACK, -1);
    fb_text(FB_W - 8 - fb_text_w("PlayStation 2"), 6, "PlayStation 2", UNO_BLACK, -1);

    /* centred title panel */
    pw = 420; ph = 200;
    px = (FB_W - pw) / 2; py = (FB_H - ph) / 2;
    fb_fill_rect(px, py, pw, ph, UNO_CYAN);
    fb_frame_rect(px, py, pw, ph, UNO_WHITE);
    fb_frame_rect(px + 2, py + 2, pw - 4, ph - 4, UNO_BLACK);

    /* big title, centred */
    tw = (int)(6 * 8 * 4);                  /* "UnoDOS" * scale 4 */
    fb_big_text((FB_W - tw) / 2, py + 28, title, UNO_BLACK, -1, 4);

    center_text(py + 90,  "PlayStation 2 port", UNO_BLACK, -1);
    center_text(py + 108, "Milestone 0  -  hello-GS", UNO_BLACK, -1);
    center_text(py + 134, "640x448 software framebuffer  ->  GS", UNO_BLUE, -1);

    /* palette swatches (proves the PORT-SPEC SS1 gamut) */
    x = px + 40;
    for (i = 0; i < 4; i++) {
        fb_fill_rect(x, py + ph - 38, 56, 22, sw[i]);
        fb_frame_rect(x, py + ph - 38, 56, 22, UNO_BLACK);
        fb_text(x + 4, py + ph - 12, swn[i], UNO_WHITE, -1);
        x += 80;
    }

    /* hint line + cursor */
    center_text(FB_H - 16, "D-pad / stick: move cursor   Start: Esc", UNO_WHITE, -1);
    uno_draw_cursor(cx, cy);
}
