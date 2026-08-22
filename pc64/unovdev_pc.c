/* ===========================================================================
 * unovdev_pc - the legacy PC platform the guest kernel expects to find.
 *
 * Split out of unovdev.c, whose header says "virtio-mmio transport" and which
 * had quietly become half a PC chipset: a booting kernel talks to the 8254,
 * the CMOS and the 8250 long before it can drive a virtio device, and those
 * models belong together rather than interleaved with the descriptor-ring
 * walk.  unovdev.c keeps the transport; this file is everything a kernel
 * reaches over PORT I/O, which on x86 arrives pre-decoded (the exit carries
 * the port, size and direction - the one place x86 is easier than ARM).
 *
 * Vendor-neutral like the transport: nothing here knows what a VMCS is.
 * ======================================================================== */
#include "unovdev.h"
#include "unovirt.h"       /* guest-time, for the channel the guest LISTENS to */
#include "pc64_native.h"   /* the wall, for the channel the guest READS        */
#include <stdio.h>         /* snprintf, for the one status line               */

typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef unsigned long long u64;

/* ---- an 8254, because a kernel calibrates its clock against one ----------
 *
 * Linux measures the TSC by watching PIT channel 2 count down, and a machine
 * where that counter never moves is a machine where `quick_pit_calibrate`
 * never returns. It presented as a kernel that stopped printing after "DMI
 * not present or invalid." and sat on port 0x42 forever - which is why the
 * diagnostic that mattered was "which port is it on", not the log.
 *
 * The counter is driven by the REAL TSC rather than by a tick of our own, and
 * that is what makes the calibration come out right even though the guest
 * only runs in slices: the kernel is reading the same TSC we are, so both
 * sides of its ratio stop and start together. */
#define PIT_HZ 1193182ull
static struct { u16 initial; u64 start; int wr_hi, rd_hi; int armed; u8 cmos; } P;

static u64 pit_cycles_since(u64 start)
{
    u64 per_us = uno_native_tsc_per_us();
    u64 dt = uno_native_rdtsc() - start;
    if (!per_us) per_us = 1000ull;
    return (dt / per_us) * PIT_HZ / 1000000ull;
}

static u64 pit_elapsed(void) { return pit_cycles_since(P.start); }

/* ---- channel 0, the one wired to IRQ0 -------------------------------------
 * Channel 2 above is a counter the kernel READS; channel 0 is the counter the
 * kernel LISTENS to - with `nolapic` it is the system's only clock event,
 * programmed periodic (mode 2/3) by an old kernel and one-shot (mode 0/4) by
 * any kernel built with NO_HZ.
 *
 * AND IT COUNTS GUEST TIME, WHERE CHANNEL 2 COUNTS THE WALL.  The two are
 * genuinely different clocks and each channel needs the one it has: channel 2
 * is compared by the guest against the real TSC it reads directly, so it must
 * follow the wall or the calibration comes out wrong; channel 0 has to be
 * slower than the guest's own progress or its interrupt outruns its own
 * handler.  A quarter-core guest driven by a wall-time tick gets four periods
 * per period of service, re-enters the timer interrupt forever and never
 * returns to userspace - which is exactly how the shell first wedged, mid
 * `ls`, spinning on the PIC's mask register with its output frozen.
 *
 * Backlog is DROPPED on delivery, deliberately: a guest descheduled for four
 * periods gets one interrupt, not four back to back.  Jiffies run slow under
 * slicing either way (wall time in the guest comes from the TSC clocksource);
 * a catch-up burst would spend whole slices replaying a past nobody needs. */
static struct {
    u16 reload, latch;
    u64 start, delivered;                /* periods handed to the PIC       */
    int mode, wr_hi, rd_hi, armed, latched, oneshot_done;
} P0;

/* The same conversion as pit_cycles_since, against the guest's own clock. */
static u64 pit0_elapsed(void)
{
    u64 per_us = uno_native_tsc_per_us();
    u64 dt = uno_vmm_guest_cycles() - P0.start;
    if (!per_us) per_us = 1000ull;
    return (dt / per_us) * PIT_HZ / 1000000ull;
}

static u16 pit0_count(void)
{
    u64 el = pit0_elapsed();
    if (!P0.armed || !P0.reload) return 0;
    if (P0.mode == 2 || P0.mode == 3) return (u16)(P0.reload - el % P0.reload);
    return el >= P0.reload ? 0 : (u16)(P0.reload - el);
}

static int pit0_irq_pending(void)
{
    if (!P0.armed || !P0.reload) return 0;
    if (P0.mode == 2 || P0.mode == 3)
        return pit0_elapsed() / P0.reload > P0.delivered;
    return !P0.oneshot_done && pit0_elapsed() >= P0.reload;
}

static void pit0_irq_taken(void)
{
    if (P0.mode == 2 || P0.mode == 3)
        P0.delivered = pit0_elapsed() / P0.reload;
    else
        P0.oneshot_done = 1;
}

/* The CMOS speaks BCD, and status register B above says so. */
static u8 bcd(int v)
{
    if (v < 0) v = 0;
    return (u8)(((v / 10) % 10) << 4 | (v % 10));
}

