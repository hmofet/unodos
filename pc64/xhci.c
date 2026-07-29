/* UnoDOS/pc64 - xHCI host-controller driver (see xhci.h). Phase A: controller
 * bring-up (reset -> rings -> run) + root-hub port reset/scan. Enumeration
 * (Address Device + descriptor fetch) builds on these rings. Polled, no IRQs.
 *
 * DMA is identity-mapped (UEFI boot services alive => a static .bss buffer's
 * address is its physical address), the same trick e1000 uses. */
#include "xhci.h"

#ifndef UNO_XHCI

int  uno_xhci_supported(void) { return 0; }
int  uno_xhci_init(void) { return 0; }
int  uno_xhci_dev_count(void) { return 0; }
const uno_usb_dev *uno_xhci_dev(int i) { (void)i; return 0; }
void uno_xhci_status(int *p, int *n, int *d, unsigned *e)
{ if (p)*p=0; if (n)*n=0; if (d)*d=0; if (e)*e=0; }
void uno_xhci_diag(int *s, int *a, int *d, int *sp)
{ if (s)*s=0; if (a)*a=0; if (d)*d=0; if (sp)*sp=0; }
void uno_xhci_diag2(unsigned *sts, unsigned *ev0, int *disc) { if (sts)*sts=0; if (ev0)*ev0=0; if (disc)*disc=0; }
int uno_usb_control(int dev, unsigned char rt, unsigned char req, unsigned short val,
                    unsigned short idx, void *data, int len)
{ (void)dev;(void)rt;(void)req;(void)val;(void)idx;(void)data;(void)len; return -1; }
int uno_usb_get_config(int dev, void *buf, int len) { (void)dev;(void)buf;(void)len; return -1; }
int uno_usb_set_config(int dev, int cfg) { (void)dev;(void)cfg; return -1; }
int uno_usb_setup_bulk(int dev, int i, int o, int im, int om) { (void)dev;(void)i;(void)o;(void)im;(void)om; return -1; }
int uno_usb_bulk_out(int dev, void *d, int l) { (void)dev;(void)d;(void)l; return -1; }
int uno_usb_bulk_in(int dev, void *d, int l) { (void)dev;(void)d;(void)l; return -1; }
int uno_usb_setup_intr_in(int dev, int a, int m) { (void)dev;(void)a;(void)m; return -1; }
int uno_usb_intr_in(int dev, void *d, int l) { (void)dev;(void)d;(void)l; return -1; }
int uno_usb_bulk_in_arm(int dev, void *d, int l) { (void)dev;(void)d;(void)l; return -1; }
int uno_usb_bulk_in_poll(int dev) { (void)dev; return -1; }

#else  /* ===================== UNO_XHCI enabled ========================= */

#include "pc64_pci.h"
#include <stdint.h>

/* detach the firmware's USB driver from this controller first (uefi_main) */
int uno_pc64_pci_disconnect(int bus, int dev, int fn);
int uno_pc64_detached(void);
void uno_pc64_delay_ms(int ms);   /* TSC-backed once detached - a real ms */

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
#define STS_HCE   (1u<<12)   /* host controller error (fatal) */

#define PORTSC_CCS (1u<<0)    /* current connect status */
#define PORTSC_PED (1u<<1)    /* port enabled */
#define PORTSC_PR  (1u<<4)    /* port reset */
#define PORTSC_PP  (1u<<9)    /* port power */
#define PORTSC_CSC (1u<<17)   /* connect status change */
#define PORTSC_PRC (1u<<21)   /* port reset change */
#define PORTSC_SPEED(v) (((v)>>10)&0xF)
/* bits that must be cleared before writing PORTSC back: the write-1-to-clear
 * change bits (17-23) AND PED (bit1, which is write-1-to-DISABLE the port). */
#define PORTSC_RW1CS (PORTSC_PED|PORTSC_CSC|PORTSC_PRC|(1u<<18)|(1u<<19)|(1u<<20)|(1u<<22)|(1u<<23))
/* just the write-1-to-clear CHANGE bits (17-23), i.e. RW1CS without PED - the
 * set of bits to ack so a port stops re-firing status-change events. */
#define PORTSC_CHG (PORTSC_RW1CS & ~PORTSC_PED)

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
#define TR_NORMAL      1
#define TR_LINK        6
#define TR_ENABLE_SLOT 9
#define TR_ADDRESS_DEV 11
#define TR_CONFIG_EP   12
#define TR_RESET_EP    14      /* endpoint Halted -> Stopped                 */
#define TR_STOP_EP     15      /* abort whatever is running on the ring      */
#define TR_SET_TR_DEQ  16      /* re-point the transfer ring's dequeue cursor */
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

/* per-device: input context (control+slot+ep0), output device context, and an
 * EP0 (control) transfer ring, all 64-byte aligned. */
/* A hub chain multiplies devices fast: the test box alone has a hub (two
 * faces), a keyboard/mouse dongle and a boot stick. 8 was enough when only
 * root ports enumerated. */
#define MAX_DEV 16
#define EP0_RING_SZ 16
/* 64-byte contexts with a max device-context index of 31 need
 * (1 input-control + 1 slot + 31 endpoint) * 64 = 2112 bytes; the old 2048
 * sizing overflowed by one endpoint context. 4096 leaves headroom. */
static u8    g_inctx[MAX_DEV][4096]  __attribute__((aligned(64)));
static u8    g_devctx[MAX_DEV][4096] __attribute__((aligned(64)));
static trb_t g_ep0[MAX_DEV][EP0_RING_SZ] __attribute__((aligned(64)));
static int   g_ep0_i[MAX_DEV], g_ep0_cyc[MAX_DEV];
static u8    g_descbuf[256] __attribute__((aligned(64)));
static uno_usb_dev g_devs[MAX_DEV];
/* where each device sits: route string, hub tier (0 = on a root port) and the
 * root-hub port its whole chain hangs off. Kept beside g_devs rather than in
 * it so the public uno_usb_dev stays what class drivers already consume. */
static int g_dev_route[MAX_DEV], g_dev_tier[MAX_DEV], g_dev_rport[MAX_DEV];

/* bulk transfer rings (one in + one out per device, set up by Configure EP) */
#define BULK_RING_SZ 32
static trb_t g_bin[MAX_DEV][BULK_RING_SZ]  __attribute__((aligned(64)));
static trb_t g_bout[MAX_DEV][BULK_RING_SZ] __attribute__((aligned(64)));
static int   g_bin_i[MAX_DEV], g_bin_cyc[MAX_DEV], g_bin_dci[MAX_DEV];
static int   g_bout_i[MAX_DEV], g_bout_cyc[MAX_DEV], g_bout_dci[MAX_DEV];

/* interrupt-IN endpoint (HID) - one outstanding TRB into a per-device buffer,
 * polled non-blocking each frame; reuses the g_bin ring of that device slot. */
static int   g_intr_dci[MAX_DEV], g_intr_mps[MAX_DEV];

