/* cosmo64/display.c -- the pc64 shell's display platform layer on the Cosmo.
 *
 * The shell draws into pc64's software framebuffer fb[] (fb.h, 0xAABBGGRR,
 * FB_W x FB_H = the desktop size) and calls uno_pc64_present() each frame;
 * this rotates it 270 degrees, scales it by an integer zoom, swizzles R<->B
 * (the panel wants 0xAARRGGBB) and writes it centred into LK's adopted
 * framebuffer -- the same present the m0 payload proved on hardware, plus the
 * channel swap the x86 build did with gSwapRB. Boot-time adoption itself
 * lives in videolfb.c, and so does the geometry (c64_geom_set), because
 * touch.c has to invert this transform without linking this file.
 *
 * The desktop starts at 1080x540 zoom 2 -- covering the panel exactly, at the
 * 2x device pixel ratio a 403 DPI phone panel wants. It used to be 640x480 at
 * zoom 2 -- the rpi port's size, carried in when this lane was forked from it
 * -- which drew a 960x1280 window on a 1080x2160 panel and left the rest
 * black. Control Panel > Display lists the sizes below, the true native
 * 2160x1080 among them, and every one of them is a whole-pixel zoom, so text
 * stays sharp at all of them.
 */

#include "cosmo64.h"
#include "fb.h"
#include "mac_compat.h"

