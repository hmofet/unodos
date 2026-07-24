/* Host-side gate for unodevices phase 1 (pc64/uno_devmgr.c).
 *
 * uno_devmgr.c consumes exactly four pc64_pci.c entry points and one GOP
 * accessor, so it can be linked against a SYNTHETIC config space and exercised
 * natively - no QEMU, no UEFI, seconds per run.  That matters because the
 * things most likely to be wrong here (bridge recursion, the multi-function
 * bit, 64-bit BAR pairing, the exact listing format the URC `devices` verb
 * forwards, and BAR sizing restoring every register it touched) are all
 * decidable from a fake bus.  QEMU then confirms the real machine.
 *
 * Build + run:  cc -O1 -Wall -Wextra -I.. -o /tmp/devmgr_test \
 *                   tools/devmgr_test.c uno_devmgr.c && /tmp/devmgr_test
 */
#include <stdio.h>
#include <string.h>
#include "pc64_pci.h"
#include "uno_devmgr.h"

/* --- synthetic PCI config space ------------------------------------------- */

typedef struct {
    int bus, dev, fn;
    unsigned cfg[64];          /* 256 bytes of config space, dword indexed */
    unsigned barmask[6];       /* what a 0xFFFFFFFF write reads back as    */
    unsigned barsaved[6];      /* shadow: what the test wrote last         */
} fake_dev;

#define MAXFAKE 16
static fake_dev g_fake[MAXFAKE];
static int      g_nfake;
static int      g_sizing;      /* set while a 0xFFFFFFFF probe is in flight */

static fake_dev *lookup(int bus, int dev, int fn)
{
    int i;
    for (i = 0; i < g_nfake; i++)
        if (g_fake[i].bus == bus && g_fake[i].dev == dev && g_fake[i].fn == fn)
            return &g_fake[i];
    return 0;
}

static fake_dev *mkdev(int bus, int dev, int fn, unsigned short ven,
                       unsigned short devid, unsigned char cls,
                       unsigned char sub, unsigned char progif,
                       unsigned char hdr)
{
    fake_dev *f = &g_fake[g_nfake++];
    memset(f, 0, sizeof *f);
    f->bus = bus; f->dev = dev; f->fn = fn;
    f->cfg[0x00 / 4] = ((unsigned)devid << 16) | ven;
    f->cfg[0x04 / 4] = 0x00100007u;                 /* cmd=7, status: cap list */
    f->cfg[0x08 / 4] = ((unsigned)cls << 24) | ((unsigned)sub << 16) |
                       ((unsigned)progif << 8) | 0x01u;   /* revision 0x01 */
    f->cfg[0x0C / 4] = (unsigned)hdr << 16;
    f->cfg[0x34 / 4] = 0;                            /* no caps unless added */
    return f;
}

/* append a capability record at `off` (id, next=0 until the following call) */
static void addcap(fake_dev *f, unsigned char id, unsigned char off)
{
    unsigned char head = (unsigned char)(f->cfg[0x34 / 4] & 0xFF);
    f->cfg[off / 4] = id | ((unsigned)head << 8);     /* chain to the old head */
    f->cfg[0x34 / 4] = off;
}

unsigned int pci_cfg_read32(const pci_dev *d, int off)
{
    fake_dev *f = lookup(d->bus, d->dev, d->fn);
    if (!f) return 0xFFFFFFFFu;
    if (g_sizing && off >= 0x10 && off < 0x28) {
        int i = (off - 0x10) / 4;
        if (f->barsaved[i] == 0xFFFFFFFFu) return f->barmask[i];
    }
    return f->cfg[(off & 0xFC) / 4];
}
unsigned short pci_cfg_read16(const pci_dev *d, int off)
{ return (unsigned short)(pci_cfg_read32(d, off) >> ((off & 2) * 8)); }

