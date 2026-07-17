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

#else  /* ===================== UNO_XHCI enabled ========================= */

#include "pc64_pci.h"
#include <stdint.h>

/* detach the firmware's USB driver from this controller first (uefi_main) */
int uno_pc64_pci_disconnect(int bus, int dev, int fn);

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
#define MAX_DEV 8
#define EP0_RING_SZ 16
static u8    g_inctx[MAX_DEV][2048]  __attribute__((aligned(64)));
static u8    g_devctx[MAX_DEV][2048] __attribute__((aligned(64)));
static trb_t g_ep0[MAX_DEV][EP0_RING_SZ] __attribute__((aligned(64)));
static int   g_ep0_i[MAX_DEV], g_ep0_cyc[MAX_DEV];
static u8    g_descbuf[256] __attribute__((aligned(64)));
static uno_usb_dev g_devs[MAX_DEV];

/* bulk transfer rings (one in + one out per device, set up by Configure EP) */
#define BULK_RING_SZ 32
static trb_t g_bin[MAX_DEV][BULK_RING_SZ]  __attribute__((aligned(64)));
static trb_t g_bout[MAX_DEV][BULK_RING_SZ] __attribute__((aligned(64)));
static int   g_bin_i[MAX_DEV], g_bin_cyc[MAX_DEV], g_bin_dci[MAX_DEV];
static int   g_bout_i[MAX_DEV], g_bout_cyc[MAX_DEV], g_bout_dci[MAX_DEV];

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
    for (;;) {
        if (!poll_event(&ev, 2000000)) return 0;
        /* only OUR command's completion (match the TRB pointer) - skip stale or
         * other events so a leftover completion can't be mistaken for this one */
        if (TRB_GET_TYPE(ev.control) == TR_CMD_COMPLETE && ev.param == mytrb) {
            if (slot_out) *slot_out = (ev.control >> 24) & 0xFF;
            return (ev.status >> 24) & 0xFF;
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
#define xd_i(v) ((void)0)
#define xd_h(v) ((void)0)
#endif

static void reset_port(int p)
{
    u32 sc = rd32(g_op, OP_PORTSC(p));
    wr32(g_op, OP_PORTSC(p), (sc & ~PORTSC_RW1CS) | PORTSC_PR | PORTSC_PP);
    { int t = 1000000; while (t-- > 0) { sc = rd32(g_op, OP_PORTSC(p));
        if (sc & PORTSC_PRC) break; spin(20); } }
    wr32(g_op, OP_PORTSC(p), (rd32(g_op, OP_PORTSC(p)) & ~PORTSC_RW1CS) | PORTSC_PRC); /* ack */
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

/* wait for the next transfer event; returns the completion code (1/13 = ok),
 * fills the residual (untransferred bytes of the last TRB). -1 on timeout. */
static int poll_xfer(int *residual, int budget)
{
    trb_t ev;
    for (;;) {
        if (!poll_event(&ev, budget)) return -1;
        if (TRB_GET_TYPE(ev.control) != TR_XFER_EVENT) continue;
        if (residual) *residual = ev.status & 0xFFFFFF;
        return (ev.status >> 24) & 0xFF;
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
    cc = poll_xfer(&resid, 3000000);
    g_dbg_cc = cc; g_dbg_resid = resid;
    return (cc == CC_SUCCESS || cc == 13) ? (len - resid) : -1;
}

static int get_device_descriptor(int di, int slot, u8 *out, int len)
{ (void)slot; return control_xfer(di, 0x80, 6, 0x0100, 0, out, len) >= 0; }

static void enumerate_port(int port)
{
    int slot = 0, cc, di, speed, mps, st, i;
    if (g_ndevs >= MAX_DEV) return;
    di = g_ndevs;
    speed = PORTSC_SPEED(rd32(g_op, OP_PORTSC(port)));
    g_dbg_speed = speed;

    cc = run_command(0, TRB_TYPE(TR_ENABLE_SLOT), &slot);
    g_dbg_slot = (cc == CC_SUCCESS) ? slot : -(int)cc;
    g_dbg_sts  = rd32(g_op, OP_USBSTS);
    g_dbg_ev0  = g_evt[0].control;
    if (cc != CC_SUCCESS || slot == 0 || slot > g_maxslots) return;

    for (i = 0; i < EP0_RING_SZ; i++) { g_ep0[di][i].param=0; g_ep0[di][i].status=0; g_ep0[di][i].control=0; }
    g_ep0_i[di] = 0; g_ep0_cyc[di] = 1;
    for (i = 0; i < 2048; i++) { g_inctx[di][i]=0; g_devctx[di][i]=0; }

    st = g_csz ? 64 : 32; mps = mps_for_speed(speed);
    ctx_wr(g_inctx[di], 4, 0x3);                                 /* add flags A0|A1 */
    ctx_wr(g_inctx[di], st,    (1u<<27) | ((u32)speed<<20));     /* slot: 1 ctx entry, speed */
    ctx_wr(g_inctx[di], st+4,  ((u32)port<<16));                 /* slot: root hub port */
    ctx_wr(g_inctx[di], 2*st+4, ((u32)mps<<16) | (4u<<3) | (3u<<1)); /* EP0: Control, CErr=3, MPS */
    { u64 tr = (u64)(uintptr_t)g_ep0[di] | 1u;                   /* TR dequeue ptr + DCS */
      ctx_wr(g_inctx[di], 2*st+8,  (u32)tr);
      ctx_wr(g_inctx[di], 2*st+12, (u32)(tr>>32)); }
    ctx_wr(g_inctx[di], 2*st+16, 8);                             /* avg TRB length */

    g_dcbaa[slot] = (u64)(uintptr_t)g_devctx[di];               /* output ctx, before Address Device */
    cc = run_command((u64)(uintptr_t)g_inctx[di], TRB_TYPE(TR_ADDRESS_DEV) | ((u32)slot<<24), 0);
    g_dbg_addr = (int)cc;
    if (cc != CC_SUCCESS) return;
    mdelay(2);                                                  /* let the address settle */

    /* publish slot/port/speed BEFORE the descriptor fetch: control_xfer reads
     * g_devs[di].slot to ring the right doorbell (it's a full transfer API now,
     * not the old slot-parameter helper). */
    g_devs[di].slot = slot; g_devs[di].port = port; g_devs[di].speed = speed;

    for (i = 0; i < 18; i++) g_descbuf[i] = 0;
    g_dbg_desc = get_device_descriptor(di, slot, g_descbuf, 18) ? g_descbuf[1] : -1;
    g_dbg_sts = rd32(g_op, OP_USBSTS);           /* post-transfer status (HCE shows here) */
    xd("[xhci] port="); xd_i(port); xd(" slot="); xd_i(slot);
    xd(" addr_cc="); xd_i((int)cc); xd(" desc_cc="); xd_i(g_dbg_cc);
    xd(" resid="); xd_i(g_dbg_resid); xd(" bDescType="); xd_i(g_dbg_desc);
    xd(" vid="); xd_h((unsigned)(g_descbuf[8]|(g_descbuf[9]<<8)));
    xd(" pid="); xd_h((unsigned)(g_descbuf[10]|(g_descbuf[11]<<8))); xd("\n");
    if (g_dbg_desc != 1) return;                                /* must be a device descriptor */
    g_devs[di].vendor  = (u16)(g_descbuf[8]  | (g_descbuf[9]  << 8));
    g_devs[di].product = (u16)(g_descbuf[10] | (g_descbuf[11] << 8));
    g_devs[di].dev_class    = g_descbuf[4];
    g_devs[di].dev_subclass = g_descbuf[5];
    g_devs[di].dev_proto    = g_descbuf[6];
    g_ndevs++;
}

/* ---- USB transfer API for class drivers (dev = index in the device list) -- */
static void setup_ep(int di, int dci, int eptype, int mps, trb_t *ring)
{
    int st = g_csz ? 64 : 32, off = (dci + 1) * st;
    u64 tr = (u64)(uintptr_t)ring | 1u;                    /* dequeue ptr + DCS */
    ctx_wr(g_inctx[di], off+4, ((u32)mps<<16) | ((u32)eptype<<3) | (3u<<1)); /* MPS, type, CErr=3 */
    ctx_wr(g_inctx[di], off+8,  (u32)tr);
    ctx_wr(g_inctx[di], off+12, (u32)(tr>>32));
    ctx_wr(g_inctx[di], off+16, 1024);                     /* average TRB length */
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

int uno_usb_setup_bulk(int dev, int in_addr, int out_addr, int in_mps, int out_mps)
{
    int di = dev, slot, in_dci, out_dci, maxdci, st, i, cc;
    u32 sdw0, sdw1;
    if (dev < 0 || dev >= g_ndevs) return -1;
    slot = g_devs[di].slot;
    in_dci  = (in_addr  & 0xF) * 2 + 1;                    /* IN  endpoint DCI */
    out_dci = (out_addr & 0xF) * 2 + 0;                    /* OUT endpoint DCI */
    maxdci  = in_dci > out_dci ? in_dci : out_dci;
    st = g_csz ? 64 : 32;
    for (i = 0; i < 2048; i++) g_inctx[di][i] = 0;
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
    setup_ep(di, in_dci,  6 /*Bulk In*/,  in_mps,  g_bin[di]);
    setup_ep(di, out_dci, 2 /*Bulk Out*/, out_mps, g_bout[di]);
    cc = run_command((u64)(uintptr_t)g_inctx[di], TRB_TYPE(TR_CONFIG_EP) | ((u32)slot<<24), 0);
    if (cc != CC_SUCCESS) return -1;
    g_bin_dci[di] = in_dci; g_bout_dci[di] = out_dci;
    return 0;
}

int uno_usb_bulk_out(int dev, void *data, int len)
{
    int resid = 0, cc;
    if (dev < 0 || dev >= g_ndevs || !g_bout_dci[dev]) return -1;
    ep_push(g_bout[dev], &g_bout_i[dev], &g_bout_cyc[dev], BULK_RING_SZ,
            (u64)(uintptr_t)data, (u32)len, TRB_TYPE(TR_NORMAL) | (1u<<5)/*IOC*/);
    wr32(g_db, g_devs[dev].slot*4, g_bout_dci[dev]);
    cc = poll_xfer(&resid, 5000000);
    return (cc == CC_SUCCESS || cc == 13) ? (len - resid) : -1;
}
int uno_usb_bulk_in(int dev, void *data, int len)
{
    int resid = 0, cc;
    if (dev < 0 || dev >= g_ndevs || !g_bin_dci[dev]) return -1;
    ep_push(g_bin[dev], &g_bin_i[dev], &g_bin_cyc[dev], BULK_RING_SZ,
            (u64)(uintptr_t)data, (u32)len, TRB_TYPE(TR_NORMAL) | (1u<<5)/*IOC*/ | (1u<<2)/*ISP*/);
    wr32(g_db, g_devs[dev].slot*4, g_bin_dci[dev]);
    cc = poll_xfer(&resid, 5000000);
    return (cc == CC_SUCCESS || cc == 13) ? (len - resid) : -1;
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

int uno_xhci_init(void)
{
    pci_dev d; u64 bar; u32 hcs1, hcc; int attempt;

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
