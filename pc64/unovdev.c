/* ===========================================================================
 * unovdev - the virtio-mmio transport and its first device.  See
 * pc64/UNOVDEV.md and docs/UNOVIRT-PLAN.md §3.6.
 *
 * Vendor-neutral by construction: it is handed a guest-physical address, a
 * direction and a size, and it answers.  Which of VMX or SVM decoded that
 * access, and how, is hv_*.c's problem.
 *
 * THE SECURITY POSTURE IS THE POINT OF THIS FILE, more than the device it
 * implements.  Every address in a virtqueue came from the guest, INCLUDING
 * the ones inside descriptors, and stage-two translation does nothing
 * whatever about them: it bounds the guest's own accesses, not the ones we
 * make on its behalf.  So every guest address here goes through
 * uno_vmm_gpa(), which checks address and length together, and every chain
 * walk is bounded by the queue size, because a guest can point a descriptor's
 * `next` field at itself and a device that trusts the chain to terminate
 * hangs the machine on request.
 * ======================================================================== */
#include "unovirt.h"
#include <stdio.h>
#include "pc64_native.h"   /* the PIT counts against the real TSC */

typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef unsigned long long u64;

/* Where the transport lives in the guest's address space.  Deliberately above
 * the carve, so an access to it is an unmapped guest-physical address and the
 * machine reports it rather than the guest quietly reading its own RAM. */
#define VDEV_BASE   0xD0000000ull
#define VDEV_SIZE   0x200

#define R_MAGIC        0x000
#define R_VERSION      0x004
#define R_DEVICE_ID    0x008
#define R_VENDOR_ID    0x00C
#define R_DEV_FEAT     0x010
#define R_DRV_FEAT     0x020
#define R_QUEUE_SEL    0x030
#define R_QUEUE_NUM_MAX 0x034
#define R_QUEUE_NUM    0x038
#define R_QUEUE_READY  0x044
#define R_QUEUE_NOTIFY 0x050
#define R_INTR_STATUS  0x060
#define R_INTR_ACK     0x064
#define R_STATUS       0x070
#define R_DESC_LO      0x080
#define R_DESC_HI      0x084
#define R_AVAIL_LO     0x090
#define R_AVAIL_HI     0x094
#define R_USED_LO      0x0A0
#define R_USED_HI      0x0A4

#define VIRTIO_MAGIC   0x74726976u        /* 'virt', as every driver expects */
#define VIRTQ_SIZE_MAX 64
#define VIRTQ_DESC_F_NEXT 1

typedef struct { u64 addr; u32 len; u16 flags; u16 next; } vq_desc;

static struct {
    u32 status, drv_feat, qsel, qnum, qready, intr;
    u64 desc, avail, used;
    u16 last_avail;
    int notifies, bytes;
    char out[128];
    int outn;
} D;

void uno_vdev_reset(void)
{
    unsigned i;
    for (i = 0; i < sizeof D; i++) ((u8 *)&D)[i] = 0;
}

/* ---- the ring walk, and the two ways a guest can weaponise it ------------- */

/* Read one descriptor, refusing anything the queue's own bounds say cannot be
 * there.  Returns 0 rather than a partly-filled descriptor. */
static int desc_read(u16 i, vq_desc *out)
{
    const vq_desc *d;
    if (i >= D.qnum || D.qnum > VIRTQ_SIZE_MAX) return 0;
    d = (const vq_desc *)uno_vmm_gpa(D.desc + (u64)i * sizeof *out, sizeof *out);
    if (!d) return 0;
    *out = *d;
    return 1;
}

/* Walk one chain, copying what it points at into `buf`.  Bounded by the queue
 * size for the reason in the file header: a self-referential `next` is one
 * store by the guest, and an unbounded walk is a machine that never comes
 * back.  Returns the number of bytes consumed, or -1 when the chain is
 * malformed - which is a REPORTABLE state, not a fatal one. */
static int chain_read(u16 head, char *buf, int cap)
{
    u16 i = head;
    int n = 0, hops;
    for (hops = 0; hops <= (int)D.qnum; hops++) {
        vq_desc d;
        const char *src;
        u32 k;
        if (!desc_read(i, &d)) return -1;
        src = (const char *)uno_vmm_gpa(d.addr, d.len);
        if (!src) return -1;                  /* the guest pointed outside   */
        for (k = 0; k < d.len && n < cap; k++) buf[n++] = src[k];
        D.bytes += (int)d.len;
        if (!(d.flags & VIRTQ_DESC_F_NEXT)) return n;
        i = d.next;
    }
    return -1;                                /* it never terminated          */
}

static void used_complete(u16 head, u32 len)
{
    u8 *u = (u8 *)uno_vmm_gpa(D.used, 8 + 8 * (u64)D.qnum);
    u16 idx;
    if (!u) return;
    idx = *(u16 *)(u + 2);
    *(u32 *)(u + 4 + 8 * (idx % D.qnum) + 0) = head;
    *(u32 *)(u + 4 + 8 * (idx % D.qnum) + 4) = len;
    *(u16 *)(u + 2) = (u16)(idx + 1);
    D.intr |= 1;
}

static void notify(void)
{
    const u8 *a = (const u8 *)uno_vmm_gpa(D.avail, 4 + 2 * (u64)D.qnum);
    u16 idx;
    if (!a || !D.qnum) return;
    D.notifies++;
    idx = *(const u16 *)(a + 2);
    while (D.last_avail != idx) {
        u16 head = *(const u16 *)(a + 4 + 2 * (D.last_avail % D.qnum));
        int n = chain_read(head, D.out + D.outn,
                           (int)sizeof D.out - D.outn - 1);
        D.last_avail++;
        if (n < 0) return;                    /* malformed: refuse, do not hang */
        D.outn += n;
        D.out[D.outn] = 0;
        used_complete(head, (u32)n);
    }
}

