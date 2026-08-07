/* ===========================================================================
 * unovdev - the virtio-mmio transport and its device models.  See
 * pc64/UNOVDEV.md and docs/UNOVIRT-PLAN.md §3.6.
 *
 * Vendor-neutral by construction: it is handed a guest-physical address, a
 * direction and a size, and it answers.  Which of VMX or SVM decoded that
 * access, and how, is hv_*.c's problem.  The legacy PC platform a kernel
 * talks to over PORT I/O lives next door in unovdev_pc.c.
 *
 * THE SECURITY POSTURE IS THE POINT OF THIS FILE, more than the devices it
 * implements.  Every address in a virtqueue came from the guest, INCLUDING
 * the ones inside descriptors, and stage-two translation does nothing
 * whatever about them: it bounds the guest's own accesses, not the ones we
 * make on its behalf.  So every guest address here goes through
 * uno_vmm_gpa(), which checks address and length together, and every chain
 * walk is bounded by the queue size, because a guest can point a descriptor's
 * `next` field at itself and a device that trusts the chain to terminate
 * hangs the machine on request.
 *
 * A7 GENERALISED THIS FROM ONE DEVICE TO SEVERAL, and the shape of the change
 * is worth naming: A5's device was a single global with a single queue, which
 * was right for proving a doorbell works and wrong for anything a real driver
 * probes.  A real driver also needs the half of the ring A5 never touched -
 * descriptors the DEVICE writes - because that is how a block read comes
 * back.  So the chain walk now returns the segments and their direction
 * instead of flattening them into a buffer.
 * ======================================================================== */
#include "unovirt.h"
#include "unovdev.h"
#include "pc64_fs.h"       /* the block device is a FILE on a real volume   */
#include <stdio.h>

typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef unsigned long long u64;

/* Where the transports live in the guest's address space, one 0x200 window
 * each.  Deliberately above the carve, so an access is an unmapped
 * guest-physical address and the machine reports it rather than the guest
 * quietly reading its own RAM.  The guest is TOLD these on its command line
 * (`virtio_mmio.device=0x200@0xd0000000:5`), which is the whole reason this
 * needs no PCI host bridge - see UNOVIRT-PLAN §3.6. */
#define VDEV_BASE   0xD0000000ull
#define VDEV_STRIDE 0x200
#define VDEV_MAX    3

#define R_MAGIC        0x000
#define R_VERSION      0x004
#define R_DEVICE_ID    0x008
#define R_VENDOR_ID    0x00C
#define R_DEV_FEAT     0x010
#define R_DEV_FEAT_SEL 0x014
#define R_DRV_FEAT     0x020
#define R_DRV_FEAT_SEL 0x024
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
#define R_CONFIG       0x100

#define VIRTIO_MAGIC   0x74726976u        /* 'virt', as every driver expects */
#define VIRTQ_SIZE_MAX 64
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2              /* the device writes it, not the guest */

/* Status bits.  FEATURES_OK is the one that matters for a version-2
 * transport: the driver sets it and READS IT BACK, and a device that did not
 * like the feature set clears it rather than failing later in some way the
 * driver cannot attribute. */
#define ST_ACK         1
#define ST_DRIVER      2
#define ST_DRIVER_OK   4
#define ST_FEATURES_OK 8
#define ST_FAILED      0x80

/* VIRTIO_F_VERSION_1 is bit 32, i.e. bit 0 of the high feature word, and a
 * version-2 transport is not allowed to work without it.  Offering nothing at
 * all (which A5 did, honestly, for a device nobody drove) makes a real driver
 * refuse the device outright. */
#define F_VERSION_1_HI (1u << 0)
#define VIRTIO_BLK_F_RO (1u << 5)         /* a low-word bit                  */

typedef struct { u64 addr; u32 len; u16 flags; u16 next; } vq_desc;

typedef struct {
    u64 desc, avail, used;
    u32 num, ready;
    u16 last_avail;
} vq;