static int pit_io(unsigned port, int is_write, unsigned long long *val)
{
    switch (port) {
    case 0x43:                                   /* the command register    */
        if (is_write) {
            unsigned ch = (unsigned)(*val >> 6) & 3;
            unsigned access = (unsigned)(*val >> 4) & 3;
            if (ch == 2) { P.wr_hi = 0; P.rd_hi = 0; }
            else if (ch == 0) {
                if (access == 0) {               /* latch the count         */
                    P0.latch = pit0_count();
                    P0.latched = 1;
                    P0.rd_hi = 0;
                } else {
                    P0.mode = (int)((*val >> 1) & 7);
                    P0.wr_hi = 0;
                    P0.rd_hi = 0;
                }
            }
        }
        return 1;
    case 0x40:                                   /* channel 0 data          */
        if (is_write) {
            if (!P0.wr_hi) { P0.reload = (u16)(*val & 0xFF); P0.wr_hi = 1; }
            else {
                P0.reload |= (u16)((*val & 0xFF) << 8);
                P0.wr_hi = 0;
                P0.start = uno_vmm_guest_cycles();
                P0.armed = 1;
                P0.delivered = 0;
                P0.oneshot_done = 0;
            }
        } else {
            u16 now = P0.latched ? P0.latch : pit0_count();
            *val = P0.rd_hi ? (now >> 8) : (now & 0xFF);
            if (P0.rd_hi) P0.latched = 0;        /* both halves read        */
            P0.rd_hi = !P0.rd_hi;
        }
        return 1;
    case 0x42:
        if (is_write) {
            if (!P.wr_hi) { P.initial = (u16)(*val & 0xFF); P.wr_hi = 1; }
            else {
                P.initial |= (u16)((*val & 0xFF) << 8);
                P.wr_hi = 0;
                P.start = uno_native_rdtsc();
                P.armed = 1;
            }
        } else {
            /* Wrapping, because mode 0 does not stop at zero - it carries on
             * decrementing, and a counter that sticks at 0 is one a caller
             * spins on just as happily as one that never moves. */
            u16 now = (u16)(P.initial - (u16)pit_elapsed());
            *val = P.rd_hi ? (now >> 8) : (now & 0xFF);
            P.rd_hi = !P.rd_hi;
        }
        return 1;
    case 0x70:                                   /* CMOS index              */
        if (is_write) P.cmos = (u8)(*val & 0x7F);
        return 1;
    case 0x71:                                   /* CMOS data               */
        /* A KERNEL READS THE RTC AND WAITS FOR IT. The default answer for an
         * absent port is all-ones, and in a STATUS register all-ones means
         * every flag is set - including update-in-progress, which the kernel
         * spins on until it clears. It never clears, so the boot ends there.
         * That is the same failure the PIT had, and the general lesson is
         * that 0xFF is a dangerous default: absent hardware should read as
         * quiet, not as busy. */
        /* THE REAL DATE, NOT A PLAUSIBLE ONE.  These fields used to be
         * constants - a fixed 2026-08-06 - which was fine for reaching a
         * shell and is not fine for anything that verifies a certificate: a
         * TLS chain is checked against the clock, so a guest fifteen days in
         * the past rejects every certificate issued since, and the error it
         * reports is `certificate verify failed`, which reads as a broken
         * network or a missing CA and is neither.  Cloudflare rotates
         * certificates weekly, so a hardcoded date does not merely risk this,
         * it guarantees it. */
        if (!is_write) {
            int y = 2026, mo = 8, d = 6, h = 12, mi = 0, s = 0;
            uno_native_rtc_read(&y, &mo, &d, &h, &mi, &s);
            switch (P.cmos) {
            case 0x00: *val = bcd(s); break;     /* seconds, BCD            */
            case 0x02: *val = bcd(mi); break;    /* minutes                 */
            case 0x04: *val = bcd(h); break;     /* hours                   */
            case 0x06: *val = 0x04; break;       /* weekday, nobody checks  */
            case 0x07: *val = bcd(d); break;     /* day                     */
            case 0x08: *val = bcd(mo); break;    /* month                   */
            case 0x09: *val = bcd(y % 100); break;          /* year         */
            case 0x0A: *val = 0x26; break;       /* status A: UIP CLEAR     */
            case 0x0B: *val = 0x02; break;       /* status B: 24-hour, BCD  */
            case 0x0D: *val = 0x80; break;       /* battery good            */
            case 0x32: *val = bcd(y / 100); break;          /* century      */
            default:   *val = 0x00; break;
            }
        }
        return 1;
    case 0x61:
        if (is_write) { if (*val & 1) P.start = uno_native_rdtsc(); }
        else {
            /* Bit 5 is OUT2: high once the count has run out. Bit 4 is the
             * refresh toggle, which some code watches to prove time passes. */
            int out2 = (P.armed && pit_elapsed() >= P.initial) ? 1 : 0;
            *val = (unsigned)(out2 << 5) | (unsigned)((pit_elapsed() & 1) << 4) | 1;
        }
        return 1;
    default:
        return 0;
    }
}

/* ---- an 8259 pair, because a byte nobody announces is a byte nobody reads -
 *
 * The 8250 driver is interrupt-driven: it enables the receive interrupt and
 * waits for IRQ4, and a guest with no interrupt controller never wakes it -
 * the RX FIFO fills, LSR says data-ready, and the blocking read sleeps
 * forever.  `nolapic` means Linux runs the legacy PIC, so the legacy PIC is
 * what gets modelled: ICW1-4 sequencing, the mask register, EOI in both
 * flavours, and OCW3's register select.  The slave exists because the init
 * sequence programs it; nothing is wired to it.
 *
 * The lines themselves are LEVELS recomputed at ask time rather than events
 * pushed at edge time: the IRR follows the wires, the ISR blocks redelivery
 * until EOI, and a level source that is still asserted after EOI re-raises -
 * which is exactly the 8250's contract (drain RBR or mask IER to deassert). */
typedef struct {
    u8 imr, irr, isr, base;
    int icw;                     /* 0 = ready; 1..3 = expecting ICW2..ICW4  */
    int icw4_needed, read_isr, inited;
} uno_pic;
static uno_pic PICM, PICS;

