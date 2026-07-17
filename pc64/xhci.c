/* UnoDOS/pc64 - xHCI host-controller driver (see xhci.h). Phase A: controller
 * bring-up (reset -> rings -> run) + root-hub port reset/scan. Enumeration
 * (Address Device + descriptor fetch) builds on these rings. Polled, no IRQs.
 *
 * DMA is identity-mapped (UEFI boot services alive => a static .bss buffer's
 * address is its physical address), the same trick e1000 uses. */
#include "xhci.h"

#ifndef UNO_XHCI

int  uno_xhci_init(void) { return 0; }
int  uno_xhci_dev_count(void) { return 0; }
const uno_usb_dev *uno_xhci_dev(int i) { (void)i; return 0; }
void uno_xhci_status(int *p, int *n, int *d, unsigned *e)
{ if (p)*p=0; if (n)*n=0; if (d)*d=0; if (e)*e=0; }

#else  /* ===================== UNO_XHCI enabled ========================= */

#include "pc64_pci.h"
#include <stdint.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* ---- capability registers (from the MMIO base) --------------------------- */
#define CAP_CAPLENGTH   0x00   /* byte */
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* ---- operational registers (base + CAPLENGTH) ---------------------------- */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_DNCTRL   0x14
#define OP_CRCR     0x18   /* 64-bit */
#define OP_DCBAAP   0x30   /* 64-bit */
#define OP_CONFIG   0x38
#define OP_PORTSC(p) (0x400 + ((p)-1)*0x10)   /* p = 1-based */

#define CMD_RS    (1u<<0)
#define CMD_HCRST (1u<<1)
#define CMD_INTE  (1u<<2)
#define STS_HCH   (1u<<0)
#define STS_CNR   (1u<<11)

#define PORTSC_CCS (1u<<0)    /* current connect status */
#define PORTSC_PED (1u<<1)    /* port enabled */
#define PORTSC_PR  (1u<<4)    /* port reset */
#define PORTSC_PP  (1u<<9)    /* port power */
#define PORTSC_CSC (1u<<17)   /* connect status change */
#define PORTSC_PRC (1u<<21)   /* port reset change */
#define PORTSC_SPEED(v) (((v)>>10)&0xF)
/* write-1-to-clear change bits live in PORTSC; preserve the rest when writing */
#define PORTSC_RW1CS (PORTSC_CSC|PORTSC_PRC|(1u<<18)|(1u<<19)|(1u<<20)|(1u<<22)|(1u<<23))

/* ---- runtime + interrupter 0 (base + RTSOFF) ----------------------------- */
#define RT_IR0      0x20
#define IR_IMAN     0x00
#define IR_IMOD     0x04
#define IR_ERSTSZ   0x08
#define IR_ERSTBA   0x10   /* 64-bit */
#define IR_ERDP     0x18   /* 64-bit */

/* ---- TRBs ---------------------------------------------------------------- */
typedef struct { u64 param; u32 status; u32 control; } __attribute__((packed)) trb_t;
#define TRB_TYPE(t)   ((u32)(t)<<10)
#define TRB_GET_TYPE(c) (((c)>>10)&0x3F)
#define TRB_CYCLE     (1u<<0)
/* types */
#define TR_LINK        6
#define TR_ENABLE_SLOT 9
#define TR_ADDRESS_DEV 11
#define TR_NOOP_CMD    23
#define TR_XFER_EVENT  32
#define TR_CMD_COMPLETE 33
#define TR_PORT_STATUS 34
#define CC_SUCCESS     1

#define CMD_RING_SZ 64
#define EVT_RING_SZ 64
#define MAX_SLOTS_SUP 64
#define MAX_SCRATCH  64

/* DMA structures, page/64-byte aligned in .bss (phys == virt) */
static u64   g_dcbaa[MAX_SLOTS_SUP+1]  __attribute__((aligned(64)));
static trb_t g_cmd[CMD_RING_SZ]        __attribute__((aligned(64)));
static trb_t g_evt[EVT_RING_SZ]        __attribute__((aligned(64)));
static struct { u64 base; u32 size; u32 rsvd; } g_erst[1] __attribute__((aligned(64)));
static u64   g_scratch_arr[MAX_SCRATCH] __attribute__((aligned(64)));
static u8    g_scratch_buf[MAX_SCRATCH][4096] __attribute__((aligned(4096)));

/* ---- state --------------------------------------------------------------- */
static volatile u8 *g_cap;         /* MMIO base */
static volatile u8 *g_op, *g_rt, *g_db;
static int g_caplen, g_csz, g_maxslots, g_maxports;
static int g_present, g_nports_conn, g_ndevs;
static unsigned g_err;             /* stage/failure code for the diagnostic */
static int g_cmd_i, g_cmd_cyc;     /* command ring enqueue + producer cycle */
static int g_evt_i, g_evt_cyc;     /* event ring dequeue + consumer cycle */