struct vdev;
typedef struct {
    const char *name;
    u32 device_id;
    u32 feat_lo, feat_hi;
    int nq;
    /* Queues whose available chains must NOT be consumed when the doorbell
     * rings.  See the doorbell handler: for a RECEIVE queue the doorbell
     * means "there are buffers now", and taking them at that moment throws
     * them away, because there is nothing to put in them yet. */
    unsigned hold_mask;
    void (*notify)(struct vdev *d, int qidx);
    u64  (*config)(struct vdev *d, u32 off, unsigned size);
} vdev_ops;

typedef struct vdev {
    const vdev_ops *ops;
    u32 status, drv_feat_lo, drv_feat_hi, dev_feat_sel, drv_feat_sel;
    u32 qsel, intr;
    int irq;
    vq q[2];
} vdev;

static vdev V[VDEV_MAX];
static int  g_ndev;

/* ---- a record of the register traffic ------------------------------------
 * A driver's handshake with a transport is a conversation, and when it goes
 * wrong the driver reports its CONCLUSION ("device refuses features") rather
 * than what it saw. The conversation is the evidence: which register, which
 * way, what value. A hundred and twenty-eight entries covers the probe of
 * both devices with room to spare. */
#define DBG_N 128
static struct { u16 tag; u32 val; } DBG[DBG_N];
static int DBGn;

static void dbg_put(unsigned dev, unsigned off, int is_write, u32 val)
{
    if (DBGn >= DBG_N) return;
    DBG[DBGn].tag = (u16)((dev << 13) | (is_write ? 0x1000 : 0) | (off & 0xFFF));
    DBG[DBGn].val = val;
    DBGn++;
}

/* dev<<45 | write<<44 | off<<32 | value, or 0 past the end. */
unsigned long long uno_vdev_dbg_entry(int i)
{
    if (i < 0 || i >= DBGn) return 0;
    return ((unsigned long long)DBG[i].tag << 32) | DBG[i].val;
}

/* ---- the ring walk, and the two ways a guest can weaponise it ------------- */

static int desc_read(vq *q, u16 i, vq_desc *out)
{
    const vq_desc *d;
    if (i >= q->num || q->num > VIRTQ_SIZE_MAX) return 0;
    d = (const vq_desc *)uno_vmm_gpa(q->desc + (u64)i * sizeof *out, sizeof *out);
    if (!d) return 0;
    *out = *d;
    return 1;
}

/* Walk one chain into an array of segments.  A5 flattened the chain into a
 * byte buffer, which is all a console-out queue needs and is useless for
 * anything the device has to WRITE: a block read is a chain of one readable
 * header, one or more writable data buffers and a writable status byte, and
 * which is which is carried in the descriptor flags rather than known in
 * advance.
 *
 * Bounded by the queue size for the reason in the file header: a
 * self-referential `next` is one store by the guest, and an unbounded walk is
 * a machine that never comes back.  Returns the segment count, or -1 when the
 * chain is malformed - a REPORTABLE state, not a fatal one. */
static int chain_walk(vq *q, u16 head, uno_vseg *segs, int max)
{
    u16 i = head;
    int n = 0, hops;
    for (hops = 0; hops <= (int)q->num; hops++) {
        vq_desc d;
        void *p;
        if (!desc_read(q, i, &d)) return -1;
        p = uno_vmm_gpa(d.addr, d.len);
        if (!p) return -1;                    /* the guest pointed outside   */
        if (n < max) {
            segs[n].p = (u8 *)p;
            segs[n].len = d.len;
            segs[n].write = (d.flags & VIRTQ_DESC_F_WRITE) ? 1 : 0;
            n++;
        } else {
            return -1;                        /* longer than we will service */
        }
        if (!(d.flags & VIRTQ_DESC_F_NEXT)) return n;
        i = d.next;
    }
    return -1;                                /* it never terminated          */
}

/* Hand a chain back to the guest: its head goes in the used ring with the
 * number of bytes the DEVICE wrote, and the device's interrupt line rises.
 * `len` is what the driver uses to know how much of its buffer is real, so a
 * device that reports more than it wrote hands the guest its own stale
 * memory as data. */