/* Outstanding ASYNC transfers (the HID interrupt-IN and the NIC's armed
 * bulk-IN). All consumers share interrupter 0's single event ring, so a
 * blocking waiter (control/bulk) could dequeue a completion that belongs to
 * an async TRB - and an async poll could dequeue a sync one. Every dequeued
 * transfer event is therefore ROUTED: if its TRB pointer matches a registered
 * async transfer, the completion is stashed for that transfer's owner instead
 * of being returned to (or dropped by) whoever happened to dequeue it. */
enum { ASY_INTR = 0, ASY_BIN = 1 };
static u64 g_async_trb[MAX_DEV][2];      /* posted TRB address; 0 = none     */
static u32 g_async_sts[MAX_DEV][2];      /* stashed ev.status; 0 = none (a   */
                                         /* real completion has cc>=1 in     */
                                         /* bits 24-31, so 0 is free)        */
static int g_abin_len[MAX_DEV];          /* armed bulk-IN posted length      */
static u8    g_hidbuf[MAX_DEV][64] __attribute__((aligned(64)));

/* ---- state --------------------------------------------------------------- */
static volatile u8 *g_cap;         /* MMIO base */
static volatile u8 *g_op, *g_rt, *g_db;
static int g_caplen, g_csz, g_maxslots, g_maxports;
static int g_present, g_nports_conn, g_ndevs;
static unsigned g_err;             /* stage/failure code for the diagnostic */
static int g_dbg_slot, g_dbg_addr, g_dbg_desc, g_dbg_speed, g_dbg_disc; /* enum diagnostics */
static int g_dbg_cc = -99, g_dbg_resid = -99;  /* last control_xfer raw cc + residual */
static unsigned g_dbg_sts, g_dbg_ev0;   /* USBSTS + first event TRB control */
static int g_cmd_i, g_cmd_cyc;     /* command ring enqueue + producer cycle */
static int g_evt_i, g_evt_cyc;     /* event ring dequeue + consumer cycle */

static u32 rd32(volatile u8 *b, u32 o) { return *(volatile u32 *)(b+o); }
static void wr32(volatile u8 *b, u32 o, u32 v) { *(volatile u32 *)(b+o)=v; }
static u64 rd64(volatile u8 *b, u32 o) { return *(volatile u64 *)(b+o); }
static void wr64(volatile u8 *b, u32 o, u64 v) { *(volatile u64 *)(b+o)=v; }
static void spin(volatile int n){ while(n-->0) __asm__ volatile(""); }
static void mdelay(int ms);

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
 * state; copy it out. Returns 1 if an event arrived, 0 on timeout.
 *
 * `budget` is a SPIN COUNT, not a duration, which makes it a poor deadline:
 * how long 5,000,000 iterations of this loop actually take depends on the
 * compiler's optimisation level and the machine. That is not academic - it is
 * why a USB mass-storage read completed in a debug build and timed out in a
 * production build of the same source, the debug build's slacker code
 * accidentally granting several times the wall-clock patience. Use it for
 * "check the ring", and poll_event_ms() below for anything with a deadline. */
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

/* ...the same wait, expressed in milliseconds.
 *
 * A device gets a real deadline: a flash stick servicing a READ(10) can take
 * tens of milliseconds, and an emulated one backed by a host file can take
 * longer still. Timing out early is worse than waiting - the transfer is
 * STILL RUNNING, so its completion arrives later and lands in whoever polls
 * next, and the endpoint has to be torn down to get back in step. */
static int poll_event_ms(trb_t *out, int ms)
{
    int t;
    for (t = 0; t <= ms; t++) {
        if (poll_event(out, 64)) return 1;      /* one sweep of the ring */
        /* uno_pc64_delay_ms, not the local mdelay(): mdelay is a calibrated
         * spin whose real duration depends on the optimiser, which is the very
         * thing that made this driver's deadlines fictional. This one is TSC-
         * backed, so "5 seconds" means five seconds in every build. */
        uno_pc64_delay_ms(1);
    }
    return 0;
}

/* enqueue a command TRB, ring the command doorbell, wait for its completion.
 * Returns the completion code (1 = success), or 0 on timeout. `slot_out` gets
 * the slot id from the completion event. */
static int run_command(u64 param, u32 control, int *slot_out)
{
    trb_t *t = &g_cmd[g_cmd_i];
    u64 mytrb = (u64)(uintptr_t)t;           /* the completion event points back here */
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
    { int strays = 0;                        /* non-matching events consumed */
    for (;;) {
        if (!poll_event_ms(&ev, 1000)) return 0;   /* command completion: 1 s */
        /* only OUR command's completion (match the TRB pointer) - skip stale or
         * other events so a leftover completion can't be mistaken for this one */
        if (TRB_GET_TYPE(ev.control) == TR_CMD_COMPLETE && ev.param == mytrb) {
            if (slot_out) *slot_out = (ev.control >> 24) & 0xFF;
            return (ev.status >> 24) & 0xFF;
        }
        /* A real USB3 adapter floods the ring with Port-Status-Change events
         * that keep it non-empty, so poll_event never times out and this loop
         * would spin forever. Bound the strays and fail like the timeout path
         * (QEMU emits no such storm, so this cap is never reached there). */
        if (++strays > 4096) return 0;
    }
    }
}

static void mdelay(int ms) { while (ms-- > 0) { volatile int n = 200000; while (n-- > 0) __asm__ volatile(""); } }

/* ---- opt-in debug console (QEMU port 0x402) - METAL-UNSAFE ---------------- */
#ifdef UNO_DBGCON
static void xd_c(char c){ __asm__ volatile("outb %0,%1"::"a"((unsigned char)c),"Nd"((unsigned short)0x402)); }
static void xd(const char *s){ while(*s) xd_c(*s++); }
static void xd_i(int v){ char b[12]; int i=0; unsigned u; if(v<0){xd_c('-');u=(unsigned)(-v);}else u=(unsigned)v;
    if(!u)xd_c('0'); while(u){b[i++]=(char)('0'+u%10);u/=10;} while(i)xd_c(b[--i]); }
static void xd_h(unsigned v){ const char *h="0123456789abcdef"; int i; xd("0x"); for(i=28;i>=0;i-=4) xd_c(h[(v>>i)&0xF]); }
#else
#define xd(s)   ((void)0)
#define xd_c(c) ((void)0)
#define xd_i(v) ((void)0)
#define xd_h(v) ((void)0)
#endif

static void reset_port(int p)
{
    u32 sc = rd32(g_op, OP_PORTSC(p));
    wr32(g_op, OP_PORTSC(p), (sc & ~PORTSC_RW1CS) | PORTSC_PR | PORTSC_PP);
    { int t = 1000000; while (t-- > 0) { sc = rd32(g_op, OP_PORTSC(p));
        if (sc & PORTSC_PRC) break; spin(20); } }
    /* ack the reset AND every other change bit that is currently set (CSC/PLC/
     * etc.). Leaving CSC/PLC asserted makes a SuperSpeed port re-fire
     * Port-Status-Change events forever, which is what floods the event ring
     * and hangs run_command/poll_xfer on real hardware. Mask PED/PP out of the
     * write so we neither disable the port nor drop its power. */
    { u32 sc = rd32(g_op, OP_PORTSC(p));
      wr32(g_op, OP_PORTSC(p), (sc & ~PORTSC_RW1CS) | (sc & PORTSC_CHG)); }
    mdelay(20);                              /* USB reset recovery (>=10ms) before addressing */
}

