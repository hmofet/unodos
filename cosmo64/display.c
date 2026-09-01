/* cosmo64/display.c -- the pc64 shell's display platform layer on the Cosmo.
 *
 * The shell draws into pc64's software framebuffer fb[] (fb.h, 0xAABBGGRR,
 * FB_W x FB_H = 640x480 here) and calls uno_pc64_present() each frame; this
 * rotates it 270 degrees, scales it 2x, swizzles R<->B (the panel wants
 * 0xAARRGGBB) and writes it centred into LK's adopted framebuffer -- the same
 * present the m0 payload proved on hardware, plus the channel swap the x86
 * build did with gSwapRB. Boot-time adoption itself lives in videolfb.c.
 */

#include "cosmo64.h"
#include "fb.h"
#include "mac_compat.h"

int uno_fb_w = 640, uno_fb_h = 480;

/* ---- the pointer -------------------------------------------------------- */
/* The shell never draws its own cursor: on x86 uefi_main.c composites it into
 * each presented row (cursor_row()), and this port replaces uefi_main.c, so
 * until now nothing drew one at all. Touch worked and the shell tracked it --
 * there was simply no arrow on the panel, which is exactly what the hardware
 * test reported.
 *
 * Drawn AFTER the blit rather than inside it: this present repaints the whole
 * UI rect every frame, so the previous position is already erased and a
 * post-pass leaves no trail, at a cost of ~15x9 source pixels instead of a
 * branch in the hot loop.
 *
 * Same sprite as the x86 build, so the pointer looks identical on both.
 * 'B' = black, 'W' = white, ' ' = transparent. */
static const char *const kCursor[] = {
    "B", "BB", "BWB", "BWWB", "BWWWB", "BWWWWB", "BWWWWWB", "BWWWWWWB",
    "BWWWWBBBB", "BWWBWB", "BWB BWB", "BB  BWB", "B    BWB", "      BWB",
    "       BB", 0
};

/* Gated on a pointer having actually reported, exactly like x86's
 * g_have_pointer -- and load-bearing for the gate: QEMU has no touch panel, so
 * no cursor is drawn there and qharness.py's pixel-exact eye check still
 * compares the panel against fb[] with nothing extra composited on top. */
int c64_input_have_pointer(void);

static void draw_cursor(void)
{
    if (!c64_input_have_pointer())
        return;
    int cx, cy, btn;
    uno_pc64_mouse(&cx, &cy, &btn);
    c64_u8 *origin = (c64_u8 *)FBDBG->fb_dorigin;
    c64_u32 ppitch = FBDBG->fb_ppitch;

    for (int r = 0; r < 15 && kCursor[r]; r++) {
        int y = cy + r;                            /* source row  */
        if (y < 0 || y >= C64_SCRH)
            continue;
        const char *row = kCursor[r];
        for (int c = 0; row[c]; c++) {
            int x = cx + c;                        /* source column */
            if (x < 0 || x >= C64_SCRW || row[c] == ' ')
                continue;
            /* the same rotation the blit above uses: source column x lands on
             * panel row (C64_SCRW-1-x), source row y on panel column y, each
             * an FB_SCALE block */
            c64_u32 out = (row[c] == 'B') ? 0xFF000000u : 0xFFFFFFFFu;
            int sr = (C64_SCRW - 1) - x;
            c64_u8 *p = origin + (c64_u64)sr * FB_SCALE * ppitch
                      + (c64_u64)y * FB_SCALE * 4;
            for (int rep = 0; rep < FB_SCALE; rep++) {
                c64_u32 *d = (c64_u32 *)p;
                for (int k = 0; k < FB_SCALE; k++)
                    d[k] = out;
                p += ppitch;
            }
        }
    }
}