int uno_fb_w = C64_SCRW, uno_fb_h = C64_SCRH;

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
void uno_screen_changed(void);   /* core hook: desktop size changed (unodos.c) */

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
        if (y < 0 || y >= c64_scrh)
            continue;
        const char *row = kCursor[r];
        for (int c = 0; row[c]; c++) {
            int x = cx + c;                        /* source column */
            if (x < 0 || x >= c64_scrw || row[c] == ' ')
                continue;
            /* the same rotation the blit above uses: source column x lands on
             * panel row (c64_scrw-1-x), source row y on panel column y, each
             * a c64_scale block */
            c64_u32 out = (row[c] == 'B') ? 0xFF000000u : 0xFFFFFFFFu;
            int sr = (c64_scrw - 1) - x;
            c64_u8 *p = origin + (c64_u64)sr * c64_scale * ppitch
                      + (c64_u64)y * c64_scale * 4;
            for (int rep = 0; rep < c64_scale; rep++) {
                c64_u32 *d = (c64_u32 *)p;
                for (int k = 0; k < c64_scale; k++)
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
 * becomes panel ROW (scrw-1-x), and source row y becomes panel COLUMN y, so a
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
static fb_px g_shadow[C64_UI_MAX_W * C64_UI_MAX_H];
static int g_shadow_valid;
static int g_conly;          /* set by uno_pc64_present_cursor() */
static int g_pcx = -1, g_pcy;                /* cursor rect last composited */
/* One output row = c64_scrh * c64_scale pixels, which every zoom clamps to
 * the panel's width, so the panel's width is the ceiling for all of them. */
static c64_u32 g_line[PANEL_W];              /* one output row, built once */

void uno_pc64_dirty_all(void)
{
    g_shadow_valid = 0;
}

/* ---- where the time actually goes ---------------------------------------- */
/* The first cut of this reported every 300 presents and printed NOTHING on a
 * real session, which was itself the finding: the shell only presents when
 * something changed, so present is not the hot path. The breakdown is now
 * driven from the poll loop (which runs every iteration) on a WALL-CLOCK
 * cadence, and splits the frame into the three things this port can see:
 * present, the input drivers' I2C, and everything else -- which is the
 * shell's own unoui_render_ui() software repaint. */
static c64_u64 g_t_present, g_t_poll, g_box_acc, g_t_report;
static unsigned g_presents, g_skipped, g_loops;
static unsigned g_perf_windows;

static void present_done(c64_u64 t0, c64_u64 area)
{
    g_t_present += c64_cnt_now() - t0;
    g_box_acc += area;
    g_presents++;
    if (!area)
        g_skipped++;
}

void c64_perf_add_poll(c64_u64 cyc)
{
    g_t_poll += cyc;
}

void c64_perf_loop(void)
{
    c64_u64 hz = c64_cnt_freq();
    if (!hz)
        hz = 13000000ull;
    c64_u64 now = c64_cnt_now();
    g_loops++;
    if (!g_t_report) {
        g_t_report = now;
        return;
    }
    c64_u64 span = now - g_t_report;
    if (span < hz * 2ull)                        /* measure every 2 seconds */
        return;
    /* Throttle the steady-state chatter so it does not bury the boot story:
     * the first 15 windows (~30 s) are logged verbatim -- that is bring-up and
     * the stretch worth watching live -- then one window in 15 (~every 30 s),
     * which still shows the frame rate drifting without filling the ring. A
     * boot's perf output drops from ~250 KB/hour to ~17 KB. The measurement
     * itself keeps running every window; only the logging is rationed. */
    g_perf_windows++;
    int say = g_perf_windows <= 15 || (g_perf_windows % 15) == 0;
#define US(c) ((int)((c) * 1000000ull / hz))
    if (say) {
        c64_u64 other = span > (g_t_present + g_t_poll)
                      ? span - g_t_present - g_t_poll : 0;
        c64_logf("perf: %d loops, %d presents (%d skipped) | present %d ms, "
                 "input %d ms, other %d ms of %d ms\n",
                 (int)g_loops, (int)g_presents, (int)g_skipped,
                 US(g_t_present) / 1000, US(g_t_poll) / 1000,
                 US(other) / 1000, US(span) / 1000);
        if (g_presents)
            c64_logf("perf: per present %d us, avg box %d px; per loop input "
                     "%d us\n", US(g_t_present) / (int)g_presents,
                     (int)(g_box_acc / g_presents),
                     g_loops ? US(g_t_poll) / (int)g_loops : 0);
    }
#undef US
    g_t_present = g_t_poll = g_box_acc = 0;
    g_presents = g_skipped = g_loops = 0;
    g_t_report = now;
}

void uno_pc64_present(void)
{
    c64_u64 t0 = c64_cnt_now();
    /* publish fb[] as the source surface for the harness's eye check (the
     * harness detects the R<->B swizzle by trying both channel orders) */
    FBDBG->fb_shadow = (c64_u64)fb;
    FBDBG->fb_base = (c64_u64)fb;
    FBDBG->fb_pitch = (c64_u32)c64_scrw * 4;
    /* g_shadow is, by construction, exactly what the panel holds (minus the
     * composited cursor): the compare below copies every changed pixel into
     * it and the box it pushes covers all of them. Publishing it lets the
     * gate keep its pixel-exact check even when a stop lands inside the
     * shell's render, when fb[] is half a frame. */
    FBDBG->fb_presented = (c64_u64)g_shadow;
    c64_u8 *origin = (c64_u8 *)FBDBG->fb_dorigin;
    c64_u32 ppitch = FBDBG->fb_ppitch;
    const int scrw = c64_scrw, scrh = c64_scrh, zoom = c64_scale;

    int x0 = scrw, x1 = -1, y0 = scrh, y1 = -1;
    if (g_conly && g_shadow_valid) {
        /* The shell promises fb[] is unchanged and only the pointer moved
         * (its cursor_only path), so the compare is pure waste -- 307k pixels
         * of it. x86 bounds the same pass to the cursor band for the same
         * reason. The cursor union below supplies the whole box. */
        g_conly = 0;
    } else if (!g_shadow_valid) {
        for (int i = 0; i < scrw * scrh; i++)
            g_shadow[i] = fb[i];
        g_shadow_valid = 1;
        x0 = 0; x1 = scrw - 1; y0 = 0; y1 = scrh - 1;
    } else {
        for (int y = 0; y < scrh; y++) {
            const fb_px *s = fb + (unsigned)y * scrw;
            fb_px *sh = g_shadow + (unsigned)y * scrw;
            int rx0 = -1, rx1 = -1;
            for (int x = 0; x < scrw; x++)
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
        if (ax1 > scrw - 1) ax1 = scrw - 1;
        if (ay1 > scrh - 1) ay1 = scrh - 1;
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
     * written `zoom` times: the strided source read is the expensive half,
     * so it happens once rather than per repeat, and both writes are
     * sequential, which is what non-cacheable memory wants. */
    for (int x = x1; x >= x0; x--) {
        const fb_px *s = fb + (unsigned)y0 * scrw + x;
        int n = 0;
        for (int y = y0; y <= y1; y++) {
            fb_px px = *s;
            c64_u32 out = 0xFF000000u | ((px & 0xFFu) << 16)
                        | (px & 0xFF00u) | ((px >> 16) & 0xFFu);
            for (int k = 0; k < zoom; k++)
                g_line[n++] = out;
            s += scrw;                            /* one source row down */
        }
        int sr = (scrw - 1) - x;
        c64_u8 *drow = origin + (c64_u64)sr * zoom * ppitch
                     + (c64_u64)y0 * zoom * 4;
        for (int rep = 0; rep < zoom; rep++) {
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
    g_conly = 1;
    uno_pc64_present();
}

static fb_px g_scene[C64_UI_MAX_W * C64_UI_MAX_H];

void uno_pc64_scene_save(void)
{
    for (int i = 0; i < c64_scrw * c64_scrh; i++)
        g_scene[i] = fb[i];
}

void uno_pc64_scene_restore(void)
{
    for (int i = 0; i < c64_scrw * c64_scrh; i++)
        fb[i] = g_scene[i];
    /* the whole surface just changed underneath the shadow */
    uno_pc64_dirty_all();
}

/* ---- the desktop size ---------------------------------------------------
 * The panel is fixed, so unlike x86 there are no video modes to enumerate --
 * but there is still a choice, because the desktop is presented at an integer
 * zoom and a smaller desktop simply means a bigger zoom over the same panel.
 * The list is therefore "how big do you want the UI", exactly the sense the
 * x86 port's list ended up having too.
 *
 * The four entries whose zoom divides the panel exactly -- 2160x1080,
 * 1080x540, 720x360, 540x270 -- fill it edge to edge; the familiar PC sizes
 * in between are centred with a black surround, which is what a fixed panel
 * can honestly do with them.
 *
 * 1080x540 AT ZOOM 2 IS THE DEFAULT (see cosmo64.h), not the native 2160x1080
 * above it: on a 5.99" 403 DPI panel a native desktop is beautiful and too
 * fine to read, and the shell's own "UI scale" preference cannot help because
 * it only scales a loaded TTF face and this device mounts no volume to load
 * one from. The zoom is the knob that works here. */
typedef struct { short w, h; } C64Res;
static const C64Res kRes[] = {
    {2160, 1080},        /* native, zoom 1 -- fills the panel exactly */
    {1920, 1080},        /* zoom 1, centred */
    {1440,  720},        /* zoom 1, centred */
    {1280,  720},        /* zoom 1, centred */
    {1080,  540},        /* zoom 2 -- fills the panel exactly */
    { 960,  540},        /* zoom 2, centred */
    { 800,  600},        /* zoom 1, centred */
    { 720,  360},        /* zoom 3 -- fills the panel exactly */
    { 640,  480},        /* zoom 2 -- what this port started at */
    { 540,  270}         /* zoom 4 -- fills the panel exactly */
};
#define NRES ((int)(sizeof kRes / sizeof kRes[0]))

/* Commit a desktop size: geometry, then the surface. The panel is cleared
 * because a SHRINKING rect leaves the old desktop standing in the margin --
 * nothing will ever draw over those pixels again -- and the shadow is dropped
 * because fb[]'s row stride is FB_W, so every row of it has just moved. */
static void apply_desktop(int w, int h)
{
    int ow = c64_scrw, oh = c64_scrh;
    c64_geom_set(w, h);
    uno_fb_w = c64_scrw;
    uno_fb_h = c64_scrh;
    if (ow != c64_scrw || oh != c64_scrh) {
        /* Carry the pointer by its POSITION ON THE PANEL, not its coordinate:
         * every size covers the same glass, so the same coordinate is a
         * different physical place either side of the change. (x86 learned
         * this as a pointer that appeared to vanish into the right edge.) */
        c64_input_rescale_pointer(ow, oh, c64_scrw, c64_scrh);
        c64_logf("display: desktop %dx%d zoom %d, rect %dx%d at %d,%d\n",
                 c64_scrw, c64_scrh, c64_scale, c64_dst_w, c64_dst_h,
                 c64_dst_x0, c64_dst_y0);
    }
    c64_fb_clear_panel();
    g_pcx = -1;                                   /* no stale cursor rect */
    uno_pc64_dirty_all();
}

/* Runner3D and any other full-screen 3D renders far fewer pixels at a small
 * desktop, and here the zoom that brings it back up to the panel is free --
 * the present writes the same number of panel bytes either way. */
static int gLowres, gSavedW, gSavedH;

void uno_pc64_lowres(int on)
{
    if (on && !gLowres) {
        gSavedW = c64_scrw;
        gSavedH = c64_scrh;
        gLowres = 1;
        apply_desktop(540, 270);                  /* zoom 4: 1/16 the pixels */
        uno_screen_changed();
    } else if (!on && gLowres) {
        gLowres = 0;
        apply_desktop(gSavedW, gSavedH);
        uno_screen_changed();
    }
}

int uno_pc64_res_count(void)
{
    return NRES;
}

void uno_pc64_res_get(int idx, short *w, short *h, short *zoom, Boolean *active)
{
    if (idx < 0 || idx >= NRES) {
        *w = *h = *zoom = 0;
        *active = 0;
        return;
    }
    *w = kRes[idx].w;
    *h = kRes[idx].h;
    /* The zoom this entry WOULD be presented at, computed the one way it is
     * computed anywhere -- reporting a stored number here is how a list ends
     * up disagreeing with the screen. */
    {
        int zx = PANEL_W / kRes[idx].h, zy = PANEL_H / kRes[idx].w;
        int z = zx < zy ? zx : zy;
        *zoom = (short)(z < 1 ? 1 : z);
    }
    *active = (Boolean)(c64_scrw == *w && c64_scrh == *h);
}

void uno_pc64_res_set(int idx)
{
    if (idx < 0 || idx >= NRES)
        return;
    if (c64_scrw == kRes[idx].w && c64_scrh == kRes[idx].h)
        return;                                   /* already active */
    apply_desktop(kRes[idx].w, kRes[idx].h);
    uno_screen_changed();            /* core: new gScreen + a full repaint */
}
