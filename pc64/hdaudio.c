/* ===========================================================================
 * UnoDOS/pc64 - Intel HD Audio (Azalia) driver.
 *
 * Same shape as the port's other silicon drivers (ahci/xhci/e1000): find the
 * controller by PCI class (04/03), map BAR0, and run it with static DMA
 * buffers, polled - no interrupts, no firmware services, no allocation.
 *
 * Scope: playback only. One output stream descriptor loops a 48 kHz s16
 * stereo ring (CBL = ring, LVI wraps) so the hardware never stops; the PCM
 * layer keeps writing ahead of LPIB. Codec bring-up walks the first Audio
 * Function Group for an output pin (speaker > line-out > headphone), follows
 * its connection list down to a DAC, and unmutes/powers that path.
 *
 * Verbs go over CORB/RIRB (the standard path); if the codec never answers
 * there the driver falls back to the Immediate Command registers - the same
 * two-tier approach Linux uses (single_cmd mode).
 *
 * Bounds: every wait is an iteration-bounded spin, so a dead controller or
 * codec fails probe instead of hanging boot.
 * ======================================================================== */
#include "pc64_pci.h"
#include "hdaudio.h"
#include <stdint.h>

/* ---- controller registers (offsets into BAR0) ----------------------------- */
#define HDA_GCAP     0x00
#define HDA_GCTL     0x08
#define HDA_STATESTS 0x0E
#define HDA_CORBLBASE 0x40
#define HDA_CORBUBASE 0x44
#define HDA_CORBWP   0x48
#define HDA_CORBRP   0x4A
#define HDA_CORBCTL  0x4C
#define HDA_CORBSIZE 0x4E
#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_RIRBWP   0x58
#define HDA_RINTCNT  0x5A
#define HDA_RIRBCTL  0x5C
#define HDA_RIRBSIZE 0x5E
#define HDA_ICOI     0x60              /* immediate command out               */
#define HDA_ICII     0x64              /* immediate response in               */
#define HDA_ICIS     0x68              /* bit0 = busy (ICB), bit1 = valid     */
#define HDA_DPLBASE  0x70              /* DMA position buffer (bit0 = enable) */
#define HDA_DPUBASE  0x74

/* stream descriptor block (0x80 + n*0x20); n counts input streams first */
#define SD_CTL0   0x00                 /* bit0 SRST, bit1 RUN                 */
#define SD_CTL2   0x02                 /* bits 4-7 = stream number            */
#define SD_STS    0x03
#define SD_LPIB   0x04
#define SD_CBL    0x08
#define SD_LVI    0x0C
#define SD_FMT    0x12
#define SD_BDPL   0x18
#define SD_BDPU   0x1C

#define SPIN_MAX  4000000u             /* bounded waits: tens of ms on any CPU */

/* 48 kHz, 16-bit, 2 channels: BASE=48k MULT=1 DIV=1 BITS=01 CHAN=1 */
#define HDA_FMT_48_S16_2 0x0011

#define RING_FRAMES 16384u             /* 64 KB = ~341 ms at 48 kHz stereo s16 */

/* ---- static DMA structures (identity-mapped, survive the M3 detach) ------- */
static uint32_t gCorb[256] __attribute__((aligned(1024)));
static uint64_t gRirb[256] __attribute__((aligned(2048)));
static struct { uint64_t addr; uint32_t len, flags; }
                gBdl[2]    __attribute__((aligned(128)));
static short    gRing[RING_FRAMES * 2] __attribute__((aligned(128)));
/* DMA Position Buffer: the controller writes each stream's link position into
   an 8-byte entry (DWORD position + DWORD reserved), entry n = SD n.  Sized
   for the architectural maximum of 30 streams. */
static volatile uint32_t gPosBuf[64] __attribute__((aligned(128)));