/* ---- only push what changed --------------------------------------------- */
/* This used to blit the whole 640x480 into the whole 960x1280 rect on EVERY
 * frame: 1.2 MB read with a 2560-byte stride and 9.4 MB written to
 * non-cacheable memory, whether or not a single pixel had moved. On the device
 * that is what "incredibly slow" looked like.
 *
 * x86 does not do that, and never did -- uefi_main.c keeps a shadow of fb[],
 * compares it per row, and Blts only the dirty ones (gShadow/gDirtyRow). That
 * lives in the platform layer, which this port REPLACES, so the port simply
 * did not have it. Same shape of omission as the missing cursor.
 *
 * The rotation decides what shape the tracking has to take. Source column x
 * becomes panel ROW (639-x), and source row y becomes panel COLUMN y, so a
 * changed source COLUMN is a contiguous run of panel memory while a changed
 * source row is a stride. A bounding box in source space therefore maps to one
 * contiguous rectangle of panel rows, which is what gets pushed. A box is
 * coarser than x86's per-row spans, but it collapses the common cases -- a
 * ticking clock, a menu opening, a cursor moving -- to a few percent of the
 * panel, and it costs one sequential compare pass over a cacheable 1.2 MB
 * rather than a strided 9.4 MB of writes.
 *
 * The cursor is composited onto the panel, not into fb[], so the compare pass
 * cannot see it. Its old and new rects are unioned into the box by hand, or a
 * moving pointer would smear. */
static fb_px g_shadow[C64_SCRW * C64_SCRH];
static int g_shadow_valid;
static int g_pcx = -1, g_pcy;                /* cursor rect last composited */
static c64_u32 g_line[C64_SCRH * FB_SCALE];  /* one output row, built once */

void uno_pc64_dirty_all(void)
{
    g_shadow_valid = 0;
}

/* Present timing, reported every 300 frames. The last round of this work was
 * slowed down by a diagnosis reached from the armchair, so the panel now
 * reports what it actually costs. */
static c64_u64 g_t_acc, g_box_acc;
static unsigned g_frames, g_skipped;

static void present_done(c64_u64 t0, c64_u64 area)
{
    g_t_acc += c64_cnt_now() - t0;
    g_box_acc += area;
    if (!area)
        g_skipped++;
    if (++g_frames < 300)
        return;
    c64_u64 hz = c64_cnt_freq();
    if (!hz)
        hz = 13000000ull;
    c64_logf("present: %d frames, avg %d us, avg box %d px, %d skipped\n",
             (int)g_frames, (int)(g_t_acc * 1000000ull / hz / g_frames),
             (int)(g_box_acc / g_frames), (int)g_skipped);
    g_t_acc = g_box_acc = 0;
    g_frames = g_skipped = 0;
}