/* ---- enumeration: Enable Slot -> Address Device -> GET_DESCRIPTOR --------- */
static void ctx_wr(u8 *ctx, int off, u32 v) { *(volatile u32 *)(ctx + off) = v; }
static int  mps_for_speed(int sp) { return sp == 4 ? 512 : sp == 3 ? 64 : 8; }

/* enqueue one TRB on a transfer ring, managing the cycle bit + a Link-TRB wrap
 * (last slot reserved for the Link). Control-transfer stages and bulk transfers
 * are single-TRB TDs, so a Link between them is harmless. */
static void ep_push(trb_t *ring, int *enq, int *cyc, int size, u64 param, u32 status, u32 ctl)
{
    ring[*enq].param = param; ring[*enq].status = status;
    ring[*enq].control = ctl | (u32)*cyc;
    if (++(*enq) >= size - 1) {
        ring[size-1].param = (u64)(uintptr_t)ring; ring[size-1].status = 0;
        ring[size-1].control = TRB_TYPE(TR_LINK) | (1u<<1) | (u32)*cyc;
        *enq = 0; *cyc ^= 1;
    }
}

/* route one transfer event: if it completes a registered async TRB, stash it
 * for that owner and return 0; else 1 = the event is the caller's. */
static int route_event(const trb_t *ev)
{
    int d, k;
    for (d = 0; d < MAX_DEV; d++)
        for (k = 0; k < 2; k++)
            if (g_async_trb[d][k] && ev->param == g_async_trb[d][k]) {
                g_async_sts[d][k] = ev->status;
                g_async_trb[d][k] = 0;
                return 0;
            }
    return 1;
}

/* wait up to `ms` milliseconds for the next transfer event; returns the
 * completion code (1/13 = ok), fills the residual (untransferred bytes of the
 * last TRB). -1 on timeout. Events that belong to outstanding async TRBs are
 * stashed, not returned. */
static int poll_xfer(int *residual, int ms)
{
    trb_t ev;
    int strays = 0;                          /* non-matching events consumed */
    for (;;) {
        if (!poll_event_ms(&ev, ms)) return -1;
        if (TRB_GET_TYPE(ev.control) != TR_XFER_EVENT) {
            if (++strays > 4096) return -1;   /* PSC storm keeps the ring full */
            continue;
        }
        if (!route_event(&ev)) {
            if (++strays > 4096) return -1;   /* only async completions arriving */
            continue;
        }
        if (residual) *residual = ev.status & 0xFFFFFF;
        return (ev.status >> 24) & 0xFF;
    }
}

/* non-blocking check of async transfer (dev,kind): drain any ready events
 * through the router, then return the stashed completion code (0 = still
 * outstanding), filling *residual. */
static int async_poll(int dev, int kind, int *residual)
{
    trb_t ev;
    int guard = EVT_RING_SZ + 4;
    while (!g_async_sts[dev][kind] && guard-- > 0) {
        if (!poll_event(&ev, 1000)) break;
        if (TRB_GET_TYPE(ev.control) != TR_XFER_EVENT) continue;
        route_event(&ev);      /* strays (timed-out sync transfers) drop */
    }
    if (!g_async_sts[dev][kind]) return 0;
    {
        u32 s = g_async_sts[dev][kind];
        g_async_sts[dev][kind] = 0;
        if (residual) *residual = (int)(s & 0xFFFFFF);
        return (int)(s >> 24) & 0xFF;
    }
}

/* a full control transfer on EP0. Returns bytes transferred (>=0) or -1. */
static int control_xfer(int di, u8 rt, u8 req, u16 val, u16 idx, u8 *buf, int len)
{
    int slot = g_devs[di].slot, in = (rt & 0x80) != 0, resid = 0, cc;
    u32 trt = len ? (in ? 3u : 2u) : 0u;         /* transfer type: IN/OUT/no-data */
    u64 setup = (u64)rt | ((u64)req<<8) | ((u64)val<<16) | ((u64)idx<<32) | ((u64)len<<48);
    ep_push(g_ep0[di], &g_ep0_i[di], &g_ep0_cyc[di], EP0_RING_SZ,
            setup, 8, TRB_TYPE(2) | (1u<<6) | (trt<<16));      /* Setup (immediate data) */
    if (len)
        ep_push(g_ep0[di], &g_ep0_i[di], &g_ep0_cyc[di], EP0_RING_SZ,
                (u64)(uintptr_t)buf, (u32)len, TRB_TYPE(3) | (in ? (1u<<16) : 0)); /* Data */
    ep_push(g_ep0[di], &g_ep0_i[di], &g_ep0_cyc[di], EP0_RING_SZ,
            0, 0, TRB_TYPE(4) | ((len && in) ? 0 : (1u<<16)) | (1u<<5));  /* Status (opp dir, IOC) */
    wr32(g_db, slot*4, 1);                        /* slot doorbell, EP0 (DCI 1) */
    cc = poll_xfer(&resid, 2000);        /* control: 2 s per USB spec-ish */
    g_dbg_cc = cc; g_dbg_resid = resid;
    return (cc == CC_SUCCESS || cc == 13) ? (len - resid) : -1;
}

static int get_device_descriptor(int di, int slot, u8 *out, int len)
{ (void)slot; return control_xfer(di, 0x80, 6, 0x0100, 0, out, len) >= 0; }

/* ---- device enumeration, including everything behind a hub ----------------
 *
 * xHCI addresses a device by WHERE IT IS, not by walking to it: the slot
 * context carries the root-hub port the whole chain hangs off, plus a 20-bit
 * Route String of 4-bit port numbers, one nibble per hub tier. So enumerating
 * a device three hubs deep is the same command as one on a root port - the
 * work is getting the hub to power and reset its downstream ports first, and
 * filling in the route.
 *
 * `route`/`tier` describe the parent: tier 0 = directly on a root port (route
 * 0), and a child of a hub at tier T takes route |= port << (4*T), tier T+1.
 * tt_slot/tt_port carry the Transaction Translator for a low/full-speed device
 * behind a high-speed hub - without them the controller has no idea how to
 * split-transact to it, and the device answers nothing. */
static void hub_scan(int hub_di);              /* recursion: hub -> its ports */

