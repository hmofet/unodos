/* ===========================================================================
 * UnoDOS/pc64 - PCI configuration-space access + device scan.
 *
 * The classic 0xCF8/0xCFC config mechanism (mechanism #1): write the target
 * (bus,dev,fn,offset) to the address port, read/write the data port. Enough to
 * find a NIC, read its BARs, and turn on bus-mastering - which is all the
 * e1000 driver needs. (ECAM/MMIO config via the ACPI MCFG table is the
 * eventual upgrade; port-CF8 works on every PC and every QEMU machine.)
 * ======================================================================== */
#include "pc64_pci.h"

static inline void outl(unsigned short port, unsigned int v)
{ __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned int inl(unsigned short port)
{ unsigned int v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outw_(unsigned short port, unsigned short v)
{ __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port)); }

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

static unsigned int cfg_addr(int bus, int dev, int fn, int off)
{
    return 0x80000000u | ((unsigned)bus << 16) | ((unsigned)dev << 11) |
           ((unsigned)fn << 8) | ((unsigned)off & 0xFC);
}

unsigned int pci_cfg_read32(const pci_dev *d, int off)
{
    outl(CFG_ADDR, cfg_addr(d->bus, d->dev, d->fn, off));
    return inl(CFG_DATA);
}

unsigned short pci_cfg_read16(const pci_dev *d, int off)
{
    return (unsigned short)(pci_cfg_read32(d, off) >> ((off & 2) * 8));
}

void pci_cfg_write32(const pci_dev *d, int off, unsigned int v)
{
    outl(CFG_ADDR, cfg_addr(d->bus, d->dev, d->fn, off));
    outl(CFG_DATA, v);
}

void pci_cfg_write16(const pci_dev *d, int off, unsigned short v)
{
    unsigned int cur = pci_cfg_read32(d, off);
    int sh = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << sh)) | ((unsigned)v << sh);
    pci_cfg_write32(d, off, cur);
}

/* enable memory-space decode + bus-mastering (command register bits 1,2) */
void pci_enable_bus_master(const pci_dev *d)
{
    unsigned short cmd = pci_cfg_read16(d, 0x04);
    cmd |= (1 << 1) | (1 << 2);
    pci_cfg_write16(d, 0x04, cmd);
}

/* BAR n as a memory address (masks the low flag bits; assumes a 32/64 mem BAR) */
unsigned long long pci_bar(const pci_dev *d, int n)
{
    unsigned int lo = pci_cfg_read32(d, 0x10 + n * 4);
    if (lo & 1) return lo & ~0x3u;              /* I/O BAR */
    if (((lo >> 1) & 3) == 2) {                 /* 64-bit memory BAR */
        unsigned int hi = pci_cfg_read32(d, 0x10 + (n + 1) * 4);
        return ((unsigned long long)hi << 32) | (lo & ~0xFu);
    }
    return lo & ~0xFu;                          /* 32-bit memory BAR */
}

/* find the first device matching vendor:device; returns 1 if found */
int pci_find(unsigned short vendor, unsigned short device, pci_dev *out)
{
    int bus, dev, fn;
    for (bus = 0; bus < 256; bus++)
        for (dev = 0; dev < 32; dev++)
            for (fn = 0; fn < 8; fn++) {
                pci_dev d; unsigned int id;
                d.bus = bus; d.dev = dev; d.fn = fn;
                id = pci_cfg_read32(&d, 0x00);
                if ((id & 0xFFFF) != vendor) continue;
                if ((unsigned short)(id >> 16) != device) continue;
                *out = d;
                return 1;
            }
    return 0;
}

/* find the first device of a given class/subclass (e.g. 0x03/0x00 = VGA);
   returns 1 and fills out (incl. vendor/device ids) if found */
int pci_find_class(unsigned char cls, unsigned char sub, pci_dev *out)
{
    int bus, dev, fn;
    for (bus = 0; bus < 256; bus++)
        for (dev = 0; dev < 32; dev++)
            for (fn = 0; fn < 8; fn++) {
                pci_dev d; unsigned int id, rev;
                d.bus = bus; d.dev = dev; d.fn = fn;
                id = pci_cfg_read32(&d, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;   /* no device */
                rev = pci_cfg_read32(&d, 0x08);
                if ((unsigned char)(rev >> 24) != cls) continue;
                if ((unsigned char)(rev >> 16) != sub) continue;
                d.vendor = (unsigned short)id;
                d.device = (unsigned short)(id >> 16);
                *out = d;
                return 1;
            }
    return 0;
}
