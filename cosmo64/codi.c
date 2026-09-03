/* cosmo64/codi.c -- the Cosmo's rear cover panel as a touchpad (M7).
 *
 * The cover panel is not a peripheral this SoC can touch. It is an STM32L4R9
 * on the always-on VBAT rail with its own display and its own FocalTech
 * FT3x67 touch controller, and the AP reaches it through ONE serial link:
 * UART1, 115200 8N1. So an AP-side cover driver is a UART, a message codec
 * and a small state machine -- which is all this file is.
 *
 * The protocol, the command numbers and the electrical facts come from
 * `docs/codi-driver-spec.md` in hmofet/cosmo, which is the device-verified
 * write-up of the whole interface; the feel of the pointer is a port of
 * `scripts/cosmo-rear-touchpad`, the Linux daemon that has been driving this
 * panel since 2026-08-31, with its user-validated tuning carried across
 * unchanged. Nothing here is copied from either -- the daemon is Python
 * against uinput and this is C against pc64's input ring -- but every
 * threshold below is that daemon's, because those numbers were chosen with a
 * finger on the glass and re-deriving them by feel on a device we can only
 * observe through a log would be worse than useless.
 *
 * IT NEEDS OurCodi. Stock Planet firmware defines the mouse messages and
 * never sends one -- measured exhaustively, see docs/codi-third-party.md --
 * so on a stock CoDi this driver finds a version string, reports it, and
 * goes quiet. That is the correct outcome, not a failure, and the log says
 * which firmware answered.
 *
 * WHICH PINS. This is the one fact the spec could not pin down, and it is
 * decided by measurement rather than guessed. UART1's signals can be brought
 * out on three different pin pairs on this SoC (110/112 in function 7,
 * 46/47 in function 2, 19/20 in function 5); 41/42 is a fourth on paper and
 * is NOT probed: those two pins carry USB IDDIG and DRVVBUS in their function
 * 1, which is what a device with a USB-C OTG port uses them for, and muxing
 * the live USB host's ID and VBUS lines away on the chance that a UART is
 * behind them is a bad trade for a guess. If all three candidates come up
 * empty, that pair is the next thing to try -- with the USB stack quiet.
 * MediaTek's own MT6771 reference design -- which Planet built this
 * device from, and whose DWS is in the vendor kernel -- routes UART1 to
 * 110/112, so that pair is tried first. Each candidate gets the pinmux, a
 * version query and a short wait; the pair the CoDi answers on is the pair,
 * and the losers are put back exactly as they were found. It costs about a
 * second at boot, once, and it turns an unknown into a line in the log.
 *
 * There is no lid sensor here. The Linux daemon suppresses touch while the
 * lid is closed (a pocket touch must never click the desktop) by asking
 * UPower; bare metal has no such oracle yet, so this driver stays armed. The
 * consequence is honest and worth knowing: with the machine shut in a bag,
 * the cover panel still moves the pointer.
 *
 * The AP's three control GPIOs (reset 77, download-select 80, wake 157) are
 * DELIBERATELY not touched. The MCU is always powered and never needs a reset
 * to talk; and a stray reset with the download-select line high leaves it
 * sitting in its flashing stub with the panel dark, which is a service call
 * rather than a bug.
 */

#include "cosmo64.h"

/* ---- UART1 --------------------------------------------------------------- */
/* MTK's 8250-compatible core: the standard register file at a 4-byte stride,
 * plus the rate extensions this driver does not need (at 115200 from the
 * 26 MHz UART clock the plain divisor latch lands within 0.8%, which is well
 * inside a UART's tolerance). */

#define UART1 0x11003000ull
#define U32(off) (*(volatile c64_u32 *)(UART1 + (off)))
#define U_RBR 0x00       /* read: received byte; write: transmit; DLAB: DLL */
#define U_IER 0x04       /* DLAB: DLH */
#define U_FCR 0x08       /* write-only (IIR on read) */
#define U_LCR 0x0C
#define U_MCR 0x10
#define U_LSR 0x14
#define U_HIGHSPEED 0x24

#define LCR_8N1  0x03
#define LCR_DLAB 0x80
#define FCR_ENABLE_CLEAR 0x07            /* FIFO on + flush both directions */
#define LSR_DR   0x01                    /* a byte is waiting */
#define LSR_THRE 0x20                    /* the transmit holding register is free */

#define UART_SRC_HZ 26000000u

