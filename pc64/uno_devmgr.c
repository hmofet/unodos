/* unodevices - Phase 1: PCI enumeration into the device registry + text and
 * tuple introspection.  See DEVICES.md for the contract and UNODEVICES-PLAN.md
 * for the rollout.
 *
 * Owns discovery; CONSUMES pc64_pci.c's config accessors (pci_cfg_read32 and
 * friends) and does not edit that shared file.
 *
 * Enumeration is a RECURSIVE walk from bus 0, following each type-1 header's
 * secondary bus, so the registry records the parent/child topology - which
 * function sits behind which bridge - rather than a flat address list.  (On a
 * UEFI-booted machine the firmware has already assigned bus numbers, so a flat
 * 0..255 scan reaches the same *set* of devices; the tree is what a flat scan
 * cannot give, and it is what the bind pipeline and hotplug rescan need.)
 *
 * Read-only by construction: enumeration issues config-space READS only.  The
 * one write path in the file, devmgr_size_bars(), is opt-in per device and
 * never runs during a scan - see the comment there for why that matters while
 * the firmware still owns the hardware. */
#include "pc64_pci.h"
#include "uno_devmgr.h"
#include "fat.h"            /* phase 4: listing \DRIVERS\ for loadable drivers */
#include "uno_debug.h"      /* svc_log; a no-op macro in production */

/* Physical base of the active GOP framebuffer (uefi_main.c), 0 if unknown.
 * Only consumed by devmgr_size_bars() to refuse the scanout BAR. */
unsigned long long uno_pc64_fb_phys(void);

/* --- registry -------------------------------------------------------------- */

static uno_device g_dev[UNO_DEV_MAX];
static int        g_n;                 /* devices in the table                  */
static int        g_scanned;           /* enumerate() has run at least once     */
static int        g_overflow;          /* hit UNO_DEV_MAX and stopped adding    */
static unsigned char g_seen[32];       /* bus-number bitmap, loop guard         */

static int bus_seen(int bus)  { return (g_seen[(bus >> 3) & 31] >> (bus & 7)) & 1; }
static void bus_mark(int bus) { g_seen[(bus >> 3) & 31] |= (unsigned char)(1 << (bus & 7)); }

/* Sticky platform-device registrations (devmgr_add_platform): logical blocks
 * that live inside a PCI function - re-applied after every PCI walk so a
 * re-scan does not drop them.  Keyed by the BACKING function's location so the
 * re-emit can re-find its (possibly renumbered) registry index. */
typedef struct {
    int  used;
    unsigned char bbus, bdev, bfn;     /* backing PCI function location         */
    unsigned char cls, sub;
    unsigned long long io_base, io_len;
    const char *drv;
} devmgr_plat_reg;
#define DEVMGR_PLAT_MAX 4
static devmgr_plat_reg g_plat[DEVMGR_PLAT_MAX];

/* --- tiny NUL-terminated string builders (bounds-checked to cap-1) --------- */

static int s_cat(char *b, int cap, int at, const char *s) {
    while (*s && at < cap - 1) b[at++] = *s++;
    b[at] = 0;
    return at;
}
static int s_hex(char *b, int cap, int at, unsigned long long v, int digits) {
    static const char H[] = "0123456789abcdef";
    char t[17];
    int i;
    if (digits > 16) digits = 16;
    for (i = digits - 1; i >= 0; i--) { t[i] = H[v & 0xF]; v >>= 4; }
    t[digits] = 0;
    return s_cat(b, cap, at, t);
}
static int s_dec(char *b, int cap, int at, unsigned long long v) {
    char t[21];
    int i = 0;
    if (!v) return s_cat(b, cap, at, "0");
    while (v && i < 20) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i-- > 0 && at < cap - 1) b[at++] = t[i];
    b[at] = 0;
    return at;
}

/* Driver names are short single tokens, so a local compare beats pulling in
 * string.h for one call site. */
static int s_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* --- class decode ---------------------------------------------------------- */

/* Short name for a class/subclass pair.  SINGLE TOKEN, always: the URC
 * `devices` host parser splits the line on whitespace and reads the LAST token
 * as the driver column, so a name with a space in it would mis-split. */
const char *devmgr_class_name(unsigned char cls, unsigned char sub)
{
    switch (cls) {
    case 0x00: return "unclassified";
    case 0x01: return sub == 0x01 ? "ide"
                    : sub == 0x06 ? "sata"
                    : sub == 0x08 ? "nvme"
                    : sub == 0x07 ? "sas"
                    : "storage";
    case 0x02: return sub == 0x00 ? "ethernet"
                    : sub == 0x80 ? "network-other"
                    : "network";
    case 0x03: return sub == 0x00 ? "vga" : "display";
    case 0x04: return sub == 0x01 ? "audio"
                    : sub == 0x03 ? "hda"
                    : "multimedia";
    case 0x05: return "memory";
    case 0x06: return sub == 0x00 ? "host-bridge"
                    : sub == 0x01 ? "isa-bridge"
                    : sub == 0x04 ? "pci-bridge"
                    : "bridge";
    case 0x07: return "comm";
    case 0x08: return sub == 0x05 ? "sd-host" : "system";
    case 0x09: return "input";
    case 0x0A: return "dock";
    case 0x0B: return "cpu";
    case 0x0C: return sub == 0x03 ? "usb"
                    : sub == 0x05 ? "smbus"
                    : "serial-bus";
    case 0x0D: return "wireless";
    case 0x0E: return "intelligent-io";
    case 0x0F: return "satellite";
    case 0x10: return "crypto";
    case 0x11: return "signal";
    case 0x12: return "accelerator";
    case 0x13: return "instrumentation";
    default:   return "other";
    }
}

/* --- enumeration ----------------------------------------------------------- */

/* Preserve a bound driver across a re-scan: same address, same ids = same
 * device, so the manager must not tear down what is already bound (DEVICES.md
 * §2 "devmgr_enumerate() is idempotent").  Only the binding is snapshotted,
 * not the whole table - everything else is re-read from config space. */
