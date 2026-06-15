/* Paint app module (APP_PAINT).  Separate artifact -> app09.so.
   MacPaint-style bitmap editor ported verbatim from the core over the KernelApi
   surface.  Canvas is a byte-per-pixel heap block (NewPtr); storage is the
   portable FAT volume (PAINT.UNO) through the KernelApi. */
#include "uno_mod.h"

#define PT_W      408
#define PT_H      240
#define PT_TOOLS  10
#define PT_TOOLW  26
#define PT_CELL   24

enum { T_PENCIL = 0, T_BRUSH, T_ERASER, T_LINE, T_RECT,
       T_FRECT, T_OVAL, T_FOVAL, T_FILL, T_SPRAY };

static unsigned char *gPtCanvas = NULL;     /* PT_W * PT_H bytes */
static short   gPtTool = T_PENCIL;
static Boolean gPtPicker = false;           /* color picker overlay up */

#if UNO_COLOR
static short    gPtColor = 0;               /* index into kPtPal */
static RGBColor kPtPal[256];
static Boolean  kPtPalInit = false;
#define PT_BG   255                         /* white (built last, below) */

static void pt_build_palette(void)
{
    short i, r, g, b;
    if (kPtPalInit) return;
    /* 0..215: 6x6x6 RGB cube */
    i = 0;
    for (r = 0; r < 6; r++) for (g = 0; g < 6; g++) for (b = 0; b < 6; b++) {
        kPtPal[i].red   = (unsigned short)(r * 13107);
        kPtPal[i].green = (unsigned short)(g * 13107);
        kPtPal[i].blue  = (unsigned short)(b * 13107);
        i++;
    }
    /* 216..231: 16 grays */
    for (g = 0; g < 16; g++) {
        kPtPal[i].red = kPtPal[i].green = kPtPal[i].blue =
            (unsigned short)(g * 4369);
        i++;
    }
    /* 232..254: hue ramps (pure + half-bright primaries/secondaries) */
    {
        static const unsigned char hues[23][3] = {
            {5,0,0},{5,2,0},{5,4,0},{4,5,0},{2,5,0},{0,5,0},{0,5,2},{0,5,4},
            {0,4,5},{0,2,5},{0,0,5},{2,0,5},{4,0,5},{5,0,4},{5,0,2},
            {3,1,0},{3,3,1},{1,3,1},{1,3,3},{1,1,3},{3,1,3},{3,2,1},{2,1,0}
        };
        for (g = 0; g < 23; g++) {
            kPtPal[i].red   = (unsigned short)(hues[g][0] * 13107);
            kPtPal[i].green = (unsigned short)(hues[g][1] * 13107);
            kPtPal[i].blue  = (unsigned short)(hues[g][2] * 13107);
            i++;
        }
    }
    /* 255: white (the canvas background) */
    kPtPal[255].red = kPtPal[255].green = kPtPal[255].blue = 0xFFFF;
    kPtPalInit = true;
}
#else
static short gPtPat = 1;                    /* pattern index (1 = black) */
#define PT_BG   0
/* the classic dither set: white, black, 25%, 50%, 75%, vert, horz,
   checker-2, diagonal, brick */
static const unsigned char kPtPats[10][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},   /* white  */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},   /* black  */
    {0x88,0x00,0x22,0x00,0x88,0x00,0x22,0x00},   /* 25%    */
    {0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55},   /* 50%    */
    {0x77,0xFF,0xDD,0xFF,0x77,0xFF,0xDD,0xFF},   /* 75%    */
    {0x88,0x88,0x88,0x88,0x88,0x88,0x88,0x88},   /* vert   */
    {0x00,0x00,0xFF,0x00,0x00,0x00,0xFF,0x00},   /* horz   */
    {0xCC,0xCC,0x33,0x33,0xCC,0xCC,0x33,0x33},   /* check2 */
    {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01},   /* diag   */
    {0xFF,0x80,0x80,0x80,0xFF,0x08,0x08,0x08},   /* brick  */
};
#endif

/* canvas geometry inside the window */
static void pt_canvas_rect(UnoWin *w, Rect *r)
{
    r->left   = (short)(w->bounds.left + PT_TOOLW + 34);
    r->top    = (short)(w->bounds.top + TBAR_H + 4);
    r->right  = (short)(r->left + PT_W);
    r->bottom = (short)(r->top + PT_H);
}

