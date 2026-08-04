/* ===========================================================================
 * unoui_anim - the shared tween clock.
 *
 * WHY THIS IS NOT IN AN APP. Every animated thing in UnoDOS currently counts
 * its own frames: the window manager's snap, UnoAmp's EQ decay, the browser's
 * smooth scroll and UnoShow's slide transitions each hand-roll a counter, so
 * every one of them runs at a speed that depends on how busy the desktop is,
 * and none of them can be sequenced against another. This is that counter,
 * written once: a value that moves from A to B over a duration in MILLISECONDS
 * along an easing curve, several at a time, addressed by handle, optionally
 * ordered into a sequence.
 *
 * NO FLOATING POINT AND NO fb.h. The whole file is integer arithmetic over a
 * 12-bit fixed point (UI_ANIM_ONE), so it compiles on every port unoui reaches
 * and on the ones it does not. It draws nothing; an animation is a NUMBER, and
 * what that number means (an x coordinate, an alpha, a dissolve threshold) is
 * the app's business.
 *
 *   1. app describes a tween          unoui_tween_to(&AC, 0, 240, 300, ...)
 *   2. shell advances the clock once  unoui_anim_frame(&AC)     per frame
 *   3. app reads the value            unoui_anim_value(&AC, h)  or an int *out
 * ===========================================================================
 */
#ifndef UNOUI_ANIM_H
#define UNOUI_ANIM_H

/* ---- fixed point ---------------------------------------------------------
 * Normalised progress runs 0..UI_ANIM_ONE. 12 bits is deliberate: an easing
 * curve is a cubic, and 4096^3 does not fit in 32 bits, so every curve below
 * normalises between multiplies. Overshoot curves return slightly outside
 * 0..ONE, which is the point of them. */
#define UI_ANIM_ONE 4096

typedef enum {
    UI_EASE_LINEAR = 0,
    UI_EASE_IN,            /* quadratic: starts still, accelerates            */
    UI_EASE_OUT,           /* quadratic: arrives slowing - the default feel   */
    UI_EASE_INOUT,
    UI_EASE_IN_CUBIC,      /* the same three, harder                          */
    UI_EASE_OUT_CUBIC,
    UI_EASE_INOUT_CUBIC,
    UI_EASE_OUT_BACK,      /* overshoots past `to`, then settles              */
    UI_EASE_OUT_BOUNCE,    /* lands, bounces, lands again                     */
    UI_EASE_STEP,          /* holds `from`, snaps to `to` at the end          */
    UI_EASE_N
} ui_ease;

/* map normalised time to normalised progress. t is clamped to 0..UI_ANIM_ONE;
 * the result may exceed that range for the overshoot curves. Public because an
 * app that already owns a counter (a canvas mid-transition) wants the curve
 * without wanting a slot. */
int unoui_ease(int ease, int t);

/* from + (to - from) * e / UI_ANIM_ONE, done so that a large delta cannot
 * overflow 32 bits mid-multiply. `e` is an eased progress, overshoot included. */
int unoui_anim_lerp(int from, int to, int e);

/* ---- the clock seam -------------------------------------------------------
 * The toolkit has no portable clock - the same reason unoui_profile_win exists
 * - so the platform supplies one. Set it once at boot to a function returning
 * milliseconds since anything (only differences are ever taken, and unsigned
 * wraparound is handled, so a 32-bit ms counter that rolls over every 49 days
 * is fine).
 *
 * Left NULL, unoui_anim_frame() falls back to counting frames at
 * UNOUI_TICK_MS each. That keeps a port working before it has a clock, at the
 * cost of the exact defect this facility exists to fix, so it is a stopgap and
 * not the plan. */
extern unsigned (*unoui_clock_ms)(void);
#define UNOUI_TICK_MS 16       /* the fallback's assumed frame time, ~60 Hz   */

/* ---- one tween ------------------------------------------------------------
 * Zero-initialised, this is a 0 ms linear hold at 0, which is inert - so a
 * struct an app memsets and half fills behaves. */
enum {
    UI_ANIM_ONCE = 0,
    UI_ANIM_LOOP,          /* restart from `from` forever                     */
    UI_ANIM_PINGPONG       /* to, then back to from, forever                  */
};

typedef struct unoui_tween {
    int  from, to;
    int  dur_ms;           /* 0 = arrive immediately                          */
    int  delay_ms;         /* hold at `from` this long first                  */
    unsigned char ease;    /* ui_ease                                         */
    unsigned char loop;    /* UI_ANIM_*                                       */
    int *out;              /* optional: written with the value every tick     */
} unoui_tween;

/* A handle. 0 is never valid, so a zeroed field reads as "no animation".
 * Handles carry a generation counter, so a handle to a slot that has since
 * been recycled is REFUSED rather than silently reading someone else's tween.
 * A finished tween keeps its slot (and stays readable) until the pool needs it
 * back; an app that cares beyond that should pass an `out` pointer. */
typedef int unoui_anim_h;

/* ---- a sequence -----------------------------------------------------------
 * The ordering half of the request: "paragraph 1 flies in, wait for a click,
 * paragraph 2 flies in while paragraph 1 dims". Steps run in order; each one
 * says how it starts relative to the step before it.
 *
 * Storage is APP-OWNED, like a window's widget array or a text field's buffer.
 * unoui_seq_start() registers the pointer with a context, which then advances
 * it; unoui_seq_stop() takes it back. A sequence must outlive its registration. */