static volatile uint8_t *gM;           /* BAR0 */
static volatile uint8_t *gSd;          /* our output stream descriptor        */
static unsigned gSdIdx;                /* its SD index (= ISS count: first out)*/
static unsigned gCorbN, gRirbN;        /* ring sizes in entries               */
static unsigned gRirbRp;               /* last consumed RIRB index            */
static int      gUseImm;               /* CORB dead -> immediate commands     */
static unsigned gCad;                  /* codec address                       */
static int      gUp;

static uint8_t  r8 (unsigned o) { return gM[o]; }
static uint16_t r16(unsigned o) { return *(volatile uint16_t *)(gM + o); }
static uint32_t r32(unsigned o) { return *(volatile uint32_t *)(gM + o); }
static void w8 (unsigned o, uint8_t  v) { gM[o] = v; }
static void w16(unsigned o, uint16_t v) { *(volatile uint16_t *)(gM + o) = v; }
static void w32(unsigned o, uint32_t v) { *(volatile uint32_t *)(gM + o) = v; }

/* a short real-time settle: MMIO reads are slow, so this is ~ms scale */
static void settle(unsigned n) { while (n--) (void)r32(HDA_GCTL); }

/* ---- codec verbs ---------------------------------------------------------- */
/* 12-bit verb + 8-bit payload */
#define V(nid, verb, pay)   ((gCad << 28) | ((uint32_t)(nid) << 20) | \
                             ((uint32_t)(verb) << 8) | (uint32_t)(pay))
/* 4-bit verb + 16-bit payload (SET_FORMAT 0x2, SET_AMP 0x3) */
#define V16(nid, verb, pay) ((gCad << 28) | ((uint32_t)(nid) << 20) | \
                             ((uint32_t)(verb) << 16) | (uint32_t)(pay))

static int cmd_imm(uint32_t cmd, uint32_t *resp)
{
    unsigned i;
    w16(HDA_ICIS, 0x2);                          /* clear a stale valid bit    */
    w32(HDA_ICOI, cmd);
    w16(HDA_ICIS, 0x1);                          /* go                         */
    for (i = 0; i < SPIN_MAX; i++)
        if (r16(HDA_ICIS) & 0x2) { *resp = r32(HDA_ICII); return 1; }
    return 0;
}

static int cmd_corb(uint32_t cmd, uint32_t *resp)
{
    unsigned i, wp = (r16(HDA_CORBWP) + 1) % gCorbN;
    gCorb[wp] = cmd;
    __asm__ volatile ("" ::: "memory");
    w16(HDA_CORBWP, (uint16_t)wp);
    for (i = 0; i < SPIN_MAX; i++)
        if ((r16(HDA_RIRBWP) % gRirbN) != gRirbRp) {
            gRirbRp = (gRirbRp + 1) % gRirbN;
            *resp = (uint32_t)*(volatile uint64_t *)&gRirb[gRirbRp];
            return 1;
        }
    return 0;
}

static uint32_t verb(uint32_t cmd)
{
    uint32_t resp = 0;
    if (gUseImm) { cmd_imm(cmd, &resp); return resp; }
    if (cmd_corb(cmd, &resp)) return resp;
    gUseImm = 1;                                 /* CORB dead: fall back       */
    cmd_imm(cmd, &resp);
    return resp;
}

static uint32_t param(unsigned nid, unsigned p) { return verb(V(nid, 0xF00, p)); }

/* ---- codec path: pin -> (mixer/selector)* -> DAC -------------------------- */
#define MAXPATH 5
static unsigned gPath[MAXPATH], gPathSel[MAXPATH];  /* nodes + chosen conn idx */
static unsigned gPathN;
static unsigned gAfg, gAfgAmpOut;

static unsigned conn_list(unsigned nid, unsigned idx)
{
    /* connection list entries come back packed 4 (short form) or 2 (long
       form) per response; mask any range bit down to the plain node id */
    unsigned len = param(nid, 0x0E), lform = len & 0x80;
    uint32_t r;
    if (idx >= (len & 0x7F)) return 0;
    if (lform) {
        r = verb(V(nid, 0xF02, (idx & ~1u)));
        return (r >> ((idx & 1) * 16)) & 0x7FFF;
    }
    r = verb(V(nid, 0xF02, (idx & ~3u)));
    return (r >> ((idx & 3) * 8)) & 0x7F;
}