static unsigned char pt_ink(short x, short y)
{
#if UNO_COLOR
    (void)x; (void)y;
    return (unsigned char)gPtColor;
#else
    /* the current pattern decides each pixel (authentic dithering) */
    return (unsigned char)((kPtPats[gPtPat][y & 7] >> (7 - (x & 7))) & 1);
#endif
}

static void pt_show_px(UnoWin *w, short x, short y)
{
    Rect cr, px;
    if (!gPtCanvas) return;
    pt_canvas_rect(w, &cr);
    SetRect(&px, (short)(cr.left + x), (short)(cr.top + y),
                 (short)(cr.left + x + 1), (short)(cr.top + y + 1));
#if UNO_COLOR
    RGBForeColor(&kPtPal[gPtCanvas[(long)y * PT_W + x]]);
    PaintRect(&px);
    RGBForeColor(&kBlack);
#else
    if (gPtCanvas[(long)y * PT_W + x]) PaintRect(&px);
    else { PenMode(patBic); PaintRect(&px); PenNormal(); }
#endif
}

static void pt_set_px(UnoWin *w, short x, short y, unsigned char v)
{
    if (x < 0 || y < 0 || x >= PT_W || y >= PT_H || !gPtCanvas) return;
    gPtCanvas[(long)y * PT_W + x] = v;
    pt_show_px(w, x, y);
}

static void pt_dot(UnoWin *w, short x, short y, short size, Boolean erase)
{
    short dx, dy;
    for (dy = 0; dy < size; dy++)
        for (dx = 0; dx < size; dx++) {
            short px = (short)(x + dx - size / 2), py = (short)(y + dy - size / 2);
            pt_set_px(w, px, py, erase ? PT_BG : pt_ink(px, py));
        }
}

static void pt_line(UnoWin *w, short x0, short y0, short x1, short y1, short size)
{
    /* Bresenham */
    short dx = (short)(x1 > x0 ? x1 - x0 : x0 - x1);
    short dy = (short)(y1 > y0 ? y1 - y0 : y0 - y1);
    short sx = (short)(x0 < x1 ? 1 : -1), sy = (short)(y0 < y1 ? 1 : -1);
    short err = (short)(dx - dy);
    for (;;) {
        pt_dot(w, x0, y0, size, false);
        if (x0 == x1 && y0 == y1) break;
        { short e2 = (short)(err * 2);
          if (e2 > -dy) { err -= dy; x0 += sx; }
          if (e2 <  dx) { err += dx; y0 += sy; } }
    }
}

static void pt_rect_shape(UnoWin *w, short x0, short y0, short x1, short y1, Boolean filled)
{
    short t, x, y;
    if (x1 < x0) { t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; }
    if (filled) {
        for (y = y0; y <= y1; y++)
            for (x = x0; x <= x1; x++) pt_set_px(w, x, y, pt_ink(x, y));
    } else {
        for (x = x0; x <= x1; x++) { pt_set_px(w, x, y0, pt_ink(x, y0)); pt_set_px(w, x, y1, pt_ink(x, y1)); }
        for (y = y0; y <= y1; y++) { pt_set_px(w, x0, y, pt_ink(x0, y)); pt_set_px(w, x1, y, pt_ink(x1, y)); }
    }
}

static void pt_oval_shape(UnoWin *w, short x0, short y0, short x1, short y1, Boolean filled)
{
    /* midpoint-ish: scan rows of the bounding box, solve the ellipse */
    short t, y;
    long a, b, cx2, cy2;
    if (x1 < x0) { t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; }
    a = (x1 - x0) / 2; b = (y1 - y0) / 2;
    if (a == 0 || b == 0) { pt_rect_shape(w, x0, y0, x1, y1, filled); return; }
    cx2 = x0 + a; cy2 = y0 + b;
    for (y = y0; y <= y1; y++) {
        long dy = y - cy2;
        long r2 = (a * a) - (a * a * dy * dy) / (b * b);
        long half = 0, x;
        while ((half + 1) * (half + 1) <= r2) half++;
        if (filled) {
            for (x = cx2 - half; x <= cx2 + half; x++)
                pt_set_px(w, (short)x, y, pt_ink((short)x, y));
        } else {
            pt_set_px(w, (short)(cx2 - half), y, pt_ink((short)(cx2 - half), y));
            pt_set_px(w, (short)(cx2 + half), y, pt_ink((short)(cx2 + half), y));
        }
    }
}