static int enumerate_dev(int root_port, u32 route, int tier, int speed,
                         int tt_slot, int tt_port)
{
    int slot = 0, cc, di, mps, st, i;
    if (g_ndevs >= MAX_DEV) return -1;
    di = g_ndevs;
    g_dbg_speed = speed;

    cc = run_command(0, TRB_TYPE(TR_ENABLE_SLOT), &slot);
    g_dbg_slot = (cc == CC_SUCCESS) ? slot : -(int)cc;
    g_dbg_sts  = rd32(g_op, OP_USBSTS);
    g_dbg_ev0  = g_evt[0].control;
    if (cc != CC_SUCCESS || slot == 0 || slot > g_maxslots) return -1;

    for (i = 0; i < EP0_RING_SZ; i++) { g_ep0[di][i].param=0; g_ep0[di][i].status=0; g_ep0[di][i].control=0; }
    g_ep0_i[di] = 0; g_ep0_cyc[di] = 1;
    for (i = 0; i < (int)sizeof g_inctx[di]; i++) { g_inctx[di][i]=0; g_devctx[di][i]=0; }

    st = g_csz ? 64 : 32; mps = mps_for_speed(speed);
    ctx_wr(g_inctx[di], 4, 0x3);                                 /* add flags A0|A1 */
    /* slot DW0: Route String 19:0, Speed 23:20, Context Entries 31:27 */
    ctx_wr(g_inctx[di], st,    (1u<<27) | ((u32)speed<<20) | (route & 0xFFFFFu));
    /* slot DW1: Root Hub Port Number 23:16 */
    ctx_wr(g_inctx[di], st+4,  ((u32)root_port<<16));
    /* slot DW2: TT Hub Slot ID 7:0, TT Port Number 15:8 - only meaningful for a
     * low/full-speed device reached through a high-speed hub's TT. */
    if (tt_slot)
        ctx_wr(g_inctx[di], st+8, (u32)(tt_slot & 0xFF) | ((u32)(tt_port & 0xFF) << 8));
    ctx_wr(g_inctx[di], 2*st+4, ((u32)mps<<16) | (4u<<3) | (3u<<1)); /* EP0: Control, CErr=3, MPS */
    { u64 tr = (u64)(uintptr_t)g_ep0[di] | 1u;                   /* TR dequeue ptr + DCS */
      ctx_wr(g_inctx[di], 2*st+8,  (u32)tr);
      ctx_wr(g_inctx[di], 2*st+12, (u32)(tr>>32)); }
    ctx_wr(g_inctx[di], 2*st+16, 8);                             /* avg TRB length */

    g_dcbaa[slot] = (u64)(uintptr_t)g_devctx[di];               /* output ctx, before Address Device */
    cc = run_command((u64)(uintptr_t)g_inctx[di], TRB_TYPE(TR_ADDRESS_DEV) | ((u32)slot<<24), 0);
    g_dbg_addr = (int)cc;
    if (cc != CC_SUCCESS) return -1;
    mdelay(2);                                                  /* let the address settle */

    /* publish slot/port/speed BEFORE the descriptor fetch: control_xfer reads
     * g_devs[di].slot to ring the right doorbell (it's a full transfer API now,
     * not the old slot-parameter helper). */
    g_devs[di].slot = slot; g_devs[di].port = root_port; g_devs[di].speed = speed;
    g_dev_route[di] = (int)route; g_dev_tier[di] = tier; g_dev_rport[di] = root_port;

    for (i = 0; i < 18; i++) g_descbuf[i] = 0;
    g_dbg_desc = get_device_descriptor(di, slot, g_descbuf, 18) ? g_descbuf[1] : -1;
    g_dbg_sts = rd32(g_op, OP_USBSTS);           /* post-transfer status (HCE shows here) */
    xd("[xhci] rport="); xd_i(root_port); xd(" route="); xd_h((unsigned)route);
    xd(" tier="); xd_i(tier); xd(" slot="); xd_i(slot);
    xd(" spd="); xd_i(speed); xd(" desc_cc="); xd_i(g_dbg_cc);
    xd(" bDescType="); xd_i(g_dbg_desc);
    xd(" vid="); xd_h((unsigned)(g_descbuf[8]|(g_descbuf[9]<<8)));
    xd(" pid="); xd_h((unsigned)(g_descbuf[10]|(g_descbuf[11]<<8))); xd("\n");
    if (g_dbg_desc != 1) return -1;                             /* must be a device descriptor */
    g_devs[di].vendor  = (u16)(g_descbuf[8]  | (g_descbuf[9]  << 8));
    g_devs[di].product = (u16)(g_descbuf[10] | (g_descbuf[11] << 8));
    g_devs[di].dev_class    = g_descbuf[4];
    g_devs[di].dev_subclass = g_descbuf[5];
    g_devs[di].dev_proto    = g_descbuf[6];
    g_ndevs++;
    if (g_devs[di].dev_class == 0x09) hub_scan(di);              /* it is a hub */
    return di;
}

/* ---- USB hub class ---------------------------------------------------------
 * A hub is just a device with a class-specific descriptor and per-port feature
 * requests. Everything downstream of it is invisible until we power its ports
 * and reset each one - the firmware did that during ITS enumeration, but we
 * reset the controller out from under it, so we have to do it again ourselves.
 *
 * Requests are the standard hub set (USB 2.0 §11.24):
 *   GET_DESCRIPTOR(hub)  0xA0 / 6 / type<<8      -> bNbrPorts, power-on delay
 *   GET_STATUS(port)     0xA3 / 0 / idx=port     -> wPortStatus, wPortChange
 *   SET_FEATURE(port)    0x23 / 3 / feature      -> PORT_POWER, PORT_RESET
 *   CLEAR_FEATURE(port)  0x23 / 1 / feature      -> the C_* change bits
 * ======================================================================== */
#define HUB_DESC_USB2   0x29
#define HUB_DESC_SS     0x2A
#define PORT_RESET      4
#define PORT_POWER      8
#define C_PORT_CONN     16
#define C_PORT_RESET    20

#define PS_CONNECTION   (1u<<0)
#define PS_ENABLE       (1u<<1)
#define PS_RESET        (1u<<4)
#define PS_LOWSPEED     (1u<<9)
#define PS_HIGHSPEED    (1u<<10)

/* Tell the CONTROLLER this slot is a hub. Without the Hub bit and port count
 * in its slot context, Address Device for anything downstream is rejected -
 * the controller will not route to a device behind something it thinks is an
 * ordinary peripheral. Configure Endpoint with only A0 set updates the slot
 * context and leaves the endpoints alone. */
static int hub_configure(int di, int nports)
{
    int st = g_csz ? 64 : 32, slot = g_devs[di].slot, i;
    u32 sdw0, sdw1;
    for (i = 0; i < (int)sizeof g_inctx[di]; i++) g_inctx[di][i] = 0;
    ctx_wr(g_inctx[di], 4, 1u);                       /* A0: slot context only */
    sdw0 = *(volatile u32 *)(g_devctx[di] + 0);
    sdw1 = *(volatile u32 *)(g_devctx[di] + 4);
    sdw0 |= (1u << 26);                               /* Hub                   */
    sdw1 = (sdw1 & ~(0xFFu << 24)) | ((u32)(nports & 0xFF) << 24);  /* NumPorts */
    ctx_wr(g_inctx[di], st + 0, sdw0);
    ctx_wr(g_inctx[di], st + 4, sdw1);
    return run_command((u64)(uintptr_t)g_inctx[di],
                       TRB_TYPE(TR_CONFIG_EP) | ((u32)slot << 24), 0) == CC_SUCCESS;
}