/* infracfg_ao module gate 0: UART1 is bit 23. LK's own uart.c carries the
 * ungate under an `#if 0 // default on`, so this is belt and braces -- a
 * write of 1 into the CLR register clears a gate that is probably already
 * clear. */
#define INFRA_CG0_CLR 0x10001084ull
#define CG0_UART1 (1u << 23)

/* The three pin pairs UART1 can appear on, most likely first. `mode_reg` and
 * `shift` locate the pin's four-bit function field in the GPIO block;
 * `ies_reg`/`ies_bit` its pad input enable, without which the receive pin is
 * deaf no matter what the mux says. */
struct pinpair {
    const char *name;
    c64_u64 rx_mode_reg; unsigned rx_shift;
    c64_u64 tx_mode_reg; unsigned tx_shift;
    unsigned func;
    c64_u64 ies_reg; c64_u32 ies_bits;
};

static const struct pinpair k_pins[3] = {
    /* GPIO110 (URXD1) + GPIO112 (UTXD1), function 7 -- MediaTek's reference
     * routing for this SoC, and therefore Planet's most likely one. */
    { "GPIO110/112 f7", 0x100053D0ull, 24, 0x100053E0ull, 0, 7,
      0x11C50000ull, (1u << 0) | (1u << 2) },
    /* GPIO46/47, function 2 */
    { "GPIO46/47 f2", 0x10005350ull, 24, 0x10005350ull, 28, 2,
      0x11E70000ull, (1u << 4) },
    /* GPIO19/20, function 5 */
    { "GPIO19/20 f5", 0x10005320ull, 12, 0x10005320ull, 16, 5,
      0x11E80000ull, (1u << 3) },
};

/* ---- the wire protocol --------------------------------------------------- */

#define CODI_MAGIC0 0x58                 /* 'X' '!' 'X' '!' */
#define CODI_MAGIC1 0x21
#define CODI_HDR 16
#define CODI_MAXMSG 0x20000u

#define CMD_GET_VERSION  1
#define CMD_INFO_VERSION 2
#define CMD_SET_MOUSE    146
#define CMD_MOUSE_INFO   147
#define CMD_SET_DISPLAY  210             /* OurCodi extension */

#define MOUSE_MOVE_REL 1
#define MOUSE_RELEASE  2
#define MOUSE_PRESS    3
#define MOUSE_FINGERS  4                 /* OurCodi extension: dx = count */

/* ---- the feel, as the Linux daemon tuned it ------------------------------ *
 * User-validated 2026-08-31: SWAP_XY=1 INVERT_X=1 INVERT_Y=1 GAIN=1.8
 * SLOW_F=0.4 ACCEL=0.01 SMOOTH=0.35 TAP_MS=150 TAP_SLOP=25.
 *
 * All of it in thousandths, because this build has no libm and a response
 * curve in floating point would buy nothing a divide does not: the inputs are
 * integer panel units and the output is integer pixels. */
#define GAIN_MILLI   1800
#define SLOW_F_MILLI 400
#define SLOW_SPEED   8                   /* below this: the precision zone */
#define FAST_SPEED   25                  /* above this: acceleration       */
#define ACCEL_MILLI  10                  /* +0.01 of gain per speed unit   */
#define MAX_BOOST_MILLI 300              /* ...capped at +30%              */
#define SMOOTH_MILLI 350                 /* of the pending pool per tick   */
#define TAP_MS   150
#define TAP_SLOP 25
#define DRAG_MS  250
#define DRAIN_MS 8
#define REARM_MS 5000

static int g_present;                    /* a CoDi answered on some pin pair */
static int g_ourcodi;                    /* ...and it forwards touch         */
static int g_pin;                        /* which pair won                   */

/* the gesture machine */
static int g_down;
static c64_u64 g_down_at;
static int g_moved;                      /* |delta| accumulated, panel units */
static int g_fingers, g_max_fingers;
static int g_pend_x, g_pend_y;           /* milli-pixels not yet emitted     */
static c64_u64 g_next_drain, g_next_rearm, g_drag_until;
static int g_dragging, g_drag_open;
static int g_btn;                        /* the level we publish             */

/* the receive resynchroniser */
static c64_u8 g_rx[512];
static unsigned g_rxn;

static int g_said_hello;

/* ---- small helpers -------------------------------------------------------- */