/* ---- the register file ---------------------------------------------------- */

int uno_vdev_mmio(u64 gpa, int is_write, unsigned size, u64 *val)
{
    u32 off;
    (void)size;
    if (gpa < VDEV_BASE || gpa >= VDEV_BASE + VDEV_SIZE) return 0;
    off = (u32)(gpa - VDEV_BASE);

    if (!is_write) {
        switch (off) {
        case R_MAGIC:        *val = VIRTIO_MAGIC; break;
        case R_VERSION:      *val = 2; break;
        case R_DEVICE_ID:    *val = 3; break;      /* console                */
        case R_VENDOR_ID:    *val = 0x554E4F00u;   /* 'UNO'                  */ break;
        case R_DEV_FEAT:     *val = 0; break;      /* offering nothing is an
                                                      honest answer, and keeps
                                                      the handshake real      */
        case R_QUEUE_NUM_MAX:*val = VIRTQ_SIZE_MAX; break;
        case R_QUEUE_READY:  *val = D.qready; break;
        case R_INTR_STATUS:  *val = D.intr; break;
        case R_STATUS:       *val = D.status; break;
        default:             *val = 0; break;
        }
        return 1;
    }

    switch (off) {
    case R_DRV_FEAT:     D.drv_feat = (u32)*val; break;
    case R_QUEUE_SEL:    D.qsel = (u32)*val; break;
    case R_QUEUE_NUM:    D.qnum = (u32)*val > VIRTQ_SIZE_MAX
                                 ? VIRTQ_SIZE_MAX : (u32)*val; break;
    case R_QUEUE_READY:  D.qready = (u32)*val; break;
    case R_QUEUE_NOTIFY: notify(); break;
    case R_INTR_ACK:     D.intr &= ~(u32)*val; break;
    case R_STATUS:       D.status = (u32)*val; break;
    case R_DESC_LO:  D.desc  = (D.desc  & ~0xFFFFFFFFull) | (u32)*val; break;
    case R_DESC_HI:  D.desc  = (D.desc  & 0xFFFFFFFFull) | ((u64)(u32)*val << 32); break;
    case R_AVAIL_LO: D.avail = (D.avail & ~0xFFFFFFFFull) | (u32)*val; break;
    case R_AVAIL_HI: D.avail = (D.avail & 0xFFFFFFFFull) | ((u64)(u32)*val << 32); break;
    case R_USED_LO:  D.used  = (D.used  & ~0xFFFFFFFFull) | (u32)*val; break;
    case R_USED_HI:  D.used  = (D.used  & 0xFFFFFFFFull) | ((u64)(u32)*val << 32); break;
    default: break;
    }
    return 1;
}

/* ---- an 8250, because it is the first thing a kernel talks to -------------
 *
 * Linux's `earlyprintk=serial` writes bytes to port 0x3F8 before it has a
 * driver for anything, before it has an interrupt controller, and before it
 * can be asked what went wrong. A virtio console is the better device and it
 * arrives far too late to help with a kernel that dies in its first
 * millisecond.
 *
 * Port I/O is also the one place x86 is EASIER than ARM here: the exit
 * qualification carries the port, the size and the direction, so unlike MMIO
 * there is no instruction to decode. */
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
static struct { char line[160]; int n; int chars; } U;

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
    case COM1 + 5: *val = 0x60; break;   /* LSR: holding + shift both empty */
    case COM1 + 2: *val = 0x01; break;   /* IIR: no interrupt pending       */
    case COM1 + 6: *val = 0xB0; break;   /* MSR: carrier, DSR, CTS          */
    default:       *val = 0x00; break;
    }
    return 1;
}

int uno_vdev_serial_chars(void) { return U.chars; }

/* ---- what the test wants to know ------------------------------------------ */

unsigned long long uno_vdev_base(void) { return VDEV_BASE; }

void uno_vdev_queue(u64 desc, u64 avail, u64 used, u32 qnum)
{
    D.desc = desc; D.avail = avail; D.used = used;
    D.qnum = qnum; D.qready = 1;
}

const char *uno_vdev_output(int *n, int *notifies, int *bytes)
{
    if (n) *n = D.outn;
    if (notifies) *notifies = D.notifies;
    if (bytes) *bytes = D.bytes;
    return D.out;
}

/* The chain walk, driven directly with a chain the guest could have built and
 * a real device must survive: descriptor 0's `next` points at descriptor 0.
 * Returning at all is the whole result. */
int uno_vdev_cycle_refused(u64 desc_gpa, u32 qnum)
{
    char scratch[16];
    u64 save_desc = D.desc;
    u32 save_qnum = D.qnum;
    vq_desc *d = (vq_desc *)uno_vmm_gpa(desc_gpa, sizeof *d);
    int r;
    if (!d) return 0;
    D.desc = desc_gpa;
    D.qnum = qnum;
    d->addr = desc_gpa; d->len = 4;
    d->flags = VIRTQ_DESC_F_NEXT; d->next = 0;   /* itself                    */
    r = chain_read(0, scratch, (int)sizeof scratch);
    D.desc = save_desc;
    D.qnum = save_qnum;
    return r < 0;                                /* refused, and came back    */
}
