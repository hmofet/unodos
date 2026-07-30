/* detachgate - the input half of the pre-EBS detach decision, and the blocker
 * attribution behind it.  See detachgate.h and pc64/DETACH.md.
 *
 * Nothing here dereferences a firmware PROTOCOL: it reads handles' device
 * paths, which are plain descriptor bytes, so it is safe at any point before
 * ExitBootServices and its verdict is latched for readers afterwards.
 */
#include "detachgate.h"
#include "uefi.h"
#include "pc64_native.h"
#include "i2c_hid.h"
#include "usbhid.h"
#include "usbboot.h"

void *uno_pc64_st(void);              /* EFI_SYSTEM_TABLE   (uefi_main.c) */
int   uno_pc64_detached(void);

/* ---- unodevices, consumed through a weak seam -----------------------------
 * The device manager answers "which PCI function is this, in the operator's
 * language" for the blocker string.  It is declared locally rather than via
 * uno_devmgr.h, and backed by weak fallbacks, so this file links in builds
 * that do not compile uno_devmgr.c (the legacy core list) and upgrades itself
 * the moment the real definitions are in the link - the r8169_dbg_cmd idiom
 * AGENTS.md §2 asks for.
 *
 * NOTE the seam this does NOT cross.  "Will a native driver own this device
 * after detach?" is answered here by asking the service owners (ps2, i2c_hid,
 * usbhid, usbboot), not by reading a bind state out of the registry, because
 * the driver registry is unodevices phase 2 and is not on master.  When it
 * lands, those predicates become a registry lookup and this comment goes with
 * them; the shape of the gate does not change.  See DETACH.md §4. */
int devmgr_count(void);
int devmgr_info(int idx, unsigned int *out, int nmax);
const char *devmgr_class_name(unsigned char cls, unsigned char sub);
__attribute__((weak)) int devmgr_count(void) { return 0; }
__attribute__((weak)) int devmgr_info(int idx, unsigned int *out, int nmax)
{ (void)idx; (void)out; (void)nmax; return -1; }
__attribute__((weak)) const char *devmgr_class_name(unsigned char c, unsigned char s)
{ (void)c; (void)s; return "?"; }
#define DG_ROW_N 15                   /* mirrors DEVMGR_ROW_N */

/* uefi.h publishes the EX variant's GUID but not the base one (nothing needed
 * it until now); keeping the literal here rather than widening a shared header
 * for one consumer. */
