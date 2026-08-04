/* ===========================================================================
 * unoui_anim host harness - the contract test AND the picture.
 *
 * Part 1 is assertions against a DETERMINISTIC clock: the harness supplies the
 * milliseconds itself, so every property below (endpoint exactness, delays,
 * looping, stale-handle refusal, sequence ordering, the click that skips a
 * build) is checked at exact times rather than at whatever the host happened to
 * schedule. It exits non-zero on the first broken property, so ./build.sh
 * fails loudly.
 *
 * Part 2 renders what those numbers look like: the ten easing curves, and a
 * slide whose three paragraphs fly in one click at a time while the previous
 * one dims - the worked example from the request that asked for this.
 *
 *   ./host_unoui_anim <out_dir>
 * ======================================================================== */
#include "unoui_theme.h"
#include "unoui_anim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ checks ------ */

static int g_fail = 0, g_checks = 0;

static void check(int ok, const char *what)
{
    g_checks++;
    if (!ok) { printf("  FAIL: %s\n", what); g_fail++; }
}

static void check_eq(int got, int want, const char *what)
{
    g_checks++;
    if (got != want) { printf("  FAIL: %s (got %d, want %d)\n", what, got, want); g_fail++; }
}

/* ------------------------------------------------------------ part 1 ------ */

static const char *EASE_NAME[UI_EASE_N] = {
    "linear", "in", "out", "in-out", "in-cubic", "out-cubic", "in-out-cubic",
    "out-back", "out-bounce", "step"
};

static void test_curves(void)
{
    int e, t, prev;

    for (e = 0; e < UI_EASE_N; e++) {
        char msg[96];
        sprintf(msg, "%s: starts at 0", EASE_NAME[e]);
        check_eq(unoui_ease(e, 0), 0, msg);
        sprintf(msg, "%s: ends at ONE", EASE_NAME[e]);
        check_eq(unoui_ease(e, UI_ANIM_ONE), UI_ANIM_ONE, msg);
        sprintf(msg, "%s: clamps past the ends", EASE_NAME[e]);
        check(unoui_ease(e, -500) == 0 && unoui_ease(e, UI_ANIM_ONE + 500) == UI_ANIM_ONE, msg);
    }

    /* the non-overshoot curves never go backwards, and never leave 0..ONE */
    for (e = 0; e <= UI_EASE_INOUT_CUBIC; e++) {
        char msg[96];
        int ok = 1;
        prev = 0;
        for (t = 0; t <= UI_ANIM_ONE; t += 16) {
            int v = unoui_ease(e, t);
            if (v < prev || v < 0 || v > UI_ANIM_ONE) { ok = 0; break; }
            prev = v;
        }
        sprintf(msg, "%s: monotonic inside 0..ONE", EASE_NAME[e]);
        check(ok, msg);
    }

    /* the overshoot curves are supposed to leave the range - if one of them
     * stopped doing so, it has quietly become a different curve */
    {
        int hi = 0, lo = UI_ANIM_ONE;
        for (t = 0; t <= UI_ANIM_ONE; t += 8) {
            int v = unoui_ease(UI_EASE_OUT_BACK, t);
            if (v > hi) hi = v;
            if (v < lo) lo = v;
        }
        check(hi > UI_ANIM_ONE, "out-back: overshoots past the target");
    }

    /* symmetry: an in-out curve passes through the middle at the middle */
    check_eq(unoui_ease(UI_EASE_INOUT, UI_ANIM_ONE / 2), UI_ANIM_ONE / 2,
             "in-out: half way at half time");
    check_eq(unoui_ease(UI_EASE_INOUT_CUBIC, UI_ANIM_ONE / 2), UI_ANIM_ONE / 2,
             "in-out-cubic: half way at half time");

    /* step holds and then snaps - what a Dissolve or an Appear build needs */
    check_eq(unoui_ease(UI_EASE_STEP, UI_ANIM_ONE - 1), 0, "step: holds until the end");

    /* the lerp survives a delta big enough to overflow a naive multiply */
    check_eq(unoui_anim_lerp(0, 1000000, UI_ANIM_ONE), 1000000, "lerp: big delta, full progress");
    check_eq(unoui_anim_lerp(0, 1000000, UI_ANIM_ONE / 2), 500000, "lerp: big delta, half way");
    check_eq(unoui_anim_lerp(200, 100, UI_ANIM_ONE), 100, "lerp: backwards delta");
}