typedef struct {
    unsigned char bus, dev, fn, state;
    unsigned short vendor, device;
    const char *drv;
    void *drvdata;
} devmgr_bind_snap;

static void carry_binding(uno_device *d, const devmgr_bind_snap *old, int oldn)
{
    int i;
    for (i = 0; i < oldn; i++) {
        const devmgr_bind_snap *o = &old[i];
        if (o->bus != d->addr.pci.bus || o->dev != d->addr.pci.dev ||
            o->fn  != d->addr.pci.fn) continue;
        if (o->vendor != d->vendor || o->device != d->device) continue;
        d->drv = o->drv; d->drvdata = o->drvdata; d->state = o->state;
        return;
    }
}

/* Walk the capability list (config 0x34 -> chained), recording which
 * capabilities exist and where.  Reads only; bounded so a corrupt/looping
 * pointer chain cannot hang the scan. */
static void read_caps(const pci_dev *pd, uno_device *d)
{
    unsigned status = pci_cfg_read32(pd, 0x04) >> 16;
    int pos, guard;
    if (!(status & (1u << 4))) return;                 /* no capability list  */
    pos = (int)(pci_cfg_read32(pd, 0x34) & 0xFC);
    for (guard = 0; pos >= 0x40 && pos < 0x100 && guard < 48; guard++) {
        unsigned w  = pci_cfg_read32(pd, pos);
        unsigned id = w & 0xFF;
        if (id == 0xFF) break;
        switch (id) {
        case 0x01: d->caps |= UNO_DEVCAP_PM;   break;
        case 0x05: d->caps |= UNO_DEVCAP_MSI;  d->cap_msi  = (unsigned char)pos; break;
        case 0x09: d->caps |= UNO_DEVCAP_VNDR; break;
        case 0x10: d->caps |= UNO_DEVCAP_PCIE; d->cap_pcie = (unsigned char)pos; break;
        case 0x11: d->caps |= UNO_DEVCAP_MSIX; d->cap_msix = (unsigned char)pos; break;
        default: break;
        }
        pos = (int)((w >> 8) & 0xFC);
    }
}

/* Record the BARs' base addresses and kind.  READ-ONLY: no size probe here
 * (that is devmgr_size_bars, opt-in).  A type-1 header has 2 BARs, type-0 has
 * 6; a 64-bit memory BAR consumes the following slot, which stays empty. */
static void read_bars(const pci_dev *pd, uno_device *d)
{
    int nbar = (d->hdr_type == 1) ? 2 : 6;
    int i;
    if (d->hdr_type > 1) nbar = 0;                     /* cardbus: not ours   */
    for (i = 0; i < nbar; i++) {
        unsigned lo = pci_cfg_read32(pd, 0x10 + i * 4);
        if (!lo) continue;                             /* unimplemented BAR   */
        d->bar_flags[i] = UNO_BAR_PRESENT;
        if (lo & 1) {                                  /* I/O space           */
            d->bar_flags[i] |= UNO_BAR_IO;
            d->bar[i] = lo & ~0x3u;
            continue;
        }
        if (lo & 0x8) d->bar_flags[i] |= UNO_BAR_PREFETCH;
        if (((lo >> 1) & 3) == 2 && i + 1 < nbar) {    /* 64-bit memory BAR   */
            unsigned hi = pci_cfg_read32(pd, 0x10 + (i + 1) * 4);
            d->bar_flags[i] |= UNO_BAR_MEM64;
            d->bar[i] = ((unsigned long long)hi << 32) | (lo & ~0xFu);
            i++;                                       /* upper half consumed */
            continue;
        }
        d->bar[i] = lo & ~0xFu;
    }
}

static void scan_bus(int bus, int parent);

/* Add one present function to the registry; recurse if it is a bridge. */
static void add_fn(int bus, int dev, int fn, int parent)
{
    pci_dev pd;
    uno_device *d;
    unsigned id, cr, sub_ids;
    int idx;

    pd.bus = bus; pd.dev = dev; pd.fn = fn;
    pd.vendor = 0; pd.device = 0;
    id = pci_cfg_read32(&pd, 0x00);
    if ((id & 0xFFFF) == 0xFFFF) return;               /* nothing here        */

    if (g_n >= UNO_DEV_MAX) { g_overflow = 1; return; }
    idx = g_n++;
    d = &g_dev[idx];

    d->bus_type = UNO_BUS_PCI;
    d->parent   = (short)parent;
    d->addr.pci.bus = (unsigned char)bus;
    d->addr.pci.dev = (unsigned char)dev;
    d->addr.pci.fn  = (unsigned char)fn;
    d->vendor = (unsigned short)(id & 0xFFFF);
    d->device = (unsigned short)(id >> 16);

    cr = pci_cfg_read32(&pd, 0x08);                    /* class triple + rev  */
    d->revision = (unsigned char)(cr & 0xFF);
    d->prog_if  = (unsigned char)((cr >> 8) & 0xFF);
    d->subcls   = (unsigned char)((cr >> 16) & 0xFF);
    d->cls      = (unsigned char)((cr >> 24) & 0xFF);
    d->hdr_type = (unsigned char)((pci_cfg_read32(&pd, 0x0C) >> 16) & 0x7F);

    if (d->hdr_type == 0) {                            /* endpoint only       */
        sub_ids = pci_cfg_read32(&pd, 0x2C);
        d->subsys_vendor = (unsigned short)(sub_ids & 0xFFFF);
        d->subsys_id     = (unsigned short)(sub_ids >> 16);
    }
    {   unsigned irq = pci_cfg_read32(&pd, 0x3C);
        d->irq_line = (unsigned char)(irq & 0xFF);
        d->irq_pin  = (unsigned char)((irq >> 8) & 0xFF);
    }
    read_caps(&pd, d);
    read_bars(&pd, d);
    d->state = UNO_DEV_UNBOUND;

    if (d->hdr_type == 1) {                            /* PCI-PCI bridge      */
        unsigned busses = pci_cfg_read32(&pd, 0x18);
        int sec = (int)((busses >> 8) & 0xFF);
        d->sec_bus = (unsigned char)sec;
        /* sec == 0 means the bridge is unconfigured (no firmware assignment);
         * there is nothing to walk and following it would re-scan bus 0. */
        if (sec > 0 && !bus_seen(sec)) scan_bus(sec, idx);
    }
}