void pci_cfg_write32(const pci_dev *d, int off, unsigned int v)
{
    fake_dev *f = lookup(d->bus, d->dev, d->fn);
    if (!f) return;
    if (off >= 0x10 && off < 0x28) {
        int i = (off - 0x10) / 4;
        f->barsaved[i] = v;
        g_sizing = 1;
        if (v != 0xFFFFFFFFu) f->cfg[off / 4] = v;
        return;
    }
    if ((off & 0xFC) == 0x04) {
        /* command (low half) is plain read/write; STATUS (high half) is
         * write-1-to-clear, so writing zeros there must leave it alone.  A
         * naive dword model here would let a BAR probe appear to "restore"
         * the register while silently wiping the status bits - the exact bug
         * this file exists to catch. */
        unsigned st = f->cfg[1] & 0xFFFF0000u;
        f->cfg[1] = (st & ~(v & 0xFFFF0000u)) | (v & 0xFFFFu);
        return;
    }
    f->cfg[(off & 0xFC) / 4] = v;
}
void pci_cfg_write16(const pci_dev *d, int off, unsigned short v)
{
    unsigned cur = pci_cfg_read32(d, off);
    int sh = (off & 2) * 8;
    pci_cfg_write32(d, off, (cur & ~(0xFFFFu << sh)) | ((unsigned)v << sh));
}
int pci_find(unsigned short v, unsigned short d, pci_dev *o)
{ (void)v; (void)d; (void)o; return 0; }
int pci_find_class(unsigned char c, unsigned char s, pci_dev *o)
{ (void)c; (void)s; (void)o; return 0; }
void pci_enable_bus_master(const pci_dev *d) { (void)d; }
unsigned long long pci_bar(const pci_dev *d, int n) { (void)d; (void)n; return 0; }

#define FB_BASE 0xfd000000ull
unsigned long long uno_pc64_fb_phys(void) { return FB_BASE; }

/* --- the machine under test ------------------------------------------------
 * bus 0: host bridge, a multi-function ISA bridge + IDE, the VGA holding the
 * live framebuffer, an e1000, and a PCI-PCI bridge to bus 2.
 * bus 2: an r8169 with a 64-bit BAR pair and MSI-X - the "card behind a
 * bridge" case a flat scan records but cannot place in the topology. */
static void build_machine(void)
{
    fake_dev *f;
    g_nfake = 0;

    mkdev(0, 0, 0, 0x8086, 0x1237, 0x06, 0x00, 0x00, 0x00);      /* host bridge */

    f = mkdev(0, 1, 0, 0x8086, 0x7000, 0x06, 0x01, 0x00, 0x00);  /* ISA bridge  */
    f->cfg[0x0C / 4] |= 0x00800000u;                             /* multi-fn    */
    mkdev(0, 1, 1, 0x8086, 0x7010, 0x01, 0x01, 0x80, 0x00);      /* IDE (fn 1)  */

    f = mkdev(0, 2, 0, 0x1234, 0x1111, 0x03, 0x00, 0x00, 0x00);  /* VGA         */
    f->cfg[0x10 / 4] = (unsigned)FB_BASE | 0x8;                  /* prefetchable */
    f->barmask[0] = 0xff000000u;                                 /* 16 MB       */

    f = mkdev(0, 3, 0, 0x8086, 0x100e, 0x02, 0x00, 0x00, 0x00);  /* e1000       */
    f->cfg[0x10 / 4] = 0xfebc0000u;                              /* mem BAR0    */
    f->barmask[0] = 0xfffe0000u;                                 /* 128 KB      */
    f->cfg[0x14 / 4] = 0x0000c041u;                              /* I/O BAR1    */
    f->barmask[1] = 0xffffffc1u;                                 /* 64 bytes    */
    f->cfg[0x2C / 4] = 0x11001af4u;                              /* subsys ids  */
    f->cfg[0x3C / 4] = 0x0000010bu;                              /* irq 11 pin A */
    addcap(f, 0x01, 0x40);                                       /* PM          */
    addcap(f, 0x05, 0x50);                                       /* MSI         */

    f = mkdev(0, 28, 0, 0x8086, 0x9d10, 0x06, 0x04, 0x00, 0x01); /* PCI bridge  */
    f->cfg[0x18 / 4] = 0x00020200u;                              /* sec/sub = 2 */

    f = mkdev(2, 0, 0, 0x10ec, 0x8168, 0x02, 0x00, 0x00, 0x00);  /* r8169       */
    f->cfg[0x10 / 4] = 0x0000e001u;                              /* I/O BAR0    */
    f->barmask[0] = 0xffffff01u;
    f->cfg[0x18 / 4] = 0xf7f00004u;                              /* 64-bit BAR2 */
    f->cfg[0x1C / 4] = 0x00000001u;                              /* upper half  */
    f->barmask[2] = 0xffffc004u; f->barmask[3] = 0xffffffffu;    /* 16 KB       */
    addcap(f, 0x10, 0x40);                                       /* PCIe        */
    addcap(f, 0x11, 0x70);                                       /* MSI-X       */
}