void uno_pc64_present(void)
{
    c64_u64 t0 = c64_cnt_now();
    /* publish fb[] as the source surface for the harness's eye check (the
     * harness detects the R<->B swizzle by trying both channel orders) */
    FBDBG->fb_shadow = (c64_u64)fb;
    FBDBG->fb_base = (c64_u64)fb;
    FBDBG->fb_pitch = C64_SCRW * 4;
    c64_u8 *origin = (c64_u8 *)FBDBG->fb_dorigin;
    c64_u32 ppitch = FBDBG->fb_ppitch;

    int x0 = C64_SCRW, x1 = -1, y0 = C64_SCRH, y1 = -1;
    if (!g_shadow_valid) {
        for (int i = 0; i < C64_SCRW * C64_SCRH; i++)
            g_shadow[i] = fb[i];
        g_shadow_valid = 1;
        x0 = 0; x1 = C64_SCRW - 1; y0 = 0; y1 = C64_SCRH - 1;
    } else {
        for (int y = 0; y < C64_SCRH; y++) {
            const fb_px *s = fb + (unsigned)y * C64_SCRW;
            fb_px *sh = g_shadow + (unsigned)y * C64_SCRW;
            int rx0 = -1, rx1 = -1;
            for (int x = 0; x < C64_SCRW; x++)
                if (s[x] != sh[x]) {
                    sh[x] = s[x];
                    if (rx0 < 0)
                        rx0 = x;
                    rx1 = x;
                }
            if (rx0 < 0)
                continue;
            if (rx0 < x0) x0 = rx0;
            if (rx1 > x1) x1 = rx1;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }

    /* union in the cursor's old and new rects (source space, 9x15) */
    int cx = -1, cy = 0;
    if (c64_input_have_pointer()) {
        int bx, by, bbtn;
        uno_pc64_mouse(&bx, &by, &bbtn);
        cx = bx;
        cy = by;
    }
    for (int i = 0; i < 2; i++) {
        int px_ = i ? g_pcx : cx, py_ = i ? g_pcy : cy;
        if (px_ < 0)
            continue;
        int ax0 = px_, ax1 = px_ + 8, ay0 = py_, ay1 = py_ + 14;
        if (ax0 < 0) ax0 = 0;
        if (ay0 < 0) ay0 = 0;
        if (ax1 > C64_SCRW - 1) ax1 = C64_SCRW - 1;
        if (ay1 > C64_SCRH - 1) ay1 = C64_SCRH - 1;
        if (ax1 < ax0 || ay1 < ay0)
            continue;
        if (ax0 < x0) x0 = ax0;
        if (ax1 > x1) x1 = ax1;
        if (ay0 < y0) y0 = ay0;
        if (ay1 > y1) y1 = ay1;
    }
    g_pcx = cx;
    g_pcy = cy;

    if (x1 < x0 || y1 < y0) {
        c64_bcn(BCN_MAIN);            /* nothing moved: the panel is correct */
        present_done(t0, 0);
        return;
    }

    /* One output row per source column, built once into g_line and then
     * written FB_SCALE times: the strided source read is the expensive half,
     * so it happens once rather than per repeat, and both writes are
     * sequential, which is what non-cacheable memory wants. */
    for (int x = x1; x >= x0; x--) {
        const fb_px *s = fb + (unsigned)y0 * C64_SCRW + x;
        int n = 0;
        for (int y = y0; y <= y1; y++) {
            fb_px px = *s;
            c64_u32 out = 0xFF000000u | ((px & 0xFFu) << 16)
                        | (px & 0xFF00u) | ((px >> 16) & 0xFFu);
            for (int k = 0; k < FB_SCALE; k++)
                g_line[n++] = out;
            s += C64_SCRW;                        /* one source row down */
        }
        int sr = (C64_SCRW - 1) - x;
        c64_u8 *drow = origin + (c64_u64)sr * FB_SCALE * ppitch
                     + (c64_u64)y0 * FB_SCALE * 4;
        for (int rep = 0; rep < FB_SCALE; rep++) {
            c64_u32 *dst = (c64_u32 *)drow;
            for (int i = 0; i < n; i++)
                dst[i] = g_line[i];
            drow += ppitch;
        }
    }
    draw_cursor();
    __asm__ volatile("dsb sy" ::: "memory");
    c64_bcn(BCN_MAIN);                            /* the shell is presenting */
    present_done(t0, (c64_u64)(x1 - x0 + 1) * (c64_u64)(y1 - y0 + 1));
}

void uno_pc64_present_cursor(void)
{
    uno_pc64_present();
}

static fb_px g_scene[C64_SCRW * C64_SCRH];

void uno_pc64_scene_save(void)
{
    for (int i = 0; i < C64_SCRW * C64_SCRH; i++)
        g_scene[i] = fb[i];
}

void uno_pc64_scene_restore(void)
{
    for (int i = 0; i < C64_SCRW * C64_SCRH; i++)
        fb[i] = g_scene[i];
    /* the whole surface just changed underneath the shadow */
    uno_pc64_dirty_all();
}

void uno_pc64_lowres(int on)
{
    (void)on;                                     /* one mode on this panel */
}

int uno_pc64_res_count(void)
{
    return 1;
}

void uno_pc64_res_get(int idx, short *w, short *h, short *zoom, Boolean *active)
{
    (void)idx;
    *w = C64_SCRW;
    *h = C64_SCRH;
    *zoom = FB_SCALE;
    *active = 1;
}

void uno_pc64_res_set(int idx)
{
    (void)idx;
}