static void scan_bus(int bus, int parent)
{
    int dev, fn;
    if (bus < 0 || bus > 255 || bus_seen(bus)) return;
    bus_mark(bus);
    for (dev = 0; dev < 32; dev++) {
        pci_dev d0;
        int nfn;
        d0.bus = bus; d0.dev = dev; d0.fn = 0;
        d0.vendor = 0; d0.device = 0;
        if ((pci_cfg_read32(&d0, 0x00) & 0xFFFF) == 0xFFFF) continue;
        /* multi-function bit (header type 0x80) - probing fns 1..7 on a
         * single-function device is legal but pointless, and some devices
         * alias fn 0 across all eight, which would duplicate the registry. */
        nfn = (((pci_cfg_read32(&d0, 0x0C) >> 16) & 0x80) != 0) ? 8 : 1;
        for (fn = 0; fn < nfn; fn++) add_fn(bus, dev, fn, parent);
    }
}

/* Append one sticky platform registration to the table as a UNO_BUS_PLATFORM
 * node, inheriting the backing PCI function's location + ids.  Silently skips a
 * registration whose backing function is no longer present, or if the table is
 * full.  Returns the new index, or -1. */
static int emit_platform(const devmgr_plat_reg *r)
{
    uno_device *d;
    int parent = UNO_DEV_NOPARENT, i;

    for (i = 0; i < g_n; i++) {          /* find the backing function (a root)   */
        if (g_dev[i].bus_type == UNO_BUS_PCI &&
            g_dev[i].addr.pci.bus == r->bbus && g_dev[i].addr.pci.dev == r->bdev &&
            g_dev[i].addr.pci.fn == r->bfn) { parent = i; break; }
    }
    if (parent == UNO_DEV_NOPARENT) return -1;
    /* idempotent against the live table: if this platform node is already
     * present (e.g. just re-emitted by enumerate), don't append a duplicate */
    for (i = 0; i < g_n; i++)
        if (g_dev[i].bus_type == UNO_BUS_PLATFORM && g_dev[i].parent == parent &&
            g_dev[i].drv == r->drv) return i;
    if (g_n >= UNO_DEV_MAX) { g_overflow = 1; return -1; }

    d = &g_dev[g_n];
    *d = g_dev[parent];                  /* inherit location + ven:dev, then edit */
    d->bus_type = UNO_BUS_PLATFORM;
    d->parent   = (short)parent;
    d->cls = r->cls; d->subcls = r->sub; d->prog_if = 0;
    d->hdr_type = 0; d->sec_bus = 0;
    d->caps = 0; d->cap_msi = d->cap_msix = d->cap_pcie = 0;
    for (i = 0; i < 6; i++) { d->bar[i] = 0; d->bar_sz[i] = 0; d->bar_flags[i] = 0; }
    d->bar[0] = r->io_base;
    d->bar_sz[0] = r->io_len;
    d->bar_flags[0] = UNO_BAR_PRESENT | UNO_BAR_IO | UNO_BAR_SIZED;
    d->state = UNO_DEV_BOUND;
    d->drv = r->drv; d->drvdata = 0;
    return g_n++;
}

static void reemit_platform(void)
{
    int i;
    for (i = 0; i < DEVMGR_PLAT_MAX; i++)
        if (g_plat[i].used) emit_platform(&g_plat[i]);
}

int devmgr_add_platform(int backing, unsigned char cls, unsigned char sub,
                        unsigned long long io_base, unsigned long long io_len,
                        const char *drv)
{
    uno_device *b = devmgr_get(backing);
    devmgr_plat_reg *slot = 0;
    int i;

    if (!b || b->bus_type != UNO_BUS_PCI) return -1;
    /* dedup on the backing location + driver; else take the first free slot */
    for (i = 0; i < DEVMGR_PLAT_MAX; i++) {
        if (g_plat[i].used && g_plat[i].bbus == b->addr.pci.bus &&
            g_plat[i].bdev == b->addr.pci.dev && g_plat[i].bfn == b->addr.pci.fn &&
            g_plat[i].drv == drv) { slot = &g_plat[i]; break; }
        if (!slot && !g_plat[i].used) slot = &g_plat[i];
    }
    if (!slot) return -1;
    slot->used = 1;
    slot->bbus = b->addr.pci.bus; slot->bdev = b->addr.pci.dev; slot->bfn = b->addr.pci.fn;
    slot->cls = cls; slot->sub = sub;
    slot->io_base = io_base; slot->io_len = io_len; slot->drv = drv;
    return emit_platform(slot);          /* materialise it now                   */
}