static int find_dac(unsigned nid, int depth)
{
    unsigned caps = param(nid, 0x09), type = (caps >> 20) & 0xF, n, i;
    if (gPathN >= MAXPATH) return 0;
    gPath[gPathN] = nid; gPathSel[gPathN] = 0; gPathN++;
    if (type == 0) return 1;                     /* Audio Output = the DAC     */
    if (depth <= 0 || (type != 2 && type != 3 && type != 4)) { gPathN--; return 0; }
    n = param(nid, 0x0E) & 0x7F;
    for (i = 0; i < n; i++) {
        unsigned peer = conn_list(nid, i);
        if (!peer) continue;
        gPathSel[gPathN - 1] = i;
        if (find_dac(peer, depth - 1)) return 1;
    }
    gPathN--;
    return 0;
}

static void amp_unmute(unsigned nid, int is_mixer, unsigned in_idx)
{
    unsigned caps = param(nid, 0x12);            /* output amp caps            */
    unsigned gain;
    if (!caps) caps = gAfgAmpOut;
    gain = caps & 0x7F;                          /* offset field = 0 dB step   */
    verb(V16(nid, 0x3, 0xB000 | gain));          /* out amp, L+R, unmute       */
    if (is_mixer) {
        unsigned icaps = param(nid, 0x0D);
        unsigned igain = icaps & 0x7F;
        verb(V16(nid, 0x3, 0x7000 | (in_idx << 8) | igain)); /* in amp L+R     */
    }
}

static int codec_setup(void)
{
    uint32_t r; unsigned fg0, fgn, fg, w0, wn, i;
    unsigned pin = 0, pin_score = 0, pin_hp = 0;

    r = param(0, 0x04);                          /* root: function groups      */
    fg0 = (r >> 16) & 0xFF; fgn = r & 0xFF;
    gAfg = 0;
    for (fg = fg0; fg < fg0 + fgn; fg++)
        if ((param(fg, 0x05) & 0x7F) == 0x01) { gAfg = fg; break; }
    if (!gAfg) return 0;

    verb(V(gAfg, 0x705, 0x00));                  /* AFG to D0                  */
    gAfgAmpOut = param(gAfg, 0x12);

    r = param(gAfg, 0x04);                       /* AFG: widget nodes          */
    w0 = (r >> 16) & 0xFF; wn = r & 0xFF;

    /* pick the best output pin: speaker > line-out > headphone > any out pin */
    for (i = w0; i < w0 + wn; i++) {
        unsigned caps = param(i, 0x09), type = (caps >> 20) & 0xF;
        unsigned cfg, dev, conn, pcap, score;
        if (type != 4) continue;                 /* pin complex                */
        pcap = param(i, 0x0C);
        if (!(pcap & 0x10)) continue;            /* not output-capable         */
        cfg  = verb(V(i, 0xF1C, 0));
        conn = (cfg >> 30) & 3;
        if (conn == 1) continue;                 /* no physical connection     */
        dev = (cfg >> 20) & 0xF;
        score = (dev == 0x1) ? 4 : (dev == 0x0) ? 3 : (dev == 0x2) ? 2 : 1;
        if (score > pin_score) { pin_score = score; pin = i; pin_hp = (dev == 0x2); }
    }
    if (!pin) return 0;

    gPathN = 0;
    if (!find_dac(pin, MAXPATH - 1)) return 0;

    /* power + unmute + route the path (gPath[0]=pin ... gPath[N-1]=DAC) */
    for (i = 0; i < gPathN; i++) {
        unsigned nid = gPath[i];
        unsigned caps = param(nid, 0x09), type = (caps >> 20) & 0xF;
        unsigned nconn = param(nid, 0x0E) & 0x7F;
        verb(V(nid, 0x705, 0x00));               /* widget to D0               */
        amp_unmute(nid, type == 2, gPathSel[i]);
        if (type != 2 && nconn > 1)              /* pins/selectors route by    */
            verb(V(nid, 0x701, gPathSel[i]));    /* connection select          */
    }
    verb(V(pin, 0x707, 0x40 | (pin_hp ? 0x80 : 0)));   /* pin: output enable   */
    if (param(pin, 0x0C) & 0x10000)
        verb(V(pin, 0x70C, 0x02));               /* EAPD on (ext speaker amp)  */

    {   unsigned dac = gPath[gPathN - 1];
        verb(V16(dac, 0x2, HDA_FMT_48_S16_2));   /* converter format           */
        verb(V(dac, 0x706, (1 << 4) | 0));       /* stream 1, channel 0        */
    }
    return 1;
}