static void test_tweens(void)
{
    unoui_anim ac;
    unoui_anim_h h, h2;
    unoui_tween tw;
    int out = -1, i;

    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 1000);              /* the clock starts at 1 s        */

    h = unoui_tween_to(&ac, 0, 240, 400, UI_EASE_LINEAR, &out);
    check(h != 0, "tween: start returns a handle");
    check_eq(unoui_anim_value(&ac, h), 0, "tween: sits at `from` before time passes");
    check_eq(out, 0, "tween: writes the out pointer immediately");

    unoui_anim_tick(&ac, 1200);
    check_eq(unoui_anim_value(&ac, h), 120, "tween: linear half way at half time");
    check_eq(out, 120, "tween: out pointer follows");
    check(!unoui_anim_done(&ac, h), "tween: not done half way");
    check_eq(unoui_anim_active(&ac), 1, "tween: one animation is active");

    unoui_anim_tick(&ac, 1400);
    check_eq(unoui_anim_value(&ac, h), 240, "tween: lands exactly on `to`");
    check(unoui_anim_done(&ac, h), "tween: done at the duration");
    check_eq(unoui_anim_active(&ac), 0, "tween: nothing active once it lands");

    /* a curve that overshoots still lands exactly, at the exact time */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    h = unoui_tween_to(&ac, 0, 100, 200, UI_EASE_OUT_BACK, 0);
    unoui_anim_tick(&ac, 150);
    check(unoui_anim_value(&ac, h) > 100, "out-back tween: overshoots mid-flight");
    unoui_anim_tick(&ac, 200);
    check_eq(unoui_anim_value(&ac, h), 100, "out-back tween: settles exactly on `to`");

    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    h = unoui_tween_to(&ac, 0, 100, 300, UI_EASE_OUT_BOUNCE, 0);
    unoui_anim_tick(&ac, 300);
    check_eq(unoui_anim_value(&ac, h), 100, "bounce tween: settles exactly on `to`");

    /* delay: hold at `from`, then run the full duration */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    memset(&tw, 0, sizeof tw);
    tw.from = 0; tw.to = 100; tw.dur_ms = 100; tw.delay_ms = 200;
    tw.ease = UI_EASE_LINEAR;
    h = unoui_tween_start(&ac, &tw);
    unoui_anim_tick(&ac, 150);
    check_eq(unoui_anim_value(&ac, h), 0, "delay: still at `from` inside the delay");
    unoui_anim_tick(&ac, 250);
    check_eq(unoui_anim_value(&ac, h), 50, "delay: half way after delay + half duration");
    unoui_anim_tick(&ac, 300);
    check(unoui_anim_done(&ac, h), "delay: done at delay + duration");

    /* a zero-duration tween is an instant set, not a divide by zero */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    h = unoui_tween_to(&ac, 5, 55, 0, UI_EASE_OUT, 0);
    check_eq(unoui_anim_value(&ac, h), 55, "zero duration: arrives immediately");
    check(unoui_anim_done(&ac, h), "zero duration: already done");

    /* looping and ping-pong never finish - a shell repainting on
     * unoui_anim_active must keep repainting for them */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    memset(&tw, 0, sizeof tw);
    tw.from = 0; tw.to = 100; tw.dur_ms = 100; tw.loop = UI_ANIM_LOOP;
    h = unoui_tween_start(&ac, &tw);
    unoui_anim_tick(&ac, 250);
    check_eq(unoui_anim_value(&ac, h), 50, "loop: wraps back around");
    check(!unoui_anim_done(&ac, h), "loop: never finishes");
    tw.loop = UI_ANIM_PINGPONG;
    h2 = unoui_tween_start(&ac, &tw);
    unoui_anim_tick(&ac, 300);              /* h2 started at 250, so e = 50    */
    check_eq(unoui_anim_value(&ac, h2), 50, "pingpong: out on the first half");
    unoui_anim_tick(&ac, 400);              /* e = 150 -> coming back          */
    check_eq(unoui_anim_value(&ac, h2), 50, "pingpong: back on the second half");
    check_eq(unoui_anim_active(&ac), 2, "loops stay active forever");

    /* cancel leaves the value where it stood; finish jumps it to the target */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    h = unoui_tween_to(&ac, 0, 100, 100, UI_EASE_LINEAR, 0);
    unoui_anim_tick(&ac, 40);
    unoui_anim_cancel(&ac, h);
    unoui_anim_tick(&ac, 90);
    check_eq(unoui_anim_value(&ac, h), 40, "cancel: stops where it stood");
    check(unoui_anim_done(&ac, h), "cancel: reads as done");
    h2 = unoui_tween_to(&ac, 0, 100, 100, UI_EASE_LINEAR, &out);
    unoui_anim_finish(&ac, h2);
    check_eq(unoui_anim_value(&ac, h2), 100, "finish: jumps to the target");
    check_eq(out, 100, "finish: writes the out pointer too");

    /* THE STALE-HANDLE PROPERTY. A handle to a recycled slot must be refused,
     * not silently read as whoever moved in - the same reason unoui_mdi stores
     * index+1 rather than a bare index. */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    h = unoui_tween_to(&ac, 0, 10, 100, UI_EASE_LINEAR, 0);
    unoui_anim_free(&ac, h);
    h2 = unoui_tween_to(&ac, 500, 900, 100, UI_EASE_LINEAR, 0);
    check(h != h2, "recycled slot: the new handle differs");
    check_eq(unoui_anim_value(&ac, h), 0, "stale handle: reads 0, not the new tween");
    check(unoui_anim_done(&ac, h), "stale handle: reads as done, so no one waits forever");
    check(!unoui_anim_live(&ac, h), "stale handle: not live");
    check_eq(unoui_anim_value(&ac, h2), 500, "recycled slot: the new tween is intact");

    /* a full pool refuses rather than corrupting; finished tweens go first */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    for (i = 0; i < UNOUI_ANIM_MAX; i++)
        check(unoui_tween_to(&ac, 0, 1, 1000, UI_EASE_LINEAR, 0) != 0,
              i == 0 ? "pool: fills up" : "");
    g_checks -= (UNOUI_ANIM_MAX - 1);        /* one property, not 48           */
    check_eq(unoui_tween_to(&ac, 0, 1, 1000, UI_EASE_LINEAR, 0), 0,
             "pool: a full pool returns 0, not a bad handle");
    check_eq(unoui_anim_value(&ac, 0), 0, "handle 0: value is 0");
    check(unoui_anim_done(&ac, 0), "handle 0: reads as done");

    /* FIRST-TICK REBASE. A shell builds its UI - and can start an animation -
     * before the frame loop reads the clock for the first time. Without the
     * rebase that tween sees a jump of however long the machine has been up
     * and completes on frame one. */
    unoui_anim_init(&ac);
    h = unoui_tween_to(&ac, 0, 100, 400, UI_EASE_LINEAR, 0);
    unoui_anim_tick(&ac, 9000000);           /* the machine was up 2.5 hours   */
    check(!unoui_anim_done(&ac, h), "first tick: an early tween is rebased, not skipped");
    unoui_anim_tick(&ac, 9000200);
    check_eq(unoui_anim_value(&ac, h), 50, "first tick: and then runs normally");

    /* the millisecond clock rolls over every 49 days; nothing may notice */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0xFFFFFF00u);
    h = unoui_tween_to(&ac, 0, 100, 400, UI_EASE_LINEAR, 0);
    unoui_anim_tick(&ac, 0xFFFFFF00u + 200u);   /* wraps through zero          */
    check_eq(unoui_anim_value(&ac, h), 50, "clock wraparound: half way is still half way");
    unoui_anim_tick(&ac, 0xFFFFFF00u + 400u);
    check(unoui_anim_done(&ac, h), "clock wraparound: and it still finishes");
}