static void pic_cmd(uno_pic *p, u8 v)            /* port 0x20 / 0xA0 write  */
{
    if (v & 0x10) {                              /* ICW1: init starts over  */
        p->icw = 1;
        p->icw4_needed = v & 1;
        p->imr = 0; p->irr = 0; p->isr = 0;
        p->read_isr = 0;
        p->inited = 0;
    } else if (v & 0x08) {                       /* OCW3                    */
        if ((v & 3) == 3) p->read_isr = 1;
        else if ((v & 3) == 2) p->read_isr = 0;
    } else {                                     /* OCW2                    */
        unsigned op = v >> 5;                    /* R | SL | EOI            */
        if (op == 1) {                           /* non-specific EOI        */
            int i;
            for (i = 0; i < 8; i++)
                if (p->isr & (1u << i)) { p->isr &= (u8)~(1u << i); break; }
        } else if (op == 3 || op == 7) {         /* specific EOI (+rotate)  */
            p->isr &= (u8)~(1u << (v & 7));
        }
    }
}

static void pic_data(uno_pic *p, u8 v)           /* port 0x21 / 0xA1 write  */
{
    switch (p->icw) {
    case 1: p->base = v & 0xF8; p->icw = 2; break;              /* ICW2     */
    case 2: p->icw = p->icw4_needed ? 3 : 0;
            p->inited = !p->icw; break;                         /* ICW3     */
    case 3: p->icw = 0; p->inited = 1; break;                   /* ICW4     */
    default: p->imr = v; break;                                 /* OCW1     */
    }
}

static int uart_irq_level(void);                 /* below, needs U          */
static int i8042_kbd_irq_level(void);            /* below, needs K8         */
static int i8042_aux_irq_level(void);

/* The IRR follows the wires.  Called before any read or arbitration, so a
 * guest inspecting the IRR sees the same lines the injector does.
 *
 * THE SLAVE IS WIRED NOW (A8): the aux mouse rides IRQ12, which is slave
 * line 4, and the slave's deliverable state asserts master line 2 - the
 * cascade.  Before A8 the slave was programmed by the init sequence and
 * nothing ever raised it, so the arbitration below never had to look. */
static void pic_refresh(void)
{
    u8 irr = 0, sirr = 0;
    int i;
    if (pit0_irq_pending())       irr |= 1u << 0;
    if (i8042_kbd_irq_level())    irr |= 1u << 1;
    if (uart_irq_level())         irr |= 1u << 4;
    /* The virtio transports, which live in unovdev.c and are told their line
     * on the guest's own command line (`...@0xd0000000:5`). Their level is a
     * level in the same sense the UART's received-data is: it stays up until
     * the driver writes InterruptACK, so a driver that returns from its
     * handler without acknowledging is interrupted again - which is what the
     * register is for and what a real device does. */
    for (i = 5; i <= 7; i++)
        if (uno_vdev_mmio_irq_level(i)) irr |= (u8)(1u << i);
    if (i8042_aux_irq_level())    sirr |= 1u << 4;          /* IRQ12        */
    PICS.irr = sirr;
    /* The cascade: the slave raises master line 2 while it has something
     * deliverable - masked lines do not travel, and a request in service on
     * the slave blocks its own and lower priorities exactly as on the
     * master. */
    if (PICS.inited) {
        u8 sready = PICS.irr & (u8)~PICS.imr;
        for (i = 0; i < 8; i++) {
            if (PICS.isr & (1u << i)) break;
            if (sready & (1u << i)) { irr |= 1u << 2; break; }
        }
    }
    PICM.irr = irr;
}

/* Arbitrate the slave: the line it would put on the cascade, or -1. */
static int pic_slave_pick(void)
{
    u8 sready;
    int i;
    if (!PICS.inited) return -1;
    sready = PICS.irr & (u8)~PICS.imr;
    for (i = 0; i < 8; i++) {
        if (PICS.isr & (1u << i)) return -1;
        if (sready & (1u << i)) return i;
    }
    return -1;
}

static int pic_io(unsigned port, int is_write, unsigned long long *val)
{
    uno_pic *p;
    switch (port) {
    case 0x20: case 0x21: p = &PICM; break;
    case 0xA0: case 0xA1: p = &PICS; break;
    case 0x4D0: case 0x4D1:                      /* ELCR: all edge, the ISA
                                                    default - NOT the absent-
                                                    hardware 0xFF, which would
                                                    claim every line is level */
        if (!is_write) *val = 0;
        return 1;
    default: return 0;
    }
    if (is_write) {
        if (port & 1) pic_data(p, (u8)(*val & 0xFF));
        else          pic_cmd(p, (u8)(*val & 0xFF));
    } else {
        if (p == &PICM) pic_refresh();
        *val = (port & 1) ? p->imr : (p->read_isr ? p->isr : p->irr);
    }
    return 1;
}

/* What the injector asks.  Fixed priority, IRQ0 highest; a request is
 * deliverable only when nothing of equal or higher priority is in service.
 * `take` commits: the ISR bit goes up and an edge source (the PIT) marks its
 * period delivered.  Committing and asking are one call, because a pending
 * answer that a later take contradicts is a race with one caller. */
int uno_vdev_irq_pending(void)
{
    u8 ready;
    int i;
    if (!PICM.inited) return 0;
    pic_refresh();
    ready = PICM.irr & (u8)~PICM.imr;
    if (!ready) return 0;
    for (i = 0; i < 8; i++) {
        if (PICM.isr & (1u << i)) return 0;
        if (ready & (1u << i)) {
            /* Line 2 is the cascade: it is only pending if the slave still
             * has something to put on it at take time. pic_refresh already
             * required that, but re-ask so pending and take cannot split. */
            if (i == 2) return pic_slave_pick() >= 0;
            return 1;
        }
    }
    return 0;
}