#define PT_STK 1024
static void pt_flood(UnoWin *w, short x, short y)
{
    unsigned char from, to;
    short *stk; long n = 0;
    if (x < 0 || y < 0 || x >= PT_W || y >= PT_H || !gPtCanvas) return;
    from = gPtCanvas[(long)y * PT_W + x];
    to = pt_ink(x, y);
#if UNO_COLOR
    if (from == to) return;
#else
    if (from == (pt_ink(0,0) ? 1 : 0) && from == to) return;
#endif
    stk = (short *)NewPtr(PT_STK * 4L);
    if (!stk) return;
    stk[n * 2] = x; stk[n * 2 + 1] = y; n++;
    while (n > 0) {
        short px, py, lx, rx, i;
        n--; px = stk[n * 2]; py = stk[n * 2 + 1];
        if (gPtCanvas[(long)py * PT_W + px] != from) continue;
        lx = px; while (lx > 0 && gPtCanvas[(long)py * PT_W + lx - 1] == from) lx--;
        rx = px; while (rx < PT_W - 1 && gPtCanvas[(long)py * PT_W + rx + 1] == from) rx++;
        for (i = lx; i <= rx; i++)
            pt_set_px(w, i, py, pt_ink(i, py));
        for (i = lx; i <= rx; i++) {
            if (py > 0 && gPtCanvas[(long)(py - 1) * PT_W + i] == from && n < PT_STK) {
                stk[n * 2] = i; stk[n * 2 + 1] = (short)(py - 1); n++;
            }
            if (py < PT_H - 1 && gPtCanvas[(long)(py + 1) * PT_W + i] == from && n < PT_STK) {
                stk[n * 2] = i; stk[n * 2 + 1] = (short)(py + 1); n++;
            }
        }
    }
    DisposePtr((Ptr)stk);
}

/* ---- chrome ------------------------------------------------------------ */
static void pt_tool_rect(UnoWin *w, short i, Rect *r)
{
    short col = (short)(i % 2), row = (short)(i / 2);
    r->left = (short)(w->bounds.left + 6 + col * (PT_CELL + 2));
    r->top  = (short)(w->bounds.top + TBAR_H + 6 + row * (PT_CELL + 2));
    r->right  = (short)(r->left + PT_CELL);
    r->bottom = (short)(r->top + PT_CELL);
}

static void pt_draw_toolglyph(short i, Rect *r)
{
    Rect g;
    short cx = (short)((r->left + r->right) / 2);
    short cy = (short)((r->top + r->bottom) / 2);
    switch (i) {
    case T_PENCIL:
        MoveTo((short)(r->left + 5), (short)(r->bottom - 5));
        LineTo((short)(r->right - 5), (short)(r->top + 5));
        break;
    case T_BRUSH:
        SetRect(&g, (short)(cx - 3), (short)(cy - 5), (short)(cx + 3), (short)(cy + 2));
        PaintOval(&g);
        MoveTo(cx, (short)(cy + 2)); LineTo(cx, (short)(cy + 6));
        break;
    case T_ERASER:
        SetRect(&g, (short)(cx - 6), (short)(cy - 4), (short)(cx + 6), (short)(cy + 4));
        FrameRect(&g);
        break;
    case T_LINE:
        MoveTo((short)(r->left + 4), (short)(r->bottom - 6));
        LineTo((short)(r->right - 4), (short)(r->top + 6));
        break;
    case T_RECT:
        SetRect(&g, (short)(cx - 7), (short)(cy - 5), (short)(cx + 7), (short)(cy + 5));
        FrameRect(&g);
        break;
    case T_FRECT:
        SetRect(&g, (short)(cx - 7), (short)(cy - 5), (short)(cx + 7), (short)(cy + 5));
        PaintRect(&g);
        break;
    case T_OVAL:
        SetRect(&g, (short)(cx - 7), (short)(cy - 5), (short)(cx + 7), (short)(cy + 5));
        FrameOval(&g);
        break;
    case T_FOVAL:
        SetRect(&g, (short)(cx - 7), (short)(cy - 5), (short)(cx + 7), (short)(cy + 5));
        PaintOval(&g);
        break;
    case T_FILL:                                /* bucket: triangle + drip */
        MoveTo((short)(cx - 6), cy); LineTo(cx, (short)(cy - 6));
        LineTo((short)(cx + 6), cy); LineTo(cx, (short)(cy + 6));
        LineTo((short)(cx - 6), cy);
        break;
    case T_SPRAY: {
        short k;
        for (k = 0; k < 7; k++) {
            short sx = (short)(r->left + 5 + ((k * 5) % 13));
            short sy = (short)(r->top + 5 + ((k * 7) % 13));
            MoveTo(sx, sy); LineTo(sx, sy);
        }
        break;
    }
    }
}

