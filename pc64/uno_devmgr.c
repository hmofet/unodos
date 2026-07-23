/* unodevices - Phase 1: PCI enumeration + text introspection.
 *
 * Owns discovery; CONSUMES pc64_pci.c's config accessors (pci_cfg_read32) and
 * does not edit that shared file.  A flat 0..255/0..31/0..7 walk (the same shape
 * xhci.c uses) - firmware assigns bus numbers at POST, so a flat scan sees every
 * enumerated bus incl. anything behind bridges (e.g. the ZimaBlade PCIe slot).
 * See DEVICES.md. */
#include "pc64_pci.h"
#include "uno_devmgr.h"

/* --- tiny NUL-terminated string builders (bounds-checked to cap-1) --------- */
static int s_cat(char *b, int cap, int at, const char *s) {
    while (*s && at < cap - 1) b[at++] = *s++;
    b[at] = 0;
    return at;
}
static int s_hex(char *b, int cap, int at, unsigned v, int digits) {
    static const char H[] = "0123456789abcdef";
    char t[9];
    int i;
    if (digits > 8) digits = 8;
    for (i = digits - 1; i >= 0; i--) { t[i] = H[v & 0xF]; v >>= 4; }
    t[digits] = 0;
    return s_cat(b, cap, at, t);
}
static int s_dec1(char *b, int cap, int at, unsigned v) {   /* single 0-9 digit */
    if (at < cap - 1) b[at++] = (char)('0' + (v % 10));
    b[at] = 0;
    return at;
}

/* PCI base-class -> short name (subclass disambiguates a couple). */
static const char *class_name(unsigned char cls, unsigned char sub) {
    switch (cls) {
    case 0x00: return "unclassified";
    case 0x01: return "storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x05: return "memory";
    case 0x06: return "bridge";
    case 0x07: return "comm";
    case 0x08: return "system";
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
    default:   return "other";
    }
}

static int emit_fn(char *buf, int cap, int at, int bus, int dev, int fn) {
    pci_dev d;
    unsigned id, cr;
    unsigned char cls, sub;
    d.bus = bus; d.dev = dev; d.fn = fn;
    id = pci_cfg_read32(&d, 0x00);
    if ((id & 0xFFFF) == 0xFFFF) return at;             /* no device */
    cr  = pci_cfg_read32(&d, 0x08);                     /* rev + class triple */
    sub = (unsigned char)((cr >> 16) & 0xFF);
    cls = (unsigned char)((cr >> 24) & 0xFF);

    at = s_hex(buf, cap, at, (unsigned)bus, 2);
    at = s_cat(buf, cap, at, ":");
    at = s_hex(buf, cap, at, (unsigned)dev, 2);
    at = s_cat(buf, cap, at, ".");
    at = s_dec1(buf, cap, at, (unsigned)fn);
    at = s_cat(buf, cap, at, " ");
    at = s_hex(buf, cap, at, id & 0xFFFF, 4);
    at = s_cat(buf, cap, at, ":");
    at = s_hex(buf, cap, at, (id >> 16) & 0xFFFF, 4);
    at = s_cat(buf, cap, at, " ");
    at = s_hex(buf, cap, at, cls, 2);
    at = s_cat(buf, cap, at, "/");
    at = s_hex(buf, cap, at, sub, 2);
    at = s_cat(buf, cap, at, " ");
    at = s_cat(buf, cap, at, class_name(cls, sub));
    at = s_cat(buf, cap, at, "\n");
    return at;
}

int devmgr_list_str(char *buf, int cap) {
    int bus, dev, fn, at = 0;
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            pci_dev d0;
            unsigned char hdr;
            int nfn;
            d0.bus = bus; d0.dev = dev; d0.fn = 0;
            if ((pci_cfg_read32(&d0, 0x00) & 0xFFFF) == 0xFFFF) continue;
            hdr = (unsigned char)((pci_cfg_read32(&d0, 0x0C) >> 16) & 0xFF);
            nfn = (hdr & 0x80) ? 8 : 1;                 /* multi-function? */
            for (fn = 0; fn < nfn; fn++)
                at = emit_fn(buf, cap, at, bus, dev, fn);
            if (at >= cap - 1) return at;               /* buffer full */
        }
    }
    return at;
}