static int hub_port_status(int di, int port, u32 *st_out)
{
    u8 b[4] = { 0, 0, 0, 0 };
    if (control_xfer(di, 0xA3, 0, 0, (u16)port, b, 4) < 0) return 0;
    *st_out = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
    return 1;
}
static int hub_set(int di, int port, int feat)
{ return control_xfer(di, 0x23, 3, (u16)feat, (u16)port, 0, 0) >= 0; }
static void hub_clear(int di, int port, int feat)
{ (void)control_xfer(di, 0x23, 1, (u16)feat, (u16)port, 0, 0); }

static void hub_scan(int hub_di)
{
    static u8 hd[16];
    int ss = (g_devs[hub_di].speed == 4);
    int nports, i, tier = g_dev_tier[hub_di] + 1;
    int pwr_ms, powered = 0;

    /* 5 nibbles of route string is the architectural limit, and a device on the
     * 5th tier still needs its own nibble - stop before authoring nonsense. */
    if (tier > 5) { xd("[xhci] hub too deep, stopping\n"); return; }

    /* CONFIGURE THE HUB FIRST. A hub does not power its downstream ports until
     * it is in the Configured state - port power is a property of the
     * configuration, and in the Address state a class request to it may simply
     * STALL. Skipping this is why the ZimaBlade came up detached with a dark
     * mouse, a dead keyboard and no boot volume: every port behind the hub had
     * no VBUS. QEMU's hub model powers up regardless and hid the omission
     * completely, which is exactly the kind of thing only metal tells you. */
    if (uno_usb_set_config(hub_di, 1) < 0) {
        xd("[xhci] hub SET_CONFIGURATION failed\n");
        return;
    }
    mdelay(5);

    if (control_xfer(hub_di, 0xA0, 6, (u16)((ss ? HUB_DESC_SS : HUB_DESC_USB2) << 8),
                     0, hd, ss ? 12 : 9) < 0) {
        xd("[xhci] hub descriptor failed\n");
        return;
    }
    nports = hd[2];
    pwr_ms = hd[5] * 2;                    /* bPwrOn2PwrGood, 2 ms units */
    if (nports < 1 || nports > 15) return; /* a route nibble holds 1..15 */
    if (pwr_ms < 20) pwr_ms = 20;          /* spec floor, and cheap insurance */

    if (!hub_configure(hub_di, nports)) { xd("[xhci] hub slot config failed\n"); return; }

    xd("[xhci] hub slot="); xd_i(g_devs[hub_di].slot);
    xd(" ports="); xd_i(nports); xd(" tier="); xd_i(tier); xd("\n");

    /* Powering the ports is the whole point of getting this far, so notice when
     * it does not take rather than walking a set of dead ports afterwards. */
    for (i = 1; i <= nports; i++) if (hub_set(hub_di, i, PORT_POWER)) powered++;
    if (!powered) { xd("[xhci] hub powered no ports\n"); return; }
    if (powered != nports) { xd("[xhci] hub powered only "); xd_i(powered); xd(" ports\n"); }
    mdelay(pwr_ms);

    for (i = 1; i <= nports; i++) {
        u32 ps = 0, route;
        int spd, t, tts = 0, ttp = 0;
        if (!hub_port_status(hub_di, i, &ps)) continue;
        if (!(ps & PS_CONNECTION)) continue;
        hub_clear(hub_di, i, C_PORT_CONN);

        hub_set(hub_di, i, PORT_RESET);
        for (t = 0; t < 100; t++) {        /* reset completes in ~10-20 ms */
            mdelay(2);
            if (!hub_port_status(hub_di, i, &ps)) break;
            if (!(ps & PS_RESET) && (ps & PS_ENABLE)) break;
        }
        hub_clear(hub_di, i, C_PORT_RESET);
        if (!(ps & PS_ENABLE)) { xd("[xhci] hub port not enabled\n"); continue; }
        mdelay(10);                        /* USB reset recovery */

        /* Speed comes from the port, not the hub: a USB2 hub reports low/high
         * in its status bits and full speed by their absence. Downstream of a
         * SuperSpeed hub everything is SuperSpeed by construction. */
        spd = ss ? 4 : (ps & PS_LOWSPEED) ? 2 : (ps & PS_HIGHSPEED) ? 3 : 1;

        /* A low- or full-speed device behind a high-speed hub is reached by
         * split transactions through that hub's Transaction Translator, and
         * the controller has to be told which hub and port to split through.
         * Inherit the parent's TT if we are already downstream of one. */
        if (spd == 1 || spd == 2) {
            if (g_devs[hub_di].speed == 3) { tts = g_devs[hub_di].slot; ttp = i; }
            else { int st2 = g_csz ? 64 : 32;
                   u32 p2 = *(volatile u32 *)(g_devctx[hub_di] + st2 + 8);
                   tts = (int)(p2 & 0xFF); ttp = (int)((p2 >> 8) & 0xFF); }
        }

        route = (u32)g_dev_route[hub_di] | ((u32)(i & 0xF) << (4 * g_dev_tier[hub_di]));
        (void)enumerate_dev(g_dev_rport[hub_di], route, tier, spd, tts, ttp);
    }
}

static void enumerate_port(int port)
{
    (void)enumerate_dev(port, 0, 0, PORTSC_SPEED(rd32(g_op, OP_PORTSC(port))), 0, 0);
}

/* ---- USB transfer API for class drivers (dev = index in the device list) --
 *
 * Endpoint Context DW1: MaxPacketSize 31:16, MaxBurstSize 15:8, EPType 5:3,
 * CErr 2:1. MaxBurstSize left at 0 is correct for Full/High Speed bulk and
 * WRONG for SuperSpeed, where the device may burst up to bMaxBurst+1 packets
 * per service opportunity - see ss_burst() below. */
static void setup_ep(int di, int dci, int eptype, int mps, int burst, trb_t *ring)
{
    int st = g_csz ? 64 : 32, off = (dci + 1) * st;
    u64 tr = (u64)(uintptr_t)ring | 1u;                    /* dequeue ptr + DCS */
    if (burst < 0)   burst = 0;
    if (burst > 15)  burst = 15;                           /* field is 8 bits, spec caps SS at 15 */
    ctx_wr(g_inctx[di], off+4, ((u32)mps<<16) | ((u32)burst<<8)
                             | ((u32)eptype<<3) | (3u<<1)); /* MPS, burst, type, CErr=3 */
    ctx_wr(g_inctx[di], off+8,  (u32)tr);
    ctx_wr(g_inctx[di], off+12, (u32)(tr>>32));
    ctx_wr(g_inctx[di], off+16, 1024);                     /* average TRB length */
}

/* ---- endpoint error recovery (xHCI 4.6.8) --------------------------------
 * A transfer error (STALL, Babble, Transaction Error) leaves the endpoint
 * HALTED inside the host controller, and the ring's dequeue cursor pointing at
 * the TRB that died. Nothing on that endpoint will ever complete again until
 * the controller is told to move on - so the class driver's CLEAR_FEATURE
 * (which only the DEVICE hears) recovers half the problem and leaves the other
 * half wedged. The symptom is exactly what a USB boot showed: the first
 * READ(10) works, one errors, and every read after that fails forever.
 *
 * The sequence: Reset Endpoint (Halted -> Stopped), then Set TR Dequeue
 * Pointer to restart the ring cleanly, then drain any event the dead transfer
 * left behind so the NEXT transfer doesn't dequeue it and believe it.
 *
 * After a TIMEOUT the transfer may still be running, so it takes Stop Endpoint
 * first - resetting a running endpoint is a context-state error, and the TRB
 * would complete later into someone else's poll. */
