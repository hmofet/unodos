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
#include "pc64_native.h"   /* the PIT counts against the real TSC */

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

static u64 pit_elapsed(void)
{
    u64 per_us = uno_native_tsc_per_us();
    u64 dt = uno_native_rdtsc() - P.start;
    if (!per_us) per_us = 1000ull;
    return (dt / per_us) * PIT_HZ / 1000000ull;
}

static int pit_io(unsigned port, int is_write, unsigned long long *val)
{
    switch (port) {
    case 0x43:                                   /* the command register    */
        if (is_write && ((*val >> 6) & 3) == 2) { P.wr_hi = 0; P.rd_hi = 0; }
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

#define COM1 0x3F8
static struct { char line[160]; int n; int chars; u8 mcr, ier; } U;

/* ---- the receive half, without which a console is a closed one -----------
 * A shell whose stdin never delivers a byte exits, and the exit code says so:
 * "Attempted to kill init! exitcode=0x00000000" is a shell that reached
 * end-of-file. Output alone was enough to watch a kernel boot and is not
 * enough to hold a shell open.
 *
 * The seed is queued and handed over on the first poll that finds the FIFO
 * empty - which is the shell asking, since nothing else polls. That is the
 * same shape Glide's first virtio console had ("input is queued BEFORE the
 * guest runs... a working console, not a finished one"), and the honest
 * version is the same too: real keystrokes come from the frame loop, which
 * already owns the keyboard. */
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

int uno_vdev_pio(unsigned port, int is_write, unsigned size,
                 unsigned long long *val, void (*sink)(const char *))
{
    (void)size;
    if (pit_io(port, is_write, val)) return 1;
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
        if (port == COM1 + 1) U.ier = (u8)(*val & 0xFF);
        if (port == COM1) {
            char c = (char)(*val & 0xFF);
            U.chars++;
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
        if (rx_empty()) { *val = 0; break; }
        *val = RX.buf[RX.head];
        RX.head = (RX.head + 1) % (int)sizeof RX.buf;
        break;
    case COM1 + 5:
        /* LSR. Bit 0 is data-ready and it is the whole difference: a driver
         * only reads RBR when this says there is something there. */
        if (rx_empty()) rx_feed_seed();
        *val = 0x60 | (rx_empty() ? 0u : 1u);
        break;
    case COM1 + 2: *val = 0x01; break;   /* IIR: no interrupt pending       */
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
    case COM1 + 1: *val = U.ier; break;  /* IER, read back as written       */
    case COM1 + 4: *val = U.mcr; break;
    default:       *val = 0x00; break;
    }
    return 1;
}

int uno_vdev_serial_chars(void) { return U.chars; }
