/* cosmo64/pci.c -- pc64's PCI config-space API on a SoC that has no PCI.
 *
 * pc64's xhci.c finds its controller the way every PC driver does: it walks
 * PCI configuration space for a class 0C/03 prog-if 30 function, reads BAR0,
 * and enables bus mastering. The MT6771 has no PCI bus; its xHCI is a memory-
 * mapped platform block at a fixed address the device tree names.
 *
 * The honest fix is a discovery seam in xhci.c. That file belongs to the USB
 * lane, not to this one (AGENTS.md, ownership registry), and the pc64_pci.h
 * API it consumes is already a seam this platform has to implement anyway --
 * fat.c asks it which storage controller the system sits on. So rather than
 * edit a file it does not own, this platform answers the questions xhci.c
 * asks through the API it already uses: there is exactly one function on the
 * bus, at 00:00.0, it is a USB3 host controller, its BAR0 is 0x11200000, and
 * it is already enabled. Every other address is empty (vendor 0xFFFF), which
 * is what an absent function reads as on real PCI.
 *
 * Nothing here is pretending in a way that could mislead a reader of the log:
 * the synthetic IDs are MediaTek's vendor number and the SoC's part number,
 * so the takeover line xhci.c prints -- "taking 00:00.0 14c3:6771 bar0=
 * 11200000" -- says exactly what is being driven.
 *
 * If xhci.c ever needs something this seam cannot express, the next step is
 * a request to the USB lane, not a widening of the fiction here.
 */

#include "pc64_pci.h"
#include "cosmo64.h"

#define XHCI_BASE   0x11200000u
#define VENDOR_MTK  0x14C3u
#define DEVICE_SOC  0x6771u

/* The function appears on the bus only once ssusb.c has brought the block
 * out of reset (usb.c flips this). Before that -- and forever on a board
 * with no SSUSB, such as the QEMU gate -- the bus is empty, so xhci.c's scan
 * finds nothing and says so in one line, instead of taking a controller
 * that reads all-ones through five timed-out bring-up attempts. The shell
 * calls uno_xhci_init() on its own account as well as through usb.c, which
 * is why this cannot be left to the caller. */
static int g_exposed;

void c64_pci_expose_xhci(int on)
{
    g_exposed = on;
}

static int is_ours(const pci_dev *d)
{
    return g_exposed && d && d->bus == 0 && d->dev == 0 && d->fn == 0;
}

/* header type 0, the fields xhci.c and fat.c read; everything else is 0 */
static unsigned int cfg_read(const pci_dev *d, int off)
{
    if (!is_ours(d))
        return 0xFFFFFFFFu;
    switch (off & ~3) {
    case 0x00: return (DEVICE_SOC << 16) | VENDOR_MTK;
    case 0x04: return 0x00000006u;          /* command: MEM + bus master;
                                             * status: no capability list  */
    case 0x08: return 0x0C033000u;          /* class 0C, sub 03, prog-if 30 */
    case 0x0C: return 0x00000000u;          /* header type 0, single-function */
    case 0x10: return XHCI_BASE;            /* BAR0: 32-bit memory, non-pref */
    default:   return 0;
    }
}

unsigned int pci_cfg_read32(const pci_dev *d, int off)
{
    return cfg_read(d, off);
}

unsigned short pci_cfg_read16(const pci_dev *d, int off)
{
    return (unsigned short)(cfg_read(d, off) >> ((off & 2) * 8));
}

/* Writes are accepted and forgotten. The one thing a caller ever writes on
 * this path is the command register, and the block is already enabled. */
void pci_cfg_write32(const pci_dev *d, int off, unsigned int v)
{
    (void)d; (void)off; (void)v;
}

void pci_cfg_write16(const pci_dev *d, int off, unsigned short v)
{
    (void)d; (void)off; (void)v;
}

void pci_enable_bus_master(const pci_dev *d)
{
    (void)d;
}

unsigned long long pci_bar(const pci_dev *d, int n)
{
    return (is_ours(d) && n == 0) ? XHCI_BASE : 0;
}

int pci_find(unsigned short vendor, unsigned short device, pci_dev *out)
{
    if (!g_exposed || vendor != VENDOR_MTK || device != DEVICE_SOC || !out)
        return 0;
    out->bus = 0; out->dev = 0; out->fn = 0;
    out->vendor = VENDOR_MTK; out->device = DEVICE_SOC;
    return 1;
}

int pci_find_class(unsigned char cls, unsigned char sub, pci_dev *out)
{
    if (cls != 0x0C || sub != 0x03 || !out)
        return 0;                          /* no storage controllers on PCI
                                            * either: fat.c's question */
    return pci_find(VENDOR_MTK, DEVICE_SOC, out);
}

/* uefi_main.c's "rip the firmware's driver off this function" -- there is no
 * firmware driver to remove on an LK payload */
int uno_pc64_pci_disconnect(int bus, int dev, int fn)
{
    (void)bus; (void)dev; (void)fn;
    return 0;
}

/* usbboot.c's boot-path hint (which PCI function the boot volume hangs off):
 * no hint, so find_xhci() takes scan order, which finds the one function */
int uno_usbboot_hc_loc(int *dev, int *fn)
{
    (void)dev; (void)fn;
    return 0;
}