static void ep_recover(int di, int dci, trb_t *ring, int *enq, int *cyc, int timed_out)
{
    int slot = g_devs[di].slot, i;
    u64 deq;
    trb_t ev;
    if (!slot || !dci) return;
    /* completion codes are ignored throughout: "the endpoint was not in the
     * state I assumed" is not a reason to leave it wedged. */
    if (timed_out)
        run_command(0, TRB_TYPE(TR_STOP_EP)  | ((u32)dci<<16) | ((u32)slot<<24), 0);
    else
        run_command(0, TRB_TYPE(TR_RESET_EP) | ((u32)dci<<16) | ((u32)slot<<24), 0);

    for (i = 0; i < BULK_RING_SZ; i++) { ring[i].param = 0; ring[i].status = 0;
                                         ring[i].control = 0; }
    *enq = 0; *cyc = 1;
    deq = (u64)(uintptr_t)ring | 1u;                        /* new cursor + DCS */
    run_command(deq, TRB_TYPE(TR_SET_TR_DEQ) | ((u32)dci<<16) | ((u32)slot<<24), 0);

    /* A transfer event for the TRB we just abandoned may still be queued.
     * Leaving it there hands the next caller a completion for a transfer that
     * no longer exists - the same class of lie the CSW tag check catches one
     * layer up in usbmsc. */
    for (i = 0; i < EVT_RING_SZ; i++) {
        if (!poll_event(&ev, 2000)) break;
        if (TRB_GET_TYPE(ev.control) == TR_XFER_EVENT) route_event(&ev);
    }
}

int uno_usb_control(int dev, unsigned char rt, unsigned char req,
                    unsigned short val, unsigned short idx, void *data, int len)
{
    if (dev < 0 || dev >= g_ndevs) return -1;
    return control_xfer(dev, rt, req, val, idx, (u8 *)data, len);
}
int uno_usb_get_config(int dev, void *buf, int len)
{ return uno_usb_control(dev, 0x80, 6, 0x0200, 0, buf, len); }        /* GET_DESCRIPTOR config */
int uno_usb_set_config(int dev, int cfg)
{ return uno_usb_control(dev, 0x00, 9, (unsigned short)cfg, 0, 0, 0); } /* SET_CONFIGURATION */

/* bMaxBurst for one endpoint, from its SuperSpeed Endpoint Companion.
 *
 * A SuperSpeed device's config descriptor puts a companion descriptor (type
 * 0x30) immediately after each endpoint descriptor, and its bMaxBurst says how
 * many packets beyond the first the device may send per service opportunity.
 * The host controller has to be told, or it services one packet where the
 * device sends several and the transfer errors out. A 1024-byte bulk max
 * packet is the tell that a device came up SuperSpeed.
 *
 * Done here rather than through the setup_bulk signature because burst is a
 * property of the device, not of the class driver's intent - every caller
 * (mass storage, the two USB NICs) gets it right without knowing it exists.
 * Full/High Speed devices have no companion, and 0 is the correct answer. */
static int ss_burst(int di, int ep_addr)
{
    static u8 cfg[512];
    int n, total, i, last_ep = -1;
    if (di < 0 || di >= g_ndevs || g_devs[di].speed != 4) return 0;
    n = uno_usb_get_config(di, cfg, (int)sizeof cfg);
    if (n < 9) return 0;
    total = cfg[2] | (cfg[3] << 8);
    if (total > n) total = n;
    for (i = 0; i + 2 <= total; ) {
        int len = cfg[i], type = cfg[i+1];
        if (len < 2) break;
        if (type == 0x05 && i + 3 <= total)            /* ENDPOINT            */
            last_ep = cfg[i+2];
        else if (type == 0x30 && i + 3 <= total &&     /* SS EP COMPANION     */
                 last_ep == (ep_addr & 0xFF))
            return cfg[i+2];                           /* bMaxBurst           */
        i += len;
    }
    return 0;
}

int uno_usb_setup_bulk(int dev, int in_addr, int out_addr, int in_mps, int out_mps)
{
    int di = dev, slot, in_dci, out_dci, maxdci, st, i, cc, in_burst, out_burst;
    u32 sdw0, sdw1;
    if (dev < 0 || dev >= g_ndevs) return -1;
    /* Ask the device about itself FIRST. ss_burst() runs control transfers, and
     * a control transfer in the middle of building the input context - after
     * the rings have been reset but before Configure Endpoint - is a transfer
     * issued against half-configured state. Do the talking, then the setup. */
    in_burst  = ss_burst(dev, in_addr);
    out_burst = ss_burst(dev, out_addr);
    slot = g_devs[di].slot;
    in_dci  = (in_addr  & 0xF) * 2 + 1;                    /* IN  endpoint DCI */
    out_dci = (out_addr & 0xF) * 2 + 0;                    /* OUT endpoint DCI */
    maxdci  = in_dci > out_dci ? in_dci : out_dci;
    /* highest write is setup_ep's off+16 = (maxdci+1)*st+16; with st=64 and the
       4096-byte context that is safe for maxdci<=31. Reject anything larger (a
       malformed endpoint address) rather than write past the context. */
    if (maxdci > 31) return -1;
    st = g_csz ? 64 : 32;
    for (i = 0; i < (int)sizeof g_inctx[di]; i++) g_inctx[di][i] = 0;
    ctx_wr(g_inctx[di], 4, 1u | (1u<<in_dci) | (1u<<out_dci));   /* add slot + both endpoints */
    sdw0 = *(volatile u32 *)(g_devctx[di] + 0);           /* copy slot ctx from device ctx */
    sdw1 = *(volatile u32 *)(g_devctx[di] + 4);
    sdw0 = (sdw0 & ~(0x1Fu<<27)) | ((u32)maxdci<<27);      /* context entries = max DCI */
    ctx_wr(g_inctx[di], st+0, sdw0);
    ctx_wr(g_inctx[di], st+4, sdw1);
    for (i = 0; i < BULK_RING_SZ; i++) {
        g_bin[di][i].param=0;  g_bin[di][i].status=0;  g_bin[di][i].control=0;
        g_bout[di][i].param=0; g_bout[di][i].status=0; g_bout[di][i].control=0;
    }
    g_bin_i[di]=0; g_bin_cyc[di]=1; g_bout_i[di]=0; g_bout_cyc[di]=1;
    xd("[xhci] bulk ep speed="); xd_i(g_devs[di].speed);
    xd(" mps="); xd_i(in_mps); xd(" burst="); xd_i(in_burst); xd_c(10);
    setup_ep(di, in_dci,  6 /*Bulk In*/,  in_mps,  in_burst,  g_bin[di]);
    setup_ep(di, out_dci, 2 /*Bulk Out*/, out_mps, out_burst, g_bout[di]);
    cc = run_command((u64)(uintptr_t)g_inctx[di], TRB_TYPE(TR_CONFIG_EP) | ((u32)slot<<24), 0);
    if (cc != CC_SUCCESS) return -1;
    g_bin_dci[di] = in_dci; g_bout_dci[di] = out_dci;
    return 0;
}