/* --- assertions ------------------------------------------------------------ */

static int g_fail;
static void ck(int cond, const char *what)
{
    printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) g_fail++;
}
static int has_line(const char *hay, const char *needle)
{ return strstr(hay, needle) != 0; }

int main(void)
{
    char buf[8192];
    uno_device *e1000, *r8169, *vga, *bridge;
    int n, i, sized;

    build_machine();
    n = devmgr_enumerate();

    printf("devmgr_list_str:\n");
    devmgr_list_str(buf, sizeof buf);
    fputs(buf, stdout);
    printf("checks:\n");

    ck(n == 7, "enumerated all 7 functions (incl. the one behind the bridge)");
    ck(!devmgr_overflow(), "no registry overflow");

    /* the multi-function bit gates fns 1..7: fn1 of dev 1 is in, and the
     * single-function host bridge did NOT get seven phantom siblings */
    ck(has_line(buf, "00:01.1 8086:7010 01/01 ide UNCLAIMED"),
       "multi-function device: fn 1 enumerated");
    for (i = 0, sized = 0; i < n; i++) {
        uno_device *d = devmgr_get(i);
        if (d->addr.pci.bus == 0 && d->addr.pci.dev == 0) sized++;
    }
    ck(sized == 1, "single-function device: no phantom fns 1..7");

    ck(has_line(buf, "02:00.0 10ec:8168 02/00 ethernet UNCLAIMED"),
       "device behind the PCI-PCI bridge is enumerated");
    ck(has_line(buf, "00:02.0 1234:1111 03/00 vga UNCLAIMED"), "class names decode");
    ck(!has_line(buf, "  "), "listing has no double space (URC last-token parse)");

    e1000  = devmgr_find(0x8086, 0x100e);
    r8169  = devmgr_find(0x10ec, 0x8168);
    vga    = devmgr_find_class(0x03, 0x00);
    bridge = devmgr_find(0x8086, 0x9d10);
    ck(e1000 && r8169 && vga && bridge, "devmgr_find / find_class locate devices");
    if (!e1000 || !r8169 || !vga || !bridge) { printf("FAILED %d\n", g_fail); return 1; }

    /* topology: the r8169's parent is the bridge, and the bridge is a root */
    ck(devmgr_get(r8169->parent) == bridge, "parent link points at the bridge");
    ck(bridge->parent == UNO_DEV_NOPARENT, "bus-0 devices are roots");
    ck(bridge->sec_bus == 2, "bridge secondary bus recorded");

    /* per-device detail read out of config space */
    ck(e1000->revision == 0x01 && e1000->subsys_vendor == 0x1af4 &&
       e1000->subsys_id == 0x1100, "revision + subsystem ids");
    ck(e1000->irq_line == 11 && e1000->irq_pin == 1, "irq line/pin");
    ck((e1000->caps & UNO_DEVCAP_MSI) && (e1000->caps & UNO_DEVCAP_PM) &&
       e1000->cap_msi == 0x50, "capability list walk (PM + MSI, offset)");
    ck((r8169->caps & UNO_DEVCAP_MSIX) && (r8169->caps & UNO_DEVCAP_PCIE),
       "capability list walk (PCIe + MSI-X)");

    /* BARs: kind, base, and the 64-bit pair consuming its upper slot */
    ck((e1000->bar_flags[0] & UNO_BAR_PRESENT) && e1000->bar[0] == 0xfebc0000ull &&
       !(e1000->bar_flags[0] & UNO_BAR_IO), "mem BAR base");
    ck((e1000->bar_flags[1] & UNO_BAR_IO) && e1000->bar[1] == 0xc040ull, "I/O BAR base");
    ck((r8169->bar_flags[2] & UNO_BAR_MEM64) && r8169->bar[2] == 0x1f7f00000ull,
       "64-bit BAR pairs into one entry");
    ck(!(r8169->bar_flags[3] & UNO_BAR_PRESENT), "upper half of a 64-bit BAR is not a BAR");
    ck((vga->bar_flags[0] & UNO_BAR_PREFETCH), "prefetchable flag");

    /* enumeration must not have written anything */
    ck(g_sizing == 0, "enumeration performed no config-space writes");

    /* opt-in sizing: refuses display, sizes the rest, restores what it touched */
    ck(devmgr_size_bars((int)(vga - devmgr_get(0))) == 0, "size_bars refuses display class");
    {
        int idx = (int)(e1000 - devmgr_get(0));
        unsigned cmd_before = lookup(0, 3, 0)->cfg[0x04 / 4];
        sized = devmgr_size_bars(idx);
        ck(sized == 2, "size_bars sized both e1000 BARs");
        ck(e1000->bar_sz[0] == 0x20000ull, "32-bit mem BAR size = 128 KB");
        ck(e1000->bar_sz[1] == 0x40ull, "I/O BAR size = 64 bytes");
        ck(lookup(0, 3, 0)->cfg[0x10 / 4] == 0xfebc0000u, "BAR restored after probe");
        ck(lookup(0, 3, 0)->cfg[0x04 / 4] == cmd_before, "command register restored");
        ck((lookup(0, 3, 0)->cfg[0x04 / 4] >> 16) == 0x0010u, "status half not cleared");
    }
    {
        int idx = (int)(r8169 - devmgr_get(0));
        sized = devmgr_size_bars(idx);
        ck(sized == 2, "size_bars handled the 64-bit pair as one BAR");
        ck(r8169->bar_sz[2] == 0x4000ull, "64-bit BAR size = 16 KB");
    }

    /* idempotence: a re-scan keeps a binding for a device still present */
    e1000->state = UNO_DEV_BOUND; e1000->drv = "e1000";
    devmgr_enumerate();
    e1000 = devmgr_find(0x8086, 0x100e);
    ck(e1000 && e1000->state == UNO_DEV_BOUND && e1000->drv &&
       !strcmp(e1000->drv, "e1000"), "re-scan preserves an existing binding");
    devmgr_list_str(buf, sizeof buf);
    ck(has_line(buf, "00:03.0 8086:100e 02/00 ethernet e1000"),
       "bound device reports its driver in the listing");
    ck(!(e1000->bar_flags[0] & UNO_BAR_SIZED), "re-scan clears stale BAR sizes");

    /* truncation safety: a cap smaller than the listing must still NUL-terminate */
    {
        char small[40];
        int len = devmgr_list_str(small, (int)sizeof small);
        ck(len < (int)sizeof small && small[len] == 0, "listing truncates safely");
    }
    {
        char one[1];
        ck(devmgr_list_str(one, 1) == 0 && one[0] == 0, "cap of 1 writes just the NUL");
    }

    /* detail view */
    devmgr_detail_str((int)(r8169 - devmgr_get(0)), buf, sizeof buf);
    printf("devmgr_detail_str(r8169):\n%s", buf);
    ck(has_line(buf, "msi-x") && has_line(buf, "behind 00:1c.0"), "detail: caps + parent");
    devmgr_detail_str(999, buf, sizeof buf);
    ck(has_line(buf, "no such device"), "detail: out-of-range index");

    printf(g_fail ? "\nFAILED (%d)\n" : "\nall checks passed (%d failures)\n", g_fail);
    return g_fail ? 1 : 0;
}