/* the fallback clock, exercised through the hook seam */
static unsigned g_fake_ms = 0;
static unsigned fake_clock(void) { return g_fake_ms; }

static void test_clock_seam(void)
{
    unoui_anim ac;
    unoui_anim_h h;
    int i;

    /* no hook: unoui_anim_frame counts frames at UNOUI_TICK_MS each */
    unoui_clock_ms = 0;
    unoui_anim_init(&ac);
    unoui_anim_frame(&ac);
    h = unoui_tween_to(&ac, 0, 100, UNOUI_TICK_MS * 10, UI_EASE_LINEAR, 0);
    for (i = 0; i < 5; i++) unoui_anim_frame(&ac);
    check_eq(unoui_anim_value(&ac, h), 50, "no clock: the frame fallback still tweens");

    /* with a hook: the platform's milliseconds win, and a frame that took
     * three times as long moves the animation three times as far - the whole
     * point of the seam */
    unoui_clock_ms = fake_clock;
    g_fake_ms = 5000;
    unoui_anim_init(&ac);
    unoui_anim_frame(&ac);
    h = unoui_tween_to(&ac, 0, 100, 100, UI_EASE_LINEAR, 0);
    g_fake_ms = 5060;                        /* one very slow frame            */
    unoui_anim_frame(&ac);
    check_eq(unoui_anim_value(&ac, h), 60, "clock hook: a slow frame advances further");
    check_eq((int)unoui_anim_now(&ac), 5060, "clock hook: the context reports its time");
    unoui_clock_ms = 0;
}