static c64_u64 ms_now(void)
{
    c64_u64 f = c64_cnt_freq();
    if (!f)
        f = 13000000ull;
    return c64_cnt_now() / (f / 1000ull);
}

/* ---- the UART ------------------------------------------------------------- */

static void uart_setup(unsigned baud)
{
    c64_u32 div = (UART_SRC_HZ + baud * 8u) / (baud * 16u);
    if (!div)
        div = 1;
    *(volatile c64_u32 *)INFRA_CG0_CLR = CG0_UART1;
    __asm__ volatile("dsb sy" ::: "memory");

    U32(U_IER) = 0;                      /* polled: no interrupts        */
    U32(U_HIGHSPEED) = 0;                /* the plain 16x divisor path   */
    U32(U_LCR) = LCR_DLAB;
    U32(U_RBR) = div & 0xFFu;            /* DLL */
    U32(U_IER) = (div >> 8) & 0xFFu;     /* DLH */
    U32(U_LCR) = LCR_8N1;                /* 8N1, DLAB back down          */
    U32(U_MCR) = 0;                      /* no flow control              */
    U32(U_FCR) = FCR_ENABLE_CLEAR;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void uart_put(c64_u8 b)
{
    c64_u64 t = c64_cnt_now() + c64_cnt_freq() / 100ull;   /* 10 ms */
    while (!(U32(U_LSR) & LSR_THRE))
        if (c64_cnt_now() > t)
            return;                      /* a wedged transmitter is not fatal */
    U32(U_RBR) = b;
}

static int uart_get(void)
{
    if (!(U32(U_LSR) & LSR_DR))
        return -1;
    return (int)(U32(U_RBR) & 0xFFu);
}

static void uart_flush_rx(void)
{
    int n = 0;
    while (uart_get() >= 0 && n < 4096)
        n++;
    g_rxn = 0;
}

/* ---- pins ----------------------------------------------------------------- */

static void pin_set(const struct pinpair *p, unsigned func)
{
    c64_u32 v = *(volatile c64_u32 *)p->rx_mode_reg;
    v = (v & ~(0xFu << p->rx_shift)) | ((func & 0xFu) << p->rx_shift);
    *(volatile c64_u32 *)p->rx_mode_reg = v;
    v = *(volatile c64_u32 *)p->tx_mode_reg;
    v = (v & ~(0xFu << p->tx_shift)) | ((func & 0xFu) << p->tx_shift);
    *(volatile c64_u32 *)p->tx_mode_reg = v;
    if (func)
        *(volatile c64_u32 *)p->ies_reg |= p->ies_bits;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void pin_restore(const struct pinpair *p, c64_u32 rx_mode, c64_u32 tx_mode)
{
    *(volatile c64_u32 *)p->rx_mode_reg = rx_mode;
    *(volatile c64_u32 *)p->tx_mode_reg = tx_mode;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- framing -------------------------------------------------------------- */

static void send_frame(c64_u32 cmd, const c64_u8 *payload, unsigned n)
{
    c64_u32 total = CODI_HDR + n;
    c64_u8 h[CODI_HDR];
    h[0] = CODI_MAGIC0; h[1] = CODI_MAGIC1; h[2] = CODI_MAGIC0; h[3] = CODI_MAGIC1;
    h[4] = (c64_u8)(total >> 24); h[5] = (c64_u8)(total >> 16);
    h[6] = (c64_u8)(total >> 8);  h[7] = (c64_u8)total;
    h[8] = (c64_u8)(cmd >> 24); h[9] = (c64_u8)(cmd >> 16);
    h[10] = (c64_u8)(cmd >> 8); h[11] = (c64_u8)cmd;
    h[12] = 0; h[13] = 0; h[14] = 0; h[15] = 1;      /* session 1 */
    for (unsigned i = 0; i < CODI_HDR; i++)
        uart_put(h[i]);
    for (unsigned i = 0; i < n; i++)
        uart_put(payload[i]);
}

static void set_mouse(int on)
{
    c64_u8 p[2] = { (c64_u8)(on ? 1 : 0), 0 };   /* on/off, relative mode */
    send_frame(CMD_SET_MOUSE, p, 2);
}

static c64_u32 be32(const c64_u8 *p)
{
    return ((c64_u32)p[0] << 24) | ((c64_u32)p[1] << 16)
         | ((c64_u32)p[2] << 8) | (c64_u32)p[3];
}

static int be16s(const c64_u8 *p)
{
    int v = ((int)p[0] << 8) | (int)p[1];
    return v >= 0x8000 ? v - 0x10000 : v;
}

/* Pull whatever the UART has into the resync buffer. The panel's frames do
 * not arrive aligned to anything -- a slow poll loop means several land
 * between two visits, and a dropped byte means the next magic is the only way
 * back -- so the buffer is a byte stream and the parser scans it. */
static void rx_fill(void)
{
    for (int i = 0; i < 512; i++) {
        int c = uart_get();
        if (c < 0)
            return;
        if (g_rxn < sizeof g_rx)
            g_rx[g_rxn++] = (c64_u8)c;
        else {
            /* Overrun: the parser has not been able to make sense of half a
             * kilobyte, which means the stream is out of step. Keep the tail,
             * which is where the next intact frame will be. */
            for (unsigned k = 0; k < sizeof g_rx / 2; k++)
                g_rx[k] = g_rx[k + sizeof g_rx / 2];
            g_rxn = sizeof g_rx / 2;
            g_rx[g_rxn++] = (c64_u8)c;
        }
    }
}

/* Hand each complete frame to `on_frame`, consuming what is parsed. Returns
 * the number of frames delivered. */
static int rx_parse(void (*on_frame)(c64_u32 cmd, const c64_u8 *p, unsigned n))
{
    unsigned i = 0;
    int got = 0;
    while (g_rxn - i >= CODI_HDR) {
        if (!(g_rx[i] == CODI_MAGIC0 && g_rx[i + 1] == CODI_MAGIC1
              && g_rx[i + 2] == CODI_MAGIC0 && g_rx[i + 3] == CODI_MAGIC1)) {
            i++;
            continue;
        }
        c64_u32 total = be32(g_rx + i + 4);
        if (total < CODI_HDR || total > CODI_MAXMSG) {
            i += 4;                       /* a magic with a lying length */
            continue;
        }
        if (total > sizeof g_rx) {
            /* Longer than anything this driver consumes and longer than the
             * buffer: skip past the header and resync. */
            i += CODI_HDR;
            continue;
        }
        if (g_rxn - i < total)
            break;                        /* the rest is still on the wire */
        on_frame(be32(g_rx + i + 8), g_rx + i + CODI_HDR, total - CODI_HDR);
        got++;
        i += total;
    }
    if (i) {
        for (unsigned k = i; k < g_rxn; k++)
            g_rx[k - i] = g_rx[k];
        g_rxn -= i;
    }
    return got;
}

/* ---- the pointer ---------------------------------------------------------- */

static void publish(int dx, int dy)
{
    c64_input_move_pointer_codi(dx, dy, g_btn);
}

static void click(int mask)
{
    g_btn = mask;
    publish(0, 0);
}

/* One coarse report from the panel becomes a short glide rather than a jump:
 * the transform, the response curve and the gain go into a pending pool, and
 * drain_motion() lets a fraction of it out every few milliseconds. The panel
 * delivers 30-50 units in a single event; emitting that at once is a cursor
 * teleport no smoothing above could hide. */
static void on_move(int dx, int dy)
{
    int adx, ady, speed, factor, mult;

    if (dx > 512) dx = 512; else if (dx < -512) dx = -512;
    if (dy > 512) dy = 512; else if (dy < -512) dy = -512;

    adx = dx < 0 ? -dx : dx;
    ady = dy < 0 ? -dy : dy;
    g_moved += (adx > ady) ? adx + ady / 2 : ady + adx / 2;
    if (g_fingers >= 2)
        return;                           /* no wander during a two-finger gesture */

    /* The cover's long axis is the machine's horizontal, and it faces the
     * other way: swap, then invert both. */
    { int t = dx; dx = dy; dy = t; }
    dx = -dx;
    dy = -dy;

    adx = dx < 0 ? -dx : dx;
    ady = dy < 0 ? -dy : dy;
    speed = (adx > ady) ? adx + ady / 2 : ady + adx / 2;

    if (speed < SLOW_SPEED)
        factor = SLOW_F_MILLI
               + (1000 - SLOW_F_MILLI) * speed / SLOW_SPEED;
    else if (speed > FAST_SPEED) {
        int boost = ACCEL_MILLI * (speed - FAST_SPEED);
        if (boost > MAX_BOOST_MILLI)
            boost = MAX_BOOST_MILLI;
        factor = 1000 + boost;
    } else {
        factor = 1000;
    }
    mult = GAIN_MILLI * factor / 1000;    /* milli-pixels per panel unit */
    g_pend_x += dx * mult;
    g_pend_y += dy * mult;
    /* The pool is a glide, not a queue: a finger that outruns the drain by
     * more than a screen's worth is a finger whose extra motion nobody wants
     * emitted three seconds later. Cap it, in milli-pixels. */
    if (g_pend_x > 2000000) g_pend_x = 2000000;
    else if (g_pend_x < -2000000) g_pend_x = -2000000;
    if (g_pend_y > 2000000) g_pend_y = 2000000;
    else if (g_pend_y < -2000000) g_pend_y = -2000000;
}

static int drain_axis(int *pend)
{
    int out = (*pend) * SMOOTH_MILLI / 1000 / 1000;
    /* A geometric drain never quite arrives; snap the last pixel out rather
     * than leaving the cursor half a pixel short of where the finger went. */
    if (!out && (*pend >= 500 || *pend <= -500))
        out = *pend > 0 ? 1 : -1;
    if (out) {
        *pend -= out * 1000;
        if (*pend < 500 && *pend > -500)
            *pend = 0;
    }
    return out;
}

static void drain_motion(c64_u64 now)
{
    if (now < g_next_drain)
        return;
    g_next_drain = now + DRAIN_MS;
    int ox = drain_axis(&g_pend_x);
    int oy = drain_axis(&g_pend_y);
    if (ox || oy)
        publish(ox, oy);
}

static void on_press(c64_u64 now)
{
    g_down = 1;
    g_down_at = now;
    g_moved = 0;
    g_fingers = 1;
    g_max_fingers = 1;
    g_pend_x = g_pend_y = 0;
    if (g_drag_open) {
        /* A tap's button is still held: this second touch turns it into a
         * drag rather than letting it complete as a click. */
        g_drag_open = 0;
        g_dragging = 1;
    }
}

static void on_release(c64_u64 now)
{
    if (!g_down)
        return;
    g_down = 0;
    g_pend_x = g_pend_y = 0;              /* no coasting after a lift */

    if (g_dragging) {
        g_dragging = 0;
        click(0);
        return;
    }
    c64_u64 dur = now - g_down_at;
    if (g_max_fingers >= 2) {
        /* Two fingers landing is inherently noisier than one, so the tap
         * window and the movement slop are both looser here. */
        if (dur < TAP_MS * 2 && g_moved < TAP_SLOP * 3) {
            click(2);                     /* right button down... */
            click(0);                     /* ...and straight back up */
        }
        return;
    }
    if (dur < TAP_MS && g_moved < TAP_SLOP) {
        click(1);
        g_drag_open = 1;
        g_drag_until = now + DRAG_MS;
    }
}

static void check_timers(c64_u64 now)
{
    if (g_drag_open && now >= g_drag_until) {
        g_drag_open = 0;
        click(0);                         /* the tap completes as a click */
    }
}

static void on_frame(c64_u32 cmd, const c64_u8 *p, unsigned n)
{
    if (cmd == CMD_INFO_VERSION && n >= 4) {
        /* string: a big-endian length then the bytes, no terminator */
        c64_u32 len = be32(p);
        char buf[48];
        unsigned i;
        if (len > n - 4)
            len = n - 4;
        if (len > sizeof buf - 1)
            len = sizeof buf - 1;
        for (i = 0; i < len; i++)
            buf[i] = (char)p[4 + i];
        buf[i] = 0;
        g_present = 1;
        /* OurCodi identifies itself as "OurCodi-..."; stock as "CODI:V...".
         * Only the former ever sends a MouseInfo. */
        g_ourcodi = (buf[0] == 'O' && buf[1] == 'u' && buf[2] == 'r');
        c64_logf("codi: firmware \"%s\"%s\n", buf,
                 g_ourcodi ? "" : " -- stock, which never forwards touch");
        return;
    }
    if (cmd != CMD_MOUSE_INFO || n < 5)
        return;

    c64_u64 now = ms_now();
    int dx = be16s(p + 1), dy = be16s(p + 3);
    if (!g_said_hello) {
        g_said_hello = 1;
        c64_log("codi: the rear panel is reporting touch\n");
    }
    switch (p[0]) {
    case MOUSE_PRESS:    on_press(now); break;
    case MOUSE_MOVE_REL: on_move(dx, dy); break;
    case MOUSE_RELEASE:  on_release(now); break;
    case MOUSE_FINGERS:
        g_fingers = dx < 1 ? 1 : dx;
        if (g_fingers > g_max_fingers)
            g_max_fingers = g_fingers;
        break;
    default: break;
    }
}

/* ---- bring-up: a probe that never blocks the shell ------------------------ *
 * Finding the pin pair means asking three questions and waiting for an answer
 * that may not come, and the first shape this took did that inline: three
 * 300 ms spins inside the shell's very first frame. It worked, and it was
 * still wrong. A driver that cannot find its device must not be able to stall
 * the machine for a second while it fails to -- the QEMU gate caught it as a
 * lost URC handshake, and on the device the same second is one where the
 * desktop is up and frozen.
 *
 * So the probe is a state machine the poll drives. init() arms the first
 * candidate and returns; each poll gives the current one a look and, when its
 * window is up, moves to the next. The whole walk is the same 900 ms of wall
 * clock, and the shell renders through all of it.
 */

enum { PR_PROBE = 0, PR_LIVE, PR_DEAD };
static int g_state;
static int g_cand;                       /* which pin pair is armed          */
static c64_u64 g_cand_until;
static c64_u32 g_rx_was, g_tx_was;       /* the armed pair's original modes  */

/* 300 ms is generous: the reply is thirty-odd bytes and the MCU answers a
 * version query immediately. Long enough that a busy firmware still lands
 * inside it, short enough that three misses are over in under a second. */
#define PROBE_MS 300

static void arm_candidate(int i)
{
    const struct pinpair *p = &k_pins[i];
    g_cand = i;
    g_rx_was = *(volatile c64_u32 *)p->rx_mode_reg;
    g_tx_was = *(volatile c64_u32 *)p->tx_mode_reg;
    pin_set(p, p->func);
    uart_setup(115200);
    uart_flush_rx();
    send_frame(CMD_GET_VERSION, 0, 0);
    g_cand_until = ms_now() + PROBE_MS;
}

/* A CoDi answered on the armed pair: finish the setup it was waiting for. */
static void adopt(void)
{
    g_pin = g_cand;
    g_state = PR_LIVE;
    c64_logf("codi: UART1 is on %s\n", k_pins[g_pin].name);

    /* The cover display: on at the brightness the Linux side settled on. It
     * is fire-and-forget and stock firmware ignores the command, so it costs
     * nothing to send either way -- and it replaces OurCodi's boot test
     * pattern with something deliberate. */
    {
        c64_u8 d[2] = { 1, 40 };
        send_frame(CMD_SET_DISPLAY, d, 2);
    }
    if (!g_ourcodi) {
        c64_log("codi: stock firmware -- cover display only, no touch\n");
        return;
    }
    set_mouse(1);
    g_next_rearm = ms_now() + REARM_MS;
    c64_log("codi: rear touchpad armed\n");
}

static void probe_tick(void)
{
    rx_fill();
    rx_parse(on_frame);
    if (g_present) {
        adopt();
        return;
    }
    if (ms_now() < g_cand_until)
        return;
    pin_restore(&k_pins[g_cand], g_rx_was, g_tx_was);
    if (g_cand + 1 < 3) {
        arm_candidate(g_cand + 1);
        return;
    }
    g_state = PR_DEAD;
    c64_log("codi: no cover MCU answered on any of the three UART1 pin pairs "
            "-- no rear touchpad. (If the panel is known good, the next thing "
            "to try is a baud sweep: this unit's MCU has run 3-8% fast since a "
            "deep battery pull.)\n");
}

void c64_codi_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    arm_candidate(0);
}

int c64_codi_present(void)
{
    return g_present && g_ourcodi;
}

void c64_codi_poll(void)
{
    c64_u64 now;

    if (g_state == PR_PROBE) {
        probe_tick();
        return;
    }
    if (g_state != PR_LIVE || !g_ourcodi)
        return;
    now = ms_now();

    /* Periodic re-arm, because a CoDi that rebooted (it is on its own
     * always-on rail and can be reflashed underneath us) forgets SetMouse.
     * NEVER mid-gesture: the firmware resets its contact tracking on
     * SetMouse, and doing that under a moving finger is a visible hitch. */
    if (!g_down && now >= g_next_rearm) {
        g_next_rearm = now + REARM_MS;
        set_mouse(1);
    }

    rx_fill();
    rx_parse(on_frame);
    check_timers(now);
    drain_motion(now);
}
