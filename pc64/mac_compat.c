/* ===========================================================================
 * UnoDOS/PS2 - Mac Toolbox compatibility shim implementation (see mac_compat.h).
 *
 * One implicit full-screen GrafPort over the software framebuffer fb.*. All
 * QuickDraw drawing state (pen position, fore/back colour, transfer mode) is
 * module-global here - UnoDOS only ever draws into the one port.
 * ===========================================================================
 */
#include "mac_compat.h"
#include <stdlib.h>
#include <string.h>

QDGlobals qd;

/* ---- drawing state ----------------------------------------------------- */
static fb_px  gFore  = UNO_BLACK;
static fb_px  gBack  = UNO_WHITE;
static short  gPenX  = 0, gPenY = 0;
static short  gPenW  = 1, gPenH = 1;
static short  gPenMode = patCopy;     /* patXor => XOR drawing (drag outlines) */
static short  gTextMode = srcOr;      /* srcCopy => opaque text (paints gBack) */

/* event queue (tiny ring) + mouse, fed by the platform main ------------- */
#define EVQ 32
static EventRecord gEvQ[EVQ];
static int gEvHead = 0, gEvTail = 0;
static Point   gMouse = { 0, 0 };
static Boolean gMouseDown = false;
static long    gTicks = 0;

/* ---- colour helpers ---------------------------------------------------- */
static fb_px rgb_to_fb(const RGBColor *c)
{
    return FB_RGB(c->red >> 8, c->green >> 8, c->blue >> 8);
}
static fb_px qd_color(long color)
{
    switch (color) {
        case whiteColor:  return UNO_WHITE;
        case redColor:    return FB_RGB(0xAA, 0x00, 0x00);
        case greenColor:  return FB_RGB(0x00, 0xAA, 0x00);
        case blueColor:   return UNO_BLUE;
        case yellowColor: return FB_RGB(0xFF, 0xFF, 0x00);
        case blackColor:
        default:          return UNO_BLACK;
    }
}

/* ---- init -------------------------------------------------------------- */
void InitGraf(void *globalsPtr)
{
    (void)globalsPtr;
    memset(&qd, 0, sizeof qd);
    qd.screenBits.bounds.top = 0;
    qd.screenBits.bounds.left = 0;
    qd.screenBits.bounds.right = FB_W;
    qd.screenBits.bounds.bottom = FB_H;
    memset(qd.white.pat, 0x00, 8);
    memset(qd.black.pat, 0xFF, 8);
    { int i; for (i = 0; i < 8; i++) qd.gray.pat[i] = (i & 1) ? 0x55 : 0xAA; }
    memcpy(qd.ltGray.pat, qd.gray.pat, 8);
    memcpy(qd.dkGray.pat, qd.gray.pat, 8);
    gFore = UNO_BLACK; gBack = UNO_WHITE;
    gPenMode = patCopy; gTextMode = srcOr;
}
void InitFonts(void)   {}
void InitWindows(void) {}
void InitMenus(void)   {}
void TEInit(void)      {}
void InitDialogs(void *r) { (void)r; }
void InitCursor(void)  {}
void FlushEvents(short a, short b) { (void)a; (void)b; gEvHead = gEvTail = 0; }

static GrafPort gPort;
WindowPtr NewWindow(void *s, const Rect *b, ConstStr255Param t, Boolean v,
                    short p, WindowPtr be, Boolean g, long rc)
{
    (void)s; (void)t; (void)v; (void)p; (void)be; (void)g; (void)rc;
    gPort.portRect = *b;
    gPort.portBits.bounds = *b;
    return &gPort;
}
WindowPtr NewCWindow(void *s, const Rect *b, ConstStr255Param t, Boolean v,
                     short p, WindowPtr be, Boolean g, long rc)
{
    return NewWindow(s, b, t, v, p, be, g, rc);
}
void SetPort(GrafPtr port) { qd.thePort = port; }

/* ---- rect math --------------------------------------------------------- */
void SetRect(Rect *r, short l, short t, short rt, short b)
{ r->left = l; r->top = t; r->right = rt; r->bottom = b; }
void OffsetRect(Rect *r, short dh, short dv)
{ r->left += dh; r->right += dh; r->top += dv; r->bottom += dv; }
void InsetRect(Rect *r, short dh, short dv)
{ r->left += dh; r->right -= dh; r->top += dv; r->bottom -= dv; }
Boolean PtInRect(Point pt, const Rect *r)
{ return (Boolean)(pt.h >= r->left && pt.h < r->right &&
                   pt.v >= r->top  && pt.v < r->bottom); }