static void used_complete(vdev *d, vq *q, u16 head, u32 len)
{
    u8 *u = (u8 *)uno_vmm_gpa(q->used, 8 + 8 * (u64)q->num);
    u16 idx;
    if (!u || !q->num) return;
    idx = *(u16 *)(u + 2);
    *(u32 *)(u + 4 + 8 * (idx % q->num) + 0) = head;
    *(u32 *)(u + 4 + 8 * (idx % q->num) + 4) = len;
    *(u16 *)(u + 2) = (u16)(idx + 1);
    d->intr |= 1;                             /* used-buffer notification    */
}

/* Take the next available chain from a HELD queue - one the doorbell loop
 * deliberately left alone.  This is how a device with something to deliver
 * gets a buffer at the moment it needs one, rather than hoarding buffers it
 * was handed earlier.  Returns the segment count, or -1 when the queue is
 * empty or the chain is malformed. */
static int vq_pull(vq *q, u16 *head, uno_vseg *segs, int max)
{
    const u8 *a;
    int n;
    if (!q->num || !q->ready) return -1;
    a = (const u8 *)uno_vmm_gpa(q->avail, 4 + 2 * (u64)q->num);
    if (!a) return -1;
    if (q->last_avail == *(const u16 *)(a + 2)) return -1;   /* nothing posted */
    *head = *(const u16 *)(a + 4 + 2 * (q->last_avail % q->num));
    n = chain_walk(q, *head, segs, max);
    if (n < 0) return -1;
    q->last_avail++;
    return n;
}

/* The chain a hook is being asked about.  Handed over through these rather
 * than through a parameter list every future device would have to repeat,
 * and set only by the doorbell loop below. */
static u16 g_cur_head;
static vq *g_cur_q;

/* ---- device 0: the console A5 proved the transport with ------------------ */

static struct {
    int notifies, bytes;
    char out[128];
    int outn;
} CON;

/* QUEUE 0 IS RECEIVE AND QUEUE 1 IS TRANSMIT, and conflating them is an
 * interrupt storm rather than a wrong character.  A5's device had one queue
 * and treated every doorbell as output, which was true of the only guest that
 * ever rang it.  A real console driver's FIRST act is to hand the device a
 * pile of empty RECEIVE buffers, and completing one of those means "here is
 * your input" - so a device that completes them all, immediately, with zero
 * bytes, tells the driver it has input forever.  The driver refills, the
 * device completes, the interrupt line never drops, and the guest does
 * nothing else for the rest of its life.  It stalled the boot at the point
 * the console driver initialises.
 *
 * A receive buffer is therefore HELD, not completed: this console has no
 * input to give (the 8250 is the guest's real console), and holding is
 * exactly what a real device does with a buffer nothing has arrived for. */
static void con_notify(vdev *d, int qidx)
{
    uno_vseg segs[8];
    int n, i;
    int wrote = 0;
    if (qidx == 0) return;                    /* receive: hold the buffers   */
    n = chain_walk(g_cur_q, g_cur_head, segs, 8);
    CON.notifies++;
    if (n < 0) return;                        /* malformed: refuse, not hang */
    for (i = 0; i < n; i++) {
        u32 k;
        if (segs[i].write) continue;          /* transmit reads only         */
        CON.bytes += (int)segs[i].len;
        for (k = 0; k < segs[i].len; k++) {
            if (CON.outn < (int)sizeof CON.out - 1)
                CON.out[CON.outn++] = (char)segs[i].p[k];
        }
        CON.out[CON.outn] = 0;
        wrote += (int)segs[i].len;
    }
    used_complete(d, g_cur_q, g_cur_head, (u32)wrote);
}

static const vdev_ops CON_OPS = { "console", 3, 0, F_VERSION_1_HI, 2,
                                  1u << 0, con_notify, 0 };