int devmgr_enumerate(void)
{
    static devmgr_bind_snap old[UNO_DEV_MAX];
    int oldn = g_n, i;

    for (i = 0; i < oldn; i++) {                       /* keep bindings        */
        old[i].bus = g_dev[i].addr.pci.bus; old[i].dev = g_dev[i].addr.pci.dev;
        old[i].fn  = g_dev[i].addr.pci.fn;  old[i].state = g_dev[i].state;
        old[i].vendor = g_dev[i].vendor;    old[i].device = g_dev[i].device;
        old[i].drv = g_dev[i].drv;          old[i].drvdata = g_dev[i].drvdata;
    }
    for (i = 0; i < UNO_DEV_MAX; i++) {
        uno_device *d = &g_dev[i];
        int b;
        d->bus_type = UNO_BUS_PCI; d->parent = UNO_DEV_NOPARENT;
        d->addr.pci.bus = d->addr.pci.dev = d->addr.pci.fn = 0;
        d->vendor = d->device = d->subsys_vendor = d->subsys_id = 0;
        d->cls = d->subcls = d->prog_if = d->revision = 0;
        d->hdr_type = d->irq_line = d->irq_pin = d->sec_bus = 0;
        d->caps = 0; d->cap_msi = d->cap_msix = d->cap_pcie = 0;
        for (b = 0; b < 6; b++) { d->bar[b] = 0; d->bar_sz[b] = 0; d->bar_flags[b] = 0; }
        d->state = UNO_DEV_UNBOUND; d->drv = 0; d->drvdata = 0;
    }
    for (i = 0; i < 32; i++) g_seen[i] = 0;
    g_n = 0; g_overflow = 0;

    /* Bus 0, then anything behind a bridge on it (add_fn recurses).  A
     * multi-function host bridge at 00:00 whose functions 1..7 are separate
     * host bridges (the classic multi-root board) is covered because those
     * roots' busses are reached through their own bridge entries. */
    scan_bus(0, UNO_DEV_NOPARENT);

    /* Sweep every bus the recursive walk never reached.  On a multi-root
     * machine each root complex owns a top-level bus with no bridge above it,
     * and a bridge left unconfigured by firmware (secondary = 0) hides its
     * children from the walk - so the flat sweep is the safety net that keeps
     * this a strict superset of the old pci_find() scan.  scan_bus() returns
     * immediately for a bus already visited; devices found here are roots
     * (no parent), which is exactly what they are topologically. */
    for (i = 1; i < 256; i++) scan_bus(i, UNO_DEV_NOPARENT);

    for (i = 0; i < g_n; i++) carry_binding(&g_dev[i], old, oldn);
    reemit_platform();                 /* sticky platform nodes survive re-scan  */
    g_scanned = 1;
    return g_n;
}

int devmgr_count(void)
{
    if (!g_scanned) devmgr_enumerate();
    return g_n;
}

uno_device *devmgr_get(int idx)
{
    if (!g_scanned) devmgr_enumerate();
    if (idx < 0 || idx >= g_n) return 0;
    return &g_dev[idx];
}

uno_device *devmgr_find(unsigned short ven, unsigned short dev)
{
    int i, n = devmgr_count();
    for (i = 0; i < n; i++)
        if (g_dev[i].vendor == ven && g_dev[i].device == dev) return &g_dev[i];
    return 0;
}

uno_device *devmgr_find_class(unsigned char cls, unsigned char sub)
{
    int i, n = devmgr_count();
    for (i = 0; i < n; i++)
        if (g_dev[i].cls == cls && g_dev[i].subcls == sub) return &g_dev[i];
    return 0;
}

const char *devmgr_driver_name(int idx)
{
    uno_device *d = devmgr_get(idx);
    return (d && d->state == UNO_DEV_BOUND) ? d->drv : 0;
}

int devmgr_overflow(void)
{
    if (!g_scanned) devmgr_enumerate();
    return g_overflow;
}

/* --- BAR sizing (opt-in, the file's only config-space WRITE) ---------------- */

/* Sizing a BAR means writing all-ones and reading back the mask, which parks a
 * bogus address in the BAR for a few config cycles.  Every OS does this at
 * boot - but every OS does it while IT owns the hardware.  UnoDOS phase 1 runs
 * ATTACHED: UEFI boot services are alive and firmware drivers are still
 * driving these controllers.  Disabling decode on a live AHCI/xHCI mid-DMA is
 * exactly the class of thing that hangs real machines, and the plan's rule for
 * this phase is "boot behavior byte-identical".  So sizing is never part of a
 * scan: a caller asks for one device, deliberately.
 *
 * Guards: decode bits off around the probe and restored after; the original
 * value always written back; display devices and the live GOP framebuffer
 * refused outright (scanout reads that aperture continuously). */
int devmgr_size_bars(int idx)
{
    uno_device *d = devmgr_get(idx);
    pci_dev pd;
    unsigned long long fb;
    unsigned cmd;
    int i, nbar, sized = 0;

    if (!d || d->bus_type != UNO_BUS_PCI) return 0;
    if (d->cls == 0x03) return 0;                      /* display: never      */
    fb = uno_pc64_fb_phys();

    pd.bus = d->addr.pci.bus; pd.dev = d->addr.pci.dev; pd.fn = d->addr.pci.fn;
    pd.vendor = d->vendor; pd.device = d->device;

    nbar = (d->hdr_type == 1) ? 2 : (d->hdr_type == 0 ? 6 : 0);
    /* Command is the low half of dword 0x04 and STATUS is the high half, whose
     * error bits are write-1-to-clear - so write the dword with the status half
     * ZEROED, never with the value just read back, or probing a BAR silently
     * eats whatever errors the firmware had recorded. */
    cmd = pci_cfg_read32(&pd, 0x04) & 0xFFFFu;
    pci_cfg_write32(&pd, 0x04, cmd & ~0x3u);           /* mem + I/O decode off */

    for (i = 0; i < nbar; i++) {
        unsigned lo, mask, hi = 0, maskhi = 0;
        int is64;
        if (!(d->bar_flags[i] & UNO_BAR_PRESENT)) continue;
        /* the aperture the firmware is scanning out of - leave it alone */
        if (fb && d->bar[i] && fb >= d->bar[i] && fb - d->bar[i] < 0x40000000ull) continue;
        is64 = (d->bar_flags[i] & UNO_BAR_MEM64) != 0;
        lo = pci_cfg_read32(&pd, 0x10 + i * 4);
        if (is64) hi = pci_cfg_read32(&pd, 0x10 + (i + 1) * 4);
        pci_cfg_write32(&pd, 0x10 + i * 4, 0xFFFFFFFFu);
        if (is64) pci_cfg_write32(&pd, 0x10 + (i + 1) * 4, 0xFFFFFFFFu);
        mask = pci_cfg_read32(&pd, 0x10 + i * 4);
        if (is64) maskhi = pci_cfg_read32(&pd, 0x10 + (i + 1) * 4);
        pci_cfg_write32(&pd, 0x10 + i * 4, lo);        /* restore, always      */
        if (is64) pci_cfg_write32(&pd, 0x10 + (i + 1) * 4, hi);

        if (d->bar_flags[i] & UNO_BAR_IO) mask &= ~0x3u; else mask &= ~0xFu;
        if (is64) {
            unsigned long long m = ((unsigned long long)maskhi << 32) | mask;
            d->bar_sz[i] = m ? (~m + 1ull) : 0;
        } else {
            d->bar_sz[i] = mask ? (unsigned long long)((~mask + 1u)) : 0;
        }
        if (d->bar_sz[i]) { d->bar_flags[i] |= UNO_BAR_SIZED; sized++; }
    }
    pci_cfg_write32(&pd, 0x04, cmd);                   /* decode back on       */
    return sized;
}