#define DG_SIMPLE_TEXT_INPUT_PROTOCOL_GUID \
    { 0x387477c1, 0x69c7, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

static EFI_BOOT_SERVICES *bs(void)
{
    EFI_SYSTEM_TABLE *st;
    if (uno_pc64_detached()) return 0;          /* boot services are gone */
    st = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    return st ? st->BootServices : 0;
}

/* ---- device-path walking --------------------------------------------------
 * A node is {u8 Type; u8 SubType; u8 Length[2];} + payload, ending at an END
 * node (type 0x7F).  usbboot.c walks the same structure for the boot volume;
 * the two copies are a few lines each and neither file owns the other's lane.
 */
#define DP_END       0x7F
#define DP_ACPI      0x02
#define DP_ACPI_STD  0x01             /* Acpi(HID,UID)                        */
#define DP_ACPI_EXP  0x02             /* Expanded Acpi(HID,UID,CID,...)       */
#define DP_MSG       0x03
#define DP_MSG_USB   0x05

static int dp_nodelen(const unsigned char *p) { return p[2] | (p[3] << 8); }

/* An ACPI device-path node carries its _HID as an EISA-packed UINT32 in the
 * first four payload bytes (both the standard and the expanded subtype).  The
 * PNP ranges are what name the i8042: PNP03xx is a keyboard controller port,
 * PNP0Fxx a pointing-device port.  Matching the RANGE rather than a list of
 * ids covers PNP0303/0301/030B and PNP0F03/0F13/0F12/0F0E alike, which is the
 * spread real firmware actually uses. */
#define EISA_PNP(hid)      (((hid) & 0xFFFF) == 0x41D0)
#define PNP_FAMILY(hid)    (((hid) >> 16) & 0xFF00)
#define PNP_KEYBOARD       0x0300
#define PNP_POINTER        0x0F00

/* Classify one device path into a UNO_DGT_* transport.  `want_ptr` picks which
 * PNP family counts as a match, so the same walk serves both questions. */
static int dp_classify(const unsigned char *p, int want_ptr)
{
    int n = 0, guard = 64, saw_any = 0;
    if (!p) return UNO_DGT_UNKNOWN;
    while (p[n] != DP_END) {
        int l = dp_nodelen(p + n);
        if (l < 4 || --guard < 0) return UNO_DGT_UNKNOWN;
        saw_any = 1;
        if (p[n] == DP_MSG && p[n + 1] == DP_MSG_USB) return UNO_DGT_USB;
        if (p[n] == DP_ACPI &&
            (p[n + 1] == DP_ACPI_STD || p[n + 1] == DP_ACPI_EXP) && l >= 8) {
            unsigned int hid = (unsigned int)p[n + 4]        |
                               ((unsigned int)p[n + 5] << 8) |
                               ((unsigned int)p[n + 6] << 16)|
                               ((unsigned int)p[n + 7] << 24);
            if (EISA_PNP(hid) &&
                PNP_FAMILY(hid) == (want_ptr ? PNP_POINTER : PNP_KEYBOARD))
                return UNO_DGT_PS2;
        }
        n += l;
    }
    return saw_any ? UNO_DGT_OTHER : UNO_DGT_UNKNOWN;
}

/* The strongest transport backing any handle publishing `guid`.  PS/2 wins
 * over USB wins over the rest: we are asking "is there a device here whose
 * native driver we already know how to bring up", and the answer is only
 * useful in its most specific form.  The ConIn/ConOut splitter handle is
 * skipped - it aggregates the device instances and has no device path of its
 * own, so counting it would turn every machine into UNO_DGT_UNKNOWN. */
static int transport_for(EFI_GUID *guid, int want_ptr)
{
    static EFI_GUID dp_guid = { 0x09576e91, 0x6d3f, 0x11d2,
        { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
    EFI_BOOT_SERVICES *BS = bs();
    EFI_SYSTEM_TABLE  *st = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    EFI_HANDLE *hs = 0;
    UINTN n = 0, i;
    int best = UNO_DGT_ABSENT;
    if (!BS) return UNO_DGT_ABSENT;
    if (EFI_ERROR(BS->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL, guid, 0, &n, &hs)))
        return UNO_DGT_ABSENT;
    for (i = 0; i < n; i++) {
        void *dp = 0;
        int t;
        if (st && hs[i] == st->ConsoleInHandle) continue;    /* the splitter */
        if (EFI_ERROR(BS->HandleProtocol(hs[i], &dp_guid, &dp)) || !dp) {
            if (best == UNO_DGT_ABSENT) best = UNO_DGT_UNKNOWN;
            continue;
        }
        t = dp_classify((const unsigned char *)dp, want_ptr);
        if (t == UNO_DGT_PS2) { best = t; break; }           /* most specific */
        if (t == UNO_DGT_USB && best != UNO_DGT_PS2) best = t;
        else if (best == UNO_DGT_ABSENT || best == UNO_DGT_UNKNOWN) best = t;
    }
    BS->FreePool(hs);
    return best;
}

/* ---- the latched verdict --------------------------------------------------- */
static int g_done, g_ptr_t, g_kbd_t;

static void evaluate(void)
{
    static EFI_GUID sp  = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    static EFI_GUID ap  = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    static EFI_GUID txt = DG_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
    int t;
    g_done  = 1;
    g_ptr_t = transport_for(&sp, 1);
    if (g_ptr_t == UNO_DGT_ABSENT || g_ptr_t == UNO_DGT_UNKNOWN) {
        t = transport_for(&ap, 1);                  /* tablets (QEMU/OVMF)   */
        if (t != UNO_DGT_ABSENT) g_ptr_t = t;
    }
    g_kbd_t = transport_for(&txt, 0);
}

static void ensure(void)
{
    if (!g_done && !uno_pc64_detached()) evaluate();
}

int uno_dg_fw_ptr_transport(void) { ensure(); return g_ptr_t; }
int uno_dg_fw_kbd_transport(void) { ensure(); return g_kbd_t; }

/* A pointer survives ExitBootServices when it is already ours (I2C-HID), or
 * when we can prove we will claim it at detach: a reachable USB HID boot mouse
 * (usbboot's preflight), or an i8042 aux mouse the firmware is driving now.
 *
 * That last arm is the ThinkPad X1 Carbon.  Its TrackPoint is a PS/2 Elan on
 * the aux port and arrives through firmware SimplePointer; uno_ps2_init()
 * claims it the moment the firmware lets go.  The old gate could not see this,
 * because uno_ps2_present() only says the CONTROLLER answers (on a laptop that
 * is the keyboard's EC) and the aux port cannot be self-tested while the
 * firmware owns the i8042.  The device path settles it without touching the
 * hardware at all: the firmware itself says the pointer it is publishing is a
 * PNP0Fxx device, i.e. the aux port. */
int uno_dg_ptr_survives(void)
{
    if (uno_i2c_hid_present())  return 1;
    if (uno_usb_hid_present())  return 1;
    if (uno_usbboot_hid_ptr())  return 1;
    return uno_dg_fw_ptr_transport() == UNO_DGT_PS2 && uno_ps2_present();
}

int uno_dg_kbd_survives(void)
{
    if (uno_ps2_present())          return 1;   /* the i8042 is ours at detach */
    if (uno_i2c_hid_kbd_present())  return 1;
    if (uno_usb_hid_kbd_present())  return 1;
    return uno_usbboot_hid_kbd();
}

int uno_dg_would_strand_pointer(void)
{
    if (uno_dg_fw_ptr_transport() == UNO_DGT_ABSENT) return 0;  /* none to lose */
    return !uno_dg_ptr_survives();
}

/* ---- blocker attribution --------------------------------------------------- */
static char g_dev[48];
static char g_blocker[48];

static char *ap_str(char *p, const char *s, char *end)
{ while (*s && p < end - 1) *p++ = *s++; return p; }
static char *ap_hex(char *p, unsigned int v, int digits, char *end)
{
    static const char *h = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; i--)
        if (p < end - 1) *p++ = h[(v >> (i * 4)) & 0xF];
    return p;
}
static char *ap_dec(char *p, unsigned int v, char *end)
{
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 12);
    while (n && p < end - 1) *p++ = t[--n];
    return p;
}