int uno_vdev_irq_take(void)
{
    u8 ready;
    int i;
    if (!PICM.inited) return -1;
    pic_refresh();
    ready = PICM.irr & (u8)~PICM.imr;
    for (i = 0; i < 8; i++) {
        if (PICM.isr & (1u << i)) return -1;
        if (ready & (1u << i)) {
            if (i == 2) {
                /* A slave interrupt is taken on BOTH chips: the master's
                 * cascade line goes in service, the slave's own line goes in
                 * service, and the vector is the SLAVE's base - which is why
                 * the guest EOIs both, and why pic_cmd needs no special case
                 * for it. */
                int s = pic_slave_pick();
                if (s < 0) return -1;
                PICM.isr |= 1u << 2;
                PICS.isr |= (u8)(1u << s);
                return (int)PICS.base + s;
            }
            PICM.isr |= (u8)(1u << i);
            if (i == 0) pit0_irq_taken();
            return (int)PICM.base + i;
        }
    }
    return -1;
}

#define COM1 0x3F8
static struct {
    char line[160];
    int n, chars;
    u8 mcr, ier, lcr;
    int thre;                /* transmit-empty interrupt LATCHED, see below  */
} U;

/* ---- the receive half, without which a console is a closed one -----------
 * A shell whose stdin never delivers a byte exits, and the exit code says so:
 * "Attempted to kill init! exitcode=0x00000000" is a shell that reached
 * end-of-file. Output alone was enough to watch a kernel boot and is not
 * enough to hold a shell open.
 *
 * THE SEED IS HANDED OVER WHEN THE DRIVER ARMS THE RECEIVE INTERRUPT, not on
 * an LSR poll as it first was.  The poll-time feed looked reasonable and had
 * a flaw with a delay on it: the driver's own startup CLEARS the receive
 * registers (it reads RBR once to drain stale state) before it enables
 * interrupts, so bytes fed early get partially eaten by the very code path
 * that is about to want them.  IER bit 0 going live is the one moment that is
 * both after the last clear-read and before the first blocking read.  Real
 * keystrokes come from the frame loop, which already owns the keyboard. */
static struct {
    u8 buf[512]; int head, tail; const char *seed;
    int pm, prompt;              /* "~ # " matcher over the TX stream        */
} RX;

static int rx_empty(void) { return RX.head == RX.tail; }

/* THE SEED WAITS FOR THE PROMPT.  Handing it over when the receive interrupt
 * arms looked airtight and was not: the 6.12 driver arms IER during its
 * probe and its open-time drain reads RBR to clear "stale" state, so the
 * seed's head got eaten and the shell ran the mangled tail ("sh: linkth0:
 * not found" was `busybox ip link set eth0 up` after the drain).  The one
 * moment a shell is provably ready to read is when it has just printed its
 * prompt - so the TX stream is watched for "~ # ", and the seed is held
 * until it has gone by. */
static void rx_tx_watch(char c)
{
    static const char P[] = "~ # ";
    if (RX.prompt) return;
    if (c == P[RX.pm]) {
        RX.pm++;
        if (!P[RX.pm]) RX.prompt = 1;
    } else {
        RX.pm = (c == P[0]) ? 1 : 0;
    }
}

void uno_vdev_serial_push(int c)
{
    int nxt = (RX.tail + 1) % (int)sizeof RX.buf;
    if (nxt == RX.head) return;                  /* full: drop, do not wrap  */
    RX.buf[RX.tail] = (u8)c;
    RX.tail = nxt;
}

void uno_vdev_serial_seed(const char *s) { RX.seed = s; }

static void rx_feed_seed(void)
{
    const char *s = RX.seed;
    if (!s || !RX.prompt) return;            /* not until the shell says so  */
    RX.seed = 0;
    while (*s) uno_vdev_serial_push(*s++);
}

/* The wire into the PIC.  MCR.OUT2 gates the IRQ line off the chip on every
 * PC ever built (the driver sets it when it wants interrupts, and autoconfig's
 * loopback tests run with it clear - which is what keeps those from ringing
 * the PIC); IER says which causes are armed; and then the cause itself.
 *
 * RECEIVED-DATA IS A LEVEL AND TRANSMIT-EMPTY IS A LATCH, and conflating them
 * is a livelock.  Data-ready follows the FIFO and deasserts when the driver
 * drains RBR, which is genuinely a level.  But this transmitter never fills,
 * so "holding register empty" is true forever - and on a real 8250 that does
 * not mean the interrupt asserts forever: THRE raises a latched request that
 * READING IIR CLEARS.  Modelled as a level it cannot be quieted by anything
 * the driver does, so the guest re-enters the handler for as long as it has
 * transmit interrupts enabled.  That is what wedged the shell mid-`ls`: the
 * kernel had finally enabled interrupt-driven transmit for a long output, and
 * from then on it did nothing but service IRQ4, output frozen, spinning on the
 * PIC's mask register (handle_level_irq masks and unmasks around every one). */
static int uart_irq_level(void)
{
    if (!(U.mcr & 0x08)) return 0;               /* OUT2 gate                */
    if (U.ier & 0x01) {
        if (rx_empty()) rx_feed_seed();          /* armed: hand the seed over */
        if (!rx_empty()) return 1;
    }
    return ((U.ier & 0x02) && U.thre) ? 1 : 0;
}

