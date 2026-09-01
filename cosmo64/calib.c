/* cosmo64/calib.c -- the touch calibration payload.
 *
 *   ./build.sh calib   ->  build/pc64arm-boot.img
 *
 * The shell's cursor moves with a finger but does not track it, and no amount
 * of reasoning about the transform settles where the error is, because two
 * separate steps could be wrong: the controller's raw coordinates might not be
 * panel pixels, or the rotate-and-scale into the 640x480 UI might be off.
 *
 * So this payload measures instead of arguing, and it is DELIBERATELY not the
 * shell: it draws its targets straight onto the panel in RAW PANEL PIXELS and
 * reports the controller's RAW report, so the calibration path contains none
 * of the transform it exists to measure. Whatever comes out is ground truth.
 *
 * How to use it:
 *   1. flash it to p38 and boot UNODOS;
 *   2. a white crosshair appears somewhere on the screen, one at a time;
 *   3. put a finger exactly on the crosshair centre and, holding it there,
 *      press any key with the other hand;
 *   4. repeat for all five targets;
 *   5. the screen turns solid green when it is done. Reboot into trixie and
 *      run ./readlog.sh -- the log holds the five (target, raw) pairs and the
 *      residuals of the transform the shell currently uses.
 *
 * THE LIVE GREEN DOT IS THE FIRST ANSWER, before any key is pressed. It is
 * drawn at the raw report treated as a panel pixel, one to one. If it sits
 * under the finger, the controller already speaks panel pixels and the bug is
 * entirely in the rotation into UI space. If it does not, the raw coordinates
 * need scaling or an axis swap first, and the dot shows which. It is green so
 * that it stays visible against the red aim square exactly when it overlaps.
 */

#include "cosmo64.h"

int c64_touch_raw(int *x, int *y);
void c64_touch_maxima(int *mx, int *my);

/* ---- the seam the drivers expect ----------------------------------------- */
/* touch.c and kbd.c push into input.c in the shell build. This payload does
 * not link input.c (that would drag in pc64's headers), so it satisfies the
 * three symbols itself. */

static volatile int g_key_uni, g_key_seq;

void c64_key_push(int scan, int uni, int mods)
{
    (void)scan;
    (void)mods;
    g_key_uni = uni;
    g_key_seq++;
}

void c64_input_set_level(int mods, int held)
{
    (void)mods;
    (void)held;
}

void c64_input_set_pointer(int x, int y, int btn)
{
    (void)x;
    (void)y;
    (void)btn;
}

/* ---- painting, in raw panel pixels --------------------------------------- */

static c64_u8 *g_panel;
static c64_u32 g_pitch;

static void px(int x, int y, c64_u32 col)
{
    if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H)
        return;
    *(volatile c64_u32 *)(g_panel + (c64_u64)y * g_pitch + (c64_u64)x * 4) = col;
}

static void rect(int x, int y, int w, int h, c64_u32 col)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            px(x + c, y + r, col);
}

static void fill(c64_u32 col)
{
    for (int y = 0; y < PANEL_H; y++)
        for (int x = 0; x < PANEL_W; x++)
            px(x, y, col);
}

#define ARM 90
#define THICK 6
#define COL_BG   0xFF101018u
#define COL_MARK 0xFFFFFFFFu
#define COL_HOT  0xFFFF3030u
#define COL_DONE 0xFF10A010u
#define COL_LIVE 0xFF30FF30u    /* the finger: GREEN, so it stays
                                 * distinguishable from the red aim
                                 * square exactly when it overlaps it */

static void crosshair(int cx, int cy, c64_u32 col)
{
    rect(cx - ARM, cy - THICK / 2, ARM * 2, THICK, col);
    rect(cx - THICK / 2, cy - ARM, THICK, ARM * 2, col);
    rect(cx - 9, cy - 9, 18, 18, COL_HOT);      /* aim HERE */
    rect(cx - 3, cy - 3, 6, 6, col);
}

/* Five targets, well spread and away from the edges so a fingertip can sit on
 * each one squarely. Panel is 1080 x 2160. */
static const struct { int x, y; } kTargets[] = {
    { 216,  432 }, { 864,  432 }, { 540, 1080 }, { 216, 1728 }, { 864, 1728 },
};
#define NTARGETS ((int)(sizeof kTargets / sizeof kTargets[0]))

static int cap_x[NTARGETS], cap_y[NTARGETS], cap_ok[NTARGETS];

/* progress: one filled square per target, solid once captured */
static void progress(int done)
{
    for (int i = 0; i < NTARGETS; i++)
        rect(30 + i * 46, 30, 34, 34,
             i < done ? COL_MARK : 0xFF404050u);
}