#if UNO_COLOR
#define PT_NSWATCH 14
static const unsigned char kPtQuick[PT_NSWATCH] = {
    /* black, white, primaries/secondaries, ramps */
    0, 255, 180, 30, 5, 210, 35, 185, 215, 223, 232, 237, 241, 246
};
#endif

static void pt_strip_rect(UnoWin *w, short i, Rect *r)
{
    r->left = (short)(w->bounds.left + PT_TOOLW + 34 + i * 26);
    r->top  = (short)(w->bounds.bottom - 28);
    r->right  = (short)(r->left + 22);
    r->bottom = (short)(r->top + 18);
}

static void pt_draw_strip(UnoWin *w)
{
    short i; Rect r;
#if UNO_COLOR
    for (i = 0; i < PT_NSWATCH; i++) {
        pt_strip_rect(w, i, &r);
        RGBForeColor(&kPtPal[kPtQuick[i]]);
        PaintRect(&r);
        RGBForeColor(&kBlack);
        if (gPtColor == kPtQuick[i]) { InsetRect(&r, -2, -2); uno_box(&r, C_WHITE); }
    }
    /* current color + "more" cell that opens the full picker */
    pt_strip_rect(w, PT_NSWATCH, &r);
    RGBForeColor(&kPtPal[gPtColor]); PaintRect(&r); RGBForeColor(&kBlack);
    uno_box(&r, C_WHITE);
    text_at((short)(r.right + 6), (short)(r.bottom - 4), "c: all colors",
            C_CYAN, C_BLUE, false);
#else
    for (i = 0; i < 10; i++) {
        Pattern p;
        pt_strip_rect(w, i, &r);
        memcpy(&p, kPtPats[i], 8);
        FillRect(&r, &p);
        FrameRect(&r);
        if (gPtPat == i) { InsetRect(&r, -2, -2); FrameRect(&r); }
    }
#endif
}

#if UNO_COLOR
static void pt_picker_cell(UnoWin *w, short i, Rect *r)
{
    Rect cr;
    pt_canvas_rect(w, &cr);
    r->left = (short)(cr.left + 28 + (i % 16) * 22);
    r->top  = (short)(cr.top + 12 + (i / 16) * 13);
    r->right  = (short)(r->left + 20);
    r->bottom = (short)(r->top + 11);
}

static void pt_draw_picker(UnoWin *w)
{
    short i; Rect r, cr;
    pt_canvas_rect(w, &cr);
    uno_fill(&cr, C_BLUE);
    for (i = 0; i < 256; i++) {
        pt_picker_cell(w, i, &r);
        RGBForeColor(&kPtPal[i]); PaintRect(&r); RGBForeColor(&kBlack);
        if (i == gPtColor) { InsetRect(&r, -2, -2); uno_box(&r, C_WHITE); }
    }
    text_at((short)(cr.left + 28), (short)(cr.bottom - 6),
            "every 8-bit color - click to pick, c: back", C_CYAN, C_BLUE, false);
}
#endif