static u32 rd32(volatile u8 *b, u32 o) { return *(volatile u32 *)(b+o); }
static void wr32(volatile u8 *b, u32 o, u32 v) { *(volatile u32 *)(b+o)=v; }
static u64 rd64(volatile u8 *b, u32 o) { return *(volatile u64 *)(b+o); }
static void wr64(volatile u8 *b, u32 o, u64 v) { *(volatile u64 *)(b+o)=v; }
static void spin(volatile int n){ while(n-->0) __asm__ volatile(""); }

/* find the xHCI controller: PCI class 0x0C, subclass 0x03, prog-if 0x30 */
static int find_xhci(pci_dev *out)
{
    int bus, dev, fn;
    for (bus = 0; bus < 256; bus++) for (dev = 0; dev < 32; dev++) for (fn = 0; fn < 8; fn++) {
        pci_dev d; u32 id, cls;
        d.bus=bus; d.dev=dev; d.fn=fn;
        id = pci_cfg_read32(&d, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        cls = pci_cfg_read32(&d, 0x08);
        if (((cls>>24)&0xFF)==0x0C && ((cls>>16)&0xFF)==0x03 && ((cls>>8)&0xFF)==0x30) {
            d.vendor=id&0xFFFF; d.device=id>>16; *out=d; return 1;
        }
    }
    return 0;
}

/* poll the event ring for the next event whose cycle matches our consumer
 * state; copy it out. Returns 1 if an event arrived, 0 on timeout. */
static int poll_event(trb_t *out, int budget)
{
    while (budget-- > 0) {
        trb_t *e = &g_evt[g_evt_i];
        if ((e->control & TRB_CYCLE) == (u32)g_evt_cyc) {
            *out = *e;
            if (++g_evt_i >= EVT_RING_SZ) { g_evt_i = 0; g_evt_cyc ^= 1; }
            /* advance ERDP to the new dequeue pointer (bit3 = EHB, keep clear) */
            wr64(g_rt, RT_IR0+IR_ERDP, (u64)(uintptr_t)&g_evt[g_evt_i] | (1u<<3));
            return 1;
        }
        spin(200);
    }
    return 0;
}

/* enqueue a command TRB, ring the command doorbell, wait for its completion.
 * Returns the completion code (1 = success), or 0 on timeout. `slot_out` gets
 * the slot id from the completion event. */
static int run_command(u64 param, u32 control, int *slot_out)
{
    trb_t *t = &g_cmd[g_cmd_i];
    trb_t ev;
    t->param = param; t->status = 0;
    t->control = control | (u32)g_cmd_cyc;
    if (++g_cmd_i >= CMD_RING_SZ-1) {        /* leave the last TRB as the Link */
        g_cmd[CMD_RING_SZ-1].param = (u64)(uintptr_t)g_cmd;
        g_cmd[CMD_RING_SZ-1].status = 0;
        g_cmd[CMD_RING_SZ-1].control = TRB_TYPE(TR_LINK) | (1u<<1) /*TC*/ | (u32)g_cmd_cyc;
        g_cmd_i = 0; g_cmd_cyc ^= 1;
    }
    wr32(g_db, 0, 0);                        /* doorbell 0 = command ring */
    for (;;) {
        if (!poll_event(&ev, 2000000)) return 0;
        if (TRB_GET_TYPE(ev.control) == TR_CMD_COMPLETE) {
            if (slot_out) *slot_out = (ev.control >> 24) & 0xFF;
            return (ev.status >> 24) & 0xFF;
        }
        /* ignore port-status-change / stray events and keep waiting */
    }
}

static void reset_port(int p)
{
    u32 sc = rd32(g_op, OP_PORTSC(p));
    wr32(g_op, OP_PORTSC(p), (sc & ~PORTSC_RW1CS) | PORTSC_PR | PORTSC_PP);
    { int t = 500000; while (t-- > 0) { sc = rd32(g_op, OP_PORTSC(p));
        if (sc & PORTSC_PRC) break; spin(20); } }
    wr32(g_op, OP_PORTSC(p), (rd32(g_op, OP_PORTSC(p)) & ~PORTSC_RW1CS) | PORTSC_PRC); /* ack */
    { int t = 200000; while (t--) spin(4); }
}

int uno_xhci_init(void)
{
    pci_dev d; u64 bar; u32 hcs1, hcs2, hcc; int i, nscratch, tries;

    g_err = 1;
    if (!find_xhci(&d)) return 0;
    pci_enable_bus_master(&d);
    bar = pci_bar(&d, 0);
    if (!bar) return 0;
    g_cap = (volatile u8 *)(uintptr_t)bar;

    g_caplen   = rd32(g_cap, CAP_CAPLENGTH) & 0xFF;
    hcs1       = rd32(g_cap, CAP_HCSPARAMS1);
    hcs2       = rd32(g_cap, CAP_HCSPARAMS2);
    hcc        = rd32(g_cap, CAP_HCCPARAMS1);
    g_maxslots = hcs1 & 0xFF;
    g_maxports = (hcs1 >> 24) & 0xFF;
    g_csz      = (hcc >> 2) & 1;               /* 1 => 64-byte contexts */
    g_op = g_cap + g_caplen;
    g_rt = g_cap + (rd32(g_cap, CAP_RTSOFF) & ~0x1Fu);
    g_db = g_cap + (rd32(g_cap, CAP_DBOFF)  & ~0x3u);
    if (g_maxslots > MAX_SLOTS_SUP) g_maxslots = MAX_SLOTS_SUP;

    g_err = 2;                                 /* wait for Controller Not Ready */
    for (tries = 1000000; (rd32(g_op, OP_USBSTS) & STS_CNR) && tries; tries--) spin(4);

    /* halt, then reset */
    wr32(g_op, OP_USBCMD, rd32(g_op, OP_USBCMD) & ~CMD_RS);
    for (tries = 1000000; !(rd32(g_op, OP_USBSTS) & STS_HCH) && tries; tries--) spin(4);
    wr32(g_op, OP_USBCMD, CMD_HCRST);
    g_err = 3;
    for (tries = 2000000; (rd32(g_op, OP_USBCMD) & CMD_HCRST) && tries; tries--) spin(4);
    if (rd32(g_op, OP_USBCMD) & CMD_HCRST) return 0;
    for (tries = 1000000; (rd32(g_op, OP_USBSTS) & STS_CNR) && tries; tries--) spin(4);

    /* max device slots */
    wr32(g_op, OP_CONFIG, (rd32(g_op, OP_CONFIG) & ~0xFFu) | (u32)g_maxslots);

    /* scratchpad buffers (if the controller needs them) */
    nscratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (nscratch > MAX_SCRATCH) nscratch = MAX_SCRATCH;
    for (i = 0; i < nscratch; i++) g_scratch_arr[i] = (u64)(uintptr_t)g_scratch_buf[i];

    /* Device Context Base Address Array; entry 0 = scratchpad array */
    for (i = 0; i <= g_maxslots; i++) g_dcbaa[i] = 0;
    if (nscratch) g_dcbaa[0] = (u64)(uintptr_t)g_scratch_arr;
    wr64(g_op, OP_DCBAAP, (u64)(uintptr_t)g_dcbaa);

    /* command ring: cycle starts at 1; CRCR points at it with RCS=1 */
    for (i = 0; i < CMD_RING_SZ; i++) { g_cmd[i].param=0; g_cmd[i].status=0; g_cmd[i].control=0; }
    g_cmd_i = 0; g_cmd_cyc = 1;
    wr64(g_op, OP_CRCR, (u64)(uintptr_t)g_cmd | 1u);

    /* event ring: one segment + a 1-entry ERST; interrupter 0 */
    for (i = 0; i < EVT_RING_SZ; i++) { g_evt[i].param=0; g_evt[i].status=0; g_evt[i].control=0; }
    g_evt_i = 0; g_evt_cyc = 1;
    g_erst[0].base = (u64)(uintptr_t)g_evt; g_erst[0].size = EVT_RING_SZ; g_erst[0].rsvd = 0;
    wr32(g_rt, RT_IR0+IR_ERSTSZ, 1);
    wr64(g_rt, RT_IR0+IR_ERDP,   (u64)(uintptr_t)g_evt);
    wr64(g_rt, RT_IR0+IR_ERSTBA, (u64)(uintptr_t)g_erst);
    wr32(g_rt, RT_IR0+IR_IMAN,   rd32(g_rt, RT_IR0+IR_IMAN) | 0x2 /*IE*/);

    /* run */
    g_err = 4;
    wr32(g_op, OP_USBCMD, rd32(g_op, OP_USBCMD) | CMD_RS);
    for (tries = 1000000; (rd32(g_op, OP_USBSTS) & STS_HCH) && tries; tries--) spin(4);
    if (rd32(g_op, OP_USBSTS) & STS_HCH) return 0;

    g_present = 1; g_err = 0;

    /* sanity: a No-Op command should complete (proves the rings + doorbell) */
    (void)run_command(0, TRB_TYPE(TR_NOOP_CMD), 0);

    /* power + reset every root port, count the connected ones */
    g_nports_conn = 0;
    for (i = 1; i <= g_maxports; i++) {
        u32 sc = rd32(g_op, OP_PORTSC(i));
        if (!(sc & PORTSC_PP)) { wr32(g_op, OP_PORTSC(i), (sc & ~PORTSC_RW1CS) | PORTSC_PP);
                                 { int t=20000; while(t--) spin(4); } }
        sc = rd32(g_op, OP_PORTSC(i));
        if (sc & PORTSC_CCS) { reset_port(i);
            if (rd32(g_op, OP_PORTSC(i)) & PORTSC_CCS) g_nports_conn++; }
    }
    return 1;
}

int uno_xhci_dev_count(void) { return g_ndevs; }
const uno_usb_dev *uno_xhci_dev(int i) { (void)i; return 0; }   /* Phase B */

void uno_xhci_status(int *present, int *nports, int *ndevs, unsigned *err)
{
    if (present) *present = g_present;
    if (nports)  *nports  = g_nports_conn;
    if (ndevs)   *ndevs   = g_ndevs;
    if (err)     *err     = g_err;
}

#endif /* UNO_XHCI */
