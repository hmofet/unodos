/* cosmo64/usb.c -- USB HID as the shell's second pointer and keyboard (M4).
 *
 * The stack, bottom to top: ssusb.c brings MediaTek's host block out of reset
 * and puts the PHY in host mode; pc64's xhci.c then does everything the xHCI
 * specification describes (rings, slots, port reset, Address Device, the hub
 * walk), discovering its controller through the PCI shim in pci.c; pc64's
 * usbhid.c claims every boot-protocol keyboard and mouse it enumerated; and
 * this file feeds what they report into input.c's ring and pointer, the way
 * uefi_main.c does on x86 -- which is the file cosmo64 replaces, and the
 * third time now that something living there had to be re-implemented here
 * (the cursor and the dirty-row present were the first two).
 *
 * THE BOUNCE. control_xfer() in xhci.c DMAs straight into whatever buffer its
 * caller passes, and usbhid.c passes a buffer on ITS STACK for the config
 * descriptor. The stack is write-back cached memory, and the controller is
 * not coherent with the cache on this SoC, so that read would come back as
 * whatever the cache already held. build.sh compiles usbhid.c with its two
 * calls renamed (-Duno_usb_get_config=c64_usb_get_config, likewise
 * uno_usb_control), and the versions below run the transfer through a buffer
 * that lives in the same Device-mapped .xdma section as xhci.c's own DMA
 * memory, then copy out. usbhid.c is unchanged; the redirection is a compile
 * flag in this lane's own build script.
 */

#include "cosmo64.h"
#include "xhci.h"
#include "usbhid.h"

/* The whole DMA arena is ONE section: xhci.c's variables land in it through
 * the force-included pragma, and this buffer through the same pragma rather
 * than a section attribute -- an attribute makes clang emit an initialised
 * (DATA) section of that name, which the linker keeps apart from the BSS one,
 * and a second .xdma placed elsewhere is a buffer the MMU map would miss. */
#pragma clang section bss = ".xdma"
static unsigned char g_bounce[512] __attribute__((aligned(64)));
#pragma clang section bss = ""

static int g_up, g_nkbd, g_nmouse;

int c64_usb_get_config(int dev, void *buf, int len)
{
    int n, i;
    if (len > (int)sizeof g_bounce)
        len = (int)sizeof g_bounce;
    n = uno_usb_get_config(dev, g_bounce, len);
    for (i = 0; i < n && i < len; i++)
        ((unsigned char *)buf)[i] = g_bounce[i];
    return n;
}

int c64_usb_control(int dev, unsigned char rt, unsigned char req,
                    unsigned short val, unsigned short idx, void *data, int len)
{
    int n, i;
    if (len > (int)sizeof g_bounce)
        len = (int)sizeof g_bounce;
    if (data && len && !(rt & 0x80))                 /* OUT: stage the payload */
        for (i = 0; i < len; i++)
            g_bounce[i] = ((const unsigned char *)data)[i];
    n = uno_usb_control(dev, rt, req, val, idx, len ? g_bounce : 0, len);
    if (data && len && (rt & 0x80))                  /* IN: copy the answer out */
        for (i = 0; i < n && i < len; i++)
            ((unsigned char *)data)[i] = g_bounce[i];
    return n;
}

/* usbhid's keyboard callback: (scan, uni, ctrl, mods) in the same space the
 * AW9523 driver already emits -- EFI scan codes, Unicode with shift applied,
 * UI_MOD_* bits -- so it drops straight into the ring */
static void key_emit(int scan, int uni, int ctrl, int mods, void *ctx)
{
    (void)ctrl; (void)ctx;
    c64_key_push(scan, uni, mods);
}

void c64_usb_init(void)
{
    int present, nports, ndevs;
    unsigned err;

    if (!c64_ssusb_present()) {
        c64_log("usb: no SSUSB block answers -- no USB on this board\n");
        return;
    }
    if (!c64_ssusb_host_up())
        return;
    c64_pci_expose_xhci(1);            /* now there is a function to find */
    /* A 64-bit register access, before xhci.c makes hundreds of them: it
     * writes DCBAAP, CRCR, ERSTBA and ERDP as 64-bit stores. If this bus
     * splits or rejects them the fault handler says so here, next to a line
     * that names what was tried. */
    {
        volatile unsigned long long *dcbaap =
            (volatile unsigned long long *)(0x11200000ull + 0x20 + 0x30);
        c64_logf("usb: 64-bit MMIO read of DCBAAP = %016x\n", *dcbaap);
    }
    c64_log_flush();

    uno_usb_hid_init();
    uno_xhci_status(&present, &nports, &ndevs, &err);
    uno_usb_hid_status(&g_nkbd, &g_nmouse);
    g_up = present;
    c64_logf("usb: xhci %s, %d connected port(s), %d device(s), err=%u; HID "
             "%d keyboard(s), %d mouse/mice\n", present ? "UP" : "DOWN",
             nports, ndevs, err, g_nkbd, g_nmouse);
    {
        int i;
        for (i = 0; i < ndevs; i++) {
            const uno_usb_dev *d = uno_xhci_dev(i);
            if (d)
                c64_logf("  usb dev %d: %04x:%04x class %02x/%02x/%02x port %d "
                         "speed %d slot %d\n", i, d->vendor, d->product,
                         d->dev_class, d->dev_subclass, d->dev_proto,
                         d->port, d->speed, d->slot);
        }
    }
    c64_log_flush();
}

int c64_usb_mice(void)
{
    return g_up ? g_nmouse : 0;
}

void c64_usb_poll(void)
{
    int dx = 0, dy = 0, btn = 0, w;
    if (!g_up)
        return;
    if (g_nmouse && uno_usb_hid_mouse_poll(&dx, &dy, &btn))
        c64_input_move_pointer(dx, dy, btn);
    w = uno_usb_hid_wheel();
    if (w)
        c64_input_add_wheel(w);
    if (g_nkbd) {
        uno_usb_hid_kbd_poll(key_emit, 0);
        c64_input_set_level_usb(uno_usb_hid_mods(), uno_usb_hid_keys_held());
    }
}