static void test_sequences(void)
{
    unoui_anim ac;
    unoui_seq  s;
    unoui_tween tw;
    int a = 0, b = 0, c = 0, dim = 255;

    memset(&tw, 0, sizeof tw);
    tw.dur_ms = 100; tw.ease = UI_EASE_LINEAR;

    /* AFTER: three steps, strictly one after another */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    unoui_seq_init(&s);
    tw.from = 0; tw.to = 10; tw.out = &a; unoui_seq_add(&s, UI_STEP_AFTER, &tw);
    tw.from = 0; tw.to = 20; tw.out = &b; unoui_seq_add(&s, UI_STEP_AFTER, &tw);
    tw.from = 0; tw.to = 30; tw.out = &c; unoui_seq_add(&s, UI_STEP_AFTER, &tw);
    check(unoui_seq_start(&ac, &s) != 0, "sequence: registers with a context");

    unoui_anim_tick(&ac, 50);
    check(a == 5 && b == 0 && c == 0, "sequence: only the first step is moving");
    /* Each tick jumps 100 ms, so the step due at 100 ms is only noticed at 150.
     * It must nevertheless be 50 ms into its own run, not 0: a sequence keeps
     * to its schedule instead of drifting a frame per step. */
    unoui_anim_tick(&ac, 150);
    check(a == 10 && b == 10 && c == 0, "sequence: the second follows the first, on schedule");
    unoui_anim_tick(&ac, 250);
    check(a == 10 && b == 20 && c == 15, "sequence: and the third follows the second");
    unoui_anim_tick(&ac, 350);
    check(c == 30, "sequence: the last step lands");
    check(unoui_seq_done(&s), "sequence: reports done when the last step lands");

    /* WITH: the request's actual case - a paragraph flies in WHILE the
     * previous one is still dimming */
    a = b = 0; dim = 255;
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    unoui_seq_init(&s);
    tw.from = 0; tw.to = 10; tw.out = &a; unoui_seq_add(&s, UI_STEP_AFTER, &tw);
    tw.from = 0; tw.to = 20; tw.out = &b; unoui_seq_add(&s, UI_STEP_AFTER, &tw);
    tw.from = 255; tw.to = 55; tw.out = &dim; unoui_seq_add(&s, UI_STEP_WITH, &tw);
    unoui_seq_start(&ac, &s);
    unoui_anim_tick(&ac, 150);
    check(b == 10 && dim == 155, "WITH: the dim runs alongside the next fly-in");
    check_eq(unoui_anim_active(&ac), 2, "WITH: two tweens in flight at once");

    /* ON_TRIGGER: parked until the click, and not a moment before */
    a = b = 0;
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    unoui_seq_init(&s);
    tw.out = &a; tw.from = 0; tw.to = 10; unoui_seq_add(&s, UI_STEP_AFTER, &tw);
    tw.out = &b; tw.from = 0; tw.to = 20; unoui_seq_add(&s, UI_STEP_ON_TRIGGER, &tw);
    unoui_seq_start(&ac, &s);
    unoui_anim_tick(&ac, 100);
    check(unoui_seq_waiting(&s), "trigger: parks after the first step");
    unoui_anim_tick(&ac, 5000);
    check(b == 0 && unoui_seq_waiting(&s), "trigger: waits indefinitely, no timeout");
    unoui_seq_trigger(&s);
    unoui_anim_tick(&ac, 5050);
    check(!unoui_seq_waiting(&s), "trigger: the click releases it");
    unoui_anim_tick(&ac, 5100);
    check_eq(b, 10, "trigger: and the next step runs from the click, not from 0");

    /* a trigger DURING a build skips it: one click finishes what is on screen,
     * the next click starts the following build */
    a = b = 0;
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    unoui_seq_init(&s);
    tw.out = &a; tw.from = 0; tw.to = 10; unoui_seq_add(&s, UI_STEP_AFTER, &tw);
    tw.out = &b; tw.from = 0; tw.to = 20; unoui_seq_add(&s, UI_STEP_ON_TRIGGER, &tw);
    unoui_seq_start(&ac, &s);
    unoui_anim_tick(&ac, 30);
    check(a == 3, "skip: the build is part way through");
    unoui_seq_trigger(&s);
    unoui_anim_tick(&ac, 40);
    check_eq(a, 10, "skip: the click snaps the running build to its end");
    check(unoui_seq_waiting(&s), "skip: and parks on the next trigger step");

    /* stopping cancels what is in flight and hands the storage back */
    unoui_anim_init(&ac);
    unoui_anim_tick(&ac, 0);
    unoui_seq_init(&s);
    tw.out = &a; tw.from = 0; tw.to = 100; unoui_seq_add(&s, UI_STEP_AFTER, &tw);
    unoui_seq_start(&ac, &s);
    unoui_anim_tick(&ac, 20);
    unoui_seq_stop(&ac, &s);
    unoui_anim_tick(&ac, 90);
    check_eq(a, 20, "stop: the in-flight tween stopped where it stood");
    check(!unoui_seq_done(&s), "stop: a stopped sequence is idle, not done");

    /* an empty sequence is done, not a hang */
    unoui_seq_init(&s);
    unoui_seq_start(&ac, &s);
    check(unoui_seq_done(&s), "empty sequence: done immediately");
}

