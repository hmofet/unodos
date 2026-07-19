/* ===========================================================================
 * UnoDOS/pc64 - USB HID boot-protocol driver (see usbhid.h).
 * ======================================================================== */
#include "usbhid.h"

#ifndef UNO_XHCI

int  uno_usb_hid_init(void) { return 0; }
int  uno_usb_hid_kbd_poll(uno_usb_key_fn e, void *c) { (void)e;(void)c; return 0; }
int  uno_usb_hid_mouse_poll(int *dx, int *dy, int *b) { (void)dx;(void)dy;(void)b; return 0; }
int  uno_usb_hid_present(void)     { return 0; }
int  uno_usb_hid_kbd_present(void) { return 0; }
void uno_usb_hid_status(int *nk, int *nm) { if (nk)*nk=0; if (nm)*nm=0; }

#else  /* ===================== UNO_XHCI enabled ========================= */

#include "xhci.h"
#include "hid_kbd.h"

typedef unsigned char u8;

#ifdef UNO_DBGCON
static inline void uh_ob(unsigned short p, unsigned char v)
{ __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static void uh_dbg(const char *s) { while (*s) uh_ob(0x402, (unsigned char)*s++); }
static void uh_dbgn(int n) { char b[12]; int i=0,j; if(n<0){uh_ob(0x402,'-');n=-n;}
    if(!n){uh_ob(0x402,'0');return;} while(n){b[i++]='0'+n%10;n/=10;} for(j=i-1;j>=0;j--)uh_ob(0x402,b[j]); }
#else
static void uh_dbg(const char *s) { (void)s; }
static void uh_dbgn(int n) { (void)n; }
#endif

/* one claimed HID interrupt endpoint */
typedef struct {
    int  dev;                  /* index into the xHCI device list */
    int  is_kbd;               /* 1 keyboard, 0 mouse             */
    hid_kbd_state kbd;         /* keyboard edge state             */
} hidep;

#define MAX_HIDEP 8
static hidep g_eps[MAX_HIDEP];
static int   g_neps, g_nkbd, g_nmouse;
static int   g_inited;

/* SET_IDLE(0) + SET_PROTOCOL(boot) on an interface (class requests) */
static void set_boot(int dev, int iface)
{
    uno_usb_control(dev, 0x21, 0x0A, 0x0000, (unsigned short)iface, 0, 0);  /* SET_IDLE 0 */
    uno_usb_control(dev, 0x21, 0x0B, 0x0000, (unsigned short)iface, 0, 0);  /* SET_PROTOCOL boot */
}

/* Walk one device's config descriptor: for each HID boot interface, put it in
 * boot protocol and claim its interrupt-IN endpoint. */
static void claim_device(int dev)
{
    u8 cfg[256];
    int total, i, cfgval;
    int cur_class = -1, cur_sub = 0, cur_proto = 0, cur_iface = 0, want = 0;
    if (uno_usb_get_config(dev, cfg, sizeof cfg) < 9) return;
    total = cfg[2] | (cfg[3] << 8);
    if (total > (int)sizeof cfg) total = sizeof cfg;
    cfgval = cfg[5];                                   /* bConfigurationValue */
    if (uno_usb_set_config(dev, cfgval) < 0) return;

    i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2) break;
        if (i + blen > total) break;
        if (btype == 0x04 && blen >= 9) {              /* INTERFACE */
            cur_iface = cfg[i + 2];
            cur_class = cfg[i + 5]; cur_sub = cfg[i + 6]; cur_proto = cfg[i + 7];
            want = (cur_class == 0x03 && (cur_proto == 1 || cur_proto == 2));
        } else if (btype == 0x05 && blen >= 7) {       /* ENDPOINT */
            int addr = cfg[i + 2], attr = cfg[i + 3];
            int mps = cfg[i + 4] | (cfg[i + 5] << 8);
            (void)cur_sub;
            if (want && (attr & 0x03) == 0x03 && (addr & 0x80) && g_neps < MAX_HIDEP) {
                if (uno_usb_setup_intr_in(dev, addr, mps) == 0) {
                    hidep *e = &g_eps[g_neps++];
                    e->dev = dev; e->is_kbd = (cur_proto == 1);
                    hid_kbd_reset(&e->kbd);
                    set_boot(dev, cur_iface);
                    if (e->is_kbd) g_nkbd++; else g_nmouse++;
                    want = 0;                          /* one EP per interface */
                }
            }
        }
        i += blen;
    }
}

int uno_usb_hid_init(void)
{
    int n, i;
    if (g_inited) return g_neps;
    g_inited = 1;
    if (!uno_xhci_init()) { uh_dbg("usbhid: no xhci\n"); return 0; }
    n = uno_xhci_dev_count();
    uh_dbg("usbhid: xhci up, devs="); uh_dbgn(n); uh_dbg("\n");
    for (i = 0; i < n && g_neps < MAX_HIDEP; i++) {
        const uno_usb_dev *d = uno_xhci_dev(i);
        if (!d) continue;
        claim_device(i);
    }
    uh_dbg("usbhid: claimed kbd="); uh_dbgn(g_nkbd);
    uh_dbg(" mouse="); uh_dbgn(g_nmouse); uh_dbg("\n");
    return g_neps;
}

int uno_usb_hid_kbd_poll(uno_usb_key_fn emit, void *ctx)
{
    int i, any = 0;
    for (i = 0; i < g_neps; i++) {
        u8 rep[16];
        int n;
        if (!g_eps[i].is_kbd) continue;
        any = 1;
        n = uno_usb_intr_in(g_eps[i].dev, rep, sizeof rep);
        if (n >= 8)                                    /* boot kbd report = 8 B */
            hid_kbd_report(&g_eps[i].kbd, rep, (hid_key_fn)emit, ctx);
    }
    return any;
}

int uno_usb_hid_mouse_poll(int *dx, int *dy, int *btn)
{
    int i, any = 0, ax = 0, ay = 0, ab = 0;
    for (i = 0; i < g_neps; i++) {
        u8 rep[16];
        int n;
        if (g_eps[i].is_kbd) continue;
        any = 1;
        n = uno_usb_intr_in(g_eps[i].dev, rep, sizeof rep);
        if (n >= 3) {                                  /* boot mouse: btn,dx,dy */
            ab |= rep[0] & 0x07;
            ax += (signed char)rep[1];
            ay += (signed char)rep[2];
        }
    }
    if (!any) return 0;
    *dx = ax; *dy = ay; *btn = ab;
    return 1;
}

int uno_usb_hid_present(void)     { return g_neps > 0; }
int uno_usb_hid_kbd_present(void) { return g_nkbd > 0; }
void uno_usb_hid_status(int *nk, int *nm) { if (nk)*nk=g_nkbd; if (nm)*nm=g_nmouse; }

#endif /* UNO_XHCI */
