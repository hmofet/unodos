/* ===========================================================================
 * unoui_anim - the shared tween clock. See unoui_anim.h for the contract.
 *
 * Integer only, no fb.h, no libm: an animation here is a number moving from A
 * to B, and nothing in this file knows what the number is for.
 * ===========================================================================
 */
#include "unoui_anim.h"

#define ONE UI_ANIM_ONE

/* ---------------------------------------------------------------- clock --- */

unsigned (*unoui_clock_ms)(void) = 0;

/* "now is at or past t", written so a 32-bit millisecond counter rolling over
 * is a non-event: the difference of two unsigneds is correct across the wrap,
 * and reading it as signed asks which side of the wrap we are on. */
static int reached(unsigned now, unsigned t) { return (int)(now - t) >= 0; }

/* ---------------------------------------------------------------- easing --- */

/* a * b in 12-bit fixed point, rounded, sign-symmetric. Every product below is
 * bounded by the curve constants (worst case 7.5625 * ONE * ONE = 1.3e8), so
 * this stays inside 32 bits without a wider intermediate. */
static int fmul(int a, int b)
{
    int p = a * b;
    return p >= 0 ? (p + ONE / 2) / ONE : -((-p + ONE / 2) / ONE);
}

static int ease_back_out(int t)
{
    /* the standard 1.70158 / 2.70158 pair, in fixed point */
    int u = t - ONE, u2, u3;
    u2 = fmul(u, u); u3 = fmul(u2, u);
    return ONE + fmul(11065, u3) + fmul(6969, u2);
}

static int ease_bounce_out(int t)
{
    /* n1 = 7.5625, d1 = 2.75; the four arcs and their landing heights */
    int x;
    if (t < 1489) { return fmul(30976, fmul(t, t)); }
    if (t < 2979) { x = t - 2234; return fmul(30976, fmul(x, x)) + 3072; }
    if (t < 3724) { x = t - 3351; return fmul(30976, fmul(x, x)) + 3840; }
    x = t - 3910;  return fmul(30976, fmul(x, x)) + 4032;
}

/* A decaying oscillation about zero: four swings, each smaller than the last,
 * arriving at exactly zero. Done from a small table rather than a sine, because
 * this file has no floating point and no trig by design, and because the SHAPE
 * is the thing being specified - a shake that is felt rather than watched. The
 * table is the peak of each half-swing; the value between two peaks is a
 * straight line, which at 60 Hz over ~300 ms is indistinguishable from a curve
 * and a great deal easier to reason about. */
static int ease_shake(int t)
{
    /* peaks, in units of ONE/16, alternating sign and decaying */
    static const signed char PEAK[] = { 0, 16, -12, 9, -6, 3, -1, 0 };
    int n = (int)(sizeof PEAK / sizeof PEAK[0]) - 1;   /* segments */
    int seg = t * n / ONE, frac, a, b;
    if (seg >= n) seg = n - 1;
    frac = t * n - seg * ONE;                          /* 0..ONE within seg */
    a = PEAK[seg] * (ONE / 16);
    b = PEAK[seg + 1] * (ONE / 16);
    return a + (b - a) * frac / ONE;
}

int unoui_ease(int ease, int t)
{
    int u;
    /* Endpoints are EXACT for every curve, including the ones whose fixed-point
     * arithmetic lands a few parts in 4096 short (bounce). An animation that
     * stops 0.1% shy of its target is a stuck pixel someone eventually files a
     * bug about, so the ends are pinned rather than computed. */
    if (t <= 0)   return 0;
    /* ...except SHAKE, whose end is zero, not ONE: it oscillates ABOUT the
     * start and comes home, so pinning it to ONE would leave the thing it moved
     * parked one full amplitude off to the side. */
    if (t >= ONE) return (ease == UI_EASE_SHAKE) ? 0 : ONE;

    switch (ease) {
    case UI_EASE_IN:          return fmul(t, t);
    case UI_EASE_OUT:         u = ONE - t; return ONE - fmul(u, u);
    case UI_EASE_INOUT:
        if (t < ONE / 2) return 2 * fmul(t, t);
        u = ONE - t;     return ONE - 2 * fmul(u, u);
    case UI_EASE_IN_CUBIC:    return fmul(fmul(t, t), t);
    case UI_EASE_OUT_CUBIC:   u = ONE - t; return ONE - fmul(fmul(u, u), u);
    case UI_EASE_INOUT_CUBIC:
        if (t < ONE / 2) return 4 * fmul(fmul(t, t), t);
        u = ONE - t;     return ONE - 4 * fmul(fmul(u, u), u);
    case UI_EASE_OUT_BACK:    return ease_back_out(t);
    case UI_EASE_OUT_BOUNCE:  return ease_bounce_out(t);
    case UI_EASE_STEP:        return 0;          /* ONE only at t == ONE      */
    case UI_EASE_SHAKE:       return ease_shake(t);
    default:                  return t;          /* UI_EASE_LINEAR            */
    }
}