const char *uno_dg_dev_str(unsigned char cls, unsigned char sub)
{
    int n = devmgr_count(), i;
    char *p = g_dev, *end = g_dev + sizeof g_dev;
    g_dev[0] = 0;
    for (i = 0; i < n; i++) {
        unsigned int r[DG_ROW_N];
        if (devmgr_info(i, r, DG_ROW_N) != DG_ROW_N) continue;
        if ((unsigned char)r[5] != cls || (unsigned char)r[6] != sub) continue;
        p = ap_hex(p, r[0], 2, end); *p++ = ':';
        p = ap_hex(p, r[1], 2, end); *p++ = '.';
        p = ap_dec(p, r[2], end);    *p++ = ' ';
        p = ap_hex(p, r[3], 4, end); *p++ = ':';
        p = ap_hex(p, r[4], 4, end); *p++ = ' ';
        p = ap_str(p, devmgr_class_name(cls, sub), end);
        *p = 0;
        return g_dev;
    }
    return g_dev;                                   /* "" - not in the tree */
}

const char *uno_dg_blocker(void) { return g_blocker; }

void uno_dg_set_blocker(const char *dev)
{
    char *p = g_blocker;
    g_blocker[0] = 0;
    if (!dev) return;
    p = ap_str(p, dev, g_blocker + sizeof g_blocker);
    *p = 0;
}

static const char *tname(int t)
{
    switch (t) {
    case UNO_DGT_ABSENT: return "none";
    case UNO_DGT_PS2:    return "ps2";
    case UNO_DGT_USB:    return "usb";
    case UNO_DGT_OTHER:  return "other";
    default:             return "unknown";
    }
}

int uno_dg_status_str(char *buf, int cap)
{
    char *p = buf, *end = buf + cap;
    if (cap <= 1) { if (cap == 1) buf[0] = 0; return 0; }
    p = ap_str(p, "fw ptr=", end);      p = ap_str(p, tname(uno_dg_fw_ptr_transport()), end);
    p = ap_str(p, " kbd=", end);        p = ap_str(p, tname(uno_dg_fw_kbd_transport()), end);
    p = ap_str(p, "  survives ptr=", end); p = ap_dec(p, (unsigned)uno_dg_ptr_survives(), end);
    p = ap_str(p, " kbd=", end);        p = ap_dec(p, (unsigned)uno_dg_kbd_survives(), end);
    if (g_blocker[0]) { p = ap_str(p, "  blocker=", end); p = ap_str(p, g_blocker, end); }
    *p = 0;
    return (int)(p - buf);
}

/* ---- post-detach honesty ---------------------------------------------------
 * Every gate above is inference about hardware the firmware would not let us
 * touch.  The authoritative check is only possible on the far side of the
 * door, so make it there and say what happened, the same way the storage arm
 * does.  Losing the pointer is survivable (the shell is keyboard-driven), so
 * this reports rather than refuses - but it must not report silence. */
int uno_dg_ptr_arrived(const char **why)
{
    int predicted = 0;
    if (why) *why = "";
    if (!uno_pc64_detached()) return 1;             /* nothing to check yet */
    if (uno_dg_fw_ptr_transport() == UNO_DGT_ABSENT) return 1;
    switch (uno_dg_fw_ptr_transport()) {
    case UNO_DGT_PS2:  predicted = 1; break;
    case UNO_DGT_USB:  predicted = 1; break;
    default:           predicted = uno_i2c_hid_present(); break;
    }
    if (!predicted) return 1;                       /* we promised nothing  */
    if (uno_i2c_hid_present() || uno_usb_hid_present()) return 1;
    {
        int aux = 0;
        uno_ps2_status(0, &aux, 0, 0);
        if (aux) return 1;
    }
    if (why) *why = uno_dg_fw_ptr_transport() == UNO_DGT_PS2
                  ? "the i8042 aux mouse did not answer after detach"
                  : "the predicted USB pointer did not enumerate after detach";
    return 0;
}