/* ------------------------------------------------------------ part 2 ------ */

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb"); int i, n = FB_W * FB_H;
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < n; i++) { unsigned p = fb[i];
        unsigned char rgb[3] = { p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF };
        fwrite(rgb, 1, 3, f); }
    fclose(f);
}

/* the ten curves, plotted from unoui_ease itself */
static void render_curves(const char *dir)
{
    const unoui_theme *th = &theme_unodos;
    char path[256];
    int i;
    const int COLS = 5, CW = 124, CH = 132, X0 = 14, Y0 = 40;

    fb_clear(th->pal.desktop);
    fb_fill_rect(0, 0, FB_W, 26, th->pal.title_bg);
    fb_text(12, 9, "unoui_ease - every curve, drawn by the code under test",
            th->pal.title_fg, -1);

    for (i = 0; i < UI_EASE_N; i++) {
        int cx = X0 + (i % COLS) * CW, cy = Y0 + (i / COLS) * CH;
        int pw = CW - 24, ph = CH - 46, t, px = -1, py = -1;

        fb_fill_rect(cx, cy, pw + 2, ph + 2, th->pal.field_bg);
        fb_frame_rect(cx - 1, cy - 1, pw + 4, ph + 4, th->pal.dark);
        fb_text(cx, cy + ph + 10, EASE_NAME[i], FB_RGB(0xFF, 0xFF, 0xFF), -1);

        /* the 0..ONE band, so overshoot is visibly outside it */
        fb_hline(cx, cy + ph - ph / 6, pw, th->pal.shadow);
        fb_hline(cx, cy + ph / 6, pw, th->pal.shadow);

        for (t = 0; t <= pw; t++) {
            int e = unoui_ease(i, t * UI_ANIM_ONE / (pw ? pw : 1));
            int y = cy + ph - ph / 6 - (e * (ph - ph / 3)) / UI_ANIM_ONE;
            int x = cx + t;
            if (y < cy) y = cy;
            if (y > cy + ph) y = cy + ph;
            if (px >= 0) {                    /* join the samples up           */
                int a = py < y ? py : y, b = py < y ? y : py, k;
                for (k = a; k <= b; k++) ui_px(x, k, th->pal.accent);
            }
            ui_px(x, y, th->pal.accent);
            px = x; py = y;
        }
    }
    sprintf(path, "%s/an_curves.ppm", dir);
    write_ppm(path);
}