/* --- introspection --------------------------------------------------------- */

static int emit_loc(char *b, int cap, int at, const uno_device *d)
{
    at = s_hex(b, cap, at, d->addr.pci.bus, 2);
    at = s_cat(b, cap, at, ":");
    at = s_hex(b, cap, at, d->addr.pci.dev, 2);
    at = s_cat(b, cap, at, ".");
    at = s_dec(b, cap, at, d->addr.pci.fn);
    return at;
}

int devmgr_list_str(char *buf, int cap)
{
    int i, n, at = 0;
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    n = devmgr_count();
    for (i = 0; i < n; i++) {
        const uno_device *d = &g_dev[i];
        at = emit_loc(buf, cap, at, d);
        at = s_cat(buf, cap, at, " ");
        at = s_hex(buf, cap, at, d->vendor, 4);
        at = s_cat(buf, cap, at, ":");
        at = s_hex(buf, cap, at, d->device, 4);
        at = s_cat(buf, cap, at, " ");
        at = s_hex(buf, cap, at, d->cls, 2);
        at = s_cat(buf, cap, at, "/");
        at = s_hex(buf, cap, at, d->subcls, 2);
        at = s_cat(buf, cap, at, " ");
        at = s_cat(buf, cap, at, devmgr_class_name(d->cls, d->subcls));
        at = s_cat(buf, cap, at, " ");
        /* Last token = the driver column.  Phase 1 binds nothing, so this is
         * UNCLAIMED for every device: it reports what the MANAGER has bound,
         * not whether a driver for the part exists in the tree (the legacy
         * pull-drivers still find their own hardware).  Phase 2 fills it in. */
        at = s_cat(buf, cap, at, (d->state == UNO_DEV_BOUND && d->drv) ? d->drv : "UNCLAIMED");
        at = s_cat(buf, cap, at, "\n");
        if (at >= cap - 1) return at;                  /* buffer full          */
    }
    return at;
}

int devmgr_detail_str(int idx, char *buf, int cap)
{
    uno_device *d = devmgr_get(idx);
    int at = 0, i;
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!d) {
        at = s_cat(buf, cap, 0, "no such device (registry holds ");
        at = s_dec(buf, cap, at, (unsigned)devmgr_count());
        at = s_cat(buf, cap, at, devmgr_overflow() ? ", TRUNCATED at UNO_DEV_MAX)\n" : ")\n");
        return at;
    }

    at = emit_loc(buf, cap, at, d);
    at = s_cat(buf, cap, at, " ");
    at = s_hex(buf, cap, at, d->vendor, 4);
    at = s_cat(buf, cap, at, ":");
    at = s_hex(buf, cap, at, d->device, 4);
    at = s_cat(buf, cap, at, " rev ");
    at = s_hex(buf, cap, at, d->revision, 2);
    at = s_cat(buf, cap, at, "\n  class ");
    at = s_hex(buf, cap, at, d->cls, 2);
    at = s_cat(buf, cap, at, "/");
    at = s_hex(buf, cap, at, d->subcls, 2);
    at = s_cat(buf, cap, at, "/");
    at = s_hex(buf, cap, at, d->prog_if, 2);
    at = s_cat(buf, cap, at, " ");
    at = s_cat(buf, cap, at, devmgr_class_name(d->cls, d->subcls));
    at = s_cat(buf, cap, at, d->hdr_type == 1 ? " (bridge)\n" : "\n");
    at = s_cat(buf, cap, at, "  subsys ");
    at = s_hex(buf, cap, at, d->subsys_vendor, 4);
    at = s_cat(buf, cap, at, ":");
    at = s_hex(buf, cap, at, d->subsys_id, 4);
    at = s_cat(buf, cap, at, "  irq ");
    at = s_dec(buf, cap, at, d->irq_line);
    at = s_cat(buf, cap, at, "  caps");
    if (d->caps & UNO_DEVCAP_PM)   at = s_cat(buf, cap, at, " pm");
    if (d->caps & UNO_DEVCAP_MSI)  at = s_cat(buf, cap, at, " msi");
    if (d->caps & UNO_DEVCAP_MSIX) at = s_cat(buf, cap, at, " msi-x");
    if (d->caps & UNO_DEVCAP_PCIE) at = s_cat(buf, cap, at, " pcie");
    if (!d->caps)                  at = s_cat(buf, cap, at, " none");
    at = s_cat(buf, cap, at, "\n");
    if (d->parent != UNO_DEV_NOPARENT) {
        uno_device *p = devmgr_get(d->parent);
        at = s_cat(buf, cap, at, "  behind ");
        if (p) at = emit_loc(buf, cap, at, p);
        at = s_cat(buf, cap, at, "\n");
    }
    for (i = 0; i < 6; i++) {
        if (!(d->bar_flags[i] & UNO_BAR_PRESENT)) continue;
        at = s_cat(buf, cap, at, "  bar");
        at = s_dec(buf, cap, at, i);
        at = s_cat(buf, cap, at, " ");
        at = s_cat(buf, cap, at, (d->bar_flags[i] & UNO_BAR_IO) ? "io " : "mem ");
        at = s_hex(buf, cap, at, d->bar[i], (d->bar_flags[i] & UNO_BAR_MEM64) ? 16 : 8);
        if (d->bar_flags[i] & UNO_BAR_SIZED) {
            at = s_cat(buf, cap, at, " size ");
            at = s_hex(buf, cap, at, d->bar_sz[i], 8);
        }
        at = s_cat(buf, cap, at, "\n");
    }
    at = s_cat(buf, cap, at, "  state ");
    at = s_cat(buf, cap, at, d->state == UNO_DEV_BOUND  ? "bound"
                           : d->state == UNO_DEV_FAILED ? "failed"
                           : d->state == UNO_DEV_GONE   ? "gone" : "unbound");
    if (d->state == UNO_DEV_BOUND && d->drv) {
        at = s_cat(buf, cap, at, " ");
        at = s_cat(buf, cap, at, d->drv);
    }
    at = s_cat(buf, cap, at, "\n");
    return at;
}