int unoui_anim_lerp(int from, int to, int e)
{
    /* Split the delta so a large one (a scroll offset in a long document, say)
     * cannot overflow the multiply. q * e is the coarse part, the remainder
     * carries the fraction, and that fraction is ROUNDED rather than truncated:
     * truncating costs most of a pixel at every sample, which reads as an
     * animation that arrives a pixel short and a frame late. */
    int d = to - from;
    int q = d / ONE, r = d - q * ONE;
    return from + q * e + fmul(r, e);
}

/* ------------------------------------------------------------ handles ------ */

/* handle = generation << 8 | (slot + 1). Slot 0 is encoded as 1 so that a
 * handle is never 0, and the generation makes a handle to a recycled slot
 * refuse rather than read whatever moved in. */
#define H_SLOT(h)  (((h) & 0xff) - 1)
#define H_GEN(h)   ((unsigned)(h) >> 8)
#define H_MAKE(i, g) (int)(((unsigned)(g) << 8) | (unsigned)((i) + 1))

static unoui_anim_slot *slot_of(const unoui_anim *ac, unoui_anim_h h)
{
    int i;
    if (h <= 0) return 0;
    i = H_SLOT(h);
    if (i < 0 || i >= UNOUI_ANIM_MAX) return 0;
    if (ac->s[i].state == 0) return 0;
    if (ac->s[i].gen != (unsigned short)H_GEN(h)) return 0;
    return (unoui_anim_slot *)&ac->s[i];
}

/* ------------------------------------------------------------ context ------ */

void unoui_anim_init(unoui_anim *ac)
{
    int i;
    for (i = 0; i < UNOUI_ANIM_MAX; i++) {
        ac->s[i].state = 0; ac->s[i].gen = 0; ac->s[i].value = 0; ac->s[i].t0 = 0;
    }
    for (i = 0; i < UNOUI_ANIM_SEQS; i++) ac->seq[i] = 0;
    ac->now = 0; ac->started = 0; ac->fallback = 0;
}

void unoui_anim_reset(unoui_anim *ac)
{
    int i;
    for (i = 0; i < UNOUI_ANIM_SEQS; i++) {
        if (ac->seq[i]) { ac->seq[i]->state = UI_SEQ_IDLE; ac->seq[i]->ngrp = 0; }
        ac->seq[i] = 0;
    }
    for (i = 0; i < UNOUI_ANIM_MAX; i++)
        if (ac->s[i].state) { ac->s[i].state = 0; ac->s[i].gen++; }
}

unsigned unoui_anim_now(const unoui_anim *ac) { return ac->now; }

int unoui_anim_active(const unoui_anim *ac)
{
    int i, n = 0;
    for (i = 0; i < UNOUI_ANIM_MAX; i++) if (ac->s[i].state == 1) n++;
    return n;
}

/* ------------------------------------------------------------ evaluation --- */

/* elapsed -> normalised time. The divide is unsigned so a very long duration
 * cannot overflow the scale-up; past ~17 minutes it loses the low bits of
 * precision rather than wrapping, which is the right way round. */
static int normalise(unsigned e, int dur)
{
    if (dur <= 0) return ONE;
    if (dur >= (1 << 20)) return (int)(e / (unsigned)(dur / ONE));
    return (int)((e * (unsigned)ONE + (unsigned)dur / 2u) / (unsigned)dur);
}