/* ---- an i8042, because the guest needs input the kernel already drives ----
 *
 * A8's finding forced this: virtio-input CANNOT cross a virtio-mmio v2
 * transport with a stock kernel - the driver reads its capability bitmaps
 * with one config access of arbitrary length, and vm_get() BUG()s on any
 * length that is not 1, 2, 4 or 8.  Every distro also ships virtio_input=m,
 * so even a correct transport would find no driver in an initramfs.  The
 * i8042 has neither problem: SERIO_I8042 and KEYBOARD_ATKBD are =y in every
 * kernel that has ever booted on a PC, and its whole protocol is byte-wide.
 *
 * Two queues rather than one buffer, each byte knowing which device it came
 * from, because that is what the AUX status bit reports and the driver
 * routes on it.  Controller and keyboard responses read back with AUX clear;
 * mouse bytes and the aux loopback read back with AUX set.  When both have
 * something, the keyboard goes first and the mouse's interrupt waits - the
 * status register and the read must agree on which byte is next, so one rule
 * decides for both.
 *
 * Scancodes go out in SET 1, which is what the driver sees on every real PC:
 * it asks for translation (CCB bit 6) and translated set 2 IS set 1.  The
 * host hands us characters and EFI scan codes, not key events, so a
 * keystroke is synthesised as press+release with the shift/ctrl wrapping the
 * table demands - unoui has no key-up event to forward (UI_EV_KEY is "a key
 * went DOWN"), and until it does, held keys cannot cross into the guest. */
#define K8Q 256
static struct {
    u8  ccb;                    /* bit0/1 kbd/aux irq, 4/5 disable, 6 xlate */
    int inited;
    int expect;                 /* controller command awaiting its data byte */
    int kbd_expect, aux_expect; /* device command awaiting its parameter    */
    u8  kq[K8Q]; int kh, kt;    /* keyboard + controller -> host            */
    u8  aq[K8Q]; int ah, at;    /* aux (mouse) -> host                      */
    int aux_stream;             /* 0xF4 seen: movement packets flow         */
    int aux_id;                 /* 0 = plain PS/2, 3 = IntelliMouse (wheel) */
    u8  rates[2];               /* the sample-rate knock, two most recent   */
    int keys, packets, dropped; /* for the status line                      */
} K8;

static void k8_init(void)
{
    if (K8.inited) return;
    K8.inited = 1;
    K8.ccb = 0x74;              /* translate on, both devices off, SYS      */
}

static int kq_n(void) { return (K8.kt - K8.kh + K8Q) % K8Q; }
static int aq_n(void) { return (K8.at - K8.ah + K8Q) % K8Q; }

static void kq_push(u8 b)
{
    int nxt = (K8.kt + 1) % K8Q;
    if (nxt == K8.kh) { K8.dropped++; return; }
    K8.kq[K8.kt] = b; K8.kt = nxt;
}
static void aq_push(u8 b)
{
    int nxt = (K8.at + 1) % K8Q;
    if (nxt == K8.ah) { K8.dropped++; return; }
    K8.aq[K8.at] = b; K8.at = nxt;
}

/* The interrupt lines are levels over the queues, gated the way the real
 * chip gates them: the CCB enable bits.  The aux line additionally waits for
 * the keyboard queue to drain, because the status register presents the
 * keyboard byte first and an IRQ12 whose read returns a keyboard byte sends
 * the byte to the wrong driver. */
static int i8042_kbd_irq_level(void)
{ return (K8.ccb & 0x01) && kq_n() > 0; }
static int i8042_aux_irq_level(void)
{ return (K8.ccb & 0x02) && kq_n() == 0 && aq_n() > 0; }

/* The keyboard device's half of a command conversation (bytes written to
 * 0x60 with no controller command pending). */
static void k8_kbd_cmd(u8 v)
{
    if (K8.kbd_expect) {
        int was = K8.kbd_expect;
        K8.kbd_expect = 0;
        kq_push(0xFA);
        if (was == 0xF0 && v == 0) kq_push(0x02);   /* current set: 2       */
        return;
    }
    switch (v) {
    case 0xFF: kq_push(0xFA); kq_push(0xAA); break; /* reset: ACK, BAT OK   */
    case 0xF2: kq_push(0xFA); kq_push(0xAB); kq_push(0x83); break;  /* ID   */
    case 0xEE: kq_push(0xEE); break;                /* echo                 */
    case 0xED: case 0xF3: case 0xF0:                /* LEDs, rate, set      */
        kq_push(0xFA); K8.kbd_expect = v; break;
    default:   kq_push(0xFA); break;                /* enable/disable/etc   */
    }
}

/* The mouse's half.  The sample-rate knock 200,100,80 is how a driver asks
 * "are you an IntelliMouse", and answering ID 3 afterwards is what turns the
 * fourth (wheel) byte on.  psmouse also probes for fancier hardware with 0xE8
 * resolution sequences; plain ACKs and a generic status read answer those as
 * "no". */
static void k8_aux_cmd(u8 v)
{
    if (K8.aux_expect) {
        int was = K8.aux_expect;
        K8.aux_expect = 0;
        if (was == 0xF3) {
            if (K8.rates[0] == 200 && K8.rates[1] == 100 && v == 80)
                K8.aux_id = 3;
            K8.rates[0] = K8.rates[1]; K8.rates[1] = v;
        }
        aq_push(0xFA);
        return;
    }
    switch (v) {
    case 0xFF:                                      /* reset                */
        K8.aux_stream = 0; K8.aux_id = 0;
        K8.rates[0] = K8.rates[1] = 0;
        aq_push(0xFA); aq_push(0xAA); aq_push(0x00);
        break;
    case 0xF2: aq_push(0xFA); aq_push((u8)K8.aux_id); break;
    case 0xF4: aq_push(0xFA); K8.aux_stream = 1; break;
    case 0xF5: aq_push(0xFA); K8.aux_stream = 0; break;
    case 0xE8: case 0xF3: aq_push(0xFA); K8.aux_expect = v; break;
    case 0xE9:                                      /* status               */
        aq_push(0xFA); aq_push(0x00); aq_push(0x02); aq_push(0x64);
        break;
    case 0xEB:                                      /* read data: one packet */
        aq_push(0xFA); aq_push(0x08); aq_push(0x00); aq_push(0x00);
        if (K8.aux_id == 3) aq_push(0x00);
        break;
    default:   aq_push(0xFA); break;                /* scaling and friends  */
    }
}