enum {
    UI_STEP_AFTER = 0,     /* starts when the previous group has finished     */
    UI_STEP_WITH,          /* starts alongside the previous step              */
    UI_STEP_ON_TRIGGER     /* waits for unoui_seq_trigger() - the click       */
};

#define UNOUI_SEQ_STEPS 32     /* steps in one sequence                       */
#define UNOUI_SEQ_GROUP 8      /* steps that can start together (WITH)        */

typedef struct unoui_seq_step {
    unoui_tween   tw;
    unsigned char start;   /* UI_STEP_*                                       */
} unoui_seq_step;

enum { UI_SEQ_IDLE = 0, UI_SEQ_RUN, UI_SEQ_WAIT, UI_SEQ_DONE };

typedef struct unoui_seq {
    unoui_seq_step step[UNOUI_SEQ_STEPS];
    int            nstep;
    int            cur;            /* next step to start                      */
    unsigned char  state;          /* UI_SEQ_*                                */
    unsigned char  trigger;        /* a trigger arrived, not yet consumed     */
    int            ngrp;
    unoui_anim_h   grp[UNOUI_SEQ_GROUP];   /* the group in flight             */
    unsigned       group_end;      /* ms at which the group finishes          */
} unoui_seq;

/* ---- the context ----------------------------------------------------------
 * One per shell is the intended shape (apps share it, handles keep them
 * apart); a context of its own is there for anything that wants to pause or
 * drop every animation it owns in one call, which a slide show does on a slide
 * change. */
#define UNOUI_ANIM_MAX  48     /* concurrent tweens                           */
#define UNOUI_ANIM_SEQS 8      /* registered sequences                        */

typedef struct unoui_anim_slot {
    unoui_tween tw;
    unsigned    t0;            /* ms at which the delay started               */
    int         value;         /* last computed value                         */
    unsigned short gen;        /* generation, for stale-handle refusal        */
    unsigned char  state;      /* 0 free, 1 running, 2 finished               */
} unoui_anim_slot;

typedef struct unoui_anim {
    unoui_anim_slot s[UNOUI_ANIM_MAX];
    unoui_seq      *seq[UNOUI_ANIM_SEQS];
    unsigned        now;       /* ms of the last tick                         */
    unsigned        started;   /* nonzero once the first tick has run         */
    unsigned        fallback;  /* the no-clock frame counter, in ms           */
} unoui_anim;

void unoui_anim_init (unoui_anim *);          /* also fine to memset to zero  */
void unoui_anim_reset(unoui_anim *);          /* drop every tween + sequence  */

/* ---- driving it -----------------------------------------------------------
 * ONE of these per frame, from the shell's frame loop, before anything reads a
 * value. _tick takes the time explicitly (a test harness drives exact times
 * that way; so does a platform that already has `now` in hand); _frame reads
 * unoui_clock_ms, or counts frames if there is none. */
void unoui_anim_tick (unoui_anim *, unsigned now_ms);
void unoui_anim_frame(unoui_anim *);
unsigned unoui_anim_now(const unoui_anim *);  /* the last ticked time, ms     */

/* how many tweens are still moving. The repaint signal: a shell that redraws
 * on demand keeps redrawing while this is nonzero. */
int  unoui_anim_active(const unoui_anim *);

/* ---- tweens ---------------------------------------------------------------
 * A start returns 0 when the pool is full. It never fails silently in a way
 * that matters: unoui_anim_value(ac, 0) is 0 and unoui_anim_done(ac, 0) is 1,
 * so an unchecked handle behaves as an animation that already finished. */
unoui_anim_h unoui_tween_start(unoui_anim *, const unoui_tween *);
/* the common case, without a struct */
unoui_anim_h unoui_tween_to(unoui_anim *, int from, int to, int dur_ms,
                            int ease, int *out);

int  unoui_anim_value   (const unoui_anim *, unoui_anim_h);
int  unoui_anim_progress(const unoui_anim *, unoui_anim_h);  /* 0..ONE, eased */
int  unoui_anim_done    (const unoui_anim *, unoui_anim_h);  /* unknown = done */
int  unoui_anim_live    (const unoui_anim *, unoui_anim_h);  /* known & moving */

void unoui_anim_cancel(unoui_anim *, unoui_anim_h);  /* stop where it stands  */
void unoui_anim_finish(unoui_anim *, unoui_anim_h);  /* jump to `to`, stop    */
void unoui_anim_free  (unoui_anim *, unoui_anim_h);  /* release the slot      */

/* ---- sequences ------------------------------------------------------------ */
void unoui_seq_init (unoui_seq *);
/* append a step; returns its index, or -1 when the sequence is full */
int  unoui_seq_add  (unoui_seq *, int start, const unoui_tween *);
/* register with a context and start running. 0 = no free sequence slot. */
int  unoui_seq_start(unoui_anim *, unoui_seq *);
/* the click. Waiting on a trigger: start the next group. Mid-group with a
 * trigger step next: snap the current group to its end and start the next -
 * the "click again to skip the build" every presenter expects. */
void unoui_seq_trigger(unoui_seq *);
void unoui_seq_stop   (unoui_anim *, unoui_seq *);   /* cancel + unregister   */
int  unoui_seq_done   (const unoui_seq *);
int  unoui_seq_waiting(const unoui_seq *);           /* parked on a trigger   */

#endif /* UNOUI_ANIM_H */