/* The value a tween SETTLES on. For every ramp curve that is `to`, because the
 * ends are pinned (unoui_ease returns exactly ONE at t == ONE) - so this is the
 * same exact endpoint the old `s->value = s->tw.to` gave, arrived at by asking
 * the curve instead of assuming the answer.
 *
 * Which matters because not every curve ends where it is pointed: UI_EASE_SHAKE
 * oscillates ABOUT the start and comes home, so its `to` is an amplitude and
 * its final value is `from`. Assuming `to` left everything a shake had moved
 * parked one amplitude off to the side, permanently. */
static int end_value(const unoui_tween *tw)
{ return unoui_anim_lerp(tw->from, tw->to, unoui_ease(tw->ease, ONE)); }

/* recompute one running slot against `now`; clears state to 2 when it ends */
static void slot_eval(unoui_anim_slot *s, unsigned now)
{
    unsigned elapsed = now - s->t0;
    int t, dur = s->tw.dur_ms;

    if (s->tw.delay_ms > 0 && elapsed < (unsigned)s->tw.delay_ms) {
        s->value = s->tw.from;
    } else {
        unsigned e = elapsed - (unsigned)(s->tw.delay_ms > 0 ? s->tw.delay_ms : 0);
        if (dur <= 0) {
            s->value = end_value(&s->tw);
            if (s->tw.loop == UI_ANIM_ONCE) s->state = 2;
        } else if (s->tw.loop == UI_ANIM_LOOP) {
            s->value = unoui_anim_lerp(s->tw.from, s->tw.to,
                                       unoui_ease(s->tw.ease,
                                                  normalise(e % (unsigned)dur, dur)));
        } else if (s->tw.loop == UI_ANIM_PINGPONG) {
            unsigned cycle = (unsigned)dur * 2u, p = e % cycle;
            t = normalise(p < (unsigned)dur ? p : cycle - p, dur);
            s->value = unoui_anim_lerp(s->tw.from, s->tw.to,
                                       unoui_ease(s->tw.ease, t));
        } else if (e >= (unsigned)dur) {
            s->value = end_value(&s->tw);   /* the curve's own end, exactly   */
            s->state = 2;
        } else {
            s->value = unoui_anim_lerp(s->tw.from, s->tw.to,
                                       unoui_ease(s->tw.ease, normalise(e, dur)));
        }
    }
    if (s->tw.out) *s->tw.out = s->value;
}

/* ------------------------------------------------------------ tweens ------- */

/* start a tween as though it had begun at `t0`. A sequence uses this to keep to
 * its SCHEDULE: a group due at 500 ms that the frame loop only gets around to
 * at 517 ms starts 17 ms in, rather than 17 ms late. Otherwise every step of a
 * long build accumulates one frame of drift, which is the frame-counting defect
 * this facility exists to remove, just moved up a level. */
static unoui_anim_h tween_start_at(unoui_anim *ac, const unoui_tween *tw, unsigned t0)
{
    int i, pick = -1;
    unoui_anim_slot *s;

    for (i = 0; i < UNOUI_ANIM_MAX; i++) if (ac->s[i].state == 0) { pick = i; break; }
    if (pick < 0)   /* under pressure, a finished tween gives up its slot first */
        for (i = 0; i < UNOUI_ANIM_MAX; i++) if (ac->s[i].state == 2) { pick = i; break; }
    if (pick < 0) return 0;

    s = &ac->s[pick];
    s->tw = *tw;
    s->gen = (unsigned short)((s->gen + 1) & 0x7fff);
    s->t0 = t0;
    s->state = 1;
    s->value = tw->from;
    slot_eval(s, ac->now);
    return H_MAKE(pick, s->gen);
}

unoui_anim_h unoui_tween_start(unoui_anim *ac, const unoui_tween *tw)
{
    return tween_start_at(ac, tw, ac->now);
}

unoui_anim_h unoui_tween_to(unoui_anim *ac, int from, int to, int dur_ms,
                            int ease, int *out)
{
    unoui_tween tw;
    tw.from = from; tw.to = to; tw.dur_ms = dur_ms; tw.delay_ms = 0;
    tw.ease = (unsigned char)ease; tw.loop = UI_ANIM_ONCE; tw.out = out;
    return unoui_tween_start(ac, &tw);
}

int unoui_anim_value(const unoui_anim *ac, unoui_anim_h h)
{
    const unoui_anim_slot *s = slot_of(ac, h);
    return s ? s->value : 0;
}