/* ---- controller bring-up -------------------------------------------------- */
static int rings_init(void)
{
    unsigned i, cap;

    w8(HDA_CORBCTL, 0); w8(HDA_RIRBCTL, 0);      /* stop any firmware DMA      */
    for (i = 0; i < SPIN_MAX && (r8(HDA_CORBCTL) & 0x2); i++) { }

    cap = r8(HDA_CORBSIZE) >> 4;
    gCorbN = (cap & 4) ? 256 : (cap & 2) ? 16 : 2;
    if (!(cap & 7)) gCorbN = 256;                /* no cap bits: assume 256    */
    w8(HDA_CORBSIZE, (gCorbN == 256) ? 2 : (gCorbN == 16) ? 1 : 0);
    cap = r8(HDA_RIRBSIZE) >> 4;
    gRirbN = (cap & 4) ? 256 : (cap & 2) ? 16 : 2;
    if (!(cap & 7)) gRirbN = 256;
    w8(HDA_RIRBSIZE, (gRirbN == 256) ? 2 : (gRirbN == 16) ? 1 : 0);

    w32(HDA_CORBLBASE, (uint32_t)(uintptr_t)gCorb);
    w32(HDA_CORBUBASE, (uint32_t)((uint64_t)(uintptr_t)gCorb >> 32));
    w32(HDA_RIRBLBASE, (uint32_t)(uintptr_t)gRirb);
    w32(HDA_RIRBUBASE, (uint32_t)((uint64_t)(uintptr_t)gRirb >> 32));

    /* CORB read-pointer reset handshake (some hw never latches: stay bounded) */
    w16(HDA_CORBRP, 0x8000);
    for (i = 0; i < 100000 && !(r16(HDA_CORBRP) & 0x8000); i++) { }
    w16(HDA_CORBRP, 0);
    for (i = 0; i < 100000 && (r16(HDA_CORBRP) & 0x8000); i++) { }
    w16(HDA_CORBWP, 0);

    w16(HDA_RIRBWP, 0x8000);                     /* reset RIRB write pointer   */
    w16(HDA_RINTCNT, 1);
    gRirbRp = 0;

    w8(HDA_CORBCTL, 0x2);                        /* DMA run (no interrupts)    */
    w8(HDA_RIRBCTL, 0x2);
    return 1;
}