/* ---- colour / pen / text state ----------------------------------------- */
void RGBForeColor(const RGBColor *c) { gFore = rgb_to_fb(c); }
void RGBBackColor(const RGBColor *c) { gBack = rgb_to_fb(c); }
void ForeColor(long color) { gFore = qd_color(color); }
void BackColor(long color) { gBack = qd_color(color); }
void PenNormal(void) { gPenW = gPenH = 1; gPenMode = patCopy; }
void PenMode(short mode) { gPenMode = mode; }
void PenSize(short w, short h) { gPenW = w; gPenH = h; }
void PenPat(const Pattern *pat) { (void)pat; }   /* solid-colour approximation */
void TextMode(short mode) { gTextMode = mode; }
void TextFont(short f) { (void)f; }
void TextSize(short s) { (void)s; }
void TextFace(short f) { (void)f; }

/* ---- rect/oval fills with the current pen mode -------------------------- */
static void rect_norm(const Rect *r, int *x, int *y, int *w, int *h)
{ *x = r->left; *y = r->top; *w = r->right - r->left; *h = r->bottom - r->top; }

void PaintRect(const Rect *r)
{
    int x, y, w, h; rect_norm(r, &x, &y, &w, &h);
    if (gPenMode == patXor || gPenMode == srcXor) fb_invert_rect(x, y, w, h);
    else fb_fill_rect(x, y, w, h, gFore);
}
void EraseRect(const Rect *r)
{ int x, y, w, h; rect_norm(r, &x, &y, &w, &h); fb_fill_rect(x, y, w, h, gBack); }

void FrameRect(const Rect *r)
{
    int x, y, w, h; rect_norm(r, &x, &y, &w, &h);
    if (gPenMode == patXor || gPenMode == srcXor) {
        if (w <= 0 || h <= 0) return;
        fb_invert_rect(x, y, w, 1); fb_invert_rect(x, y + h - 1, w, 1);
        fb_invert_rect(x, y, 1, h); fb_invert_rect(x + w - 1, y, 1, h);
    } else {
        fb_frame_rect(x, y, w, h, gFore);
    }
}
void FillRect(const Rect *r, const Pattern *pat)
{
    /* colour port: pattern fills are gray-ish dithers or solids. Treat an
       all-ones pattern as fore-black, all-zeros as white, else 50% dither. */
    int x, y, w, h, i, j, solid0 = 1, solid1 = 1;
    rect_norm(r, &x, &y, &w, &h);
    for (i = 0; i < 8; i++) { if (pat->pat[i] != 0xFF) solid1 = 0; if (pat->pat[i] != 0x00) solid0 = 0; }
    if (solid1) { fb_fill_rect(x, y, w, h, UNO_BLACK); return; }
    if (solid0) { fb_fill_rect(x, y, w, h, UNO_WHITE); return; }
    if (!(w > 0 && h > 0)) return;
    for (i = 0; i < h; i++) for (j = 0; j < w; j++) {
        int xx = x + j, yy = y + i;
        if (xx < 0 || xx >= FB_W || yy < 0 || yy >= FB_H) continue;
        if ((pat->pat[i & 7] >> (7 - (j & 7))) & 1) fb[yy * FB_W + xx] = UNO_BLACK;
        else                                        fb[yy * FB_W + xx] = UNO_WHITE;
    }
}
void InvertRect(const Rect *r)
{ int x, y, w, h; rect_norm(r, &x, &y, &w, &h); fb_invert_rect(x, y, w, h); }

