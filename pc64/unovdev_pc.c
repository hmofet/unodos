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
        if (!is_write) {
            switch (P.cmos) {
            case 0x00: *val = 0x00; break;       /* seconds, BCD            */
            case 0x02: *val = 0x00; break;       /* minutes                 */
            case 0x04: *val = 0x12; break;       /* hours                   */
            case 0x06: *val = 0x04; break;       /* weekday                 */
            case 0x07: *val = 0x06; break;       /* day                     */
            case 0x08: *val = 0x08; break;       /* month                   */
            case 0x09: *val = 0x26; break;       /* year                    */
            case 0x0A: *val = 0x26; break;       /* status A: UIP CLEAR     */
            case 0x0B: *val = 0x02; break;       /* status B: 24-hour, BCD  */
            case 0x0D: *val = 0x80; break;       /* battery good            */
            case 0x32: *val = 0x20; break;       /* century                 */
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

/* The IRR follows the wires.  Called before any read or arbitration, so a
 * guest inspecting the IRR sees the same lines the injector does. */
static void pic_refresh(void)
{
    u8 irr = 0;
    if (pit0_irq_pending())  irr |= 1u << 0;
    if (uart_irq_level())    irr |= 1u << 4;
    PICM.irr = irr;
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
        if (ready & (1u << i)) return 1;
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
static struct { u8 buf[512]; int head, tail; const char *seed; } RX;

static int rx_empty(void) { return RX.head == RX.tail; }

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
    if (!s) return;
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

int uno_vdev_pio(unsigned port, int is_write, unsigned size,
                 unsigned long long *val, void (*sink)(const char *))
{
    (void)size;
    if (pit_io(port, is_write, val)) return 1;
    if (pic_io(port, is_write, val)) return 1;
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
