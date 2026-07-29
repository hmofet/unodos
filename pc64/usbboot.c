/* USB boot-volume detach preflight - see usbboot.h. */
#include "usbboot.h"
#include "usbio.h"
#include "xhci.h"
#include "pc64_pci.h"
#include "uno_debug.h"

void *uno_pc64_boot_dp(void);       /* boot partition's device path (uefi_main) */
int   uno_pc64_detached(void);

/* ---- device-path walking --------------------------------------------------
 * An EFI_DEVICE_PATH node is {u8 Type; u8 SubType; u8 Length[2];} + payload,
 * terminated by an END node (type 0x7F). Nothing here dereferences firmware
 * protocols, so it is safe to run at any point before ExitBootServices. */
#define DP_END      0x7F
#define DP_HW       0x01
#define DP_HW_PCI   0x01
#define DP_MSG      0x03
#define DP_MSG_USB  0x05

static int dp_nodelen(const unsigned char *p) { return p[2] | (p[3] << 8); }

/* total bytes of a path INCLUDING its end node, or 0 if it is malformed */
static int dp_bytelen(const unsigned char *p)
{
    int n = 0, guard = 64;
    if (!p) return 0;
    while (p[n] != DP_END) {
        int l = dp_nodelen(p + n);
        if (l < 4 || --guard < 0) return 0;
        n += l;
    }
    return n + dp_nodelen(p + n);
}

/* is `pre` a whole-node prefix of `full`?  A USB interface's path is exactly
 * the boot partition's path minus the trailing HD() node, so this is the test
 * for "this UsbIo handle is the device the system booted from". */
static int dp_is_prefix(const unsigned char *pre, const unsigned char *full)
{
    int n = 0, guard = 64;
    if (!pre || !full) return 0;
    while (pre[n] != DP_END) {                  /* compare node by node */
        int l = dp_nodelen(pre + n), i;
        if (l < 4 || --guard < 0) return 0;
        if (full[n] == DP_END) return 0;        /* full ran out first   */
        for (i = 0; i < l; i++)
            if (pre[n + i] != full[n + i]) return 0;
        n += l;
    }
    return n > 0;
}

static int dp_has_usb(const unsigned char *p)
{
    int n = 0, guard = 64;
    if (!p) return 0;
    while (p[n] != DP_END) {
        int l = dp_nodelen(p + n);
        if (l < 4 || --guard < 0) return 0;
        if (p[n] == DP_MSG && p[n + 1] == DP_MSG_USB) return 1;
        n += l;
    }
    return 0;
}

/* the first PCI() node of a path = the host controller the device hangs off */
static int dp_pci(const unsigned char *p, int *dev, int *fn)
{
    int n = 0, guard = 64;
    if (!p) return 0;
    while (p[n] != DP_END) {
        int l = dp_nodelen(p + n);
        if (l < 4 || --guard < 0) return 0;
        if (p[n] == DP_HW && p[n + 1] == DP_HW_PCI && l >= 6) {
            *fn = p[n + 4]; *dev = p[n + 5];
            return 1;
        }
        n += l;
    }
    return 0;
}

/* ---- an xHCI function at this PCI location? -------------------------------
 * usbmsc.c rides xhci.c and nothing else, so a stick on a UHCI/EHCI companion
 * (or on a machine whose only USB is pre-3.0) cannot be reclaimed - and that
 * machine must keep its firmware. Class 0x0C / subclass 0x03 / prog-if 0x30 is
 * xHCI; the same triple with prog-if 0x20 is EHCI, which is why the prog-if
 * check is not optional. */