static int i8042_io(unsigned port, int is_write, unsigned long long *val)
{
    if (port != 0x60 && port != 0x64) return 0;
    k8_init();

    if (port == 0x64 && !is_write) {                /* status               */
        int obf = kq_n() > 0 || aq_n() > 0;
        int aux = kq_n() == 0 && aq_n() > 0;
        /* BIT 4 IS THE INHIBIT SWITCH, AND IT MEANS "UNLOCKED" WHEN SET.
         * Leaving it clear is how a machine says its keyboard is physically
         * key-locked, and Linux says so out loud - `i8042: Warning: Keylock
         * active` in a boot log that was otherwise clean.  Real hardware
         * then declines to deliver keystrokes at all, so a guest is entitled
         * to believe it.  Every emulator sets this bit; so do we now. */
        *val = 0x04u | 0x10u | (obf ? 0x01u : 0u) | (aux ? 0x20u : 0u);
        return 1;
    }
    if (port == 0x60 && !is_write) {                /* pop, keyboard first  */
        if (kq_n())      { *val = K8.kq[K8.kh]; K8.kh = (K8.kh + 1) % K8Q; }
        else if (aq_n()) { *val = K8.aq[K8.ah]; K8.ah = (K8.ah + 1) % K8Q; }
        else               *val = 0;
        return 1;
    }
    if (port == 0x64) {                             /* controller command   */
        u8 v = (u8)(*val & 0xFF);
        switch (v) {
        case 0x20: kq_push(K8.ccb); break;
        case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4:
            K8.expect = v; break;
        case 0xAA:                                  /* self-test            */
            K8.kh = K8.kt = K8.ah = K8.at = 0;
            kq_push(0x55);
            break;
        case 0xAB: kq_push(0x00); break;            /* kbd interface test   */
        case 0xA9: kq_push(0x00); break;            /* aux interface test   */
        case 0xA7: K8.ccb |= 0x20;  break;          /* aux off              */
        case 0xA8: K8.ccb &= (u8)~0x20; break;      /* aux on               */
        case 0xAD: K8.ccb |= 0x10;  break;          /* kbd off              */
        case 0xAE: K8.ccb &= (u8)~0x10; break;      /* kbd on               */
        case 0xC0: kq_push(0x00); break;            /* input port           */
        case 0xD0: kq_push(0x03); break;            /* output port: A20 on  */
        default: break;                             /* pulses and the rest  */
        }
        return 1;
    }
    /* port 0x60, write: a parameter if one is owed, else the keyboard. */
    {
        u8 v = (u8)(*val & 0xFF);
        switch (K8.expect) {
        case 0x60: K8.ccb = v;      K8.expect = 0; return 1;
        case 0xD1:                  K8.expect = 0; return 1;   /* A20 etc  */
        case 0xD2: kq_push(v);      K8.expect = 0; return 1;
        case 0xD3: aq_push(v);      K8.expect = 0; return 1;   /* loopback */
        case 0xD4: k8_aux_cmd(v);   K8.expect = 0; return 1;
        default:   k8_kbd_cmd(v);                  return 1;
        }
    }
}

/* ---- what the host types and points, translated --------------------------
 *
 * The shell hands over what its input layer has: an ASCII character or an
 * EFI SimpleTextIn scan code, both key-DOWN edges only.  Set-1 make/break
 * pairs are synthesised around each, with shift or ctrl wrapped where the
 * character demands it.  The gate is the CCB interrupt-enable bit: bytes
 * pushed before the guest's driver is listening would sit in front of the
 * probe conversation and corrupt it. */
static const struct { u8 code, shift; } K8_ASC[95] = {
    {0x39,0},{0x02,1},{0x28,1},{0x04,1},{0x05,1},{0x06,1},{0x08,1},{0x28,0},
    {0x0A,1},{0x0B,1},{0x09,1},{0x0D,1},{0x33,0},{0x0C,0},{0x34,0},{0x35,0},
    {0x0B,0},{0x02,0},{0x03,0},{0x04,0},{0x05,0},{0x06,0},{0x07,0},{0x08,0},
    {0x09,0},{0x0A,0},{0x27,1},{0x27,0},{0x33,1},{0x0D,0},{0x34,1},{0x35,1},
    {0x03,1},{0x1E,1},{0x30,1},{0x2E,1},{0x20,1},{0x12,1},{0x21,1},{0x22,1},
    {0x23,1},{0x17,1},{0x24,1},{0x25,1},{0x26,1},{0x32,1},{0x31,1},{0x18,1},
    {0x19,1},{0x10,1},{0x13,1},{0x1F,1},{0x14,1},{0x16,1},{0x2F,1},{0x11,1},
    {0x2D,1},{0x15,1},{0x2C,1},{0x1A,0},{0x2B,0},{0x1B,0},{0x07,1},{0x0C,1},
    {0x29,0},{0x1E,0},{0x30,0},{0x2E,0},{0x20,0},{0x12,0},{0x21,0},{0x22,0},
    {0x23,0},{0x17,0},{0x24,0},{0x25,0},{0x26,0},{0x32,0},{0x31,0},{0x18,0},
    {0x19,0},{0x10,0},{0x13,0},{0x1F,0},{0x14,0},{0x16,0},{0x2F,0},{0x11,0},
    {0x2D,0},{0x15,0},{0x2C,0},{0x1A,1},{0x2B,1},{0x1B,1},{0x29,1}
};