/* Struct-free row for callers on the far side of a module boundary (the
 * pc64-python uno.pci() binding lives in PYRT.UNO and resolves kernel symbols
 * by name, so handing it a uno_device* would pin the struct layout into a
 * separately-built module).  Keep the column order append-only. */
int devmgr_info(int idx, unsigned int *out, int nmax)
{
    uno_device *d = devmgr_get(idx);
    if (!d || !out || nmax < DEVMGR_ROW_N) return -1;
    out[0]  = d->addr.pci.bus;
    out[1]  = d->addr.pci.dev;
    out[2]  = d->addr.pci.fn;
    out[3]  = d->vendor;
    out[4]  = d->device;
    out[5]  = d->cls;
    out[6]  = d->subcls;
    out[7]  = d->prog_if;
    out[8]  = d->revision;
    out[9]  = d->subsys_vendor;
    out[10] = d->subsys_id;
    out[11] = d->caps;
    out[12] = d->state;
    out[13] = (unsigned int)(int)d->parent;
    out[14] = d->irq_line;
    return DEVMGR_ROW_N;
}

/* ===========================================================================
 * PHASE 2 - the driver registry, matching, and the fixpoint bind loop
 * ======================================================================== */

/* The linker set.  Entries land in `.unodrv$m`; these two markers land either
 * side of them because the COFF linker orders grouped sections by their `$`
 * suffix.  Iterating the span and SKIPPING NULLs is deliberate: the linker is
 * free to pad between contributions, and a padded slot reads as zero. */
__attribute__((used, section(".unodrv$a")))
static const uno_driver *const g_drv_a[1] = { 0 };
__attribute__((used, section(".unodrv$z")))
static const uno_driver *const g_drv_z[1] = { 0 };

/* Runtime-registered drivers (phase 4's loadable ones, and tests). */
#define DEVMGR_DRV_MAX 16
static const uno_driver *g_rt_drv[DEVMGR_DRV_MAX];
static int g_rt_n;

static int builtin_count(void)
{
    const uno_driver *const *p;
    int n = 0;
    for (p = g_drv_a + 1; p < g_drv_z; p++) if (*p) n++;
    return n;
}

static const uno_driver *builtin_at(int i)
{
    const uno_driver *const *p;
    int n = 0;
    for (p = g_drv_a + 1; p < g_drv_z; p++) {
        if (!*p) continue;
        if (n == i) return *p;
        n++;
    }
    return 0;
}

int devmgr_driver_count(void) { return builtin_count() + g_rt_n; }

static const uno_driver *driver_at(int i)
{
    int nb = builtin_count();
    if (i < 0) return 0;
    if (i < nb) return builtin_at(i);
    i -= nb;
    return (i < g_rt_n) ? g_rt_drv[i] : 0;
}

const char *devmgr_driver_at(int i)
{
    const uno_driver *d = driver_at(i);
    return d ? d->name : 0;
}

int devmgr_register(const uno_driver *drv)
{
    int i;
    if (!drv || !drv->probe || !drv->match) return 0;
    if (drv->api != UNO_DEVMGR_API) return 0;   /* built for another contract */
    for (i = 0; i < devmgr_driver_count(); i++) {
        const uno_driver *e = driver_at(i);
        if (e && e->name && drv->name && s_eq(e->name, drv->name)) return 0;  /* already have it */
    }
    if (g_rt_n >= DEVMGR_DRV_MAX) return 0;
    g_rt_drv[g_rt_n++] = drv;
    return 1;
}

/* How specifically does this driver match this device?  0 = not at all.
 * The BEST entry in the table wins, so a driver may list both an exact id and
 * a class fallback without the fallback weakening it. */
static int match_score(const uno_driver *drv, const uno_device *d)
{
    const uno_match *m;
    int best = 0;
    if (!drv || !drv->match || drv->bus != d->bus_type) return 0;
    for (m = drv->match; m->kind != UNO_MATCH_END; m++) {
        int s = 0;
        if (m->kind == UNO_MATCH_PCI_ID) {
            if (m->vendor == d->vendor && m->device == d->device) s = UNO_SPEC_ID;
        } else if (m->kind == UNO_MATCH_PCI_CLASS) {
            if (m->cls == d->cls && m->subcls == d->subcls) {
                if (m->have_progif)
                    s = (m->prog_if == d->prog_if) ? UNO_SPEC_CLASSPI : 0;
                else
                    s = UNO_SPEC_CLASS;
            }
        }
        if (s > best) best = s;
    }
    return best;
}

/* Offer one device to every driver that matches it, most specific first, until
 * one accepts.  A declining probe is not an error and not a log line: "not
 * mine" is the normal way two drivers that share a class sort themselves out
 * (the NIC lane relies on exactly this). */