static void spin_ms(int ms)
{
    c64_u64 f = c64_cnt_freq();
    if (!f)
        f = 13000000ull;
    c64_u64 until = c64_cnt_now() + (f / 1000ull) * (c64_u64)ms;
    while (c64_cnt_now() < until)
        __asm__ volatile("yield");
}

/* ---- the report ---------------------------------------------------------- */
/* The shell's present maps a panel point to the UI like this (display.c and
 * touch.c, FB_SCALE=2, 270 degrees): ux = 639 - (py - 440)/2, uy = (px - 60)/2.
 * Printing what that WOULD have produced next to the target the finger was
 * actually on turns the log into the answer rather than raw material. */
static void report(void)
{
    int mx, my;
    c64_touch_maxima(&mx, &my);
    c64_logf("\ncalib: controller maxima %dx%d, panel %dx%d\n",
             mx, my, PANEL_W, PANEL_H);
    c64_log("calib:  target(panel)      raw report      delta      "
            "current-transform UI\n");
    for (int i = 0; i < NTARGETS; i++) {
        if (!cap_ok[i]) {
            c64_logf("calib:  %4d,%4d        NO CONTACT\n",
                     kTargets[i].x, kTargets[i].y);
            continue;
        }
        int dx = cap_x[i] - kTargets[i].x;
        int dy = cap_y[i] - kTargets[i].y;
        /* what the shell would have computed from this raw report */
        int ux = (C64_SCRW - 1) - (cap_y[i] - C64_DST_Y0) / FB_SCALE;
        int uy = (cap_x[i] - C64_DST_X0) / FB_SCALE;
        /* and where the target actually is in UI space */
        int tux = (C64_SCRW - 1) - (kTargets[i].y - C64_DST_Y0) / FB_SCALE;
        int tuy = (kTargets[i].x - C64_DST_X0) / FB_SCALE;
        c64_logf("calib:  %4d,%4d      %4d,%4d    %4d,%4d    ui %3d,%3d "
                 "(target ui %3d,%3d)\n",
                 kTargets[i].x, kTargets[i].y, cap_x[i], cap_y[i], dx, dy,
                 ux, uy, tux, tuy);
    }
    c64_log("calib: done. Reboot into trixie and run ./readlog.sh\n");
}

/* ---- boot ---------------------------------------------------------------- */

void c_main(void *dtb)
{
    c64_beacon(224, 0xFFFF00FFu);
    c64_log_survey();
    c64_log_init();
    c64_log("calib payload: touch calibration\n");
    mmu_init();
    c64_log_survey_report();

    c64_u32 ppitch;
    c64_u64 raw = c64_fb_adopt(dtb, &ppitch);
    g_panel = (c64_u8 *)raw;
    g_pitch = ppitch;
    c64_logf("calib: panel %016x pitch %d\n", raw, (int)ppitch);

    c64_blk_init();
    c64_kbd_init();
    c64_touch_init();
    c64_logf("calib: keyboard %s, touch %s\n",
             c64_kbd_present() ? "present" : "ABSENT",
             c64_touch_present() ? "present" : "ABSENT");
    c64_log_flush();

    int i = 0, last_seq = g_key_seq;
    int lx = -1, ly = -1;
    fill(COL_BG);

    while (i < NTARGETS) {
        c64_kbd_poll();
        c64_touch_poll();

        int tx, ty;
        int down = c64_touch_raw(&tx, &ty);

        /* repaint only what moved: erase the old live dot, redraw target */
        if (lx >= 0)
            rect(lx - 7, ly - 7, 14, 14, COL_BG);
        crosshair(kTargets[i].x, kTargets[i].y, COL_MARK);
        progress(i);
        if (down) {
            /* the raw report treated as a panel pixel, one to one: if this
             * lands under the finger the controller speaks panel pixels */
            rect(tx - 7, ty - 7, 14, 14, COL_LIVE);
            lx = tx;
            ly = ty;
        } else {
            lx = -1;
        }
        __asm__ volatile("dsb sy" ::: "memory");

        if (g_key_seq != last_seq) {
            last_seq = g_key_seq;
            cap_ok[i] = down;
            cap_x[i] = tx;
            cap_y[i] = ty;
            if (down)
                c64_logf("calib: target %d (%d,%d) captured raw %d,%d "
                         "(key %04x)\n", i, kTargets[i].x, kTargets[i].y,
                         tx, ty, g_key_uni);
            else
                c64_logf("calib: target %d (%d,%d) -- key pressed with NO "
                         "finger down, recorded as a miss\n", i,
                         kTargets[i].x, kTargets[i].y);
            c64_log_flush();
            fill(COL_BG);                       /* clean slate for the next */
            lx = -1;
            i++;
            spin_ms(250);                       /* debounce the key */
        }
        spin_ms(16);
    }

    report();
    c64_log_flush();
    fill(COL_DONE);
    for (;;)
        __asm__ volatile("wfe");
}