static void k8_tap(u8 code, int shift, int ctrl, int ext)
{
    if (!(K8.ccb & 0x01)) return;                   /* nobody is listening  */
    if (shift) kq_push(0x2A);
    if (ctrl)  kq_push(0x1D);
    if (ext)   kq_push(0xE0);
    kq_push(code);
    if (ext)   kq_push(0xE0);
    kq_push((u8)(code | 0x80));
    if (ctrl)  kq_push(0x1D | 0x80);
    if (shift) kq_push(0x2A | 0x80);
    K8.keys++;
}

void uno_vdev_kbd_char(int ch)
{
    k8_init();
    if (ch == '\r' || ch == '\n') { k8_tap(0x1C, 0, 0, 0); return; }
    if (ch == '\b' || ch == 127)  { k8_tap(0x0E, 0, 0, 0); return; }
    if (ch == '\t')               { k8_tap(0x0F, 0, 0, 0); return; }
    if (ch == 27)                 { k8_tap(0x01, 0, 0, 0); return; }
    if (ch >= 1 && ch <= 26) {                      /* ^A..^Z               */
        k8_tap(K8_ASC['a' - 32 + ch - 1].code, 0, 1, 0);
        return;
    }
    if (ch >= 32 && ch < 127)
        k8_tap(K8_ASC[ch - 32].code, K8_ASC[ch - 32].shift, 0, 0);
}

void uno_vdev_kbd_scan(int efi_scan)
{
    /* EFI SimpleTextIn scan codes, which is the space the shell's key events
     * already speak (hid_kbd.h). */
    static const struct { u8 efi, code, ext; } T[] = {
        {0x01, 0x48, 1}, {0x02, 0x50, 1}, {0x03, 0x4D, 1}, {0x04, 0x4B, 1},
        {0x05, 0x47, 1}, {0x06, 0x4F, 1}, {0x07, 0x52, 1}, {0x08, 0x53, 1},
        {0x09, 0x49, 1}, {0x0A, 0x51, 1},
        {0x0B, 0x3B, 0}, {0x0C, 0x3C, 0}, {0x0D, 0x3D, 0}, {0x0E, 0x3E, 0},
        {0x0F, 0x3F, 0}, {0x10, 0x40, 0}, {0x11, 0x41, 0}, {0x12, 0x42, 0},
        {0x13, 0x43, 0}, {0x14, 0x44, 0}, {0x15, 0x57, 0}, {0x16, 0x58, 0},
        {0x17, 0x01, 0}
    };
    unsigned i;
    k8_init();
    for (i = 0; i < sizeof T / sizeof T[0]; i++)
        if ((int)T[i].efi == efi_scan) { k8_tap(T[i].code, 0, 0, T[i].ext); return; }
}

/* One movement packet.  dx/dy in pixels (screen convention, y down); PS/2 y
 * is up, so it flips here.  `buttons` is unoui's: bit0 left, bit1 right,
 * bit2 middle - which happens to be the PS/2 order too. */
void uno_vdev_mouse(int dx, int dy, unsigned buttons, int wheel)
{
    u8 b0;
    k8_init();
    if (!K8.aux_stream || !(K8.ccb & 0x02)) return;
    if (aq_n() > K8Q - 8) { K8.dropped++; return; } /* stay ahead of typing */
    dy = -dy;                                       /* PS/2 y counts up     */
    /* A MOVEMENT LARGER THAN ONE PACKET IS SPLIT, not clipped.  A PS/2 packet
     * carries +-255, and clipping a long drag silently shortens it - which
     * makes the guest's pointer drift away from the host's by exactly the
     * amount thrown away, and no amount of aiming can correct a target that
     * moves when you reach for it. */
    while (dx > 255 || dx < -255 || dy > 255 || dy < -255) {
        int sx = dx > 255 ? 255 : (dx < -255 ? -255 : dx);
        int sy = dy > 255 ? 255 : (dy < -255 ? -255 : dy);
        u8 h = (u8)(0x08 | (buttons & 7)
                  | (u8)((sx < 0) ? 0x10 : 0) | (u8)((sy < 0) ? 0x20 : 0));
        if (aq_n() > K8Q - 8) { K8.dropped++; return; }
        aq_push(h);
        aq_push((u8)(sx & 0xFF));
        aq_push((u8)(sy & 0xFF));
        if (K8.aux_id == 3) aq_push(0);
        K8.packets++;
        dx -= sx;
        dy -= sy;
    }
    b0 = 0x08 | (u8)(buttons & 7)
       | (u8)((dx < 0) ? 0x10 : 0) | (u8)((dy < 0) ? 0x20 : 0);
    aq_push(b0);
    aq_push((u8)(dx & 0xFF));
    aq_push((u8)(dy & 0xFF));
    if (K8.aux_id == 3) {
        int z = -wheel;                             /* wheel down = z up    */
        if (z < -8) z = -8; if (z > 7) z = 7;
        aq_push((u8)(z & 0x0F));
    }
    K8.packets++;
}

int uno_vdev_input_str(char *buf, int cap)
{
    k8_init();
    return snprintf(buf, (unsigned)cap,
                    "ccb %02x, %d keys %d packets %d dropped, mouse %s id %d",
                    K8.ccb, K8.keys, K8.packets, K8.dropped,
                    K8.aux_stream ? "streaming" : "idle", K8.aux_id);
}