static int bind_one(uno_device *d)
{
    int tried[DEVMGR_DRV_MAX + 8];
    int ntried = 0, i, n = devmgr_driver_count();
    for (;;) {
        const uno_driver *best = 0;
        int bestscore = 0, bestidx = -1;
        for (i = 0; i < n; i++) {
            const uno_driver *drv = driver_at(i);
            int s, j, skip = 0;
            if (!drv) continue;
            for (j = 0; j < ntried; j++) if (tried[j] == i) { skip = 1; break; }
            if (skip) continue;
            s = match_score(drv, d);
            if (s > bestscore) { bestscore = s; best = drv; bestidx = i; }
        }
        if (!best) return 0;                    /* nothing (left) matches      */
        if (ntried < (int)(sizeof tried / sizeof tried[0])) tried[ntried++] = bestidx;
        else return 0;
        if (best->probe(d)) {
            d->state = UNO_DEV_BOUND;
            d->drv   = best->name;
            return 1;
        }
        /* declined: fall through and offer the next-most-specific candidate */
    }
}

int devmgr_bind_all(void)
{
    int total = 0, pass;
    if (!g_scanned) devmgr_enumerate();
    /* Fixpoint, not a fixed count: binding a controller may create children
     * (plan decision 2) whose drivers deserve a pass of their own. The cap is
     * a runaway guard, not the expected depth - a machine reaching it has a
     * driver whose probe creates a device on every call. */
    for (pass = 0; pass < 8; pass++) {
        int changed = 0, i;
        for (i = 0; i < g_n; i++) {
            if (g_dev[i].state != UNO_DEV_UNBOUND) continue;
            if (bind_one(&g_dev[i])) changed++;
        }
        total += changed;
        if (!changed) break;
    }
    return total;
}

static const uno_driver *driver_by_name(const char *name)
{
    int i, n = devmgr_driver_count();
    if (!name) return 0;
    for (i = 0; i < n; i++) {
        const uno_driver *d = driver_at(i);
        if (d && d->name && s_eq(d->name, name)) return d;
    }
    return 0;
}

int devmgr_release(int idx)
{
    uno_device *d = devmgr_get(idx);
    const uno_driver *drv;
    if (!d || d->state != UNO_DEV_BOUND) return 0;
    drv = driver_by_name(d->drv);
    if (drv && drv->remove) drv->remove(d);
    d->drv = 0; d->drvdata = 0;
    d->state = UNO_DEV_UNBOUND;
    return 1;
}

/* ===========================================================================
 * PHASE 4 - hotplug rescan, the driver services, and loadable .UNO drivers
 * ======================================================================== */

/* Hotplug, the departure half.  devmgr_enumerate() is idempotent and carries
 * bindings across a re-scan (carry_binding), so a device that is STILL there
 * keeps its driver - and a device that has gone would otherwise leave a stale
 * BOUND node behind.  This diffs that: anything bound before the scan and
 * absent after gets its remove() called and is counted as a departure.
 *
 * The contract that matters is the driver's, and it is the lesson the trackpad
 * detach-gate bug taught: after remove() returns, the driver must not touch
 * that device's MMIO again.  The manager cannot enforce it, so it is written
 * down (DEVICES.md 7) and remove() is called before the node is reused. */
int devmgr_rescan(void)
{
    struct { unsigned char bus, dev, fn; const char *drv; } was[UNO_DEV_MAX];
    int i, j, n_was = 0, changes = 0, n_before = g_n;

    if (!g_scanned) { devmgr_enumerate(); return devmgr_bind_all(); }

    for (i = 0; i < n_before && n_was < UNO_DEV_MAX; i++) {
        if (g_dev[i].state != UNO_DEV_BOUND) continue;
        if (g_dev[i].bus_type != UNO_BUS_PCI) continue;
        was[n_was].bus = g_dev[i].addr.pci.bus;
        was[n_was].dev = g_dev[i].addr.pci.dev;
        was[n_was].fn  = g_dev[i].addr.pci.fn;
        was[n_was].drv = g_dev[i].drv;
        n_was++;
    }

    devmgr_enumerate();                 /* rebuilds the table, carries bindings */

    for (j = 0; j < n_was; j++) {       /* departures: bound before, absent now */
        int still = 0;
        for (i = 0; i < g_n; i++) {
            if (g_dev[i].bus_type != UNO_BUS_PCI) continue;
            if (g_dev[i].addr.pci.bus == was[j].bus &&
                g_dev[i].addr.pci.dev == was[j].dev &&
                g_dev[i].addr.pci.fn  == was[j].fn) { still = 1; break; }
        }
        if (still) continue;
        {   const uno_driver *drv = driver_by_name(was[j].drv);
            /* The node is gone from the table, so a departing driver gets a
             * synthetic stand-in carrying its ADDRESS and nothing else.  That
             * is deliberate: drvdata pointed into a table slot the re-scan has
             * already rebuilt, so handing it back would be handing back a
             * dangling reference.  The contract says remove() may trust the
             * address and must not trust anything else. */
            if (drv && drv->remove) {
                uno_device tmp;
                unsigned char *p = (unsigned char *)&tmp;
                int k;
                for (k = 0; k < (int)sizeof tmp; k++) p[k] = 0;
                tmp.bus_type     = UNO_BUS_PCI;
                tmp.parent       = UNO_DEV_NOPARENT;
                tmp.addr.pci.bus = was[j].bus;
                tmp.addr.pci.dev = was[j].dev;
                tmp.addr.pci.fn  = was[j].fn;
                tmp.state        = UNO_DEV_GONE;
                tmp.drv          = was[j].drv;
                drv->remove(&tmp);
            }
        }
        changes++;
    }

    changes += devmgr_bind_all();       /* arrivals bind on the way out */
    return changes;
}

/* --- the services struct handed to a loadable driver ----------------------- */

void uno_pc64_delay_ms(int ms);                    /* uefi_main.c   */
unsigned long long uno_native_rdtsc(void);         /* pc64_native.c */
/* uno_debug.h compiles uno_dbg_log away to a no-op in production, so the
 * services log is free there rather than an unresolved symbol. */