static void pt_repaint_canvas(UnoWin *w)
{
    Rect cr;
    short y;
    pt_canvas_rect(w, &cr);
#if UNO_COLOR
    if (gPtPicker) { pt_draw_picker(w); return; }
#endif
    if (!gPtCanvas) return;
    /* run-length rows */
    for (y = 0; y < PT_H; y++) {
        short x = 0;
        unsigned char *row = gPtCanvas + (long)y * PT_W;
        while (x < PT_W) {
            short x0 = x;
            unsigned char v = row[x];
            Rect run;
            while (x < PT_W && row[x] == v) x++;
            SetRect(&run, (short)(cr.left + x0), (short)(cr.top + y),
                          (short)(cr.left + x),  (short)(cr.top + y + 1));
#if UNO_COLOR
            RGBForeColor(&kPtPal[v]); PaintRect(&run);
#else
            if (v) PaintRect(&run);
            else { PenMode(patBic); PaintRect(&run); PenNormal(); }
#endif
        }
    }
#if UNO_COLOR
    RGBForeColor(&kBlack);
#endif
}

static void paint_draw(UnoWin *w)
{
    Rect r = w->bounds, ct = r, cr;
    short i;
    ct.top += TBAR_H; InsetRect(&ct, 1, 1); uno_fill(&ct, C_BLUE);
    /* tool palette */
    for (i = 0; i < PT_TOOLS; i++) {
        Rect tr; pt_tool_rect(w, i, &tr);
        uno_fill(&tr, C_WHITE);
#if UNO_COLOR
        RGBForeColor(&kBlack);
#endif
        pt_draw_toolglyph(i, &tr);
        if (i == gPtTool) { InsetRect(&tr, -2, -2); uno_box(&tr, C_MAG); }
        else { uno_box(&tr, C_CYAN); }
    }
    /* canvas frame + content */
    pt_canvas_rect(w, &cr);
    InsetRect(&cr, -1, -1); uno_box(&cr, C_WHITE); InsetRect(&cr, 1, 1);
    pt_repaint_canvas(w);
    pt_draw_strip(w);
}

static void paint_open(void)
{
#if UNO_COLOR
    pt_build_palette();
#endif
    if (!gPtCanvas) {
        gPtCanvas = (unsigned char *)NewPtr((long)PT_W * PT_H);
        if (gPtCanvas) memset(gPtCanvas, PT_BG, (long)PT_W * PT_H);
    }
}

static void pt_save(void)
{
    if (!gPtCanvas) return;
    fat12_write("PAINT.UNO", gPtCanvas, (long)PT_W * PT_H);
    fat12_list();
}

static void pt_load(UnoWin *w)
{
    if (!gPtCanvas) return;
    fat12_read("PAINT.UNO", gPtCanvas, (long)PT_W * PT_H);
    if (w) draw_window(w);
}

static Boolean paint_key(char ch, short code, Boolean cmd)
{
    UnoWin *w = find_app_window(APP_PAINT);
    (void)code;
    if (cmd) return false;
    if (ch >= '1' && ch <= '9') gPtTool = (short)(ch - '1');
    else if (ch == '0') gPtTool = T_SPRAY;
#if UNO_COLOR
    else if (ch == 'c') gPtPicker = !gPtPicker;
#endif
    else if (ch == 'n') { if (gPtCanvas) memset(gPtCanvas, PT_BG, (long)PT_W * PT_H); }
    else if (ch == 's') { pt_save(); return true; }
    else if (ch == 'l') { pt_load(w); return true; }
    else return false;
    if (w) draw_window(w);
    return true;
}