static int xhci_at(int dev, int fn)
{
    int bus;
    for (bus = 0; bus < 256; bus++) {
        pci_dev d; unsigned int id, rev;
        d.bus = bus; d.dev = dev; d.fn = fn;
        id = pci_cfg_read32(&d, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        rev = pci_cfg_read32(&d, 0x08);
        if ((unsigned char)(rev >> 24) == 0x0C &&
            (unsigned char)(rev >> 16) == 0x03 &&
            (unsigned char)(rev >>  8) == 0x30)
            return 1;
    }
    return 0;
}

/* ---- the verdict ---------------------------------------------------------- */
static int   g_done, g_is_usb, g_ok, g_nbot, g_matched;
static unsigned short g_vid, g_pid;     /* the boot stick's USB id */
static const char *g_why = "not evaluated";

static void evaluate(void)
{
    const unsigned char *boot = (const unsigned char *)uno_pc64_boot_dp();
    int cdev = -1, cfn = -1, n, i;

    g_done = 1;
    g_why  = "boot volume is not USB";
    if (!boot || !dp_bytelen(boot)) { g_why = "no boot device path"; return; }
    if (!dp_has_usb(boot)) return;
    g_is_usb = 1;

    if (!uno_xhci_supported()) { g_why = "no native USB stack in this build"; return; }
    if (!dp_pci(boot, &cdev, &cfn) || !xhci_at(cdev, cfn)) {
        g_why = "USB boot controller is not xHCI";
        return;
    }

    /* Which USB interface is the stick?  Prefer the one whose device path the
     * boot path extends - that is the boot device by construction. Some
     * firmwares hand out UsbIo paths that do not line up node-for-node with
     * the Block IO path; when nothing matches but the machine has exactly ONE
     * BOT interface, that one is unambiguously it. Two unmatched candidates is
     * a guess, and a guess here costs the machine, so it stays attached. */
    n = uno_usbio_count();
    for (i = 0; i < n; i++) {
        unsigned char cls = 0, sub = 0, proto = 0;
        unsigned short vid = 0, pid = 0;
        int in_ep = 0, out_ep = 0;
        void *dp = 0;
        if (uno_usbio_iface(i, &cls, &sub, &proto) < 0) continue;
        if (cls != 0x08 || sub != 0x06 || proto != 0x50) continue;   /* SCSI/BOT */
        if (uno_usbio_bulk_eps(i, &in_ep, &out_ep) < 0) continue;    /* needs both */
        g_nbot++;
        uno_usbio_info(i, &vid, &pid, 0, 0);
        if (g_nbot == 1) { g_vid = vid; g_pid = pid; }   /* the sole candidate */
        if (!g_matched && uno_usbio_devpath(i, &dp) == 0 &&
            dp_is_prefix((const unsigned char *)dp, boot)) {
            g_matched = 1;
            g_vid = vid; g_pid = pid;                    /* ...or the right one */
        }
    }

    if (g_matched)        { g_ok = 1; g_why = "boot stick is BOT/xHCI (path matched)"; }
    else if (g_nbot == 1) { g_ok = 1; g_why = "the one BOT device is the boot stick"; }
    else if (g_nbot == 0) g_why = "boot device is not a BOT mass-storage interface";
    else                  g_why = "several BOT devices, none matched the boot path";
    if (!g_ok) { g_vid = 0; g_pid = 0; }
}

static void ensure(void)
{
    /* Only computable while the firmware is alive; the answer is latched then
     * and read back afterwards (the detach path itself is a reader). */
    if (!g_done && !uno_pc64_detached()) {
        evaluate();
        uno_dbg_log("usbboot: usb=%d bot=%d matched=%d ok=%d (%s)",
                    g_is_usb, g_nbot, g_matched, g_ok, g_why);
    }
}

int uno_usbboot_is_usb(void)    { ensure(); return g_is_usb; }
int uno_usbboot_native_ok(void) { ensure(); return g_ok; }

int uno_usbboot_target(unsigned short *vid, unsigned short *pid)
{
    ensure();
    if (vid) *vid = g_vid;
    if (pid) *pid = g_pid;
    return g_ok;
}

void uno_usbboot_status(int *is_usb, int *nbot, int *matched, const char **why)
{
    ensure();
    if (is_usb)  *is_usb  = g_is_usb;
    if (nbot)    *nbot    = g_nbot;
    if (matched) *matched = g_matched;
    if (why)     *why     = g_why;
}