/* ---- device 1: virtio-blk over a FILE on a real volume -------------------
 *
 * The appliance's disk is `EFI\UNODOS\VM\ROOTFS.IMG` on whichever volume
 * carries it - no new partition scheme, no installer change, and a disk the
 * user can delete with the file manager (UNOVIRT-PLAN §3.6).
 *
 * IT IS READ-ONLY, AND THAT IS A LIMIT OF THE LAYER BELOW RATHER THAN A
 * CHOICE.  Both unofs and the native FAT driver read from an offset
 * (`uno_fs_read_at`, for streaming media) but write only WHOLE FILES
 * (`uno_fs_write`), so a single 512-byte sector write means rewriting a
 * multi-megabyte image.  VIRTIO_BLK_F_RO is offered so the guest knows
 * before it tries, which turns a confusing write error deep in a filesystem
 * into `mount: read-only` - and a read-only rootfs with a tmpfs overlay is
 * the ordinary shape for an appliance anyway.  A write-at-offset primitive
 * is filed with the unofs lane. */
#define BLK_T_IN    0
#define BLK_T_OUT   1
#define BLK_T_FLUSH 4
#define BLK_T_GETID 8
#define BLK_S_OK      0
#define BLK_S_IOERR   1
#define BLK_S_UNSUPP  2
#define BLK_SECTOR    512

static struct {
    int  vol, attached, tried;
    long size;
    int  reads, sectors, errors;
    const char *path;
} BLK;

static void blk_attach(void)
{
    static const char *paths[2] = { "EFI\\UNODOS\\VM\\ROOTFS.IMG", "ROOTFS.IMG" };
    int nvol, v, i;
    if (BLK.tried) return;
    BLK.tried = 1;
    nvol = uno_fs_volumes();
    for (v = 0; v < nvol; v++) {
        for (i = 0; i < 2; i++) {
            long sz = uno_fs_size(v, paths[i]);
            if (sz > 0) {
                BLK.vol = v;
                BLK.path = paths[i];
                BLK.size = sz;
                BLK.attached = 1;
                return;
            }
        }
    }
}

/* The capacity, in 512-byte sectors, is the whole of a blk device's config
 * space that matters.  A capacity of zero is a disk the guest will probe and
 * then ignore, which is the honest answer when no image is present. */
static u64 blk_config(vdev *d, u32 off, unsigned size)
{
    u64 cap;
    (void)d; (void)size;
    blk_attach();
    cap = BLK.attached ? (u64)(BLK.size / BLK_SECTOR) : 0;
    if (off == 0) return cap & 0xFFFFFFFFull;
    if (off == 4) return cap >> 32;
    return 0;
}

static void blk_notify(vdev *d, int qidx)
{
    uno_vseg segs[8];
    int n = chain_walk(g_cur_q, g_cur_head, segs, 8), i;
    u32 type;
    u64 sector;
    u32 wrote = 0;
    u8 *status = 0;
    (void)qidx;
    blk_attach();
    if (n < 2) return;                        /* malformed: refuse, not hang */

    /* The header is the first readable segment: type, reserved, sector. */
    if (segs[0].write || segs[0].len < 16) return;
    type   = *(const u32 *)segs[0].p;
    sector = *(const u64 *)(segs[0].p + 8);

    /* The status byte is the last segment and the device writes it.  Found
     * by position rather than by scanning, because that is what the spec
     * fixes and a device that guesses will one day pick a data buffer. */
    if (segs[n - 1].write && segs[n - 1].len >= 1) status = segs[n - 1].p;

    if (type == BLK_T_IN) {
        for (i = 1; i < n - 1; i++) {
            long off, got = 0;
            if (!segs[i].write) continue;     /* a read fills WRITABLE ones  */
            off = (long)(sector * BLK_SECTOR) + (long)wrote;
            if (!BLK.attached || off < 0 || off >= BLK.size) break;
            got = uno_fs_read_at(BLK.vol, BLK.path, off, segs[i].p,
                                 (long)segs[i].len);
            if (got < 0) break;
            /* Short of the end of the image reads as zeroes rather than as
             * stale guest memory: whatever was in that buffer came from the
             * guest, and handing it back as disk contents is a disclosure
             * dressed up as a read. */
            if (got < (long)segs[i].len) {
                long k;
                for (k = got; k < (long)segs[i].len; k++) segs[i].p[k] = 0;
            }
            wrote += segs[i].len;
        }
        BLK.reads++;
        BLK.sectors += (int)(wrote / BLK_SECTOR);
        if (status) *status = BLK.attached ? BLK_S_OK : BLK_S_IOERR;
    } else if (type == BLK_T_GETID) {
        for (i = 1; i < n - 1; i++) {
            u32 k;
            if (!segs[i].write) continue;
            for (k = 0; k < segs[i].len; k++)
                segs[i].p[k] = (u8)("UNODOS-VM"[k < 9 ? k : 9] );
            wrote += segs[i].len;
        }
        if (status) *status = BLK_S_OK;
    } else if (type == BLK_T_FLUSH) {
        if (status) *status = BLK_S_OK;       /* nothing is cached to flush  */
    } else {
        /* OUT and anything else.  RO is advertised, so a write here is a
         * driver ignoring the feature bit; it gets an error rather than
         * silence, because silence would look like it worked. */
        BLK.errors++;
        if (status) *status = BLK_S_UNSUPP;
    }
    if (status) wrote += 1;
    used_complete(d, g_cur_q, g_cur_head, wrote);
}