int uno_vdev_pio(unsigned port, int is_write, unsigned size,
                 unsigned long long *val, void (*sink)(const char *))
{
    (void)size;
    if (pit_io(port, is_write, val)) return 1;
    if (pic_io(port, is_write, val)) return 1;
    if (i8042_io(port, is_write, val)) return 1;
    if (port < COM1 || port > COM1 + 7) {
        /* Everything else answers as absent hardware: reads all-ones, writes
         * dropped. A kernel probing a port that is not there gets the same
         * answer it would on a machine without the device, which is a thing
         * it already knows how to handle. */
        if (!is_write) *val = 0xFFFFFFFFull;
        return 1;
    }
    if (is_write) {
        if (port == COM1 + 4) U.mcr = (u8)(*val & 0xFF);
        if (port == COM1 + 3) U.lcr = (u8)(*val & 0xFF);
        /* DLAB (LCR bit 7) redirects the first two ports at the divisor
         * latch.  Unmodelled, the driver's baud programming lands where it
         * must not: the DLL byte becomes a character on the console and the
         * DLM byte OVERWRITES THE IER - the divisor for 115200 is 0x0001, so
         * setting the speed silently disarmed every interrupt the driver had
         * just enabled.  The latch itself needs no storage: the baud rate of
         * a port with no wire is nobody's business. */
        if (port == COM1 + 1 && !(U.lcr & 0x80)) {
            /* Arming transmit interrupts on an already-empty transmitter
             * raises the request there and then - which is how the driver
             * gets the first one, since nothing else will happen until it
             * does. */
            if (((*val & 0x02) && !(U.ier & 0x02))) U.thre = 1;
            U.ier = (u8)(*val & 0xFF);
        }
        if (port == COM1 && !(U.lcr & 0x80)) {
            char c = (char)(*val & 0xFF);
            U.chars++;
            U.thre = 1;                  /* it emptied again, immediately    */
            rx_tx_watch(c);              /* the seed waits for the prompt    */
            if (c == '\n' || U.n >= (int)sizeof U.line - 1) {
                U.line[U.n] = 0;
                if (sink && U.n) sink(U.line);
                U.n = 0;
            } else if (c != '\r') {
                U.line[U.n++] = c;
            }
        }
        return 1;
    }
    switch (port) {
    case COM1 + 0:                       /* RBR: the byte itself            */
        if (U.lcr & 0x80) { *val = 0; break; }        /* DLL reads back 0   */
        if (rx_empty()) { *val = 0; break; }
        *val = RX.buf[RX.head];
        RX.head = (RX.head + 1) % (int)sizeof RX.buf;
        break;
    case COM1 + 5:
        /* LSR. Bit 0 is data-ready and it is the whole difference: a driver
         * only reads RBR when this says there is something there. */
        *val = 0x60 | (rx_empty() ? 0u : 1u);
        break;
    case COM1 + 2:
        /* IIR, highest cause first: received data (0x04) outranks transmit
         * holding empty (0x02); 0x01 is "none".  A handler that reads 0x01
         * while its IRQ line is up concludes the interrupt was not this
         * device, and the kernel retires the whole IRQ as 'nobody cared' -
         * so this register agreeing with uart_irq_level() is load-bearing.
         *
         * AND READING IT CLEARS THE TRANSMIT REQUEST, which is not a detail:
         * it is the only way that request is ever cleared, and hence the only
         * thing that lets the guest leave the handler. */
        if ((U.mcr & 0x08) && (U.ier & 0x01) && !rx_empty()) *val = 0x04;
        else if ((U.mcr & 0x08) && (U.ier & 0x02) && U.thre) {
            *val = 0x02;
            U.thre = 0;
        } else                                               *val = 0x01;
        break;
    case COM1 + 6:
        /* MSR, WITH LOOPBACK. The 8250 driver's autoconfig writes MCR with
         * LOOP set and checks that the modem inputs follow the outputs; a
         * port that answers a constant fails that test and is registered as
         * hardware the driver will not really drive. Reflecting the four bits
         * is the whole of what it wants. */
        if (U.mcr & 0x10)
            *val = (unsigned)(((U.mcr & 0x01) << 5)   /* DTR  -> DSR        */
                            | ((U.mcr & 0x02) << 3)   /* RTS  -> CTS        */
                            | ((U.mcr & 0x04) << 4)   /* OUT1 -> RI         */
                            | ((U.mcr & 0x08) << 4)); /* OUT2 -> DCD        */
        else
            *val = 0xB0;                 /* carrier, DSR, CTS               */
        break;
    case COM1 + 1: *val = (U.lcr & 0x80) ? 0 : U.ier; break;   /* IER / DLM */
    case COM1 + 3: *val = U.lcr; break;  /* LCR, saved/restored by autoconfig */
    case COM1 + 4: *val = U.mcr; break;
    default:       *val = 0x00; break;
    }
    return 1;
}

int uno_vdev_serial_chars(void) { return U.chars; }

/* The legacy platform's state in one word, for the "still going" trace: a
 * guest that has gone quiet is either not writing or writing somewhere we
 * are not listening, and LCR.DLAB, IER and the PIC's mask are the three
 * registers that decide which. */
unsigned uno_vdev_pc_state(void)
{
    return (unsigned)U.lcr
         | ((unsigned)U.ier << 8)
         | ((unsigned)U.mcr << 16)
         | ((unsigned)PICM.imr << 24);
}

/* The interrupt path's own state, for the same trace: whether a line is up
 * (IRR), whether one is still in service and therefore blocking every other
 * (ISR), whether the chip has been initialised at all, and what the timer
 * channel is doing.  A guest polling the PIC while nothing is being injected
 * is one of these four saying no. */
unsigned long long uno_vdev_pic_state(void)
{
    pic_refresh();
    return (unsigned long long)PICM.irr
         | ((unsigned long long)PICM.isr << 8)
         | ((unsigned long long)PICM.imr << 16)
         | ((unsigned long long)(unsigned)PICM.inited << 24)
         | ((unsigned long long)(unsigned)PICM.icw << 28)
         | ((unsigned long long)(unsigned)P0.mode << 32)
         | ((unsigned long long)P0.reload << 40)
         | ((unsigned long long)(unsigned)P0.armed << 56)
         | ((unsigned long long)(unsigned)(P0.oneshot_done ? 1 : 0) << 57);
}
