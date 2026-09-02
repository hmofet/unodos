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

/* TWO FETCHES, HEADER THEN EXACT LENGTH -- what the USB core does, and on
 * this controller not optional. usbhid.c asks for the config descriptor with
 * a 256-byte request, and on the MediaTek xHCI a 256-byte control IN on the
 * hub "completes" in 0 ms with the buffer untouched (third M4 boot, measured
 * at 256/64/25/9 bytes: the three shorter requests all return the real
 * descriptor, 64 being a short packet too, so it is the 256 itself). An
 * all-zero descriptor then made usbhid issue SET_CONFIGURATION(0) on the
 * HUB, which unconfigures it and drops every device behind it -- which is why
 * the receivers, enumerated fine a moment earlier, timed out on everything
 * afterwards. So: 9 bytes for the header, then exactly wTotalLength, and a
 * header that does not parse is a failure here, never a zero handed up. */
int c64_usb_get_config(int dev, void *buf, int len)
{
    int n, i, total;
    if (len > (int)sizeof g_bounce)
        len = (int)sizeof g_bounce;
    n = uno_usb_get_config(dev, g_bounce, 9);
    if (n < 9 || g_bounce[0] != 9 || g_bounce[1] != 0x02) {
        c64_logf("usb: get_config dev %d header -> %d bytes (%02x %02x), "
                 "refusing\n", dev, n, g_bounce[0], g_bounce[1]);
        return -1;
    }
    total = g_bounce[2] | (g_bounce[3] << 8);
    if (total < 9)
        return -1;
    if (total > len)
        total = len;
    if (total > 9) {
        n = uno_usb_get_config(dev, g_bounce, total);
        if (n < total) {
            c64_logf("usb: get_config dev %d body (%d bytes) -> %d, refusing\n",
                     dev, total, n);
            return -1;
        }
    }
    n = total;
    for (i = 0; i < n && i < len; i++)
        ((unsigned char *)buf)[i] = g_bounce[i];
    /* say what came back: the interfaces and endpoints usbhid.c is about to
     * walk, so a claim that finds nothing can be read against the descriptor
     * it was shown */
    if (n < 9) {
        c64_logf("usb: get_config dev %d -> %d bytes (no descriptor)\n", dev, n);
        return n;
    }
    {
        int total = g_bounce[2] | (g_bounce[3] << 8), at = 0;
        if (total > n) total = n;
        c64_logf("usb: get_config dev %d -> %d bytes, wTotalLength %d, "
                 "config %d\n", dev, n, total, g_bounce[5]);
        while (at + 2 <= total && g_bounce[at] >= 2) {
            int bl = g_bounce[at], bt = g_bounce[at + 1];
            if (at + bl > total) break;
            if (bt == 0x04 && bl >= 9)
                c64_logf("    iface %d: class %02x/%02x/%02x\n", g_bounce[at + 2],
                         g_bounce[at + 5], g_bounce[at + 6], g_bounce[at + 7]);
            else if (bt == 0x05 && bl >= 7)
                c64_logf("      ep %02x attr %02x mps %d interval %d\n",
                         g_bounce[at + 2], g_bounce[at + 3],
                         g_bounce[at + 4] | (g_bounce[at + 5] << 8),
                         g_bounce[at + 6]);
            at += bl;
        }
    }
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

/* Interposed the same way, for visibility only: usbhid.c claims an endpoint
 * through uno_usb_setup_intr_in() and says nothing when that fails, and the
 * first hardware boot of M4 enumerated four devices and claimed zero
 * endpoints without a word about why. */
int c64_usb_setup_intr_in(int dev, int in_addr, int mps)
{
    int h = uno_usb_setup_intr_in(dev, in_addr, mps);
    c64_logf("usb: setup_intr_in dev %d ep %02x mps %d -> %s%d\n", dev,
             in_addr, mps, h < 0 ? "FAILED " : "handle ", h);
    return h;
}

/* ---- the MediaTek endpoint-context words -----------------------------------
 * Called by xhci.c on every endpoint context it builds, before Configure
 * Endpoint (uno_xhci_set_ep_quirk). MediaTek's SSUSB host keeps a bandwidth
 * schedule of its own and reads it from the endpoint context's reserved
 * words: DW5 = BPKTS[5:0] | BCSCOUNT[10:8] | BBM[11], DW6 = BOFFSET[13:0] |
 * BREPEAT[30:16]. Without them a periodic endpoint is never serviced. The
 * values are the vendor driver's for a full/low-speed periodic endpoint
 * behind a transaction translator (both receivers on this hub): one packet
 * per microframe, up to three complete-splits, no burst, offset 0, no
 * repeat; and BPKTS=1 alone for one on a root port. Bulk and control
 * endpoints need nothing.
 *
 * The same pass writes the INTERVAL field (DW0[23:16]), which setup_ep()
 * leaves at 0. That is "every microframe", outside the 3..18 the spec allows
 * a full-speed interrupt endpoint, and a value a strict controller rejects
 * at Configure Endpoint -- and one the vendor scheduler cannot fit its five-
 * microframe budget into. 3 (= 8 microframes = 1 ms) is the fastest legal
 * full-speed interval; 7 (16 ms) for low speed, which cannot poll faster.
 * (Fact sources: the Gemian kernel's xhci-mtk-sch.c; the xHCI spec 6.2.3.) */
static void mtk_ep_quirk(unsigned char *epctx, int eptype, int speed, int has_tt)
{
    volatile unsigned int *dw = (volatile unsigned int *)epctx;
    int periodic = (eptype == 7 || eptype == 3 ||       /* interrupt in/out */
                    eptype == 5 || eptype == 1);        /* isoch in/out     */
    if (!periodic)
        return;
    if (speed == 1 || speed == 2) {                     /* FS, LS */
        dw[0] = (dw[0] & ~(0xFFu << 16)) | ((speed == 2 ? 7u : 3u) << 16);
        if (has_tt) {
            dw[5] |= 1u | (3u << 8);                    /* BPKTS 1, BCSCOUNT 3 */
            dw[6] |= 0;                                 /* BOFFSET 0, BREPEAT 0 */
        } else {
            dw[5] |= 1u;                                /* BPKTS 1 */
        }
    } else {                                            /* HS, SS */
        if (!((dw[0] >> 16) & 0xFF))
            dw[0] |= 3u << 16;                          /* 1 ms, if unset */
        dw[5] |= 1u;                                    /* BPKTS 1, no burst */
    }
    __asm__ volatile("dsb sy" ::: "memory");
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
    uno_xhci_set_ep_quirk(mtk_ep_quirk);
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