static const vdev_ops BLK_OPS = { "blk", 2, VIRTIO_BLK_F_RO, F_VERSION_1_HI, 1,
                                  0, blk_notify, blk_config };

/* ---- device 2: virtio-net, and the receive queue is the interesting half --
 *
 * Queue 0 is receive and queue 1 is transmit, as on the console - but where
 * the console has nothing to receive and simply holds its buffers, this
 * device DOES eventually fill them, which makes the discipline explicit:
 *
 * A POSTED RECEIVE BUFFER IS NOT CONSUMED WHEN IT ARRIVES.  The doorbell on
 * queue 0 means "there are buffers available now", not "here is work".  So
 * that notify does nothing at all, `last_avail` stays where it is, and a
 * buffer is only taken at the moment there is a frame to put in it.  The
 * alternative - taking buffers as they are offered and remembering them - is
 * a second queue of our own to keep in step with the guest's, for no gain.
 *
 * Every frame carries a 12-byte virtio_net_hdr in front of it, in BOTH
 * directions.  With VIRTIO_F_VERSION_1 the header is always the 12-byte v1
 * form (it carries num_buffers even when buffer merging is off), and getting
 * that length wrong shifts every frame by four bytes - which presents as a
 * peer that talks nonsense rather than as a header problem. */
#define VIRTIO_NET_F_MAC (1u << 5)
#define NET_HDR 12
#define NET_MTU 1600

static struct { int tx, rx, dropped_norx, too_big, kick0, kick1, badchain; } NET;

/* Put one frame into a receive buffer the guest has posted, if it has posted
 * one.  No buffer means the frame is DROPPED and counted - which is what a
 * real NIC does when the driver has not kept the ring fed, and is much better
 * than blocking the device on the guest's housekeeping. */
static int net_rx_deliver(vdev *d, const u8 *frame, int len)
{
    vq *q = &d->q[0];
    u16 head = 0;
    uno_vseg segs[8];
    int n, i, off = 0, wrote = 0;

    n = vq_pull(q, &head, segs, 8);
    if (n < 0) { NET.dropped_norx++; return 0; }

    for (i = 0; i < n && off < NET_HDR + len; i++) {
        u32 k;
        if (!segs[i].write) continue;         /* a receive buffer is writable */
        for (k = 0; k < segs[i].len && off < NET_HDR + len; k++, off++) {
            segs[i].p[k] = (off < NET_HDR) ? 0 : frame[off - NET_HDR];
            wrote++;
        }
    }
    used_complete(d, q, head, (u32)wrote);
    NET.rx++;
    return 1;
}