/* ---- lines ------------------------------------------------------------- */
static void put_pen(int x, int y)
{
    int dx, dy;
    for (dy = 0; dy < gPenH; dy++) for (dx = 0; dx < gPenW; dx++) {
        int xx = x + dx, yy = y + dy;
        if (xx < 0 || xx >= FB_W || yy < 0 || yy >= FB_H) continue;
        if (gPenMode == patXor || gPenMode == srcXor) fb[yy * FB_W + xx] ^= 0x00FFFFFFu;
        else fb[yy * FB_W + xx] = gFore;
    }
}
static void draw_line(int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        put_pen(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
void MoveTo(short h, short v) { gPenX = h; gPenY = v; }
void LineTo(short h, short v) { draw_line(gPenX, gPenY, h, v); gPenX = h; gPenY = v; }

/* ---- ovals / round-rects / arcs (approximate) -------------------------- */
static void oval_span(const Rect *r, int fill)
{
    int x, y, w, h, cx2, cy2, a, b, i, j;
    rect_norm(r, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    cx2 = 2 * x + w - 1; cy2 = 2 * y + h - 1;     /* centre*2 (fixed .5) */
    a = w - 1; b = h - 1;
    for (i = 0; i < h; i++) for (j = 0; j < w; j++) {
        int xx = x + j, yy = y + i;
        long nx = (long)(2 * xx - cx2) * b, ny = (long)(2 * yy - cy2) * a;
        long d = nx * nx + ny * ny, rr = (long)a * b; rr *= rr;
        int inside = d <= rr;
        int edge = inside && !( (j>0 && j<w-1 && i>0 && i<h-1) &&
                    ((long)(2*(xx-1)-cx2)*b*(long)(2*(xx-1)-cx2)*b + ny*ny <= rr) &&
                    ((long)(2*(xx+1)-cx2)*b*(long)(2*(xx+1)-cx2)*b + ny*ny <= rr) &&
                    (nx*nx + (long)(2*(yy-1)-cy2)*a*(long)(2*(yy-1)-cy2)*a <= rr) &&
                    (nx*nx + (long)(2*(yy+1)-cy2)*a*(long)(2*(yy+1)-cy2)*a <= rr) );
        if (xx < 0 || xx >= FB_W || yy < 0 || yy >= FB_H) continue;
        if (fill ? inside : edge) {
            if (gPenMode == patXor || gPenMode == srcXor) fb[yy * FB_W + xx] ^= 0x00FFFFFFu;
            else fb[yy * FB_W + xx] = gFore;
        }
    }
}
void PaintOval(const Rect *r) { oval_span(r, 1); }
void FrameOval(const Rect *r) { oval_span(r, 0); }
void FrameRoundRect(const Rect *r, short ow, short oh) { (void)ow; (void)oh; FrameRect(r); }
void FrameArc(const Rect *r, short s, short a) { (void)r; (void)s; (void)a; } /* clock hand sweep - skip */

/* ---- text -------------------------------------------------------------- */
void DrawText(const void *buf, short first, short count)
{
    const char *s = (const char *)buf + first;
    long bg = (gTextMode == srcCopy) ? (long)gBack : -1L;
    int x = gPenX, ytop = gPenY - 7, i;     /* pen v = baseline; 8x8 top = -7 */
    for (i = 0; i < count; i++) x = fb_glyph(x, ytop, (unsigned char)s[i], gFore, bg);
    gPenX = (short)x;
}
short TextWidth(const void *buf, short first, short count)
{ (void)buf; (void)first; return (short)(count * 8); }
void GlobalToLocal(Point *pt) { (void)pt; }   /* full-screen port anchored at 0,0 */

/* ---- events / time / input --------------------------------------------- */
long TickCount(void) { return ++gTicks; }     /* monotonic call-clock (deterministic) */

void uno_post_event(short what, long message, Point where, short modifiers)
{
    int n = (gEvTail + 1) % EVQ;
    if (n == gEvHead) return;                  /* full - drop */
    gEvQ[gEvTail].what = what;
    gEvQ[gEvTail].message = message;
    gEvQ[gEvTail].when = gTicks;
    gEvQ[gEvTail].where = where;
    gEvQ[gEvTail].modifiers = modifiers;
    gEvTail = n;
}
void uno_set_mouse(short h, short v, Boolean down)
{ gMouse.h = h; gMouse.v = v; gMouseDown = down; }

Boolean GetNextEvent(short mask, EventRecord *ev)
{
    (void)mask;
    if (gEvHead == gEvTail) { return false; }
    *ev = gEvQ[gEvHead];
    gEvHead = (gEvHead + 1) % EVQ;
    return true;
}
/* Optional live-pointer hook (platform-set). When present, GetMouse pumps it so
 * a blocking drag loop (Paint) tracks the real cursor and the button releases;
 * NULL on ports that update gMouse from their event pump instead. */
int (*uno_mac_mouse)(short *h, short *v) = 0;
void GetMouse(Point *p)
{
    if (uno_mac_mouse) { short h = 0, v = 0; int b = uno_mac_mouse(&h, &v);
        gMouse.h = h; gMouse.v = v; gMouseDown = b ? true : false; }
    *p = gMouse;
}
Boolean StillDown(void) { return gMouseDown; }

static unsigned long gRnd = 0x2A6D365Bul;
short Random(void)
{ gRnd = gRnd * 1103515245ul + 12345ul; return (short)((gRnd >> 16) & 0xFFFF); }

/* ---- memory ------------------------------------------------------------ */
Ptr NewPtr(long n) { return (Ptr)malloc(n > 0 ? (size_t)n : 1); }
void DisposePtr(Ptr p) { free(p); }