/* A bulk transfer that ends in an error must not just be reported to the
 * caller - the endpoint is halted in the controller and the ring cursor is
 * parked on the dead TRB, so without recovery the NEXT transfer on this
 * endpoint fails too, and every one after it. Recover before returning, so a
 * caller's retry has something working to retry on. */
int uno_usb_bulk_out(int dev, void *data, int len)
{
    int resid = 0, cc;
    if (dev < 0 || dev >= g_ndevs || !g_bout_dci[dev]) return -1;
    ep_push(g_bout[dev], &g_bout_i[dev], &g_bout_cyc[dev], BULK_RING_SZ,
            (u64)(uintptr_t)data, (u32)len, TRB_TYPE(TR_NORMAL) | (1u<<5)/*IOC*/);
    wr32(g_db, g_devs[dev].slot*4, g_bout_dci[dev]);
    cc = poll_xfer(&resid, 5000);        /* bulk: a slow stick can take seconds */
    if (cc == CC_SUCCESS || cc == 13) return len - resid;
    ep_recover(dev, g_bout_dci[dev], g_bout[dev], &g_bout_i[dev], &g_bout_cyc[dev],
               cc < 0);
    return -1;
}
int uno_usb_bulk_in(int dev, void *data, int len)
{
    int resid = 0, cc;
    if (dev < 0 || dev >= g_ndevs || !g_bin_dci[dev]) return -1;
    ep_push(g_bin[dev], &g_bin_i[dev], &g_bin_cyc[dev], BULK_RING_SZ,
            (u64)(uintptr_t)data, (u32)len, TRB_TYPE(TR_NORMAL) | (1u<<5)/*IOC*/ | (1u<<2)/*ISP*/);
    wr32(g_db, g_devs[dev].slot*4, g_bin_dci[dev]);
    cc = poll_xfer(&resid, 5000);        /* bulk: a slow stick can take seconds */
    if (cc == CC_SUCCESS || cc == 13) return len - resid;
    /* cc -1 = we gave up waiting (endpoint state 1 = still Running means the
     * device simply had not answered yet); anything else is a real error
     * completion. The distinction is the whole diagnosis, so print both. */
    xd("[xhci] bulk-in failed cc="); xd_i(cc); xd(" len="); xd_i(len);
    { int st2 = g_csz ? 64 : 32;
      u32 e0 = *(volatile u32 *)(g_devctx[dev] + (g_bin_dci[dev] + 1) * st2);
      xd(" epstate="); xd_i((int)(e0 & 7)); }
    xd_c(10);
    ep_recover(dev, g_bin_dci[dev], g_bin[dev], &g_bin_i[dev], &g_bin_cyc[dev],
               cc < 0);
    return -1;
}

/* Async bulk-IN for the NIC recv path: arm posts one TRB and returns at once;
 * poll is non-blocking and returns the byte count when the transfer lands
 * (0 = still outstanding). One outstanding transfer per device; the caller's
 * buffer must stay valid until poll reports completion. */
int uno_usb_bulk_in_arm(int dev, void *data, int len)
{
    if (dev < 0 || dev >= g_ndevs || !g_bin_dci[dev]) return -1;
    if (g_async_trb[dev][ASY_BIN] || g_async_sts[dev][ASY_BIN]) return 0; /* busy */
    g_async_trb[dev][ASY_BIN] = (u64)(uintptr_t)&g_bin[dev][g_bin_i[dev]];
    g_async_sts[dev][ASY_BIN] = 0;
    ep_push(g_bin[dev], &g_bin_i[dev], &g_bin_cyc[dev], BULK_RING_SZ,
            (u64)(uintptr_t)data, (u32)len, TRB_TYPE(TR_NORMAL) | (1u<<5)/*IOC*/ | (1u<<2)/*ISP*/);
    wr32(g_db, g_devs[dev].slot*4, g_bin_dci[dev]);
    g_abin_len[dev] = len;
    return 1;
}

int uno_usb_bulk_in_poll(int dev)
{
    int resid = 0, cc;
    if (dev < 0 || dev >= g_ndevs || !g_bin_dci[dev]) return -1;
    if (!g_async_trb[dev][ASY_BIN] && !g_async_sts[dev][ASY_BIN]) return -1; /* not armed */
    cc = async_poll(dev, ASY_BIN, &resid);
    if (cc == 0) return 0;                                /* still outstanding */
    if (cc != CC_SUCCESS && cc != 13) return -1;
    { int n = g_abin_len[dev] - resid; return n > 0 ? n : -1; }
}

/* post one interrupt-IN TRB into the device's HID buffer + ring the doorbell */
static void intr_post(int dev)
{
    g_async_trb[dev][ASY_INTR] = (u64)(uintptr_t)&g_bin[dev][g_bin_i[dev]];
    g_async_sts[dev][ASY_INTR] = 0;
    ep_push(g_bin[dev], &g_bin_i[dev], &g_bin_cyc[dev], BULK_RING_SZ,
            (u64)(uintptr_t)g_hidbuf[dev], (u32)g_intr_mps[dev],
            TRB_TYPE(TR_NORMAL) | (1u<<5)/*IOC*/ | (1u<<2)/*ISP short-packet ok*/);
    wr32(g_db, g_devs[dev].slot*4, g_intr_dci[dev]);
}

/* Configure a single interrupt-IN endpoint (HID). in_addr = bEndpointAddress
 * (e.g. 0x81); mps from the endpoint descriptor. Posts the first TRB. */
int uno_usb_setup_intr_in(int dev, int in_addr, int mps)
{
    int di = dev, slot, in_dci, st, i, cc;
    u32 sdw0, sdw1;
    if (dev < 0 || dev >= g_ndevs) return -1;
    if (mps <= 0 || mps > (int)sizeof g_hidbuf[0]) mps = (int)sizeof g_hidbuf[0];
    slot = g_devs[di].slot;
    in_dci = (in_addr & 0xF) * 2 + 1;                     /* IN endpoint DCI */
    if (in_dci > 31) return -1;
    st = g_csz ? 64 : 32;
    for (i = 0; i < (int)sizeof g_inctx[di]; i++) g_inctx[di][i] = 0;
    ctx_wr(g_inctx[di], 4, 1u | (1u<<in_dci));            /* add slot + the EP */
    sdw0 = *(volatile u32 *)(g_devctx[di] + 0);
    sdw1 = *(volatile u32 *)(g_devctx[di] + 4);
    sdw0 = (sdw0 & ~(0x1Fu<<27)) | ((u32)in_dci<<27);     /* context entries = max DCI */
    ctx_wr(g_inctx[di], st+0, sdw0);
    ctx_wr(g_inctx[di], st+4, sdw1);
    for (i = 0; i < BULK_RING_SZ; i++) {
        g_bin[di][i].param=0; g_bin[di][i].status=0; g_bin[di][i].control=0;
    }
    g_bin_i[di]=0; g_bin_cyc[di]=1;
    setup_ep(di, in_dci, 7 /*Interrupt In*/, mps, ss_burst(di, in_addr), g_bin[di]);
    cc = run_command((u64)(uintptr_t)g_inctx[di], TRB_TYPE(TR_CONFIG_EP) | ((u32)slot<<24), 0);
    if (cc != CC_SUCCESS) return -1;
    g_intr_dci[di] = in_dci; g_intr_mps[di] = mps;
    intr_post(di);                                        /* first outstanding TRB */
    return 0;
}