static void net_notify(vdev *d, int qidx)
{
    uno_vseg segs[8];
    static u8 frame[NET_MTU], reply[NET_MTU];
    int n, i, len = 0, rl;

    if (qidx == 0) { NET.kick0++; return; }   /* buffers offered, not work   */
    NET.kick1++;

    n = chain_walk(g_cur_q, g_cur_head, segs, 8);
    if (n < 0) { NET.badchain++; return; }
    /* Flatten the readable part and drop the 12-byte header. */
    for (i = 0; i < n; i++) {
        u32 k;
        if (segs[i].write) continue;
        for (k = 0; k < segs[i].len; k++) {
            if (len < (int)sizeof frame) frame[len++] = segs[i].p[k];
        }
    }
    used_complete(d, g_cur_q, g_cur_head, 0);   /* transmit writes nothing   */
    if (len <= NET_HDR) return;
    if (len >= (int)sizeof frame) { NET.too_big++; return; }
    NET.tx++;

    /* The peer is on the other side of unovdev_net.c: a frame in, a frame
     * out, and no idea any of this is virtual. */
    rl = uno_vnet_respond(frame + NET_HDR, len - NET_HDR, reply, (int)sizeof reply);
    if (rl > 0) net_rx_deliver(d, reply, rl);
}

/* The MAC lives in config space, and VIRTIO_NET_F_MAC is what tells the guest
 * to read it rather than invent a random one.  Byte-addressed, because that
 * is how a driver reads a six-byte field. */
static u64 net_config(vdev *d, u32 off, unsigned size)
{
    const u8 *mac = uno_vnet_guest_mac();
    u64 v = 0;
    unsigned k;
    (void)d;
    if (!size) size = 1;
    for (k = 0; k < size && k < 8; k++) {
        u32 o = off + k;
        u8 b = (o < 6) ? mac[o] : 0;          /* then status, which is 0     */
        v |= (u64)b << (8 * k);
    }
    return v;
}

static const vdev_ops NET_OPS = { "net", 1, VIRTIO_NET_F_MAC, F_VERSION_1_HI, 2,
                                  1u << 0, net_notify, net_config };

/* tx | rx<<8 | no-buffer<<16 | too-big<<24, for the one-line trace: whether
 * the guest's frames reach us at all is the first question, and whether our
 * replies find a posted buffer is the second. */
unsigned long long uno_vdev_net_stats(void)
{
    return (unsigned long long)(NET.tx & 0xFF)
         | ((unsigned long long)(NET.rx & 0xFF) << 8)
         | ((unsigned long long)(NET.dropped_norx & 0xFF) << 16)
         | ((unsigned long long)(NET.too_big & 0xFF) << 24)
         | ((unsigned long long)(NET.kick0 & 0xFF) << 32)
         | ((unsigned long long)(NET.kick1 & 0xFF) << 40)
         | ((unsigned long long)(NET.badchain & 0xFF) << 48);
}

int uno_vdev_net_str(char *buf, int cap)
{
    int n = snprintf(buf, (unsigned)cap, "tx %d rx %d, no-buffer %d, ",
                     NET.tx, NET.rx, NET.dropped_norx);
    if (n < 0 || n >= cap) return 0;
    return n + uno_vnet_str(buf + n, cap - n);
}

/* ---- the register file ---------------------------------------------------- */

void uno_vdev_reset(void)
{
    unsigned i;
    for (i = 0; i < sizeof V; i++) ((u8 *)&V)[i] = 0;
    for (i = 0; i < sizeof CON; i++) ((u8 *)&CON)[i] = 0;
    for (i = 0; i < sizeof BLK; i++) ((u8 *)&BLK)[i] = 0;
    for (i = 0; i < sizeof NET; i++) ((u8 *)&NET)[i] = 0;
    V[0].ops = &CON_OPS; V[0].irq = 5;
    V[1].ops = &BLK_OPS; V[1].irq = 6;
    V[2].ops = &NET_OPS; V[2].irq = 7;
    g_ndev = 3;
}

static u32 dev_feat(vdev *d)
{
    return d->dev_feat_sel ? d->ops->feat_hi : d->ops->feat_lo;
}