static int stream_start(void)
{
    unsigned iss = (r16(HDA_GCAP) >> 8) & 0xF, i;
    gSdIdx = iss;                                /* first OUTPUT descriptor    */
    gSd = gM + 0x80 + iss * 0x20;

#define sr8(o)     (*(volatile uint8_t  *)(gSd + (o)))
#define sr32(o)    (*(volatile uint32_t *)(gSd + (o)))
#define sw8(o, v)  (*(volatile uint8_t  *)(gSd + (o)) = (v))
#define sw16(o, v) (*(volatile uint16_t *)(gSd + (o)) = (v))
#define sw32(o, v) (*(volatile uint32_t *)(gSd + (o)) = (v))

    sw8(SD_CTL0, 0);                             /* stop                       */
    for (i = 0; i < SPIN_MAX && (sr8(SD_CTL0) & 0x2); i++) { }
    sw8(SD_CTL0, 0x1);                           /* stream reset               */
    for (i = 0; i < 100000 && !(sr8(SD_CTL0) & 0x1); i++) { }
    sw8(SD_CTL0, 0);
    for (i = 0; i < 100000 && (sr8(SD_CTL0) & 0x1); i++) { }

    gBdl[0].addr = (uint64_t)(uintptr_t)gRing;
    gBdl[0].len  = RING_FRAMES * 2;              /* half the ring, in bytes    */
    gBdl[0].flags = 0;
    gBdl[1].addr = (uint64_t)(uintptr_t)gRing + RING_FRAMES * 2;
    gBdl[1].len  = RING_FRAMES * 2;
    gBdl[1].flags = 0;

    /* point the controller's position buffer at ours (128-byte aligned, so
       OR-ing the enable bit into the low base is safe); the DMA engine then
       snapshots every stream's read position into memory, which tracks what
       has actually been fetched better than a racy LPIB read on real codecs */
    w32(HDA_DPUBASE, (uint32_t)((uint64_t)(uintptr_t)gPosBuf >> 32));
    w32(HDA_DPLBASE, (uint32_t)(uintptr_t)gPosBuf | 0x1);

    sw32(SD_BDPL, (uint32_t)(uintptr_t)gBdl);
    sw32(SD_BDPU, (uint32_t)((uint64_t)(uintptr_t)gBdl >> 32));
    sw32(SD_CBL,  RING_FRAMES * 4);              /* cyclic buffer = whole ring */
    sw16(SD_LVI,  1);                            /* 2 BDL entries              */
    sw16(SD_FMT,  HDA_FMT_48_S16_2);
    sw8 (SD_CTL2, 1 << 4);                       /* tag as stream 1            */
    sw8 (SD_CTL0, 0x2);                          /* RUN                        */
    return 1;
}

int uno_hda_init(void)
{
    pci_dev d; unsigned i; uint16_t sts;
    uint64_t bar;

    if (!pci_find_class(0x04, 0x03, &d)) return 0;
    pci_enable_bus_master(&d);
    /* route audio DMA to traffic class 0 (Intel TCSEL; harmless elsewhere) */
    pci_cfg_write32(&d, 0x44, pci_cfg_read32(&d, 0x44) & ~0x07u);
    bar = pci_bar(&d, 0);
    if (!bar) return 0;
    gM = (volatile uint8_t *)(uintptr_t)bar;     /* keep 64-bit BARs intact    */

    w32(HDA_GCTL, 0);                            /* CRST: pull into reset      */
    for (i = 0; i < SPIN_MAX && (r32(HDA_GCTL) & 1); i++) { }
    w32(HDA_GCTL, 1);                            /* release reset              */
    for (i = 0; i < SPIN_MAX && !(r32(HDA_GCTL) & 1); i++) { }
    if (!(r32(HDA_GCTL) & 1)) return 0;
    settle(60000);                               /* codecs need ~1 ms to check in */

    sts = r16(HDA_STATESTS);
    for (gCad = 0; gCad < 15; gCad++) if (sts & (1u << gCad)) break;
    if (gCad == 15) return 0;                    /* no codec present           */

    rings_init();
    if (!codec_setup()) return 0;
    stream_start();
    gUp = 1;
    return 1;
}

short *uno_hda_ring(unsigned *frames) { *frames = RING_FRAMES; return gRing; }

unsigned uno_hda_pos(void)
{
    uint32_t p;
    if (!gUp) return 0;
    p = gPosBuf[gSdIdx * 2];                     /* our 8-byte entry's DWORD   */
    if (!p)                                      /* posbuf dead on this hw     */
        p = *(volatile uint32_t *)(gSd + SD_LPIB);
    return (p / 4) % RING_FRAMES;
}