static unsigned int svc_cfg_read32(const uno_device *d, int off)
{
    pci_dev pd;
    if (!d || d->bus_type != UNO_BUS_PCI) return 0xFFFFFFFFu;
    pd.bus = d->addr.pci.bus; pd.dev = d->addr.pci.dev; pd.fn = d->addr.pci.fn;
    pd.vendor = d->vendor; pd.device = d->device;
    return pci_cfg_read32(&pd, off);
}

static void svc_cfg_write32(const uno_device *d, int off, unsigned int v)
{
    pci_dev pd;
    if (!d || d->bus_type != UNO_BUS_PCI) return;
    pd.bus = d->addr.pci.bus; pd.dev = d->addr.pci.dev; pd.fn = d->addr.pci.fn;
    pd.vendor = d->vendor; pd.device = d->device;
    pci_cfg_write32(&pd, off, v);
}

/* Hand back a usable pointer to a BAR and turn its decode on.  Identity-mapped
 * on this platform, so there is no page table to touch - but drivers go through
 * here anyway, so a future paged port has exactly one place to change and the
 * refusals below are unavoidable rather than per-driver etiquette. */
static void *svc_map_bar(uno_device *d, int bar, unsigned long long *len)
{
    unsigned int cmd;
    if (len) *len = 0;
    if (!d || d->bus_type != UNO_BUS_PCI || bar < 0 || bar > 5) return 0;
    if (!(d->bar_flags[bar] & UNO_BAR_PRESENT)) return 0;
    if (d->bar_flags[bar] & UNO_BAR_IO) return 0;      /* I/O ports are not mapped */
    if (!d->bar[bar]) return 0;
    cmd = svc_cfg_read32(d, 0x04) & 0xFFFFu;
    svc_cfg_write32(d, 0x04, cmd | 0x6u);              /* memory decode + bus master */
    if (len && (d->bar_flags[bar] & UNO_BAR_SIZED)) *len = d->bar_sz[bar];
    return (void *)(unsigned long long)d->bar[bar];
}

/* A bump arena, never freed.  A driver's DMA buffers live as long as the driver
 * does, and a general allocator reachable post-EBS is a far larger promise than
 * phase 4 needs to make.  64-byte alignment is the BOT lesson from usbmsc.c
 * restated as a service: DMA must never target the stack. */
#define DRV_DMA_BYTES 65536
static unsigned char g_drv_dma[DRV_DMA_BYTES] __attribute__((aligned(64)));
static unsigned long g_drv_dma_at;

static void *svc_dma_alloc(unsigned long bytes)
{
    unsigned long at = (g_drv_dma_at + 63u) & ~63ul;
    if (!bytes || bytes > DRV_DMA_BYTES || at + bytes > DRV_DMA_BYTES) return 0;
    g_drv_dma_at = at + bytes;
    return &g_drv_dma[at];
}

/* MSI, owned by the manager so no driver ever touches _PRT or INTx routing.
 * Every machine in this fleet has a history of unusable legacy IRQs, so the
 * house style is MSI everywhere, and a driver that cannot get one should poll
 * rather than fall back to a line it has no reason to trust. */
static int svc_msi_enable(uno_device *d, int vector)
{
    unsigned int dw, mc;
    int off;
    if (!d || d->bus_type != UNO_BUS_PCI || !d->cap_msi) return 0;
    off = d->cap_msi;
    dw = svc_cfg_read32(d, off);
    mc = dw >> 16;                                     /* message control */
    svc_cfg_write32(d, off + 0x04, 0xFEE00000u);       /* LAPIC 0 */
    if (mc & 0x0080u) {                                /* 64-bit capable */
        svc_cfg_write32(d, off + 0x08, 0);
        svc_cfg_write32(d, off + 0x0C, (unsigned int)(vector & 0xFF));
    } else {
        svc_cfg_write32(d, off + 0x08, (unsigned int)(vector & 0xFF));
    }
    mc &= ~0x0070u;                                    /* one vector allocated */
    mc |= 0x0001u;                                     /* MSI enable           */
    svc_cfg_write32(d, off, (dw & 0xFFFFu) | (mc << 16));
    return 1;
}

static void svc_log(const char *s) { uno_dbg_log("%s", s ? s : ""); }

static const uno_drv_services g_svc = {
    UNO_DRVSVC_API,
    svc_cfg_read32, svc_cfg_write32, svc_map_bar, svc_dma_alloc,
    svc_msi_enable, uno_pc64_delay_ms, uno_native_rdtsc, svc_log
};

/* --- loading \DRIVERS\*.UNO ------------------------------------------------ */

int uno_fat_volumes(void);
int uno_fat_list_ex(int vol, const char *dir, uno_fat_entry *ents, int maxn);
/* Returns void* rather than UnoDrvEntry so pc64_modload.c needs no include of
 * this header: the loader knows how to place a module, not what a driver is. */
void *uno_mod_load_drv(int vol, const char *file);         /* pc64_modload.c */

#define DRV_SCAN_MAX 12

int devmgr_load_drivers(void)
{
    int nv = uno_fat_volumes(), v, loaded = 0;
    for (v = 0; v < nv; v++) {
        uno_fat_entry ent[DRV_SCAN_MAX];
        int n = uno_fat_list_ex(v, "DRIVERS", ent, DRV_SCAN_MAX), i;
        if (n <= 0) continue;
        for (i = 0; i < n; i++) {
            UnoDrvEntry e;
            const uno_drv_module *m;
            if (ent[i].is_dir || ent[i].size <= 0) continue;
            e = (UnoDrvEntry)uno_mod_load_drv(v, ent[i].name);
            if (!e) continue;
            m = e(&g_svc);
            /* TWO independent version gates, and both earn their keep: the
             * MODULE says which services struct it was compiled against, the
             * DRIVER record says which registry contract.  A driver built for
             * an older services struct would read function pointers at the
             * wrong offsets, and that is not a failure that reports itself. */
            if (!m || m->api != UNO_DRVSVC_API || !m->drv) continue;
            if (devmgr_register(m->drv)) loaded++;
        }
    }
    if (loaded) devmgr_bind_all();
    return loaded;
}