int uno_vdev_mmio(u64 gpa, int is_write, unsigned size, u64 *val)
{
    unsigned idx, off;
    vdev *d;
    vq *q;
    if (!g_ndev) uno_vdev_reset();
    if (gpa < VDEV_BASE || gpa >= VDEV_BASE + (u64)VDEV_STRIDE * g_ndev) return 0;
    idx = (unsigned)((gpa - VDEV_BASE) / VDEV_STRIDE);
    off = (unsigned)((gpa - VDEV_BASE) % VDEV_STRIDE);
    d = &V[idx];
    q = &d->q[d->qsel < 2 ? d->qsel : 0];

    if (off >= R_CONFIG) {
        if (!is_write) *val = d->ops->config
                            ? d->ops->config(d, off - R_CONFIG, size) : 0;
        dbg_put(idx, off, is_write, (u32)*val);
        return 1;
    }

    if (is_write) dbg_put(idx, off, 1, (u32)*val);
    if (!is_write) {
        switch (off) {
        case R_MAGIC:        *val = VIRTIO_MAGIC; break;
        case R_VERSION:      *val = 2; break;
        case R_DEVICE_ID:    *val = d->ops->device_id; break;
        case R_VENDOR_ID:    *val = 0x554E4F00u;   /* 'UNO'                  */ break;
        case R_DEV_FEAT:     *val = dev_feat(d); break;
        case R_QUEUE_NUM_MAX:*val = VIRTQ_SIZE_MAX; break;
        case R_QUEUE_READY:  *val = q->ready; break;
        case R_INTR_STATUS:  *val = d->intr; break;
        case R_STATUS:       *val = d->status; break;
        default:             *val = 0; break;
        }
        dbg_put(idx, off, 0, (u32)*val);
        return 1;
    }

    switch (off) {
    case R_DEV_FEAT_SEL: d->dev_feat_sel = (u32)*val; break;
    case R_DRV_FEAT_SEL: d->drv_feat_sel = (u32)*val; break;
    case R_DRV_FEAT:
        if (d->drv_feat_sel) d->drv_feat_hi = (u32)*val;
        else                 d->drv_feat_lo = (u32)*val;
        break;
    case R_QUEUE_SEL:    d->qsel = (u32)*val; break;
    case R_QUEUE_NUM:    q->num = (u32)*val > VIRTQ_SIZE_MAX
                                ? VIRTQ_SIZE_MAX : (u32)*val; break;
    case R_QUEUE_READY:  q->ready = (u32)*val; break;
    case R_QUEUE_NOTIFY:
        {   unsigned qi = (unsigned)*val;
            if (qi < 2 && (d->ops->hold_mask & (1u << qi))) {
                /* A held queue: tell the device buffers arrived and let it
                 * pull one when it has something to put in it. */
                if (d->ops->notify) d->ops->notify(d, (int)qi);
            } else if (qi < 2 && d->q[qi].num) {
                vq *nq = &d->q[qi];
                const u8 *a = (const u8 *)uno_vmm_gpa(nq->avail,
                                                      4 + 2 * (u64)nq->num);
                u16 aidx;
                int guard;
                if (!a) break;
                aidx = *(const u16 *)(a + 2);
                for (guard = 0; nq->last_avail != aidx && guard <= (int)nq->num;
                     guard++) {
                    g_cur_q = nq;
                    g_cur_head = *(const u16 *)(a + 4
                                    + 2 * (nq->last_avail % nq->num));
                    nq->last_avail++;
                    if (d->ops->notify) d->ops->notify(d, (int)qi);
                }
            }
        }
        break;
    case R_INTR_ACK:     d->intr &= ~(u32)*val; break;
    case R_STATUS:
        d->status = (u32)*val;
        /* FEATURES_OK is a NEGOTIATION, not an announcement: if the driver
         * did not accept VIRTIO_F_VERSION_1 we clear the bit, and it reads
         * back its own failure instead of finding out later through a ring
         * layout neither side agrees on. */
        if ((d->status & ST_FEATURES_OK) && !(d->drv_feat_hi & F_VERSION_1_HI))
            d->status &= ~(u32)ST_FEATURES_OK;
        if (!d->status) {                     /* a reset, per the spec       */
            unsigned k;
            for (k = 0; k < 2; k++) {
                d->q[k].num = d->q[k].ready = 0;
                d->q[k].last_avail = 0;
                d->q[k].desc = d->q[k].avail = d->q[k].used = 0;
            }
            d->intr = 0;
            d->drv_feat_lo = d->drv_feat_hi = 0;
        }
        break;
    case R_DESC_LO:  q->desc  = (q->desc  & ~0xFFFFFFFFull) | (u32)*val; break;
    case R_DESC_HI:  q->desc  = (q->desc  & 0xFFFFFFFFull) | ((u64)(u32)*val << 32); break;
    case R_AVAIL_LO: q->avail = (q->avail & ~0xFFFFFFFFull) | (u32)*val; break;
    case R_AVAIL_HI: q->avail = (q->avail & 0xFFFFFFFFull) | ((u64)(u32)*val << 32); break;
    case R_USED_LO:  q->used  = (q->used  & ~0xFFFFFFFFull) | (u32)*val; break;
    case R_USED_HI:  q->used  = (q->used  & 0xFFFFFFFFull) | ((u64)(u32)*val << 32); break;
    default: break;
    }
    return 1;
}