/* Non-blocking: if the outstanding interrupt-IN transfer completed, copy the
 * report into `data`, re-post, and return its byte count; 0 if none yet; -1 on
 * error. Keeps exactly one TRB outstanding, so it never desyncs the ring. */
int uno_usb_intr_in(int dev, void *data, int maxlen)
{
    int resid = 0, cc, n, i;
    if (dev < 0 || dev >= g_ndevs || !g_intr_dci[dev]) return -1;
    cc = async_poll(dev, ASY_INTR, &resid);               /* non-blocking */
    if (cc != CC_SUCCESS && cc != 13) return 0;           /* no report this frame */
    n = g_intr_mps[dev] - resid;
    if (n > maxlen) n = maxlen;
    for (i = 0; i < n; i++) ((u8 *)data)[i] = g_hidbuf[dev][i];
    intr_post(dev);                                       /* re-arm */
    return n;
}

/* one bring-up attempt: reset -> rings -> run -> port scan -> enumerate.
 * Returns 1 if the controller ran (whether or not a device enumerated), 0 if
 * reset/run failed. HCE is checked by the caller so it can retry. */
static int xhci_bringup(void)
{
    u32 hcs2 = rd32(g_cap, CAP_HCSPARAMS2);
    int i, nscratch, tries;

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
    mdelay(50);                        /* let the controller + any firmware activity settle */

    /* power + reset every root port, count the connected ones */
    g_nports_conn = 0;
    for (i = 1; i <= g_maxports; i++) {
        u32 sc = rd32(g_op, OP_PORTSC(i));
        if (!(sc & PORTSC_PP)) { wr32(g_op, OP_PORTSC(i), (sc & ~PORTSC_RW1CS) | PORTSC_PP);
                                 { int t=20000; while(t--) spin(4); } }
        sc = rd32(g_op, OP_PORTSC(i));
        if (sc & PORTSC_CCS) { reset_port(i);
            if (rd32(g_op, OP_PORTSC(i)) & (PORTSC_CCS|PORTSC_PED)) {
                g_nports_conn++;
                enumerate_port(i);           /* Enable Slot -> Address Device -> descriptor */
            } }
    }
    return 1;
}

int uno_xhci_supported(void) { return 1; }

/* Taking the controller is a one-way door while the firmware is alive.
 *
 * uno_pc64_pci_disconnect() below rips the firmware's own USB stack off this
 * xHCI - which is the stack currently carrying the USB boot volume, the
 * firmware keyboard on a laptop with no i8042, and (on a stick boot) the
 * Block IO the running system is reading its modules from. Doing that BEFORE
 * ExitBootServices is how you lose the machine you are standing on.
 *
 * So the native stack is DETACHED-mode by construction: compiled into every
 * build, inert until firmware ownership has ended. Attached-mode USB goes
 * through usbio.c (EFI_USB_IO, no ownership change) instead.
 *
 * UNO_XHCI_EAGER (implied by the older UNO_USBHID_TEST) opts a test build out,
 * so QEMU can exercise the native path attached - pair it with -DUNO_NO_DETACH,
 * never ship it. */
#if defined(UNO_USBHID_TEST) && !defined(UNO_XHCI_EAGER)
#define UNO_XHCI_EAGER 1
#endif

int uno_xhci_init(void)
{
    pci_dev d; u64 bar; u32 hcs1, hcc; int attempt;

    if (g_present) return 1;          /* idempotent: 3 callers, one bring-up */
#ifndef UNO_XHCI_EAGER
    if (!uno_pc64_detached()) return 0;
#endif
    g_err = 1;
    if (!find_xhci(&d)) return 0;
    g_dbg_disc = uno_pc64_pci_disconnect(d.bus, d.dev, d.fn);   /* take it from the firmware first */
    pci_enable_bus_master(&d);
    bar = pci_bar(&d, 0);
    if (!bar) return 0;
    g_cap = (volatile u8 *)(uintptr_t)bar;

    g_caplen   = rd32(g_cap, CAP_CAPLENGTH) & 0xFF;
    hcs1       = rd32(g_cap, CAP_HCSPARAMS1);
    hcc        = rd32(g_cap, CAP_HCCPARAMS1);
    g_maxslots = hcs1 & 0xFF;
    g_maxports = (hcs1 >> 24) & 0xFF;
    g_csz      = (hcc >> 2) & 1;               /* 1 => 64-byte contexts */
    g_op = g_cap + g_caplen;
    g_rt = g_cap + (rd32(g_cap, CAP_RTSOFF) & ~0x1Fu);
    g_db = g_cap + (rd32(g_cap, CAP_DBOFF)  & ~0x3u);
    if (g_maxslots > MAX_SLOTS_SUP) g_maxslots = MAX_SLOTS_SUP;

    /* A fresh HCRST + re-init recovers from an intermittent HC error (the
     * firmware-handoff race), so retry a few times until it comes up clean and
     * enumerates any connected device. */
    for (attempt = 0; attempt < 5; attempt++) {
        g_present = 0; g_ndevs = 0; g_nports_conn = 0;
        if (!xhci_bringup()) { mdelay(20); continue; }
        if (!(rd32(g_op, OP_USBSTS) & STS_HCE) &&
            (g_nports_conn == 0 || g_ndevs > 0)) return 1;   /* clean + enumerated */
        mdelay(20);                                          /* HCE or nothing found: retry */
    }
    return g_present;
}

int uno_xhci_dev_count(void) { return g_ndevs; }
const uno_usb_dev *uno_xhci_dev(int i)
{ return (i >= 0 && i < g_ndevs) ? &g_devs[i] : 0; }

void uno_xhci_status(int *present, int *nports, int *ndevs, unsigned *err)
{
    if (present) *present = g_present;
    if (nports)  *nports  = g_nports_conn;
    if (ndevs)   *ndevs   = g_ndevs;
    if (err)     *err     = g_err;
}

void uno_xhci_diag(int *slot, int *addr_cc, int *desc, int *speed)
{
    if (slot)    *slot    = g_dbg_slot;
    if (addr_cc) *addr_cc = g_dbg_addr;
    if (desc)    *desc    = g_dbg_desc;
    if (speed)   *speed   = g_dbg_speed;
}
void uno_xhci_diag2(unsigned *sts, unsigned *ev0, int *disc)
{ if (sts) *sts = g_dbg_sts; if (ev0) *ev0 = g_dbg_ev0; if (disc) *disc = g_dbg_disc; }

#endif /* UNO_XHCI */
