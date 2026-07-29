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

/* How many USB tiers deep is this device?  One USB() node per hub level, so a
 * device plugged straight into a root-hub port has exactly one.
 *
 * xhci.c walks hubs now, so depth is a RANGE check rather than a refusal: the
 * xHCI route string is five 4-bit nibbles, so five hubs is the architectural
 * limit and anything past it cannot be addressed at all. (Before the hub
 * driver landed this had to be depth == 1, which is what kept the ZimaBlade
 * attached - its keyboard AND its boot stick are both behind one hub, because
 * the machine has a single USB port.) Returns 0 for a malformed path. */
/* xhci.c's route string holds five hub tiers; depth counts the device's own
 * USB() node too, so 1 (root port) .. 6 (five hubs up) is addressable. */
#define USB_MAX_DEPTH 6
static int usb_depth_ok(int d) { return d >= 1 && d <= USB_MAX_DEPTH; }

static int dp_usb_depth(const unsigned char *p)
{
    int n = 0, guard = 64, depth = 0;
    if (!p) return 0;
    while (p[n] != DP_END) {
        int l = dp_nodelen(p + n);
        if (l < 4 || --guard < 0) return 0;
        if (p[n] == DP_MSG && p[n + 1] == DP_MSG_USB) depth++;
        n += l;
    }
    return depth;
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
static int   g_done, g_is_usb, g_ok, g_nbot, g_matched, g_deep;
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
        if (uno_usbio_devpath(i, &dp) == 0 &&
            !usb_depth_ok(dp_usb_depth((const unsigned char *)dp))) {
            g_deep++;                        /* deeper than we can address */
            continue;
        }
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
    else if (g_nbot == 0) g_why = g_deep ? "boot stick is too many hubs deep to address"
                                        : "boot device is not a BOT mass-storage interface";
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

/* ===========================================================================
 * ...and the same question for INPUT.
 *
 * The detach gate refuses to leave the firmware without a native keyboard,
 * because the shell is keyboard-driven and firmware ConIn dies with EBS. That
 * is right, but as written it was unsatisfiable on a machine whose only
 * keyboard is USB: `uno_usb_hid_kbd_present()` can only be true once xhci.c
 * owns the controller, and xhci.c only takes the controller after EBS. So we
 * would not leave the firmware until the keyboard existed, and the keyboard
 * could not exist until we left the firmware - and every desktop with USB-only
 * input (the ZimaBlade, most desktops) was permanently attached.
 *
 * The way out is the one usbmsc already uses for the boot volume: ask the
 * FIRMWARE's descriptors what the native stack will be able to claim. A HID
 * interface with the boot subclass, on a root-hub port of an xHCI controller,
 * is exactly what uno_usb_hid_init() claims at detach.
 *
 * Both conditions are load-bearing. Boot subclass (03/01) because the native
 * driver speaks boot protocol and nothing else; root-hub port because xhci.c
 * enumerates root ports only, so a keyboard behind a hub is one we can see and
 * cannot have.
 * ======================================================================== */
static int g_hid_done, g_hid_kbd, g_hid_ptr;
static const char *g_hid_why = "not evaluated";

static void hid_evaluate(void)
{
    int n, i, deep = 0;
    g_hid_done = 1;
    if (!uno_xhci_supported()) { g_hid_why = "no native USB stack in this build"; return; }
    n = uno_usbio_count();
    for (i = 0; i < n; i++) {
        unsigned char cls = 0, sub = 0, proto = 0;
        int cdev = -1, cfn = -1;
        void *dp = 0;
        if (uno_usbio_iface(i, &cls, &sub, &proto) < 0) continue;
        if (cls != 0x03 || sub != 0x01) continue;      /* HID, boot subclass    */
        if (uno_usbio_devpath(i, &dp) < 0) continue;
        if (!usb_depth_ok(dp_usb_depth((const unsigned char *)dp))) { deep++; continue; }
        if (!dp_pci((const unsigned char *)dp, &cdev, &cfn) || !xhci_at(cdev, cfn))
            continue;                                  /* not on an xHCI        */
        if (proto == 1) g_hid_kbd = 1;                 /* boot keyboard         */
        if (proto == 2) g_hid_ptr = 1;                 /* boot mouse            */
    }
    g_hid_why = g_hid_kbd ? "USB boot keyboard reachable on the xHCI"
              : deep      ? "USB keyboard is too many hubs deep to address"
                          : "no USB boot keyboard the native stack can reach";
}

static void hid_ensure(void)
{
    if (!g_hid_done && !uno_pc64_detached()) {
        hid_evaluate();
        uno_dbg_log("usbboot: hid kbd=%d ptr=%d (%s)", g_hid_kbd, g_hid_ptr, g_hid_why);
    }
}

int uno_usbboot_hid_kbd(void) { hid_ensure(); return g_hid_kbd; }
int uno_usbboot_hid_ptr(void) { hid_ensure(); return g_hid_ptr; }
void uno_usbboot_hid_status(int *kbd, int *ptr, const char **why)
{
    hid_ensure();
    if (kbd) *kbd = g_hid_kbd;
    if (ptr) *ptr = g_hid_ptr;
    if (why) *why = g_hid_why;
}

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