/* Is any device wired to this PIC line asking for attention?  A level, like
 * the UART's received-data: it stays up until the driver acknowledges it by
 * writing InterruptACK, which is exactly what the register is for. */
int uno_vdev_mmio_irq_level(int irq)
{
    int i;
    for (i = 0; i < g_ndev; i++)
        if (V[i].irq == irq && V[i].intr && (V[i].status & ST_DRIVER_OK))
            return 1;
    return 0;
}

/* ---- what the tests want to know ------------------------------------------ */

unsigned long long uno_vdev_base(void) { return VDEV_BASE; }

/* A5's rings, placed on the console's TRANSMIT queue - queue 1, because that
 * is where a console's outbound traffic goes and the test guest now rings it
 * by number. */
void uno_vdev_queue(u64 desc, u64 avail, u64 used, u32 qnum)
{
    if (!g_ndev) uno_vdev_reset();
    V[0].q[1].desc = desc; V[0].q[1].avail = avail; V[0].q[1].used = used;
    V[0].q[1].num = qnum; V[0].q[1].ready = 1;
}

const char *uno_vdev_output(int *n, int *notifies, int *bytes)
{
    if (n) *n = CON.outn;
    if (notifies) *notifies = CON.notifies;
    if (bytes) *bytes = CON.bytes;
    return CON.out;
}

int uno_vdev_blk_str(char *buf, int cap)
{
    blk_attach();
    if (!BLK.attached)
        return snprintf(buf, (unsigned)cap, "no ROOTFS.IMG");
    return snprintf(buf, (unsigned)cap,
                    "%ld KB ro on vol %d, %d reads %d sectors%s",
                    BLK.size / 1024, BLK.vol, BLK.reads, BLK.sectors,
                    BLK.errors ? " (writes REFUSED)" : "");
}

/* The chain walk, driven directly with a chain the guest could have built and
 * a real device must survive: descriptor 0's `next` points at descriptor 0.
 * Returning at all is the whole result. */
int uno_vdev_cycle_refused(u64 desc_gpa, u32 qnum)
{
    uno_vseg segs[8];
    vq q;
    vq_desc *d = (vq_desc *)uno_vmm_gpa(desc_gpa, sizeof *d);
    int r;
    if (!d) return 0;
    q.desc = desc_gpa; q.num = qnum;
    q.avail = q.used = 0; q.ready = 1; q.last_avail = 0;
    d->addr = desc_gpa; d->len = 4;
    d->flags = VIRTQ_DESC_F_NEXT; d->next = 0;   /* itself                    */
    r = chain_walk(&q, 0, segs, 8);
    return r < 0;                                /* refused, and came back    */
}