static void paint_click(UnoWin *w, Point p)
{
    Rect cr;
    short i;
    /* tool palette */
    for (i = 0; i < PT_TOOLS; i++) {
        Rect tr; pt_tool_rect(w, i, &tr);
        if (PtInRect(p, &tr)) { gPtTool = i; draw_window(w); return; }
    }
    /* strip */
#if UNO_COLOR
    for (i = 0; i <= PT_NSWATCH; i++) {
        Rect sr; pt_strip_rect(w, i, &sr);
        if (PtInRect(p, &sr)) {
            if (i == PT_NSWATCH) gPtPicker = !gPtPicker;
            else gPtColor = kPtQuick[i];
            draw_window(w);
            return;
        }
    }
#else
    for (i = 0; i < 10; i++) {
        Rect sr; pt_strip_rect(w, i, &sr);
        if (PtInRect(p, &sr)) { gPtPat = i; draw_window(w); return; }
    }
#endif
    pt_canvas_rect(w, &cr);
    if (!PtInRect(p, &cr) || !gPtCanvas) return;

#if UNO_COLOR
    if (gPtPicker) {                            /* picking from the grid */
        for (i = 0; i < 256; i++) {
            Rect pc; pt_picker_cell(w, i, &pc);
            if (PtInRect(p, &pc)) { gPtColor = (short)i; break; }
        }
        gPtPicker = false;
        draw_window(w);
        return;
    }
#endif
    {
        short x0 = (short)(p.h - cr.left), y0 = (short)(p.v - cr.top);
        short lx = x0, ly = y0;
        Point q;
        switch (gPtTool) {
        case T_PENCIL: case T_BRUSH: case T_ERASER: case T_SPRAY:
            for (;;) {
                GetMouse(&q);
                {
                    short x = (short)(q.h - cr.left), y = (short)(q.v - cr.top);
                    if (x < 0) x = 0; if (y < 0) y = 0;
                    if (x >= PT_W) x = PT_W - 1; if (y >= PT_H) y = PT_H - 1;
                    if (gPtTool == T_SPRAY) {
                        for (i = 0; i < 6; i++) {
                            short rx = (short)(x + (Random() % 11) - 5);
                            short ry = (short)(y + (Random() % 11) - 5);
                            if (rx >= 0 && ry >= 0 && rx < PT_W && ry < PT_H)
                                pt_set_px(w, rx, ry, pt_ink(rx, ry));
                        }
                    } else {
                        short sz = (short)(gPtTool == T_PENCIL ? 1 :
                                           gPtTool == T_BRUSH  ? 4 : 8);
                        /* connect drag gaps with a line of dots */
                        short sdx = (short)(x > lx ? x - lx : lx - x);
                        short sdy = (short)(y > ly ? y - ly : ly - y);
                        if (sdx > 1 || sdy > 1) {
                            short steps = (short)(sdx > sdy ? sdx : sdy), s2;
                            for (s2 = 1; s2 <= steps; s2++)
                                pt_dot(w, (short)(lx + (long)(x - lx) * s2 / steps),
                                          (short)(ly + (long)(y - ly) * s2 / steps),
                                       sz, gPtTool == T_ERASER);
                        } else {
                            pt_dot(w, x, y, sz, gPtTool == T_ERASER);
                        }
                        lx = x; ly = y;
                    }
                }
                if (!StillDown()) break;
            }
            break;
        case T_FILL:
            pt_flood(w, x0, y0);
            break;
        default: {                              /* rubber-band shapes */
            Rect band; short x1 = x0, y1 = y0;
            Boolean shown = false;
            PenMode(patXor);
            for (;;) {
                GetMouse(&q);
                {
                    short nx = (short)(q.h - cr.left), ny = (short)(q.v - cr.top);
                    if (nx < 0) nx = 0; if (ny < 0) ny = 0;
                    if (nx >= PT_W) nx = PT_W - 1; if (ny >= PT_H) ny = PT_H - 1;
                    if (nx != x1 || ny != y1 || !shown) {
                        if (shown) FrameRect(&band);    /* erase old */
                        x1 = nx; y1 = ny;
                        SetRect(&band,
                            (short)(cr.left + (x0 < x1 ? x0 : x1)),
                            (short)(cr.top  + (y0 < y1 ? y0 : y1)),
                            (short)(cr.left + (x0 > x1 ? x0 : x1) + 1),
                            (short)(cr.top  + (y0 > y1 ? y0 : y1) + 1));
                        FrameRect(&band);
                        shown = true;
                    }
                }
                if (!StillDown()) break;
            }
            if (shown) FrameRect(&band);
            PenNormal();
            switch (gPtTool) {
            case T_LINE:  pt_line(w, x0, y0, x1, y1, 1); break;
            case T_RECT:  pt_rect_shape(w, x0, y0, x1, y1, false); break;
            case T_FRECT: pt_rect_shape(w, x0, y0, x1, y1, true);  break;
            case T_OVAL:  pt_oval_shape(w, x0, y0, x1, y1, false); break;
            case T_FOVAL: pt_oval_shape(w, x0, y0, x1, y1, true);  break;
            }
            break;
        }
        }
    }
}
static const AppInterface kIface = {
    paint_draw, paint_key, paint_click, 0, paint_open, 0,
    "Paint", { 14, 24, 498, 334 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