/* ---- the worked example: a slide that builds one click at a time ---------- */

#define NPAR 3
static struct { int x, dim; const char *text; } g_par[NPAR];
static unoui_ui   UI;
static unoui_window SLIDE;
static unoui_anim   AC;
static unoui_seq    SEQ;
static int g_frame = 0;

/* blend the theme's text colour toward the background by `dim` (255 = full) */
static fb_px dimmed(const unoui_theme *th, int dim)
{
    unsigned fg = th->pal.text, bg = th->pal.win_bg, o = 0xFF000000u;
    int k;
    if (dim > 255) dim = 255;
    if (dim < 0) dim = 0;
    for (k = 0; k < 24; k += 8) {
        unsigned a = (fg >> k) & 0xFF, b = (bg >> k) & 0xFF;
        o |= (unsigned)((b + (int)(a - b) * dim / 255) & 0xFF) << k;
    }
    return (fb_px)o;
}

static void slide_draw(unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *th = UI.theme;
    int i;
    (void)w; (void)ctx;
    fb_fill_rect(r.x, r.y, r.w, r.h, th->pal.win_bg);
    fb_text(r.x + 20, r.y + 14, "Quarterly Review", th->pal.text, -1);
    fb_hline(r.x + 20, r.y + 26, r.w - 40, th->pal.accent);
    for (i = 0; i < NPAR; i++) {
        int y = r.y + 48 + i * 24, x = r.x + g_par[i].x, skip = 0;
        fb_px c = dimmed(th, g_par[i].dim);
        const char *s = g_par[i].text;
        /* A paragraph that has not been built yet is parked off the LEFT of the
         * slide and slides in, so this canvas deliberately draws outside its own
         * rect - and on this host the clip does not catch it. unoui does call
         * fb_set_clip around a canvas painter, but the host framebuffer
         * (ps2/fb.c) only honours that clip in the fb_aa.c primitives: its
         * fill and text clip to the SCREEN. pc64/fb.c honours it everywhere, so
         * a real app is fine; the request is filed. Meanwhile this drops the
         * characters that are left of the slide, at 8 px (one glyph) of
         * granularity. */
        if (x < r.x) { skip = (r.x - x + 7) / 8; x += skip * 8; }
        if (skip >= (int)strlen(s)) continue;
        if (!skip) fb_fill_rect(x - 12, y + 2, 4, 4, c);
        fb_text(x, y, s + skip, c, -1);
    }
}

static unoui_canvas SLIDE_CANVAS = { slide_draw, 0, 0 };

