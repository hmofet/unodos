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