int unoui_anim_progress(const unoui_anim *ac, unoui_anim_h h)
{
    const unoui_anim_slot *s = slot_of(ac, h);
    unsigned elapsed;
    if (!s) return ONE;
    if (s->state == 2) return ONE;
    elapsed = ac->now - s->t0;
    if (s->tw.delay_ms > 0 && elapsed < (unsigned)s->tw.delay_ms) return 0;
    return unoui_ease(s->tw.ease,
                      normalise(elapsed - (unsigned)(s->tw.delay_ms > 0
                                                     ? s->tw.delay_ms : 0),
                                s->tw.dur_ms));
}

/* An unknown handle reads as DONE and not LIVE. That is the safe direction:
 * code that waits for an animation to finish must not wait forever on a handle
 * whose slot was recycled, and code that redraws while one is live must not
 * redraw forever. */
int unoui_anim_done(const unoui_anim *ac, unoui_anim_h h)
{
    const unoui_anim_slot *s = slot_of(ac, h);
    return !s || s->state == 2;
}

int unoui_anim_live(const unoui_anim *ac, unoui_anim_h h)
{
    const unoui_anim_slot *s = slot_of(ac, h);
    return s && s->state == 1;
}

void unoui_anim_cancel(unoui_anim *ac, unoui_anim_h h)
{
    unoui_anim_slot *s = slot_of(ac, h);
    if (s) s->state = 2;                  /* stops where it stands */
}

void unoui_anim_finish(unoui_anim *ac, unoui_anim_h h)
{
    unoui_anim_slot *s = slot_of(ac, h);
    if (!s) return;
    s->value = end_value(&s->tw);
    s->state = 2;
    if (s->tw.out) *s->tw.out = s->value;
}

void unoui_anim_free(unoui_anim *ac, unoui_anim_h h)
{
    unoui_anim_slot *s = slot_of(ac, h);
    if (s) { s->state = 0; s->gen = (unsigned short)((s->gen + 1) & 0x7fff); }
}

/* ------------------------------------------------------------ sequences ---- */

void unoui_seq_init(unoui_seq *s)
{
    int i;
    s->nstep = 0; s->cur = 0; s->state = UI_SEQ_IDLE; s->trigger = 0;
    s->ngrp = 0; s->group_end = 0;
    for (i = 0; i < UNOUI_SEQ_GROUP; i++) s->grp[i] = 0;
}

int unoui_seq_add(unoui_seq *s, int start, const unoui_tween *tw)
{
    int i;
    if (s->nstep >= UNOUI_SEQ_STEPS) return -1;
    i = s->nstep++;
    s->step[i].tw = *tw;
    s->step[i].start = (unsigned char)start;
    return i;
}

/* Start step `cur` plus every following UI_STEP_WITH step, and remember when
 * the whole group finishes. Completion is TIME-based, not handle-based: a
 * group's end is known the moment it starts, so a sequence stays correct even
 * if the pool recycles one of its tweens out from under it. */
static void seq_start_group(unoui_anim *ac, unoui_seq *s, unsigned at)
{
    int first = 1;

    /* Never schedule INTO the future, and never catch up more than the group
     * itself: `at` is the moment the group was due, which in normal running is
     * at most one frame behind now. */
    if (!reached(ac->now, at)) at = ac->now;

    s->ngrp = 0;
    s->group_end = at;
    while (s->cur < s->nstep) {
        const unoui_seq_step *st = &s->step[s->cur];
        unsigned end;
        if (!first && st->start != UI_STEP_WITH) break;
        {
            unoui_anim_h h = tween_start_at(ac, &st->tw, at);
            if (s->ngrp < UNOUI_SEQ_GROUP) s->grp[s->ngrp++] = h;
        }
        end = at + (unsigned)(st->tw.delay_ms > 0 ? st->tw.delay_ms : 0)
                 + (unsigned)(st->tw.dur_ms > 0 ? st->tw.dur_ms : 0);
        if (!reached(s->group_end, end)) s->group_end = end;
        s->cur++;
        first = 0;
    }
    s->state = UI_SEQ_RUN;
}

/* what happens once a group is over: park on a trigger step, start the next
 * group, or finish */