static void build_slide(void)
{
    unoui_tween tw;
    int i;

    for (i = 0; i < NPAR; i++) { g_par[i].x = -400; g_par[i].dim = 255; }
    g_par[0].text = "Revenue up 12% on the quarter";
    g_par[1].text = "Two new ports shipped";
    g_par[2].text = "Headcount flat";

    unoui_window_init(&SLIDE, "UnoShow - build demo", 90, 70, 460, 200);
    unoui_add_canvas(&SLIDE, 0, 0, 452, 168, &SLIDE_CANVAS);
    unoui_ui_add(&UI, &SLIDE);

    /* Exactly the shape the request described: each paragraph flies in on a
     * click, and the one before it dims WITH that fly-in. The tweens write
     * straight into the drawing state - no polling, no per-app counter. */
    unoui_seq_init(&SEQ);
    memset(&tw, 0, sizeof tw);
    tw.ease = UI_EASE_OUT_CUBIC; tw.dur_ms = 500;

    tw.from = -300; tw.to = 24; tw.out = &g_par[0].x;
    unoui_seq_add(&SEQ, UI_STEP_AFTER, &tw);

    for (i = 1; i < NPAR; i++) {
        tw.ease = UI_EASE_OUT_CUBIC; tw.dur_ms = 500;
        tw.from = -300; tw.to = 24; tw.out = &g_par[i].x;
        unoui_seq_add(&SEQ, UI_STEP_ON_TRIGGER, &tw);
        tw.ease = UI_EASE_LINEAR; tw.dur_ms = 400;
        tw.from = 255; tw.to = 90; tw.out = &g_par[i - 1].dim;
        unoui_seq_add(&SEQ, UI_STEP_WITH, &tw);
    }
    unoui_seq_start(&AC, &SEQ);
}

static void snap(const char *dir, const char *label)
{
    char path[256], cap[128];
    UI.ticks = 0;
    unoui_render_ui(&UI);
    fb_fill_rect(0, 0, FB_W, 13, FB_RGB(0x10, 0x10, 0x10));
    fb_hline(0, 13, FB_W, FB_RGB(0x80, 0x80, 0x80));
    sprintf(cap, "%d. t=%4u ms  active=%d  %s", g_frame + 1,
            unoui_anim_now(&AC), unoui_anim_active(&AC), label);
    fb_text(6, 3, cap, FB_RGB(0xFF, 0xFF, 0xFF), -1);
    sprintf(path, "%s/an_%02d.ppm", dir, g_frame);
    write_ppm(path);
    printf("frame %d: %s\n", g_frame + 1, label);
    g_frame++;
}

/* advance the deterministic clock to `to_ms` in 20 ms frames */
static unsigned g_ms = 0;
static void run_to(unsigned to_ms)
{
    while (g_ms < to_ms) { g_ms += 20; unoui_anim_tick(&AC, g_ms); }
}

static void render_build(const char *dir)
{
    unoui_ui_init(&UI, &theme_unodos, FB_W, FB_H);
    unoui_anim_init(&AC);
    unoui_anim_tick(&AC, 0);
    build_slide();

    run_to(80);   snap(dir, "paragraph 1 flying in from off-slide");
    run_to(240);  snap(dir, "eased out-cubic: most of the way, slowing");
    run_to(520);  snap(dir, "landed, parked on the click");
    unoui_seq_trigger(&SEQ);
    run_to(660);  snap(dir, "click: 2 flies in WHILE 1 dims");
    run_to(1100); snap(dir, "both settled, parked again");
    unoui_seq_trigger(&SEQ);
    run_to(1260); snap(dir, "click: 3 flies in, 2 dimming");
    unoui_seq_trigger(&SEQ);                 /* impatient: skip the build      */
    run_to(1320); snap(dir, "a second click snaps the build to its end");
    run_to(1420); snap(dir, "sequence done");
    if (!unoui_seq_done(&SEQ)) {
        printf("  FAIL: storyboard: the sequence did not finish\n");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "build";

    printf("unoui_anim contract:\n");
    test_curves();
    test_tweens();
    test_clock_seam();
    test_sequences();
    printf("  %d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) return 1;

    render_curves(dir);
    render_build(dir);
    return 0;
}
