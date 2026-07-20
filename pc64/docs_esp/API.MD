# UnoDOS app API reference

Everything an app can call. The drawing/util/sound/file calls come through the
**KernelApi** table (reached by the `gK->...` prelude macros, which `UNO.H`
exposes under these plain names). The Toolbox geometry/drawing calls and the
libc slice are ordinary functions your app calls directly.

## The window and palette

- `TBAR_H` — title-bar height; your content starts below it.
- Colours: `C_BLUE`, `C_CYAN`, `C_MAG`, `C_WHITE` (the 4-colour window palette).
- `kPalette[c]` — the `RGBColor` for palette index `c`; `kBlack`.

## Drawing (KernelApi)

    void uno_fill(Rect *r, short c);          /* fill r with palette colour c */
    void uno_box (Rect *r, short c);          /* 1px outline                  */
    void uno_invert(Rect *r);                 /* invert the pixels in r       */
    void text_at(short x, short y, const char *s, short fg, short bg, Boolean opaque);
    void text_at_max(short x, short y, const char *s, short fg, short maxw);
    void fill_rgb(Rect *q, const GameRGB *c); /* true-colour fill (r,g,b)     */
    void fmt_u(long v, char *out);            /* unsigned -> decimal string   */
    void put2(long v, char *out);             /* 2-digit zero-padded          */

## Toolbox geometry + drawing (call directly)

    void SetRect(Rect *r, short l, short t, short ri, short b);
    void OffsetRect(Rect *r, short dh, short dv);
    void InsetRect(Rect *r, short dh, short dv);
    void PaintRect(const Rect *r);   void FrameRect(const Rect *r);
    void InvertRect(const Rect *r);
    void PaintOval(const Rect *r);   void FrameOval(const Rect *r);
    void MoveTo(short h, short v);   void LineTo(short h, short v);
    void PenNormal(void);            void PenMode(short mode);
    void RGBForeColor(const RGBColor *c);

`Point` is passed **by value** (`{short v, h;}`) — the one small struct UnoC
passes in a register. `PtInRect` is not available (it also takes a Point by
value in a form UnoC can't express); compare `p.h`/`p.v` against the rect.

## Windows / launching (KernelApi)

    UnoWin *find_app_window(short proc);      /* your window, or 0            */
    void    draw_window(UnoWin *w);           /* request a repaint            */
    void    launch_app(short proc);
    void    repaint_all(void);

Your own window answers `find_app_window(APP_RUNNER)` (the user-app slot id).

## Time, input, memory (call directly)

    long  TickCount(void);     /* ~60 Hz tick counter                        */
    void  GetMouse(Point *p);  /* current pointer (into a Point you own)     */
    Boolean StillDown(void);   /* is the button still held                   */
    short Random(void);        /* pseudo-random                              */
    Ptr   NewPtr(long n);      /* allocate n bytes (malloc)                  */
    void  DisposePtr(Ptr p);   /* free                                       */

## Sound (KernelApi)

    void music_note_on(short midi, short durTicks);   /* one note            */
    void music_quiet(void);
    void gm_start(const Note *notes, short count, short owner);  /* loop song */
    void gm_stop(void);

`Note` is `{unsigned char midi; unsigned char dur;}`.

## Files (KernelApi)

    Boolean fat12_mount(void);
    long    fat12_read(const char *name, unsigned char *buf, long max);
    Boolean fat12_write(const char *name, const unsigned char *buf, long len);

## libc (call directly)

    memcpy memmove memset memcmp
    strlen strcpy strncpy strcat strcmp strncmp

---

**This list is the whole surface.** Studio's compiler checks every function
you call against it at build time, so an unavailable call is a compile error
with a line number, never a mysterious crash. The list is generated from the
kernel's own export table, so it can't drift out of date.