static void seq_next(unoui_anim *ac, unoui_seq *s, unsigned at)
{
    if (s->cur >= s->nstep) { s->state = UI_SEQ_DONE; s->ngrp = 0; return; }
    if (s->step[s->cur].start == UI_STEP_ON_TRIGGER) { s->state = UI_SEQ_WAIT; return; }
    seq_start_group(ac, s, at);
}

static void seq_tick(unoui_anim *ac, unoui_seq *s)
{
    int i;
    if (s->state == UI_SEQ_IDLE || s->state == UI_SEQ_DONE) { s->trigger = 0; return; }

    if (s->state == UI_SEQ_WAIT) {
        if (!s->trigger) return;
        s->trigger = 0;
        seq_start_group(ac, s, ac->now);   /* the click IS the schedule */
        return;
    }

    if (s->trigger) {
        /* Click during a build: snap what is in flight to its end and move on.
         * If the next step is itself a trigger step this lands in WAIT, so one
         * click skips the build and the next one starts the following build -
         * which is what a presenter clicking through a deck expects. */
        s->trigger = 0;
        for (i = 0; i < s->ngrp; i++) unoui_anim_finish(ac, s->grp[i]);
        s->ngrp = 0;
        s->group_end = ac->now;
        seq_next(ac, s, ac->now);
        return;
    }
    if (reached(ac->now, s->group_end)) seq_next(ac, s, s->group_end);
}

int unoui_seq_start(unoui_anim *ac, unoui_seq *s)
{
    int i, slot = -1;
    for (i = 0; i < UNOUI_ANIM_SEQS; i++) {
        if (ac->seq[i] == s) { slot = i; break; }
        if (ac->seq[i] == 0 && slot < 0) slot = i;
    }
    if (slot < 0) return 0;
    ac->seq[slot] = s;

    s->cur = 0; s->trigger = 0; s->ngrp = 0;
    if (s->nstep == 0) { s->state = UI_SEQ_DONE; return 1; }
    if (s->step[0].start == UI_STEP_ON_TRIGGER) s->state = UI_SEQ_WAIT;
    else seq_start_group(ac, s, ac->now);
    return 1;
}

void unoui_seq_trigger(unoui_seq *s) { s->trigger = 1; }

void unoui_seq_stop(unoui_anim *ac, unoui_seq *s)
{
    int i;
    for (i = 0; i < s->ngrp; i++) unoui_anim_cancel(ac, s->grp[i]);
    s->ngrp = 0; s->state = UI_SEQ_IDLE; s->trigger = 0;
    for (i = 0; i < UNOUI_ANIM_SEQS; i++) if (ac->seq[i] == s) ac->seq[i] = 0;
}

int unoui_seq_done   (const unoui_seq *s) { return s->state == UI_SEQ_DONE; }
int unoui_seq_waiting(const unoui_seq *s) { return s->state == UI_SEQ_WAIT; }

/* ------------------------------------------------------------ the tick ----- */

void unoui_anim_tick(unoui_anim *ac, unsigned now_ms)
{
    int i;

    /* First tick after boot: rebase anything already started onto it. A shell
     * builds its UI (and can start an animation) before the frame loop reads
     * the clock for the first time, and without this that tween would see a
     * jump of however long the machine had been up and complete instantly. */
    if (!ac->started) {
        for (i = 0; i < UNOUI_ANIM_MAX; i++) if (ac->s[i].state) ac->s[i].t0 = now_ms;
        for (i = 0; i < UNOUI_ANIM_SEQS; i++)
            if (ac->seq[i] && ac->seq[i]->state == UI_SEQ_RUN)
                ac->seq[i]->group_end = now_ms + (ac->seq[i]->group_end - ac->now);
        ac->started = 1;
    }
    ac->now = now_ms;

    for (i = 0; i < UNOUI_ANIM_MAX; i++)
        if (ac->s[i].state == 1) slot_eval(&ac->s[i], now_ms);

    for (i = 0; i < UNOUI_ANIM_SEQS; i++)
        if (ac->seq[i]) seq_tick(ac, ac->seq[i]);
}

void unoui_anim_frame(unoui_anim *ac)
{
    if (unoui_clock_ms) { unoui_anim_tick(ac, unoui_clock_ms()); return; }
    ac->fallback += UNOUI_TICK_MS;
    unoui_anim_tick(ac, ac->fallback);
}
